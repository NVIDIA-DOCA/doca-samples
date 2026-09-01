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
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <doca_pe.h>
#include <doca_log.h>
#include <doca_common_defines.h>
#include <doca_devemu_virtio_tlp.h>

#include "vblk_io_ctx.h"
#include "vblk_ctrl.h"
#include "vblk_pci.h"
#include "vblk_pci_dev_core.h"

DOCA_LOG_REGISTER(VBLK_IO_CTX);

#define VBLK_DMA_PE_PROGRESS_MAX 512
#define VBLK_IO_PE_PROGRESS_PER_EP 1

/* Per-EP SR-IOV VF datapath helpers, defined later but used by the hotplug
 * plug/io_ctx/unplug paths above their definitions. */
static doca_error_t oe_build_vf_engines_for_ep(struct vblk_io_ctx_cfg *cfg, uint32_t ep);
static doca_error_t io_vf_create_io_ctx_for_ep_on_thread(struct vblk_io_ctx_cfg *cfg,
							 uint32_t ep,
							 uint8_t ctx_id,
							 struct doca_pe *pe);
static void io_vf_shutdown_io_ctx_for_ep_on_thread(struct vblk_io_ctx_cfg *cfg, uint32_t ep, uint8_t ctx_id);
static void oe_teardown_vf_dp_for_ep(struct vblk_io_ctx_cfg *cfg, uint32_t ep);
static void vblk_ctrl_destroy_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id);

/* When fw runs out of ACG grants, vblk_pci_trigger_hotplug() returns
 * DOCA_ERROR_AGAIN. Without a backoff the OE main loop spins ~1000 retries/ms,
 * monopolizing the CPU, flooding the log with WRN lines, and (observed under
 * hp.sh 100/0.5/10) destabilizing the DPU OS itself. Periodically yielding
 * for VBLK_OE_BACKOFF_US gives fw time to replenish ACG credits and relieves
 * sustained cmd_if pressure on mlx5_core (the suspected trigger for the
 * kernel-side RCU stall observed in a round-31 hang). */
#define VBLK_OE_BACKOFF_US 500
#define VBLK_OE_BACKOFF_ITERS 1000

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
 *
 * Rate-limit: process at most VBLK_OE_MAX_PLUG_PER_LOOP requests per
 * main-loop iteration. plug_ep_ctrl() drives several synchronous doca calls
 * (vblk_ctrl_init -> doca_devemu_vblk_offload_engine_create/start, etc.)
 * each of which costs a fw round-trip. With 32 EPs queued at once the OE
 * thread would stay inside this for-loop for hundreds of ms, missing the
 * main-loop heartbeat and starving the TLP thread of CPU, which in turn
 * delays host MMIO read completions and trips pciehp_ist flush_work
 * (the host backtrace seen on hang).
 */
#define VBLK_OE_MAX_PLUG_PER_LOOP 4

static void oe_check_plug_requests(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	doca_error_t err;
	uint32_t processed = 0;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].plug_requested)
			continue;

		if (hp[ep].unplug_requested || hp[ep].need_io_ctx_shutdown) {
			DOCA_LOG_INFO("OE: deferring plug for EP %u — unplug still in progress", ep);
			continue;
		}

		if (processed >= VBLK_OE_MAX_PLUG_PER_LOOP)
			return;

		DOCA_LOG_INFO("OE: processing plug for EP %u", ep);

		err = plug_ep_ctrl(cfg, ep);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_WARN("OE: plug EP %u failed, re-issue 'plug %u' to retry", ep, ep);
			hp[ep].plug_requested = false;
			continue;
		}

		/* plug_ep_ctrl created this PF's static resources, including its SR-IOV
		 * VF reps/TLP devices. Build the VF engines here on the OE thread so the
		 * IO threads create the per-thread VF io_ctxs alongside the PF's below -
		 * mirrors the static-mode startup so a hotplugged SR-IOV PF gets full
		 * PF + VF datapaths. */
		if (cfg->sriov_enabled) {
			err = oe_build_vf_engines_for_ep(cfg, ep);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_WARN("OE: VF engine build for EP %u failed (%s), re-issue 'plug %u'",
					      ep,
					      doca_error_get_descr(err),
					      ep);
				oe_teardown_vf_dp_for_ep(cfg, ep);
				vblk_ctrl_cleanup(&cfg->ctrls[ep], cfg->pe);
				vblk_pci_destroy_device(ep);
				hp[ep].enabled = false;
				hp[ep].need_io_ctx = false;
				hp[ep].io_ctx_ready = 0;
				for (uint8_t i = 0; i < cfg->num_io_ctx; i++)
					hp[ep].io_ctx_created[i] = false;
				continue;
			}
		}

		hp[ep].io_ctx_ready = 0;
		for (uint8_t i = 0; i < cfg->num_io_ctx; i++)
			hp[ep].io_ctx_created[i] = false;
		hp[ep].need_io_ctx = true;
		hp[ep].plug_requested = false;
		processed++;
	}
}

/*
 * Backpressure: cap how many EPs are "in-flight plug" at the same time
 * (hotplug MSI delivered but host has not finished Power-On yet).
 *
 * Without this cap, oe_check_io_ctx_complete() fires hotplug MSI for all
 * ready EPs as soon as their IO ctxs are ready. With hp.sh 32 EP / 0.1s,
 * that can deliver ~32 hotplug MSIs in < 2 s. Each MSI on the host enqueues
 * a pciehp_ist work item that does a 5 s power-on debounce + virtio_blk
 * probe (MMIO-heavy). The host pciehp work queue cannot service them in
 * parallel — pciehp_ist wedges inside __flush_work waiting on a probe that
 * is starved of CPU/MMIO bandwidth, which matches the kernel backtrace
 * (irq/N-pciehp blocked >10 s in pciehp_configure_device → pci_call_probe
 * → work_on_cpu_key → __flush_work) every time the test hangs.
 *
 * Holding back until host_power_on_done arrives gives the host pciehp work
 * queue room to drain before the next batch lands.
 *
 * Use the same backpressure on unplug: pending_unplug means ABP was accepted
 * and vblk is waiting for the host to power the slot off.
 */
#define VBLK_OE_MAX_INFLIGHT_PLUG 4
#define VBLK_OE_MAX_INFLIGHT_UNPLUG 4

static uint32_t oe_count_inflight_plug(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();
	uint32_t count = 0;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct pci_device_config *ep_cfg = &ctx->devs_config[FIRST_PF_IDX(ctx) + ep];

		/* plug_msi_pending covers the window between "OE handed MSI off
		 * to TLP" and "TLP actually sent it" so a burst of ready EPs
		 * cannot push past the cap while the TLP thread is still draining
		 * ACG credits. */
		if (hp[ep].plug_msi_pending || (hp[ep].enabled && !ep_cfg->host_power_on_done))
			count++;
	}
	return count;
}

static uint32_t oe_count_inflight_unplug(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();
	uint32_t count = 0;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct pci_device_config *ep_cfg = &ctx->devs_config[FIRST_PF_IDX(ctx) + ep];

		if (ep_cfg->pending_unplug)
			count++;
	}
	return count;
}

/*
 * OE thread: check if IO ctx creation is complete, then enable ctrl and queue
 * the hotplug MSI for the TLP thread to send.
 *
 * The MSI itself must originate from the TLP thread (see
 * vblk_pci_retry_hotplug_msi in vblk_pci.c) because doca ctx is thread-unsafe
 * and the lib's req_pool.acquire() runs on the TLP thread inside the ev_cb
 * dispatch; issuing complete_acg() from OE would race that unlocked pool.
 * We only set plug_msi_pending here; TLP will set 'enabled' on success.
 */
static void oe_check_io_ctx_complete(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	doca_error_t err;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_io_ctx || hp[ep].enabled || hp[ep].plug_msi_pending)
			continue;
		if (hp[ep].io_ctx_ready < cfg->num_io_ctx)
			continue;

		if (cfg->hotplug_mode && oe_count_inflight_plug(cfg) >= VBLK_OE_MAX_INFLIGHT_PLUG)
			return;

		DOCA_LOG_INFO("OE: all IO ctx ready for EP %u, enabling controller", ep);
		err = vblk_ctrl_enable(&cfg->ctrls[ep]);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to enable ctrl EP %u: %s", ep, doca_error_get_descr(err));
			continue;
		}

		hp[ep].need_io_ctx = false;

		if (cfg->hotplug_mode) {
			/* Hand off MSI send to the TLP thread. 'enabled' stays false
			 * until TLP confirms the MSI was actually delivered so the
			 * unplug FSM (which waits on host_power_on_done, itself set
			 * only after MSI is seen by host) cannot start prematurely. */
			DOCA_LOG_INFO("OE: EP %u queued for hotplug MSI on TLP thread", ep);
			__atomic_store_n(&hp[ep].plug_msi_pending, true, __ATOMIC_RELEASE);
		} else {
			/* Static mode: no hotplug MSI is needed, controller is operational. */
			hp[ep].enabled = true;
		}
	}
}

/*
 * OE thread: Phase 1 — queue ABP + MSI for the TLP thread, wait for host
 * to power off. Do NOT shutdown IO ctx yet — device must remain alive for
 * host's power-off sequence.
 *
 * MSI send is deferred to the TLP thread (see vblk_pci_retry_hotplug_msi in
 * vblk_pci.c) for the same doca ctx thread-safety reason as the plug path.
 * OE keeps ownership of pending_unplug (used for in-flight capping) and
 * TLP clears unplug_requested once the MSI actually goes out.
 */
static void oe_check_unplug_requests(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].unplug_requested)
			continue;

		/* Race: stdin accepted unplug while enabled=true, but oe_check_host_power_off
		 * has since cleared enabled=false from a prior unplug cycle. Sending another
		 * ABP here would land inside host's pciehp button-press debounce window and
		 * trigger 'Button cancel / Action canceled', leaving the host wedged. */
		if (!hp[ep].enabled) {
			hp[ep].unplug_requested = false;
			DOCA_LOG_WARN("OE: dropping stale unplug for EP %u (already unplugging)", ep);
			continue;
		}

		struct pci_device_config *ep_cfg = &ctx->devs_config[FIRST_PF_IDX(ctx) + ep];
		if (ep_cfg->pending_unplug)
			continue;

		/* Defer unplug ABP until host has finished Power-On. Otherwise the
		 * unplug ABP lands inside the host pciehp 5s power-on debounce
		 * window and is interpreted as 'Action canceled', so the host
		 * never completes the power cycle. The request stays queued and is
		 * retried on the next OE loop. */
		if (!ep_cfg->host_power_on_done)
			continue;

		if (cfg->hotplug_mode && oe_count_inflight_unplug(cfg) >= VBLK_OE_MAX_INFLIGHT_UNPLUG)
			return;

		DOCA_LOG_INFO("OE: initiating unplug for EP %u — queuing MSI on TLP thread", ep);
		ep_cfg->pending_unplug = true;

		if (cfg->hotplug_mode) {
			/* TLP thread will send ABP MSI and clear unplug_requested on success. */
			__atomic_store_n(&hp[ep].unplug_msi_pending, true, __ATOMIC_RELEASE);
		} else {
			hp[ep].unplug_requested = false;
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
 * Destroy a VF slot's datapath controller for final PF teardown: drain
 * in-flight I/O and release the offload engine, io_ctx, and VQs, then free and
 * clear the slot pointer. Runs on the OE thread (the datapath's PE owner).
 * VFE 1->0 and VF FLR must not call this; they keep vf_ctrl alive.
 */
static void oe_vf_dp_destroy(struct vblk_sriov_vf_slot *slot, struct doca_pe *pe)
{
	struct vblk_ctrl *vf_ctrl = slot->vf_ctrl;

	if (vf_ctrl == NULL)
		return;

	atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_DOWN, memory_order_release);
	/* Reset the TLP->OE intent so a reused slot starts clean instead of
	 * acting on stale VFE state. */
	atomic_store_explicit(&slot->dp_want_up, false, memory_order_release);
	slot->vf_ctrl = NULL;
	vblk_ctrl_cleanup_vf(vf_ctrl, pe);
	free(vf_ctrl);
}

/*
 * Tear down all per-VF datapaths of one PF on the OE thread. Each VF datapath
 * (vf_ctrl: offload engine + io_ctx + VQs) is reached from the VF TLP config
 * callback, so stop the VF TLP device before freeing vf_ctrl. Used both on
 * app shutdown and on PF hotplug-unplug. Runs on the OE thread (the datapath's
 * PE owner). No-op when SR-IOV is disabled or the PF has no VFs. VFE and PF FLR
 * do not use this path; they only disable VF offload and keep VF state alive
 * for restore.
 */
static void oe_teardown_vf_dp_for_ep(struct vblk_io_ctx_cfg *cfg, uint32_t ep)
{
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();
	struct pci_device_config *pf_cfg = &ctx->devs_config[FIRST_PF_IDX(ctx) + ep];
	struct vblk_sriov_pf_state *sriov;

	if (!cfg->sriov_enabled)
		return;
	sriov = pf_cfg->sriov;
	if (sriov == NULL || sriov->vf_slots == NULL)
		return;

	for (uint16_t i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];

		if (slot->vf_ctrl == NULL)
			continue;
		if (vblk_pci_sriov_vf_tlp_dev_stop(slot) != DOCA_SUCCESS) {
			DOCA_LOG_ERR("OE: VF[%u] TLP stop failed, leaving datapath live", slot->dynamic_vf_index);
			continue;
		}
		oe_vf_dp_destroy(slot, cfg->pe);
		DOCA_LOG_INFO("OE: VF[%u] datapath torn down", slot->dynamic_vf_index);
	}
}

/*
 * OE thread: Phase 3 — after all IO threads finished shutdown, cleanup ctrl
 * and destroy device. vblk_ctrl_cleanup() drains the offload-engine stop
 * synchronously (iteration-bounded); on the rare hard failure we log + drop
 * through so the EP slot fails loudly instead of silently deferring.
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

		/* Tear down this PF's VF datapaths on the OE thread before
		 * vblk_ctrl_cleanup -> free_pf_state -> close_vf_reps destroys the VF
		 * TLP devices they reference. Mirrors the app-shutdown drain; without it
		 * an unplug with live VFs would destroy a VF TLP device under its
		 * running OE datapath. No-op for a non-SR-IOV PF (no VF slots). */
		oe_teardown_vf_dp_for_ep(cfg, ep);

		/* Defensive ack of host vp_reset before destroying virtio_dev: the
		 * normal path (host writes device_status=0 -> virtio_ctrl_change_cb
		 * -> reset_vq_done) already clears it, but vblk_ctrl_shutdown_io_ctx_on_thread
		 * unbinds VQs directly without going through reset_vq_done, so under
		 * stress device_status can still hold NEEDS_RESET here. We rely on
		 * host_power_off to gate this stage (oe_check_host_power_off set
		 * need_io_ctx_shutdown only after pciehp serialised driver remove +
		 * vp_reset before powering the slot off), so no grace is needed:
		 * by this point the host is no longer polling device_status. */
		vblk_pci_ack_host_reset(ep);

		doca_error_t err = vblk_ctrl_cleanup(&cfg->ctrls[ep], cfg->pe);
		if (err != DOCA_SUCCESS)
			DOCA_LOG_ERR("OE: EP %u cleanup returned %s, proceeding with destroy",
				     ep,
				     doca_error_get_descr(err));

		vblk_pci_destroy_device(ep);

		for (uint8_t i = 0; i < cfg->num_io_ctx; i++)
			hp[ep].io_ctx_created[i] = false;
		hp[ep].io_ctx_ready = 0;
		hp[ep].plug_msi_pending = false;
		hp[ep].unplug_msi_pending = false;
		hp[ep].need_post_flr_oe_restart = false;
		hp[ep].need_post_flr_reenable = false;
		hp[ep].io_ctx_flr_destroyed = 0;

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
		/* SR-IOV: this thread also creates its io_ctx for each of the PF's VFs
		 * (engines were built by oe_build_vf_engines_for_ep on plug). Count the
		 * thread ready only once both PF and VF io_ctxs exist, so the enable
		 * barrier (oe_check_io_ctx_complete) waits for the full datapath. */
		if (cfg->sriov_enabled) {
			err = io_vf_create_io_ctx_for_ep_on_thread(cfg, ep, ctx_id, pe);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to init VF IO ctx for EP %u ctx %u: %s",
					     ep,
					     ctx_id,
					     doca_error_get_descr(err));
				(void)vblk_ctrl_shutdown_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
				vblk_ctrl_destroy_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
				continue;
			}
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

		/* SR-IOV: drain+stop this PF's VF io_ctxs on this thread before PF
		 * shutdown and before the OE destroys the VF datapaths
		 * (oe_check_unplug_complete -> oe_teardown_vf_dp_for_ep). */
		if (cfg->sriov_enabled)
			io_vf_shutdown_io_ctx_for_ep_on_thread(cfg, ep, ctx_id);
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

		if (!hp[ep].io_ctx_created[ctx_id])
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

/*
 * OE thread: detect the FLR kickoff on each EP and fan out to per-io_ctx
 * flags so each IO thread can shutdown its own ctx_id
 * (vblk_ctrl_shutdown_io_ctx_on_thread must run on the thread that owns
 * the PE).
 */
static void oe_check_flr_kickoff(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct vblk_ctrl *ctrl = &cfg->ctrls[ep];
		if (ctrl->vblk_pci_dev == NULL || !ctrl->vblk_pci_dev->flr_kickoff)
			continue;

		DOCA_LOG_INFO("OE: FLR detected on EP %u — signalling IO ctxs to stop", ep);
		hp[ep].io_ctx_flr_destroyed = 0;
		hp[ep].need_post_flr_oe_restart = true;
		for (uint8_t i = 0; i < cfg->num_io_ctx; i++)
			atomic_store_explicit(&ctrl->io_ctxs[i].need_flr_teardown, true, memory_order_release);

		ctrl->vblk_pci_dev->flr_kickoff = false;
	}
}

/* sriov state for PF endpoint `ep`, or NULL when SR-IOV is off / this PF has none. */
static struct vblk_sriov_pf_state *oe_pf_sriov(struct vblk_io_ctx_cfg *cfg, uint32_t ep)
{
	struct vblk_tlp_context *ctx = vblk_pci_get_tlp_ctx();

	if (!cfg->sriov_enabled)
		return NULL;
	return ctx->devs_config[FIRST_PF_IDX(ctx) + ep].sriov;
}

/*
 * OE thread, startup: build each VF's offload engine (no io_ctx, left DISABLED).
 * The VF queue->core rotation continues per PF from the PF's queues via
 * io_ctx_base, so VF queues scatter across all cores. Mirrors plug_ep_ctrl for
 * the PF; the per-thread io_ctxs are created later by io_vf_create_io_ctx_on_thread.
 */
static doca_error_t oe_build_vf_engines_for_ep(struct vblk_io_ctx_cfg *cfg, uint32_t ep)
{
	struct vblk_sriov_pf_state *sriov = oe_pf_sriov(cfg, ep);
	doca_error_t err = DOCA_SUCCESS;
	uint16_t base;

	if (sriov == NULL || sriov->vf_slots == NULL)
		return DOCA_SUCCESS;
	/* Continue the queue->core rotation from the PF over all io_ctxs, so VF
	 * queues fan out across the same cores as the PF's. */
	base = (uint16_t)(cfg->num_queues % cfg->num_io_ctx);
	for (uint16_t i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
		struct vblk_ctrl *vf_ctrl;

		if (slot->vf_dev == NULL || slot->vf_dev->pci_tlp_dev == NULL || slot->vf_ctrl != NULL)
			continue;
		vf_ctrl = calloc(1, sizeof(*vf_ctrl));
		if (vf_ctrl == NULL) {
			err = DOCA_ERROR_NO_MEMORY;
			goto cleanup_built_vfs;
		}
		err = vblk_ctrl_init_vf_engine(vf_ctrl,
					       slot->vf_dev,
					       sriov->vf_num_db,
					       cfg->seg_max,
					       cfg->indirect_enabled,
					       cfg->num_io_ctx,
					       base);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("OE: VF[%u] engine build failed: %s",
				     slot->dynamic_vf_index,
				     doca_error_get_descr(err));
			free(vf_ctrl);
			goto cleanup_built_vfs;
		}
		vf_ctrl->ios_period = cfg->stats_ios_period;
		slot->vf_ctrl = vf_ctrl;
		atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_DOWN, memory_order_release);
		DOCA_LOG_INFO("OE: VF[%u] engine built (queues=%u io_ctx=%u base=%u)",
			      slot->dynamic_vf_index,
			      sriov->vf_num_db,
			      cfg->num_io_ctx,
			      base);
		base = (uint16_t)((base + sriov->vf_num_db) % cfg->num_io_ctx);
	}
	return DOCA_SUCCESS;

cleanup_built_vfs:
	oe_teardown_vf_dp_for_ep(cfg, ep);
	return err;
}

/* Build every PF's VF engines (static-mode startup; hotplug builds per-EP on plug). */
static doca_error_t oe_build_vf_engines(struct vblk_io_ctx_cfg *cfg)
{
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		doca_error_t err = oe_build_vf_engines_for_ep(cfg, ep);

		if (err != DOCA_SUCCESS)
			return err;
	}
	return DOCA_SUCCESS;
}

/* Per IO thread: create this thread's io_ctx for every VF of one PF. */
static doca_error_t io_vf_create_io_ctx_for_ep_on_thread(struct vblk_io_ctx_cfg *cfg,
							 uint32_t ep,
							 uint8_t ctx_id,
							 struct doca_pe *pe)
{
	struct vblk_sriov_pf_state *sriov = oe_pf_sriov(cfg, ep);

	if (sriov == NULL || sriov->vf_slots == NULL)
		return DOCA_SUCCESS;
	for (uint16_t i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
		doca_error_t err;

		if (slot->vf_ctrl == NULL)
			continue;
		/* Idempotent: a PF FLR tears down and rebuilds only the PF's per-thread
		 * io_ctxs (oe_check_post_flr_restart re-arms need_io_ctx, which calls us
		 * via io_check_need_io_ctx), but VF io_ctxs SURVIVE a PF FLR - they stay
		 * bound to their untouched VF offload engines. Skip a VF whose io_ctx
		 * already exists on this thread; re-initializing it would overwrite the
		 * live vq_io_ctx/dma_ctx pointers and orphan the originals (still in the
		 * engine's virtio_io_list and added to this PE). That leak is what makes
		 * offload_engine_stop return NOT_PERMITTED and blocks pe/device destroy
		 * at shutdown after a PF reset. Mirrors the VF-FLR rebuild guard in
		 * io_check_vf_flr_ctx(). */
		if (vblk_ctrl_io_ctx_dma_pe(slot->vf_ctrl, ctx_id) != NULL)
			continue;
		err = vblk_ctrl_init_io_ctx_on_thread(slot->vf_ctrl, ctx_id, pe, cfg->shared_mpool);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("VF[%u] io_ctx[%u] init failed: %s",
				     slot->dynamic_vf_index,
				     ctx_id,
				     doca_error_get_descr(err));
			vblk_ctrl_destroy_io_ctx_on_thread(slot->vf_ctrl, ctx_id);
			for (uint16_t j = 0; j < i; j++) {
				struct vblk_sriov_vf_slot *prev_slot = &sriov->vf_slots[j];

				if (prev_slot->vf_ctrl == NULL)
					continue;
				(void)vblk_ctrl_shutdown_io_ctx_on_thread(prev_slot->vf_ctrl, ctx_id);
				vblk_ctrl_destroy_io_ctx_on_thread(prev_slot->vf_ctrl, ctx_id);
			}
			return err;
		}
	}
	return DOCA_SUCCESS;
}

/* Per IO thread, startup: create this thread's io_ctx for every VF of every PF. */
static doca_error_t io_vf_create_io_ctx_on_thread(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id, struct doca_pe *pe)
{
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		doca_error_t err = io_vf_create_io_ctx_for_ep_on_thread(cfg, ep, ctx_id, pe);

		if (err != DOCA_SUCCESS)
			return err;
	}
	return DOCA_SUCCESS;
}

/* Per IO thread, main loop: progress each enabled VF's io_ctx for this ctx_id
 * (engine progress only on the OE thread, like the PF). */
static void io_vf_progress(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id, bool is_oe)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct vblk_sriov_pf_state *sriov;

		/* Same gate as io_progress_all_eps: skip an EP that is not enabled or
		 * whose io_ctx for this thread has been shut down. On PF unplug/FLR the
		 * IO threads clear io_ctx_created[ctx_id] before the OE frees the VF
		 * datapaths (oe_teardown_vf_dp_for_ep), so once all threads pass the
		 * shutdown barrier none of them is still touching a VF datapath that is
		 * about to be freed - without this gate that is a use-after-free. */
		if (!hp[ep].enabled || !hp[ep].io_ctx_created[ctx_id])
			continue;

		sriov = oe_pf_sriov(cfg, ep);
		if (sriov == NULL || sriov->vf_slots == NULL)
			continue;
		for (uint16_t i = 0; i < sriov->total_vfs; i++) {
			struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
			struct doca_pe *dma_pe;

			if (slot->vf_ctrl == NULL ||
			    atomic_load_explicit(&slot->dp_state, memory_order_acquire) != VBLK_SRIOV_VF_DP_UP)
				continue;
			dma_pe = vblk_ctrl_io_ctx_dma_pe(slot->vf_ctrl, ctx_id);
			if (dma_pe != NULL)
				for (int n = 0; n < VBLK_DMA_PE_PROGRESS_MAX && doca_pe_progress(dma_pe); n++) {}
			if (is_oe && !vblk_poll_skip(vblk_ctrl_engine_poll(slot->vf_ctrl)))
				vblk_ctrl_offload_engine_progress(slot->vf_ctrl);
			if (!vblk_poll_skip(vblk_ctrl_io_ctx_poll(slot->vf_ctrl, ctx_id)))
				vblk_ctrl_io_context_progress(slot->vf_ctrl, ctx_id);
		}
	}
}

/* Per IO thread, shutdown: drain+stop this thread's io_ctx for every VF of one PF. */
static void io_vf_shutdown_io_ctx_for_ep_on_thread(struct vblk_io_ctx_cfg *cfg, uint32_t ep, uint8_t ctx_id)
{
	struct vblk_sriov_pf_state *sriov = oe_pf_sriov(cfg, ep);

	if (sriov == NULL || sriov->vf_slots == NULL)
		return;
	for (uint16_t i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];

		if (slot->vf_ctrl != NULL && vblk_ctrl_io_ctx_dma_pe(slot->vf_ctrl, ctx_id) != NULL)
			(void)vblk_ctrl_shutdown_io_ctx_on_thread(slot->vf_ctrl, ctx_id);
	}
}

/* Per IO thread, shutdown: drain+stop this thread's io_ctx for every VF of every PF. */
static void io_vf_shutdown_io_ctx_on_thread(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id)
{
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++)
		io_vf_shutdown_io_ctx_for_ep_on_thread(cfg, ep, ctx_id);
}

static void io_pf_shutdown_created_io_ctx_on_thread(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id)
{
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++)
		if (cfg->hp_states[ep].io_ctx_created[ctx_id])
			(void)vblk_ctrl_shutdown_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
}

/*
 * OE thread: reconcile each SR-IOV VF slot's datapath against the TLP-thread
 * VFE intent (dp_want_up). VF engines and per-thread io_ctxs are built during
 * startup and progressed by io_vf_progress(); this pass only enables/disables
 * the already-built engine. VF FLR datapath recycle is handled separately.
 */
static void oe_check_vf_dp(struct vblk_io_ctx_cfg *cfg)
{
	if (!cfg->sriov_enabled)
		return;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct vblk_sriov_pf_state *sriov = oe_pf_sriov(cfg, ep);

		if (!atomic_load_explicit(&cfg->hp_states[ep].enabled, memory_order_acquire))
			continue;
		if (sriov == NULL || sriov->vf_slots == NULL)
			continue;

		for (uint16_t i = 0; i < sriov->total_vfs; i++) {
			struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
			bool want_up;
			uint8_t state;

			if (slot->vf_ctrl == NULL)
				continue;
			if (slot->vf_dev != NULL &&
			    atomic_load_explicit(&slot->vf_dev->flr_in_progress, memory_order_acquire))
				continue;

			want_up = atomic_load_explicit(&slot->dp_want_up, memory_order_acquire);
			state = atomic_load_explicit(&slot->dp_state, memory_order_acquire);
			if (state != VBLK_SRIOV_VF_DP_DOWN && state != VBLK_SRIOV_VF_DP_UP)
				continue;
			if (want_up) {
				doca_error_t err;

				if (state != VBLK_SRIOV_VF_DP_DOWN)
					continue;
				err = vblk_ctrl_enable(slot->vf_ctrl);
				if (err != DOCA_SUCCESS) {
					DOCA_LOG_ERR("OE: VF[%u] engine enable failed: %s",
						     slot->dynamic_vf_index,
						     doca_error_get_descr(err));
					continue;
				}
				atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_UP, memory_order_release);
				DOCA_LOG_INFO("OE: VF[%u] datapath enabled (VFE)", slot->dynamic_vf_index);
			} else {
				doca_error_t err;

				if (state != VBLK_SRIOV_VF_DP_UP)
					continue;
				err = vblk_ctrl_disable(slot->vf_ctrl);
				if (err != DOCA_SUCCESS) {
					DOCA_LOG_WARN("OE: VF[%u] engine disable failed: %s",
						      slot->dynamic_vf_index,
						      doca_error_get_descr(err));
					continue;
				}
				atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_DOWN, memory_order_release);
				DOCA_LOG_INFO("OE: VF[%u] datapath disabled (VFE)", slot->dynamic_vf_index);
			}
		}
	}
}

/* Destroy the IO context resources (dma_ctx, vq_io_ctx, dma_pe). Must be called
 * after vblk_ctrl_shutdown_io_ctx_on_thread() and on the managing thread.
 * Shared mpool is owned by the application and is NOT destroyed. */
static void vblk_ctrl_destroy_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id)
{
	if (ctrl == NULL || ctx_id >= ctrl->num_io_ctx)
		return;
	vblk_ctrl_io_ctx_reset(&ctrl->io_ctxs[ctx_id], false);
}

/*
 * IO thread (all threads including OE): if our own io_ctx was marked
 * need_flr_teardown by the OE, shutdown io_ctx + dma_ctx on this thread and
 * clear the flag.
 */
static void io_check_flr_teardown(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct vblk_ctrl_io_ctx *io_ctx = &cfg->ctrls[ep].io_ctxs[ctx_id];

		if (!hp[ep].io_ctx_created[ctx_id])
			continue;

		if (!atomic_load_explicit(&io_ctx->need_flr_teardown, memory_order_acquire))
			continue;
		DOCA_LOG_INFO("IO thread %u: handling FLR shutdown for EP %u", ctx_id, ep);
		(void)vblk_ctrl_shutdown_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
		vblk_ctrl_destroy_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);

		hp[ep].io_ctx_created[ctx_id] = false;
		hp[ep].io_ctx_flr_destroyed++;
	}
}

/*
 * OE thread: drive VF FLR through the same order as PF FLR: signal per-thread
 * io_ctx teardown, recycle the offload engine, then re-enable after all io_ctxs
 * have been rebuilt.
 */
static void oe_vf_flr_enter_teardown(struct vblk_sriov_vf_slot *slot, struct vblk_ctrl *ctrl)
{
	atomic_store_explicit(&slot->flr_io_ctx_done, 0, memory_order_release);
	atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_FLR_TEARDOWN, memory_order_release);
	for (uint8_t ctx_id = 0; ctx_id < ctrl->num_io_ctx; ctx_id++)
		atomic_store_explicit(&ctrl->io_ctxs[ctx_id].need_flr_teardown, true, memory_order_release);
}

static void oe_vf_flr_handle_ready_state(struct vblk_sriov_vf_slot *slot,
					 struct vblk_ctrl *ctrl,
					 struct vblk_pci_virtio_dev *vf_dev)
{
	if (!atomic_exchange_explicit(&vf_dev->flr_kickoff, false, memory_order_acq_rel))
		return;

	DOCA_LOG_INFO("OE: VF[%u] FLR detected - signalling VF IO ctxs to stop", slot->dynamic_vf_index);
	oe_vf_flr_enter_teardown(slot, ctrl);
}

static void oe_vf_flr_handle_teardown_state(struct vblk_sriov_vf_slot *slot,
					    struct vblk_ctrl *ctrl,
					    struct vblk_pci_virtio_dev *vf_dev)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	doca_error_t err;

	if (ctrl->vq_engine == NULL)
		return;
	if (atomic_load_explicit(&slot->flr_io_ctx_done, memory_order_acquire) < ctrl->num_io_ctx)
		return;

	/* A second VF FLR that arrives before the OE recycle is covered by this
	 * recycle. Keep flr_in_progress set until the rebuild tail releases it. */
	(void)atomic_exchange_explicit(&vf_dev->flr_kickoff, false, memory_order_acq_rel);

	DOCA_LOG_INFO("OE: all VF[%u] IO ctx destroyed (FLR) - recycle VF offload engine", slot->dynamic_vf_index);

	err = vblk_ctrl_recycle_vq_backends(ctrl);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("VF[%u]: backend VQ recycle failed: %s - leaving recycle pending",
			     slot->dynamic_vf_index,
			     doca_error_get_descr(err));
		return;
	}

	virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
	if (ctrl->engine_enabled) {
		err = doca_devemu_virtio_offload_engine_disable(virtio_engine);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_WARN("VF[%u]: offload engine disable failed: %s",
				      slot->dynamic_vf_index,
				      doca_error_get_descr(err));
			return;
		}
		ctrl->engine_enabled = false;
	}

	err = doca_devemu_virtio_offload_engine_stop(virtio_engine);
	if (err != DOCA_SUCCESS && err != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_WARN("VF[%u]: offload engine stop failed: %s",
			      slot->dynamic_vf_index,
			      doca_error_get_descr(err));
		return;
	}

	/* New FW invalidates the VF mkey and suspends the VF GVMI MSI-X on FLR;
	 * only a TLP-object recycle re-establishes MSI-X delivery. */
	err = vblk_pci_sriov_vf_tlp_dev_suspend(slot);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_WARN("VF[%u]: TLP suspend failed: %s - leaving recycle pending",
			      slot->dynamic_vf_index,
			      doca_error_get_descr(err));
		return;
	}
	err = vblk_pci_sriov_vf_tlp_dev_resume(slot);
	if (err != DOCA_SUCCESS) {
		/* Expected while FW invalidation is in flight; retried next iteration. */
		DOCA_LOG_DBG("VF[%u]: TLP resume pending: %s", slot->dynamic_vf_index, doca_error_get_descr(err));
		return;
	}

	err = doca_devemu_virtio_offload_engine_start(virtio_engine);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("VF[%u]: offload engine start failed: %s - leaving recycle pending",
			     slot->dynamic_vf_index,
			     doca_error_get_descr(err));
		return;
	}

	atomic_store_explicit(&slot->flr_io_ctx_done, 0, memory_order_release);
	atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_FLR_REBUILD, memory_order_release);
}

static bool vblk_flr_complete_device_status(struct vblk_pci_virtio_dev *dev)
{
	uint8_t restored_status;

	/* If Linux already restored status bits while FLR was in progress,
	 * preserve them and only remove a stale NEEDS_RESET bit. If there are no
	 * restored bits, release reset completion as status 0. */
	restored_status = dev->pci_cfg.device_status & (uint8_t)~VBLK_PCI_VIRTIO_DEVICE_STATUS_NEEDS_RESET;
	dev->pci_cfg.device_status = restored_status;
	atomic_store_explicit(&dev->flr_in_progress, false, memory_order_release);

	return (restored_status & VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER_OK) != 0;
}

static void oe_vf_flr_handle_rebuild_state(struct vblk_sriov_vf_slot *slot,
					   struct vblk_ctrl *ctrl,
					   struct vblk_pci_virtio_dev *vf_dev)
{
	bool want_up;
	bool driver_ok;

	if (ctrl->vq_engine == NULL)
		return;
	if (atomic_load_explicit(&slot->flr_io_ctx_done, memory_order_acquire) < ctrl->num_io_ctx)
		return;
	if (atomic_exchange_explicit(&vf_dev->flr_kickoff, false, memory_order_acq_rel)) {
		DOCA_LOG_INFO("OE: VF[%u] additional FLR detected during rebuild - restarting teardown",
			      slot->dynamic_vf_index);
		oe_vf_flr_enter_teardown(slot, ctrl);
		return;
	}

	want_up = atomic_load_explicit(&slot->dp_want_up, memory_order_acquire);
	if (want_up) {
		doca_error_t err = vblk_ctrl_enable(ctrl);

		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("VF[%u]: offload engine enable failed: %s - leaving recycle pending",
				     slot->dynamic_vf_index,
				     doca_error_get_descr(err));
			return;
		}
		atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_UP, memory_order_release);
	} else {
		ctrl->engine_enabled = false;
		atomic_store_explicit(&slot->dp_state, VBLK_SRIOV_VF_DP_DOWN, memory_order_release);
	}

	atomic_store_explicit(&slot->flr_io_ctx_done, 0, memory_order_release);
	DOCA_LOG_INFO("VF[%u] FLR completed: VF datapath recycled%s",
		      slot->dynamic_vf_index,
		      want_up ? " and enabled" : "");
	ctrl->last_device_status = 0;
	driver_ok = vblk_flr_complete_device_status(vf_dev);
	if (want_up && driver_ok)
		(void)vblk_pci_virtio_cfg_change_notify(vf_dev);
}

static void oe_check_vf_flr(struct vblk_io_ctx_cfg *cfg)
{
	if (!cfg->sriov_enabled)
		return;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct vblk_sriov_pf_state *sriov = oe_pf_sriov(cfg, ep);

		if (!atomic_load_explicit(&cfg->hp_states[ep].enabled, memory_order_acquire))
			continue;
		if (sriov == NULL || sriov->vf_slots == NULL)
			continue;
		for (uint16_t i = 0; i < sriov->total_vfs; i++) {
			struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
			struct vblk_pci_virtio_dev *vf_dev = slot->vf_dev;
			struct vblk_ctrl *ctrl = slot->vf_ctrl;
			uint8_t state;

			if (vf_dev == NULL || ctrl == NULL)
				continue;

			state = atomic_load_explicit(&slot->dp_state, memory_order_acquire);
			switch (state) {
			case VBLK_SRIOV_VF_DP_DOWN:
			case VBLK_SRIOV_VF_DP_UP:
				oe_vf_flr_handle_ready_state(slot, ctrl, vf_dev);
				break;
			case VBLK_SRIOV_VF_DP_FLR_TEARDOWN:
				oe_vf_flr_handle_teardown_state(slot, ctrl, vf_dev);
				break;
			case VBLK_SRIOV_VF_DP_FLR_REBUILD:
				oe_vf_flr_handle_rebuild_state(slot, ctrl, vf_dev);
				break;
			}
		}
	}
}

/*
 * IO thread: handle this thread's VF io_ctx work for VF FLR.
 * During teardown it drains and destroys the ctx; during rebuild it recreates
 * the ctx after the OE has restarted the VF offload engine.
 */
static void io_check_vf_flr_ctx(struct vblk_io_ctx_cfg *cfg, uint8_t ctx_id, struct doca_pe *pe)
{
	if (!cfg->sriov_enabled)
		return;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		struct vblk_sriov_pf_state *sriov = oe_pf_sriov(cfg, ep);

		/* Hot-unplug sets enabled=false before the OE tears down and frees
		 * this PF's SR-IOV state. Do not let IO threads walk vf_slots while
		 * the OE is allowed to free them. PF/VF FLR keeps the PF enabled, so
		 * recycle handling is unaffected. */
		if (!atomic_load_explicit(&cfg->hp_states[ep].enabled, memory_order_acquire))
			continue;
		if (sriov == NULL || sriov->vf_slots == NULL)
			continue;
		for (uint16_t i = 0; i < sriov->total_vfs; i++) {
			struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
			struct vblk_ctrl *ctrl = slot->vf_ctrl;
			struct vblk_ctrl_io_ctx *io_ctx;
			uint8_t state;
			doca_error_t err;

			if (ctrl == NULL || ctx_id >= ctrl->num_io_ctx)
				continue;

			state = atomic_load_explicit(&slot->dp_state, memory_order_acquire);
			if (state == VBLK_SRIOV_VF_DP_FLR_TEARDOWN) {
				io_ctx = &ctrl->io_ctxs[ctx_id];
				if (!atomic_load_explicit(&io_ctx->need_flr_teardown, memory_order_acquire))
					continue;

				DOCA_LOG_INFO("IO thread %u: handling VF[%u] FLR shutdown",
					      ctx_id,
					      slot->dynamic_vf_index);
				(void)vblk_ctrl_shutdown_io_ctx_on_thread(ctrl, ctx_id);
				vblk_ctrl_destroy_io_ctx_on_thread(ctrl, ctx_id);
				atomic_fetch_add_explicit(&slot->flr_io_ctx_done, 1, memory_order_acq_rel);
				continue;
			}

			if (state != VBLK_SRIOV_VF_DP_FLR_REBUILD)
				continue;
			if (vblk_ctrl_io_ctx_dma_pe(ctrl, ctx_id) != NULL)
				continue;

			err = vblk_ctrl_init_io_ctx_on_thread(ctrl, ctx_id, pe, cfg->shared_mpool);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("VF[%u] FLR io_ctx[%u] init failed: %s",
					     slot->dynamic_vf_index,
					     ctx_id,
					     doca_error_get_descr(err));
				vblk_ctrl_destroy_io_ctx_on_thread(ctrl, ctx_id);
				continue;
			}
			atomic_fetch_add_explicit(&slot->flr_io_ctx_done, 1, memory_order_acq_rel);
		}
	}
}

/*
 * OE thread: when all IO threads have torn down their io_ctxs, recycle the
 * offload engine through disable -> stop -> start and arm io_ctx re-creation.
 *
 * The engine "enable" step is deferred to oe_check_post_flr_reenable() because
 * doca_devemu_vblk_io_create_from_offload_engine() (invoked when IO threads
 * rebuild their io_ctxs) must run while the engine is in STARTED, not
 * ENABLED. The documented init order is: start engine -> bind io_ctxs ->
 * enable engine. vq_engine and vblk_pci_virtio_dev are preserved across FLR;
 * only the per-thread io_ctxs are recycled.
 */
static void oe_check_post_flr_restart(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_post_flr_oe_restart)
			continue;
		if (hp[ep].io_ctx_flr_destroyed < cfg->num_io_ctx)
			continue;

		struct vblk_ctrl *ctrl = &cfg->ctrls[ep];

		if (ctrl->vq_engine == NULL || ctrl->vblk_pci_dev == NULL)
			continue;
		struct doca_devemu_virtio_offload_engine *virtio_engine =
			doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
		doca_error_t err;

		DOCA_LOG_INFO("OE: all IO ctx destroyed for EP %u (FLR) — recycle offload engine", ep);

		if (ctrl->engine_enabled) {
			err = vblk_ctrl_disable(ctrl);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_WARN("EP %u: offload engine disable failed: %s",
					      ep,
					      doca_error_get_descr(err));
				continue;
			}
		}
		err = doca_devemu_virtio_offload_engine_stop(virtio_engine);
		if (err != DOCA_SUCCESS && err != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_WARN("EP %u: offload engine stop failed: %s", ep, doca_error_get_descr(err));
			continue;
		}

		err = vblk_pci_pf_tlp_dev_suspend(ctrl->vblk_pci_dev);
		if (err != DOCA_SUCCESS)
			continue; /* keep need_post_flr_oe_restart; retry next iteration */
		err = vblk_pci_pf_tlp_dev_resume(ctrl->vblk_pci_dev);
		if (err != DOCA_SUCCESS) {
			/* Expected while FW invalidation is in flight. */
			DOCA_LOG_DBG("EP %u: PF TLP resume pending: %s", ep, doca_error_get_descr(err));
			continue;
		}

		err = doca_devemu_virtio_offload_engine_start(virtio_engine);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EP %u: offload engine start failed: %s — leaving FLR pending",
				     ep,
				     doca_error_get_descr(err));
			continue; /* keep need_post_flr_oe_restart set; retry next iteration */
		}

		hp[ep].io_ctx_ready = 0;
		hp[ep].io_ctx_flr_destroyed = 0;
		hp[ep].need_post_flr_oe_restart = false;
		hp[ep].need_io_ctx = true;
		hp[ep].need_post_flr_reenable = true;
	}
}

/*
 * OE thread: once all IO threads have rebuilt their io_ctxs (io_ctx_ready ==
 * num_io_ctx), the engine has the bindings it needs to accept events. Enable
 * the engine and surface the device back to the host.
 */
static void oe_check_post_flr_reenable(struct vblk_io_ctx_cfg *cfg)
{
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;

	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!hp[ep].need_post_flr_reenable)
			continue;
		if (hp[ep].io_ctx_ready < cfg->num_io_ctx)
			continue;

		struct vblk_ctrl *ctrl = &cfg->ctrls[ep];
		if (ctrl->vq_engine == NULL || ctrl->vblk_pci_dev == NULL)
			continue;
		doca_error_t err;

		err = vblk_ctrl_enable(ctrl);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EP %u: offload engine enable failed: %s — leaving FLR pending",
				     ep,
				     doca_error_get_descr(err));
			continue; /* keep need_post_flr_reenable set; retry next iteration */
		}

		ctrl->last_device_status = 0;
		hp[ep].need_io_ctx = false;
		hp[ep].need_post_flr_reenable = false;

		if (vblk_flr_complete_device_status(ctrl->vblk_pci_dev))
			(void)vblk_pci_virtio_cfg_change_notify(ctrl->vblk_pci_dev);
		DOCA_LOG_INFO("FLR completed for EP %u: OE enabled after io_ctx rebind", ep);
	}
}

/* App-shutdown drain: tear down every PF's VF datapaths before the PF
 * controllers are cleaned up (see oe_teardown_vf_dp_for_ep). */
static void oe_teardown_all_vf_dp(struct vblk_io_ctx_cfg *cfg)
{
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++)
		oe_teardown_vf_dp_for_ep(cfg, ep);
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
	/* Declared here so error-path 'goto out / goto shutdown' before the main
	 * loop don't skip past its initializer (Coverity PW.BRANCH_PAST_INIT). */
	uint64_t oe_loop_iter = 0;

	/*
	 * Static mode: OE thread creates all controllers at startup.
	 * Hotplug mode: skip, controllers created dynamically via plug commands.
	 */
	if (is_oe && !hotplug) {
		for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
			err = plug_ep_ctrl(cfg, ep);
			if (err != DOCA_SUCCESS) {
				for (uint32_t j = 0; j < ep; j++)
					vblk_ctrl_cleanup(&cfg->ctrls[j], pe);
				ops->signal_ctrl_created(err, NULL);
				goto out;
			}
		}
		/* Build each PF's SR-IOV VF engines (no io_ctx, disabled). The
		 * per-thread io_ctxs are created below by every IO thread. */
		err = oe_build_vf_engines(cfg);
		if (err != DOCA_SUCCESS) {
			/* Free any VF engines built before the failure (this is the OE
			 * thread, the engines' PE owner) before tearing down the PFs. */
			oe_teardown_all_vf_dp(cfg);
			for (uint32_t j = 0; j < cfg->num_ep; j++)
				vblk_ctrl_cleanup(&cfg->ctrls[j], pe);
			ops->signal_ctrl_created(err, NULL);
			goto out;
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
				io_pf_shutdown_created_io_ctx_on_thread(cfg, ctx_id);
				if (is_oe) {
					ops->signal_ctrl_created(err, NULL);
					oe_teardown_all_vf_dp(cfg);
					for (uint32_t j = 0; j < cfg->num_ep; j++)
						vblk_ctrl_cleanup(&cfg->ctrls[j], pe);
				}
				ops->signal_io_ready();
				goto out;
			}
			cfg->hp_states[ep].io_ctx_created[ctx_id] = true;
			cfg->hp_states[ep].enabled = true;
		}
		/* This thread also creates its io_ctx for every VF of every PF (the VF
		 * engines exist from oe_build_vf_engines). VF engines stay DISABLED until
		 * VFE 0->1 (oe_check_vf_dp), so no per-VF enable flag is set here. */
		if (cfg->sriov_enabled) {
			err = io_vf_create_io_ctx_on_thread(cfg, ctx_id, pe);
			if (err != DOCA_SUCCESS) {
				force_quit = true;
				if (is_oe) {
					ops->signal_ctrl_created(err, NULL);
					ops->signal_io_ready();
					goto shutdown;
				}
				/* Non-OE threads do not run the common shutdown tail.
				 * Drain this thread's already-created VF + PF io_ctxs;
				 * the OE thread performs final VF/PF cleanup. */
				io_vf_shutdown_io_ctx_on_thread(cfg, ctx_id);
				io_pf_shutdown_created_io_ctx_on_thread(cfg, ctx_id);
				ops->signal_io_ready();
				goto out;
			}
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
			oe_check_unplug_requests(cfg);
			oe_check_host_power_off(cfg);
			oe_check_unplug_complete(cfg);

			if (++oe_loop_iter % VBLK_OE_BACKOFF_ITERS == 0)
				usleep(VBLK_OE_BACKOFF_US);
		}

		if (doca_unlikely(is_oe)) {
			oe_check_flr_kickoff(cfg);
			oe_check_vf_flr(cfg);
		}

		io_check_need_io_ctx(cfg, ctx_id, pe);
		io_check_need_io_ctx_shutdown(cfg, ctx_id);
		io_check_flr_teardown(cfg, ctx_id);
		io_check_vf_flr_ctx(cfg, ctx_id, pe);

		if (doca_unlikely(is_oe)) {
			oe_check_post_flr_restart(cfg);
			oe_check_post_flr_reenable(cfg);
			oe_check_vf_flr(cfg);
			oe_check_vf_dp(cfg);
		}

		io_progress_all_eps(cfg, ctx_id, is_oe);
		io_vf_progress(cfg, ctx_id, is_oe);

		for (uint32_t i = 0; i < VBLK_IO_PE_PROGRESS_PER_EP && doca_pe_progress(pe); i++) {}
	}

shutdown:
	/* Shutdown: stop IO ctxs for EPs still active on this thread. Drain+stop
	 * each PF's VF io_ctxs before its PF io_ctx. Hot-unplug already clears
	 * io_ctx_created, so those VFs must not be shut down again here. */
	for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
		if (!cfg->hp_states[ep].io_ctx_created[ctx_id])
			continue;
		if (cfg->sriov_enabled)
			io_vf_shutdown_io_ctx_for_ep_on_thread(cfg, ep, ctx_id);
		(void)vblk_ctrl_shutdown_io_ctx_on_thread(&cfg->ctrls[ep], ctx_id);
	}

	if (is_oe) {
		ops->wait_all_io_exited((uint8_t)(cfg->num_io_ctx > 0 ? cfg->num_io_ctx - 1 : 0));
		ops->wait_pci_stopped();
		/* Tear down VF datapaths before vblk_ctrl_cleanup destroys the VF
		 * TLP devices (via free_pf_state) - each datapath references the ep. */
		oe_teardown_all_vf_dp(cfg);
		for (uint32_t ep = 0; ep < cfg->num_ep; ep++) {
			if (cfg->hp_states[ep].enabled)
				vblk_ctrl_cleanup(&cfg->ctrls[ep], pe);
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
