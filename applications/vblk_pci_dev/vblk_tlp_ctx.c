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
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <doca_pe.h>
#include <doca_log.h>

#include "vblk_tlp_ctx.h"
#include "vblk_pci.h"
#include "vblk_ctrl.h"
#include "vblk_pci_dev_core.h"

DOCA_LOG_REGISTER(VBLK_TLP_CTX);

#define VBLK_TLP_CTX_MAX_WAIT_ITERATIONS_IDLE (1000000)

static bool stdin_has_input(void)
{
	struct pollfd pfd = {.fd = 0, .events = POLLIN};

	return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

static doca_error_t handle_stdin_command(bool hotplug, uint32_t num_ep, struct vblk_ep_hotplug_state *hp)
{
	char line[128];
	char cmd[32];
	int val;

	if (fgets(line, sizeof(line), stdin) == NULL) {
		clearerr(stdin);
		return DOCA_ERROR_AGAIN;
	}

	if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
		return DOCA_ERROR_AGAIN;

	if (sscanf(line, "%31s %d", cmd, &val) != 2)
		return DOCA_ERROR_INVALID_VALUE;

	if (strcmp(cmd, "cap") == 0) {
		if (val <= 0) {
			DOCA_LOG_ERR("Capacity must be greater than 0 GB");
			return DOCA_ERROR_INVALID_VALUE;
		}
		uint64_t capacity_bytes = (uint64_t)val * 1024UL * 1024UL * 1024UL;

		DOCA_LOG_INFO("Setting capacity: %d GB", val);
		vblk_pci_set_capacity(capacity_bytes);
		vblk_pci_notify_host();
		return DOCA_SUCCESS;
	}

	if (!hotplug) {
		DOCA_LOG_WARN("Unknown command '%s'. Available: cap <GB>", cmd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (val < 0 || (uint32_t)val >= num_ep) {
		DOCA_LOG_ERR("DSP index %d out of range [0, %u)", val, num_ep);
		return DOCA_ERROR_INVALID_VALUE;
	}

	uint32_t dsp_idx = (uint32_t)val;

	if (strcmp(cmd, "plug") == 0) {
		if (hp[dsp_idx].enabled || hp[dsp_idx].plug_requested) {
			DOCA_LOG_WARN("EP %u already plugged or pending", dsp_idx);
		} else {
			DOCA_LOG_INFO("Queuing plug for DSP[%u] to OE thread", dsp_idx);
			hp[dsp_idx].plug_requested = true;
		}
		return DOCA_SUCCESS;
	}

	if (strcmp(cmd, "unplug") == 0) {
		if (!hp[dsp_idx].enabled) {
			DOCA_LOG_WARN("EP %u not plugged", dsp_idx);
		} else if (hp[dsp_idx].unplug_requested) {
			DOCA_LOG_WARN("EP %u already has a pending unplug", dsp_idx);
		} else {
			hp[dsp_idx].unplug_requested = true;
			DOCA_LOG_INFO("Requesting unplug for DSP[%u]", dsp_idx);
		}
		return DOCA_SUCCESS;
	}

	DOCA_LOG_WARN("Unknown command '%s'. Available: cap, plug, unplug", cmd);
	return DOCA_ERROR_INVALID_VALUE;
}

static void *vblk_tlp_ctx_thread(void *arg)
{
	struct vblk_tlp_ctx_cfg *cfg = arg;
	struct doca_pe *pe = cfg->pe;
	struct doca_ctx *pci_ctx = vblk_pci_ev_channel_ctx();
	struct vblk_ep_hotplug_state *hp = cfg->hp_states;
	bool hotplug = cfg->hotplug_mode;
	uint32_t num_ep = cfg->num_ep;

	if (hotplug) {
		DOCA_LOG_INFO("Hotplug control (enter commands):");
		DOCA_LOG_INFO("  plug <DSP_IDX>   - Plug device to DSP slot");
		DOCA_LOG_INFO("  unplug <DSP_IDX> - Unplug device from DSP slot");
		DOCA_LOG_INFO("Example: plug 0");
	}

	while (!force_quit) {
		(void)doca_pe_progress(pe);
		vblk_pci_tlp_poll();

		if (stdin_has_input())
			handle_stdin_command(hotplug, num_ep, hp);
	}

	vblk_pci_stop();
	(void)vblk_wait_ctx_idle(pe, pci_ctx, "vblk_pci", VBLK_TLP_CTX_MAX_WAIT_ITERATIONS_IDLE);
	cfg->signal_pci_stopped();

	return NULL;
}

doca_error_t vblk_pci_tlp_thread_create(struct vblk_tlp_ctx_cfg *cfg, pthread_t *thread, uint8_t affinity_core)
{
	doca_error_t err;
	pthread_attr_t attr;
	cpu_set_t cpus;
	struct doca_ctx *pci_ctx;
	int rc;

	err = doca_pe_connect_ctx(cfg->pe, vblk_pci_ev_channel_ctx());
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to attach vblk_pci context to the progress engine");
		return err;
	}

	err = vblk_pci_tlp_start();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("failed to start vblk_pci");
		return err;
	}

	rc = pthread_attr_init(&attr);
	if (rc != 0) {
		err = DOCA_ERROR_INITIALIZATION;
		goto stop_ctx;
	}

	CPU_ZERO(&cpus);
	CPU_SET(affinity_core, &cpus);
	rc = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
	if (rc != 0)
		DOCA_LOG_WARN("Failed to set TLP thread affinity to core %u (rc=%d)", affinity_core, rc);

	rc = pthread_create(thread, &attr, vblk_tlp_ctx_thread, cfg);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		DOCA_LOG_ERR("Failed to create TLP progress thread");
		err = DOCA_ERROR_INITIALIZATION;
		goto stop_ctx;
	}

	DOCA_LOG_DBG("thread_created type=tlp affinity_core=%u", affinity_core);
	return DOCA_SUCCESS;

stop_ctx:
	pci_ctx = vblk_pci_ev_channel_ctx();
	vblk_pci_stop();
	(void)vblk_wait_ctx_idle(cfg->pe, pci_ctx, "vblk_pci", VBLK_TLP_CTX_MAX_WAIT_ITERATIONS_IDLE);
	return err;
}
