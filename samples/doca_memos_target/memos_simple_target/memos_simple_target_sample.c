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

/*
 * Brings up a single DOCA Memos target subsystem that exposes one namespace,
 * backed by a regular file, over a single NVMe-oF RDMA port. Runs until
 * interrupted with Ctrl-C.
 *
 * Built entirely on the DOCA Memos target public API together with the DOCA
 * progress engine, polled from a single thread.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_types.h>

#include <doca_memos_target.h>
#include <doca_memos_target_io.h>
#include <doca_memos_target_mgmt.h>
#include <doca_memos_target_port.h>
#include <doca_memos_target_subsys.h>

DOCA_LOG_REGISTER(MEMOS_SIMPLE_TARGET);

/*
 * NVMe-oF specification values for the transport type and address family, as
 * defined by the NVMe-oF specification and validated by the DOCA Memos target
 * library. They are defined locally so the sample depends only on the DOCA
 * Memos target public API.
 */
#define MEMOS_NVMF_TRTYPE_RDMA 0x1
#define MEMOS_NVMF_ADRFAM_IPV4 0x1
#define MEMOS_NVMF_ADRFAM_IPV6 0x2

/* Subsystem / namespace defaults. */
#define MEMOS_DEFAULT_NQN "nqn.2026-06.com.nvidia:memos0"
#define MEMOS_DEFAULT_BLOCK_SIZE 4096
#define MEMOS_DEFAULT_MAX_VALUE_LEN (512 * 1024)
#define MEMOS_DEFAULT_NSID 1

/* Fallback buffer alignment (log2) if the capability query is unavailable. */
#define MEMOS_DEFAULT_LOG_ALIGN 6

/* Per-namespace backing store context, referenced via ns_user_data. */
struct memos_backing {
	int fd;
	uint32_t block_size;
	uint64_t num_blocks;
};

/* Tracks the completion of a single asynchronous subsystem operation. */
struct memos_async_op {
	bool done;
	doca_error_t status;
};

/* Sample state. */
struct memos_simple_target_app {
	struct doca_dev **devs; /* every opened Memos target capable device */
	uint32_t num_devs;	/* number of entries in 'devs' */
	struct doca_pe *pe;
	struct doca_memos_target_mgmt *mgmt;
	struct doca_memos_target_io *io;
	struct doca_memos_target_port *port;
	struct doca_memos_target_subsys *subsys;
	struct doca_memos_target_subsys_ns *ns;
	struct doca_memos_target_backend *backend;
	struct memos_backing backing;
	/* Reused across the (non-concurrent) namespace attach/detach, backend add/rm and subsystem start/stop
	 * operations; the event handlers for these operations are registered once, while the subsystem is idle,
	 * and carry no per-operation user data of their own.
	 */
	struct memos_async_op ns_op;
	struct memos_async_op backend_op;
	struct memos_async_op subsys_op;
	bool target_initialized;
	bool target_started;
};

/* Buffer alignment requested when registering the buffer allocator. */
static uint8_t g_buffer_log_align = MEMOS_DEFAULT_LOG_ALIGN;

/* Set from the signal handler to request a graceful shutdown. */
static volatile sig_atomic_t g_force_quit;

/*
 * Signal handler - request the main loop to stop.
 *
 * @signum [in]: received signal number
 */
static void signal_handler(int signum)
{
	(void)signum;
	g_force_quit = 1;
}

/*
 * Install handlers for SIGINT and SIGTERM.
 */
static void install_signal_handlers(void)
{
	struct sigaction sa = {0};

	sa.sa_handler = signal_handler;
	(void)sigemptyset(&sa.sa_mask);

	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);
}

/*
 * Buffer allocator used by the DOCA Memos target for internal DOCA buffers.
 *
 * @size [in]: size in bytes to allocate
 * @ctx [in]: unused user context
 * @return: aligned pointer on success, NULL on failure
 */
static void *memos_buffer_alloc(size_t size, void *ctx)
{
	size_t alignment = (size_t)1 << g_buffer_log_align;
	void *ptr = NULL;

	(void)ctx;

	if (posix_memalign(&ptr, alignment, size) != 0)
		return NULL;

	return ptr;
}

/*
 * Free a buffer previously returned by memos_buffer_alloc().
 *
 * @ptr [in]: buffer to release
 * @ctx [in]: unused user context
 */
static void memos_buffer_free(void *ptr, void *ctx)
{
	(void)ctx;
	free(ptr);
}

/*
 * Completion handler for asynchronous port add/remove and subsystem start/stop operations.
 *
 * The same handler covers all four operations; each call is correlated through its per-operation user
 * data.
 */
static void async_op_completion_cb(struct doca_memos_target_subsys *subsys,
				   union doca_data reg_user_data,
				   union doca_data op_user_data,
				   doca_error_t status)
{
	struct memos_async_op *op = (struct memos_async_op *)op_user_data.ptr;

	(void)subsys;
	(void)reg_user_data;

	if (op != NULL) {
		op->status = status;
		op->done = true;
	}
}

/*
 * Success handler for namespace attach/detach operations. The same handler covers both operations;
 * 'reg_user_data' points at the single (non-concurrent) namespace operation tracker.
 */
static void ns_op_success_cb(struct doca_memos_target_subsys *subsys,
			     struct doca_memos_target_subsys_ns *ns,
			     union doca_data reg_user_data)
{
	struct memos_async_op *op = (struct memos_async_op *)reg_user_data.ptr;

	(void)subsys;
	(void)ns;

	op->status = DOCA_SUCCESS;
	op->done = true;
}

/*
 * Error handler for namespace attach/detach operations. The specific failure reason is not reported
 * to this callback; only that the operation did not succeed.
 */
static void ns_op_error_cb(struct doca_memos_target_subsys *subsys,
			   struct doca_memos_target_subsys_ns *ns,
			   union doca_data reg_user_data)
{
	struct memos_async_op *op = (struct memos_async_op *)reg_user_data.ptr;

	(void)subsys;
	(void)ns;

	op->status = DOCA_ERROR_UNKNOWN;
	op->done = true;
}

/*
 * Success handler for backend add/remove operations. The same handler covers both operations;
 * 'reg_user_data' points at the single (non-concurrent) backend operation tracker.
 */
static void backend_op_success_cb(struct doca_memos_target_subsys_ns *ns,
				  struct doca_memos_target_backend *backend,
				  union doca_data reg_user_data)
{
	struct memos_async_op *op = (struct memos_async_op *)reg_user_data.ptr;

	(void)ns;
	(void)backend;

	op->status = DOCA_SUCCESS;
	op->done = true;
}

/*
 * Error handler for backend add/remove operations. The specific failure reason is not reported to
 * this callback; only that the operation did not succeed.
 */
static void backend_op_error_cb(struct doca_memos_target_subsys_ns *ns,
				struct doca_memos_target_backend *backend,
				union doca_data reg_user_data)
{
	struct memos_async_op *op = (struct memos_async_op *)reg_user_data.ptr;

	(void)ns;
	(void)backend;

	op->status = DOCA_ERROR_UNKNOWN;
	op->done = true;
}

/*
 * Resolve a request's backing store and validate its offset/length against the namespace.
 *
 * @backend [in]: backend the request is targeting
 * @op [in]: request kind, for logging ("READ" or "WRITE")
 * @offset [in]: request offset in bytes
 * @data_len [in]: request length in bytes
 * @iovcnt [in]: request iovec count
 * @backing [out]: resolved backing store
 * @return: DOCA_SUCCESS if the request is in range, DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t req_resolve_backing(struct doca_memos_target_backend *backend,
					const char *op,
					uint64_t offset,
					uint32_t data_len,
					uint32_t iovcnt,
					struct memos_backing **backing)
{
	union doca_data backend_user_data;
	uint64_t ns_capacity;

	(void)doca_memos_target_backend_get_user_data(backend, &backend_user_data);
	*backing = (struct memos_backing *)backend_user_data.ptr;
	ns_capacity = (uint64_t)(*backing)->num_blocks * (*backing)->block_size;

	if (iovcnt > (uint32_t)INT_MAX || offset > (uint64_t)INT64_MAX || offset > ns_capacity ||
	    data_len > ns_capacity - offset) {
		DOCA_LOG_ERR("%s request out of range: offset %" PRIu64 " len %u iovcnt %u, "
			     "namespace capacity %" PRIu64,
			     op,
			     offset,
			     data_len,
			     iovcnt,
			     ns_capacity);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * WRITE request handler - copy the request data into the backing file.
 */
static void write_req_handler(struct doca_memos_target_req *req,
			      void *req_user_data,
			      struct doca_memos_target_backend *backend,
			      struct doca_memos_target_subsys_ns *ns,
			      uint64_t offset,
			      uint32_t data_len)
{
	struct memos_backing *backing;
	struct iovec *iov = doca_memos_target_req_get_iovec(req);
	uint32_t iovcnt = doca_memos_target_req_get_iovcnt(req);
	doca_error_t status;
	ssize_t written;

	(void)req_user_data;
	(void)ns;

	status = req_resolve_backing(backend, "WRITE", offset, data_len, iovcnt, &backing);
	if (status != DOCA_SUCCESS) {
		doca_memos_target_req_complete(req, status);
		return;
	}

	written = pwritev(backing->fd, iov, (int)iovcnt, (off_t)offset);
	if (written < 0 || (uint32_t)written != data_len) {
		DOCA_LOG_ERR("WRITE failed at offset %" PRIu64 " len %u: %s", offset, data_len, strerror(errno));
		status = DOCA_ERROR_IO_FAILED;
	}

	doca_memos_target_req_complete(req, status);
}

/*
 * READ request handler - fill the request buffers from the backing file.
 */
static void read_req_handler(struct doca_memos_target_req *req,
			     void *req_user_data,
			     struct doca_memos_target_backend *backend,
			     struct doca_memos_target_subsys_ns *ns,
			     uint64_t offset,
			     uint32_t data_len)
{
	struct memos_backing *backing;
	struct iovec *iov = doca_memos_target_req_get_iovec(req);
	uint32_t iovcnt = doca_memos_target_req_get_iovcnt(req);
	doca_error_t status;
	ssize_t bytes_read;

	(void)req_user_data;
	(void)ns;

	status = req_resolve_backing(backend, "READ", offset, data_len, iovcnt, &backing);
	if (status != DOCA_SUCCESS) {
		doca_memos_target_req_complete(req, status);
		return;
	}

	bytes_read = preadv(backing->fd, iov, (int)iovcnt, (off_t)offset);
	if (bytes_read < 0 || (uint32_t)bytes_read != data_len) {
		DOCA_LOG_ERR("READ failed at offset %" PRIu64 " len %u: %s", offset, data_len, strerror(errno));
		status = DOCA_ERROR_IO_FAILED;
	}

	doca_memos_target_req_complete(req, status);
}

/*
 * Open and register every DOCA device that reports DOCA Memos target support.
 *
 * The sample only receives the transport address (IP) on the command line.
 * The NVMe-oF RDMA listener resolves that IP to the underlying RDMA device on
 * its own, which is not necessarily the first capable device in the list. To
 * make sure whichever device backs the configured IP is available to the target,
 * every capable device is opened and registered (rather than just one).
 *
 * @app [in/out]: sample state (opened devices are recorded in 'devs')
 * @return: DOCA_SUCCESS if at least one device was registered, error otherwise
 */
static doca_error_t memos_add_devices(struct memos_simple_target_app *app)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;
	uint32_t i;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to list DOCA devices: %s", doca_error_get_descr(result));
		return result;
	}

	if (nb_devs == 0) {
		doca_devinfo_destroy_list(dev_list);
		return DOCA_ERROR_NOT_FOUND;
	}

	app->devs = calloc(nb_devs, sizeof(*app->devs));
	if (app->devs == NULL) {
		doca_devinfo_destroy_list(dev_list);
		return DOCA_ERROR_NO_MEMORY;
	}

	for (i = 0; i < nb_devs; i++) {
		uint8_t supported = 0;
		struct doca_dev *dev = NULL;

		result = doca_memos_target_cap_is_supported(dev_list[i], &supported);
		if (result != DOCA_SUCCESS || !supported)
			continue;

		result = doca_dev_open(dev_list[i], &dev);
		if (result != DOCA_SUCCESS)
			continue;

		result = doca_memos_target_add_dev(dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("doca_memos_target_add_dev() unavailable (%s); skipping a device",
				      doca_error_get_descr(result));
			(void)doca_dev_close(dev);
			continue;
		}

		app->devs[app->num_devs++] = dev;
	}

	doca_devinfo_destroy_list(dev_list);

	return (app->num_devs > 0) ? DOCA_SUCCESS : DOCA_ERROR_NOT_FOUND;
}

/*
 * Poll the progress engine until a DOCA context reaches the target state.
 *
 * @app [in]: sample state (holds the progress engine)
 * @ctx [in]: context to wait on
 * @target_state [in]: desired state
 * @return: DOCA_SUCCESS once the state is reached, error otherwise
 */
static doca_error_t wait_ctx_state(struct memos_simple_target_app *app,
				   struct doca_ctx *ctx,
				   enum doca_ctx_states target_state)
{
	enum doca_ctx_states state;
	doca_error_t result;

	do {
		(void)doca_pe_progress(app->pe);

		result = doca_ctx_get_state(ctx, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query context state: %s", doca_error_get_descr(result));
			return result;
		}

		if (g_force_quit)
			return DOCA_ERROR_UNEXPECTED;
	} while (state != target_state);

	return DOCA_SUCCESS;
}

/*
 * Poll the progress engine until an asynchronous operation completes.
 *
 * @app [in]: sample state (holds the progress engine)
 * @op [in]: operation completion tracker
 * @return: the completion status of the operation
 */
static doca_error_t wait_async_op(struct memos_simple_target_app *app, struct memos_async_op *op)
{
	while (!op->done) {
		(void)doca_pe_progress(app->pe);

		if (g_force_quit)
			return DOCA_ERROR_UNEXPECTED;
	}

	return op->status;
}

/*
 * Open and size the backing file for the namespace.
 *
 * @app [in/out]: sample state (backing context is populated)
 * @path [in]: backing file path
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t backing_open(struct memos_simple_target_app *app, const char *path)
{
	struct memos_backing *backing = &app->backing;
	struct stat st;

	backing->fd = open(path, O_RDWR);
	if (backing->fd < 0) {
		DOCA_LOG_ERR("Failed to open backing file '%s': %s", path, strerror(errno));
		return DOCA_ERROR_NOT_FOUND;
	}

	if (fstat(backing->fd, &st) != 0) {
		DOCA_LOG_ERR("Failed to stat backing file '%s': %s", path, strerror(errno));
		close(backing->fd);
		backing->fd = -1;
		return DOCA_ERROR_IO_FAILED;
	}

	backing->block_size = MEMOS_DEFAULT_BLOCK_SIZE;
	backing->num_blocks = (uint64_t)st.st_size / backing->block_size;
	if (backing->num_blocks == 0) {
		DOCA_LOG_ERR("Backing file '%s' is smaller than one block (%u bytes)", path, backing->block_size);
		close(backing->fd);
		backing->fd = -1;
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Backing file '%s': block_size=%u num_blocks=%" PRIu64,
		      path,
		      backing->block_size,
		      backing->num_blocks);

	return DOCA_SUCCESS;
}

/*
 * Close the backing file.
 *
 * @app [in/out]: sample state
 */
static void backing_close(struct memos_simple_target_app *app)
{
	if (app->backing.fd >= 0) {
		close(app->backing.fd);
		app->backing.fd = -1;
	}
}

/*
 * Create and start the single target-level IO context.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t io_start(struct memos_simple_target_app *app)
{
	struct doca_ctx *io_ctx;
	doca_error_t result;

	result = doca_memos_target_io_create(&app->io);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IO context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_io_event_write_req(app->io, write_req_handler);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register WRITE handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_io_event_read_req(app->io, read_req_handler);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register READ handler: %s", doca_error_get_descr(result));
		return result;
	}

	io_ctx = doca_memos_target_io_as_ctx(app->io);

	result = doca_pe_connect_ctx(app->pe, io_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect IO context to PE: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(io_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start IO context: %s", doca_error_get_descr(result));
		return result;
	}

	result = wait_ctx_state(app, io_ctx, DOCA_CTX_STATE_RUNNING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("IO context did not reach running state: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create and start the target management context.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mgmt_start(struct memos_simple_target_app *app)
{
	struct doca_ctx *mgmt_ctx;
	doca_error_t result;

	result = doca_memos_target_mgmt_create(&app->mgmt);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create management context: %s", doca_error_get_descr(result));
		return result;
	}

	mgmt_ctx = doca_memos_target_mgmt_as_ctx(app->mgmt);

	result = doca_pe_connect_ctx(app->pe, mgmt_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect management context to PE: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(mgmt_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start management context: %s", doca_error_get_descr(result));
		return result;
	}

	result = wait_ctx_state(app, mgmt_ctx, DOCA_CTX_STATE_RUNNING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Management context did not reach running state: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Derive the NVMe-oF address family from a transport address.
 *
 * @traddr [in]: NVMe-oF transport address (IPv4 or IPv6)
 * @adrfam [out]: NVMe-oF address family of the address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE if 'traddr' is neither
 */
static doca_error_t traddr_get_adrfam(const char *traddr, uint8_t *adrfam)
{
	struct in6_addr addr;

	if (inet_pton(AF_INET, traddr, &addr) == 1) {
		*adrfam = MEMOS_NVMF_ADRFAM_IPV4;
		return DOCA_SUCCESS;
	}

	if (inet_pton(AF_INET6, traddr, &addr) == 1) {
		*adrfam = MEMOS_NVMF_ADRFAM_IPV6;
		return DOCA_SUCCESS;
	}

	DOCA_LOG_ERR("Transport address '%s' is neither an IPv4 nor an IPv6 address", traddr);
	return DOCA_ERROR_INVALID_VALUE;
}

/*
 * Create the target-level listener port. The target must already be started.
 *
 * @app [in/out]: sample state
 * @traddr [in]: NVMe-oF transport address (IP)
 * @trsvcid [in]: NVMe-oF transport service id (port number)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t port_create(struct memos_simple_target_app *app, const char *traddr, const char *trsvcid)
{
	struct doca_memos_target_port_create_attr *attr = NULL;
	uint8_t adrfam;
	doca_error_t result;

	result = traddr_get_adrfam(traddr, &adrfam);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_memos_target_port_create_attr_create(&attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create port attributes: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_port_create_attr_set_trtype(attr, MEMOS_NVMF_TRTYPE_RDMA);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port transport type: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_port_create_attr_set_adrfam(attr, adrfam);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port address family: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_port_create_attr_set_traddr(attr, traddr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port transport address: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_port_create_attr_set_trsvcid(attr, trsvcid);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port transport service id: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_port_create(attr, &app->port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create port: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	DOCA_LOG_INFO("Listening on RDMA %s:%s", traddr, trsvcid);

destroy_attr:
	(void)doca_memos_target_port_create_attr_destroy(attr);
	return result;
}

/*
 * Bring up the DOCA Memos target subsystem.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t subsystem_start(struct memos_simple_target_app *app)
{
	union doca_data reg_user_data = {.ptr = NULL};
	union doca_data ns_op_reg_user_data = {.ptr = &app->ns_op};
	union doca_data backend_op_reg_user_data = {.ptr = &app->backend_op};
	union doca_data subsys_op_user_data = {.ptr = &app->subsys_op};
	doca_error_t result;

	result = doca_memos_target_subsys_create(&app->subsys);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create subsystem: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_subsys_set_nqn(app->subsys, MEMOS_DEFAULT_NQN);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set subsystem NQN: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_ns_attach(app->subsys,
								   ns_op_success_cb,
								   ns_op_error_cb,
								   ns_op_reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register namespace attach handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_ns_detach(app->subsys,
								   ns_op_success_cb,
								   ns_op_error_cb,
								   ns_op_reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register namespace detach handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_backend_add(app->subsys,
								     backend_op_success_cb,
								     backend_op_error_cb,
								     backend_op_reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register backend add handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_backend_rm(app->subsys,
								    backend_op_success_cb,
								    backend_op_error_cb,
								    backend_op_reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register backend remove handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_port_add(app->subsys, async_op_completion_cb, reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register port add handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_port_rm(app->subsys, async_op_completion_cb, reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register port remove handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_start(app->subsys, async_op_completion_cb, reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register subsystem start handler: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_register_subsys_event_stop(app->subsys, async_op_completion_cb, reg_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register subsystem stop handler: %s", doca_error_get_descr(result));
		return result;
	}

	app->subsys_op = (struct memos_async_op){0};
	result = doca_memos_target_subsys_start(app->subsys, subsys_op_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit subsystem start: %s", doca_error_get_descr(result));
		return result;
	}

	result = wait_async_op(app, &app->subsys_op);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Subsystem did not reach running state: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Subsystem '%s' is running", MEMOS_DEFAULT_NQN);

	return DOCA_SUCCESS;
}

/*
 * Add the listener port to the running subsystem.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t subsystem_add_port(struct memos_simple_target_app *app)
{
	struct memos_async_op op = {0};
	union doca_data op_user_data = {.ptr = &op};
	doca_error_t result;

	result = doca_memos_target_subsys_add_port(app->subsys, app->port, op_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit add port: %s", doca_error_get_descr(result));
		return result;
	}

	result = wait_async_op(app, &op);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Add port failed: %s", doca_error_get_descr(result));

	return result;
}

/*
 * Create a backend describing the backing file.
 *
 * @backing [in]: backing file geometry, also stored as the backend user data
 * @backend [out]: created backend
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t backend_create(struct memos_backing *backing, struct doca_memos_target_backend **backend)
{
	struct doca_memos_target_backend_create_attr *attr;
	union doca_data user_data = {.ptr = backing};
	doca_error_t result;

	result = doca_memos_target_backend_create_attr_create(&attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create backend attributes: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_backend_create_attr_set_block_size(attr, backing->block_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set backend block size: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_backend_create_attr_set_num_blocks(attr, backing->num_blocks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set backend number of blocks: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	/* A regular file reports no write granularity or optimal write size, so use the block size. */
	result = doca_memos_target_backend_create_attr_set_write_granularity(attr, backing->block_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set backend write granularity: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_backend_create_attr_set_optimal_write_size(attr, backing->block_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set backend optimal write size: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_backend_create_attr_set_user_data(attr, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set backend user data: %s", doca_error_get_descr(result));
		goto destroy_attr;
	}

	result = doca_memos_target_backend_create(attr, backend);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create backend: %s", doca_error_get_descr(result));

destroy_attr:
	(void)doca_memos_target_backend_create_attr_destroy(attr);
	return result;
}

/*
 * Add the file-backed namespace to the running subsystem.
 *
 * @app [in]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t subsystem_add_ns(struct memos_simple_target_app *app)
{
	const uint8_t nguid[DOCA_MEMOS_TARGET_SUBSYS_NGUID_LEN] = {0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	doca_error_t result;

	result = doca_memos_target_subsys_ns_create(app->subsys, &app->ns);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create namespace: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_subsys_ns_set_ns_id(app->ns, MEMOS_DEFAULT_NSID);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set namespace ID: %s", doca_error_get_descr(result));
		goto destroy_ns;
	}

	result = doca_memos_target_subsys_ns_set_nguid(app->ns, nguid);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set namespace NGUID: %s", doca_error_get_descr(result));
		goto destroy_ns;
	}

	result = doca_memos_target_subsys_ns_set_max_value_len(app->ns, MEMOS_DEFAULT_MAX_VALUE_LEN);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set namespace max Value length: %s", doca_error_get_descr(result));
		goto destroy_ns;
	}

	result = doca_memos_target_subsys_ns_set_size(app->ns,
						      (uint64_t)app->backing.num_blocks * app->backing.block_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set namespace size: %s", doca_error_get_descr(result));
		goto destroy_ns;
	}

	result = doca_memos_target_subsys_ns_start(app->ns);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start namespace: %s", doca_error_get_descr(result));
		goto destroy_ns;
	}

	result = backend_create(&app->backing, &app->backend);
	if (result != DOCA_SUCCESS)
		goto stop_ns;

	app->backend_op = (struct memos_async_op){0};
	result = doca_memos_target_subsys_ns_add_backend(app->ns, app->backend);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit add backend: %s", doca_error_get_descr(result));
		goto destroy_backend;
	}

	result = wait_async_op(app, &app->backend_op);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Add backend failed: %s", doca_error_get_descr(result));
		goto destroy_backend;
	}

	app->ns_op = (struct memos_async_op){0};
	result = doca_memos_target_subsys_attach_ns(app->subsys, app->ns);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit attach namespace: %s", doca_error_get_descr(result));
		goto rm_backend;
	}

	result = wait_async_op(app, &app->ns_op);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Attach namespace failed: %s", doca_error_get_descr(result));
		goto rm_backend;
	}

	DOCA_LOG_INFO("Namespace %u attached (%" PRIu64 " blocks of %u bytes)",
		      MEMOS_DEFAULT_NSID,
		      app->backing.num_blocks,
		      app->backing.block_size);

	return DOCA_SUCCESS;

rm_backend:
	app->backend_op = (struct memos_async_op){0};
	if (doca_memos_target_subsys_ns_rm_backend(app->ns, app->backend) == DOCA_SUCCESS)
		(void)wait_async_op(app, &app->backend_op);
destroy_backend:
	(void)doca_memos_target_backend_destroy(app->backend);
	app->backend = NULL;
stop_ns:
	(void)doca_memos_target_subsys_ns_stop(app->ns);
destroy_ns:
	(void)doca_memos_target_subsys_ns_destroy(app->ns);
	app->ns = NULL;
	return result;
}

/*
 * Remember the first failure of a teardown. Teardown always runs to the end so that everything that can be
 * released is, and the first failure is what the sample reports.
 *
 * @first [in/out]: first failure seen so far
 * @result [in]: outcome of the step
 */
static void record_first_error(doca_error_t *first, doca_error_t result)
{
	if (*first == DOCA_SUCCESS)
		*first = result;
}

/*
 * Log and remember the failure of a teardown step, if it failed.
 *
 * @first [in/out]: first failure seen so far
 * @result [in]: outcome of the step
 * @what [in]: description of the step, for the log
 */
static void teardown_check(doca_error_t *first, doca_error_t result, const char *what)
{
	if (result == DOCA_SUCCESS)
		return;

	DOCA_LOG_ERR("Teardown failed to %s: %s", what, doca_error_get_descr(result));
	record_first_error(first, result);
}

/*
 * Stop a DOCA context and wait for it to become idle.
 *
 * @app [in]: sample state (holds the progress engine)
 * @ctx [in]: context to stop
 * @return: DOCA_SUCCESS once the context is idle, error otherwise
 */
static doca_error_t ctx_stop(struct memos_simple_target_app *app, struct doca_ctx *ctx)
{
	doca_error_t result = doca_ctx_stop(ctx);

	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS)
		return result;

	return wait_ctx_state(app, ctx, DOCA_CTX_STATE_IDLE);
}

/*
 * Tear down the subsystem: detach namespace and port, stop and destroy the subsystem.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS if every step succeeded, the first failure otherwise
 */
static doca_error_t subsystem_teardown(struct memos_simple_target_app *app)
{
	doca_error_t status = DOCA_SUCCESS;
	doca_error_t result;

	if (app->subsys == NULL)
		return DOCA_SUCCESS;

	/* Remove the port from the subsystem. The port itself, and its listener, outlive this. */
	if (app->port != NULL) {
		union doca_data op_user_data = {.ptr = &app->subsys_op};

		app->subsys_op = (struct memos_async_op){0};
		result = doca_memos_target_subsys_rm_port(app->subsys, app->port, op_user_data);
		if (result == DOCA_SUCCESS)
			result = wait_async_op(app, &app->subsys_op);
		teardown_check(&status, result, "remove the port from the subsystem");
	}

	/* Detach the namespace, remove and destroy its backend, then stop and destroy it. */
	if (app->ns != NULL) {
		app->ns_op = (struct memos_async_op){0};
		result = doca_memos_target_subsys_detach_ns(app->subsys, app->ns);
		if (result == DOCA_SUCCESS)
			result = wait_async_op(app, &app->ns_op);
		teardown_check(&status, result, "detach the namespace");

		if (app->backend != NULL) {
			app->backend_op = (struct memos_async_op){0};
			result = doca_memos_target_subsys_ns_rm_backend(app->ns, app->backend);
			if (result == DOCA_SUCCESS)
				result = wait_async_op(app, &app->backend_op);
			teardown_check(&status, result, "remove the backend from the namespace");

			teardown_check(&status, doca_memos_target_backend_destroy(app->backend), "destroy the backend");
			app->backend = NULL;
		}

		teardown_check(&status, doca_memos_target_subsys_ns_stop(app->ns), "stop the namespace");
		teardown_check(&status, doca_memos_target_subsys_ns_destroy(app->ns), "destroy the namespace");
		app->ns = NULL;
	}

	/* Stop the subsystem and wait for it to become idle. */
	app->subsys_op = (struct memos_async_op){0};
	result = doca_memos_target_subsys_stop(app->subsys, (union doca_data){.ptr = &app->subsys_op});
	if (result == DOCA_SUCCESS)
		result = wait_async_op(app, &app->subsys_op);
	teardown_check(&status, result, "stop the subsystem");

	teardown_check(&status, doca_memos_target_subsys_destroy(app->subsys), "destroy the subsystem");
	app->subsys = NULL;

	return status;
}

/*
 * Destroy the target-level listener port. Must be called after the port has been removed from every
 * subsystem.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t port_teardown(struct memos_simple_target_app *app)
{
	doca_error_t status = DOCA_SUCCESS;

	if (app->port == NULL)
		return DOCA_SUCCESS;

	teardown_check(&status, doca_memos_target_port_destroy(app->port), "destroy the port");
	app->port = NULL;

	return status;
}

/*
 * Stop and destroy the management context.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mgmt_teardown(struct memos_simple_target_app *app)
{
	doca_error_t status = DOCA_SUCCESS;

	if (app->mgmt == NULL)
		return DOCA_SUCCESS;

	teardown_check(&status, ctx_stop(app, doca_memos_target_mgmt_as_ctx(app->mgmt)), "stop the management context");
	teardown_check(&status, doca_memos_target_mgmt_destroy(app->mgmt), "destroy the management context");
	app->mgmt = NULL;

	return status;
}

/*
 * Stop and destroy the IO context.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t io_teardown(struct memos_simple_target_app *app)
{
	doca_error_t status = DOCA_SUCCESS;

	if (app->io == NULL)
		return DOCA_SUCCESS;

	teardown_check(&status, ctx_stop(app, doca_memos_target_io_as_ctx(app->io)), "stop the IO context");
	teardown_check(&status, doca_memos_target_io_destroy(app->io), "destroy the IO context");
	app->io = NULL;

	return status;
}

/*
 * Configure the buffer allocator and initialize the DOCA Memos target.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t target_init(struct memos_simple_target_app *app)
{
	doca_error_t result;
	uint8_t min_log_align = 0;

	result = doca_memos_target_set_max_ns_value_len(MEMOS_DEFAULT_MAX_VALUE_LEN);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max value length: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_memos_target_cap_get_min_buffer_log_align(&min_log_align);
	if (result == DOCA_SUCCESS && min_log_align > g_buffer_log_align)
		g_buffer_log_align = min_log_align;

	result = doca_memos_target_register_buffer_allocator(memos_buffer_alloc,
							     memos_buffer_free,
							     NULL,
							     1,
							     SIZE_MAX,
							     g_buffer_log_align);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register buffer allocator: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register every capable device: the one the configured IP resolves to is not known up front. */
	result = memos_add_devices(app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("No DOCA Memos target capable device could be registered: %s",
			     doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Registered %u DOCA Memos target capable device(s)", app->num_devs);

	result = doca_memos_target_init();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA Memos target: %s", doca_error_get_descr(result));
		return result;
	}

	app->target_initialized = true;
	return DOCA_SUCCESS;
}

/*
 * Release all DOCA Memos target resources.
 *
 * @app [in/out]: sample state
 * @return: DOCA_SUCCESS if every resource was released, the first failure otherwise
 */
static doca_error_t target_cleanup(struct memos_simple_target_app *app)
{
	doca_error_t status = DOCA_SUCCESS;
	uint32_t i;

	record_first_error(&status, subsystem_teardown(app));
	record_first_error(&status, port_teardown(app));

	if (app->target_started) {
		teardown_check(&status, doca_memos_target_stop(), "stop the target");
		app->target_started = false;
	}

	record_first_error(&status, mgmt_teardown(app));
	record_first_error(&status, io_teardown(app));

	if (app->target_initialized) {
		teardown_check(&status, doca_memos_target_teardown(), "tear down the target");
		app->target_initialized = false;
	}

	if (app->pe != NULL) {
		teardown_check(&status, doca_pe_destroy(app->pe), "destroy the progress engine");
		app->pe = NULL;
	}

	if (app->devs != NULL) {
		for (i = 0; i < app->num_devs; i++)
			teardown_check(&status, doca_dev_close(app->devs[i]), "close a device");
		free(app->devs);
		app->devs = NULL;
		app->num_devs = 0;
	}

	backing_close(app);

	return status;
}

/*
 * Bring up the DOCA Memos target, serve READ/WRITE requests against the backing file until
 * interrupted, then tear everything down.
 *
 * @traddr [in]: NVMe-oF transport address (IP) to listen on
 * @trsvcid [in]: NVMe-oF transport service id (port number)
 * @file_path [in]: backing file for the namespace
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t memos_simple_target_run(const char *traddr, const char *trsvcid, const char *file_path)
{
	struct memos_simple_target_app app = {0};
	doca_error_t cleanup_result;
	doca_error_t result;

	app.backing.fd = -1;

	install_signal_handlers();

	result = backing_open(&app, file_path);
	if (result != DOCA_SUCCESS)
		return result;

	result = target_init(&app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = doca_pe_create(&app.pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = io_start(&app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = mgmt_start(&app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = doca_memos_target_start();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA Memos target: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	app.target_started = true;

	result = port_create(&app, traddr, trsvcid);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = subsystem_start(&app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = subsystem_add_port(&app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = subsystem_add_ns(&app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	DOCA_LOG_INFO("DOCA Memos target running. Press Ctrl-C to stop.");

	while (!g_force_quit)
		(void)doca_pe_progress(app.pe);

	DOCA_LOG_INFO("Shutting down");
	g_force_quit = 0;

cleanup:
	cleanup_result = target_cleanup(&app);
	if (result == DOCA_SUCCESS)
		result = cleanup_result;

	return result;
}
