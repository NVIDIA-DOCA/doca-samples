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
 * @file vblk_ipc.c
 * @brief Generic typed-message IPC — implementation.
 */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <doca_log.h>
#include "vblk_ipc.h"

DOCA_LOG_REGISTER(VBLK_IPC);

/* ─── Internal helpers ───────────────────────────────────────────── */

static void set_peer_addr(struct sockaddr_un *addr, const char *path)
{
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	strncpy(addr->sun_path, path, sizeof(addr->sun_path) - 1);
}

static doca_error_t send_raw(int fd, const struct sockaddr_un *dest, uint32_t type, const void *payload, uint32_t len)
{
	static uint8_t dgram[sizeof(struct vblk_ipc_hdr) + VBLK_IPC_MAX_PAYLOAD];
	struct vblk_ipc_hdr *hdr = (struct vblk_ipc_hdr *)dgram;

	if (len > VBLK_IPC_MAX_PAYLOAD)
		return DOCA_ERROR_INVALID_VALUE;

	hdr->type = type;
	hdr->len = len;
	if (len > 0)
		memcpy(dgram + sizeof(*hdr), payload, len);

	ssize_t total = sizeof(*hdr) + len;
	ssize_t r = sendto(fd, dgram, total, 0, (const struct sockaddr *)dest, sizeof(*dest));
	if (r != total) {
		if (errno == ENOENT || errno == ECONNREFUSED || errno == EAGAIN)
			return DOCA_ERROR_AGAIN;
		return DOCA_ERROR_IO_FAILED;
	}
	return DOCA_SUCCESS;
}

/* ─── Endpoint lifecycle ─────────────────────────────────────────── */

doca_error_t vblk_ipc_ep_open(struct vblk_ipc_ep *ep, const char *bind_path, const char *peer_path)
{
	memset(ep, 0, sizeof(*ep));

	ep->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (ep->fd < 0) {
		DOCA_LOG_ERR("IPC: socket() failed: %s", strerror(errno));
		return DOCA_ERROR_INITIALIZATION;
	}

	int rcvbuf = 2 * 1024 * 1024;
	(void)setsockopt(ep->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	struct sockaddr_un addr;
	set_peer_addr(&addr, bind_path);
	unlink(bind_path);
	if (bind(ep->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		DOCA_LOG_ERR("IPC: bind(%s) failed: %s", bind_path, strerror(errno));
		close(ep->fd);
		return DOCA_ERROR_INITIALIZATION;
	}
	strncpy(ep->path, bind_path, sizeof(ep->path) - 1);

	if (peer_path)
		set_peer_addr(&ep->peer, peer_path);

	DOCA_LOG_INFO("IPC ep: bound %s, peer %s", bind_path, peer_path ? peer_path : "(none)");
	return DOCA_SUCCESS;
}

void vblk_ipc_ep_close(struct vblk_ipc_ep *ep)
{
	if (ep->fd >= 0) {
		close(ep->fd);
		ep->fd = -1;
	}
	if (ep->path[0])
		unlink(ep->path);
}

void vblk_ipc_ep_set_peer(struct vblk_ipc_ep *ep, const char *peer_path)
{
	set_peer_addr(&ep->peer, peer_path);
}

/* ─── Send / Receive ─────────────────────────────────────────────── */

doca_error_t vblk_ipc_send(struct vblk_ipc_ep *ep, uint32_t type, const void *payload, uint32_t len)
{
	return send_raw(ep->fd, &ep->peer, type, payload, len);
}

doca_error_t vblk_ipc_send_to(struct vblk_ipc_ep *ep,
			      const char *dest_path,
			      uint32_t type,
			      const void *payload,
			      uint32_t len)
{
	struct sockaddr_un dest;
	set_peer_addr(&dest, dest_path);
	return send_raw(ep->fd, &dest, type, payload, len);
}

doca_error_t vblk_ipc_recv(struct vblk_ipc_ep *ep, uint32_t *type, void *buf, uint32_t buf_size, uint32_t *out_len)
{
	static uint8_t dgram[sizeof(struct vblk_ipc_hdr) + VBLK_IPC_MAX_PAYLOAD];

	ssize_t r = recv(ep->fd, dgram, sizeof(dgram), 0);
	if (r <= 0)
		return DOCA_ERROR_AGAIN;
	if (r < (ssize_t)sizeof(struct vblk_ipc_hdr))
		return DOCA_ERROR_AGAIN;

	const struct vblk_ipc_hdr *hdr = (const struct vblk_ipc_hdr *)dgram;
	if ((ssize_t)(sizeof(*hdr) + hdr->len) > r)
		return DOCA_ERROR_AGAIN;

	*type = hdr->type;
	*out_len = hdr->len;

	uint32_t copy = hdr->len < buf_size ? hdr->len : buf_size;
	if (copy > 0 && buf)
		memcpy(buf, dgram + sizeof(*hdr), copy);

	return DOCA_SUCCESS;
}
