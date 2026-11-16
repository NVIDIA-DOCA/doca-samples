/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_admin.h"
#include "memos_initiator_ref_app_dma.h"
#include "memos_initiator_ref_app_io.h"
#include "memos_initiator_ref_app_kv.h"
#include "memos_initiator_ref_app_nvme_spec.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_devemu_nvme_cq.h>
#include <doca_devemu_nvme_dev.h>
#include <doca_devemu_nvme_io.h>
#include <doca_devemu_nvme_sq.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_ADMIN);

/* Admin queues always use qid 0 (NVMe spec). The admin CQ always uses MSI-X vector 0 with
 * interrupts enabled: the kernel nvme driver is interrupt-driven and waits on the admin vector. */
#define NVME_ADMIN_QID 0
#define NVME_ADMIN_CQ_INTERRUPT_VECTOR 0
#define NVME_ADMIN_CQ_INTERRUPT_ENABLE 1
/* The admin SQ priority field is ignored for the admin queue; pass 0. */
#define NVME_ADMIN_SQ_PRIORITY 0
/* Admin queue base addresses (ASQ/ACQ) are always physically contiguous per spec. */
#define NVME_QUEUE_PHYSICALLY_CONTIGUOUS 1

/* Controller identity reported via Identify Controller. VID/SSVID reuse the PCI vendor ID the
 * emulated function is created with (MEMOS_INITIATOR_REF_APP_PCI_VENDOR_ID in memos_initiator_ref_app_core.h). SUBNQN
 * is a valid NQN (kernel-required; SPDK is lenient). */
#define MEMOS_INITIATOR_REF_APP_CTRL_SN "DOCA-NVME-BASIC-0001"
#define MEMOS_INITIATOR_REF_APP_CTRL_MN "DOCA MEMOS Initiator Ref App"
#define MEMOS_INITIATOR_REF_APP_CTRL_FR "1.0"
#define MEMOS_INITIATOR_REF_APP_CTRL_SUBNQN "nqn.2026-06.com.nvidia:doca-nvme-basic"
/* MDTS=1 caps host transfers at 2 pages so a spec-compliant host never builds a PRP list (our DMA
 * resolver handles PRP1/PRP2 only). */
#define MEMOS_INITIATOR_REF_APP_CTRL_MDTS 1
/* Abort Command Limit (0-based). Host may have up to ACL+1 Abort commands outstanding. */
#define MEMOS_INITIATOR_REF_APP_CTRL_ACL 3

/* Bounded wait for an asynchronous queue disable / io-ctx stop to settle. The admin queues carry
 * no in-flight DMA in stage 2, so these transitions are synchronous and the loops usually do not
 * spin. */
#define NVME_ADMIN_DISABLE_TIMEOUT_US (1 * 1000 * 1000) /* 1 s */
#define NVME_ADMIN_DISABLE_SLEEP_NS (10 * 1000)		/* 10 us */

/* Recover the owning app from a request: the io ctx user_data is set to the app at bring-up. */
static struct memos_initiator_ref_app_resources *req_get_app(struct doca_devemu_nvme_req *req)
{
	struct doca_ctx *io_ctx = doca_devemu_nvme_io_as_ctx(doca_devemu_nvme_req_get_nvme_io(req));
	union doca_data user_data = {0};

	(void)doca_ctx_get_user_data(io_ctx, &user_data);
	return user_data.ptr;
}

/* Number-of-Queues grant: requested/granted counts are 0-based (count = field + 1). We grant up
 * to app->max_io_queues pairs (runtime: cores x queues-per-core, clamped), so the granted 0-based
 * value caps at max_io_queues - 1. */
static uint16_t num_queues_grant(struct memos_initiator_ref_app_resources *app, uint16_t requested_zero_based)
{
	const uint16_t max_zero_based = (uint16_t)(app->max_io_queues > 0 ? app->max_io_queues - 1 : 0);

	return requested_zero_based < max_zero_based ? requested_zero_based : max_zero_based;
}

static const char *feature_name(uint8_t fid)
{
	switch (fid) {
	case NVME_FEAT_VOLATILE_WRITE_CACHE:
		return "Volatile-Write-Cache";
	case NVME_FEAT_NUMBER_OF_QUEUES:
		return "Number-Of-Queues";
	case NVME_FEAT_ASYNC_EVENT_CONFIG:
		return "Async-Event-Config";
	case NVME_FEAT_IO_CMD_SET_PROFILE:
		return "IO-Command-Set-Profile";
	default:
		return "Unknown";
	}
}

static void admin_set_features(struct memos_initiator_ref_app_resources *app,
			       struct doca_devemu_nvme_req *req,
			       const void *sqe)
{
	uint32_t cdw10 = nvme_sqe_cdw(sqe, 10);
	uint32_t cdw11 = nvme_sqe_cdw(sqe, 11);
	uint8_t fid = (uint8_t)(cdw10 & 0xFFu);
	uint32_t dw0 = 0;
	uint16_t status = NVME_SC_SUCCESS;

	DOCA_LOG_INFO("Set-Features: %s (fid=0x%02x) cdw11=0x%08x", feature_name(fid), fid, cdw11);

	switch (fid) {
	case NVME_FEAT_NUMBER_OF_QUEUES: {
		uint16_t nsqr = (uint16_t)(cdw11 & 0xFFFFu);
		uint16_t ncqr = (uint16_t)((cdw11 >> 16) & 0xFFFFu);
		uint16_t nsqa = num_queues_grant(app, nsqr);
		uint16_t ncqa = num_queues_grant(app, ncqr);

		dw0 = (uint32_t)nsqa | ((uint32_t)ncqa << 16);
		DOCA_LOG_INFO("Set-Features NUM_QUEUES: requested sq=%u cq=%u -> granted sq=%u cq=%u",
			      nsqr + 1,
			      ncqr + 1,
			      nsqa + 1,
			      ncqa + 1);
		break;
	}
	case NVME_FEAT_VOLATILE_WRITE_CACHE:
		/* Identify reports VWC=0, so this controller has no volatile write cache and the feature
		 * does not exist on it. Accepting the set anyway would let a host read back an enabled
		 * cache from Get-Features on a controller whose Identify says there is none. */
		DOCA_LOG_INFO("Set-Features %s rejected: no volatile write cache on this controller -> INVALID_FIELD",
			      feature_name(fid));
		status = NVME_SC_INVALID_FIELD;
		break;
	case NVME_FEAT_ASYNC_EVENT_CONFIG:
		/* Accept and store; this app generates no asynchronous events, so the value is only
		 * echoed back on Get-Features. */
		app->aen_config = cdw11;
		break;
	case NVME_FEAT_IO_CMD_SET_PROFILE:
		if ((cdw11 & 0x1FFu) != 0) {
			DOCA_LOG_WARN("Set-Features IO_CMD_SET_PROFILE index %u not supported -> INVALID_FIELD",
				      cdw11 & 0x1FFu);
			status = NVME_SC_INVALID_FIELD;
		}
		break;
	default:
		DOCA_LOG_INFO("Set-Features %s (fid=0x%02x) not supported -> INVALID_FIELD", feature_name(fid), fid);
		status = NVME_SC_INVALID_FIELD;
		break;
	}

	doca_devemu_nvme_req_complete(req, dw0, 0, status);
}

static void admin_get_features(struct memos_initiator_ref_app_resources *app,
			       struct doca_devemu_nvme_req *req,
			       const void *sqe)
{
	uint8_t fid = (uint8_t)(nvme_sqe_cdw(sqe, 10) & 0xFFu);
	uint32_t dw0 = 0;
	uint16_t status = NVME_SC_SUCCESS;

	DOCA_LOG_INFO("Get-Features: %s (fid=0x%02x)", feature_name(fid), fid);

	switch (fid) {
	case NVME_FEAT_NUMBER_OF_QUEUES: {
		uint16_t n = (uint16_t)(app->max_io_queues > 0 ? app->max_io_queues - 1 : 0);

		dw0 = (uint32_t)n | ((uint32_t)n << 16);
		break;
	}
	case NVME_FEAT_VOLATILE_WRITE_CACHE:
		/* Answered the same way as the set: Identify advertises no volatile write cache. */
		DOCA_LOG_INFO("Get-Features %s rejected: no volatile write cache on this controller -> INVALID_FIELD",
			      feature_name(fid));
		status = NVME_SC_INVALID_FIELD;
		break;
	case NVME_FEAT_IO_CMD_SET_PROFILE:
		/* The only combination we publish is index 0, which the zeroed dw0 already reports. */
		break;
	case NVME_FEAT_ASYNC_EVENT_CONFIG:
		dw0 = app->aen_config;
		break;
	default:
		DOCA_LOG_INFO("Get-Features %s (fid=0x%02x) not supported -> INVALID_FIELD", feature_name(fid), fid);
		status = NVME_SC_INVALID_FIELD;
		break;
	}

	doca_devemu_nvme_req_complete(req, dw0, 0, status);
}

/* Hold an Async-Event-Request without completing it: this app never generates events, so the
 * request stays outstanding until controller reset / teardown completes it (see
 * memos_initiator_ref_app_admin_complete_held_aers()). The held req pins an io-ctx outstanding
 * slot, hence the cap. */
static void admin_hold_aer(struct memos_initiator_ref_app_resources *app,
			   struct doca_devemu_nvme_req *req,
			   uint16_t cid)
{
	if (app->num_held_aers >= MEMOS_INITIATOR_REF_APP_MAX_HELD_AER) {
		DOCA_LOG_WARN("AER cid=%u exceeds limit (%u held) -> AER_LIMIT_EXCEEDED", cid, app->num_held_aers);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_STATUS(NVME_SCT_CMD_SPECIFIC, NVME_SC_AER_LIMIT_EXCEEDED));
		return;
	}

	app->held_aers[app->num_held_aers++] = req;
	DOCA_LOG_INFO("AER cid=%u held (%u outstanding)", cid, app->num_held_aers);
}

static const char *admin_opcode_name(uint8_t opcode)
{
	switch (opcode) {
	case NVME_ADMIN_OPC_DELETE_IO_SQ:
		return "Delete-IO-SQ";
	case NVME_ADMIN_OPC_CREATE_IO_SQ:
		return "Create-IO-SQ";
	case NVME_ADMIN_OPC_GET_LOG_PAGE:
		return "Get-Log-Page";
	case NVME_ADMIN_OPC_DELETE_IO_CQ:
		return "Delete-IO-CQ";
	case NVME_ADMIN_OPC_CREATE_IO_CQ:
		return "Create-IO-CQ";
	case NVME_ADMIN_OPC_IDENTIFY:
		return "Identify";
	case NVME_ADMIN_OPC_ABORT:
		return "Abort";
	case NVME_ADMIN_OPC_SET_FEATURES:
		return "Set-Features";
	case NVME_ADMIN_OPC_GET_FEATURES:
		return "Get-Features";
	case NVME_ADMIN_OPC_ASYNC_EVENT:
		return "Async-Event-Request";
	default:
		return "Unknown";
	}
}

/* Fill a zeroed 4 KiB buffer with the Identify Controller data structure. */
static void identify_build_ctrl(uint8_t *buf)
{
	nvme_wr16(buf + NVME_ID_CTRL_VID, MEMOS_INITIATOR_REF_APP_PCI_VENDOR_ID);
	nvme_wr16(buf + NVME_ID_CTRL_SSVID, MEMOS_INITIATOR_REF_APP_PCI_VENDOR_ID);
	nvme_wr_ascii(buf + NVME_ID_CTRL_SN, MEMOS_INITIATOR_REF_APP_CTRL_SN, NVME_ID_CTRL_SN_LEN);
	nvme_wr_ascii(buf + NVME_ID_CTRL_MN, MEMOS_INITIATOR_REF_APP_CTRL_MN, NVME_ID_CTRL_MN_LEN);
	nvme_wr_ascii(buf + NVME_ID_CTRL_FR, MEMOS_INITIATOR_REF_APP_CTRL_FR, NVME_ID_CTRL_FR_LEN);
	buf[NVME_ID_CTRL_MDTS] = MEMOS_INITIATOR_REF_APP_CTRL_MDTS;
	nvme_wr16(buf + NVME_ID_CTRL_CNTLID, 0);
	nvme_wr32(buf + NVME_ID_CTRL_VER, NVME_VERSION_1_4_0);
	buf[NVME_ID_CTRL_ACL] = MEMOS_INITIATOR_REF_APP_CTRL_ACL;
	buf[NVME_ID_CTRL_AERL] = MEMOS_INITIATOR_REF_APP_MAX_HELD_AER - 1;
	buf[NVME_ID_CTRL_SQES] = NVME_ID_CTRL_SQES_VAL;
	buf[NVME_ID_CTRL_CQES] = NVME_ID_CTRL_CQES_VAL;
	nvme_wr32(buf + NVME_ID_CTRL_NN, 1);
	/* OACS/ONCS/FRMW/LPA/VWC left zero: no optional commands, no write cache. SGLS = 0. */
	memcpy(buf + NVME_ID_CTRL_SUBNQN,
	       MEMOS_INITIATOR_REF_APP_CTRL_SUBNQN,
	       sizeof(MEMOS_INITIATOR_REF_APP_CTRL_SUBNQN));
}

/* The namespace's 128-bit identity, written into every structure that carries an NGUID - a KV client
 * keys off it to find the namespace, and libdoca_kv reads it from CNS 00h specifically.
 *
 * It is the backing target namespace's own NGUID, so every structure reports the same value and the
 * emulated namespace is traceable to the remote one serving it. Before the backend is up there is no
 * target NGUID to report, so one is synthesised from our vendor id and the NSID; that only shows up
 * if a host manages to Identify ahead of the backend, and it keeps the field non-zero. */
static void identify_fill_nguid(const struct memos_initiator_ref_app_resources *app, uint8_t *nguid, uint32_t nsid)
{
	uint8_t target_nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN];
	uint16_t max_key_len;
	uint32_t max_value_len;
	uint64_t ns_size;

	if (memos_initiator_ref_app_kv_get_geometry(app, &max_key_len, &max_value_len, &ns_size, target_nguid) ==
	    DOCA_SUCCESS) {
		memcpy(nguid, target_nguid, sizeof(target_nguid));
		return;
	}

	nvme_wr16(nguid, MEMOS_INITIATOR_REF_APP_PCI_VENDOR_ID);
	memcpy(nguid + 2, "NVMEBASIC0", 10);
	nvme_wr32(nguid + 12, nsid);
}

/* Fill a zeroed 4 KiB buffer with the Identify Namespace data structure for NSID 1.
 *
 * This is the NVM-flavoured structure (CNS 00h). The namespace is CSI=KV, so its geometry lives in
 * CNS 05h and nothing here describes how it actually behaves - but it is still answered, because it
 * is where both Linux and libdoca_kv's query_ns_nguid() read the NGUID from. */
static void identify_build_ns(const struct memos_initiator_ref_app_resources *app, uint8_t *buf, uint32_t nsid)
{
	nvme_wr64(buf + NVME_ID_NS_NSZE, MEMOS_INITIATOR_REF_APP_NS_NUM_BLOCKS);
	nvme_wr64(buf + NVME_ID_NS_NCAP, MEMOS_INITIATOR_REF_APP_NS_NUM_BLOCKS);
	nvme_wr64(buf + NVME_ID_NS_NUSE, MEMOS_INITIATOR_REF_APP_NS_NUM_BLOCKS);
	buf[NVME_ID_NS_NLBAF] = 0; /* one LBA format (0-based) */
	buf[NVME_ID_NS_FLBAS] = 0; /* using LBA format 0 */
	nvme_wr32(buf + NVME_ID_NS_LBAF0, (uint32_t)MEMOS_INITIATOR_REF_APP_NS_LBA_SHIFT << NVME_LBAF_LBADS_SHIFT);
	identify_fill_nguid(app, buf + NVME_ID_NS_NGUID, nsid);
}

/* Fill a zeroed 4 KiB buffer with the KV Identify Namespace data structure (CNS 05h, CSI=KV).
 *
 * These three numbers are the namespace's entire KV contract: how long a key may be, how large a
 * value may be, and how much space exists. libdoca_kv reads them in query_kv_ns_capabilities() and
 * rejects any task that exceeds them, so they must match what the IO path will actually accept.
 *
 * They are taken from the target the backend attached to, not compiled in, so the namespace the host
 * sees describes the volume actually serving it. The NGUID is the target namespace's own, which lets
 * a KV client correlate the emulated namespace with the remote one it is really talking to. */
static doca_error_t identify_build_kv_ns(const struct memos_initiator_ref_app_resources *app, uint8_t *buf)
{
	uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN];
	uint16_t max_key_len;
	uint32_t max_value_len;
	uint64_t ns_size;
	doca_error_t result;

	result = memos_initiator_ref_app_kv_get_geometry(app, &max_key_len, &max_value_len, &ns_size, nguid);
	if (result != DOCA_SUCCESS)
		return result;

	nvme_wr64(buf + NVME_ID_KV_NS_NSZE, ns_size);
	nvme_wr16(buf + NVME_ID_KV_NS_MXKL, max_key_len);
	nvme_wr32(buf + NVME_ID_KV_NS_MXVL, max_value_len);
	memcpy(buf + NVME_ID_KV_NS_NGUID, nguid, sizeof(nguid));

	return DOCA_SUCCESS;
}

/* Fill a zeroed buffer with the Namespace Identification Descriptor list (CNS 0x03): an NGUID
 * descriptor giving the namespace a stable 128-bit identity, then a CSI descriptor naming its command
 * set, then the implicit zero (NIDT=0) terminator the rest of the zeroed page already provides.
 * Descriptors are packed with no padding - a host walks the list by NIDL. The NGUID is synthesised
 * deterministically from our vendor id and the NSID so it is stable across runs and unique per
 * namespace.
 *
 * The CSI descriptor is what makes this a KV namespace to a host. A host in the multiple
 * command set model requires every namespace to report one, and reporting KV is also what stops
 * Linux binding a block device, leaving the /dev/ngXnY character device libdoca_kv opens. */
static void identify_build_ns_desc_list(const struct memos_initiator_ref_app_resources *app,
					uint8_t *buf,
					uint32_t nsid)
{
	uint8_t *nguid = buf + NVME_NS_DESC_HDR_LEN;
	uint8_t *csi = nguid + NVME_NIDT_NGUID_LEN;

	buf[0] = NVME_NIDT_NGUID;
	buf[1] = NVME_NIDT_NGUID_LEN;
	/* buf[2..3] reserved, already zero. NGUID: [vendor id][fixed signature][nsid]. */
	identify_fill_nguid(app, nguid, nsid);

	csi[0] = NVME_NIDT_CSI;
	csi[1] = NVME_NIDT_CSI_LEN;
	/* csi[2..3] reserved, already zero. */
	csi[NVME_NS_DESC_HDR_LEN] = NVME_CSI_KV;
}

/*
 * Identify (opcode 0x06): build the 4 KiB data structure for the requested CNS in a scratch buffer
 * and DMA it to the host PRP destination; the request completes from the DMA callback.
 */
static void admin_identify(struct memos_initiator_ref_app_resources *app,
			   struct doca_devemu_nvme_req *req,
			   const void *sqe)
{
	uint8_t cns = (uint8_t)(nvme_sqe_cdw(sqe, 10) & 0xFFu);
	uint8_t csi = NVME_IDENTIFY_CDW11_CSI(nvme_sqe_cdw(sqe, 11));
	uint32_t nsid = nvme_sqe_nsid(sqe);
	uint8_t buf[NVME_IDENTIFY_DATA_SIZE];

	DOCA_LOG_INFO("Identify CNS=0x%02x csi=0x%02x nsid=0x%08x prp1=0x%016" PRIx64 " prp2=0x%016" PRIx64,
		      cns,
		      csi,
		      nsid,
		      nvme_sqe_prp1(sqe),
		      nvme_sqe_prp2(sqe));

	memset(buf, 0, sizeof(buf));

	switch (cns) {
	case NVME_IDENTIFY_CNS_CTRL:
		identify_build_ctrl(buf);
		break;
	case NVME_IDENTIFY_CNS_NS:
		if (nsid != MEMOS_INITIATOR_REF_APP_NS_NSID) {
			DOCA_LOG_WARN("Identify Namespace nsid=%u not present -> INVALID_NS", nsid);
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_NS);
			return;
		}
		identify_build_ns(app, buf, nsid);
		break;
	case NVME_IDENTIFY_CNS_ACTIVE_NS_LIST:
		/* The list carries the NSIDs greater than CDW1.NSID, so a host that has already walked past
		 * the only namespace we expose must get the zero-filled page that ends its enumeration -
		 * repeating NSID 1 would make the walk either loop or see the namespace twice. */
		if (nsid < MEMOS_INITIATOR_REF_APP_NS_NSID)
			nvme_wr32(buf, MEMOS_INITIATOR_REF_APP_NS_NSID);
		break;
	case NVME_IDENTIFY_CNS_CS_ACTIVE_NS_LIST:
		/* Same list and the same cursor rule, restricted to one command set. Our only namespace is
		 * KV, so any other CSI correctly yields the empty list the zeroed buffer already holds. */
		if (csi == NVME_CSI_KV && nsid < MEMOS_INITIATOR_REF_APP_NS_NSID)
			nvme_wr32(buf, MEMOS_INITIATOR_REF_APP_NS_NSID);
		break;
	case NVME_IDENTIFY_CNS_NS_DESC_LIST:
		if (nsid != MEMOS_INITIATOR_REF_APP_NS_NSID) {
			DOCA_LOG_WARN("Identify NS-Desc-List nsid=%u not present -> INVALID_NS", nsid);
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_NS);
			return;
		}
		identify_build_ns_desc_list(app, buf, nsid);
		break;
	case NVME_IDENTIFY_CNS_CS_NS:
		/* I/O Command Set specific Identify Namespace. For CSI=KV this is the structure that
		 * actually describes the namespace - MXKL, MXVL, NSZE - so it has to be built. Any other
		 * CSI names a command set this namespace does not implement. */
		if (nsid != MEMOS_INITIATOR_REF_APP_NS_NSID) {
			DOCA_LOG_WARN("Identify CS-Namespace nsid=%u not present -> INVALID_NS", nsid);
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_NS);
			return;
		}
		if (csi != NVME_CSI_KV) {
			DOCA_LOG_WARN("Identify CS-Namespace csi=0x%02x is not KV -> INVALID_FIELD", csi);
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_FIELD);
			return;
		}
		/* The only Identify this app cannot answer from compiled-in values: it reports the backing
		 * target's geometry, so a host asking before the backend attached gets NS_NOT_READY rather
		 * than a namespace whose advertised limits nothing stands behind. */
		if (identify_build_kv_ns(app, buf) != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Identify CS-Namespace before the KV backend is up -> NS_NOT_READY");
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_NS_NOT_READY);
			return;
		}
		break;
	case NVME_IDENTIFY_CNS_CS_CTRL:
		/* I/O Command Set specific Identify Controller. The KV structure carries only a version
		 * field and we advertise no optional KV controller capabilities, so an all-zero page is
		 * the correct response. */
		DOCA_LOG_INFO("Identify CNS=0x%02x csi=0x%02x (CS Identify Controller) -> zero-filled SUCCESS",
			      cns,
			      csi);
		break;
	case NVME_IDENTIFY_CNS_NS_CS_INDEP:
		/* Command Set Independent Identify Namespace. Small structure, but not optional: once a
		 * host selects CC.CSS=110b it asks this before it will use a namespace, and a failure here
		 * makes it skip the namespace *silently* - the controller enumerates, Identify reports
		 * NSID 1 active, and no device node ever appears. NSTAT.NRDY is the answer it wants. */
		if (nsid != MEMOS_INITIATOR_REF_APP_NS_NSID) {
			DOCA_LOG_WARN("Identify CS-Independent nsid=%u not present -> INVALID_NS", nsid);
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_NS);
			return;
		}
		buf[NVME_ID_NS_CS_INDEP_NSTAT] = NVME_NSTAT_NRDY;
		break;
	case NVME_IDENTIFY_CNS_IO_COMMAND_SET:
		/* Selectable command-set combinations, one 512-bit vector each; index 0 is the default.
		 * Linux does not query this, SPDK and other NVMe 2.0 hosts do. */
		nvme_wr64(buf, 1ULL << NVME_CSI_KV);
		break;
	default:
		DOCA_LOG_WARN("Identify CNS=0x%02x not supported -> INVALID_FIELD", cns);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_FIELD);
		return;
	}

	(void)memos_initiator_ref_app_dma_to_host(app, req, sqe, buf, NVME_IDENTIFY_DATA_SIZE, NVME_SC_SUCCESS);
}

/*
 * Get-Log-Page (opcode 0x02): this app keeps no real logs, so it returns a zero-filled buffer of
 * the host-requested length (capped to one page) for every LID and completes SUCCESS via DMA.
 */
static void admin_get_log_page(struct memos_initiator_ref_app_resources *app,
			       struct doca_devemu_nvme_req *req,
			       const void *sqe)
{
	uint32_t cdw10 = nvme_sqe_cdw(sqe, 10);
	uint32_t cdw11 = nvme_sqe_cdw(sqe, 11);
	uint8_t lid = (uint8_t)(cdw10 & 0xFFu);
	/* NUMD (0-based dword count) is split: NUMDL = CDW10[31:16], NUMDU = CDW11[15:0]. */
	uint32_t numd = ((cdw11 & 0xFFFFu) << 16) | ((cdw10 >> 16) & 0xFFFFu);
	size_t len = ((size_t)numd + 1) * sizeof(uint32_t);
	uint8_t buf[NVME_IDENTIFY_DATA_SIZE];

	/* MDTS lets a host ask for more than this app serves in one go. Truncating to the buffer and
	 * still reporting success would leave the host believing the tail it never received is log
	 * content, so an oversized request is refused instead. */
	if (len > sizeof(buf)) {
		DOCA_LOG_WARN("Get-Log-Page lid=0x%02x asks for %zu B, more than the %zu B served -> INVALID_FIELD",
			      lid,
			      len,
			      sizeof(buf));
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_FIELD);
		return;
	}
	memset(buf, 0, len);

	DOCA_LOG_INFO("Get-Log-Page lid=0x%02x len=%zu -> zero-filled SUCCESS", lid, len);
	(void)memos_initiator_ref_app_dma_to_host(app, req, sqe, buf, len, NVME_SC_SUCCESS);
	//(void)doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_SUCCESS);
}

/* Create-IO-CQ (0x05): decode the command fields and create the completion queue via the io
 * module, completing with the resulting NVMe status. */
static void admin_create_io_cq(struct memos_initiator_ref_app_resources *app,
			       struct doca_devemu_nvme_req *req,
			       const void *sqe)
{
	uint32_t cdw10 = nvme_sqe_cdw(sqe, 10);
	uint32_t cdw11 = nvme_sqe_cdw(sqe, 11);
	uint16_t qid = NVME_CREATE_IOQ_CDW10_QID(cdw10);
	/* QSIZE is zero-based, so its largest value asks for 65536 entries - one more than the entry
	 * count fits. Saturate rather than narrowing, so the create path below reports it as out of
	 * range instead of being handed a depth that wrapped to zero. */
	uint32_t entries = NVME_CREATE_IOQ_CDW10_QSIZE(cdw10) + 1;
	uint16_t qsize = (uint16_t)(entries > UINT16_MAX ? UINT16_MAX : entries);
	uint16_t status;

	status = memos_initiator_ref_app_io_create_cq(app,
						      qid,
						      qsize,
						      NVME_CREATE_IOCQ_CDW11_IV(cdw11),
						      (uint8_t)NVME_CREATE_IOCQ_CDW11_IEN(cdw11),
						      (uint8_t)NVME_CREATE_IOQ_CDW11_PC(cdw11),
						      nvme_sqe_prp1(sqe));
	doca_devemu_nvme_req_complete(req, 0, 0, status);
}

/* Create-IO-SQ (0x01). */
static void admin_create_io_sq(struct memos_initiator_ref_app_resources *app,
			       struct doca_devemu_nvme_req *req,
			       const void *sqe)
{
	uint32_t cdw10 = nvme_sqe_cdw(sqe, 10);
	uint32_t cdw11 = nvme_sqe_cdw(sqe, 11);
	uint16_t qid = NVME_CREATE_IOQ_CDW10_QID(cdw10);
	/* QSIZE is zero-based, so its largest value asks for 65536 entries - one more than the entry
	 * count fits. Saturate rather than narrowing, so the create path below reports it as out of
	 * range instead of being handed a depth that wrapped to zero. */
	uint32_t entries = NVME_CREATE_IOQ_CDW10_QSIZE(cdw10) + 1;
	uint16_t qsize = (uint16_t)(entries > UINT16_MAX ? UINT16_MAX : entries);
	uint16_t status;

	status = memos_initiator_ref_app_io_create_sq(app,
						      qid,
						      qsize,
						      NVME_CREATE_IOSQ_CDW11_CQID(cdw11),
						      (uint8_t)NVME_CREATE_IOSQ_CDW11_QPRIO(cdw11),
						      (uint8_t)NVME_CREATE_IOQ_CDW11_PC(cdw11),
						      nvme_sqe_prp1(sqe));
	doca_devemu_nvme_req_complete(req, 0, 0, status);
}

/*
 * Admin command handler for requests on the admin SQ (qid 0). Dispatches on opcode and completes
 * the command. Identify and Get-Log-Page DMA their payload to the host PRP destination and complete
 * asynchronously; Create/Delete IO CQ/SQ manage the IO queue registry. SGL (PSDT != 0) is rejected
 * up front since this app supports PRP only.
 */
static void admin_handle(struct memos_initiator_ref_app_resources *app,
			 struct doca_devemu_nvme_req *req,
			 const void *sqe)
{
	uint8_t opcode = nvme_sqe_opcode(sqe);
	uint16_t cid = nvme_sqe_cid(sqe);

	DOCA_LOG_INFO("Admin command: %s (opcode=0x%02x) cid=%u", admin_opcode_name(opcode), opcode, cid);

	if (nvme_sqe_psdt(sqe) != NVME_PSDT_PRP) {
		DOCA_LOG_WARN("Admin opcode=0x%02x cid=%u uses SGL (PSDT=%u) -> INVALID_FIELD",
			      opcode,
			      cid,
			      nvme_sqe_psdt(sqe));
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_FIELD);
		return;
	}

	switch (opcode) {
	case NVME_ADMIN_OPC_IDENTIFY:
		admin_identify(app, req, sqe);
		break;
	case NVME_ADMIN_OPC_GET_LOG_PAGE:
		admin_get_log_page(app, req, sqe);
		break;
	case NVME_ADMIN_OPC_SET_FEATURES:
		admin_set_features(app, req, sqe);
		break;
	case NVME_ADMIN_OPC_GET_FEATURES:
		admin_get_features(app, req, sqe);
		break;
	case NVME_ADMIN_OPC_CREATE_IO_CQ:
		admin_create_io_cq(app, req, sqe);
		break;
	case NVME_ADMIN_OPC_CREATE_IO_SQ:
		admin_create_io_sq(app, req, sqe);
		break;
	case NVME_ADMIN_OPC_DELETE_IO_CQ:
		doca_devemu_nvme_req_complete(
			req,
			0,
			0,
			memos_initiator_ref_app_io_delete_cq(app, NVME_DELETE_IOQ_CDW10_QID(nvme_sqe_cdw(sqe, 10))));
		break;
	case NVME_ADMIN_OPC_DELETE_IO_SQ:
		doca_devemu_nvme_req_complete(
			req,
			0,
			0,
			memos_initiator_ref_app_io_delete_sq(app, NVME_DELETE_IOQ_CDW10_QID(nvme_sqe_cdw(sqe, 10))));
		break;
	case NVME_ADMIN_OPC_ABORT:
		/* No command tracking yet: report "not aborted" (CDW0 bit0 = 1) with success. */
		DOCA_LOG_INFO("Admin Abort cid=%u -> command not aborted", cid);
		doca_devemu_nvme_req_complete(req, 1, 0, NVME_SC_SUCCESS);
		break;
	case NVME_ADMIN_OPC_ASYNC_EVENT:
		admin_hold_aer(app, req, cid);
		break;
	default:
		DOCA_LOG_ERR("Admin command %s (opcode=0x%02x) cid=%u not implemented -> INVALID_OPCODE",
			     admin_opcode_name(opcode),
			     opcode,
			     cid);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_OPCODE);
		break;
	}
}

/*
 * The single request handler registered on the shared io ctx. The admin SQ binds NULL user data
 * and IO SQs bind their registry slot, so the SQ binding user data selects the path: admin commands
 * (qid 0) go to admin_handle, IO commands to the io module.
 */
static void memos_initiator_ref_app_req_dispatch(struct doca_devemu_nvme_req *req, const void *sqe, void *req_user_data)
{
	void *sq_user_data = doca_devemu_nvme_req_get_sq_user_data(req);

	/* Datapath debug: every drained submission entry passes through here. Distinguishes admin (main
	 * thread) from IO (worker thread) and confirms the SQE reached the dispatcher. */
	DOCA_LOG_DBG("REQ dispatch: %s SQ, opcode=0x%02x cid=%u",
		     sq_user_data != NULL ? "IO" : "admin",
		     nvme_sqe_opcode(sqe),
		     nvme_sqe_cid(sqe));

	/* The per-request buffer stays valid until doca_devemu_nvme_req_complete(), which is exactly the
	 * lifetime a KV Store needs: its key and conditional-store flags have to outlive the command's
	 * data DMA and still be there when the completion callback records the value. */
	if (sq_user_data != NULL)
		memos_initiator_ref_app_io_handle(req, sqe, sq_user_data, req_user_data);
	else
		admin_handle(req_get_app(req), req, sqe);
}

doca_error_t memos_initiator_ref_app_io_register_dispatch(struct memos_initiator_ref_app_resources *app,
							  struct doca_devemu_nvme_io *io)
{
	struct doca_ctx *io_ctx = doca_devemu_nvme_io_as_ctx(io);
	doca_error_t result;

	/* Handler registration is only allowed while the io ctx is idle (before start). */
	result = doca_devemu_nvme_io_event_nvme_req_register(io, memos_initiator_ref_app_req_dispatch);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register request handler: %s", doca_error_get_descr(result));
		return result;
	}

	/* The handler recovers the app from the io ctx user_data (see req_get_app). */
	result = doca_ctx_set_user_data(io_ctx, (union doca_data){.ptr = app});
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to set io ctx user data: %s", doca_error_get_descr(result));
	return result;
}

void memos_initiator_ref_app_admin_complete_held_aers(struct memos_initiator_ref_app_resources *app)
{
	uint32_t i;

	if (app->num_held_aers == 0)
		return;

	for (i = 0; i < app->num_held_aers; ++i) {
		doca_devemu_nvme_req_complete(app->held_aers[i], 0, 0, NVME_SC_CMD_ABORTED_SQ_DELETION);
		app->held_aers[i] = NULL;
	}
	DOCA_LOG_INFO("Completed %u held AER(s) on teardown", app->num_held_aers);
	app->num_held_aers = 0;
}

static void admin_pe_idle_sleep(struct memos_initiator_ref_app_resources *app)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = NVME_ADMIN_DISABLE_SLEEP_NS};

	/* Queue disable/drain (CQE DMA, io ctx flush) advances on the datapath PE, so pump both. */
	if (memos_initiator_ref_app_progress_all(app) == 0) {
		struct timespec ts = sleep_ts;

		(void)nanosleep(&ts, NULL);
	}
}

static bool admin_disable_deadline_passed(const struct timespec *start)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (size_t)((now.tv_sec - start->tv_sec) * 1000000LL + (now.tv_nsec - start->tv_nsec) / 1000LL) >=
	       NVME_ADMIN_DISABLE_TIMEOUT_US;
}

/* Disable the admin CQ and, if the disable is asynchronous, drive the PE until it reaches
 * DISABLED so the subsequent stop is permitted. */
static void admin_cq_disable_and_wait(struct memos_initiator_ref_app_resources *app)
{
	struct timespec start;
	enum doca_devemu_nvme_cq_states state;
	doca_error_t result;

	result = doca_devemu_nvme_cq_get_state(app->admin_cq, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin teardown: failed to query CQ state: %s", doca_error_get_descr(result));
		return;
	}
	if (state != DOCA_DEVEMU_NVME_CQ_STATE_ENABLED)
		return;

	result = doca_devemu_nvme_cq_disable(app->admin_cq);
	if (result == DOCA_SUCCESS)
		return;
	if (result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Admin teardown: failed to disable CQ: %s", doca_error_get_descr(result));
		return;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		result = doca_devemu_nvme_cq_get_state(app->admin_cq, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Admin teardown: failed to query CQ state: %s", doca_error_get_descr(result));
			return;
		}
		if (state == DOCA_DEVEMU_NVME_CQ_STATE_DISABLED)
			return;
		if (admin_disable_deadline_passed(&start)) {
			DOCA_LOG_ERR("Admin teardown: timed out waiting for CQ to disable");
			return;
		}
		admin_pe_idle_sleep(app);
	}
}

/* SQ counterpart of admin_cq_disable_and_wait. */
static void admin_sq_disable_and_wait(struct memos_initiator_ref_app_resources *app)
{
	struct timespec start;
	enum doca_devemu_nvme_sq_states state;
	doca_error_t result;

	result = doca_devemu_nvme_sq_get_state(app->admin_sq, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin teardown: failed to query SQ state: %s", doca_error_get_descr(result));
		return;
	}
	if (state != DOCA_DEVEMU_NVME_SQ_STATE_ENABLED)
		return;

	result = doca_devemu_nvme_sq_disable(app->admin_sq);
	if (result == DOCA_SUCCESS)
		return;
	if (result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Admin teardown: failed to disable SQ: %s", doca_error_get_descr(result));
		return;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		result = doca_devemu_nvme_sq_get_state(app->admin_sq, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Admin teardown: failed to query SQ state: %s", doca_error_get_descr(result));
			return;
		}
		if (state == DOCA_DEVEMU_NVME_SQ_STATE_DISABLED)
			return;
		if (admin_disable_deadline_passed(&start)) {
			DOCA_LOG_ERR("Admin teardown: timed out waiting for SQ to disable");
			return;
		}
		admin_pe_idle_sleep(app);
	}
}

/* Stop and destroy the admin SQ. The SQ must already be disabled (and unbound from the io ctx).
 * Idempotent and tolerant of partially-built state so it can also unwind a failed bring-up. */
static void admin_sq_stop_destroy(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	if (app->admin_sq == NULL)
		return;

	result = doca_devemu_nvme_sq_destroy(app->admin_sq, DOCA_DEVEMU_NVME_SQ_DESTROY_FLAG_NONE);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Admin teardown: failed to destroy SQ: %s", doca_error_get_descr(result));

	app->admin_sq = NULL;
}

/* Stop and destroy the admin CQ. The SQ must already be gone (a started SQ pins its CQ) and the
 * CQ disabled (and unbound from the io ctx). */
static void admin_cq_stop_destroy(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	if (app->admin_cq == NULL)
		return;

	result = doca_devemu_nvme_cq_destroy(app->admin_cq, DOCA_DEVEMU_NVME_CQ_DESTROY_FLAG_NONE);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Admin teardown: failed to destroy CQ: %s", doca_error_get_descr(result));

	app->admin_cq = NULL;
}

/* Drive the PE until the io ctx reaches the target state, so a synchronous-looking start/stop
 * completes before the next step. The io ctx is a leaf (no async sub-context), so these usually
 * settle without spinning. */
static doca_error_t admin_io_wait_state(struct memos_initiator_ref_app_resources *app, enum doca_ctx_states target)
{
	struct doca_ctx *io_ctx = doca_devemu_nvme_io_as_ctx(app->admin_io);
	struct timespec start;
	enum doca_ctx_states state;
	doca_error_t result;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		result = doca_ctx_get_state(io_ctx, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Admin io: failed to query ctx state: %s", doca_error_get_descr(result));
			return result;
		}
		if (state == target)
			return DOCA_SUCCESS;
		if (admin_disable_deadline_passed(&start)) {
			DOCA_LOG_ERR("Admin io: timed out waiting for ctx state %d (current %d)",
				     (int)target,
				     (int)state);
			return DOCA_ERROR_TIME_OUT;
		}
		admin_pe_idle_sleep(app);
	}
}

static doca_error_t admin_io_bring_up(struct memos_initiator_ref_app_resources *app)
{
	struct doca_ctx *io_ctx;
	doca_error_t result;

	result = doca_devemu_nvme_io_create(&app->admin_io);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to create io ctx: %s", doca_error_get_descr(result));
		app->admin_io = NULL;
		return result;
	}

	result = memos_initiator_ref_app_io_register_dispatch(app, app->admin_io);
	if (result != DOCA_SUCCESS)
		return result;

	io_ctx = doca_devemu_nvme_io_as_ctx(app->admin_io);

	/* The admin io ctx is the admin request dispatcher and owns the admin queues' datapath worker.
	 * It lives on the main-thread admin PE (alongside the admin DMA), not on a per-core worker PE;
	 * doca_devemu_nvme_sq_flush() then drains on this same PE. */
	result = doca_pe_connect_ctx(app->admin_pe, io_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to connect io ctx to PE: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(io_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to start io ctx: %s", doca_error_get_descr(result));
		return result;
	}
	result = admin_io_wait_state(app, DOCA_CTX_STATE_RUNNING);
	if (result != DOCA_SUCCESS)
		return result;

	/* The io ctx owns the datapath worker once running, so it must be up before any queue is
	 * bound + enabled. The admin CQ/SQ bind to it inside their own bring-up (start -> bind ->
	 * enable), since enable now consumes the worker the queue inherits at bind. */
	DOCA_LOG_INFO("Admin io ctx up: handler registered - ready to bring up admin queues");

	return DOCA_SUCCESS;
}

static doca_error_t admin_cq_bring_up(struct memos_initiator_ref_app_resources *app, uint16_t acq_size, uint64_t acq)
{
	struct doca_devemu_nvme_cq_create_attr *cq_attr = NULL;
	doca_error_t result;

	result = doca_devemu_nvme_cq_create_attr_create(&cq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to create CQ attributes: %s", doca_error_get_descr(result));
		return result;
	}

	(void)doca_devemu_nvme_cq_create_attr_set_qid(cq_attr, NVME_ADMIN_QID);
	(void)doca_devemu_nvme_cq_create_attr_set_qsize(cq_attr, acq_size);
	(void)doca_devemu_nvme_cq_create_attr_set_interrupt_vector(cq_attr, NVME_ADMIN_CQ_INTERRUPT_VECTOR);
	(void)doca_devemu_nvme_cq_create_attr_set_interrupt_enable(cq_attr, NVME_ADMIN_CQ_INTERRUPT_ENABLE);
	(void)doca_devemu_nvme_cq_create_attr_set_physically_contiguous(cq_attr, NVME_QUEUE_PHYSICALLY_CONTIGUOUS);
	(void)doca_devemu_nvme_cq_create_attr_set_prp1(cq_attr, acq);

	result = doca_devemu_nvme_cq_create_from_dev(cq_attr,
						     app->nvme_dev,
						     DOCA_DEVEMU_NVME_CQ_CREATE_FLAG_NONE,
						     &app->admin_cq);
	(void)doca_devemu_nvme_cq_create_attr_destroy(cq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to create CQ: %s", doca_error_get_descr(result));
		app->admin_cq = NULL;
		return result;
	}

	/* Bind before enable: the CQ inherits the io ctx's worker at bind, which enable consumes. */
	result = doca_devemu_nvme_cq_bind(app->admin_cq, app->admin_io);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to bind CQ: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_nvme_cq_enable(app->admin_cq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to enable CQ: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Admin CQ up: qid=%u depth=%u base=0x%016" PRIx64
		      " - first doorbell CQE expected on next progress tick",
		      NVME_ADMIN_QID,
		      acq_size,
		      acq);

	return DOCA_SUCCESS;
}

static doca_error_t admin_sq_bring_up(struct memos_initiator_ref_app_resources *app, uint16_t asq_size, uint64_t asq)
{
	struct doca_devemu_nvme_sq_create_attr *sq_attr = NULL;
	doca_error_t result;

	result = doca_devemu_nvme_sq_create_attr_create(&sq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to create SQ attributes: %s", doca_error_get_descr(result));
		return result;
	}

	(void)doca_devemu_nvme_sq_create_attr_set_qid(sq_attr, NVME_ADMIN_QID);
	(void)doca_devemu_nvme_sq_create_attr_set_qsize(sq_attr, asq_size);
	(void)doca_devemu_nvme_sq_create_attr_set_cq(sq_attr, app->admin_cq);
	(void)doca_devemu_nvme_sq_create_attr_set_qprio(sq_attr, NVME_ADMIN_SQ_PRIORITY);
	(void)doca_devemu_nvme_sq_create_attr_set_physically_contiguous(sq_attr, NVME_QUEUE_PHYSICALLY_CONTIGUOUS);
	(void)doca_devemu_nvme_sq_create_attr_set_prp1(sq_attr, asq);

	result = doca_devemu_nvme_sq_create(sq_attr, DOCA_DEVEMU_NVME_SQ_CREATE_FLAG_NONE, &app->admin_sq);
	(void)doca_devemu_nvme_sq_create_attr_destroy(sq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to create SQ: %s", doca_error_get_descr(result));
		app->admin_sq = NULL;
		return result;
	}

	/* Bind before enable: the SQ inherits its CQ's worker at bind (CQ bound first above). */
	result = doca_devemu_nvme_sq_bind(app->admin_sq, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to bind SQ: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_nvme_sq_enable(app->admin_sq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to enable SQ: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Admin SQ up: qid=%u depth=%u base=0x%016" PRIx64
		      " - first doorbell CQE expected on next progress tick",
		      NVME_ADMIN_QID,
		      asq_size,
		      asq);

	return DOCA_SUCCESS;
}

doca_error_t memos_initiator_ref_app_admin_bring_up(struct memos_initiator_ref_app_resources *app)
{
	uint32_t aqa;
	uint64_t acq;
	uint64_t asq;
	uint16_t acq_size;
	uint16_t asq_size;
	doca_error_t result;

	if (app->admin_cq != NULL)
		return DOCA_SUCCESS;

	result = doca_devemu_nvme_dev_read_reg32(app->nvme_dev, NVME_REG_AQA, &aqa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to read AQA: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_devemu_nvme_dev_read_reg64(app->nvme_dev, NVME_REG_ACQ, &acq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to read ACQ: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_devemu_nvme_dev_read_reg64(app->nvme_dev, NVME_REG_ASQ, &asq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Admin bring-up: failed to read ASQ: %s", doca_error_get_descr(result));
		return result;
	}

	/* AQA.ACQS/ASQS are 0-based; the public set_conf takes the 1-based entry count. */
	acq_size = (uint16_t)(NVME_AQA_ACQS(aqa) + 1);
	asq_size = (uint16_t)(NVME_AQA_ASQS(aqa) + 1);

	/* The io ctx owns the datapath worker and must be running before any queue is bound and
	 * enabled, so it is brought up first (each queue then binds to it during its own bring-up). */
	result = admin_io_bring_up(app);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = admin_cq_bring_up(app, acq_size, acq);
	if (result != DOCA_SUCCESS)
		goto fail;

	/* The admin SQ posts its completions to the admin CQ, so it is created after the CQ. */
	result = admin_sq_bring_up(app, asq_size, asq);
	if (result != DOCA_SUCCESS)
		goto fail;

	/* The IO queue registry needs the shared io ctx, so it is allocated after admin_io_bring_up. */
	result = memos_initiator_ref_app_io_init(app);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	/* Idempotent and tolerant of partial state: unwinds whatever bring-up managed to set up. */
	memos_initiator_ref_app_admin_tear_down(app);
	return result;
}

void memos_initiator_ref_app_admin_tear_down(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	if (app->admin_io == NULL && app->admin_sq == NULL && app->admin_cq == NULL)
		return;

	/* Drain held Async-Event-Requests first: they pin io-ctx outstanding slots, so the SQ flush
	 * below would otherwise spin. Requires the queues / io ctx still bound, which they are here. */
	if (app->admin_io != NULL)
		memos_initiator_ref_app_admin_complete_held_aers(app);

	/* Tear down all IO queues while the shared io ctx is still up (they are bound to it). */
	memos_initiator_ref_app_io_teardown(app);

	/*
	 * Reverse of bring-up, honouring the public API ordering constraints:
	 *   disable SQ -> flush + unbind SQ -> disable CQ -> unbind CQ -> stop io ->
	 *   stop/destroy SQ -> stop/destroy CQ.
	 * A queue must be DISABLED before it can be unbound, every queue must be unbound before the
	 * io ctx can stop, and a started SQ pins its CQ so the SQ is always torn down first.
	 */
	if (app->admin_sq != NULL)
		admin_sq_disable_and_wait(app);

	if (app->admin_io != NULL && app->admin_sq != NULL) {
		(void)doca_devemu_nvme_sq_flush(app->admin_sq);
		result = doca_devemu_nvme_sq_unbind(app->admin_sq);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Admin teardown: failed to unbind SQ: %s", doca_error_get_descr(result));
	}

	if (app->admin_cq != NULL)
		admin_cq_disable_and_wait(app);

	if (app->admin_io != NULL && app->admin_cq != NULL) {
		result = doca_devemu_nvme_cq_unbind(app->admin_cq);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Admin teardown: failed to unbind CQ: %s", doca_error_get_descr(result));
	}

	if (app->admin_io != NULL) {
		struct doca_ctx *io_ctx = doca_devemu_nvme_io_as_ctx(app->admin_io);

		result = doca_ctx_stop(io_ctx);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Admin teardown: failed to stop io ctx: %s", doca_error_get_descr(result));
		(void)admin_io_wait_state(app, DOCA_CTX_STATE_IDLE);

		result = doca_devemu_nvme_io_destroy(app->admin_io);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Admin teardown: failed to destroy io ctx: %s", doca_error_get_descr(result));
		app->admin_io = NULL;
	}

	admin_sq_stop_destroy(app);
	admin_cq_stop_destroy(app);

	DOCA_LOG_INFO("Admin queues torn down");
}
