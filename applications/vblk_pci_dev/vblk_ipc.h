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

/**
 * @file vblk_ipc.h
 * @brief Generic typed-message IPC over Unix datagram sockets.
 *
 * Provides a transport layer with no domain knowledge.  Callers define
 * their own message types and payload structs (see vblk_ipc_msgs.h).
 *
 * Each endpoint binds to a filesystem path and can send/receive typed
 * messages to/from any other endpoint.  Messages are datagrams: atomic,
 * ordered, and reliable on Unix domain sockets.
 */

#ifndef VBLK_IPC_H_
#define VBLK_IPC_H_

#include <stdint.h>
#include <sys/un.h>
#include <doca_error.h>

/* ─── Wire format ────────────────────────────────────────────────── */

/** On-wire header prepended to every datagram. */
struct vblk_ipc_hdr {
	uint32_t type; /**< Caller-defined message type */
	uint32_t len;  /**< Payload bytes following this header */
};

/** Maximum payload per datagram (conservative, well under kernel limit). */
#define VBLK_IPC_MAX_PAYLOAD (64 * 1024)

/* ─── Endpoint ───────────────────────────────────────────────────── */

/** A bound Unix datagram socket with an optional default peer. */
struct vblk_ipc_ep {
	int fd;
	struct sockaddr_un peer;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

/**
 * @brief Create and bind an IPC endpoint.
 *
 * @param[out] ep         Endpoint to initialise.
 * @param[in]  bind_path  Filesystem path to bind (unlinked first if exists).
 * @param[in]  peer_path  Default peer path, or NULL for no default peer.
 * @return DOCA_SUCCESS, or DOCA_ERROR_INITIALIZATION on failure.
 */
doca_error_t vblk_ipc_ep_open(struct vblk_ipc_ep *ep, const char *bind_path, const char *peer_path);

/** Close socket and unlink the bound path. */
void vblk_ipc_ep_close(struct vblk_ipc_ep *ep);

/** Change the default peer at runtime (e.g. redirect TLP from SRC to DST). */
void vblk_ipc_ep_set_peer(struct vblk_ipc_ep *ep, const char *peer_path);

/**
 * @brief Send a typed message to the default peer.
 *
 * @return DOCA_SUCCESS, or DOCA_ERROR_AGAIN if the peer is not reachable yet.
 */
doca_error_t vblk_ipc_send(struct vblk_ipc_ep *ep, uint32_t type, const void *payload, uint32_t len);

/**
 * @brief Send a typed message to a specific peer (bypasses default).
 */
doca_error_t vblk_ipc_send_to(struct vblk_ipc_ep *ep,
			      const char *dest_path,
			      uint32_t type,
			      const void *payload,
			      uint32_t len);

/**
 * @brief Non-blocking receive.
 *
 * @param[out] type     Message type from the header.
 * @param[out] buf      Buffer for payload (may be NULL if buf_size == 0).
 * @param[in]  buf_size Size of buf in bytes.
 * @param[out] out_len  Actual payload length (may exceed buf_size if truncated).
 * @return DOCA_SUCCESS on message received, DOCA_ERROR_AGAIN if nothing pending.
 */
doca_error_t vblk_ipc_recv(struct vblk_ipc_ep *ep, uint32_t *type, void *buf, uint32_t buf_size, uint32_t *out_len);

#endif /* VBLK_IPC_H_ */
