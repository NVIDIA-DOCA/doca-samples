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

#ifndef VBLK_HANDOVER_H_
#define VBLK_HANDOVER_H_

#include <stdbool.h>
#include <stdint.h>
#include <doca_error.h>
#include "vblk_ipc.h"

struct vblk_lu_device_cfg;

/*
 * Consider: Route all LU IPC through SRC to prevent races: TLP→SRC→DST until
 * SRC is disabled, then DST reattaches directly to TLP.  Currently DST
 * sends INITIATE_ACK and DEVICE_ACK directly to TLP which could race with
 * state updates.
 */

#define VBLK_LU_TRIGGER_PATH "/tmp/vblk_lu_trigger"

enum vblk_ho_role {
	VBLK_HO_ROLE_SRC = 1,
	VBLK_HO_ROLE_DST,
};

enum vblk_ho_phase {
	VBLK_HO_IDLE = 0,
	VBLK_HO_SETUP,	      /* global: SETUP/SETUP_ACK */
	VBLK_HO_WAIT_DEVICE,  /* DST: waiting for HANDOVER_DEVICE from IO-Engine */
	VBLK_HO_GET_STATE,    /* per-device: GET_STATE/GET_STATE_ACK */
	VBLK_HO_SWITCHOVER,   /* per-device: BEGIN/BEGIN_ACK */
	VBLK_HO_SRC_DRAINING, /* SRC: engine disabling async */
	VBLK_HO_DST_RESUMING, /* DST: waiting for VQs to reach RUNNING */
	VBLK_HO_CLEANUP,      /* per-device: END/END_ACK */
	VBLK_HO_DONE,
	VBLK_HO_FAILED,
};

struct vblk_ho_ctx {
	enum vblk_ho_role role;
	enum vblk_ho_phase phase;
	uint16_t current_device_id;

	struct vblk_ctrl *ctrl;
	struct vblk_ipc_ep *ipc;
	const char *peer_path;

	/* SRC: produced by export() at startup, forwarded to DST on GET_STATE.
	 * DST: received from SRC via GET_STATE_ACK. */
	void *export_desc;
	size_t export_desc_len;

	/* DST: device configuration state received from SRC */
	struct vblk_lu_device_cfg *initial_cfg;
};

void vblk_ho_init(struct vblk_ho_ctx *ho, enum vblk_ho_role role, struct vblk_ctrl *ctrl, struct vblk_ipc_ep *ipc);

doca_error_t vblk_ho_dst_get_initial_state(struct vblk_ho_ctx *ho);
doca_error_t vblk_ho_dst_begin_switchover(struct vblk_ho_ctx *ho);

bool vblk_ho_process_msg(struct vblk_ho_ctx *ho, uint32_t type, const void *payload, uint32_t len);

void vblk_ho_poll(struct vblk_ho_ctx *ho);

/**
 * Store a copy of an export descriptor blob.
 * The caller is responsible for releasing it via vblk_export_desc_release().
 */
doca_error_t vblk_export_desc_store(void **destp, size_t *lenp, const void *src, size_t src_len);

/**
 * Release an export descriptor: unlink the SHM file it describes
 * and free the buffer.  No-op if *destp is NULL.
 * Returns the result of export_release (DOCA_SUCCESS, DOCA_ERROR_NOT_FOUND
 * on harmless race, or another error on real failure).
 */
doca_error_t vblk_export_desc_release(void **destp, size_t *lenp);

/**
 * Free an export descriptor buffer without unlinking the SHM.
 * Used on the DST side where the SHM is owned by the SRC.
 */
void vblk_export_desc_free(void **destp, size_t *lenp);

static inline bool vblk_ho_is_done(const struct vblk_ho_ctx *ho)
{
	return ho->phase == VBLK_HO_DONE;
}
static inline bool vblk_ho_is_failed(const struct vblk_ho_ctx *ho)
{
	return ho->phase == VBLK_HO_FAILED;
}

#endif /* VBLK_HANDOVER_H_ */
