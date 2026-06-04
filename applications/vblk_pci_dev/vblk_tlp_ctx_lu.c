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

#include "vblk_tlp_ctx_lu.h"
#include "vblk_pci_lu.h"
#include "vblk_pci_dev_core_lu.h"

DOCA_LOG_REGISTER(VBLK_TLP_CTX);

#define VBLK_TLP_CTX_MAX_WAIT_ITERATIONS_IDLE (1000000)

static void *vblk_tlp_ctx_thread(void *arg)
{
	struct doca_pe *pe = arg;
	struct doca_ctx *pci_ctx = vblk_pci_ev_channel_ctx();

	while (!force_quit)
		(void)doca_pe_progress(pe);

	vblk_pci_stop();
	(void)vblk_wait_ctx_idle(pe, pci_ctx, "vblk_pci", VBLK_TLP_CTX_MAX_WAIT_ITERATIONS_IDLE);

	return NULL;
}

doca_error_t vblk_pci_tlp_thread_create(struct doca_pe *pe, pthread_t *thread, uint8_t affinity_core)
{
	doca_error_t err;
	pthread_attr_t attr;
	cpu_set_t cpus;
	struct doca_ctx *pci_ctx;
	int rc;

	err = doca_pe_connect_ctx(pe, vblk_pci_ev_channel_ctx());
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

	rc = pthread_create(thread, &attr, vblk_tlp_ctx_thread, pe);
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
	(void)vblk_wait_ctx_idle(pe, pci_ctx, "vblk_pci", VBLK_TLP_CTX_MAX_WAIT_ITERATIONS_IDLE);
	return err;
}
