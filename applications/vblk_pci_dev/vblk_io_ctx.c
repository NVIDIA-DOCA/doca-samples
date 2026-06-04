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
#include <doca_pe.h>
#include <doca_log.h>
#include <doca_common_defines.h>

#include "vblk_io_ctx.h"
#include "vblk_ctrl.h"
#include "vblk_pci.h"
#include "vblk_pci_dev_core.h"

DOCA_LOG_REGISTER(VBLK_IO_CTX);

#define VBLK_DMA_PE_PROGRESS_MAX 512
#define VBLK_IO_PE_PROGRESS_PER_EP 1

/*
 * Create ctrl + OE + virtio_dev for a single EP.
 * Called on OE thread only (both static and hotplug modes).
 */
static doca_error_t plug_ep_ctrl(struct vblk_io_ctx_cfg *cfg, uint32_t ep)
{
	doca_error_t err;
	const struct vblk_ctrl_attrs attr = {
		.num_queues = cfg->num_queues,
		.num_io_ctx = cfg->num_io_ctx,
		.seg_max = cfg->seg_max,
		.indirect_enabled = cfg->indirect_enabled,
		.ep_index = ep,
	};

	err = vblk_ctrl_init(&cfg->ctrls[ep], &attr);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ctrl for EP %u: %s", ep, doca_error_get_descr(err));
		return err;
	}
	cfg->ctrls[ep].ios_period = cfg->stats_ios_period;
	return DOCA_SUCCESS;
}

/*
 * OE thread: handle pending plug requests in hotplug mode.
 * Creates ctrl + signals IO threads to create IO ctx.
 */
static void oe_check_plug_requests(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	doca_error_t err;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].plug_requested)
			continue;

		DOCA_LOG_INFO("OE: processing plug for EP %u", ep);
		hp[ep].plug_requested = false;

		err = plug_ep_ctrl(cfg, ep);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_WARN("OE: plug EP %u failed, re-issue 'plug %u' to retry", ep, ep);
			continue;
		}

		hp[ep].io_ctx_ready = 0;
		for (uint8_t i = 0; i < cfg->num_io_ctx; i++)
			hp[ep].io_ctx_created[i] = false;
		hp[ep].need_io_ctx = true;
	}
}

/*
 * OE thread: check if IO ctx creation is complete, then enable ctrl + trigger hotplug.
 */
static void oe_check_io_ctx_complete(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	doca_error_t err;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_io_ctx || hp[ep].enabled)
			continue;
		if (hp[ep].io_ctx_ready < cfg->num_io_ctx)
			continue;

		DOCA_LOG_INFO("OE: all IO ctx ready for EP %u, enabling controller", ep);
		err = vblk_ctrl_enable(&cfg->ctrls[ep]);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to enable ctrl EP %u: %s", ep, doca_error_get_descr(err));
			continue;
		}

		if (cfg->hotplug_mode) {
			err = vblk_pci_trigger_hotplug(ep, true);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_WARN("Hotplug MSI for EP %u failed (%s), will retry",
					      ep,
					      doca_error_get_descr(err));
				hp[ep].plug_msi_pending = true;
			} else {
				DOCA_LOG_INFO("OE: EP %u hotplug complete (DSP slot updated)", ep);
			}
		}

		hp[ep].need_io_ctx = false;
		hp[ep].enabled = true;
	}
}

static void oe_retry_hotplug_msi(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].plug_msi_pending)
			continue;
		if (!hp[ep].enabled) {
			hp[ep].plug_msi_pending = false;
			continue;
		}
		doca_error_t err = vblk_pci_trigger_hotplug(ep, true);
		if (err == DOCA_SUCCESS) {
			hp[ep].plug_msi_pending = false;
			DOCA_LOG_INFO("OE: EP %u hotplug MSI retry succeeded", ep);
		} else {
			DOCA_LOG_DBG("OE: EP %u hotplug MSI retry failed (%s)", ep, doca_error_get_descr(err));
		}
	}
}

/*
 * OE thread: Phase 1 — send ABP + MSI, wait for host to power off.
 * Do NOT shutdown IO ctx yet — device must remain alive for host's power-off sequence.
 */
static void oe_check_unplug_requests(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].unplug_requested)
			continue;

		struct pci_device_config *ep_cfg = &ctx->devs_config[FIRST_PF_IDX(ctx) + ep];
		if (ep_cfg->pending_unplug)
			continue;

		DOCA_LOG_INFO("OE: initiating unplug for EP %u — sending ABP, waiting for host Power OFF", ep);
		ep_cfg->pending_unplug = true;

		if (cfg->hotplug_mode) {
			doca_error_t err = vblk_pci_trigger_hotplug(ep, false);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_WARN("Unplug MSI for EP %u failed (%s), will retry",
					      ep,
					      doca_error_get_descr(err));
				hp[ep].unplug_msi_pending = true;
			} else {
				hp[ep].unplug_requested = false;
			}
		} else {
			hp[ep].unplug_requested = false;
		}
	}
}

static void oe_retry_unplug_msi(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].unplug_msi_pending)
			continue;
		if (!hp[ep].enabled) {
			hp[ep].unplug_msi_pending = false;
			continue;
		}
		doca_error_t err = vblk_pci_trigger_hotplug(ep, false);
		if (err == DOCA_SUCCESS) {
			hp[ep].unplug_msi_pending = false;
			hp[ep].unplug_requested = false;
			DOCA_LOG_INFO("OE: EP %u unplug MSI retry succeeded", ep);
		} else {
			DOCA_LOG_DBG("OE: EP %u unplug MSI retry failed (%s)", ep, doca_error_get_descr(err));
		}
	}
}

/*
 * OE thread: Phase 2 — detect host Power OFF (DLActive cleared by bridge_cap_write),
 * then signal IO threads to shutdown.
 */
static void oe_check_host_power_off(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].enabled)
			continue;

		struct pci_device_config *ep_cfg = &ctx->devs_config[FIRST_PF_IDX(ctx) + ep];

		if (!ep_cfg->pending_unplug)
			continue;
		if (!ep_cfg->host_power_off)
			continue;

		DOCA_LOG_INFO("OE: host powered OFF DSP[%u] — destroying IO contexts", ep);
		ep_cfg->host_power_off = false;
		ep_cfg->pending_unplug = false;
		hp[ep].enabled = false;
		hp[ep].io_ctx_shutdown_done = 0;
		hp[ep].need_io_ctx_shutdown = true;
	}
}

/*
 * OE thread: Phase 3 — after all IO threads finished shutdown, cleanup ctrl and destroy device.
 */
static void oe_check_unplug_complete(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_io_ctx_shutdown)
			continue;
		if (hp[ep].io_ctx_shutdown_done < cfg->num_io_ctx)
			continue;

		DOCA_LOG_INFO("OE: all IO ctx shutdown for EP %u — cleaning up", ep);
		hp[ep].need_io_ctx_shutdown = false;

		vblk_ctrl_cleanup(&cfg->ctrls[ep]);
		vblk_pci_destroy_device(ep);

		for (uint8_t i = 0; i < cfg->num_io_ctx; i++)
			hp[ep].io_ctx_created[i] = false;
		hp[ep].io_ctx_ready = 0;
		hp[ep].plug_msi_pending = false;
		hp[ep].unplug_msi_pending = false;

		DOCA_LOG_INFO("OE: EP %u unplug complete", ep);
	}
}

/*
 * IO thread (all threads including OE): create IO ctx for EPs that need it.
 */
static void io_check_need_io_ctx(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id, struct doca_pe *pe)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_io_ctx || hp[ep].io_ctx_created[ctx_id])
			continue;

		doca_error_t err = vblk_ctrl_init_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id, pe, cfg->shared_mpool);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init IO ctx for EP %u ctx %u: %s",
				     ep,
				     ctx_id,
				     doca_error_get_descr(err));
			continue;
		}
		hp[ep].io_ctx_created[ctx_id] = true;
		hp[ep].io_ctx_ready++;
		DOCA_LOG_DBG("IO ctx created for EP %u on thread %u (%u/%u ready)",
			     ep,
			     ctx_id,
			     (uint32_t)hp[ep].io_ctx_ready,
			     cfg->num_io_ctx);
	}
}

/*
 * IO thread (all threads including OE): shutdown IO ctx for EPs being unplugged.
 */
static void io_check_need_io_ctx_shutdown(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_io_ctx_shutdown || !hp[ep].io_ctx_created[ctx_id])
			continue;

		(void)vblk_ctrl_shutdown_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
		hp[ep].io_ctx_created[ctx_id] = false;
		hp[ep].io_ctx_shutdown_done++;
		DOCA_LOG_DBG("IO ctx shutdown for EP %u on thread %u (%u/%u done)",
			     ep,
			     ctx_id,
			     (uint32_t)hp[ep].io_ctx_shutdown_done,
			     cfg->num_io_ctx);
	}
}

/*
 * Progress IO for all enabled EPs on this thread.
 */
static void io_progress_all_eps(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id, bool is_oe)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].enabled)
			continue;

		struct doca_pe *dma_pe = vblk_ctrl_io_ctx_dma_pe(&cfg->ctrls[ep], ctx_id);
		if (dma_pe != NULL)
			for (int i = 0; i < VBLK_DMA_PE_PROGRESS_MAX && doca_pe_progress(dma_pe); i++) {}

		struct vblk_poll_state *io_poll = vblk_ctrl_io_ctx_poll(&cfg->ctrls[ep], ctx_id);

		if (doca_unlikely(is_oe)) {
			struct vblk_poll_state *engine_poll = vblk_ctrl_engine_poll(&cfg->ctrls[ep]);
			if (!vblk_poll_skip(engine_poll))
				vblk_ctrl_offload_engine_progress(&cfg->ctrls[ep]);
		}
		if (doca_unlikely(!vblk_poll_skip(io_poll)))
			vblk_ctrl_io_context_progress(&cfg->ctrls[ep], ctx_id);
	}
}

static void *vblk_io_ctx_thread(void *arg)
{
	struct vblk_io_ctx_cfg *cfg = arg;
	const struct vblk_io_ctx_app_ops *ops = cfg->ops;
	const uint8_t ctx_id = cfg->ctx_id;
	const bool is_oe = (cfg->affinity_core == cfg->offload_engine_core_idx);
	struct doca_pe *pe = cfg->pe;
	bool hotplug = cfg->hotplug_mode;
	doca_error_t err;

	/*
	 * Static mode: OE thread creates all controllers at startup.
	 * Hotplug mode: skip, controllers created dynamically via plug commands.
	 */
	if (is_oe && !hotplug) {
		for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
			err = plug_ep_ctrl(cfg, ep);
			if (err != DOCA_SUCCESS) {
				for (uint32_t j = 0; j < ep; j++)
					vblk_ctrl_cleanup(&cfg->ctrls[j]);
				ops->signal_ctrl_created(err, NULL);
				goto out;
			}
		}
	}

	if (is_oe)
		ops->signal_ctrl_created(DOCA_SUCCESS, cfg->ctrls);
	else {
		/* Non-OE threads wait for OE to finish ctrl creation before proceeding */
		struct vblk_ctrl *dummy;

		err = ops->wait_ctrl_created(&dummy);
		if (err != DOCA_SUCCESS || force_quit)
			goto out;
	}

	/* Static mode: all IO threads create IO ctx for all EPs now */
	if (!hotplug) {
		for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
			err = vblk_ctrl_init_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id, pe, cfg->shared_mpool);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to init IO ctx for EP %u ctx %u: %s",
					     ep,
					     ctx_id,
					     doca_error_get_descr(err));
				force_quit = true;
				if (is_oe) {
					ops->signal_ctrl_created(err, NULL);
					for (uint32_t j = 0; j < cfg->num_ep; j++)
						vblk_ctrl_cleanup(&cfg->ctrls[j]);
				}
				ops->signal_io_ready();
				goto out;
			}
			cfg->hp_states[ep].io_ctx_created[ctx_id] = true;
			cfg->hp_states[ep].enabled = true;
		}
	}
	ops->signal_io_ready();

	if (is_oe) {
		err = ops->wait_io_ready();
		if (!force_quit && err == DOCA_SUCCESS) {
			if (!hotplug) {
				for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
					err = vblk_ctrl_enable(&cfg->ctrls[ep]);
					if (err != DOCA_SUCCESS) {
						DOCA_LOG_ERR("Failed to enable ctrl EP %u: %s",
							     ep,
							     doca_error_get_descr(err));
						break;
					}
				}
			} else {
				DOCA_LOG_INFO("Hotplug mode: EPs will be created dynamically via plug commands");
				err = DOCA_SUCCESS;
			}
			ops->signal_ctrl_enabled(err);
		}
	}

	/* Main progress loop */
	while (!force_quit) {
		if (doca_unlikely(is_oe && hotplug)) {
			oe_check_plug_requests(cfg);
			oe_check_io_ctx_complete(cfg);
			oe_retry_hotplug_msi(cfg);
			oe_check_unplug_requests(cfg);
			oe_retry_unplug_msi(cfg);
			oe_check_host_power_off(cfg);
			oe_check_unplug_complete(cfg);
		}

		io_check_need_io_ctx(cfg, ctx_id, pe);
		io_check_need_io_ctx_shutdown(cfg, ctx_id);
		io_progress_all_eps(cfg, ctx_id, is_oe);
		for (uint32_t i = 0; i < VBLK_IO_PE_PROGRESS_PER_EP && doca_pe_progress(pe); i++) {}
	}

	/* Shutdown: stop IO ctx for all enabled EPs on this thread */
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (cfg->hp_states[ep].io_ctx_created[ctx_id])
			(void)vblk_ctrl_shutdown_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
	}

	if (is_oe) {
		ops->wait_all_io_exited((uint8_t)(cfg->num_io_ctx > 0 ? cfg->num_io_ctx - 1 : 0));
		ops->wait_pci_stopped();
		for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
			if (cfg->hp_states[ep].enabled)
				vblk_ctrl_cleanup(&cfg->ctrls[ep]);
		}
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
	DOCA_LOG_DBG("thread_created type=%s ctx_id=%u affinity_core=%u num_ep=%u hotplug=%d",
		     type,
		     cfg->ctx_id,
		     cfg->affinity_core,
		     cfg->num_ep,
		     cfg->hotplug_mode);
	return DOCA_SUCCESS;
}
