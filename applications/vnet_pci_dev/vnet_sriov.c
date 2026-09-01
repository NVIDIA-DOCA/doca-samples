/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "vnet_sriov.h"

#include <stdlib.h>
#include <string.h>

#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_devemu_pci_type.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_types.h>

#include "pci_spec_tlp.h"
#include "vnet_pci_device.h"
#include "vnet_pci_dev_core.h"

DOCA_LOG_REGISTER(VNET_SRIOV);

/* Fixed VF Routing-ID layout: VFs are functions 1..N on the PF's own bus (ARI
 * lets the function number exceed 7). The PCIe SR-IOV cap synthesizer in
 * vnet_pci_device.c hardcodes the same values; validate_caps() refuses to
 * proceed if the SDK reports a device that cannot honor them. */
#define VNET_SRIOV_REQUIRED_VF_STRIDE 1
#define VNET_SRIOV_REQUIRED_FIRST_VF_OFFSET 1

/*********************************************************************************************************************
 *                                                                                                                   *
 *  Cap probing                                                                                                      *
 *                                                                                                                   *
 *********************************************************************************************************************/

doca_error_t vnet_sriov_probe_caps(const struct doca_devinfo *devinfo, struct vnet_sriov_caps *caps)
{
	uint8_t supported = 0;
	doca_error_t result;

	if (devinfo == NULL || caps == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	memset(caps, 0, sizeof(*caps));

	/* The dynamic-SR-IOV predicate gates everything else: with no
	 * firmware-provisioned VF pool there is no point reading the geometry
	 * caps. Report "unavailable" so the caller uses the non-rep_ex path. */
	result = doca_devemu_pci_tlp_cap_type_is_dynamic_sriov_supported(devinfo, &supported);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("SR-IOV: dynamic-SR-IOV support query failed: %s; treating as unavailable",
			      doca_error_get_descr(result));
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	if (!supported) {
		DOCA_LOG_DBG("SR-IOV: device reports dynamic SR-IOV unsupported");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	result = doca_devemu_pci_cap_get_max_total_vfs(devinfo, &caps->max_total_vfs);
	if (result != DOCA_SUCCESS)
		goto probe_failed;
	result = doca_devemu_pci_cap_get_dynamic_vf_pool_size(devinfo, &caps->pool_size);
	if (result != DOCA_SUCCESS)
		goto probe_failed;
	result = doca_devemu_pci_cap_get_max_vf_stride(devinfo, &caps->max_vf_stride);
	if (result != DOCA_SUCCESS)
		goto probe_failed;
	result = doca_devemu_pci_cap_get_max_first_vf_offset(devinfo, &caps->max_first_vf_offset);
	if (result != DOCA_SUCCESS)
		goto probe_failed;

	/* Per-function MSI-X / doorbell ceilings (best-effort). On query failure
	 * leave 0, which validate_caps() treats as "unknown" and skips. */
	if (doca_devemu_pci_tlp_cap_type_get_max_num_msix(devinfo, &caps->max_num_msix_per_device) != DOCA_SUCCESS)
		caps->max_num_msix_per_device = 0;
	if (doca_devemu_pci_tlp_cap_type_get_max_num_db(devinfo, &caps->max_num_db_per_device) != DOCA_SUCCESS)
		caps->max_num_db_per_device = 0;

	/* A zero pool means firmware has not provisioned a dynamic-VF range, so
	 * fall back to the non-rep_ex path rather than failing a later reserve. */
	if (caps->pool_size == 0) {
		DOCA_LOG_WARN("SR-IOV: dynamic-VF pool size is 0 (firmware not provisioned); per-VF backing disabled");
		memset(caps, 0, sizeof(*caps));
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	caps->dynamic_sriov_supported = true;
	DOCA_LOG_INFO("SR-IOV: dynamic SR-IOV supported (max_total_vfs=%u, pool_size=%u, "
		      "max_vf_stride=%u, max_first_vf_offset=%u, max_msix/dev=%u, max_db/dev=%u)",
		      caps->max_total_vfs,
		      caps->pool_size,
		      caps->max_vf_stride,
		      caps->max_first_vf_offset,
		      caps->max_num_msix_per_device,
		      caps->max_num_db_per_device);
	return DOCA_SUCCESS;

probe_failed:
	DOCA_LOG_ERR("SR-IOV: capability probe failed: %s", doca_error_get_descr(result));
	memset(caps, 0, sizeof(*caps));
	return result;
}

/*
 * Reject an aggregate VF count (num_pf * total_vfs) that exceeds the device-wide
 * VF engine pool (VNET_SRIOV_MAX_TOTAL_VFS_ALL_PF). The per-PF cap and the VQ
 * budget do not bound this when many PFs each use few queue pairs.
 */
static doca_error_t vnet_sriov_validate_total_vf_count(const struct tlp_context *tlp_ctx)
{
	uint32_t num_pf = tlp_ctx->num_ep ? tlp_ctx->num_ep : 1;
	uint32_t total_vfs_all = num_pf * (uint32_t)tlp_ctx->sriov_total_vfs;

	if (total_vfs_all > VNET_SRIOV_MAX_TOTAL_VFS_ALL_PF) {
		DOCA_LOG_ERR("SR-IOV: aggregate VF count %u (%u PF x %u VF) exceeds device-wide VF capacity %u",
			     total_vfs_all,
			     num_pf,
			     tlp_ctx->sriov_total_vfs,
			     (unsigned)VNET_SRIOV_MAX_TOTAL_VFS_ALL_PF);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

doca_error_t vnet_sriov_validate_caps(const struct tlp_context *tlp_ctx, const struct vnet_sriov_caps *caps)
{
	if (tlp_ctx == NULL || caps == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Upper-bound re-check (the CLI parser already range-checks at parse time;
	 * validate_caps runs only when SR-IOV is enabled, so total_vfs is > 0). */
	if (tlp_ctx->sriov_total_vfs > VNET_SRIOV_MAX_TOTAL_VFS) {
		DOCA_LOG_ERR("SR-IOV: sriov_total_vfs=%u exceeds max %u",
			     tlp_ctx->sriov_total_vfs,
			     VNET_SRIOV_MAX_TOTAL_VFS);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Single-bus ARI fit: VFs are ARI functions 1..N on the PF's bus, so the
	 * highest VF Function Number (first_vf_offset + (total-1)*stride) must not
	 * exceed 255. Written generally so a future stride/offset change is
	 * validated too; exceeding this needs multi-bus routing (out of scope). */
	if (VNET_SRIOV_REQUIRED_FIRST_VF_OFFSET + (tlp_ctx->sriov_total_vfs - 1) * VNET_SRIOV_REQUIRED_VF_STRIDE >
	    VNET_SRIOV_MAX_ARI_FUNCTION) {
		DOCA_LOG_ERR("SR-IOV: sriov_total_vfs=%u does not fit single-bus ARI (max function %u)",
			     tlp_ctx->sriov_total_vfs,
			     VNET_SRIOV_MAX_ARI_FUNCTION);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Supported per-PF VF cap (tighter than the ARI ceiling above). */
	if (tlp_ctx->sriov_total_vfs > VNET_SRIOV_MAX_VFS_PER_PF) {
		DOCA_LOG_ERR("SR-IOV: sriov_total_vfs=%u exceeds supported per-PF max %u",
			     tlp_ctx->sriov_total_vfs,
			     VNET_SRIOV_MAX_VFS_PER_PF);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Device-wide VF datapath budget: rejecting oversubscription here avoids a
	 * VF VQ-start failing only after the guest set DRIVER_OK (which can wedge the
	 * host). per_vf_vqs = VNET_TOTAL_VQS(max_qp) = sriov_num_vf_msix - 1 (read
	 * from msix since tlp_ctx->max_queue_pairs is not set until after this). */
	{
		uint32_t num_pf = tlp_ctx->num_ep ? tlp_ctx->num_ep : 1;
		uint32_t per_vf_vqs = tlp_ctx->sriov_num_vf_msix ? (uint32_t)(tlp_ctx->sriov_num_vf_msix - 1) : 0;
		uint64_t total_vf_vqs = (uint64_t)num_pf * tlp_ctx->sriov_total_vfs * per_vf_vqs;

		if (total_vf_vqs > VNET_SRIOV_MAX_TOTAL_VF_VQS) {
			DOCA_LOG_ERR("SR-IOV: aggregate VF datapath footprint %llu VQs "
				     "(%u PF x %u VF x %u VQ/VF) exceeds device-wide budget %u "
				     "(reference envelope %u PF x %u VF x %u QP); "
				     "reduce --num-ep, --total-vfs, or queue pairs",
				     (unsigned long long)total_vf_vqs,
				     num_pf,
				     tlp_ctx->sriov_total_vfs,
				     per_vf_vqs,
				     (unsigned)VNET_SRIOV_MAX_TOTAL_VF_VQS,
				     VNET_SRIOV_REF_MAX_PF,
				     VNET_SRIOV_MAX_VFS_PER_PF,
				     VNET_SRIOV_REF_MAX_QP);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	/* Aggregate VF count must also fit the device-wide VF engine pool. */
	{
		doca_error_t result = vnet_sriov_validate_total_vf_count(tlp_ctx);

		if (result != DOCA_SUCCESS)
			return result;
	}

	/* SDK-bound checks only run when the probe yielded real values; skipped
	 * when caps is all-zero (dynamic SR-IOV unavailable). */
	if (!caps->dynamic_sriov_supported)
		return DOCA_SUCCESS;

	if (tlp_ctx->sriov_total_vfs > caps->max_total_vfs) {
		DOCA_LOG_ERR("SR-IOV: sriov_total_vfs=%u exceeds device max_total_vfs=%u",
			     tlp_ctx->sriov_total_vfs,
			     caps->max_total_vfs);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (caps->max_vf_stride < VNET_SRIOV_REQUIRED_VF_STRIDE) {
		DOCA_LOG_ERR("SR-IOV: device max_vf_stride=%u below required %d",
			     caps->max_vf_stride,
			     VNET_SRIOV_REQUIRED_VF_STRIDE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (caps->max_first_vf_offset < VNET_SRIOV_REQUIRED_FIRST_VF_OFFSET) {
		DOCA_LOG_ERR("SR-IOV: device max_first_vf_offset=%u below required %d",
			     caps->max_first_vf_offset,
			     VNET_SRIOV_REQUIRED_FIRST_VF_OFFSET);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Per-VF MSI-X / doorbells must fit the per-function HW max (0 = unknown). */
	if (caps->max_num_msix_per_device != 0 && tlp_ctx->sriov_num_vf_msix > caps->max_num_msix_per_device) {
		DOCA_LOG_ERR("SR-IOV: per-VF MSI-X=%u exceeds device per-function max=%u",
			     tlp_ctx->sriov_num_vf_msix,
			     caps->max_num_msix_per_device);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (caps->max_num_db_per_device != 0 && tlp_ctx->sriov_num_vf_db > caps->max_num_db_per_device) {
		DOCA_LOG_ERR("SR-IOV: per-VF doorbells=%u exceeds device per-function max=%u",
			     tlp_ctx->sriov_num_vf_db,
			     caps->max_num_db_per_device);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Total VF count across all PFs must fit the dynamic-VF pool. */
	if (caps->pool_size != 0) {
		uint32_t num_pf = tlp_ctx->num_ep ? tlp_ctx->num_ep : 1;
		uint32_t total_vfs_all = num_pf * (uint32_t)tlp_ctx->sriov_total_vfs;

		if (total_vfs_all > caps->pool_size) {
			DOCA_LOG_ERR("SR-IOV: total VF count %u (%u PF x %u VF) exceeds device dynamic-VF pool=%u",
				     total_vfs_all,
				     num_pf,
				     tlp_ctx->sriov_total_vfs,
				     caps->pool_size);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}
	return DOCA_SUCCESS;
}

/*********************************************************************************************************************
 *                                                                                                                   *
 *  Pool allocator (cursor; LIFO release)                                                                            *
 *                                                                                                                   *
 *********************************************************************************************************************/

void vnet_sriov_pool_init(struct tlp_context *tlp_ctx, uint16_t pool_size)
{
	if (tlp_ctx == NULL)
		return;

	tlp_ctx->sriov_pool.pool_size = pool_size;
	tlp_ctx->sriov_pool.next_idx = 0;
	tlp_ctx->sriov_pool.allocated = 0;
	DOCA_LOG_DBG("SR-IOV: pool initialized with size=%u", pool_size);
}

doca_error_t vnet_sriov_pool_alloc_range(struct tlp_context *tlp_ctx, uint16_t count, uint32_t *out_first_idx)
{
	if (tlp_ctx == NULL || out_first_idx == NULL || count == 0)
		return DOCA_ERROR_INVALID_VALUE;

	if ((uint32_t)tlp_ctx->sriov_pool.next_idx + count > tlp_ctx->sriov_pool.pool_size) {
		DOCA_LOG_DBG("SR-IOV: pool alloc count=%u failed (next_idx=%u, pool_size=%u)",
			     count,
			     tlp_ctx->sriov_pool.next_idx,
			     tlp_ctx->sriov_pool.pool_size);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* The cursor is uint16_t (it indexes the uint16_t pool-size cap), while
	 * out_first_idx is uint32_t to match the SDK's first_dynamic_vf_idx type. */
	*out_first_idx = tlp_ctx->sriov_pool.next_idx;
	tlp_ctx->sriov_pool.next_idx = (uint16_t)(tlp_ctx->sriov_pool.next_idx + count);
	tlp_ctx->sriov_pool.allocated = (uint16_t)(tlp_ctx->sriov_pool.allocated + count);
	DOCA_LOG_INFO("SR-IOV: pool reserved range [%u, %u) (count=%u, cursor=%u)",
		      *out_first_idx,
		      *out_first_idx + count,
		      count,
		      tlp_ctx->sriov_pool.next_idx);
	return DOCA_SUCCESS;
}

void vnet_sriov_pool_release_range(struct tlp_context *tlp_ctx, uint32_t first_idx, uint16_t count)
{
	if (tlp_ctx == NULL || count == 0)
		return;

	/* LIFO fast path: releasing the most-recent allocation rewinds the cursor
	 * so the indices are immediately reusable. */
	if ((uint32_t)first_idx + count == tlp_ctx->sriov_pool.next_idx)
		tlp_ctx->sriov_pool.next_idx = (uint16_t)first_idx;
	else
		DOCA_LOG_DBG("SR-IOV: non-LIFO pool release [%u, %u) (cursor=%u); indices reclaimed on full drain",
			     first_idx,
			     first_idx + count,
			     tlp_ctx->sriov_pool.next_idx);

	if (count <= tlp_ctx->sriov_pool.allocated)
		tlp_ctx->sriov_pool.allocated = (uint16_t)(tlp_ctx->sriov_pool.allocated - count);

	/* Once every reserved range is freed, reset the cursor so a non-LIFO
	 * release order (multi-PF teardown) strands no indices. */
	if (tlp_ctx->sriov_pool.allocated == 0)
		tlp_ctx->sriov_pool.next_idx = 0;

	DOCA_LOG_DBG("SR-IOV: pool released range [%u, %u) (cursor=%u, outstanding=%u)",
		     first_idx,
		     first_idx + count,
		     tlp_ctx->sriov_pool.next_idx,
		     tlp_ctx->sriov_pool.allocated);
}

/*********************************************************************************************************************
 *                                                                                                                   *
 *  PF representor creation with reserved VF range                                                                   *
 *                                                                                                                   *
 *********************************************************************************************************************/

doca_error_t vnet_sriov_create_pf_rep_ex(struct tlp_context *tlp_ctx,
					 uint32_t first_vf_idx,
					 struct pci_device_config *pf_endpoint)
{
	struct doca_devemu_pci_rep_init_attr *init_attr = NULL;
	doca_error_t result;
	doca_error_t destroy_result;

	if (tlp_ctx == NULL || pf_endpoint == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Build the PF representor bound to the reserved VF range. Stride and
	 * first-VF-offset are the fixed no-ARI values (= 1); the per-VF MSI-X and
	 * doorbell counts come from the config and are provisioned per VF by the
	 * SDK as part of this call. */
	result = doca_devemu_pci_rep_init_attr_create(&init_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to allocate rep init attr: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_rep_init_attr_set_total_vfs(init_attr, tlp_ctx->sriov_total_vfs);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_first_dynamic_vf_idx(init_attr, first_vf_idx);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_vf_stride(init_attr, (uint16_t)VNET_SRIOV_REQUIRED_VF_STRIDE);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_first_vf_offset(init_attr,
								   (uint16_t)VNET_SRIOV_REQUIRED_FIRST_VF_OFFSET);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_vf_num_msix(init_attr, tlp_ctx->sriov_num_vf_msix);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_vf_num_db(init_attr, tlp_ctx->sriov_num_vf_db);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;

	result = doca_devemu_pci_type_create_rep_ex(tlp_ctx->pci_type, init_attr, &pf_endpoint->rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: create_rep_ex failed (total_vfs=%u, first_vf_idx=%u): %s",
			     tlp_ctx->sriov_total_vfs,
			     first_vf_idx,
			     doca_error_get_descr(result));
		goto destroy_attr;
	}

	DOCA_LOG_INFO("SR-IOV: PF representor created with VF range [%u, %u) (vf_num_msix=%u, vf_num_db=%u)",
		      first_vf_idx,
		      first_vf_idx + tlp_ctx->sriov_total_vfs,
		      tlp_ctx->sriov_num_vf_msix,
		      tlp_ctx->sriov_num_vf_db);

destroy_attr:
	destroy_result = doca_devemu_pci_rep_init_attr_destroy(init_attr);
	if (destroy_result != DOCA_SUCCESS)
		DOCA_LOG_WARN("SR-IOV: failed to destroy rep init attr: %s", doca_error_get_descr(destroy_result));
	return result;
}

/*********************************************************************************************************************
 *                                                                                                                   *
 *  VF representor enumeration                                                                                       *
 *                                                                                                                   *
 *********************************************************************************************************************/

/* Look up the PF's VUID from its open representor, used to filter the global VF
 * rep list to those whose parent PF VUID matches us. */
static doca_error_t sriov_get_pf_vuid(struct doca_dev_rep *pf_rep, char *out_vuid, uint32_t size)
{
	struct doca_devinfo_rep *rep_info = doca_dev_rep_as_devinfo(pf_rep);

	if (rep_info == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	return doca_devinfo_rep_get_vuid(rep_info, out_vuid, size);
}

/* Try to open one rep_info entry into vf_slots[].
 *
 * Filters: must be a VF function, parent PF VUID must match this PF, and the
 * SDK-reported (PF-relative) vf_index must be < total_vf. On the first
 * confirmed match the slot array is lazy-allocated; the rep is opened into
 * vf_slots[vf_index] and a globally-unique VF id (first_vf_idx + vf_index) is
 * recorded. The reported vf_index is PF-relative (each PF numbers its VFs from
 * 0) so it is the local slot index; first_vf_idx is the SDK's global accounting
 * and is folded in only for the unique VF id.
 *
 * Return contract: DOCA_SUCCESS covers an opened or deliberately-skipped rep
 * (wrong type / different PF / out-of-range / duplicate); *out_opened_count is
 * incremented only on an actual open. A non-SUCCESS return is a real failure
 * (rep open) that aborts discovery. */
static doca_error_t sriov_try_open_vf_rep(struct tlp_context *tlp_ctx,
					  struct pci_device_config *pf_endpoint,
					  struct doca_devinfo_rep *rep_info,
					  const char *pf_vuid,
					  uint16_t *out_opened_count)
{
	struct vnet_sriov_pf_state *sriov = pf_endpoint->sriov;
	enum doca_pci_func_type func_type;
	char this_pf_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	uint32_t vf_index = 0;
	uint32_t local_index;
	uint32_t global_vf_index;
	doca_error_t result;

	result = doca_devinfo_rep_get_pci_func_type(rep_info, &func_type);
	if (result != DOCA_SUCCESS || func_type != DOCA_PCI_FUNC_TYPE_VF)
		return DOCA_SUCCESS; /* not a VF rep — skip silently */

	result = doca_devinfo_rep_get_pf_vuid(rep_info, this_pf_vuid, sizeof(this_pf_vuid));
	if (result != DOCA_SUCCESS)
		return DOCA_SUCCESS; /* unattributed rep — skip */
	if (strncmp(this_pf_vuid, pf_vuid, DOCA_DEVINFO_REP_VUID_SIZE) != 0)
		return DOCA_SUCCESS; /* belongs to a different PF — skip */

	result = doca_devinfo_rep_get_vf_index(rep_info, &vf_index);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("SR-IOV: rep with matching pf_vuid has no vf_index: %s", doca_error_get_descr(result));
		return DOCA_SUCCESS; /* tolerate a malformed entry */
	}
	/* The SDK reports a PF-relative vf_index (0..total_vf-1); use it directly
	 * as the slot index. The pool base (first_vf_idx) is the SDK's global
	 * accounting and must NOT be subtracted (that would drop every VF rep of a
	 * PF whose pool base is non-zero, e.g. the second PF's range [4,8)). */
	if (vf_index >= sriov->total_vf) {
		DOCA_LOG_DBG("SR-IOV: VF rep vf_index=%u outside our range [0, %u); skip", vf_index, sriov->total_vf);
		return DOCA_SUCCESS;
	}

	/* First confirmed VF rep for this PF: lazy-allocate the slot array.
	 * Deferring to a confirmed match keeps vf_slots NULL when this PF has no
	 * VF backing, which the destroy path uses to tell "owns a pool range"
	 * from "no VFs". */
	if (sriov->vf_slots == NULL) {
		sriov->vf_slots = calloc(sriov->total_vf, sizeof(*sriov->vf_slots));
		if (sriov->vf_slots == NULL) {
			DOCA_LOG_ERR("SR-IOV: failed to allocate vf_slots[%u]", sriov->total_vf);
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	local_index = vf_index;
	global_vf_index = sriov->first_vf_idx + vf_index;
	if (sriov->vf_slots[local_index].rep != NULL) {
		DOCA_LOG_WARN("SR-IOV: duplicate VF rep at vf_index=%u (local %u); skip", vf_index, local_index);
		return DOCA_SUCCESS;
	}

	result = doca_dev_rep_open(rep_info, &sriov->vf_slots[local_index].rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to open VF rep at vf_index=%u: %s",
			     vf_index,
			     doca_error_get_descr(result));
		return result;
	}
	sriov->vf_slots[local_index].vf_index = global_vf_index;

	/* Build the per-VF VirtIO cfg-space template; it sits idle in vf_dev until
	 * VFE 0->1 binds a tlp_dev to it. On failure undo the rep_open so the slot
	 * returns to a clean state. */
	result = vnet_pci_device_init_vf(tlp_ctx, pf_endpoint, global_vf_index, &sriov->vf_slots[local_index].vf_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to init VF[%u] cfg-space template: %s",
			     local_index,
			     doca_error_get_descr(result));
		(void)doca_dev_rep_close(sriov->vf_slots[local_index].rep);
		sriov->vf_slots[local_index].rep = NULL;
		sriov->vf_slots[local_index].vf_index = 0;
		return result;
	}

	(*out_opened_count)++;
	DOCA_LOG_INFO("SR-IOV: opened VF rep[local=%u] (vf_index=%u) and initialized template",
		      local_index,
		      global_vf_index);
	return DOCA_SUCCESS;
}

doca_error_t vnet_sriov_discover_vf_reps(struct tlp_context *tlp_ctx, struct pci_device_config *pf_endpoint)
{
	struct doca_devinfo_rep **rep_list = NULL;
	uint32_t nb_reps = 0;
	uint16_t opened = 0;
	char pf_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	doca_error_t result;
	uint32_t i;

	if (tlp_ctx == NULL || pf_endpoint == NULL || pf_endpoint->sriov == NULL || pf_endpoint->rep == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	result = sriov_get_pf_vuid(pf_endpoint->rep, pf_vuid, sizeof(pf_vuid));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to get PF VUID for VF rep filter: %s", doca_error_get_descr(result));
		return result;
	}

	/* VFs provisioned for the PF appear as emulated PCI functions, so the
	 * EMULATED filter surfaces them; the per-entry func_type == VF + pf_vuid
	 * match below narrows the list to this PF's VFs. */
	result = doca_devinfo_rep_create_list(tlp_ctx->dev, DOCA_DEVINFO_REP_FILTER_EMULATED, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		/* NOT_SUPPORTED is normal on devices without an emulated rep filter
		 * (no dynamic SR-IOV); treat as "no VF reps to enumerate". */
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_INFO("SR-IOV: rep_create_list NOT_SUPPORTED — no VF reps to discover");
			return DOCA_SUCCESS;
		}
		DOCA_LOG_ERR("SR-IOV: rep_create_list failed: %s", doca_error_get_descr(result));
		return result;
	}

	if (nb_reps == 0) {
		DOCA_LOG_INFO("SR-IOV: 0 VF reps available system-wide");
		result = DOCA_SUCCESS;
		goto out;
	}

	/* Open every VF rep that belongs to this PF. sriov_try_open_vf_rep()
	 * lazy-allocates vf_slots on the first confirmed match, so a PF with no VF
	 * backing leaves vf_slots NULL. */
	for (i = 0; i < nb_reps; i++) {
		result = sriov_try_open_vf_rep(tlp_ctx, pf_endpoint, rep_list[i], pf_vuid, &opened);
		if (result != DOCA_SUCCESS) {
			vnet_sriov_close_vf_reps(pf_endpoint->sriov);
			free(pf_endpoint->sriov->vf_slots);
			pf_endpoint->sriov->vf_slots = NULL;
			goto out;
		}
	}

	if (pf_endpoint->sriov->vf_slots == NULL) {
		DOCA_LOG_INFO("SR-IOV: 0 VF reps discovered for this PF (vf_slots stays NULL)");
		result = DOCA_SUCCESS;
		goto out;
	}

	if (opened != pf_endpoint->sriov->total_vf)
		DOCA_LOG_WARN("SR-IOV: opened %u/%u VF reps for this PF (range gaps may indicate an SDK issue)",
			      opened,
			      pf_endpoint->sriov->total_vf);
	else
		DOCA_LOG_INFO("SR-IOV: opened all %u VF reps for this PF", opened);
	result = DOCA_SUCCESS;

out:
	(void)doca_devinfo_rep_destroy_list(rep_list);
	return result;
}

void vnet_sriov_close_vf_reps(struct vnet_sriov_pf_state *sriov)
{
	uint16_t i;

	if (sriov == NULL || sriov->vf_slots == NULL)
		return;

	for (i = 0; i < sriov->total_vf; i++) {
		doca_error_t stop_result;

		/* Stop and destroy a still-started VF tlp_dev first: a started
		 * tlp_dev holds a refcount on its rep, so closing the rep below
		 * would fail while VFs are still active at teardown (e.g. app exit
		 * with SR-IOV still enabled). Idempotent on inactive slots. */
		stop_result = vnet_sriov_stop_vf_slot(&sriov->vf_slots[i]);
		if (stop_result != DOCA_SUCCESS && stop_result != DOCA_ERROR_NOT_FOUND)
			DOCA_LOG_WARN("SR-IOV: failed to stop VF slot[%u] during close: %s",
				      i,
				      doca_error_get_descr(stop_result));

		/* Tear down the per-VF cfg-space template (app-side state, no SDK
		 * dependency) before closing the rep handle. */
		if (sriov->vf_slots[i].vf_dev != NULL) {
			vnet_pci_device_destroy_vf(sriov->vf_slots[i].vf_dev);
			sriov->vf_slots[i].vf_dev = NULL;
		}
		if (sriov->vf_slots[i].rep != NULL) {
			doca_error_t r = doca_dev_rep_close(sriov->vf_slots[i].rep);

			if (r != DOCA_SUCCESS)
				DOCA_LOG_WARN("SR-IOV: failed to close VF rep[%u]: %s", i, doca_error_get_descr(r));
			sriov->vf_slots[i].rep = NULL;
		}
	}
}

/*********************************************************************************************************************
 *                                                                                                                   *
 *  Per-VF tlp_dev create/start (host VFE 0->1 path)                                                                 *
 *                                                                                                                   *
 *********************************************************************************************************************/

doca_error_t vnet_sriov_start_vf_slot(struct tlp_context *tlp_ctx, struct vnet_vf_slot *slot)
{
	doca_error_t result;

	if (tlp_ctx == NULL || slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* No opened rep: nothing to start. Returning NOT_FOUND lets the fan-out
	 * worker iterate over every slot uniformly without per-slot pre-checks. */
	if (slot->rep == NULL)
		return DOCA_ERROR_NOT_FOUND;

	/* Already started (re-entrant fan-out or a duplicate host VFE 0->1). */
	if (atomic_load(&slot->active))
		return DOCA_SUCCESS;

	result = doca_devemu_pci_tlp_dev_create(tlp_ctx->pci_type, slot->rep, &slot->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: tlp_dev_create failed for VF[vf_index=%u]: %s",
			     slot->vf_index,
			     doca_error_get_descr(result));
		slot->tlp_dev = NULL;
		return result;
	}

	result = doca_devemu_pci_tlp_dev_start(slot->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: tlp_dev_start failed for VF[vf_index=%u]: %s",
			     slot->vf_index,
			     doca_error_get_descr(result));
		(void)doca_devemu_pci_tlp_dev_destroy(slot->tlp_dev);
		slot->tlp_dev = NULL;
		return result;
	}

	atomic_store(&slot->active, true);
	DOCA_LOG_INFO("SR-IOV: VF[vf_index=%u] tlp_dev started", slot->vf_index);
	return DOCA_SUCCESS;
}

void vnet_sriov_derive_vf_mac(const uint8_t *base_mac, uint16_t num_ep, uint32_t vf_index, uint8_t *out_mac)
{
	/* base16 + num_ep + vf_index, folded into the low 2 MAC bytes. PFs occupy
	 * [0, num_ep); VFs occupy num_ep + vf_index (the globally-unique VF id), so
	 * VFs never collide with PFs or across PFs. Wraps only if the base MAC's low
	 * 16 bits are within (num_ep + total_vfs) of 0xFFFF; pick a base MAC with
	 * headroom in the low bytes. Shared by the VF cfg-space template and the VF
	 * offload engine so both present the identical MAC. */
	uint16_t mac_offset = (uint16_t)(num_ep + vf_index);
	uint16_t mac_low16;

	memcpy(out_mac, base_mac, ETH_ALEN);
	mac_low16 = (uint16_t)((((uint16_t)out_mac[4] << 8) | out_mac[5]) + mac_offset);
	out_mac[4] = (uint8_t)(mac_low16 >> 8);
	out_mac[5] = (uint8_t)(mac_low16 & 0xFF);
}

doca_error_t vnet_sriov_stop_vf_slot(struct vnet_vf_slot *slot)
{
	doca_error_t result;

	if (slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Teardown ordering is engine -> tlp_dev. Do not depend on rep presence:
	 * LU/error paths can drop rep first while handles still need cleanup. */
	if (slot->offload_engine != NULL) {
		result = vnet_pci_dev_vf_engine_destroy(slot);
		if (result != DOCA_SUCCESS)
			return result;
	}

	/* No started endpoint. */
	if (slot->tlp_dev == NULL) {
		atomic_store(&slot->active, false);
		return (slot->rep == NULL) ? DOCA_ERROR_NOT_FOUND : DOCA_SUCCESS;
	}

	/* Tear down the per-VF datapath engine first: a live engine holds a
	 * refcount on the VF tlp_dev's endpoint, so it must go before the
	 * tlp_dev is stopped/destroyed. No-op when no engine was created. */
	result = doca_devemu_pci_tlp_dev_stop(slot->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: tlp_dev_stop failed for VF[vf_index=%u]: %s",
			     slot->vf_index,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_tlp_dev_destroy(slot->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: tlp_dev_destroy failed for VF[vf_index=%u]: %s",
			     slot->vf_index,
			     doca_error_get_descr(result));
		return result;
	}

	slot->tlp_dev = NULL;
	atomic_store(&slot->active, false);
	DOCA_LOG_INFO("SR-IOV: VF[vf_index=%u] tlp_dev stopped", slot->vf_index);
	return DOCA_SUCCESS;
}
