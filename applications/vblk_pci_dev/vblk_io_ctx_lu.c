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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sched.h>
#include <pthread.h>
#include <doca_pe.h>
#include <doca_log.h>
#include <doca_common_defines.h>

#include "vblk_io_ctx_lu.h"
#include "vblk_ctrl_lu.h"
#include "vblk_pci_dev_core_lu.h"

DOCA_LOG_REGISTER(VBLK_IO_CTX);

static pthread_mutex_t io_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void *vblk_io_ctx_thread(void *arg)
{
	struct vblk_io_ctx_cfg *cfg = arg;
	const struct vblk_io_ctx_app_ops *ops = cfg->ops;
	const uint8_t ctx_id = cfg->ctx_id;
	const bool is_offload_engine = (cfg->affinity_core == cfg->offload_engine_core_idx);
	struct vblk_ctrl *ctrl = NULL;
	struct doca_pe *pe = cfg->pe;
	bool io_inited = false;
	doca_error_t err;

	if (is_offload_engine) {
		const struct vblk_ctrl_attrs attr = {
			.num_queues = cfg->num_queues,
			.num_io_ctx = cfg->num_io_ctx,
			.seg_max = cfg->seg_max,
			.indirect_enabled = cfg->indirect_enabled,
		};
		err = vblk_ctrl_init(cfg->ctrl, &attr, cfg->ep);
		if (err != DOCA_SUCCESS) {
			ops->signal_ctrl_created(err, NULL);
			goto out;
		}
		ctrl = cfg->ctrl;
		ctrl->ios_period = cfg->stats_ios_period;
		ops->signal_ctrl_created(DOCA_SUCCESS, ctrl);
	} else {
		err = ops->wait_ctrl_created(&ctrl);
		if (err != DOCA_SUCCESS || force_quit)
			goto out;
	}

	pthread_mutex_lock(&io_init_lock);
	err = vblk_ctrl_init_io_ctx_on_thread(ctrl, ctx_id, pe);
	pthread_mutex_unlock(&io_init_lock);
	if (err != DOCA_SUCCESS) {
		if (is_offload_engine) {
			ops->signal_ctrl_created(err, NULL);
			vblk_ctrl_cleanup(ctrl);
			ctrl = NULL;
		}
		goto out;
	}
	io_inited = true;
	ops->signal_io_ready();

	if (is_offload_engine) {
		err = ops->wait_io_ready();
		if (!force_quit && err == DOCA_SUCCESS && ops->wait_pre_enable)
			err = ops->wait_pre_enable();
		if (!force_quit && err == DOCA_SUCCESS) {
			if (!ctrl->handover_dst)
				err = vblk_ctrl_enable(ctrl);
			if (err != DOCA_SUCCESS)
				DOCA_LOG_ERR("failed to enable virtio controller (%s)", doca_error_get_name(err));
		} else if (!force_quit) {
			DOCA_LOG_ERR("pre-enable coordination failed (%s)", doca_error_get_name(err));
		}
		ops->signal_ctrl_enabled(err);
	}
	DOCA_LOG_INFO("IO context thread %u: setup complete", ctx_id);
	/* Main progress loop */
	while (!force_quit) {
		struct vblk_poll_state *io_poll = vblk_ctrl_io_ctx_poll(ctrl, ctx_id);
		struct vblk_poll_state *engine_poll = is_offload_engine ? vblk_ctrl_engine_poll(ctrl) : NULL;

		if (doca_unlikely(is_offload_engine && cfg->ipc_poll))
			cfg->ipc_poll(cfg->ipc_poll_arg);
		if (doca_unlikely(engine_poll != NULL && !vblk_poll_skip(engine_poll)))
			vblk_ctrl_offload_engine_progress(ctrl);
		(void)doca_pe_progress(pe);
		if (doca_unlikely(!vblk_poll_skip(io_poll)))
			vblk_ctrl_io_context_progress(ctrl, ctx_id);
	}

	if (io_inited)
		(void)vblk_ctrl_shutdown_io_ctx_on_thread(ctrl, ctx_id);

	if (is_offload_engine) {
		ops->wait_all_io_exited((uint8_t)(cfg->num_io_ctx > 0 ? cfg->num_io_ctx - 1 : 0));
		vblk_ctrl_cleanup(ctrl);
	}

out:
	ops->signal_io_exited();
	return NULL;
}

doca_error_t vblk_io_ctx_thread_create(struct vblk_io_ctx_cfg *cfg, pthread_t *thread)
{
	pthread_attr_t attr;
	cpu_set_t cpus;
	int rc;

	rc = pthread_attr_init(&attr);
	if (rc != 0)
		return DOCA_ERROR_INITIALIZATION;

	CPU_ZERO(&cpus);
	CPU_SET(cfg->affinity_core, &cpus);
	rc = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
	if (rc != 0)
		DOCA_LOG_WARN("Failed to set IO thread affinity to core %u (rc=%d)", cfg->affinity_core, rc);

	rc = pthread_create(thread, &attr, vblk_io_ctx_thread, cfg);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		DOCA_LOG_ERR("Failed to create IO context thread %u", cfg->ctx_id);
		return DOCA_ERROR_INITIALIZATION;
	}

	const char *type = (cfg->affinity_core == cfg->offload_engine_core_idx) ? "offload_engine+io_context" :
										  "io_context";
	DOCA_LOG_DBG("thread_created type=%s ctx_id=%u affinity_core=%u", type, cfg->ctx_id, cfg->affinity_core);
	return DOCA_SUCCESS;
}
