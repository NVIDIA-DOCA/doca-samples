/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <pthread.h>
#include <linux/if_ether.h>

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_rdma_bridge.h>
#include <doca_devemu_pci.h>
#include <doca_ctx.h>
#include <doca_devemu_virtio.h>
#include <doca_devemu_virtio_tlp.h>
#include <doca_devemu_vnet_offload_engine.h>
#include <doca_devemu_vnet_io.h>
#include <doca_devemu_vnet_counters.h>

#include "vnet_pci_dev_lu.h"
#include "vnet_pci_dev_core.h"

/* Forward declaration for channel LU active handler (defined below) */
static doca_error_t vnet_lu_channel_handover(int conn_fd, struct tlp_context *tlp_ctx);

DOCA_LOG_REGISTER(VNET_LIVE_UPDATE);

static volatile sig_atomic_t vnet_lu_handover_requested = 0;
static int vnet_lu_listen_fd = -1;
static int vnet_lu_standby_conn_fd = -1;
static int vnet_lu_lock_fd = -1;

static struct vnet_lu_shm *vnet_lu_saved_shm = NULL;
static size_t vnet_lu_saved_shm_size = 0;

static struct vnet_lu_shm *vnet_lu_active_shm = NULL;
static size_t vnet_lu_active_shm_size = 0;

/*********************************************************************************************************************
 * Utilities -- signal handling, socket/poll helpers, SCM_RIGHTS, query helpers
 *********************************************************************************************************************/

static void vnet_lu_sigusr1_handler(int sig)
{
	(void)sig;
	vnet_lu_handover_requested = 1;
}

bool vnet_lu_handover_was_triggered(void)
{
	return vnet_lu_handover_requested != 0;
}

static doca_error_t vnet_lu_wait_readable(int fd, int timeout_sec)
{
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	int ret = poll(&pfd, 1, timeout_sec * 1000);

	if (ret < 0) {
		DOCA_LOG_ERR("poll() failed: %s", strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}
	if (ret == 0)
		return DOCA_ERROR_AGAIN;
	/* POLLIN takes priority: peer may send() then close(), causing
	 * POLLIN|POLLHUP simultaneously.  Read the data first. */
	if (pfd.revents & POLLIN)
		return DOCA_SUCCESS;
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return DOCA_ERROR_IO_FAILED;
	return DOCA_SUCCESS;
}

static doca_error_t vnet_lu_recv_all(int fd, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;

	while (len > 0) {
		ssize_t n = recv(fd, p, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			DOCA_LOG_ERR("recv() failed: %s", strerror(errno));
			return DOCA_ERROR_IO_FAILED;
		}
		if (n == 0) {
			DOCA_LOG_ERR("recv(): peer closed connection mid-stream (%zu bytes remaining)", len);
			return DOCA_ERROR_IO_FAILED;
		}
		p += (size_t)n;
		len -= (size_t)n;
	}
	return DOCA_SUCCESS;
}

static doca_error_t vnet_lu_send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			DOCA_LOG_ERR("send() failed: %s", strerror(errno));
			return DOCA_ERROR_IO_FAILED;
		}
		p += (size_t)n;
		len -= (size_t)n;
	}
	return DOCA_SUCCESS;
}

static doca_error_t vnet_lu_send_msg(int fd, enum vnet_lu_msg msg)
{
	uint32_t val = (uint32_t)msg;

	return vnet_lu_send_all(fd, &val, sizeof(val));
}

static doca_error_t vnet_lu_recv_msg(int fd, enum vnet_lu_msg *msg_out)
{
	uint32_t val = 0;
	doca_error_t result = vnet_lu_recv_all(fd, &val, sizeof(val));

	if (result == DOCA_SUCCESS)
		*msg_out = (enum vnet_lu_msg)val;
	return result;
}

doca_error_t vnet_lu_send_cmd_fd(int sock_fd, int fd_to_send)
{
	char buf = VNET_LU_MSG_CMD_FD;
	struct iovec iov = {.iov_base = &buf, .iov_len = 1};
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} cmsg_buf;
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	int fd_dup;

	fd_dup = dup(fd_to_send);
	if (fd_dup < 0) {
		DOCA_LOG_ERR("dup(cmd_fd=%d) failed: %s", fd_to_send, strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}
	DOCA_LOG_DBG("Sending cmd_fd: original=%d, dup=%d", fd_to_send, fd_dup);

	memset(&cmsg_buf, 0, sizeof(cmsg_buf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf.buf;
	msg.msg_controllen = sizeof(cmsg_buf.buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd_dup, sizeof(fd_dup));

	if (sendmsg(sock_fd, &msg, MSG_NOSIGNAL) < 0) {
		DOCA_LOG_ERR("sendmsg(SCM_RIGHTS) failed: %s", strerror(errno));
		close(fd_dup);
		return DOCA_ERROR_IO_FAILED;
	}

	close(fd_dup);
	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_recv_cmd_fd(int sock_fd, int *fd_out)
{
	char buf = 0;
	struct iovec iov = {.iov_base = &buf, .iov_len = 1};
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} cmsg_buf;
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	ssize_t rc;

	memset(&cmsg_buf, 0, sizeof(cmsg_buf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf.buf;
	msg.msg_controllen = sizeof(cmsg_buf.buf);

	rc = recvmsg(sock_fd, &msg, 0);
	if (rc == 0) {
		DOCA_LOG_ERR("recvmsg(SCM_RIGHTS): peer closed connection");
		return DOCA_ERROR_IO_FAILED;
	}
	if (rc < 0) {
		DOCA_LOG_ERR("recvmsg(SCM_RIGHTS) failed: %s", strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}
	if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		DOCA_LOG_ERR("recvmsg(SCM_RIGHTS): message truncated (flags=0x%x)", msg.msg_flags);
		return DOCA_ERROR_IO_FAILED;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
		DOCA_LOG_ERR("SCM_RIGHTS: invalid or missing ancillary data");
		return DOCA_ERROR_IO_FAILED;
	}

	memcpy(fd_out, CMSG_DATA(cmsg), sizeof(int));
	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_get_cmd_fd(struct doca_dev *dev, int *cmd_fd_out)
{
	struct ibv_pd *pd = NULL;
	doca_error_t result;

	result = doca_rdma_bridge_get_dev_pd(dev, &pd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_rdma_bridge_get_dev_pd() failed: %s", doca_error_get_descr(result));
		return result;
	}
	if (!pd || !pd->context) {
		DOCA_LOG_ERR("ibv_pd or ibv_context is NULL after doca_rdma_bridge_get_dev_pd()");
		return DOCA_ERROR_DRIVER;
	}

	*cmd_fd_out = pd->context->cmd_fd;
	DOCA_LOG_DBG("Extracted cmd_fd=%d from doca_dev", *cmd_fd_out);
	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_get_sf_vhca_id(struct doca_devemu_vnet_offload_engine *engine, uint16_t *vhca_id_out)
{
	struct doca_dev_rep *sf_rep = NULL;
	struct doca_devinfo_rep *sf_devinfo;

	if (doca_devemu_vnet_offload_engine_get_rep(engine, &sf_rep) != DOCA_SUCCESS)
		return DOCA_ERROR_NOT_FOUND;

	sf_devinfo = doca_dev_rep_as_devinfo(sf_rep);
	if (!sf_devinfo)
		return DOCA_ERROR_NOT_FOUND;

	return doca_devinfo_rep_get_vhca_id(sf_devinfo, vhca_id_out);
}

/*********************************************************************************************************************
 * Active -- save state to SHM, handover to standby
 *********************************************************************************************************************/

static void vnet_lu_save_one_vq(struct vnet_lu_vq_state *dst, const struct vnet_virtio_queue_config *src, uint16_t idx)
{
	dst->index = idx;
	dst->size = src->queue_size;
	dst->msix_vector = src->queue_msix_vector;
	dst->desc_addr = src->queue_desc;
	dst->driver_addr = src->queue_driver;
	dst->device_addr = src->queue_device;
	dst->enabled = src->queue_enable != 0;
}

struct vnet_lu_write_ctx {
	void *shm_base;
	uint32_t blob_cursor;
};

static void vnet_lu_fill_device_state(struct vnet_lu_device_state *ds,
				      struct tlp_context *tlp_ctx,
				      uint32_t idx,
				      const void *blob,
				      size_t blob_len,
				      struct vnet_lu_write_ctx *wctx)
{
	struct vnet_pci_dev_controller *ctrl = &tlp_ctx->vnet_controller[idx];
	struct pci_device_config *ep = &tlp_ctx->devs_config[tlp_ctx->num_bridges + idx];
	struct vnet_pci_device *vdev = &tlp_ctx->virtio_dev[idx];
	const struct vnet_virtio_queue_config *vq_cfg;
	const struct vnet_virtio_net_config *net_cfg;
	uint16_t max_vqs = VNET_TOTAL_VQS(VNET_CTRL_MAX_QUEUES_PAIRS);

	ds->magic = VNET_LU_MAGIC;
	ds->ep_vhca_id = (uint16_t)ep->vhca_id;

	/* SF vhca_id logged for diagnostics only. */
	if (ctrl->offload_engine) {
		uint16_t sf_vhca_id = 0;

		if (vnet_lu_get_sf_vhca_id(ctrl->offload_engine, &sf_vhca_id) == DOCA_SUCCESS)
			DOCA_LOG_INFO("Device %u: SF vhca_id=0x%x (pre-handover)", idx, sf_vhca_id);
	}

	net_cfg = vnet_pci_device_get_vnet_dev_cfg(vdev);
	if (net_cfg) {
		memcpy(ds->net.mac, net_cfg->mac, ETH_ALEN);
		ds->net.mtu = net_cfg->mtu;
	}

	ds->virtio.device_features = vdev->driver_features;
	ds->virtio.max_queue_pairs = ctrl->max_queue_pairs;
	ds->virtio.num_active_qps = atomic_load(&ctrl->num_active_qps);
	ds->virtio.queue_size = vdev->queue_size;
	ds->virtio.mq_feature_negotiated = ctrl->mq_feature_negotiated;

	vq_cfg = vnet_pci_device_get_virtq_pci_cfg(vdev);
	ds->vqs.num_vqs = vdev->vqs_count;
	for (uint16_t q = 0; q < vdev->vqs_count && q < max_vqs; q++)
		vnet_lu_save_one_vq(&ds->vqs.vqs[q], &vq_cfg[q], q);

	if (blob && blob_len > 0) {
		ds->blob_offset = wctx->blob_cursor;
		ds->blob_len = (uint32_t)blob_len;
		memcpy((uint8_t *)wctx->shm_base + wctx->blob_cursor, blob, blob_len);
		wctx->blob_cursor += (uint32_t)blob_len;
	} else {
		ds->blob_offset = 0;
		ds->blob_len = 0;
	}

	DOCA_LOG_INFO("Saved device %u: ep_vhca=0x%x, vqs=%u, active_qps=%u, blob=%u bytes",
		      idx,
		      ds->ep_vhca_id,
		      ds->vqs.num_vqs,
		      ds->virtio.num_active_qps,
		      ds->blob_len);
}

static doca_error_t vnet_lu_create_shm(size_t shm_size, struct vnet_lu_shm **shm_out)
{
	int fd;

	(void)shm_unlink(VNET_LU_SHM_NAME);

	fd = shm_open(VNET_LU_SHM_NAME, O_CREAT | O_RDWR, 0600);
	if (fd < 0) {
		DOCA_LOG_ERR("shm_open(%s) failed: %s", VNET_LU_SHM_NAME, strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}

	if (ftruncate(fd, (off_t)shm_size) < 0) {
		DOCA_LOG_ERR("ftruncate(%zu) failed: %s", shm_size, strerror(errno));
		close(fd);
		(void)shm_unlink(VNET_LU_SHM_NAME);
		return DOCA_ERROR_IO_FAILED;
	}

	*shm_out = (struct vnet_lu_shm *)mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (*shm_out == MAP_FAILED) {
		DOCA_LOG_ERR("mmap(%zu) failed: %s", shm_size, strerror(errno));
		*shm_out = NULL;
		(void)shm_unlink(VNET_LU_SHM_NAME);
		return DOCA_ERROR_IO_FAILED;
	}

	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_save_state(struct tlp_context *tlp_ctx)
{
	size_t base_size, total_blob_size = 0, shm_size;
	const void **blobs = NULL;
	size_t *blob_lens = NULL;
	struct vnet_lu_shm *shm = NULL;
	doca_error_t result;
	uint32_t num_ep;

	if (!tlp_ctx) {
		DOCA_LOG_ERR("'tlp_ctx' is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	num_ep = tlp_ctx->num_ep;
	base_size = sizeof(struct vnet_lu_shm) + num_ep * sizeof(struct vnet_lu_device_state);

	/* Pass 1: export blobs to determine total blob size */
	blobs = (const void **)calloc(num_ep, sizeof(const void *));
	blob_lens = (size_t *)calloc(num_ep, sizeof(size_t));
	if (!blobs || !blob_lens) {
		DOCA_LOG_ERR("Allocation failed for blob arrays");
		free(blobs);
		free(blob_lens);
		return DOCA_ERROR_NO_MEMORY;
	}

	for (uint32_t i = 0; i < num_ep; i++) {
		struct vnet_pci_dev_controller *ctrl = &tlp_ctx->vnet_controller[i];

		if (!ctrl->offload_engine) {
			DOCA_LOG_WARN("Device %u: no offload engine, skipping blob export", i);
			continue;
		}
		result = doca_devemu_vnet_offload_engine_export(ctrl->offload_engine, &blobs[i], &blob_lens[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Device %u: offload engine export failed (%s), proceeding without blob",
				      i,
				      doca_error_get_descr(result));
			blobs[i] = NULL;
			blob_lens[i] = 0;
		}
		total_blob_size += blob_lens[i];
	}

	shm_size = base_size + total_blob_size;

	result = vnet_lu_create_shm(shm_size, &shm);
	if (result != DOCA_SUCCESS)
		goto release_blobs;

	memset(shm, 0, shm_size);
	shm->state = VNET_LU_STATE_PRE_COPY;
	DOCA_LOG_INFO("LU state: INITIALIZED -> PRE_COPY");
	shm->num_devices = num_ep;
	shm->blob_data_offset = (uint32_t)base_size;

	/* Pass 2: fill per-device state and copy blobs into trailing region */
	struct vnet_lu_write_ctx wctx = {.shm_base = shm, .blob_cursor = (uint32_t)base_size};

	for (uint32_t i = 0; i < num_ep; i++)
		vnet_lu_fill_device_state(&shm->devices[i], tlp_ctx, i, blobs[i], blob_lens[i], &wctx);

	vnet_lu_active_shm = shm;
	vnet_lu_active_shm_size = shm_size;

	DOCA_LOG_INFO("State saved to SHM (%s): %u devices, %zu bytes (blobs: %zu bytes)",
		      VNET_LU_SHM_NAME,
		      num_ep,
		      shm_size,
		      total_blob_size);
	result = DOCA_SUCCESS;

release_blobs:
	free(blobs);
	free(blob_lens);
	return result;
}

static doca_error_t vnet_lu_create_listen_socket(int *listen_fd)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0) {
		DOCA_LOG_ERR("socket() failed: %s", strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, VNET_LU_SOCK_PATH, sizeof(addr.sun_path) - 1);
	(void)unlink(VNET_LU_SOCK_PATH);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		DOCA_LOG_ERR("bind(%s) failed: %s", VNET_LU_SOCK_PATH, strerror(errno));
		close(fd);
		return DOCA_ERROR_IO_FAILED;
	}

	if (listen(fd, 1) < 0) {
		DOCA_LOG_ERR("listen() failed: %s", strerror(errno));
		close(fd);
		(void)unlink(VNET_LU_SOCK_PATH);
		return DOCA_ERROR_IO_FAILED;
	}

	*listen_fd = fd;
	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_active_init(void)
{
	struct sigaction sa = {0};

	vnet_lu_handover_requested = 0;
	(void)shm_unlink(VNET_LU_SHM_NAME);

	sa.sa_handler = vnet_lu_sigusr1_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		DOCA_LOG_ERR("sigaction(SIGUSR1) failed: %s", strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	return vnet_lu_create_listen_socket(&vnet_lu_listen_fd);
}

static doca_error_t vnet_lu_handover(struct vnet_pci_dev_resources *resources)
{
	struct tlp_context *tlp_ctx = resources->tlp_ctx;
	enum vnet_lu_msg msg;
	doca_error_t result;
	int conn_fd = -1;
	int cmd_fd = -1;

	/* Pre-copy phase: export state, send cmd_fd, let App_B init. */

	result = vnet_lu_get_cmd_fd(tlp_ctx->dev, &cmd_fd);
	if (result != DOCA_SUCCESS)
		return result;

	result = vnet_lu_save_state(tlp_ctx);
	if (result != DOCA_SUCCESS)
		return result;

	result = vnet_lu_wait_readable(vnet_lu_listen_fd, VNET_LU_ACCEPT_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS)
		goto out;

	conn_fd = accept(vnet_lu_listen_fd, NULL, NULL);
	if (conn_fd < 0) {
		DOCA_LOG_ERR("accept() failed: %s", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto out;
	}

	/* Unlink early to prevent race with standby creating a new socket. */
	close(vnet_lu_listen_fd);
	vnet_lu_listen_fd = -1;
	(void)unlink(VNET_LU_SOCK_PATH);

	result = vnet_lu_send_cmd_fd(conn_fd, cmd_fd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send cmd_fd via SCM_RIGHTS");
		goto out;
	}

	/* Wait for standby pre-copy init to complete. */
	result = vnet_lu_wait_readable(conn_fd, VNET_LU_READY_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting for standby ready signal");
		goto out;
	}

	result = vnet_lu_recv_msg(conn_fd, &msg);
	if (result != DOCA_SUCCESS || msg != VNET_LU_MSG_DEV_READY) {
		DOCA_LOG_ERR("Invalid ready signal: got %u (expected DEV_READY=%u)",
			     (unsigned)msg,
			     (unsigned)VNET_LU_MSG_DEV_READY);
		result = DOCA_ERROR_IO_FAILED;
		goto out;
	}

	/* Switchover: per-device disable + DEV_GO, then wait for DEV_ACK. */

	pci_cfg_workqueue_shutdown();

	/* Pipelined: disable + send DEV_GO per device for parallel enable. */
	for (uint32_t i = 0; i < tlp_ctx->num_ep; i++) {
		(void)vnet_controller_cleanup(&tlp_ctx->vnet_controller[i], false);

		result = vnet_lu_send_msg(conn_fd, VNET_LU_MSG_DEV_GO);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send DEV_GO for device %u", i);
			goto out;
		}
	}

	if (vnet_lu_active_shm) {
		vnet_lu_active_shm->state = VNET_LU_STATE_STOPPED;
		DOCA_LOG_INFO("LU state: PRE_COPY -> STOPPED");
	}

	/* Wait for final DEV_ACK from standby. */
	result = vnet_lu_wait_readable(conn_fd, VNET_LU_ACK_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Waiting for standby DEV_ACK failed: %s", doca_error_get_descr(result));
		goto out;
	}

	result = vnet_lu_recv_msg(conn_fd, &msg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("recv DEV_ACK failed: %s", doca_error_get_descr(result));
		result = DOCA_ERROR_IO_FAILED;
	} else if (msg != VNET_LU_MSG_DEV_ACK) {
		DOCA_LOG_ERR("Unexpected message: got %u (expected DEV_ACK=%u)",
			     (unsigned)msg,
			     (unsigned)VNET_LU_MSG_DEV_ACK);
		result = DOCA_ERROR_IO_FAILED;
	}

	if (result == DOCA_SUCCESS && vnet_lu_active_shm) {
		vnet_lu_active_shm->state = VNET_LU_STATE_MIGRATED;
		DOCA_LOG_INFO("LU state: STOPPED -> MIGRATED");
	}

	/* Channel LU: handover TLP channel after device LU completes.
	 * Device traffic is already restored; channel LU transfers PCI config
	 * handling.  Failure here is non-fatal for device traffic. */
	if (result == DOCA_SUCCESS) {
		doca_error_t ch_result = vnet_lu_channel_handover(conn_fd, tlp_ctx);

		if (ch_result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Channel LU handover failed: %s (device LU succeeded)",
				      doca_error_get_descr(ch_result));
	}

out:
	if (conn_fd >= 0)
		close(conn_fd);
	if (vnet_lu_active_shm) {
		if (result != DOCA_SUCCESS)
			(void)shm_unlink(VNET_LU_SHM_NAME);
		munmap(vnet_lu_active_shm, vnet_lu_active_shm_size);
		vnet_lu_active_shm = NULL;
		vnet_lu_active_shm_size = 0;
	}
	return result;
}

bool vnet_lu_active_post_loop(struct vnet_pci_dev_resources *resources)
{
	struct sigaction sa_dfl = {0};
	bool did_handover = false;

	if (vnet_lu_handover_requested) {
		doca_error_t result = vnet_lu_handover(resources);

		did_handover = (result == DOCA_SUCCESS);
		if (!did_handover)
			DOCA_LOG_ERR("LU handover failed: %s", doca_error_get_descr(result));
	}

	/* Clean up only if handover was never triggered. */
	if (vnet_lu_listen_fd >= 0) {
		close(vnet_lu_listen_fd);
		vnet_lu_listen_fd = -1;
		(void)unlink(VNET_LU_SOCK_PATH);
	}
	sa_dfl.sa_handler = SIG_DFL;
	sigemptyset(&sa_dfl.sa_mask);
	(void)sigaction(SIGUSR1, &sa_dfl, NULL);
	return did_handover;
}

/*********************************************************************************************************************
 * Channel LU -- active-side: export TLP channel, send config state, yield primary
 *********************************************************************************************************************/

static void vnet_lu_collect_channel_config(struct vnet_lu_channel_config *ch_cfg, const struct tlp_context *tlp_ctx)
{
	uint32_t count = tlp_ctx->num_devices;

	if (count > (MAX_NUM_BRIDGE + MAX_NUM_EP))
		count = MAX_NUM_BRIDGE + MAX_NUM_EP;

	memset(ch_cfg, 0, sizeof(*ch_cfg));
	ch_cfg->num_devices = count;
	ch_cfg->num_bridges = tlp_ctx->num_bridges;
	ch_cfg->num_ep = tlp_ctx->num_ep;
	ch_cfg->transaction_region_size = (uint32_t)tlp_ctx->transaction_region_size;

	for (uint32_t i = 0; i < count; i++) {
		const struct pci_device_config *dev = &tlp_ctx->devs_config[i];
		struct vnet_lu_channel_dev_state *dst = &ch_cfg->devices[i];

		memcpy(&dst->cfg_space_hdr, &dev->cfg_space_hdr, sizeof(dst->cfg_space_hdr));
		dst->caps = dev->caps;
		dst->bus = dev->bus;
		dst->device = dev->device;
		dst->function = dev->function;
		dst->is_bdf_set = dev->is_bdf_set ? 1 : 0;
		dst->bdf = dev->bdf;
		dst->requester_id = dev->requester_id;
		dst->completer_id = dev->completer_id;
		dst->tag9 = dev->tag9;
		dst->tag8 = dev->tag8;
		dst->tag = dev->tag;
		dst->req_fmt = dev->req_fmt;
		dst->req_type = dev->req_type;
		dst->cmpl_fmt = dev->cmpl_fmt;
		dst->cmpl_type = dev->cmpl_type;
		dst->cmpl_status = dev->cmpl_status;
		dst->cmpl_length = dev->cmpl_length;
		dst->cmpl_data = dev->cmpl_data;
	}
}

static void vnet_lu_apply_channel_config(const struct vnet_lu_channel_config *ch_cfg, struct tlp_context *tlp_ctx)
{
	uint32_t count = ch_cfg->num_devices;

	if (count > tlp_ctx->num_devices)
		count = tlp_ctx->num_devices;

	tlp_ctx->num_devices = count;
	tlp_ctx->num_bridges = ch_cfg->num_bridges;
	tlp_ctx->num_ep = ch_cfg->num_ep;
	tlp_ctx->transaction_region_size = ch_cfg->transaction_region_size;

	for (uint32_t i = 0; i < count; i++) {
		struct pci_device_config *dev = &tlp_ctx->devs_config[i];
		const struct vnet_lu_channel_dev_state *src = &ch_cfg->devices[i];

		memcpy(&dev->cfg_space_hdr, &src->cfg_space_hdr, sizeof(dev->cfg_space_hdr));
		dev->caps = src->caps;
		dev->bus = src->bus;
		dev->device = src->device;
		dev->function = src->function;
		dev->is_bdf_set = src->is_bdf_set;
		dev->bdf = src->bdf;
		dev->requester_id = src->requester_id;
		dev->completer_id = src->completer_id;
		dev->tag9 = src->tag9;
		dev->tag8 = src->tag8;
		dev->tag = src->tag;
		dev->req_fmt = src->req_fmt;
		dev->req_type = src->req_type;
		dev->cmpl_fmt = src->cmpl_fmt;
		dev->cmpl_type = src->cmpl_type;
		dev->cmpl_status = src->cmpl_status;
		dev->cmpl_length = src->cmpl_length;
		dev->cmpl_data = src->cmpl_data;
	}
}

static void vnet_lu_channel_reclaim(struct tlp_context *tlp_ctx)
{
	doca_error_t result;

	result = doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, true);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Channel LU reclaim: set_primary(true) failed: %s", doca_error_get_descr(result));

	result = doca_ctx_start(doca_devemu_pci_tlp_channel_as_ctx(tlp_ctx->tlp_channel));
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Channel LU reclaim: ctx_start failed: %s", doca_error_get_descr(result));
}

doca_error_t vnet_lu_channel_enable_export(struct doca_devemu_pci_tlp_channel *tlp_channel)
{
	doca_error_t result;

	if (mkdir(VNET_LU_CH_SHM_DIR, 0700) != 0 && errno != EEXIST) {
		DOCA_LOG_ERR("Failed to create channel SHM dir '%s': %s", VNET_LU_CH_SHM_DIR, strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	result = doca_devemu_pci_tlp_channel_set_shm_dir_path(tlp_channel, VNET_LU_CH_SHM_DIR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set channel SHM dir: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_tlp_channel_set_primary(tlp_channel, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set channel as primary: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t vnet_lu_channel_handover(int conn_fd, struct tlp_context *tlp_ctx)
{
	struct vnet_lu_channel_config ch_cfg;
	const void *export_desc = NULL;
	size_t export_desc_len = 0;
	struct doca_ctx *ch_ctx = NULL;
	uint8_t handover_ok = 0;
	enum vnet_lu_msg msg;
	doca_error_t result;
	uint32_t desc_len32;

	if (!tlp_ctx->tlp_channel) {
		DOCA_LOG_ERR("Channel LU active: no TLP channel to export");
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Channel LU active: starting EXPORT phase");

	result = doca_devemu_pci_tlp_channel_export(tlp_ctx->tlp_channel, &export_desc, &export_desc_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: export failed: %s", doca_error_get_descr(result));
		return result;
	}

	result = vnet_lu_send_msg(conn_fd, VNET_LU_MSG_CH_EXPORT);
	if (result != DOCA_SUCCESS)
		return result;

	if (export_desc_len > UINT32_MAX) {
		DOCA_LOG_ERR("Channel LU: export_desc_len too large (%zu)", export_desc_len);
		result = DOCA_ERROR_INVALID_VALUE;
		return result;
	}
	desc_len32 = (uint32_t)export_desc_len;
	result = vnet_lu_send_all(conn_fd, &desc_len32, sizeof(desc_len32));
	if (result != DOCA_SUCCESS)
		return result;

	result = vnet_lu_send_all(conn_fd, export_desc, export_desc_len);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("Channel LU active: export sent (%u bytes), waiting for CH_BEGIN", desc_len32);

	result = vnet_lu_wait_readable(conn_fd, VNET_LU_CH_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: timeout waiting for CH_BEGIN");
		return result;
	}
	result = vnet_lu_recv_msg(conn_fd, &msg);
	if (result != DOCA_SUCCESS || msg != VNET_LU_MSG_CH_BEGIN) {
		DOCA_LOG_ERR("Channel LU: expected CH_BEGIN, got %u", (unsigned)msg);
		return (result == DOCA_SUCCESS) ? DOCA_ERROR_IO_FAILED : result;
	}

	/* Yield primary role first, then stop context -- matches SDK sample ordering.
	 * Yielding before stopping lets standby become primary earlier. */
	result = doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: set_primary(false) failed: %s", doca_error_get_descr(result));
		goto send_begin_nack;
	}

	ch_ctx = doca_devemu_pci_tlp_channel_as_ctx(tlp_ctx->tlp_channel);
	result = doca_ctx_stop(ch_ctx);
	if (result == DOCA_ERROR_IN_PROGRESS) {
		enum doca_ctx_states ctx_state;

		do {
			(void)doca_pe_progress(tlp_ctx->pe);
			result = doca_ctx_get_state(ch_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Channel LU: ctx_get_state failed: %s", doca_error_get_descr(result));
				goto rollback_primary;
			}
		} while (ctx_state != DOCA_CTX_STATE_IDLE);
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: ctx_stop failed: %s", doca_error_get_descr(result));
		goto rollback_primary;
	}
	ch_ctx = NULL;

	DOCA_LOG_INFO("Channel LU active: yielded primary, channel stopped");

	vnet_lu_collect_channel_config(&ch_cfg, tlp_ctx);

	/* Standby recv/apply assumes ch_cfg.transaction_region_size matches its allocation; keep wire value
	 * consistent with buffers we send next (fail if zero or uint32_t truncation in ch_cfg). */
	if (tlp_ctx->transaction_region_size == 0) {
		DOCA_LOG_ERR("Channel LU active: transaction_region_size is zero");
		result = DOCA_ERROR_INVALID_VALUE;
		goto rollback_stopped;
	}
	if ((size_t)ch_cfg.transaction_region_size != tlp_ctx->transaction_region_size) {
		DOCA_LOG_ERR("Channel LU active: transaction_region_size overflow in ch_cfg (cfg=%u ctx=%zu)",
			     ch_cfg.transaction_region_size,
			     tlp_ctx->transaction_region_size);
		result = DOCA_ERROR_INVALID_VALUE;
		goto rollback_stopped;
	}

	result = vnet_lu_send_msg(conn_fd, VNET_LU_MSG_CH_BEGIN_ACK);
	if (result != DOCA_SUCCESS)
		goto rollback_stopped;

	result = vnet_lu_send_all(conn_fd, &ch_cfg, sizeof(ch_cfg));
	if (result != DOCA_SUCCESS)
		goto rollback_stopped;

	/* Send per-device transaction regions (PCI config space snapshots) */
	for (uint32_t i = 0; i < tlp_ctx->num_ep; i++) {
		if (!tlp_ctx->transaction_region_memories || !tlp_ctx->transaction_region_memories[i])
			continue;
		result = vnet_lu_send_all(conn_fd,
					  tlp_ctx->transaction_region_memories[i],
					  tlp_ctx->transaction_region_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Channel LU: send transaction region %u failed", i);
			goto rollback_stopped;
		}
	}

	/* Send per-endpoint VirtIO PCI config space (dw_regs[]).
	 * The VirtIO layer maintains its own config space shadow that holds
	 * host-written BAR addresses, command register, and capability state.
	 * This is separate from the topology-level cfg_space_hdr above. */
	for (uint32_t i = 0; i < tlp_ctx->num_ep; i++) {
		result = vnet_lu_send_all(conn_fd,
					  tlp_ctx->virtio_dev[i].pcie_dev.dw_regs,
					  sizeof(tlp_ctx->virtio_dev[i].pcie_dev.dw_regs));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Channel LU: send dw_regs[%u] failed", i);
			goto rollback_stopped;
		}
	}

	DOCA_LOG_INFO("Channel LU active: config sent (%u devices, %u bridges, %u ep), waiting for CH_END",
		      ch_cfg.num_devices,
		      ch_cfg.num_bridges,
		      ch_cfg.num_ep);

	/* On UDS a successful send() guarantees the data reaches the peer's
	 * kernel buffer, so if the standby's set_primary(true) + CH_END send
	 * both succeeded we will receive CH_END here.  A timeout or recv
	 * failure means the socket broke, which also means the standby's
	 * CH_END send failed -- the standby will have reverted primary and
	 * destroyed its channel.  Safe to reclaim on the active side. */
	result = vnet_lu_wait_readable(conn_fd, VNET_LU_CH_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: timeout waiting for CH_END");
		goto rollback_stopped;
	}
	result = vnet_lu_recv_msg(conn_fd, &msg);
	if (result != DOCA_SUCCESS || msg != VNET_LU_MSG_CH_END) {
		DOCA_LOG_ERR("Channel LU: expected CH_END, got %u", (unsigned)msg);
		result = (result == DOCA_SUCCESS) ? DOCA_ERROR_IO_FAILED : result;
		goto rollback_stopped;
	}

	result = vnet_lu_recv_all(conn_fd, &handover_ok, sizeof(handover_ok));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: failed to receive handover status");
		goto rollback_stopped;
	}

	if (!handover_ok) {
		DOCA_LOG_WARN("Channel LU: standby reports failure, rolling back");
		vnet_lu_channel_reclaim(tlp_ctx);
		result = vnet_lu_send_msg(conn_fd, VNET_LU_MSG_CH_END_ACK);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Channel LU: failed to send CH_END_ACK after rollback: %s",
				      doca_error_get_descr(result));
		return DOCA_ERROR_IO_FAILED;
	}

	result = vnet_lu_send_msg(conn_fd, VNET_LU_MSG_CH_END_ACK);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("Channel LU: failed to send CH_END_ACK: %s", doca_error_get_descr(result));

	result = doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("Channel LU: channel destroy failed: %s", doca_error_get_descr(result));
	tlp_ctx->tlp_channel = NULL;

	DOCA_LOG_INFO("Channel LU active: handover complete, channel destroyed");
	return DOCA_SUCCESS;

rollback_stopped:
	/* Channel already stopped + non-primary; attempt to reclaim ownership. */
	DOCA_LOG_WARN("Channel LU active: post-stop send/recv failed, rolling back");
	vnet_lu_channel_reclaim(tlp_ctx);
	return result;

rollback_primary:
	(void)doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, true);
send_begin_nack:
	(void)vnet_lu_send_msg(conn_fd, VNET_LU_MSG_NACK);
	return result;
}

/*********************************************************************************************************************
 * Standby -- connect to active, restore state from SHM, SHM accessors
 *********************************************************************************************************************/

static doca_error_t vnet_lu_open_shm(struct vnet_lu_shm **shm_out, size_t *shm_size_out)
{
	struct vnet_lu_shm *shm;
	size_t file_size, full_size;
	struct stat st;
	int fd;

	fd = shm_open(VNET_LU_SHM_NAME, O_RDONLY, 0);
	if (fd < 0) {
		DOCA_LOG_ERR("shm_open(%s) failed: %s", VNET_LU_SHM_NAME, strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}
	if (fstat(fd, &st) != 0) {
		DOCA_LOG_ERR("fstat(%s) failed: %s", VNET_LU_SHM_NAME, strerror(errno));
		close(fd);
		return DOCA_ERROR_IO_FAILED;
	}

	file_size = (size_t)st.st_size;

	if (file_size < sizeof(struct vnet_lu_shm)) {
		DOCA_LOG_ERR("SHM too small: %zu < %zu", file_size, sizeof(struct vnet_lu_shm));
		close(fd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	shm = (struct vnet_lu_shm *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (shm == MAP_FAILED) {
		DOCA_LOG_ERR("mmap(%zu) failed: %s", file_size, strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}

	if (shm->num_devices == 0 || shm->num_devices > MAX_NUM_EP) {
		DOCA_LOG_ERR("SHM invalid: num_devices=%u", shm->num_devices);
		munmap(shm, file_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	full_size = sizeof(struct vnet_lu_shm) + shm->num_devices * sizeof(struct vnet_lu_device_state);
	if (file_size < full_size) {
		DOCA_LOG_ERR("SHM truncated: %zu < %zu (need %u devices)", file_size, full_size, shm->num_devices);
		munmap(shm, file_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (uint32_t i = 0; i < shm->num_devices; i++) {
		const struct vnet_lu_device_state *ds = &shm->devices[i];

		if (ds->blob_len > 0 &&
		    ((size_t)ds->blob_offset + ds->blob_len > file_size || ds->blob_offset < full_size)) {
			DOCA_LOG_ERR("SHM device %u: blob out of bounds (offset=%u, len=%u, file=%zu)",
				     i,
				     ds->blob_offset,
				     ds->blob_len,
				     file_size);
			munmap(shm, file_size);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	*shm_out = shm;
	*shm_size_out = file_size;
	DOCA_LOG_INFO("SHM opened: %u devices, state=%d", shm->num_devices, shm->state);
	return DOCA_SUCCESS;
}

static doca_error_t vnet_lu_connect_to_active(int *conn_fd)
{
	struct sockaddr_un addr;
	int fd;

	/* Acquire an exclusive lock to ensure only one standby at a time */
	vnet_lu_lock_fd = open(VNET_LU_LOCK_PATH, O_CREAT | O_RDWR, 0600);
	if (vnet_lu_lock_fd < 0) {
		DOCA_LOG_ERR("open(%s) failed: %s", VNET_LU_LOCK_PATH, strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}
	if (flock(vnet_lu_lock_fd, LOCK_EX | LOCK_NB) < 0) {
		DOCA_LOG_ERR("Another standby is already waiting; only one standby allowed");
		close(vnet_lu_lock_fd);
		vnet_lu_lock_fd = -1;
		return DOCA_ERROR_IN_USE;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		DOCA_LOG_ERR("socket() failed: %s", strerror(errno));
		close(vnet_lu_lock_fd);
		vnet_lu_lock_fd = -1;
		return DOCA_ERROR_IO_FAILED;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, VNET_LU_SOCK_PATH, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		DOCA_LOG_ERR("connect(%s) failed: %s", VNET_LU_SOCK_PATH, strerror(errno));
		close(fd);
		close(vnet_lu_lock_fd);
		vnet_lu_lock_fd = -1;
		return DOCA_ERROR_IO_FAILED;
	}

	*conn_fd = fd;
	return DOCA_SUCCESS;
}

/**
 * Reconstruct a doca_dev from an imported cmd_fd using public APIs.
 * ibv_import_device() takes ownership of cmd_fd on success.
 * Stores the ibv handles in tlp_ctx for cleanup after doca_dev_close().
 */
static doca_error_t vnet_lu_open_dev_from_cmd_fd(struct tlp_context *tlp_ctx, int cmd_fd)
{
	struct ibv_context *ibv_ctx;
	struct ibv_pd *pd;
	doca_error_t result;

	ibv_ctx = ibv_import_device(cmd_fd);
	if (ibv_ctx == NULL) {
		DOCA_LOG_ERR("ibv_import_device(cmd_fd=%d) failed: %s", cmd_fd, strerror(errno));
		return DOCA_ERROR_DRIVER;
	}

	pd = ibv_alloc_pd(ibv_ctx);
	if (pd == NULL) {
		DOCA_LOG_ERR("ibv_alloc_pd() failed: %s", strerror(errno));
		result = DOCA_ERROR_NO_MEMORY;
		goto close_ctx;
	}

	result = doca_rdma_bridge_open_dev_from_pd(pd, &tlp_ctx->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_rdma_bridge_open_dev_from_pd() failed: %s", doca_error_get_descr(result));
		goto dealloc_pd;
	}

	tlp_ctx->imported_ibv_pd = pd;
	tlp_ctx->imported_ibv_ctx = ibv_ctx;
	return DOCA_SUCCESS;

dealloc_pd:
	(void)ibv_dealloc_pd(pd);
close_ctx:
	(void)ibv_close_device(ibv_ctx);
	return result;
}

/* Phase 1 (pre-copy): receive cmd_fd, open SHM, reconstruct doca_dev.
 * Called while App_A is still serving traffic. */
doca_error_t vnet_lu_restore_early(struct vnet_pci_dev_resources *resources)
{
	const struct vnet_lu_device_state *ds0;
	struct vnet_lu_shm *shm = NULL;
	doca_error_t result;
	size_t shm_size = 0;
	int cmd_fd = -1;

	if (!resources || !resources->tlp_ctx) {
		DOCA_LOG_ERR("'resources' or 'tlp_ctx' is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Restore early 1/4: connecting to active");
	result = vnet_lu_connect_to_active(&vnet_lu_standby_conn_fd);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("Restore early 2/4: waiting for cmd_fd from active");
	result = vnet_lu_wait_readable(vnet_lu_standby_conn_fd, VNET_LU_STANDBY_IDLE_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting for LU trigger from active");
		goto cleanup_conn;
	}

	result = vnet_lu_recv_cmd_fd(vnet_lu_standby_conn_fd, &cmd_fd);
	if (result != DOCA_SUCCESS)
		goto cleanup_conn;

	DOCA_LOG_INFO("Restore early 3/4: opening SHM");
	result = vnet_lu_open_shm(&shm, &shm_size);
	if (result != DOCA_SUCCESS)
		goto cleanup_conn;

	for (uint32_t i = 0; i < shm->num_devices; i++) {
		const struct vnet_lu_device_state *ds = &shm->devices[i];

		if (ds->magic != VNET_LU_MAGIC) {
			DOCA_LOG_ERR("Device %u: invalid magic (0x%x)", i, ds->magic);
			munmap(shm, shm_size);
			result = DOCA_ERROR_INVALID_VALUE;
			goto cleanup_conn;
		}
		DOCA_LOG_DBG("Device %u: ep_vhca=0x%x, vqs=%u, active_qps=%u",
			     i,
			     ds->ep_vhca_id,
			     ds->vqs.num_vqs,
			     ds->virtio.num_active_qps);
	}

	DOCA_LOG_INFO("Restore early 4/4: reconstructing doca_dev");
	result = vnet_lu_open_dev_from_cmd_fd(resources->tlp_ctx, cmd_fd);
	if (result != DOCA_SUCCESS) {
		munmap(shm, shm_size);
		goto cleanup_conn;
	}
	cmd_fd = -1; /* now owned by ibv_context */

	ds0 = &shm->devices[0];
	if (ds0->net.mtu > 0)
		resources->tlp_ctx->mtu = ds0->net.mtu;
	if (ds0->virtio.max_queue_pairs == 0 || ds0->virtio.queue_size == 0) {
		DOCA_LOG_ERR("SHM device 0: invalid config (max_queue_pairs=%u, queue_size=%u)",
			     ds0->virtio.max_queue_pairs,
			     ds0->virtio.queue_size);
		munmap(shm, shm_size);
		result = DOCA_ERROR_INVALID_VALUE;
		goto cleanup_conn;
	}
	resources->tlp_ctx->max_queue_pairs = ds0->virtio.max_queue_pairs;
	resources->tlp_ctx->queue_size = ds0->virtio.queue_size;
	memcpy(resources->tlp_ctx->mac_bytes_base, ds0->net.mac, ETH_ALEN);

	vnet_lu_saved_shm = shm;
	vnet_lu_saved_shm_size = shm_size;
	return DOCA_SUCCESS;

cleanup_conn:
	if (vnet_lu_standby_conn_fd >= 0) {
		close(vnet_lu_standby_conn_fd);
		vnet_lu_standby_conn_fd = -1;
	}
	if (cmd_fd >= 0)
		close(cmd_fd);
	return result;
}

const struct vnet_lu_shm *vnet_lu_get_shm(void)
{
	return vnet_lu_saved_shm;
}

void vnet_lu_release_shm(void)
{
	if (vnet_lu_saved_shm) {
		munmap(vnet_lu_saved_shm, vnet_lu_saved_shm_size);
		vnet_lu_saved_shm = NULL;
		vnet_lu_saved_shm_size = 0;
	}
}

doca_error_t vnet_lu_get_ep_vhca_id(uint32_t idx, uint16_t *vhca_id_out)
{
	if (!vnet_lu_saved_shm || !vhca_id_out || idx >= vnet_lu_saved_shm->num_devices)
		return DOCA_ERROR_INVALID_VALUE;

	*vhca_id_out = vnet_lu_saved_shm->devices[idx].ep_vhca_id;
	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_get_import_desc(uint32_t idx, const void **export_desc, size_t *export_desc_len)
{
	const struct vnet_lu_device_state *ds;

	if (!vnet_lu_saved_shm || !export_desc || !export_desc_len)
		return DOCA_ERROR_INVALID_VALUE;

	if (idx >= vnet_lu_saved_shm->num_devices)
		return DOCA_ERROR_INVALID_VALUE;

	ds = &vnet_lu_saved_shm->devices[idx];
	if (ds->blob_len == 0 || ds->blob_offset == 0)
		return DOCA_ERROR_NOT_FOUND;

	*export_desc = (const uint8_t *)vnet_lu_saved_shm + ds->blob_offset;
	*export_desc_len = ds->blob_len;
	return DOCA_SUCCESS;
}

/* Phase 1 end: notify active that pre-copy init is done. */
doca_error_t vnet_lu_phase1_send_ready(void)
{
	if (vnet_lu_standby_conn_fd < 0) {
		DOCA_LOG_ERR("No connection to active process (send_ready)");
		return DOCA_ERROR_BAD_STATE;
	}

	return vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_DEV_READY);
}

/* Phase 2 end: send final DEV_ACK to active. */
doca_error_t vnet_lu_restore_complete(void)
{
	doca_error_t result;

	if (vnet_lu_standby_conn_fd < 0) {
		DOCA_LOG_ERR("No connection to active process (restore_complete)");
		return DOCA_ERROR_BAD_STATE;
	}

	result = vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_DEV_ACK);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send DEV_ACK: %s", doca_error_get_descr(result));
		close(vnet_lu_standby_conn_fd);
		vnet_lu_standby_conn_fd = -1;
		return result;
	}

	/* Close deferred to vnet_lu_close_conn(); early close races with active. */
	(void)shm_unlink(VNET_LU_SHM_NAME);
	return DOCA_SUCCESS;
}

void vnet_lu_close_conn(void)
{
	if (vnet_lu_standby_conn_fd >= 0) {
		close(vnet_lu_standby_conn_fd);
		vnet_lu_standby_conn_fd = -1;
	}
	if (vnet_lu_lock_fd >= 0) {
		close(vnet_lu_lock_fd);
		vnet_lu_lock_fd = -1;
		(void)unlink(VNET_LU_LOCK_PATH);
	}
}

doca_error_t vnet_lu_find_existing_rep(struct doca_devemu_pci_type *pci_type,
				       uint16_t target_vhca_id,
				       struct doca_dev_rep **rep_out)
{
	struct doca_devinfo_rep **rep_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	result = doca_devemu_pci_type_create_rep_list(pci_type, &rep_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU: failed to list existing representors: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("LU: scanning %u existing representor(s) for vhca_id=0x%x", nb_devs, target_vhca_id);

	for (uint32_t i = 0; i < nb_devs; i++) {
		uint16_t vhca_id = 0;

		result = doca_devinfo_rep_get_vhca_id(rep_list[i], &vhca_id);
		if (result != DOCA_SUCCESS)
			continue;

		if (vhca_id == target_vhca_id) {
			DOCA_LOG_DBG("LU: found matching rep at index %u (vhca_id=0x%x)", i, vhca_id);
			result = doca_dev_rep_open(rep_list[i], rep_out);
			doca_devinfo_rep_destroy_list(rep_list);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("LU: failed to open existing rep: %s", doca_error_get_descr(result));
			return result;
		}
	}

	DOCA_LOG_WARN("LU: no existing rep matched vhca_id=0x%x (found %u reps)", target_vhca_id, nb_devs);
	doca_devinfo_rep_destroy_list(rep_list);
	return DOCA_ERROR_NOT_FOUND;
}

void vnet_lu_override_config(struct vnet_pci_dev_config *config, const struct tlp_context *tlp_ctx, uint8_t *mac_bytes)
{
	config->max_queue_pairs = tlp_ctx->max_queue_pairs;
	config->queue_size = tlp_ctx->queue_size;
	config->mtu = tlp_ctx->mtu;
	memcpy(mac_bytes, tlp_ctx->mac_bytes_base, ETH_ALEN);
	snprintf(config->mac_addr,
		 sizeof(config->mac_addr),
		 "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac_bytes[0],
		 mac_bytes[1],
		 mac_bytes[2],
		 mac_bytes[3],
		 mac_bytes[4],
		 mac_bytes[5]);

	DOCA_LOG_INFO("LU standby: config overridden from SHM (qps=%u, qs=%u, mtu=%u, mac=%s)",
		      config->max_queue_pairs,
		      config->queue_size,
		      config->mtu,
		      config->mac_addr);
}

/*********************************************************************************************************************
 * Replay -- restore device state on App_B from SHM
 *********************************************************************************************************************/

static void vnet_lu_drain_pe(struct doca_pe *pe)
{
	for (int i = 0; i < VNET_PCI_DEV_PE_DRAIN_MAX_ITERATIONS; i++) {
		if (doca_pe_progress(pe) == 0)
			break;
	}
}

static doca_error_t vnet_lu_init_io_context(struct vnet_pci_dev_controller *ctrl, uint32_t idx)
{
	struct doca_devemu_virtio_io *virtio_io;
	struct doca_ctx *ctx;
	doca_error_t result;

	result = doca_devemu_vnet_io_create_from_offload_engine(ctrl->offload_engine, &ctrl->io_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: IO context create failed: %s", doca_error_get_descr(result));
		return result;
	}

	virtio_io = doca_devemu_vnet_io_as_virtio_io(ctrl->io_ctx);
	if (!virtio_io) {
		DOCA_LOG_ERR("LU replay: vnet_io_as_virtio_io returned NULL for device %u", idx);
		result = DOCA_ERROR_UNEXPECTED;
		goto err_destroy;
	}
	ctx = doca_devemu_virtio_io_as_ctx(virtio_io);
	if (!ctx) {
		DOCA_LOG_ERR("LU replay: virtio_io_as_ctx returned NULL for device %u", idx);
		result = DOCA_ERROR_UNEXPECTED;
		goto err_destroy;
	}

	result = doca_pe_connect_ctx(ctrl->worker_pe, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: PE connect IO ctx failed: %s", doca_error_get_descr(result));
		goto err_destroy;
	}

	/* Per the DOCA API contract, the ctrl_req handler must be registered while the IO ctx
	 * is idle (i.e. before doca_ctx_start()). */
	result = doca_devemu_vnet_io_event_vnet_ctrl_req_register(ctrl->io_ctx, vnet_pci_dev_ctrl_req_handler);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: ctrl_req handler register failed: %s", doca_error_get_descr(result));
		goto err_destroy;
	}

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: IO ctx start failed: %s", doca_error_get_descr(result));
		goto err_destroy;
	}

	ctrl->io_ctx_started = true;
	atomic_store(&ctrl->initialization_in_progress, false);
	DOCA_LOG_DBG("LU replay: IO context initialized for device %u", idx);
	return DOCA_SUCCESS;

err_destroy:
	doca_devemu_vnet_io_destroy(ctrl->io_ctx);
	ctrl->io_ctx = NULL;
	return result;
}

static doca_error_t vnet_lu_validate_replay_params(const struct vnet_pci_dev_controller *ctrl,
						   const struct vnet_lu_device_state *ds,
						   const struct vnet_pci_device *vdev)
{
	if (ds->virtio.max_queue_pairs != ctrl->max_queue_pairs) {
		DOCA_LOG_ERR("LU replay: max_queue_pairs mismatch: SHM=%u, allocated=%u",
			     ds->virtio.max_queue_pairs,
			     ctrl->max_queue_pairs);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (ds->virtio.num_active_qps > ds->virtio.max_queue_pairs) {
		DOCA_LOG_ERR("LU replay: num_active_qps (%u) > max_queue_pairs (%u)",
			     ds->virtio.num_active_qps,
			     ds->virtio.max_queue_pairs);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (ds->vqs.num_vqs > vdev->vqs_count) {
		DOCA_LOG_ERR("LU replay: num_vqs (%u) > vqs_count (%u)", ds->vqs.num_vqs, vdev->vqs_count);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (ds->vqs.num_vqs > VNET_TOTAL_VQS(VNET_CTRL_MAX_QUEUES_PAIRS)) {
		DOCA_LOG_ERR("LU replay: num_vqs (%u) > static array limit (%u)",
			     ds->vqs.num_vqs,
			     VNET_TOTAL_VQS(VNET_CTRL_MAX_QUEUES_PAIRS));
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

static doca_error_t vnet_lu_replay_one_device(struct vnet_pci_dev_controller *ctrl,
					      const struct vnet_lu_device_state *ds,
					      uint32_t idx)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	struct vnet_virtio_common_config *common_cfg;
	struct vnet_pci_device *vdev;
	/* Non-const: replay must write VQ shadow config restored from SHM */
	struct vnet_virtio_queue_config *vq_shadow;
	doca_error_t result;

	vdev = atomic_load(&ctrl->virtio_device);
	if (!vdev) {
		DOCA_LOG_ERR("LU replay: virtio_device is NULL for device %u", idx);
		return DOCA_ERROR_UNEXPECTED;
	}
	common_cfg = vnet_pci_device_get_pci_cfg(vdev);
	vq_shadow = (struct vnet_virtio_queue_config *)vnet_pci_device_get_virtq_pci_cfg(vdev);
	if (!common_cfg || !vq_shadow) {
		DOCA_LOG_ERR("LU replay: failed to get PCI config for device %u", idx);
		return DOCA_ERROR_UNEXPECTED;
	}

	DOCA_LOG_INFO("LU replay: device %u (ep_vhca=0x%x, active_qps=%u, mq=%d)",
		      idx,
		      ds->ep_vhca_id,
		      ds->virtio.num_active_qps,
		      ds->virtio.mq_feature_negotiated);

	result = vnet_lu_validate_replay_params(ctrl, ds, vdev);
	if (result != DOCA_SUCCESS)
		return result;

	ctrl->mq_feature_negotiated = ds->virtio.mq_feature_negotiated;
	atomic_store(&ctrl->num_active_qps, ds->virtio.num_active_qps);
	ctrl->max_queue_pairs = ds->virtio.max_queue_pairs;
	vdev->driver_features = ds->virtio.device_features;
	common_cfg->device_status = VNET_VIRTIO_DEVICE_STATUS_ACK | VNET_VIRTIO_DEVICE_STATUS_DRIVER |
				    VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK | VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK;
	vdev->prev_status = common_cfg->device_status;

	for (uint16_t q = 0; q < ds->vqs.num_vqs && q < vdev->vqs_count; q++) {
		vq_shadow[q].queue_size = ds->vqs.vqs[q].size;
		vq_shadow[q].queue_msix_vector = ds->vqs.vqs[q].msix_vector;
		vq_shadow[q].queue_desc = ds->vqs.vqs[q].desc_addr;
		vq_shadow[q].queue_driver = ds->vqs.vqs[q].driver_addr;
		vq_shadow[q].queue_device = ds->vqs.vqs[q].device_addr;
		vq_shadow[q].queue_enable = ds->vqs.vqs[q].enabled;
	}

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine);
	if (!virtio_engine) {
		DOCA_LOG_ERR("LU replay: failed to get virtio engine for device %u", idx);
		return DOCA_ERROR_UNEXPECTED;
	}
	result = doca_devemu_virtio_offload_engine_start(virtio_engine);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: engine start failed for device %u: %s", idx, doca_error_get_descr(result));
		return result;
	}
	atomic_store(&ctrl->offload_engine_started, true);

	vnet_lu_drain_pe(ctrl->worker_pe);

	result = vnet_pci_dev_initialize_vqs(ctrl);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: initialize VQs failed for device %u: %s", idx, doca_error_get_descr(result));
		return result;
	}

	if (ctrl->mq_feature_negotiated) {
		result = vnet_lu_init_io_context(ctrl, idx);
		if (result != DOCA_SUCCESS)
			return result;
	}

	vnet_lu_drain_pe(ctrl->worker_pe);

	result = vnet_pci_dev_start_vqs(ctrl, vdev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU replay: start_vqs failed for device %u: %s", idx, doca_error_get_descr(result));
		return result;
	}

	vnet_lu_drain_pe(ctrl->worker_pe);

	DOCA_LOG_INFO("LU replay: device %u VQs prepared (engine not yet enabled)", idx);
	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_dev_lu_replay(struct vnet_pci_dev_resources *resources,
				    const struct vnet_lu_device_state *dev_states,
				    uint32_t num_devices)
{
	doca_error_t result;

	DOCA_LOG_INFO("LU replay: restoring %u devices from SHM state", num_devices);

	for (uint32_t i = 0; i < num_devices; i++) {
		result = vnet_lu_replay_one_device(&resources->tlp_ctx->vnet_controller[i], &dev_states[i], i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("LU replay: device %u failed: %s (%u/%u restored)",
				     i,
				     doca_error_get_descr(result),
				     i,
				     num_devices);
			return result;
		}
	}

	for (uint32_t i = 0; i < num_devices; i++) {
		struct vnet_pci_dev_controller *ctrl = &resources->tlp_ctx->vnet_controller[i];
		uint16_t sf_vhca_id = 0;

		if (ctrl->offload_engine && vnet_lu_get_sf_vhca_id(ctrl->offload_engine, &sf_vhca_id) == DOCA_SUCCESS)
			DOCA_LOG_DBG("LU replay: device %u SF vhca_id=0x%x (post-restore)", i, sf_vhca_id);
	}

	DOCA_LOG_INFO("LU replay complete: %u devices restored", num_devices);
	return DOCA_SUCCESS;
}

doca_error_t vnet_lu_apply_shm_replay(struct vnet_pci_dev_resources *resources, struct vnet_pci_dev_config *config)
{
	const struct vnet_lu_shm *shm = vnet_lu_get_shm();
	doca_error_t result;

	if (!shm) {
		DOCA_LOG_ERR("LU standby: SHM not available for replay");
		return DOCA_ERROR_BAD_STATE;
	}

	if (shm->num_devices == 0 || shm->num_devices > resources->tlp_ctx->num_ep) {
		DOCA_LOG_ERR("LU: SHM num_devices (%u) exceeds allocated EPs (%u)",
			     shm->num_devices,
			     resources->tlp_ctx->num_ep);
		vnet_lu_release_shm();
		return DOCA_ERROR_INVALID_VALUE;
	}

	config->num_ep = shm->num_devices;
	resources->tlp_ctx->num_ep = shm->num_devices;

	result = vnet_pci_dev_lu_replay(resources, shm->devices, shm->num_devices);
	vnet_lu_release_shm();
	return result;
}

struct lu_enable_arg {
	struct vnet_pci_dev_controller *ctrl;
	uint32_t idx;
	doca_error_t result;
};

static void *lu_enable_thread_fn(void *arg)
{
	struct lu_enable_arg *a = arg;
	struct vnet_pci_dev_controller *ctrl = a->ctrl;

	a->result = vnet_pci_dev_enable_engine(ctrl);
	if (a->result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU phase2: enable engine failed for device %u: %s",
			     a->idx,
			     doca_error_get_descr(a->result));
		return NULL;
	}

	if (ctrl->vnet_counters == NULL) {
		doca_error_t cnt_result = doca_devemu_vnet_counters_create(ctrl->offload_engine, &ctrl->vnet_counters);
		if (cnt_result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to create VNET counters for device %u: %s",
				      a->idx,
				      doca_error_get_descr(cnt_result));
			ctrl->vnet_counters = NULL;
		} else {
			(void)doca_devemu_vnet_counters_reset(ctrl->vnet_counters);
		}
	}

	vnet_lu_drain_pe(ctrl->worker_pe);
	return NULL;
}

doca_error_t vnet_lu_phase2_enable_engines(struct vnet_pci_dev_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;
	uint32_t num_ep = resources->tlp_ctx->num_ep;
	pthread_t threads[MAX_NUM_EP];
	struct lu_enable_arg args[MAX_NUM_EP];
	uint32_t spawned = 0;

	DOCA_LOG_INFO("LU phase2: enabling %u engines in parallel", num_ep);

	/* Read per-device DEV_GO, spawn concurrent enable thread for each. */
	for (uint32_t i = 0; i < num_ep; i++) {
		enum vnet_lu_msg msg;

		result = vnet_lu_wait_readable(vnet_lu_standby_conn_fd, VNET_LU_ACK_TIMEOUT_SEC);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("LU phase2: timeout waiting for DEV_GO (device %u)", i);
			goto join;
		}
		result = vnet_lu_recv_msg(vnet_lu_standby_conn_fd, &msg);
		if (result != DOCA_SUCCESS || msg != VNET_LU_MSG_DEV_GO) {
			DOCA_LOG_ERR("LU phase2: expected DEV_GO for device %u, got %u", i, (unsigned)msg);
			result = DOCA_ERROR_IO_FAILED;
			goto join;
		}
		args[i].ctrl = &resources->tlp_ctx->vnet_controller[i];
		args[i].idx = i;
		args[i].result = DOCA_SUCCESS;

		if (pthread_create(&threads[i], NULL, lu_enable_thread_fn, &args[i]) != 0) {
			DOCA_LOG_ERR("LU phase2: failed to create enable thread for device %u", i);
			result = DOCA_ERROR_OPERATING_SYSTEM;
			goto join;
		}
		spawned++;
	}

join:
	for (uint32_t i = 0; i < spawned; i++) {
		pthread_join(threads[i], NULL);
		if (args[i].result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("LU phase2: device %u enable thread failed: %s",
				     i,
				     doca_error_get_descr(args[i].result));
			if (result == DOCA_SUCCESS)
				result = args[i].result;
		}
	}

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("LU phase2: enable failed -- %u/%u threads spawned, "
			     "devices 0..%u received DEV_GO. No fallback, partial state.",
			     spawned,
			     num_ep,
			     spawned > 0 ? spawned - 1 : 0);
		for (uint32_t i = 0; i < spawned; i++) {
			DOCA_LOG_WARN("LU phase2: device %u: %s",
				      i,
				      args[i].result == DOCA_SUCCESS ? "enabled (OK)" : "FAILED");
		}
		for (uint32_t i = spawned; i < num_ep; i++)
			DOCA_LOG_WARN("LU phase2: device %u: not started (no DEV_GO received)", i);
		return result;
	}

	DOCA_LOG_INFO("LU phase2: all %u engines enabled -- traffic restored", num_ep);
	return DOCA_SUCCESS;
}

/*********************************************************************************************************************
 * Channel LU -- standby-side: receive export, create secondary channel, apply config, become primary
 *********************************************************************************************************************/

doca_error_t vnet_lu_channel_restore(struct vnet_pci_dev_resources *resources)
{
	struct tlp_context *tlp_ctx = resources->tlp_ctx;
	struct vnet_lu_channel_config ch_cfg;
	union doca_data user_data = {0};
	uint8_t success_flag = 0;
	enum vnet_lu_msg msg;
	void *export_desc = NULL;
	uint32_t export_desc_len = 0;
	doca_error_t end_result;
	doca_error_t result;

	/* Channel LU reuses the socket and doca_dev established during device LU.
	 * No separate SETUP phase needed -- proceeds directly to EXPORT. */

	DOCA_LOG_INFO("Channel LU standby: waiting for CH_EXPORT");

	result = vnet_lu_wait_readable(vnet_lu_standby_conn_fd, VNET_LU_CH_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: timeout waiting for CH_EXPORT");
		return result;
	}
	result = vnet_lu_recv_msg(vnet_lu_standby_conn_fd, &msg);
	if (result != DOCA_SUCCESS || msg != VNET_LU_MSG_CH_EXPORT) {
		DOCA_LOG_ERR("Channel LU: expected CH_EXPORT, got %u", (unsigned)msg);
		return (result == DOCA_SUCCESS) ? DOCA_ERROR_IO_FAILED : result;
	}

	result = vnet_lu_recv_all(vnet_lu_standby_conn_fd, &export_desc_len, sizeof(export_desc_len));
	if (result != DOCA_SUCCESS)
		return result;

	if (export_desc_len == 0 || export_desc_len > VNET_LU_CH_EXPORT_MAX_LEN) {
		DOCA_LOG_ERR("Channel LU: invalid export_desc_len=%u", export_desc_len);
		return DOCA_ERROR_INVALID_VALUE;
	}

	export_desc = malloc(export_desc_len);
	if (!export_desc) {
		DOCA_LOG_ERR("Channel LU: malloc(%u) failed", export_desc_len);
		return DOCA_ERROR_NO_MEMORY;
	}

	result = vnet_lu_recv_all(vnet_lu_standby_conn_fd, export_desc, export_desc_len);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	DOCA_LOG_INFO("Channel LU standby: received export (%u bytes)", export_desc_len);

	result = vnet_pci_dev_start_from_export(tlp_ctx, export_desc, export_desc_len, VNET_LU_CH_SHM_DIR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: start_from_export failed: %s", doca_error_get_descr(result));
		goto nack_cleanup;
	}

	result = doca_pe_connect_ctx(tlp_ctx->pe, vnet_pci_dev_tlp_channel_ctx(tlp_ctx));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: pe_connect_ctx failed: %s", doca_error_get_descr(result));
		goto nack_cleanup_channel;
	}

	user_data.ptr = tlp_ctx;
	result = doca_ctx_set_user_data(vnet_pci_dev_tlp_channel_ctx(tlp_ctx), user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: set_user_data failed: %s", doca_error_get_descr(result));
		goto nack_cleanup_channel;
	}

	result = doca_ctx_start(vnet_pci_dev_tlp_channel_ctx(tlp_ctx));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: ctx_start failed: %s", doca_error_get_descr(result));
		goto nack_cleanup_channel;
	}

	DOCA_LOG_INFO("Channel LU standby: secondary channel started, sending CH_BEGIN");

	result = vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_CH_BEGIN);
	if (result != DOCA_SUCCESS)
		goto cleanup_channel;

	result = vnet_lu_wait_readable(vnet_lu_standby_conn_fd, VNET_LU_CH_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: timeout waiting for CH_BEGIN_ACK");
		goto send_ch_end_fail;
	}
	result = vnet_lu_recv_msg(vnet_lu_standby_conn_fd, &msg);
	if (result != DOCA_SUCCESS || msg != VNET_LU_MSG_CH_BEGIN_ACK) {
		DOCA_LOG_ERR("Channel LU: expected CH_BEGIN_ACK, got %u", (unsigned)msg);
		result = (result == DOCA_SUCCESS) ? DOCA_ERROR_IO_FAILED : result;
		goto send_ch_end_fail;
	}

	result = vnet_lu_recv_all(vnet_lu_standby_conn_fd, &ch_cfg, sizeof(ch_cfg));
	if (result != DOCA_SUCCESS)
		goto send_ch_end_fail;

	if (ch_cfg.num_devices > (MAX_NUM_BRIDGE + MAX_NUM_EP) || ch_cfg.num_ep > MAX_NUM_EP) {
		DOCA_LOG_ERR("Channel LU: invalid topology: num_devices=%u (max %u), num_ep=%u (max %u)",
			     ch_cfg.num_devices,
			     MAX_NUM_BRIDGE + MAX_NUM_EP,
			     ch_cfg.num_ep,
			     MAX_NUM_EP);
		result = DOCA_ERROR_INVALID_VALUE;
		goto send_ch_end_fail;
	}

	/* Must match standby allocation: recv copies ch_cfg.transaction_region_size bytes per EP into
	 * transaction_region_memories[i]; apply overwrites tlp_ctx->transaction_region_size without realloc. */
	if (ch_cfg.transaction_region_size == 0 ||
	    (size_t)ch_cfg.transaction_region_size != tlp_ctx->transaction_region_size) {
		DOCA_LOG_ERR("Channel LU: transaction_region_size mismatch (active=%u standby=%zu)",
			     ch_cfg.transaction_region_size,
			     tlp_ctx->transaction_region_size);
		result = DOCA_ERROR_INVALID_VALUE;
		goto send_ch_end_fail;
	}

	DOCA_LOG_INFO("Channel LU standby: received config (%u devices, %u bridges, %u ep, region_size=%u)",
		      ch_cfg.num_devices,
		      ch_cfg.num_bridges,
		      ch_cfg.num_ep,
		      ch_cfg.transaction_region_size);

	vnet_lu_apply_channel_config(&ch_cfg, tlp_ctx);

	/* Receive and apply per-endpoint transaction regions */
	for (uint32_t i = 0; i < ch_cfg.num_ep && i < tlp_ctx->num_ep; i++) {
		if (!tlp_ctx->transaction_region_memories || !tlp_ctx->transaction_region_memories[i] ||
		    ch_cfg.transaction_region_size == 0)
			continue;
		result = vnet_lu_recv_all(vnet_lu_standby_conn_fd,
					  tlp_ctx->transaction_region_memories[i],
					  ch_cfg.transaction_region_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Channel LU: recv transaction region %u failed", i);
			goto send_ch_end_fail;
		}
	}

	/* Receive per-endpoint VirtIO PCI config space (dw_regs[]).
	 * Restores host-written BAR addresses, command register, and capability
	 * state into the VirtIO layer's config space shadow. */
	for (uint32_t i = 0; i < ch_cfg.num_ep && i < tlp_ctx->num_ep; i++) {
		result = vnet_lu_recv_all(vnet_lu_standby_conn_fd,
					  tlp_ctx->virtio_dev[i].pcie_dev.dw_regs,
					  sizeof(tlp_ctx->virtio_dev[i].pcie_dev.dw_regs));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Channel LU: recv dw_regs[%u] failed", i);
			goto send_ch_end_fail;
		}
	}

	result = doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Channel LU: set_primary(true) failed: %s", doca_error_get_descr(result));
		goto send_ch_end_fail;
	}

	DOCA_LOG_INFO("Channel LU standby: became primary, sending CH_END (success)");

	/* Send CH_END + success flag so active can destroy its channel */
	end_result = vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_CH_END);
	if (end_result != DOCA_SUCCESS) {
		(void)doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, false);
		result = end_result;
		goto cleanup_channel;
	}
	success_flag = 1;
	end_result = vnet_lu_send_all(vnet_lu_standby_conn_fd, &success_flag, sizeof(success_flag));
	if (end_result != DOCA_SUCCESS) {
		(void)doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, false);
		result = end_result;
		goto cleanup_channel;
	}

	/* Wait for CH_END_ACK -- non-fatal if missing (we are already primary) */
	result = vnet_lu_wait_readable(vnet_lu_standby_conn_fd, VNET_LU_CH_TIMEOUT_SEC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Channel LU: timeout waiting for CH_END_ACK (already primary, continuing)");
		result = DOCA_SUCCESS;
	} else {
		doca_error_t ack_result = vnet_lu_recv_msg(vnet_lu_standby_conn_fd, &msg);

		if (ack_result != DOCA_SUCCESS || msg != VNET_LU_MSG_CH_END_ACK)
			DOCA_LOG_WARN("Channel LU: unexpected CH_END_ACK response (got %u)", (unsigned)msg);
	}

	DOCA_LOG_INFO("Channel LU standby: handover complete");
	goto cleanup;

send_ch_end_fail:
	/* Notify active of failure so it can rollback to primary */
	(void)vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_CH_END);
	success_flag = 0;
	(void)vnet_lu_send_all(vnet_lu_standby_conn_fd, &success_flag, sizeof(success_flag));

nack_cleanup_channel:
	(void)vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_NACK);
	goto cleanup_channel;

nack_cleanup:
	(void)vnet_lu_send_msg(vnet_lu_standby_conn_fd, VNET_LU_MSG_NACK);
	goto cleanup;

cleanup_channel:
	/* Tear down the standby-side channel so the process does not carry
	 * stale channel state into chainable-active mode. */
	if (tlp_ctx->tlp_channel) {
		(void)doca_ctx_stop(vnet_pci_dev_tlp_channel_ctx(tlp_ctx));
		doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
	}

cleanup:
	free(export_desc);
	return result;
}
