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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dma.h>

#include <doca_devemu_pci_tlp.h>
#include <doca_devemu_virtio.h>
#include <doca_devemu_virtio_tlp.h>
#include <doca_devemu_vblk_io.h>
#include <doca_devemu_vblk_type.h>

#include <samples/common.h>
#include "vblk_utils.h"
#include "vblk_mpool.h"
#include "vblk_pci.h"
#include "vblk_ctrl.h"

DOCA_LOG_REGISTER(VBLK_CTRL);

static struct doca_dev *g_vblk_dev;

#define VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE (1000000)
#define VBLK_IO_CTX_MAX_WAIT_ITERATIONS_RUNNING (1000000)
#define MAX_VQ_PER_CYCLE 8

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

static inline struct doca_pe *vblk_ctrl_io_pe(struct vblk_ctrl *ctrl, const int ctx_id)
{
	return ctrl->io_ctxs[ctx_id].pe;
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

doca_error_t vblk_ctrl_connect_io_ctx(struct vblk_ctrl *ctrl, const int ctx_id, struct doca_pe *pe)
{
	doca_error_t err;

	if (ctx_id < 0 || ctx_id >= ctrl->num_io_ctx)
		return DOCA_ERROR_INVALID_VALUE;

	err = doca_pe_connect_ctx(pe, vblk_ctrl_io_ctx(ctrl, ctx_id));
	if (err != DOCA_SUCCESS)
		return err;

	err = doca_pe_create(&ctrl->io_ctxs[ctx_id].dma_pe);
	if (err != DOCA_SUCCESS)
		return err;

	err = doca_pe_connect_ctx(ctrl->io_ctxs[ctx_id].dma_pe, vblk_ctrl_dma_ctx(ctrl, ctx_id));
	if (err != DOCA_SUCCESS)
		goto err_destroy_dma_pe;

	ctrl->io_ctxs[ctx_id].pe = pe;
	return DOCA_SUCCESS;

err_destroy_dma_pe:
	doca_pe_destroy(ctrl->io_ctxs[ctx_id].dma_pe);
	ctrl->io_ctxs[ctx_id].dma_pe = NULL;
	return err;
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

doca_error_t vblk_init(const char *device_name, bool indirect_enabled, struct doca_dev **g_dev)
{
	doca_error_t err;
	struct doca_dev *dev = NULL;

	err = open_doca_device_with_ibdev_name((const uint8_t *)device_name, strlen(device_name), NULL, &dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("device %s not found", device_name);
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
		DOCA_LOG_ERR("Failed to set seg_max on vblk ctrl manager:%s, err:%s",
			     device_name,
			     doca_error_get_name(err));
		goto rm_dev;
	}

	err = vblk_ctrl_manager_set_max_queue_size(doca_dev_as_devinfo(dev), VBLK_CTRL_MAX_QUEUE_SIZE);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max queue size on vblk ctrl manager:%s, err:%s",
			     device_name,
			     doca_error_get_name(err));
		goto rm_dev;
	}

	if (indirect_enabled) {
		uint8_t indirect_supported = 0;
		err = doca_devemu_vblk_cap_is_indir_descs_supported(doca_dev_as_devinfo(dev), &indirect_supported);
		if (err != DOCA_SUCCESS || indirect_supported == 0) {
			DOCA_LOG_ERR("no indirect feature capability on vblk manager:%s, err: %s",
				     device_name,
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

/* Returns true if a controller reset is in progress (VQs still being torn down) */
static inline bool vblk_ctrl_reset_in_progress(struct vblk_ctrl *ctrl)
{
	return ctrl->pending_vqs > 0;
}

/* Called on offload engine when a VQ reaches DESTROYED.
 * When the last VQ is destroyed, clear device_status to 0 so host can
 * proceed with reinitialization (host polls until status==0). */
static inline void vblk_ctrl_reset_vq_done(struct vblk_ctrl *ctrl)
{
	uint16_t expected = ctrl->pending_vqs;
	do {
		if (expected == 0)
			return;
	} while (!atomic_compare_exchange_weak(&ctrl->pending_vqs, &expected, expected - 1));
	if (expected > 1)
		return;
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = vblk_pci_virtio_get_pci_cfg(ctrl->vblk_pci_dev);
	pci_cfg->device_status = 0;
	DOCA_LOG_INFO("All VQs destroyed, device_status cleared to 0");
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
	case VBLK_VQ_BINDING:
		/* VQ created; io_ctx must bind, engine work done */
		ctrl->io_ctxs[io_ctx_id].poll.pending += 1;
		ctrl->engine_poll.pending -= 1;
		break;
	case VBLK_VQ_UNBINDING:
		/* VQ stop requested; io_ctx must unbind, engine will destroy later */
		ctrl->io_ctxs[io_ctx_id].poll.pending += 1;
		ctrl->engine_poll.pending += 1;
		ctrl->pending_vqs++;
		break;
	case VBLK_VQ_RUNNING:
		if (old_state == VBLK_VQ_BINDING)
			ctrl->io_ctxs[io_ctx_id].poll.pending -= 1;
		break;
	case VBLK_VQ_STOPPING:
		/* io_ctx finished unbinding */
		if (old_state == VBLK_VQ_UNBINDING)
			ctrl->io_ctxs[io_ctx_id].poll.pending -= 1;
		/* STARTING->STOPPING during reset: track pending teardown */
		if (old_state == VBLK_VQ_STARTING)
			ctrl->pending_vqs++;
		break;
	case VBLK_VQ_DESTROYED:
		ctrl->engine_poll.pending -= 1;
		vblk_ctrl_reset_vq_done(ctrl);
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

	DOCA_LOG_DBG("Binding vq: %d to io_ctx %u", qid, ctx_id);
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

	DOCA_LOG_DBG("vq_start_done qid=%d ctx_id=%u", qid, ctx_id);
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
		DOCA_LOG_ERR("failed to get vq%d state: %s", qid, doca_error_get_name(err));
		return err;
	}

	if (vq_state == DOCA_DEVEMU_VIRTIO_VQ_STATE_ENABLED) {
		DOCA_LOG_DBG("disable vq conf %d to io_ctx %u", qid, ctx_id);
		err = doca_devemu_virtio_vq_disable(vq);
		DOCA_LOG_DBG("vq_disable_rc qid=%d rc=%s", qid, doca_error_get_name(err));
		if (err == DOCA_SUCCESS) {
			/* No inflight requests, VQ is already DISABLED */
			DOCA_LOG_DBG("vq_stop_done qid=%d ctx_id=%u req_vq=%p", qid, ctx_id, (void *)req_vq);
			return DOCA_SUCCESS;
		}
		if (err == DOCA_ERROR_IN_PROGRESS) {
			/* VQ enters DISABLING; flush pending events of io_ctx associated with vq */
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
		/* Still draining; PE progress happens externally */
		DOCA_LOG_DBG("vq%d still DISABLING", qid);
		return DOCA_ERROR_AGAIN;
	}

	/* DISABLED */
	DOCA_LOG_DBG("vq_stop_done qid=%d ctx_id=%u req_vq=%p", qid, ctx_id, (void *)req_vq);
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

	/* Create stats list once: after all enabled queues are running.
	 * Use TLP-cached ctrl->vqs[].cfg instead of reading dev->vqs directly,
	 * as this function runs on IO thread. */
	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		if (!ctrl->vqs[qid].cfg.queue_enable)
			continue;
		if (ctrl->vqs[qid].state != VBLK_VQ_RUNNING)
			return;
	}

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

	DOCA_LOG_DBG("io_ctx=%u queues=%u enabled=%u inflights=%u", ctx_id, q_count, q_enabled, inflights_sum);
}

static void vblk_ctrl_stats_progress(struct vblk_ctrl_io_ctx *io_ctx)
{
	struct vblk_ctrl *ctrl = io_ctx->ctrl;
	doca_error_t err;
	uint8_t is_populated;

	if (ctrl->ios_period == 0)
		return;
	if (ctrl->stats_destroy_pending)
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
			ctrl->stats_populate_wip = false;
		}
		return;
	}

	err = doca_devemu_virtio_queue_dbg_state_is_populated(ctrl->state_list, &is_populated);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("vq stats is_populated failed (%s)", doca_error_get_name(err));
		io_ctx->stats_wip = false;
		ctrl->stats_populate_wip = false;
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
	ctrl->stats_populate_wip = false;
}

static void virtio_ctrl_change_cb(struct vblk_pci_virtio_dev *dev, void *arg)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg;
	pci_cfg = vblk_pci_virtio_get_pci_cfg(dev);
	const struct vblk_pci_virtq_pci_cfg *vqs = vblk_pci_virtio_get_virtq_pci_cfg(dev);
	struct vblk_ctrl *ctrl = arg;
	int qid;

	const uint8_t prev_status = ctrl->last_device_status;
	uint8_t cur_status = pci_cfg->device_status;

	DOCA_LOG_TRC("VIRTIO_CB status=0x%x prev=0x%x queue_select=%u", cur_status, prev_status, pci_cfg->queue_select);
	if (cur_status == 0 && prev_status != 0) {
		DOCA_LOG_INFO("Virtio controller reset detected -- stopping all queues");

		ctrl = arg;
		/* Defer stats list destruction to IO thread to avoid blocking TLP thread */
		if (ctrl->state_list && !ctrl->stats_destroy_pending) {
			ctrl->stats_destroy_pending = true;
			ctrl->engine_poll.pending++;
		}
		for (uint16_t ctx_id = 0; ctx_id < ctrl->num_io_ctx; ctx_id++) {
			ctrl->io_ctxs[ctx_id].stats_ios = 0;
			ctrl->io_ctxs[ctx_id].stats_wip = false;
		}

		ctrl->pending_vqs = 0;
		for (qid = 0; qid < ctrl->num_queues; qid++) {
			const enum vblk_vq_state state = ctrl->vqs[qid].state;
			switch (state) {
			case VBLK_VQ_DESTROYED:
				break;
			case VBLK_VQ_UNBINDING:
			case VBLK_VQ_STOPPING:
				ctrl->pending_vqs++;
				break;
			case VBLK_VQ_RUNNING:
			case VBLK_VQ_BINDING:
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_UNBINDING);
				break;
			case VBLK_VQ_STARTING:
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_STOPPING);
				break;
			}
			/* Clear cached config after state transition so OE thread
			 * sees STOPPING before cfg is zeroed (avoids torn read) */
			memset(&ctrl->vqs[qid].cfg, 0, sizeof(ctrl->vqs[qid].cfg));
		}

		/* Hold device_status non-zero while VQ teardown is in progress.
		 * Host polls until status==0 before reinitializing (VirtIO spec 4.1.4.3.1).
		 * This guarantees DRIVER_OK arrives only after all VQs are DESTROYED. */
		if (ctrl->pending_vqs > 0) {
			pci_cfg->device_status = VBLK_PCI_VIRTIO_DEVICE_STATUS_NEEDS_RESET;
			DOCA_LOG_INFO("Reset: holding status=0x40 until %d VQs destroyed", (int)ctrl->pending_vqs);
		}
	}

	if (cur_status & VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER_OK) {
		DOCA_LOG_DBG("Virtio controller starting queues");

		/* Always cache VQ configs from PCI config space on TLP thread first.
		 * OE thread must NOT read vqs[] directly as vblk_pci_virtio_get_virtq_pci_cfg
		 * only syncs the current queue_select, causing race conditions. */
		for (qid = 0; qid < ctrl->num_queues; qid++)
			ctrl->vqs[qid].cfg = vqs[qid];

		/* Reset sync via 0x40: host only reaches DRIVER_OK after device_status
		 * was cleared to 0 (all VQs DESTROYED). Safe to start directly. */
		for (qid = 0; qid < ctrl->num_queues; qid++) {
			if (!ctrl->vqs[qid].cfg.queue_enable)
				continue;
			if (ctrl->vqs[qid].vq != NULL || ctrl->vqs[qid].state != VBLK_VQ_DESTROYED)
				continue;
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_STARTING);
		}
	}

	/* Store device_status to detect next transition correctly */
	ctrl->last_device_status = cur_status;
}

struct vblk_poll_state *vblk_ctrl_engine_poll(struct vblk_ctrl *ctrl)
{
	return &ctrl->engine_poll;
}

static doca_error_t vblk_ctrl_vq_stop_destroy(struct doca_devemu_vblk_req_vq *vq)
{
	doca_error_t err;

	err = doca_devemu_virtio_vq_stop(doca_devemu_vblk_req_vq_as_vq(vq));
	if (err == DOCA_ERROR_BAD_STATE)
		DOCA_LOG_WARN("vq_stop: vq %p already stopped", (void *)vq);
	else if (err != DOCA_SUCCESS)
		return err;
	return doca_devemu_vblk_req_vq_destroy(vq);
}

void vblk_ctrl_offload_engine_progress(struct vblk_ctrl *ctrl)
{
	if (ctrl == NULL)
		return;
	doca_error_t err;
	struct doca_devemu_vblk_req_vq *vq;
	struct doca_devemu_virtio_vq *virtio_vq;
	int processed = 0;

	/* Handle deferred stats list destruction (moved from TLP thread) */
	if (ctrl->stats_destroy_pending) {
		if (ctrl->state_list) {
			doca_error_t result = doca_devemu_virtio_queue_dbg_state_destroy_list(ctrl->state_list);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy stats list, will retry");
				goto skip_stats_destroy;
			}
			ctrl->state_list = NULL;
			ctrl->list_len = 0;
			ctrl->stats_populate_wip = false;
		}
		ctrl->stats_destroy_pending = false;
		ctrl->engine_poll.pending--;
	}
skip_stats_destroy:

	for (int qid = 0; qid < ctrl->num_queues && processed < MAX_VQ_PER_CYCLE; qid++) {
		const enum vblk_vq_state state = ctrl->vqs[qid].state;
		switch (state) {
		case VBLK_VQ_RUNNING:
		case VBLK_VQ_BINDING:
		case VBLK_VQ_UNBINDING:
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
				continue;
			}
			err = doca_devemu_virtio_vq_start(virtio_vq);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("vq_start failed qid=%d err=%s", qid, doca_error_get_name(err));
				(void)doca_devemu_vblk_req_vq_destroy(vq);
				continue;
			}
			ctrl->vqs[qid].vq = vq;
			DOCA_LOG_DBG("vq_started qid=%d req_vq=%p", qid, (void *)vq);
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_BINDING);
			processed++;
			break; /* Continue to next VQ instead of return */
		case VBLK_VQ_STOPPING:
			vq = ctrl->vqs[qid].vq;
			/* VQ may be NULL if reset happened during STARTING before VQ was created */
			if (vq == NULL) {
				DOCA_LOG_DBG("vq_stop qid=%d skipped (no VQ)", qid);
				vblk_vq_set_state(ctrl, qid, VBLK_VQ_DESTROYED);
				processed++;
				break;
			}
			virtio_vq = doca_devemu_vblk_req_vq_as_vq(vq);
			DOCA_LOG_DBG("vq_stop qid=%d req_vq=%p vq=%p", qid, (void *)vq, (void *)virtio_vq);
			err = doca_devemu_virtio_vq_stop(virtio_vq);
			if (err != DOCA_SUCCESS && err != DOCA_ERROR_BAD_STATE)
				DOCA_LOG_DBG("vq_stop qid=%d err=%s, proceeding to destroy",
					     qid,
					     doca_error_get_name(err));
			err = doca_devemu_vblk_req_vq_destroy(vq);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("vq_destroy failed qid=%d err=%s, will retry",
					     qid,
					     doca_error_get_name(err));
				break;
			}
			ctrl->vqs[qid].vq = NULL;
			vblk_vq_set_state(ctrl, qid, VBLK_VQ_DESTROYED);
			DOCA_LOG_DBG("vq_destroy_done qid=%d", qid);
			processed++;
			break; /* Continue to next VQ instead of return */
		}
	}
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

	DOCA_LOG_DBG("GET_ID, data len is %d", req_len);

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

static doca_error_t vblk_ctrl_io_ctx_init(struct vblk_ctrl *ctrl,
					  struct vblk_ctrl_io_ctx *io_ctx,
					  struct vblk_mpool_set *shared_mpool)
{
	doca_error_t err;
	union doca_data ctx_udata = {.ptr = io_ctx};
	struct doca_dev *dev = g_vblk_dev;
	bool owns_mpool = false;
	uint32_t dma_pool_size;

	if (shared_mpool != NULL) {
		io_ctx->mpool = shared_mpool;
	} else {
		struct vblk_mpool_attr mpool_attr[] = {
			{.buf_size = 4096, .num_bufs = 1024, .devs = &dev, .num_devs = 1},
			{.buf_size = 8192, .num_bufs = 1024, .devs = &dev, .num_devs = 1},
			{.buf_size = 65536, .num_bufs = 512, .devs = &dev, .num_devs = 1},
			{.buf_size = 262144, .num_bufs = 512, .devs = &dev, .num_devs = 1},
			{.buf_size = 524288, .num_bufs = 256, .devs = &dev, .num_devs = 1},
		};
		struct vblk_mpool_set_attr mpool_set_attr = {
			.mpools = mpool_attr,
			.num_pools = ARRAY_SIZE(mpool_attr),
		};
		io_ctx->mpool = vblk_mpool_set_create(&mpool_set_attr);
		if (io_ctx->mpool == NULL) {
			DOCA_LOG_ERR("Failed to create mpool set");
			return DOCA_ERROR_NO_MEMORY;
		}
		owns_mpool = true;
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

	dma_pool_size = (uint32_t)ctrl->num_queues * VBLK_CTRL_MAX_QUEUE_SIZE;

	err = doca_dma_task_memcpy_set_conf(io_ctx->dma_ctx, vblk_ctrl_dma_done, vblk_ctrl_dma_error, dma_pool_size);
	if (err != DOCA_SUCCESS)
		goto destroy_dma_ctx;

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
	if (owns_mpool)
		vblk_mpool_set_destroy(io_ctx->mpool);
	io_ctx->mpool = NULL;
	return err;
}

static void vblk_ctrl_io_ctx_reset(struct vblk_ctrl_io_ctx *io_ctx, bool owns_mpool)
{
	if (owns_mpool && io_ctx->mpool != NULL)
		vblk_mpool_set_destroy(io_ctx->mpool);
	io_ctx->mpool = NULL;
	doca_dma_destroy(io_ctx->dma_ctx);
	if (io_ctx->dma_pe != NULL) {
		doca_pe_destroy(io_ctx->dma_pe);
		io_ctx->dma_pe = NULL;
	}
	doca_devemu_vblk_io_destroy(io_ctx->vq_io_ctx);
}
static void vblk_ctrl_io_ctxs_reset(struct vblk_ctrl *ctrl, uint8_t ctxs_num)
{
	/* Shared mpools are owned by the application, not by the io_ctx */
	while (ctxs_num-- > 0)
		vblk_ctrl_io_ctx_reset(&ctrl->io_ctxs[ctxs_num], false);
}

static void vblk_ctrl_vqs_destroy(struct vblk_ctrl *ctrl)
{
	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		struct doca_devemu_vblk_req_vq *vq = ctrl->vqs[qid].vq;
		if (vq == NULL)
			continue;
		(void)vblk_ctrl_vq_stop_destroy(vq);
		ctrl->vqs[qid].vq = NULL;
	}
}

doca_error_t vblk_ctrl_init(struct vblk_ctrl *ctrl, const struct vblk_ctrl_attrs *attr)
{
	doca_error_t err;
	struct doca_devemu_virtio_offload_engine *virtio_engine = NULL;

	/* create a single virtio block pci endpoint */
	const struct vblk_pci_virtio_blk_config blk_cfg = {
		.num_queues = attr->num_queues,
		.seg_max = attr->seg_max,
		.size_max = 4096,			   // hardcoded as 4KB
		.capacity = VBLK_CTRL_CAPACITY_BYTES / 512 // capacity is in 512b sectors
	};

	struct vblk_pci_virtio_attrs pci_attr = {.num_queues = attr->num_queues,
						 .device_type = VBLK_PCI_DEVICE_TYPE_VBLK,
						 .device_features = (1 << VBLK_F_MQ | 1 << VBLK_PCI_VIRTIO_F_SEG_MAX |
								     1 << VBLK_PCI_VIRTIO_F_SIZE_MAX),
						 .ep_index = attr->ep_index,
						 .pci_cfg_change_cb = virtio_ctrl_change_cb,
						 .dev_cfg = &blk_cfg};

	pci_attr.device_features |= attr->indirect_enabled ? (1ULL << VBLK_PCI_VIRTIO_F_INDIRECT_DESC) : 0;

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->num_queues = attr->num_queues;
	ctrl->num_io_ctx = attr->num_io_ctx;
	ctrl->ep_index = attr->ep_index;

	ctrl->vqs = calloc(ctrl->num_queues, sizeof(*ctrl->vqs));
	if (!ctrl->vqs)
		return DOCA_ERROR_NO_MEMORY;

	/* Initialize queue<->io_ctx mapping once (round-robin for now) */
	for (int qid = 0; qid < ctrl->num_queues; qid++)
		ctrl->vqs[qid].io_ctx_id = vblk_qid_to_io_ctx_id(ctrl, qid);

	pci_attr.cb_arg = ctrl;
	ctrl->vblk_pci_dev = vblk_pci_virtio_dev_create(&pci_attr);
	if (!ctrl->vblk_pci_dev) {
		err = DOCA_ERROR_INITIALIZATION;
		goto destroy_vqs;
	}

	err = doca_devemu_vblk_offload_engine_create(doca_devemu_pci_tlp_dev_as_ep(ctrl->vblk_pci_dev->pci_tlp_dev),
						     &ctrl->vq_engine);
	if (err != DOCA_SUCCESS)
		goto destroy_pci_dev;

	err = doca_devemu_vblk_offload_engine_set_seg_max(ctrl->vq_engine, attr->seg_max);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_engine;

	virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);

	err = doca_devemu_virtio_offload_engine_set_indir_descs_enabled(virtio_engine, attr->indirect_enabled);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_engine;

	err = doca_devemu_virtio_offload_engine_set_num_queues(virtio_engine, ctrl->num_queues);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_engine;

	err = doca_devemu_virtio_offload_engine_start(virtio_engine);
	if (err != DOCA_SUCCESS)
		goto destroy_vq_engine;

	strncpy(ctrl->vblk_dev_id, VBLK_CTRL_DEFAULT_ID, sizeof(ctrl->vblk_dev_id));
	return DOCA_SUCCESS;

destroy_vq_engine:
	doca_devemu_vblk_offload_engine_destroy(ctrl->vq_engine);
destroy_pci_dev:
	vblk_pci_virtio_dev_destroy(ctrl->vblk_pci_dev);
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

doca_error_t vblk_ctrl_init_io_ctx_on_thread(struct vblk_ctrl *ctrl,
					     uint16_t ctx_id,
					     struct doca_pe *pe,
					     struct vblk_mpool_set *shared_mpool)
{
	doca_error_t err;
	struct doca_ctx *io_ctx;

	if (ctrl == NULL || pe == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (ctx_id >= ctrl->num_io_ctx)
		return DOCA_ERROR_INVALID_VALUE;

	err = vblk_ctrl_io_ctx_init(ctrl, &ctrl->io_ctxs[ctx_id], shared_mpool);
	if (err != DOCA_SUCCESS)
		return err;

	err = vblk_ctrl_connect_io_ctx(ctrl, ctx_id, pe);
	if (err != DOCA_SUCCESS)
		return err;

	/* Start per-IO DMA ctx */
	err = doca_ctx_start(vblk_ctrl_dma_ctx(ctrl, ctx_id));
	if (err != DOCA_SUCCESS)
		goto err_destroy_dma_pe;

	/* Start IO ctx (may be async) */
	io_ctx = vblk_ctrl_io_ctx(ctrl, ctx_id);
	err = doca_ctx_start(io_ctx);
	if (err != DOCA_SUCCESS && err != DOCA_ERROR_IN_PROGRESS)
		goto err_stop_dma;

	/* Progress until IO ctx reaches RUNNING */
	err = vblk_wait_ctx_running(pe, io_ctx, "io", VBLK_IO_CTX_MAX_WAIT_ITERATIONS_RUNNING);
	if (err != DOCA_SUCCESS)
		goto err_stop_io;

	return DOCA_SUCCESS;

err_stop_io:
	(void)doca_ctx_stop(io_ctx);
	(void)vblk_wait_ctx_idle(pe, io_ctx, "io", VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE);
err_stop_dma:
	(void)doca_ctx_stop(vblk_ctrl_dma_ctx(ctrl, ctx_id));
	(void)vblk_wait_ctx_idle(ctrl->io_ctxs[ctx_id].dma_pe,
				 vblk_ctrl_dma_ctx(ctrl, ctx_id),
				 "dma",
				 VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE);
err_destroy_dma_pe:
	doca_pe_destroy(ctrl->io_ctxs[ctx_id].dma_pe);
	ctrl->io_ctxs[ctx_id].dma_pe = NULL;
	return err;
}

doca_error_t vblk_ctrl_shutdown_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id)
{
	struct vblk_ctrl_io_ctx *io_ctx;
	struct doca_pe *pe;

	io_ctx = &ctrl->io_ctxs[ctx_id];
	pe = io_ctx->pe;

	/* Stop all queues mapped to this IO ctx (handles all VQ states) */
	for (int qid = 0; qid < ctrl->num_queues; qid++) {
		doca_error_t err;
		enum vblk_vq_state st = ctrl->vqs[qid].state;
		if (ctrl->vqs[qid].io_ctx_id != ctx_id)
			continue;
		if (st == VBLK_VQ_DESTROYED || st == VBLK_VQ_STOPPING || st == VBLK_VQ_STARTING)
			continue;

		/* RUNNING or UNBINDING: disable the VQ first */
		if (st == VBLK_VQ_RUNNING || st == VBLK_VQ_UNBINDING || st == VBLK_VQ_BINDING) {
			if (ctrl->vqs[qid].vq == NULL)
				continue;
			while ((err = vblk_ctrl_queue_stop(ctrl, qid)) == DOCA_ERROR_AGAIN) {
				doca_pe_progress(pe);
				doca_pe_progress(io_ctx->dma_pe);
			}

			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("failed to stop vq qid=%d, err=%s", qid, doca_error_get_name(err));
				continue;
			}

			struct doca_devemu_virtio_io *ioctx = doca_devemu_vblk_io_as_virtio_io(io_ctx->vq_io_ctx);
			struct doca_devemu_virtio_vq *vq = doca_devemu_vblk_req_vq_as_vq(ctrl->vqs[qid].vq);

			doca_devemu_virtio_io_flush_vq(ioctx, vq);
			err = doca_devemu_virtio_io_unbind_vq(ioctx, vq);
			if (err != DOCA_SUCCESS && err != DOCA_ERROR_BAD_STATE)
				DOCA_LOG_ERR("vq_unbind failed qid=%d err=%s", qid, doca_error_get_name(err));
			DOCA_LOG_DBG("shutdown: vq_unbound qid=%d ctx_id=%u", qid, ctx_id);
		}
	}

	/* Stop contexts (PE thread is expected to keep progressing) */
	(void)doca_ctx_stop(doca_dma_as_ctx(io_ctx->dma_ctx));
	(void)doca_ctx_stop(vblk_io_as_ctx(io_ctx->vq_io_ctx));

	/* Wait for contexts to become IDLE (dma_ctx uses its own dma_pe) */
	(void)vblk_wait_ctx_idle(io_ctx->dma_pe,
				 doca_dma_as_ctx(io_ctx->dma_ctx),
				 "dma",
				 VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE);
	(void)vblk_wait_ctx_idle(pe, vblk_io_as_ctx(io_ctx->vq_io_ctx), "io", VBLK_IO_CTX_MAX_WAIT_ITERATIONS_IDLE);

	return DOCA_SUCCESS;
}

void vblk_ctrl_cleanup(struct vblk_ctrl *ctrl)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine =
		doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);

	vblk_ctrl_vqs_destroy(ctrl);
	vblk_ctrl_io_ctxs_reset(ctrl, ctrl->num_io_ctx);
	(void)doca_devemu_virtio_offload_engine_disable(virtio_engine);
	(void)doca_devemu_virtio_offload_engine_stop(virtio_engine);
	(void)doca_devemu_vblk_offload_engine_destroy(ctrl->vq_engine);
	vblk_pci_virtio_dev_destroy(ctrl->vblk_pci_dev);
	free(ctrl->vqs);
	ctrl->vqs = NULL;
}

struct vblk_poll_state *vblk_ctrl_io_ctx_poll(struct vblk_ctrl *ctrl, uint8_t ctx_id)
{
	return &ctrl->io_ctxs[ctx_id].poll;
}

struct doca_pe *vblk_ctrl_io_ctx_dma_pe(struct vblk_ctrl *ctrl, uint8_t ctx_id)
{
	return ctrl->io_ctxs[ctx_id].dma_pe;
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
			/* Reset handling is done in virtio_ctrl_change_cb */
			break;
		case VBLK_VQ_UNBINDING:
			err = vblk_ctrl_queue_stop(vblk_ctrl, qid);
			if (err == DOCA_SUCCESS) {
				struct doca_devemu_virtio_io *io_ctx =
					doca_devemu_vblk_io_as_virtio_io(vblk_ctrl->io_ctxs[core_id].vq_io_ctx);
				struct doca_devemu_virtio_vq *vq =
					doca_devemu_vblk_req_vq_as_vq(vblk_ctrl->vqs[qid].vq);

				DOCA_LOG_DBG("flushing vq conf %d to io_ctx %u", qid, core_id);
				doca_devemu_virtio_io_flush_vq(io_ctx, vq);

				DOCA_LOG_DBG("unbinding vq conf %d to io_ctx %u", qid, core_id);
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
