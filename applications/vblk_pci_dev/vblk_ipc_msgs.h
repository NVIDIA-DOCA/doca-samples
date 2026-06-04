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

#ifndef VBLK_IPC_MSGS_H_
#define VBLK_IPC_MSGS_H_

#include <stdint.h>
#include <stddef.h>

/* ─── Well-known endpoint paths ──────────────────────────────────── */

#define VBLK_IPC_TLP_PATH "/tmp/vblk_ipc_tlp"
#define VBLK_IPC_EMU_SRC_PATH "/tmp/vblk_ipc_emu_src"
#define VBLK_IPC_EMU_DST_PATH "/tmp/vblk_ipc_emu_dst"

/* ═══════════════════════════════════════════════════════════════════
 * State forwarding  (TLP ↔ EMU)
 * ═══════════════════════════════════════════════════════════════════ */

enum {
	VBLK_MSG_STATE_UPDATE = 0x001,	 /* TLP → EMU: state snapshot (push or response) */
	VBLK_MSG_STATE_REQUEST = 0x002,	 /* EMU → TLP: request current state */
	VBLK_MSG_EXPORT_DESC = 0x003,	 /* EMU → TLP: export descriptor blob (after export) */
	VBLK_MSG_RECOVERY_STATE = 0x004, /* TLP → EMU: export descriptor for recovery (may be empty) */
};

/* ═══════════════════════════════════════════════════════════════════
 * Live-update handover  (SRC ↔ DST ↔ IO-Engine)
 * ═══════════════════════════════════════════════════════════════════ */

enum {
	/* Global setup (DST ↔ SRC) */
	VBLK_MSG_HO_SETUP = 0x101,
	VBLK_MSG_HO_SETUP_ACK = 0x102,

	/* IO-Engine → DST: migrate next device */
	VBLK_MSG_HO_DEVICE = 0x110,
	VBLK_MSG_HO_DEVICE_ACK = 0x111,
	VBLK_MSG_HO_DEVICE_NACK = 0x112,

	/* DST → IO-Engine: global setup done */
	VBLK_MSG_HO_INITIATE_ACK = 0x113,

	/* Per-device: initial state (DST → SRC) */
	VBLK_MSG_HO_GET_STATE = 0x120,
	VBLK_MSG_HO_GET_STATE_ACK = 0x121,
	VBLK_MSG_HO_GET_STATE_NACK = 0x122,

	/* Per-device: switchover (DST → SRC) */
	VBLK_MSG_HO_BEGIN = 0x130,
	VBLK_MSG_HO_BEGIN_ACK = 0x131,
	VBLK_MSG_HO_BEGIN_NACK = 0x132,

	/* Per-device: cleanup (DST → SRC) */
	VBLK_MSG_HO_END = 0x140,
	VBLK_MSG_HO_END_ACK = 0x141,
	VBLK_MSG_HO_END_NACK = 0x142,
};

/* ─── Payload structs ────────────────────────────────────────────── */

/** SETUP_ACK: global handover configuration from SRC.
 *  PCI TLP type config is implicitly shared (same binary + config).
 *  Device list is currently limited to a single device. */
struct vblk_msg_ho_setup_ack {
	uint16_t num_devices;
};

struct vblk_msg_ho_device {
	uint16_t device_id;
};

/** GET_STATE_ACK: runtime state + configuration state.
 *  Layout: header | export_desc blob | vblk_lu_device_cfg */
struct vblk_msg_ho_get_state_ack {
	uint32_t export_desc_len;
	uint32_t device_cfg_len;
	uint8_t data[];
};

static inline size_t vblk_msg_get_state_ack_size(uint32_t desc_len, uint32_t cfg_len)
{
	return sizeof(struct vblk_msg_ho_get_state_ack) + desc_len + cfg_len;
}

static inline const void *vblk_msg_get_state_ack_desc(const struct vblk_msg_ho_get_state_ack *ack)
{
	return ack->data;
}

static inline const void *vblk_msg_get_state_ack_cfg(const struct vblk_msg_ho_get_state_ack *ack)
{
	return ack->data + ack->export_desc_len;
}

/** END: carries success/fail status. */
struct vblk_msg_ho_end {
	uint8_t success; /* 1 = handover succeeded, 0 = failed */
};

#endif /* VBLK_IPC_MSGS_H_ */
