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

#ifndef VBLK_IO_CTX_LU_H_
#define VBLK_IO_CTX_LU_H_

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <doca_error.h>
#include "vblk_ctrl_lu.h"

/* Callbacks for app-level coordination (state machine stays in vblk_app) */
struct vblk_io_ctx_app_ops {
	void (*signal_ctrl_created)(doca_error_t err, struct vblk_ctrl *ctrl);
	void (*signal_io_ready)(void);
	void (*signal_ctrl_enabled)(doca_error_t err);
	void (*signal_io_exited)(void);
	doca_error_t (*wait_ctrl_created)(struct vblk_ctrl **ctrl_out);
	doca_error_t (*wait_io_ready)(void);
	doca_error_t (*wait_pre_enable)(void);
	void (*wait_all_io_exited)(uint8_t expected);
};

struct vblk_io_ctx_cfg {
	struct doca_pe *pe;
	/*
	 * Multi-device: vblk_ctrl, ep are per-device.
	 */
	struct vblk_ctrl *ctrl;
	struct doca_devemu_pci_ep *ep;
	uint8_t ctx_id;
	uint8_t affinity_core;
	uint8_t offload_engine_core_idx;
	uint8_t num_io_ctx;
	uint16_t num_queues;
	uint16_t seg_max;
	bool indirect_enabled;
	uint32_t stats_ios_period;
	const char *shm_dir_path;
	const struct vblk_io_ctx_app_ops *ops;
	void (*ipc_poll)(void *arg); /* OE thread: drain IPC messages */
	void *ipc_poll_arg;
};

/* Create and start IO context thread */
doca_error_t vblk_io_ctx_thread_create(struct vblk_io_ctx_cfg *cfg, pthread_t *thread);

#endif /* VBLK_IO_CTX_LU_H_ */
