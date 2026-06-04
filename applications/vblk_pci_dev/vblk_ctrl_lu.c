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
#include <stdbool.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dma.h>

#include <doca_devemu_virtio.h>
#include <doca_devemu_virtio_tlp.h>
#include <doca_devemu_vblk_io.h>
#include <doca_devemu_vblk_type.h>

#include <samples/common.h>
#include "vblk_utils_lu.h"
#include "vblk_mpool_lu.h"
#include "vblk_ctrl_lu.h"
#include "vblk_handover.h"

DOCA_LOG_REGISTER(VBLK_CTRL);

static struct doca_dev *g_vblk_dev;

#define VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE (1000000)
#define VBLK_IO_CTX_MAX_WAIT_ITERATIONS_RUNNING (1000000)

/* at the moment the context is global per controller, ideally controller will have io
 * context per core and will spit queues across contexts
 */

static struct doca_ctx *vblk_io_as_ctx(struct doca_devemu_vblk_io *io_ctx)
{
	return doca_devemu_virtio_io_as_ctx(doca_devemu_vblk_io_as_virtio_io(io_ctx));
}

static inline struct doca_ctx *vblk_ctrl_io_ctx(struct vblk_ctrl *ctrl, const int ctx_id)
{
	return vblk_io_as_ctx(ctrl->io_ctxs[ctx_id].vq_io_ctx);
}

static inline struct doca_ctx *vblk_ctrl_dma_ctx(struct vblk_ctrl *ctrl, const int ctx_id)
{
	return doca_dma_as_ctx(ctrl->io_ctxs[ctx_id].dma_ctx);
}

static inline struct vblk_mpool_set *vblk_io_ctx_mpool(struct vblk_ctrl_io_ctx *ctx)
{
	/* once multi threading is supported it will return per core mpool set */
	return ctx->mpool;
}

static doca_error_t vblk_ctrl_connect_io_ctx(struct vblk_ctrl *ctrl, const int ctx_id, struct doca_pe *pe)
{
	doca_error_t err;

	if (ctx_id < 0 || ctx_id >= ctrl->num_io_ctx)
		return DOCA_ERROR_INVALID_VALUE;

	err = doca_pe_connect_ctx(pe, vblk_ctrl_io_ctx(ctrl, ctx_id));
	if (err != DOCA_SUCCESS)
		return err;

	err = doca_pe_connect_ctx(pe, vblk_ctrl_dma_ctx(ctrl, ctx_id));
	if (err != DOCA_SUCCESS)
		return err;

	ctrl->io_ctxs[ctx_id].pe = pe;
	return DOCA_SUCCESS;
}

static doca_error_t vblk_ctrl_manager_set_max_seg_max(const struct doca_devinfo *devinfo, uint16_t max_seg_max)
{
	doca_error_t err;
	uint16_t supported_max_seg_max;

	err = doca_devemu_vblk_cap_get_max_seg_max(devinfo, &supported_max_seg_max);
	if (err != DOCA_SUCCESS) {
		return err;
	}

	if (max_seg_max > supported_max_seg_max) {
		DOCA_LOG_ERR("Error: configured seg_max:%d > supported seg_max:%d", max_seg_max, supported_max_seg_max);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	return doca_devemu_vblk_set_seg_max(max_seg_max);
}

static doca_error_t vblk_ctrl_manager_set_max_queue_size(const struct doca_devinfo *devinfo, uint16_t max_queue_size)
{
	doca_error_t err;
	uint16_t supported_max_queue_size;

	err = doca_devemu_vblk_cap_get_max_queue_size(devinfo, &supported_max_queue_size);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max queue size capability, err:%s", doca_error_get_name(err));
		return err;
	}

	if (max_queue_size > supported_max_queue_size) {
		DOCA_LOG_ERR("Error: configured queue_size:%d > supported queue_size:%d",
			     max_queue_size,
			     supported_max_queue_size);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	return doca_devemu_vblk_set_max_queue_size(max_queue_size);
}

doca_error_t vblk_init(const struct vblk_app_cfg *app_cfg, struct doca_dev **g_dev)
{
	doca_error_t err;
	struct doca_dev *dev = NULL;
	const char *dev_name = app_cfg->device_name;

	err = doca_devemu_vblk_set_datapath_on_dpa(app_cfg->datapath_on_dpa);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set datapath provider, err: %d", err);
		return err;
	}

	err = open_doca_device_with_ibdev_name((const uint8_t *)dev_name, strlen(dev_name), NULL, &dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("device %s not found", dev_name);
		return err;
	}
	err = doca_devemu_vblk_add_dev(dev);
	if (err != DOCA_SUCCESS)
		goto close_dev;

	err = doca_devemu_vblk_set_vblk_req_user_data_size(sizeof(struct vblk_io_request));
	if (err != DOCA_SUCCESS)
		goto rm_dev;

	err = vblk_ctrl_manager_set_max_seg_max(doca_dev_as_devinfo(dev), VBLK_CTRLS_MAX_SEG_MAX);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to seg seg_max on vblk ctrl manager:%s, err:%s",
			     dev_name,
			     doca_error_get_name(err));
		goto rm_dev;
	}

	err = vblk_ctrl_manager_set_max_queue_size(doca_dev_as_devinfo(dev), VBLK_CTRL_MAX_QUEUE_SIZE);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max queue size on vblk ctrl manager:%s, err:%s",
			     dev_name,
			     doca_error_get_name(err));
		goto rm_dev;
	}

	if (app_cfg->indirect_enabled) {
		uint8_t indirect_supported = 0;
		err = doca_devemu_vblk_cap_is_indir_descs_supported(doca_dev_as_devinfo(dev), &indirect_supported);
		if (err != DOCA_SUCCESS || indirect_supported == 0) {
			DOCA_LOG_ERR("no indirect feature capability on vblk manager:%s, err: %s",
				     dev_name,
				     doca_error_get_name(err));
			err = DOCA_ERROR_NOT_SUPPORTED;
			goto rm_dev;
		}
	}

	err = doca_devemu_vblk_init();
	if (err != DOCA_SUCCESS)
		goto rm_dev;

	g_vblk_dev = dev;
	*g_dev = g_vblk_dev;
	return DOCA_SUCCESS;
rm_dev:
	doca_devemu_vblk_rm_dev(dev);
close_dev:
	doca_dev_close(dev);
	return err;
}

void vblk_reset(void)
{
	/* note that teardown also does rm_dev */
	doca_devemu_vblk_teardown();
	g_vblk_dev = NULL;
}
/**
 * queues are assigned roundrobin between io contexts
 */
static inline uint16_t vblk_qid_to_io_ctx_id(struct vblk_ctrl *ctrl, const int qid)
{
	return (uint16_t)(qid % ctrl->num_io_ctx);
}

/* Scan VQ states to determine if any non-idle VQs exist.
 * Thread-safe: reads individual VQ states (each is atomic). */
static inline bool vblk_ctrl_has_intermediate_state_vqs(struct vblk_ctrl *ctrl)
{
	for (int q = 0; q < ctrl->num_queues; q++) {
		enum vblk_vq_state s = ctrl->vqs[q].state;
		if (s != VBLK_VQ_DESTROYED && s != VBLK_VQ_RUNNING)
			return true;
	}
	return false;
}

/**
 * Centralized VQ state transition with poll counter updates.
 * This is the single source of truth for state changes, preventing
 * lost doorbell on state transitions by ensuring the correct poll
 * counters are always updated atomically with state changes.
 *
 * @param ctrl    Controller instance
 * @param qid     Queue ID
 * @param new_state Target state to transition to
 */
static inline void vblk_vq_set_state(struct vblk_ctrl *ctrl, int qid, enum vblk_vq_state new_state)
{
	const enum vblk_vq_state old_state = ctrl->vqs[qid].state;
	const uint16_t io_ctx_id = ctrl->vqs[qid].io_ctx_id;

	/* Update poll counters based on state transition */
	switch (new_state) {
	case VBLK_VQ_STARTING:
		/* Queue creation requested; engine must create VQ */
		ctrl->engine_poll.pending += 1;
		break;
	case VBLK_VQ_CONFIGURED:
		/* Parked for handover — engine work paused */
		ctrl->engine_poll.pending -= 1;
		break;
	case VBLK_VQ_BINDING:
		/* VQ started; io_ctx must bind + enable */
		ctrl->io_ctxs[io_ctx_id].poll.pending += 1;
		if (old_state == VBLK_VQ_STARTING)
			ctrl->engine_poll.pending -= 1;
		break;
	case VBLK_VQ_UNBINDING:
		/* VQ stop requested; io_ctx must unbind, engine will destroy later */
		ctrl->io_ctxs[io_ctx_id].poll.pending += 1;
		ctrl->engine_poll.pending += 1;
		break;
	case VBLK_VQ_RUNNING:
		if (old_state == VBLK_VQ_BINDING)
			ctrl->io_ctxs[io_ctx_id].poll.pending -= 1;
		break;
	case VBLK_VQ_STOPPING:
		if (old_state == VBLK_VQ_UNBINDING)
			ctrl->io_ctxs[io_ctx_id].poll.pending -= 1;
		break;
	case VBLK_VQ_DESTROYED:
		ctrl->engine_poll.pending -= 1;
		break;
	}

	ctrl->vqs[qid].state = new_state;
}

static bool vblk_ctrl_queue_start(struct vblk_ctrl *ctrl, int qid)
{
	struct doca_devemu_virtio_io *io_ctx = NULL;
	uint16_t ctx_id;
	ctx_id = ctrl->vqs[qid].io_ctx_id;
	struct doca_devemu_virtio_vq *vq = doca_devemu_vblk_req_vq_as_vq(ctrl->vqs[qid].vq);
	io_ctx = doca_devemu_vblk_io_as_virtio_io(ctrl->io_ctxs[ctx_id].vq_io_ctx);

	DOCA_LOG_INFO("Binding vq: %d to io_ctx %u", qid, ctx_id);
	doca_error_t err = doca_devemu_virtio_io_bind_vq(io_ctx, vq, &ctrl->vqs[qid]);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("vq_bind failed qid=%d err=%s", qid, doca_error_get_name(err));
		return false;
	}
	DOCA_LOG_DBG("enable vq %d to io_ctx %u", qid, ctx_id);
	err = doca_devemu_virtio_vq_enable(vq);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("vq_enable failed qid=%d err=%s", qid, doca_error_get_name(err));
		(void)doca_devemu_virtio_io_unbind_vq(io_ctx, vq);
		return false;
	}

	DOCA_LOG_INFO("vq_start_done qid=%d ctx_id=%u", qid, ctx_id);
	return true;
}

/* Non-blocking VQ stop: returns DOCA_SUCCESS when VQ reaches DISABLED,
 * DOCA_ERROR_AGAIN when still in progress (caller should retry on next progress). */
static doca_error_t vblk_ctrl_queue_stop(struct vblk_ctrl *ctrl, int qid)
{
	const uint16_t ctx_id = ctrl->vqs[qid].io_ctx_id;
	struct doca_devemu_vblk_req_vq *req_vq = ctrl->vqs[qid].vq;
	struct doca_devemu_virtio_vq *vq = doca_devemu_vblk_req_vq_as_vq(req_vq);
	enum doca_devemu_virtio_vq_states vq_state;
	doca_error_t err;
	err = doca_devemu_virtio_vq_get_state(vq, &vq_state);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to get vq%d state", qid);
		return err;
	}

	if (vq_state == DOCA_DEVEMU_VIRTIO_VQ_STATE_ENABLED) {
		DOCA_LOG_DBG("disable vq %d io_ctx %u", qid, ctx_id);
		err = doca_devemu_virtio_vq_disable(vq);
		if (err == DOCA_SUCCESS) {
			DOCA_LOG_DBG("vq%d disabled immediately", qid);
			return DOCA_SUCCESS;
		}
		if (err == DOCA_ERROR_IN_PROGRESS) {
			struct doca_devemu_virtio_io *io_ctx =
				doca_devemu_vblk_io_as_virtio_io(ctrl->io_ctxs[ctx_id].vq_io_ctx);
			doca_devemu_virtio_io_flush_vq(io_ctx, vq);
			DOCA_LOG_DBG("vq%d DISABLING, flushed", qid);
			return DOCA_ERROR_AGAIN;
		}
		DOCA_LOG_ERR("vq_disable failed qid=%d err=%s", qid, doca_error_get_name(err));
		return err;
	}

	if (vq_state == DOCA_DEVEMU_VIRTIO_VQ_STATE_DISABLING) {
		DOCA_LOG_DBG("vq%d still DISABLING", qid);
		return DOCA_ERROR_AGAIN;
	}

	DOCA_LOG_DBG("vq%d DISABLED", qid);
	return DOCA_SUCCESS;
}

static void print_vq_stats(struct doca_devemu_virtio_queue_dbg_state *stats)
{
	uint16_t id, size, inflights, hw_avail_idx, driver_avail_idx, hw_used_idx, driver_used_idx;
	uint8_t enabled;

	if (doca_devemu_virtio_queue_dbg_state_get_id(stats, &id) == DOCA_SUCCESS) {
		printf("Queue ID: %u\n", id);
	}

	if (doca_devemu_virtio_queue_dbg_state_get_enabled(stats, &enabled) == DOCA_SUCCESS) {
		printf("Queue %s\n", enabled == 1 ? "enabled" : "disabled");
	}

	if (doca_devemu_virtio_queue_dbg_state_get_size(stats, &size) == DOCA_SUCCESS) {
		printf("Queue Size: %u\n", size);
	}

	if (doca_devemu_virtio_queue_dbg_state_get_inflights(stats, &inflights) == DOCA_SUCCESS) {
		printf("In-flight Requests: %u\n", inflights);
	}

	if (doca_devemu_virtio_queue_dbg_state_get_hw_avail_idx(stats, &hw_avail_idx) == DOCA_SUCCESS) {
		printf("HW Available Index: %u\n", hw_avail_idx);
	}

	if (doca_devemu_virtio_queue_dbg_state_get_driver_avail_idx(stats, &driver_avail_idx) == DOCA_SUCCESS) {
		printf("Driver Available Index: %u\n", driver_avail_idx);
	}

	if (doca_devemu_virtio_queue_dbg_state_get_hw_used_idx(stats, &hw_used_idx) == DOCA_SUCCESS) {
		printf("HW Used Index: %u\n", hw_used_idx);
	}

	if (doca_devemu_virtio_queue_dbg_state_get_driver_used_idx(stats, &driver_used_idx) == DOCA_SUCCESS) {
		printf("Driver Used Index: %u\n", driver_used_idx);
	}
}

static void vblk_ctrl_stats_list_try_create(struct vblk_ctrl *ctrl)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	doca_error_t err;

	if (ctrl == NULL || ctrl->state_list != NULL)
		return;

	/* Create stats list once: after all enabled queues are running */
	bool any_running = false;
	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		if (!ctrl->vqs[qid].cfg.queue_enable)
			continue;
		if (ctrl->vqs[qid].state != VBLK_VQ_RUNNING)
			return;
		any_running = true;
	}
	if (!any_running)
		return;

	virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
	err = doca_devemu_virtio_offload_engine_queue_dbg_state_create_list(virtio_engine,
									    &ctrl->state_list,
									    &ctrl->list_len);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vq stats list (%s)", doca_error_get_name(err));
		ctrl->state_list = NULL;
		ctrl->list_len = 0;
	}
}

static void vblk_ctrl_print_io_ctx_stats_agg(struct vblk_ctrl_io_ctx *io_ctx)
{
	struct vblk_ctrl *ctrl = io_ctx->ctrl;
	const uint16_t ctx_id = (uint16_t)(io_ctx - ctrl->io_ctxs);
	uint32_t q_count = 0, q_enabled = 0, inflights_sum = 0;

	for (uint32_t i = 0; i < ctrl->list_len; i++) {
		struct doca_devemu_virtio_queue_dbg_state *stats = ctrl->state_list[i];
		uint16_t qid, inflights;
		uint8_t enabled;

		if (doca_devemu_virtio_queue_dbg_state_get_id(stats, &qid) != DOCA_SUCCESS)
			continue;
		if (qid >= ctrl->num_queues)
			continue;
		if (ctrl->vqs[qid].io_ctx_id != ctx_id)
			continue;

		q_count++;
		if (doca_devemu_virtio_queue_dbg_state_get_enabled(stats, &enabled) == DOCA_SUCCESS && enabled)
			q_enabled++;
		if (doca_devemu_virtio_queue_dbg_state_get_inflights(stats, &inflights) == DOCA_SUCCESS)
			inflights_sum += inflights;
	}

	DOCA_LOG_INFO("io_ctx=%u queues=%u enabled=%u inflights=%u", ctx_id, q_count, q_enabled, inflights_sum);
}

static void vblk_ctrl_stats_progress(struct vblk_ctrl_io_ctx *io_ctx)
{
	struct vblk_ctrl *ctrl = io_ctx->ctrl;
	doca_error_t err;
	uint8_t is_populated;

	if (ctrl->ios_period == 0)
		return;

	/* Skip unless it's time to check or we're already tracking a populate in progress */
	if (++io_ctx->stats_ios % ctrl->ios_period != 0 && !io_ctx->stats_wip)
		return;

	if (ctrl->state_list == NULL || ctrl->list_len == 0) {
		vblk_ctrl_stats_list_try_create(ctrl);
		if (ctrl->state_list == NULL || ctrl->list_len == 0) {
			io_ctx->stats_wip = false;
			return;
		}
	}

	if (!io_ctx->stats_wip) {
		/* Try to acquire the shared populate lock atomically */
		bool expected = false;
		if (!atomic_compare_exchange_strong(&ctrl->stats_populate_wip, &expected, true))
			return; /* Another ctx is already populating */

		err = doca_devemu_virtio_queue_dbg_state_populate_list(ctrl->state_list);
		if (err == DOCA_SUCCESS || err == DOCA_ERROR_IN_PROGRESS)
			io_ctx->stats_wip = true;
		else {
			DOCA_LOG_ERR("Can't request populate vq stats (%s)", doca_error_get_name(err));
			atomic_store(&ctrl->stats_populate_wip, false);
		}
		return;
	}

	err = doca_devemu_virtio_queue_dbg_state_is_populated(ctrl->state_list, &is_populated);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("vq stats is_populated failed (%s)", doca_error_get_name(err));
		io_ctx->stats_wip = false;
		atomic_store(&ctrl->stats_populate_wip, false);
		return;
	}
	if (!is_populated)
		return;

	vblk_ctrl_print_io_ctx_stats_agg(io_ctx);
	for (uint32_t i = 0; i < ctrl->list_len; i++) {
		struct doca_devemu_virtio_queue_dbg_state *stats = ctrl->state_list[i];
		uint16_t qid;

		if (doca_devemu_virtio_queue_dbg_state_get_id(stats, &qid) != DOCA_SUCCESS)
			continue;
		if (qid >= ctrl->num_queues || ctrl->vqs[qid].io_ctx_id != (uint16_t)(io_ctx - ctrl->io_ctxs))
			continue;
		print_vq_stats(stats);
	}
	io_ctx->stats_wip = false;
	atomic_store(&ctrl->stats_populate_wip, false);
}

static void vblk_ctrl_vq_stop_destroy(struct doca_devemu_vblk_req_vq *vq)
{
	struct doca_devemu_virtio_vq *virtio_vq = doca_devemu_vblk_req_vq_as_vq(vq);
	doca_error_t err;

	err = doca_devemu_virtio_vq_stop(virtio_vq);
	if (err != DOCA_SUCCESS && err != DOCA_ERROR_BAD_STATE)
		DOCA_LOG_ERR("vq_stop failed vq=%p err=%s, forcing destroy", (void *)vq, doca_error_get_name(err));

	err = doca_devemu_vblk_req_vq_destroy(vq);
	if (err != DOCA_SUCCESS)
		DOCA_LOG_ERR("vq_destroy failed vq=%p err=%s", (void *)vq, doca_error_get_name(err));
}

static void vblk_ctrl_cancel_pending_queue_start(struct vblk_ctrl *ctrl)
{
	if (!ctrl->pending_queue_start)
		return;
	ctrl->pending_queue_start = false;
	ctrl->engine_poll.pending -= 1;
}

static void vblk_ctrl_start_ready_vqs(struct vblk_ctrl *ctrl)
{
	for (uint16_t q = 0; q < ctrl->num_queues; q++) {
		if (!ctrl->vqs[q].cfg.queue_enable)
			continue;
		if (ctrl->vqs[q].state != VBLK_VQ_DESTROYED || ctrl->vqs[q].vq != NULL)
			continue;
		vblk_ctrl_ipc_queue_start(ctrl, q, &ctrl->vqs[q].cfg);
	}
}

struct vblk_poll_state *vblk_ctrl_engine_poll(struct vblk_ctrl *ctrl)
{
	return &ctrl->engine_poll;
}

void vblk_ctrl_apply_state(struct vblk_ctrl *ctrl, const struct vblk_lu_device_cfg *state)
{
	if (state->vq_cfg.num_queues > VBLK_PCI_VIRTIO_MAX_QUEUES)
		return;
	DOCA_LOG_INFO("apply_state: device_status=0x%x num_queues=%u", state->device_status, state->vq_cfg.num_queues);

	if (state->device_status == 0)
		vblk_ctrl_ipc_device_reset(ctrl);

	if (state->device_status & VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER_OK) {
		for (uint16_t q = 0; q < state->vq_cfg.num_queues && q < ctrl->num_queues; q++) {
			if (state->vq_cfg.vqs[q].queue_enable)
				ctrl->vqs[q].cfg = state->vq_cfg.vqs[q];
		}

		if (vblk_ctrl_has_intermediate_state_vqs(ctrl)) {
			DOCA_LOG_INFO("EMU: DRIVER_OK but VQs still active, deferring");
			if (!ctrl->pending_queue_start) {
				ctrl->pending_queue_start = true;
				ctrl->engine_poll.pending += 1;
			}
		} else {
			vblk_ctrl_start_ready_vqs(ctrl);
		}
	}
}

void vblk_ctrl_offload_engine_progress(struct vblk_ctrl *ctrl)
{
	if (ctrl == NULL)
		return;

	/* Deferred start: DRIVER_OK arrived during reset, now reset is done */
	if (ctrl->pending_queue_start && !vblk_ctrl_has_intermediate_state_vqs(ctrl)) {
		vblk_ctrl_cancel_pending_queue_start(ctrl);
		vblk_ctrl_start_ready_vqs(ctrl);
	}

	doca_error_t err;
	struct doca_devemu_vblk_req_vq *vq;
	struct doca_devemu_virtio_vq *virtio_vq;
	bool did_work = false;

	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		const enum vblk_vq_state state = ctrl->vqs[qid].state;
		switch (state) {
		case VBLK_VQ_RUNNING:
		case VBLK_VQ_BINDING:
		case VBLK_VQ_UNBINDING:
			continue;
		case VBLK_VQ_DESTROYED:
			continue;
		case VBLK_VQ_STARTING:
			DOCA_LOG_DBG("VQ%d starting: size=%u msix=%u desc=0x%lx drv=0x%lx dev=0x%lx",
				     qid,
				     ctrl->vqs[qid].cfg.queue_size,
				     ctrl->vqs[qid].cfg.queue_msix_vector,
				     ctrl->vqs[qid].cfg.queue_desc,
				     ctrl->vqs[qid].cfg.queue_driver,
				     ctrl->vqs[qid].cfg.queue_device);
			err = doca_devemu_vblk_req_vq_create(ctrl->vq_engine, &vq);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create VQ%d (%s)", qid, doca_error_get_name(err));
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_DESTROYED);
				continue;
			}
			virtio_vq = doca_devemu_vblk_req_vq_as_vq(vq);
			err = doca_devemu_virtio_vq_set_conf(virtio_vq,
							     qid,
							     ctrl->vqs[qid].cfg.queue_size,
							     ctrl->vqs[qid].cfg.queue_msix_vector,
							     ctrl->vqs[qid].cfg.queue_desc,
							     ctrl->vqs[qid].cfg.queue_driver,
							     ctrl->vqs[qid].cfg.queue_device);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("vq_set_conf failed qid=%d err=%s", qid, doca_error_get_name(err));
				(void)doca_devemu_vblk_req_vq_destroy(vq);
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_DESTROYED);
				continue;
			}
			ctrl->vqs[qid].vq = vq;
			err = doca_devemu_virtio_vq_start(virtio_vq);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("vq_start failed qid=%d err=%s", qid, doca_error_get_name(err));
				(void)doca_devemu_vblk_req_vq_destroy(vq);
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_DESTROYED);
				ctrl->vqs[qid].vq = NULL;
				continue;
			}
			if (ctrl->handover_dst) {
				DOCA_LOG_INFO("VQ%d started (parked for handover, DB unmapped)", qid);
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_CONFIGURED);
				return;
			}
			DOCA_LOG_DBG("vq_started qid=%d req_vq=%p", qid, (void *)vq);
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_BINDING);
			did_work = true;
			goto out;
		case VBLK_VQ_CONFIGURED:
			continue; /* parked — handover will advance */
		case VBLK_VQ_STOPPING:
			vq = ctrl->vqs[qid].vq;
			if (vq == NULL) {
				DOCA_LOG_DBG("vq_stop qid=%d skipped (no VQ)", qid);
			} else {
				vblk_ctrl_vq_stop_destroy(vq);
				ctrl->vqs[qid].vq = NULL;
			}
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_DESTROYED);
			DOCA_LOG_DBG("vq_destroy_done qid=%d", qid);
			did_work = true;
			goto out;
		}
	}

out:
	if (!did_work && ctrl->engine_poll.pending > 0 && !ctrl->pending_queue_start &&
	    !vblk_ctrl_has_intermediate_state_vqs(ctrl))
		ctrl->engine_poll.pending = 0;
}

static inline doca_error_t vblk_io_ctx_memcpy(struct vblk_ctrl_io_ctx *io_ctx, struct vblk_io_request *req)
{
	union doca_data udata;
	doca_error_t err;
	struct doca_dma_task_memcpy *task;

	udata.ptr = req;
	if (req->type == VBLK_T_IN || req->type == VBLK_T_GET_ID)
		err = doca_dma_task_memcpy_alloc_init(io_ctx->dma_ctx,
						      req->dpu_buf,
						      doca_devemu_vblk_req_get_data(req->doca_req),
						      udata,
						      &task);
	else
		err = doca_dma_task_memcpy_alloc_init(io_ctx->dma_ctx,
						      doca_devemu_vblk_req_get_data(req->doca_req),
						      req->dpu_buf,
						      udata,
						      &task);

	if (err != DOCA_SUCCESS)
		return err;

	return doca_task_submit(doca_dma_task_memcpy_as_task(task));
}

static doca_error_t vblk_cmd_get_id(struct vblk_ctrl_io_ctx *io_ctx, struct vblk_io_request *req)
{
	uint32_t req_len = doca_devemu_vblk_req_get_data_len(req->doca_req);
	doca_error_t err;
	void *buf_ptr;

	DOCA_LOG_INFO("GET_ID, data len is %d", req_len);

	/* SPEC: The device ID string is a NUL-padded ASCII string up to 20 bytes long. If the string is 20 bytes long
	 * then there is no NUL terminator. */
	if (req_len < VBLK_GET_ID_SIZE) {
		DOCA_LOG_WARN("request len %d is too short to fetch device id", req_len);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* here should be logic to either use id of the attached bdev or use
	 * id set on the controller level
	 */
	err = vblk_mpool_set_buf_get(vblk_io_ctx_mpool(io_ctx), VBLK_GET_ID_SIZE, &req->dpu_buf);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to request mpool buffer of size %d, error: %s",
			     VBLK_GET_ID_SIZE,
			     doca_error_get_name(err));
		return err;
	}

	(void)doca_buf_get_data(req->dpu_buf, &buf_ptr);
	memcpy(buf_ptr, io_ctx->ctrl->vblk_dev_id, VBLK_GET_ID_SIZE);
	(void)doca_buf_set_data_len(req->dpu_buf, VBLK_GET_ID_SIZE);
	doca_buf_reset_data_len(doca_devemu_vblk_req_get_data(req->doca_req));

	/* copy get id to the src_buf */
	err = vblk_io_ctx_memcpy(io_ctx, req);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to start memcpy");
		vblk_mpool_set_buf_put(req->dpu_buf);
		return err;
	}

	return DOCA_ERROR_IN_PROGRESS;
}

static doca_error_t vblk_cmd_read(struct vblk_ctrl_io_ctx *io_ctx, struct vblk_io_request *req)
{
	doca_error_t err;
	struct doca_buf *buf;
	uint32_t req_len = doca_devemu_vblk_req_get_data_len(req->doca_req);

	err = vblk_mpool_set_buf_get(vblk_io_ctx_mpool(io_ctx), req_len, &req->dpu_buf);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to request mpool buffer of size %d, error: %s", req_len, doca_error_get_name(err));
		return err;
	}

	/**
	 * start async bdev read into req->dpu_buf here
	 * code below should be executed on the bdev read completion
	 */
	(void)doca_buf_set_data_len(req->dpu_buf, req_len);
	buf = doca_devemu_vblk_req_get_data(req->doca_req);
	do {
		doca_buf_reset_data_len(buf);
		doca_buf_get_next_in_list(buf, &buf);
	} while (buf != NULL);

	err = vblk_io_ctx_memcpy(io_ctx, req);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to start memcpy");
		vblk_mpool_set_buf_put(req->dpu_buf);
		return err;
	}

	return DOCA_ERROR_IN_PROGRESS;
}

static doca_error_t vblk_cmd_write(struct vblk_ctrl_io_ctx *io_ctx, struct vblk_io_request *req)
{
	doca_error_t err;
	uint32_t req_len = doca_devemu_vblk_req_get_data_len(req->doca_req);

	err = vblk_mpool_set_buf_get(vblk_io_ctx_mpool(io_ctx), req_len, &req->dpu_buf);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to request mpool buffer of size %d, error: %s", req_len, doca_error_get_name(err));
		return err;
	}

	err = vblk_io_ctx_memcpy(io_ctx, req);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to start memcpy");
		vblk_mpool_set_buf_put(req->dpu_buf);
		return err;
	}

	return DOCA_ERROR_IN_PROGRESS;
}

static inline void vblk_io_request_init(struct vblk_io_request *req,
					struct doca_devemu_vblk_req *doca_req,
					const uint32_t type,
					const uint64_t sector)
{
	req->type = type;
	req->lba = sector;
	req->doca_req = doca_req;
}

static void vblk_io_handler(struct doca_devemu_vblk_req *req, uint32_t type, uint64_t sector, void *req_user_data)
{
	union doca_data user_data;
	struct vblk_ctrl_io_ctx *io_ctx;
	doca_error_t err;
	struct vblk_io_request *vblk_req = req_user_data;

	err = doca_ctx_get_user_data(vblk_io_as_ctx(doca_devemu_vblk_req_get_vblk_io(req)), &user_data);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from vblk_io");
		goto complete_req_err;
	}

	io_ctx = user_data.ptr;

	if (io_ctx->ctrl->ios_period > 0) {
		DOCA_LOG_TRC("get user data : %p from vblk_req set in vq bind with io ctx",
			     doca_devemu_vblk_req_get_vq_user_data(req));
		vblk_ctrl_stats_progress(io_ctx);
	}

	vblk_io_request_init(vblk_req, req, type, sector);

	switch (type) {
	case VBLK_T_IN:
		err = vblk_cmd_read(io_ctx, vblk_req);
		break;
	case VBLK_T_OUT:
		err = vblk_cmd_write(io_ctx, vblk_req);
		break;
	case VBLK_T_GET_ID:
		err = vblk_cmd_get_id(io_ctx, vblk_req);
		break;
	default:
		DOCA_LOG_WARN("unsupported io request received type %d sector 0x%lx udata %p nbufs %d data_len %d",
			      type,
			      sector,
			      req_user_data,
			      doca_devemu_vblk_req_get_data_list_len(req),
			      doca_devemu_vblk_req_get_data_len(req));
		doca_devemu_vblk_req_complete(req, VBLK_S_UNSUPP, 0);
		return;
	}

	/* likely
	 * NOTE: all command implementations above are async and return
	 * IN_PROGRESS on successful completion or error status
	 */
	if (err == DOCA_ERROR_IN_PROGRESS)
		return;

complete_req_err:
	doca_devemu_vblk_req_complete(req, VBLK_S_IOERR, 0);
}

static void vblk_ctrl_dma_done(struct doca_dma_task_memcpy *dma_task,
			       union doca_data task_user_data,
			       union doca_data ctx_user_data)
{
	(void)ctx_user_data;

	struct vblk_io_request *req = task_user_data.ptr;
	uint32_t out_len = doca_devemu_vblk_req_get_data_len(req->doca_req);

	(void)ctx_user_data;

	DOCA_LOG_TRC("DMA completed for req %p type %d, out len %d", req, req->type, out_len);

	doca_task_free(doca_dma_task_memcpy_as_task(dma_task));

	if (req->type == VBLK_T_IN || req->type == VBLK_T_GET_ID) {
		vblk_mpool_set_buf_put(req->dpu_buf);
		doca_devemu_vblk_req_complete(req->doca_req, VBLK_S_OK, out_len);
	} else {
		/* here should start async write to the bdev, upon
		 * completion free dpu_buf and complete request */
		vblk_mpool_set_buf_put(req->dpu_buf);
		doca_devemu_vblk_req_complete(req->doca_req, VBLK_S_OK, 0);
	}
}

static void vblk_ctrl_dma_error(struct doca_dma_task_memcpy *dma_task,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	(void)ctx_user_data;

	struct vblk_io_request *req = task_user_data.ptr;
	struct doca_task *task = doca_dma_task_memcpy_as_task(dma_task);
	doca_error_t err = doca_task_get_status(task);

	(void)ctx_user_data;

	DOCA_LOG_ERR("DMA ERROR %d(%s:%s) for req %p type %d list_len %d len %d",
		     err,
		     doca_error_get_name(err),
		     doca_error_get_descr(err),
		     req,
		     req->type,
		     doca_devemu_vblk_req_get_data_list_len(req->doca_req),
		     doca_devemu_vblk_req_get_data_len(req->doca_req));
	doca_task_free(task);
	vblk_mpool_set_buf_put(req->dpu_buf);
	doca_devemu_vblk_req_complete(req->doca_req, VBLK_S_IOERR, 0);
}

static doca_error_t vblk_ctrl_io_ctx_init(struct vblk_ctrl *ctrl, struct vblk_ctrl_io_ctx *io_ctx)
{
	doca_error_t err;
	union doca_data ctx_udata = {.ptr = io_ctx};
	struct doca_dev *dev = g_vblk_dev;

	struct vblk_mpool_attr mpool_attr[] = {
		{.buf_size = 4096, .num_bufs = 1024, .devs = &dev, .num_devs = 1},  // 4KB
		{.buf_size = 8192, .num_bufs = 1024, .devs = &dev, .num_devs = 1},  // 8KB
		{.buf_size = 65536, .num_bufs = 512, .devs = &dev, .num_devs = 1},  // 64KB
		{.buf_size = 262144, .num_bufs = 512, .devs = &dev, .num_devs = 1}, // 256KB
		{.buf_size = 524288, .num_bufs = 256, .devs = &dev, .num_devs = 1}, // 512KB
	};
	struct vblk_mpool_set_attr mpool_set_attr = {.mpools = mpool_attr, .num_pools = ARRAY_SIZE(mpool_attr)};

	io_ctx->mpool = vblk_mpool_set_create(&mpool_set_attr);
	if (io_ctx->mpool == NULL) {
		DOCA_LOG_ERR("Failed to create mpool set");
		return DOCA_ERROR_NO_MEMORY;
	}
	err = doca_devemu_vblk_io_create_from_offload_engine(ctrl->vq_engine, &io_ctx->vq_io_ctx);
	if (err != DOCA_SUCCESS)
		goto destroy_mpool;

	err = doca_ctx_set_user_data(vblk_io_as_ctx(io_ctx->vq_io_ctx), ctx_udata);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_io_ctx;

	err = doca_dma_create(g_vblk_dev, &io_ctx->dma_ctx);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_io_ctx;

	err = doca_ctx_set_user_data(doca_dma_as_ctx(io_ctx->dma_ctx), ctx_udata);
	if (err != DOCA_SUCCESS)
		goto destroy_dma_ctx;

	err = doca_dma_task_memcpy_set_conf(io_ctx->dma_ctx,
					    vblk_ctrl_dma_done,
					    vblk_ctrl_dma_error,
					    VBLK_PCI_DEV_MAX_QUEUES * VBLK_CTRL_MAX_QUEUE_SIZE);
	if (err != DOCA_SUCCESS)
		goto destroy_dma_ctx;

	/* setting this to one will disable umr support for chained doca bufs */
	err = doca_dma_set_ordered_completions(io_ctx->dma_ctx, 0);
	if (err != DOCA_SUCCESS)
		goto destroy_dma_ctx;

	err = doca_devemu_vblk_io_event_vblk_req_register(io_ctx->vq_io_ctx, vblk_io_handler);
	if (err != DOCA_SUCCESS)
		goto destroy_dma_ctx;

	io_ctx->ctrl = ctrl;
	return DOCA_SUCCESS;

destroy_dma_ctx:
	doca_dma_destroy(io_ctx->dma_ctx);
destroy_vq_io_ctx:
	doca_devemu_vblk_io_destroy(io_ctx->vq_io_ctx);
destroy_mpool:
	vblk_mpool_set_destroy(io_ctx->mpool);
	return err;
}

static void vblk_ctrl_io_ctx_reset(struct vblk_ctrl_io_ctx *io_ctx)
{
	vblk_mpool_set_destroy(io_ctx->mpool);
	doca_dma_destroy(io_ctx->dma_ctx);
	doca_devemu_vblk_io_destroy(io_ctx->vq_io_ctx);
}
static void vblk_ctrl_io_ctxs_reset(struct vblk_ctrl *ctrl, uint8_t ctxs_num)
{
	while (ctxs_num-- > 0)
		vblk_ctrl_io_ctx_reset(&ctrl->io_ctxs[ctxs_num]);
}

static void vblk_ctrl_vqs_destroy(struct vblk_ctrl *ctrl)
{
	if (ctrl->vqs == NULL)
		return;
	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		struct doca_devemu_vblk_req_vq *vq = ctrl->vqs[qid].vq;
		if (vq == NULL)
			continue;
		vblk_ctrl_vq_stop_destroy(vq);
		ctrl->vqs[qid].vq = NULL;
	}
}
doca_error_t vblk_ctrl_set_recovery_export(struct vblk_ctrl *ctrl, const void *desc, size_t len)
{
	return vblk_export_desc_store((void **)&ctrl->recovery_export_desc, &ctrl->recovery_export_desc_len, desc, len);
}

void vblk_ctrl_release_recovery_export(struct vblk_ctrl *ctrl)
{
	/* Free the app-owned copy; the SHM file itself is NOT unlinked here
	 * because SRC recovery skips re-export.  TLP unlinks the old SHM on
	 * graceful exit or when a subsequent live-update export replaces it.
	 */
	vblk_export_desc_free((void **)&ctrl->recovery_export_desc, &ctrl->recovery_export_desc_len);
}

doca_error_t vblk_ctrl_init(struct vblk_ctrl *ctrl, const struct vblk_ctrl_attrs *attr, struct doca_devemu_pci_ep *ep)
{
	doca_error_t err;
	struct doca_devemu_virtio_offload_engine *virtio_engine;

	if (ep == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	ctrl->num_queues = attr->num_queues;
	ctrl->num_io_ctx = attr->num_io_ctx;

	ctrl->vqs = calloc(ctrl->num_queues, sizeof(*ctrl->vqs));
	if (!ctrl->vqs)
		return DOCA_ERROR_NO_MEMORY;

	for (int qid = 0; qid < ctrl->num_queues; qid++)
		ctrl->vqs[qid].io_ctx_id = vblk_qid_to_io_ctx_id(ctrl, qid);

	if (ctrl->recovery_export_desc != NULL && ctrl->recovery_export_desc_len > 0) {
		err = doca_devemu_vblk_offload_engine_create_from_export(ctrl->recovery_export_desc,
									 ctrl->recovery_export_desc_len,
									 ep,
									 &ctrl->vq_engine);
		vblk_ctrl_release_recovery_export(ctrl);
	} else {
		err = doca_devemu_vblk_offload_engine_create(ep, &ctrl->vq_engine);
		if (err == DOCA_SUCCESS)
			err = doca_devemu_vblk_offload_engine_set_seg_max(ctrl->vq_engine, attr->seg_max);
		if (err == DOCA_SUCCESS) {
			struct doca_devemu_virtio_offload_engine *voe =
				doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
			err = doca_devemu_virtio_offload_engine_set_indir_descs_enabled(voe, attr->indirect_enabled);
			if (err == DOCA_SUCCESS)
				err = doca_devemu_virtio_offload_engine_set_num_queues(voe, ctrl->num_queues);
		}
	}
	if (err != DOCA_SUCCESS)
		goto destroy_vqs;

	virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
	err = doca_devemu_virtio_offload_engine_start(virtio_engine);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_engine;

	strncpy(ctrl->vblk_dev_id, VBLK_CTRL_DEFAULT_ID, sizeof(ctrl->vblk_dev_id));
	return DOCA_SUCCESS;

destroy_vq_engine:
	doca_devemu_vblk_offload_engine_destroy(ctrl->vq_engine);
	ctrl->vq_engine = NULL;
destroy_vqs:
	free(ctrl->vqs);
	ctrl->vqs = NULL;
	return err;
}

doca_error_t vblk_ctrl_enable(struct vblk_ctrl *ctrl)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;

	if (ctrl == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
	doca_error_t err = doca_devemu_virtio_offload_engine_enable(virtio_engine);
	if (err != DOCA_SUCCESS)
		return err;
	return DOCA_SUCCESS;
}

static doca_error_t vblk_wait_ctx_state(struct doca_pe *pe,
					struct doca_ctx *ctx,
					enum doca_ctx_states desired,
					enum doca_ctx_states allowed,
					const char *name,
					const char *action,
					uint32_t max_iters)
{
	enum doca_ctx_states state;
	uint32_t n = 0;

	if (ctx == NULL || pe == NULL)
		return DOCA_SUCCESS;

	while (1) {
		(void)doca_pe_progress(pe);
		(void)doca_ctx_get_state(ctx, &state);
		if (state == desired)
			return DOCA_SUCCESS;

		if (state != allowed) {
			DOCA_LOG_ERR("%s ctx %p failed to %s (state=%d)", name, ctx, action, state);
			return DOCA_ERROR_BAD_STATE;
		}

		if (++n > max_iters) {
			DOCA_LOG_ERR("%s ctx %p %s timeout after %u iters", name, ctx, action, n);
			return DOCA_ERROR_TIME_OUT;
		}
	}
}

static doca_error_t vblk_wait_ctx_running(struct doca_pe *pe, struct doca_ctx *ctx, const char *name, int max_iters)
{
	return vblk_wait_ctx_state(pe, ctx, DOCA_CTX_STATE_RUNNING, DOCA_CTX_STATE_STARTING, name, "start", max_iters);
}

doca_error_t vblk_wait_ctx_idle(struct doca_pe *pe, struct doca_ctx *ctx, const char *name, int max_iters)
{
	return vblk_wait_ctx_state(pe, ctx, DOCA_CTX_STATE_IDLE, DOCA_CTX_STATE_STOPPING, name, "stop", max_iters);
}

doca_error_t vblk_ctrl_init_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id, struct doca_pe *pe)
{
	doca_error_t err;
	struct doca_ctx *io_ctx;

	if (ctrl == NULL || pe == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (ctx_id >= ctrl->num_io_ctx)
		return DOCA_ERROR_INVALID_VALUE;

	/* Must be invoked on the managing thread */
	err = vblk_ctrl_io_ctx_init(ctrl, &ctrl->io_ctxs[ctx_id]);
	if (err != DOCA_SUCCESS)
		return err;

	err = vblk_ctrl_connect_io_ctx(ctrl, ctx_id, pe);
	if (err != DOCA_SUCCESS)
		return err;

	/* Start per-IO DMA ctx */
	err = doca_ctx_start(vblk_ctrl_dma_ctx(ctrl, ctx_id));
	if (err != DOCA_SUCCESS)
		return err;

	/* Start IO ctx (may be async) */
	io_ctx = vblk_ctrl_io_ctx(ctrl, ctx_id);
	err = doca_ctx_start(io_ctx);
	if (err != DOCA_SUCCESS && err != DOCA_ERROR_IN_PROGRESS)
		return err;

	/* Progress until IO ctx reaches RUNNING */
	err = vblk_wait_ctx_running(pe, io_ctx, "io", VBLK_IO_CTX_MAX_WAIT_ITERATIONS_RUNNING);
	if (err != DOCA_SUCCESS)
		return err;

	return DOCA_SUCCESS;
}

doca_error_t vblk_ctrl_shutdown_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id)
{
	struct vblk_ctrl_io_ctx *io_ctx;
	struct doca_pe *pe;

	io_ctx = &ctrl->io_ctxs[ctx_id];
	pe = io_ctx->pe;

	/* Stop RUNNING queues mapped to this IO ctx.
	 * VQs in intermediate states (BINDING, CONFIGURED) are only possible
	 * if the TLP app died mid-setup — assumed to be a consistent state. */
	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		doca_error_t err;
		enum vblk_vq_state st = ctrl->vqs[qid].state;
		if (st != VBLK_VQ_RUNNING)
			continue;
		if (ctrl->vqs[qid].io_ctx_id != ctx_id)
			continue;
		while ((err = vblk_ctrl_queue_stop(ctrl, qid)) == DOCA_ERROR_AGAIN) {
			doca_pe_progress(pe);
		};

		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("failed to stop vq qid=%d, err=%s", qid, doca_error_get_name(err));
		} else {
			struct doca_devemu_virtio_io *ioctx = doca_devemu_vblk_io_as_virtio_io(io_ctx->vq_io_ctx);
			struct doca_devemu_virtio_vq *vq = doca_devemu_vblk_req_vq_as_vq(ctrl->vqs[qid].vq);

			err = doca_devemu_virtio_io_unbind_vq(ioctx, vq);

			if (err == DOCA_SUCCESS) {
				DOCA_LOG_DBG("vq_stop_ready qid=%d ctx_id=%u", qid, ctx_id);
			} else if (err == DOCA_ERROR_BAD_STATE) {
				DOCA_LOG_DBG("vq qid=%d already unbind", qid);
			} else {
				DOCA_LOG_ERR("vq_unbind failed qid=%d err=%s", qid, doca_error_get_name(err));
			}
		}
	}

	/* Stop contexts (PE thread is expected to keep progressing) */
	(void)doca_ctx_stop(doca_dma_as_ctx(io_ctx->dma_ctx));
	(void)doca_ctx_stop(vblk_io_as_ctx(io_ctx->vq_io_ctx));

	/* Wait for contexts to become IDLE */
	(void)vblk_wait_ctx_idle(pe, doca_dma_as_ctx(io_ctx->dma_ctx), "dma", VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE);
	(void)vblk_wait_ctx_idle(pe, vblk_io_as_ctx(io_ctx->vq_io_ctx), "io", VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE);

	return DOCA_SUCCESS;
}

void vblk_ctrl_cleanup(struct vblk_ctrl *ctrl)
{
	vblk_ctrl_vqs_destroy(ctrl);
	vblk_ctrl_io_ctxs_reset(ctrl, ctrl->num_io_ctx);

	if (ctrl->vq_engine != NULL) {
		struct doca_devemu_virtio_offload_engine *virtio_engine =
			doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
		enum doca_devemu_virtio_offload_engine_states state;
		if (doca_devemu_virtio_offload_engine_get_state(virtio_engine, &state) == DOCA_SUCCESS &&
		    state == DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_ENABLED)
			(void)doca_devemu_virtio_offload_engine_disable(virtio_engine);
		(void)doca_devemu_virtio_offload_engine_stop(virtio_engine);
		(void)doca_devemu_vblk_offload_engine_destroy(ctrl->vq_engine);
		ctrl->vq_engine = NULL;
	}

	free(ctrl->vqs);
	ctrl->vqs = NULL;
}

struct vblk_poll_state *vblk_ctrl_io_ctx_poll(struct vblk_ctrl *ctrl, uint8_t ctx_id)
{
	return &ctrl->io_ctxs[ctx_id].poll;
}

void vblk_ctrl_io_context_progress(struct vblk_ctrl *vblk_ctrl, uint8_t core_id)
{
	doca_error_t err;
	for (int qid = 0; qid < vblk_ctrl->num_queues; qid++) {
		if (vblk_ctrl->vqs[qid].io_ctx_id != core_id)
			continue;
		const enum vblk_vq_state state = vblk_ctrl->vqs[qid].state;
		switch (state) {
		case VBLK_VQ_BINDING:
			if (vblk_ctrl_queue_start(vblk_ctrl, qid))
				vblk_vq_set_state(vblk_ctrl, qid, VBLK_VQ_RUNNING);
			else
				vblk_vq_set_state(vblk_ctrl, qid, VBLK_VQ_STOPPING);
			break;
		case VBLK_VQ_RUNNING:
			break;
		case VBLK_VQ_UNBINDING:
			err = vblk_ctrl_queue_stop(vblk_ctrl, qid);
			if (err == DOCA_SUCCESS) {
				struct doca_devemu_virtio_io *io_ctx =
					doca_devemu_vblk_io_as_virtio_io(vblk_ctrl->io_ctxs[core_id].vq_io_ctx);
				struct doca_devemu_virtio_vq *vq =
					doca_devemu_vblk_req_vq_as_vq(vblk_ctrl->vqs[qid].vq);

				doca_devemu_virtio_io_flush_vq(io_ctx, vq);

				DOCA_LOG_DBG("unbinding vq %d io_ctx %u", qid, core_id);
				err = doca_devemu_virtio_io_unbind_vq(io_ctx, vq);
				if (err != DOCA_SUCCESS && err != DOCA_ERROR_BAD_STATE)
					DOCA_LOG_ERR("vq_unbind failed qid=%d err=%s", qid, doca_error_get_name(err));

				DOCA_LOG_DBG("vq_stop_ready qid=%d ctx_id=%u", qid, core_id);
				vblk_vq_set_state(vblk_ctrl, qid, VBLK_VQ_STOPPING);
			}
			break;
		default:
			break;
		}
	}
}

doca_error_t vblk_ctrl_ipc_queue_start(struct vblk_ctrl *ctrl, uint16_t qid, const struct vblk_pci_virtq_pci_cfg *cfg)
{
	if (qid >= ctrl->num_queues || ctrl->vqs[qid].state != VBLK_VQ_DESTROYED)
		return DOCA_ERROR_BAD_STATE;
	ctrl->vqs[qid].cfg = *cfg;
	vblk_vq_set_state(ctrl, qid, VBLK_VQ_STARTING);
	DOCA_LOG_INFO("IPC: queue_start qid=%u size=%u msix=%u", qid, cfg->queue_size, cfg->queue_msix_vector);
	return DOCA_SUCCESS;
}

/*
 * Resume parked VQs after SRC releases doorbells.  VQs are already
 * started with DB_UNMAPPED -- advance to BINDING so IO threads do
 * bind + enable (which remaps the doorbell to DB_MAPPED).
 *
 * bind_vq is a pure software operation (links VQ to IO context) with
 * no conceptual reason to be inside the LU window, but it is fast
 * enough (~us) to remain here rather than adding the complexity of
 * a pre-bind state that must run on the IO threads before BEGIN_ACK.
 */
void vblk_ctrl_resume_configured_vqs(struct vblk_ctrl *ctrl)
{
	ctrl->handover_dst = false;
	for (uint16_t q = 0; q < ctrl->num_queues; q++) {
		if (ctrl->vqs[q].state == VBLK_VQ_CONFIGURED) {
			DOCA_LOG_INFO("Resuming VQ%u from CONFIGURED to BINDING (DPA ready, DB unmapped)", q);
			vblk_vq_set_state(ctrl, q, VBLK_VQ_BINDING);
		}
	}
}

doca_error_t vblk_ctrl_ipc_device_reset(struct vblk_ctrl *ctrl)
{
	DOCA_LOG_INFO("IPC: device_reset - stopping all queues");

	vblk_ctrl_cancel_pending_queue_start(ctrl);

	/* Stats cleanup */
	if (ctrl->state_list) {
		(void)doca_devemu_virtio_queue_dbg_state_destroy_list(ctrl->state_list);
		ctrl->state_list = NULL;
		ctrl->list_len = 0;
		atomic_store(&ctrl->stats_populate_wip, false);
	}
	for (uint16_t ctx_id = 0; ctx_id < ctrl->num_io_ctx; ctx_id++) {
		ctrl->io_ctxs[ctx_id].stats_ios = 0;
		ctrl->io_ctxs[ctx_id].stats_wip = false;
	}

	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		memset(&ctrl->vqs[qid].cfg, 0, sizeof(ctrl->vqs[qid].cfg));
		const enum vblk_vq_state state = ctrl->vqs[qid].state;
		switch (state) {
		case VBLK_VQ_DESTROYED:
		case VBLK_VQ_UNBINDING:
		case VBLK_VQ_STOPPING:
			continue;
		case VBLK_VQ_CONFIGURED:
		case VBLK_VQ_RUNNING:
		case VBLK_VQ_BINDING:
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_UNBINDING);
			break;
		case VBLK_VQ_STARTING:
			/* VQ queued but not yet processed by offload_engine_progress
			 * — vq handle is still NULL.  STOPPING handler has a NULL
			 * guard that skips straight to DESTROYED for this case. */
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_STOPPING);
			break;
		}
	}
	return DOCA_SUCCESS;
}
