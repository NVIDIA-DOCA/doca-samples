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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <doca_log.h>
#include <doca_devemu_vblk_offload_engine.h>
#include <doca_devemu_vblk_io.h>
#include <doca_devemu_virtio_tlp.h>

#include "vblk_handover.h"
#include "vblk_ipc_msgs.h"
#include "vblk_ctrl_lu.h"
#include "vblk_pci_dev_core_lu.h"

DOCA_LOG_REGISTER(VBLK_HANDOVER);

static const char *phase_str(enum vblk_ho_phase p)
{
	switch (p) {
	case VBLK_HO_IDLE:
		return "IDLE";
	case VBLK_HO_SETUP:
		return "SETUP";
	case VBLK_HO_WAIT_DEVICE:
		return "WAIT_DEVICE";
	case VBLK_HO_GET_STATE:
		return "GET_STATE";
	case VBLK_HO_SWITCHOVER:
		return "SWITCHOVER";
	case VBLK_HO_SRC_DRAINING:
		return "SRC_DRAINING";
	case VBLK_HO_DST_RESUMING:
		return "DST_RESUMING";
	case VBLK_HO_CLEANUP:
		return "CLEANUP";
	case VBLK_HO_DONE:
		return "DONE";
	case VBLK_HO_FAILED:
		return "FAILED";
	}
	return "?";
}

doca_error_t vblk_export_desc_store(void **destp, size_t *lenp, const void *src, size_t src_len)
{
	void *copy = malloc(src_len);

	if (copy == NULL)
		return DOCA_ERROR_NO_MEMORY;
	memcpy(copy, src, src_len);
	*destp = copy;
	*lenp = src_len;
	return DOCA_SUCCESS;
}

void vblk_export_desc_free(void **destp, size_t *lenp)
{
	free(*destp);
	*destp = NULL;
	if (lenp != NULL)
		*lenp = 0;
}

static void ho_release_dst_state(struct vblk_ho_ctx *ho)
{
	free(ho->initial_cfg);
	ho->initial_cfg = NULL;
}

static void ho_set_phase(struct vblk_ho_ctx *ho, enum vblk_ho_phase p)
{
	if ((p == VBLK_HO_FAILED || p == VBLK_HO_DONE) && ho->role == VBLK_HO_ROLE_DST)
		ho_release_dst_state(ho);
	DOCA_LOG_INFO("HO [%s]: %s -> %s",
		      ho->role == VBLK_HO_ROLE_SRC ? "SRC" : "DST",
		      phase_str(ho->phase),
		      phase_str(p));
	ho->phase = p;
}

static struct doca_devemu_virtio_offload_engine *get_virtio_oe(struct vblk_ho_ctx *ho)
{
	if (!ho->ctrl || !ho->ctrl->vq_engine)
		return NULL;
	return doca_devemu_vblk_offload_engine_as_virtio_offload(ho->ctrl->vq_engine);
}

static void src_handle_setup(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	struct vblk_msg_ho_setup_ack ack = {.num_devices = 1};
	vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_SETUP_ACK, &ack, sizeof(ack));
	ho_set_phase(ho, VBLK_HO_WAIT_DEVICE);
}

static void src_handle_get_state(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	struct vblk_ctrl *ctrl = ho->ctrl;

	ho_set_phase(ho, VBLK_HO_GET_STATE);

	if (ho->export_desc == NULL || ho->export_desc_len == 0) {
		DOCA_LOG_ERR("HO SRC: no export_desc available");
		vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_GET_STATE_NACK, NULL, 0);
		ho_set_phase(ho, VBLK_HO_FAILED);
		return;
	}

	struct vblk_lu_device_cfg device_cfg = {0};
	device_cfg.vq_cfg.num_queues = ctrl->num_queues;
	for (uint16_t q = 0; q < ctrl->num_queues && q < VBLK_PCI_VIRTIO_MAX_QUEUES; q++)
		device_cfg.vq_cfg.vqs[q] = ctrl->vqs[q].cfg;

	doca_devemu_vblk_offload_engine_get_seg_max(ctrl->vq_engine, &device_cfg.seg_max);
	struct doca_devemu_virtio_offload_engine *voe_get =
		doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
	doca_devemu_virtio_offload_engine_get_indir_descs_enabled(voe_get, &device_cfg.indir_desc_enabled);

	uint32_t desc_len = (uint32_t)ho->export_desc_len;
	uint32_t cfg_len = sizeof(struct vblk_lu_device_cfg);
	size_t total = vblk_msg_get_state_ack_size(desc_len, cfg_len);

	struct vblk_msg_ho_get_state_ack *ack = calloc(1, total);
	if (!ack) {
		DOCA_LOG_ERR("HO SRC: failed to allocate GET_STATE_ACK (%zu bytes)", total);
		ho_set_phase(ho, VBLK_HO_FAILED);
		return;
	}
	ack->export_desc_len = desc_len;
	ack->device_cfg_len = cfg_len;
	memcpy(ack->data, ho->export_desc, desc_len);
	memcpy(ack->data + desc_len, &device_cfg, cfg_len);

	DOCA_LOG_INFO("HO SRC: sending GET_STATE_ACK (exp=%u cfg=%u nq=%u)", desc_len, cfg_len, ctrl->num_queues);
	vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_GET_STATE_ACK, ack, (uint32_t)total);
	free(ack);
	ho_set_phase(ho, VBLK_HO_SWITCHOVER);
}

static void src_send_begin_ack(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_BEGIN_ACK, NULL, 0);
	ho_set_phase(ho, VBLK_HO_CLEANUP);
}

static void src_handle_begin(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	DOCA_LOG_INFO("HO SRC: BEGIN — disabling offload engine");
	struct doca_devemu_virtio_offload_engine *oe = get_virtio_oe(ho);
	if (!oe) {
		vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_BEGIN_NACK, NULL, 0);
		ho_set_phase(ho, VBLK_HO_FAILED);
		return;
	}

	doca_error_t err = doca_devemu_virtio_offload_engine_disable(oe);
	if (err == DOCA_SUCCESS) {
		DOCA_LOG_INFO("HO SRC: engine disabled synchronously");
		src_send_begin_ack(ho, ep);
	} else if (err == DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_INFO("HO SRC: engine draining async");
		ho_set_phase(ho, VBLK_HO_SRC_DRAINING);
	} else {
		DOCA_LOG_ERR("HO SRC: disable failed: %s", doca_error_get_name(err));
		vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_BEGIN_NACK, NULL, 0);
		ho_set_phase(ho, VBLK_HO_FAILED);
	}
}

static void src_poll_draining(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	struct doca_devemu_virtio_offload_engine *oe = get_virtio_oe(ho);
	enum doca_devemu_virtio_offload_engine_states state;

	if (!oe || doca_devemu_virtio_offload_engine_get_state(oe, &state) != DOCA_SUCCESS)
		return;
	if (state == DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_DISABLED) {
		DOCA_LOG_INFO("HO SRC: engine fully disabled after drain");
		src_send_begin_ack(ho, ep);
	}
}

static void src_handle_end(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep, const void *payload, uint32_t len)
{
	uint8_t success = 1;
	if (len >= sizeof(struct vblk_msg_ho_end))
		success = ((const struct vblk_msg_ho_end *)payload)->success;

	struct doca_devemu_virtio_offload_engine *oe = get_virtio_oe(ho);

	if (success) {
		DOCA_LOG_INFO("HO SRC: END{SUCCESS} — handover complete");
		vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_END_ACK, NULL, 0);
		ho_set_phase(ho, VBLK_HO_DONE);
	} else {
		DOCA_LOG_INFO("HO SRC: END{FAIL} — rolling back, re-enabling engine");
		doca_error_t err = oe ? doca_devemu_virtio_offload_engine_enable(oe) : DOCA_ERROR_BAD_STATE;
		uint32_t ack = (err == DOCA_SUCCESS) ? VBLK_MSG_HO_END_ACK : VBLK_MSG_HO_END_NACK;
		if (err != DOCA_SUCCESS)
			DOCA_LOG_ERR("HO SRC: rollback failed: %s", doca_error_get_name(err));
		vblk_ipc_send_to(ep, ho->peer_path, ack, NULL, 0);
		ho_set_phase(ho, VBLK_HO_IDLE);
	}
}

static void dst_handle_setup_ack(struct vblk_ho_ctx *ho, const void *payload, uint32_t len)
{
	if (len < sizeof(struct vblk_msg_ho_setup_ack)) {
		ho_set_phase(ho, VBLK_HO_FAILED);
		return;
	}
	const struct vblk_msg_ho_setup_ack *ack = payload;
	DOCA_LOG_INFO("HO DST: SETUP_ACK devices=%u", ack->num_devices);

	vblk_ipc_send_to(ho->ipc, VBLK_IPC_TLP_PATH, VBLK_MSG_HO_INITIATE_ACK, NULL, 0);
	ho_set_phase(ho, VBLK_HO_WAIT_DEVICE);
}

static void dst_handle_device(struct vblk_ho_ctx *ho, const void *payload, uint32_t len)
{
	if (len < sizeof(struct vblk_msg_ho_device)) {
		ho_set_phase(ho, VBLK_HO_FAILED);
		return;
	}
	ho->current_device_id = ((const struct vblk_msg_ho_device *)payload)->device_id;
	DOCA_LOG_INFO("HO DST: migrating device %u — sending GET_STATE to SRC", ho->current_device_id);

	struct vblk_msg_ho_device dev = {.device_id = ho->current_device_id};
	vblk_ipc_send_to(ho->ipc, ho->peer_path, VBLK_MSG_HO_GET_STATE, &dev, sizeof(dev));
	ho_set_phase(ho, VBLK_HO_GET_STATE);
}

doca_error_t vblk_ho_dst_begin_switchover(struct vblk_ho_ctx *ho)
{
	struct vblk_ctrl *ctrl = ho->ctrl;

	if (ho->initial_cfg == NULL)
		return DOCA_ERROR_BAD_STATE;

	struct vblk_vq_cfg *cfg = &ho->initial_cfg->vq_cfg;
	for (uint16_t q = 0; q < cfg->num_queues && q < ctrl->num_queues; q++) {
		if (!cfg->vqs[q].queue_enable)
			continue;
		vblk_ctrl_ipc_queue_start(ctrl, q, &cfg->vqs[q]);
	}

	/* Wait for OE thread to create all VQs (STARTING → CONFIGURED) */
	for (uint16_t q = 0; q < ctrl->num_queues; q++) {
		if (ctrl->vqs[q].state == VBLK_VQ_DESTROYED)
			continue;
		while (atomic_load(&ctrl->vqs[q].state) != VBLK_VQ_CONFIGURED)
			usleep(1000);
	}

	DOCA_LOG_INFO("HO DST: all VQs parked — sending BEGIN to SRC");
	struct vblk_msg_ho_device dev = {.device_id = ho->current_device_id};
	vblk_ipc_send_to(ho->ipc, ho->peer_path, VBLK_MSG_HO_BEGIN, &dev, sizeof(dev));
	ho_set_phase(ho, VBLK_HO_SWITCHOVER);
	return DOCA_SUCCESS;
}

static void dst_handle_begin_ack(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	DOCA_LOG_INFO("HO DST: SRC disabled — enabling OE and resuming VQs");
	struct vblk_ctrl *ctrl = ho->ctrl;

	struct doca_devemu_virtio_offload_engine *oe = get_virtio_oe(ho);
	if (oe) {
		doca_error_t err = doca_devemu_virtio_offload_engine_enable(oe);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("HO DST: enable failed: %s", doca_error_get_name(err));
			struct vblk_msg_ho_end end = {.success = 0};
			vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_END, &end, sizeof(end));
			ho_set_phase(ho, VBLK_HO_CLEANUP);
			return;
		}
	}

	/* Advance parked VQs to BINDING so IO threads do bind + enable
	 * (which remaps DB UNMAPPED → MAPPED + starts DPA).
	 * Defer END until all VQs reach RUNNING — polled in vblk_ho_poll. */
	vblk_ctrl_resume_configured_vqs(ctrl);
	ho_set_phase(ho, VBLK_HO_DST_RESUMING);
}

static void dst_handle_end_ack(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	DOCA_LOG_INFO("HO DST: handover complete for device %u", ho->current_device_id);

	/* Notify IO-Engine */
	struct vblk_msg_ho_device dev = {.device_id = ho->current_device_id};
	vblk_ipc_send_to(ep, VBLK_IPC_TLP_PATH, VBLK_MSG_HO_DEVICE_ACK, &dev, sizeof(dev));

	ho_set_phase(ho, VBLK_HO_DONE);
}

void vblk_ho_init(struct vblk_ho_ctx *ho, enum vblk_ho_role role, struct vblk_ctrl *ctrl, struct vblk_ipc_ep *ipc)
{
	memset(ho, 0, sizeof(*ho));
	ho->role = role;
	ho->phase = VBLK_HO_IDLE;
	ho->ctrl = ctrl;
	ho->ipc = ipc;
	ho->peer_path = (role == VBLK_HO_ROLE_SRC) ? VBLK_IPC_EMU_DST_PATH : VBLK_IPC_EMU_SRC_PATH;
}

static doca_error_t dst_parse_get_state_ack(struct vblk_ho_ctx *ho, const void *payload, uint32_t len)
{
	const struct vblk_msg_ho_get_state_ack *ack = payload;

	if (len < sizeof(*ack) || ack->export_desc_len == 0 ||
	    len < vblk_msg_get_state_ack_size(ack->export_desc_len, ack->device_cfg_len))
		return DOCA_ERROR_INVALID_VALUE;

	DOCA_LOG_INFO("HO DST: GET_STATE_ACK export_desc_len=%u", ack->export_desc_len);

	doca_error_t alloc_err = vblk_export_desc_store(&ho->export_desc,
							&ho->export_desc_len,
							vblk_msg_get_state_ack_desc(ack),
							ack->export_desc_len);
	if (alloc_err != DOCA_SUCCESS)
		return alloc_err;

	if (ack->device_cfg_len >= sizeof(struct vblk_lu_device_cfg)) {
		ho->initial_cfg = malloc(sizeof(struct vblk_lu_device_cfg));
		if (!ho->initial_cfg) {
			vblk_export_desc_free(&ho->export_desc, &ho->export_desc_len);
			return DOCA_ERROR_NO_MEMORY;
		}
		memcpy(ho->initial_cfg, vblk_msg_get_state_ack_cfg(ack), sizeof(struct vblk_lu_device_cfg));
		DOCA_LOG_INFO("HO DST: device_cfg queues=%u", ho->initial_cfg->vq_cfg.num_queues);
	}

	return DOCA_SUCCESS;
}

doca_error_t vblk_ho_dst_get_initial_state(struct vblk_ho_ctx *ho)
{
	if (ho->role != VBLK_HO_ROLE_DST)
		return DOCA_ERROR_BAD_STATE;

	DOCA_LOG_INFO("HO DST: starting handshake");
	vblk_ipc_send_to(ho->ipc, VBLK_IPC_EMU_SRC_PATH, VBLK_MSG_HO_SETUP, NULL, 0);
	ho_set_phase(ho, VBLK_HO_SETUP);

	static uint8_t buf[VBLK_IPC_MAX_PAYLOAD];
	uint32_t type, len;

	while (ho->phase != VBLK_HO_FAILED) {
		if (vblk_ipc_recv(ho->ipc, &type, buf, sizeof(buf), &len) != DOCA_SUCCESS) {
			usleep(1000);
			continue;
		}
		switch (type) {
		case VBLK_MSG_HO_SETUP_ACK:
			dst_handle_setup_ack(ho, buf, len);
			break;
		case VBLK_MSG_HO_DEVICE:
			dst_handle_device(ho, buf, len);
			break;
		case VBLK_MSG_HO_GET_STATE_ACK:
			if (dst_parse_get_state_ack(ho, buf, len) != DOCA_SUCCESS) {
				ho_set_phase(ho, VBLK_HO_FAILED);
				break;
			}
			ho_set_phase(ho, VBLK_HO_SWITCHOVER);
			return DOCA_SUCCESS;
		case VBLK_MSG_HO_GET_STATE_NACK:
			ho_set_phase(ho, VBLK_HO_FAILED);
			break;
		default:
			break;
		}
	}
	return DOCA_ERROR_BAD_STATE;
}

static void dst_poll_resuming(struct vblk_ho_ctx *ho, struct vblk_ipc_ep *ep)
{
	struct vblk_ctrl *ctrl = ho->ctrl;

	for (uint16_t q = 0; q < ctrl->num_queues; q++) {
		enum vblk_vq_state st = atomic_load(&ctrl->vqs[q].state);
		if (st == VBLK_VQ_DESTROYED)
			continue;
		if (st != VBLK_VQ_RUNNING)
			return;
	}

	DOCA_LOG_INFO("HO DST: all VQs running — sending END{success}");
	struct vblk_msg_ho_end end = {.success = 1};
	vblk_ipc_send_to(ep, ho->peer_path, VBLK_MSG_HO_END, &end, sizeof(end));
	ho_set_phase(ho, VBLK_HO_CLEANUP);
}

void vblk_ho_poll(struct vblk_ho_ctx *ho)
{
	if (ho->phase == VBLK_HO_SRC_DRAINING)
		src_poll_draining(ho, ho->ipc);
	else if (ho->phase == VBLK_HO_DST_RESUMING)
		dst_poll_resuming(ho, ho->ipc);
}

bool vblk_ho_process_msg(struct vblk_ho_ctx *ho, uint32_t type, const void *payload, uint32_t len)
{
	if (type < VBLK_MSG_HO_SETUP || type > VBLK_MSG_HO_END_NACK)
		return false;

	if (ho->role == VBLK_HO_ROLE_SRC) {
		switch (type) {
		case VBLK_MSG_HO_SETUP:
			src_handle_setup(ho, ho->ipc);
			return true;
		case VBLK_MSG_HO_GET_STATE:
			src_handle_get_state(ho, ho->ipc);
			return true;
		case VBLK_MSG_HO_BEGIN:
			src_handle_begin(ho, ho->ipc);
			return true;
		case VBLK_MSG_HO_END:
			src_handle_end(ho, ho->ipc, payload, len);
			return true;
		default:
			break;
		}
	} else {
		switch (type) {
		case VBLK_MSG_HO_BEGIN_ACK:
			dst_handle_begin_ack(ho, ho->ipc);
			return true;
		case VBLK_MSG_HO_BEGIN_NACK:
			ho_set_phase(ho, VBLK_HO_FAILED);
			return true;
		case VBLK_MSG_HO_END_ACK:
			dst_handle_end_ack(ho, ho->ipc);
			return true;
		case VBLK_MSG_HO_END_NACK:
			ho_set_phase(ho, VBLK_HO_FAILED);
			return true;
		default:
			break;
		}
	}
	return false;
}
