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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <doca_log.h>
#include <doca_error.h>
#include <doca_argp.h>
#include <doca_pe.h>

#include "verbs_common.h"
#include "common.h"

DOCA_LOG_REGISTER(GPUVERBS::COMMON);

/*
 * ARGP Callback - Set DB ring mode
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t nic_handler_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int nic_handler = *(uint32_t *)param;

	if (nic_handler < 0) {
		DOCA_LOG_ERR("NIC handler mode for DOCA Verbs must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (nic_handler == 0)
		verbs_cfg->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO;
	else if (nic_handler == 1)
		verbs_cfg->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_CPU_PROXY;
	else if (nic_handler == 2)
		verbs_cfg->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_GPU_SM_DB;
	else if (nic_handler == 3)
		verbs_cfg->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_GPU_SM_BF;
	else if (nic_handler == 4)
		verbs_cfg->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_GPU_SM_NO_DBR;
	else if (nic_handler == 5)
		verbs_cfg->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_CPU_PROXY_NO_DBR;
	else
		return DOCA_ERROR_INVALID_VALUE;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t nic_device_name_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(verbs_cfg->nic_device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle GPU PCIe address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpu_pcie_addr_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered GPU PCIe address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(verbs_cfg->gpu_pcie_addr, device_name, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set GID index
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gid_index_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int gid_index = *(uint32_t *)param;

	if (gid_index < 0) {
		DOCA_LOG_ERR("GID index for DOCA Verbs must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->gid_index = (uint32_t)gid_index;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - If it's client, then server IP is specified
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t client_param_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	char *server_ip_addr = (char *)param;
	struct in_addr addr;
	int len;

	len = strnlen(server_ip_addr, MAX_IP_ADDRESS_LEN);
	if (len == MAX_IP_ADDRESS_LEN) {
		DOCA_LOG_ERR("Entered server address exceeded the maximum size of %d", MAX_IP_ADDRESS_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (inet_pton(AF_INET, server_ip_addr, &addr) != 1) {
		DOCA_LOG_ERR("Entered server address %s is not a valid IPv4 address", server_ip_addr);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(verbs_cfg->server_ip_addr, server_ip_addr, len + 1);
	verbs_cfg->is_server = false;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Sent number of iterations
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t iters_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int iters = *(uint32_t *)param;

	if (iters <= 0) {
		DOCA_LOG_ERR("Number of iterations must be greater than 0, requested %d", iters);
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->num_iters = (uint32_t)iters;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set number of CUDA threads per CUDA kernel
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t threads_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int threads = *(uint32_t *)param;

	if (threads < 0 || (threads % VERBS_CUDA_BLOCK) != 0 ||
	    threads < (VERBS_CUDA_BLOCK * DOCA_GPUNETIO_VERBS_WARP_SIZE)) {
		DOCA_LOG_ERR("CUDA threads must be > 0, at least %d and divisible by %d, requested %d",
			     VERBS_CUDA_BLOCK * DOCA_GPUNETIO_VERBS_WARP_SIZE,
			     VERBS_CUDA_BLOCK,
			     threads);
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->cuda_threads = (uint32_t)threads;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Enable receive inline of 32B messages
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t recv_inline_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int inl = *(uint32_t *)param;

	if (inl == 0)
		verbs_cfg->recv_inline = 0;
	else
		verbs_cfg->recv_inline = 1;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set shared QP execution mode (THREAD or WARP)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t exec_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int exec_scope = *(uint32_t *)param;

	if (exec_scope != DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD && exec_scope != DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP) {
		DOCA_LOG_ERR("Execution scope must be %d (thread) or %d (warp), requested %d",
			     DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD,
			     DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP,
			     exec_scope);
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->exec_scope = (uint8_t)exec_scope;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Enable Data Direct
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t data_direct_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int enable_data_direct = *(int *)param;

#if !GPUNETIO_VERBS_DATA_DIRECT_SUPPORTED
	if (enable_data_direct != 0) {
		DOCA_LOG_ERR("Data Direct requires CUDA 12.8 or newer and Data Direct-capable libmlx5 headers");
		return DOCA_ERROR_NOT_SUPPORTED;
	}
#endif

	verbs_cfg->enable_data_direct = (enable_data_direct != 0);

	return DOCA_SUCCESS;
}

doca_error_t verbs_validate_config(const struct verbs_config *cfg, uint32_t cuda_blocks, uint32_t max_iters)
{
	uint32_t threads_per_block;
	int cuda_id = 0, max_block_threads = 0, cc_major = 0;
	cudaError_t cuda_ret;

	if (cfg->num_iters == 0) {
		DOCA_LOG_ERR("Number of iterations must be greater than 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (max_iters != 0 && cfg->num_iters > max_iters) {
		DOCA_LOG_ERR("Number of iterations %u exceeds the maximum of %u supported by this sample",
			     cfg->num_iters,
			     max_iters);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cuda_blocks == 0 || cfg->cuda_threads == 0 || (cfg->cuda_threads % cuda_blocks) != 0) {
		DOCA_LOG_ERR("Number of CUDA threads %u must be a non-zero multiple of the %u CUDA blocks",
			     cfg->cuda_threads,
			     cuda_blocks);
		return DOCA_ERROR_INVALID_VALUE;
	}
	threads_per_block = cfg->cuda_threads / cuda_blocks;

	/* Every CUDA thread must run the same number of iterations: the kernels synchronize the block in the loop */
	if ((cfg->num_iters % cfg->cuda_threads) != 0) {
		DOCA_LOG_ERR("Number of iterations %u must be a multiple of the number of CUDA threads %u",
			     cfg->num_iters,
			     cfg->cuda_threads);
		return DOCA_ERROR_INVALID_VALUE;
	}

	cuda_ret = cudaDeviceGetByPCIBusId(&cuda_id, cfg->gpu_pcie_addr);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Invalid GPU bus id provided %s", cfg->gpu_pcie_addr);
		return DOCA_ERROR_INVALID_VALUE;
	}

	cuda_ret = cudaDeviceGetAttribute(&max_block_threads, cudaDevAttrMaxThreadsPerBlock, cuda_id);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Failed to query GPU max threads per block: %s", cudaGetErrorString(cuda_ret));
		return DOCA_ERROR_DRIVER;
	}

	if (threads_per_block > (uint32_t)max_block_threads) {
		DOCA_LOG_ERR("%u CUDA threads over %u CUDA blocks exceed the GPU limit of %d threads per block",
			     cfg->cuda_threads,
			     cuda_blocks,
			     max_block_threads);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cfg->exec_scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP) {
		if ((threads_per_block % DOCA_GPUNETIO_VERBS_WARP_SIZE) != 0) {
			DOCA_LOG_ERR("Warp execution scope requires a multiple of %d CUDA threads per block, got %u",
				     DOCA_GPUNETIO_VERBS_WARP_SIZE,
				     threads_per_block);
			return DOCA_ERROR_INVALID_VALUE;
		}

		cuda_ret = cudaDeviceGetAttribute(&cc_major, cudaDevAttrComputeCapabilityMajor, cuda_id);
		if (cuda_ret != cudaSuccess) {
			DOCA_LOG_ERR("Failed to query GPU compute capability: %s", cudaGetErrorString(cuda_ret));
			return DOCA_ERROR_DRIVER;
		}

		/* The warp-scope device functions are compiled out below sm_80 */
		if (cc_major < 8) {
			DOCA_LOG_ERR("Warp execution scope requires a GPU with compute capability 8.0 or newer");
			return DOCA_ERROR_NOT_SUPPORTED;
		}
	}

	return DOCA_SUCCESS;
}

int verbs_oob_send(int sock_fd, const void *buf, size_t len)
{
	const uint8_t *ptr = (const uint8_t *)buf;
	ssize_t ret;

	while (len > 0) {
		/* MSG_NOSIGNAL: a peer that already exited must not kill this process with SIGPIPE */
		ret = send(sock_fd, ptr, len, MSG_NOSIGNAL);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		ptr += ret;
		len -= (size_t)ret;
	}

	return 0;
}

int verbs_oob_recv(int sock_fd, void *buf, size_t len)
{
	uint8_t *ptr = (uint8_t *)buf;
	ssize_t ret;

	while (len > 0) {
		ret = recv(sock_fd, ptr, len, 0);
		if (ret < 0 && errno == EINTR)
			continue;
		/* 0 means the peer closed the connection before sending everything */
		if (ret <= 0)
			return -1;
		ptr += ret;
		len -= (size_t)ret;
	}

	return 0;
}

doca_error_t verbs_oob_check_config(int sock_fd, const char *sample_name, const struct verbs_config *cfg)
{
	struct verbs_oob_config local = {0}, remote = {0};

	strncpy(local.sample_name, sample_name, sizeof(local.sample_name) - 1);
	local.num_iters = htonl(cfg->num_iters);
	local.cuda_threads = htonl(cfg->cuda_threads);
	local.recv_inline = htonl(cfg->recv_inline);

	if (verbs_oob_send(sock_fd, &local, sizeof(local)) != 0) {
		DOCA_LOG_ERR("Failed to send local configuration to remote peer");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (verbs_oob_recv(sock_fd, &remote, sizeof(remote)) != 0) {
		DOCA_LOG_ERR("Failed to receive remote peer configuration");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	remote.sample_name[sizeof(remote.sample_name) - 1] = '\0';
	if (strcmp(local.sample_name, remote.sample_name) != 0) {
		DOCA_LOG_ERR("Remote peer runs %s, this side runs %s", remote.sample_name, local.sample_name);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (local.num_iters != remote.num_iters || local.cuda_threads != remote.cuda_threads ||
	    local.recv_inline != remote.recv_inline) {
		DOCA_LOG_ERR(
			"Configuration mismatch with remote peer: local iters %u threads %u recv_inline %u, remote iters %u threads %u recv_inline %u. Both sides must use the same options",
			cfg->num_iters,
			cfg->cuda_threads,
			cfg->recv_inline,
			ntohl(remote.num_iters),
			ntohl(remote.cuda_threads),
			ntohl(remote.recv_inline));
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

bool verbs_oob_wait_peer_close(int sock_fd, volatile bool *force_quit)
{
	struct pollfd pfd = {.fd = sock_fd, .events = POLLIN};
	uint8_t byte;
	int ret;

	while (*force_quit == false) {
		/* Bounded wait so that a signal handler setting force_quit is noticed promptly */
		ret = poll(&pfd, 1, 100);
		if (ret < 0 && errno != EINTR)
			return false;
		if (ret <= 0)
			continue;

		/* The peer never sends data at this stage: readable means it closed or reset the connection */
		ret = recv(sock_fd, &byte, sizeof(byte), 0);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return true;
	}

	return false;
}

/*
 * OOB connection to exchange RDMA info - server side
 *
 * @oob_sock_fd [out]: Socket FD
 * @oob_client_sock [out]: Client socket FD
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t oob_verbs_connection_server_setup(int *oob_sock_fd, int *oob_client_sock)
{
	struct sockaddr_in server_addr = {0}, client_addr = {0};
	unsigned int client_size = 0;
	int enable = 1;
	int oob_sock_fd_ = 0;
	int oob_client_sock_ = 0;

	/* Create socket */
	oob_sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (oob_sock_fd_ < 0) {
		DOCA_LOG_ERR("Error while creating socket: %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Socket created successfully");

	/*
	 * SO_REUSEADDR lets a new server bind while the previous run's connection is in TIME_WAIT.
	 * SO_REUSEPORT is not used: it would let two servers bind the port and split the clients between them.
	 */
	if (setsockopt(oob_sock_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable))) {
		DOCA_LOG_ERR("Error setting socket options: %s", strerror(errno));
		close(oob_sock_fd_);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Set port and IP: */
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(VERBS_OOB_PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY; /* listen on any interface */

	/* Bind to the set port and IP: */
	if (bind(oob_sock_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		DOCA_LOG_ERR("Couldn't bind to port %d: %s", VERBS_OOB_PORT, strerror(errno));
		close(oob_sock_fd_);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Done with binding");

	/* Listen for clients: */
	if (listen(oob_sock_fd_, 1) < 0) {
		DOCA_LOG_ERR("Error while listening: %s", strerror(errno));
		close(oob_sock_fd_);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Listening for incoming connections");

	/* Accept an incoming connection: */
	client_size = sizeof(client_addr);
	oob_client_sock_ = accept(oob_sock_fd_, (struct sockaddr *)&client_addr, &client_size);
	if (oob_client_sock_ < 0) {
		DOCA_LOG_ERR("Can't accept socket connection: %s", strerror(errno));
		close(oob_sock_fd_);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	*(oob_sock_fd) = oob_sock_fd_;
	*(oob_client_sock) = oob_client_sock_;

	DOCA_LOG_INFO("Client connected at IP: %s and port: %i",
		      inet_ntoa(client_addr.sin_addr),
		      ntohs(client_addr.sin_port));

	return DOCA_SUCCESS;
}

/*
 * OOB connection to exchange RDMA info - server side closure
 *
 * @oob_sock_fd [in]: Socket FD
 * @oob_client_sock [in]: Client socket FD
 */
void oob_verbs_connection_server_close(int oob_sock_fd, int oob_client_sock)
{
	if (oob_client_sock >= 0)
		close(oob_client_sock);

	if (oob_sock_fd >= 0)
		close(oob_sock_fd);
}

/*
 * OOB connection to exchange RDMA info - client side
 *
 * @server_ip [in]: Server IP address to connect
 * @oob_sock_fd [out]: Socket FD
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t oob_verbs_connection_client_setup(const char *server_ip, int *oob_sock_fd)
{
	struct sockaddr_in server_addr = {0};
	int oob_sock_fd_;

	/* Set port and IP the same as server-side: */
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(VERBS_OOB_PORT);
	if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
		DOCA_LOG_ERR("Invalid server IP address %s", server_ip);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Create socket */
	oob_sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (oob_sock_fd_ < 0) {
		DOCA_LOG_ERR("Unable to create socket: %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Socket created successfully");

	/* Send connection request to server: */
	if (connect(oob_sock_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		DOCA_LOG_ERR("Unable to connect to server at %s:%d: %s", server_ip, VERBS_OOB_PORT, strerror(errno));
		close(oob_sock_fd_);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Connected with server successfully");

	*oob_sock_fd = oob_sock_fd_;
	return DOCA_SUCCESS;
}

/*
 * OOB connection to exchange RDMA info - client side closure
 *
 * @oob_sock_fd [in]: Socket FD
 */
void oob_verbs_connection_client_close(int oob_sock_fd)
{
	if (oob_sock_fd >= 0)
		close(oob_sock_fd);
}

/*
 * Open IB device via device name (e.g. mlx5_0)
 *
 * @name [in]: Device name
 * @return: doca context object
 */
static struct doca_verbs_context *open_ib_device(char *name)
{
	int nb_ibdevs = 0;
	struct ibv_device **ibdev_list = ibv_get_device_list(&nb_ibdevs);
	struct doca_verbs_context *context = NULL;
	doca_error_t status;
	bool found = false;

	if ((ibdev_list == NULL) || (nb_ibdevs == 0)) {
		DOCA_LOG_ERR("Failed to get RDMA devices list, ibdev_list null");
		if (ibdev_list != NULL)
			ibv_free_device_list(ibdev_list);
		return NULL;
	}

	for (int i = 0; i < nb_ibdevs; i++) {
		/* Exact match: a prefix match could open mlx5_10 when mlx5_1 is requested */
		if (strcmp(ibv_get_device_name(ibdev_list[i]), name) == 0) {
			found = true;
			/* The device must be opened before the list that owns it is freed */
			status = doca_verbs_bridge_verbs_context_create(ibdev_list[i],
									DOCA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
									&context);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to open IB device %s: %s", name, doca_error_get_descr(status));
				context = NULL;
			}
			break;
		}
	}

	if (!found)
		DOCA_LOG_ERR("IB device %s not found", name);

	ibv_free_device_list(ibdev_list);

	return context;
}

static doca_error_t create_verbs_ah_attr(struct doca_verbs_context *verbs_context,
					 uint32_t gid_index,
					 enum doca_verbs_addr_type addr_type,
					 struct doca_verbs_ah_attr **verbs_ah_attr)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_verbs_ah_attr *new_ah_attr = NULL;

	status = doca_verbs_ah_attr_create(verbs_context, &new_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs ah attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_ah_attr_set_addr_type(new_ah_attr, addr_type);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address type: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	status = doca_verbs_ah_attr_set_sgid_index(new_ah_attr, gid_index);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sgid index: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	if (addr_type == DOCA_VERBS_ADDR_TYPE_IPv4 || addr_type == DOCA_VERBS_ADDR_TYPE_IPv6) {
		status = doca_verbs_ah_attr_set_hop_limit(new_ah_attr, VERBS_TEST_HOP_LIMIT);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set hop limit: %s", doca_error_get_descr(status));
			goto destroy_verbs_ah;
		}
	}

	*verbs_ah_attr = new_ah_attr;

	return DOCA_SUCCESS;

destroy_verbs_ah:
	tmp_status = doca_verbs_ah_attr_destroy(new_ah_attr);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy doca verbs AH: %s", doca_error_get_descr(tmp_status));

	return status;
}

/*
 * Check that the NIC supports Data Direct
 *
 * @resources [in]: DOCA Verbs resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t check_data_direct(struct verbs_resources *resources)
{
#if GPUNETIO_VERBS_DATA_DIRECT_SUPPORTED
	char data_direct_path[IBV_SYSFS_PATH_MAX + 1] = {0};
	int data_direct_status;

	data_direct_status =
		mlx5dv_get_data_direct_sysfs_path(resources->pd->context, data_direct_path, sizeof(data_direct_path));
	if (data_direct_status != 0) {
		DOCA_LOG_ERR("Data Direct is not available for device %s: %s",
			     resources->cfg->nic_device_name,
			     strerror(data_direct_status));
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	DOCA_LOG_INFO("Using Data Direct device /sys%s for %s", data_direct_path, resources->cfg->nic_device_name);
	return DOCA_SUCCESS;
#else
	(void)resources;
	DOCA_LOG_ERR("Data Direct requires CUDA 12.8 or newer and Data Direct-capable libmlx5 headers");
	return DOCA_ERROR_NOT_SUPPORTED;
#endif
}

doca_error_t create_verbs_resources(struct verbs_config *cfg, struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	union ibv_gid rgid;
	int ret = 0;
	struct ibv_port_attr port_attr;
	struct doca_gpu_verbs_qp_init_attr_hl qp_init = {0};
	int cuda_id = 0;
	cudaError_t cuda_ret;
	int fd, fd_flags = 0, fd_fcntl_status = 0;

	resources->cfg = cfg;

	/* In a multi-GPU system, ensure CUDA refers to the right GPU device */
	cuda_ret = cudaDeviceGetByPCIBusId(&cuda_id, cfg->gpu_pcie_addr);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Invalid GPU bus id provided %s", cfg->gpu_pcie_addr);
		return DOCA_ERROR_INVALID_VALUE;
	}

	cuda_ret = cudaSetDevice(cuda_id);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Failed to cudaSetDevice %s: %d", cfg->gpu_pcie_addr, cuda_ret);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * A CUDA context must be initialized before calling GPUNetIO functions.
	 * cudaFree(0) creates the context on the device selected above and reports any errors.
	 */
	cuda_ret = cudaFree(0);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("CUDA initialization failed: %s", cudaGetErrorString(cuda_ret));
		return DOCA_ERROR_INITIALIZATION;
	}

	status = doca_gpu_create(cfg->gpu_pcie_addr, &resources->gpu_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca gpu context: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	resources->verbs_context = open_ib_device(cfg->nic_device_name);
	if (resources->verbs_context == NULL) {
		DOCA_LOG_ERR("Failed to create doca network context");
		status = DOCA_ERROR_BAD_STATE;
		goto destroy_resources;
	}

	status = doca_verbs_pd_create(resources->verbs_context, &resources->verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs pd: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	resources->pd = doca_verbs_bridge_verbs_pd_get_ibv_pd(resources->verbs_pd);
	if (resources->pd == NULL) {
		status = DOCA_ERROR_UNEXPECTED;
		DOCA_LOG_ERR("Failed to get ibv_pd");
		goto destroy_resources;
	}

	status = doca_rdma_bridge_open_dev_from_pd(resources->pd, &resources->dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open doca device from pd: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	if (cfg->enable_data_direct) {
		status = check_data_direct(resources);
		if (status != DOCA_SUCCESS)
			goto destroy_resources;
	}

	/*
	 * Resolve AUTO here rather than in doca_gpu_verbs_export_qp(): when the GPU cannot ring the doorbell, the
	 * export silently falls back to CPU proxy, but by then the QP DBR has been placed in GPU memory where the proxy
	 * thread cannot update it, and the sample does not know it must start the proxy thread.
	 */
	if (resources->nic_handler == DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO) {
		if (doca_gpu_nic_cap_is_nic_handler_gpu_sm_db_supported(resources->gpu_dev, resources->dev) ==
		    DOCA_SUCCESS) {
			resources->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_GPU_SM_DB;
			DOCA_LOG_INFO("NIC handler AUTO resolved to %s",
				      doca_gpu_nic_handler_to_string(resources->nic_handler));
		} else {
			resources->nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_CPU_PROXY;
			DOCA_LOG_WARN("GPU can't ring the NIC doorbell, NIC handler AUTO resolved to %s",
				      doca_gpu_nic_handler_to_string(resources->nic_handler));
		}
	}

	ret = ibv_query_gid(resources->pd->context, 1, cfg->gid_index, &rgid);
	if (ret) {
		DOCA_LOG_ERR("Failed to query ibv gid attributes");
		status = DOCA_ERROR_DRIVER;
		goto destroy_resources;
	}
	memcpy(resources->gid.raw, rgid.raw, DOCA_GID_BYTE_LENGTH);

	ret = ibv_query_port(resources->pd->context, 1, &port_attr);
	if (ret) {
		DOCA_LOG_ERR("Failed to query ibv port attributes");
		status = DOCA_ERROR_DRIVER;
		goto destroy_resources;
	}

	if (port_attr.link_layer == 1) {
		status = create_verbs_ah_attr(resources->verbs_context,
					      cfg->gid_index,
					      DOCA_VERBS_ADDR_TYPE_IB_NO_GRH,
					      &resources->verbs_ah_attr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca verbs ah attributes");
			goto destroy_resources;
		}
		resources->lid = port_attr.lid;
	} else {
		status = create_verbs_ah_attr(resources->verbs_context,
					      cfg->gid_index,
					      DOCA_VERBS_ADDR_TYPE_IPv4,
					      &resources->verbs_ah_attr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca verbs ah attributes");
			goto destroy_resources;
		}
	}

	qp_init.gpu_dev = resources->gpu_dev;
	qp_init.dev = resources->dev;
	qp_init.verbs_context = resources->verbs_context;
	qp_init.verbs_pd = resources->verbs_pd;
	qp_init.rq_nwqe = VERBS_TEST_QUEUE_SIZE;
	qp_init.sq_nwqe = VERBS_TEST_QUEUE_SIZE;
	qp_init.nic_handler = resources->nic_handler;
	qp_init.recv_inline = resources->recv_inline;
	qp_init.cq_collapsed = resources->cq_collapsed;

	if (resources->enable_comp_channel) {
		status = doca_verbs_comp_channel_create(resources->verbs_context, &resources->comp_channel);
		if (status == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_WARN("DOCA SDK version doesn't support Verbs comp channel feature");
			resources->comp_channel = NULL;
		} else if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_verbs_comp_channel_create with %d", status);
			goto destroy_resources;
		}

		if (resources->comp_channel) {
			fd = (int)doca_verbs_comp_channel_get_handle(resources->comp_channel);

			fd_flags = fcntl(fd, F_GETFL);
			if (fd_flags < 0) {
				DOCA_LOG_ERR("Failed to set at %s fcntl %d", __func__, fd_flags);
				status = DOCA_ERROR_DRIVER;
				goto destroy_resources;
			}

			fd_fcntl_status = fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK);
			if (fd_fcntl_status < 0) {
				DOCA_LOG_ERR("Failed to set at %s fcntl %d", __func__, fd_fcntl_status);
				status = DOCA_ERROR_DRIVER;
				goto destroy_resources;
			}
		}
	}

	qp_init.comp_channel = resources->comp_channel;

	if (resources->qp_group) {
		status = doca_gpu_verbs_create_qp_group_hl(&qp_init, &(resources->qpg));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca verbs high-level qp %d", status);
			goto destroy_resources;
		}
		resources->local_qp_main_number = doca_verbs_qp_get_qpn(resources->qpg->qp_main.qp);
		resources->local_qp_companion_number = doca_verbs_qp_get_qpn(resources->qpg->qp_companion.qp);

	} else {
		status = doca_gpu_verbs_create_qp_hl(&qp_init, &(resources->qp));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca verbs high-level qp %d", status);
			goto destroy_resources;
		}

		resources->local_cq_rq_number = doca_verbs_cq_get_cqn(resources->qp->cq_rq);
		resources->local_cq_sq_number = doca_verbs_cq_get_cqn(resources->qp->cq_sq);
		resources->local_qp_number = doca_verbs_qp_get_qpn(resources->qp->qp);
	}

	return DOCA_SUCCESS;

destroy_resources:
	tmp_status = destroy_verbs_resources(resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy resources: %s", doca_error_get_descr(tmp_status));
	}
	return status;
}

doca_error_t destroy_verbs_resources(struct verbs_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS, status;

	/*
	 * Called both at the end of a run and from create_verbs_resources() error path, so any object may be missing.
	 * Keep going on errors so one failure does not leak everything created after it, and clear each pointer so a
	 * second call is harmless.
	 */
	if (resources->qpg != NULL) {
		status = doca_gpu_verbs_destroy_qp_group_hl(resources->qpg);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs high-level qp group: %s",
				     doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}

		free(resources->qpg);
		resources->qpg = NULL;
	}

	if (resources->qp != NULL) {
		status = doca_gpu_verbs_destroy_qp_hl(resources->qp);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs high-level qp: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}

		free(resources->qp);
		resources->qp = NULL;
	}

	if (resources->comp_channel) {
		status = doca_verbs_comp_channel_destroy(resources->comp_channel);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs comp_channel: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}
		resources->comp_channel = NULL;
	}

	if (resources->verbs_ah_attr) {
		status = doca_verbs_ah_attr_destroy(resources->verbs_ah_attr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs AH: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}
		resources->verbs_ah_attr = NULL;
	}

	/* The device was opened on top of the PD, close it before the PD goes away */
	if (resources->dev) {
		status = doca_dev_close(resources->dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close pf device: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}
		resources->dev = NULL;
	}

	if (resources->verbs_pd) {
		status = doca_verbs_pd_destroy(resources->verbs_pd);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs PD: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}
		resources->verbs_pd = NULL;
		resources->pd = NULL;
	}

	if (resources->verbs_context) {
		status = doca_verbs_context_destroy(resources->verbs_context);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs Context: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}
		resources->verbs_context = NULL;
	}

	if (resources->gpu_dev) {
		status = doca_gpu_destroy(resources->gpu_dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy pf doca gpu context: %s", doca_error_get_descr(status));
			DOCA_ERROR_PROPAGATE(result, status);
		}
		resources->gpu_dev = NULL;
	}

	return result;
}

/*
 * Connect a DOCA Verbs QP with a remote QP
 *
 * @resources [in]: Sample resources
 * @return: doca error value
 */
doca_error_t connect_verbs_qp(struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_verbs_qp_attr *verbs_qp_attr = NULL;
	int ret;
	struct ibv_port_attr port_attr;
	uint8_t max_dest_rd_atomic, max_rd_atomic;
	struct doca_verbs_device_attr *verbs_device_attr;

	status = doca_verbs_ah_attr_set_gid(resources->verbs_ah_attr, resources->remote_gid);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set remote gid: %s", doca_error_get_descr(status));
		return status;
	}

	ret = ibv_query_port(resources->pd->context, 1, &port_attr);
	if (ret) {
		DOCA_LOG_ERR("Failed to query ibv port attributes");
		status = DOCA_ERROR_DRIVER;
		return status;
	}

	/* IB only parameter */
	if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
		status = doca_verbs_ah_attr_set_dlid(resources->verbs_ah_attr, resources->dlid);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set dlid");
			return status;
		}
	}

	status = doca_verbs_ah_attr_set_sl(resources->verbs_ah_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sl");
		return status;
	}

	status = doca_verbs_qp_attr_create(&verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_qp_attr_set_path_mtu(verbs_qp_attr, DOCA_MTU_SIZE_4K_BYTES);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set path MTU: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_rq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ PSN: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_sq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ PSN: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_port_num(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port number: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_ack_timeout(verbs_qp_attr, 14);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ACK timeout: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_retry_cnt(verbs_qp_attr, 7);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set retry counter: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_rnr_retry(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RNR retry: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_min_rnr_timer(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set minimum RNR timer: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_INIT);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_allow_remote_write(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote write: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_allow_remote_read(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote read: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_atomic_mode(verbs_qp_attr, DOCA_VERBS_QP_ATOMIC_MODE_IB_SPEC);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set atomic mode: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_query_device(resources->verbs_context, &verbs_device_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query device attributes: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	max_dest_rd_atomic = doca_verbs_device_attr_get_max_qp_init_rd_atom(verbs_device_attr);
	max_rd_atomic = doca_verbs_device_attr_get_max_qp_rd_atom(verbs_device_attr);

	status = doca_verbs_device_attr_free(verbs_device_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free device attributes: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	/*
	 * Both the companion QP (RC_QP_*_REQ_ATTR_MASK) and the single QP modify the RD atomic limits, so set them
	 * once for both paths. Otherwise the companion QP is moved to RTR/RTS with the attribute defaults.
	 */
	status = doca_verbs_qp_attr_set_max_dest_rd_atomic(verbs_qp_attr, max_dest_rd_atomic);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max_dest_rd_atomic: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_max_rd_atomic(verbs_qp_attr, max_rd_atomic);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max_rd_atomic: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_ah_attr(verbs_qp_attr, resources->verbs_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address handle: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_pkey_index(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PKey index: %d", status);
		goto destroy_verbs_qp_attr;
	}

	if (resources->qp_group == true) {
		status = doca_verbs_qp_attr_set_dest_qp_num(verbs_qp_attr, resources->remote_qp_main_number);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set destination QP number: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qpg->qp_main.qp,
					      verbs_qp_attr,
					      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
						      DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_READ |
						      DOCA_VERBS_QP_ATTR_ATOMIC_MODE | DOCA_VERBS_QP_ATTR_PKEY_INDEX |
						      DOCA_VERBS_QP_ATTR_PORT_NUM);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTR);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qpg->qp_main.qp,
					      verbs_qp_attr,
					      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN |
						      DOCA_VERBS_QP_ATTR_DEST_QP_NUM | DOCA_VERBS_QP_ATTR_PATH_MTU |
						      DOCA_VERBS_QP_ATTR_AH_ATTR | DOCA_VERBS_QP_ATTR_MIN_RNR_TIMER);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTS);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qpg->qp_main.qp,
					      verbs_qp_attr,
					      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_SQ_PSN |
						      DOCA_VERBS_QP_ATTR_ACK_TIMEOUT | DOCA_VERBS_QP_ATTR_RETRY_CNT |
						      DOCA_VERBS_QP_ATTR_RNR_RETRY);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_INIT);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_dest_qp_num(verbs_qp_attr, resources->remote_qp_companion_number);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set destination QP number: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qpg->qp_companion.qp,
					      verbs_qp_attr,
					      RC_QP_RST2INIT_REQ_ATTR_MASK);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTR);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qpg->qp_companion.qp,
					      verbs_qp_attr,
					      RC_QP_INIT2RTR_REQ_ATTR_MASK);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTS);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qpg->qp_companion.qp,
					      verbs_qp_attr,
					      RC_QP_RTR2RTS_REQ_ATTR_MASK);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

	} else {
		status = doca_verbs_qp_attr_set_dest_qp_num(verbs_qp_attr, resources->remote_qp_number);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set destination QP number: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qp->qp,
					      verbs_qp_attr,
					      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
						      DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_READ |
						      DOCA_VERBS_QP_ATTR_ATOMIC_MODE | DOCA_VERBS_QP_ATTR_PKEY_INDEX |
						      DOCA_VERBS_QP_ATTR_PORT_NUM);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTR);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qp->qp,
					      verbs_qp_attr,
					      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN |
						      DOCA_VERBS_QP_ATTR_DEST_QP_NUM | DOCA_VERBS_QP_ATTR_PATH_MTU |
						      DOCA_VERBS_QP_ATTR_AH_ATTR | DOCA_VERBS_QP_ATTR_MIN_RNR_TIMER |
						      DOCA_VERBS_QP_ATTR_MAX_DEST_RD_ATOMIC);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTS);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}

		status = doca_verbs_qp_modify(resources->qp->qp,
					      verbs_qp_attr,
					      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_SQ_PSN |
						      DOCA_VERBS_QP_ATTR_ACK_TIMEOUT | DOCA_VERBS_QP_ATTR_RETRY_CNT |
						      DOCA_VERBS_QP_ATTR_RNR_RETRY |
						      DOCA_VERBS_QP_ATTR_MAX_QP_RD_ATOMIC);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
			goto destroy_verbs_qp_attr;
		}
	}

	status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	DOCA_LOG_INFO("QP has been successfully connected and ready to use");

	return DOCA_SUCCESS;

destroy_verbs_qp_attr:
	tmp_status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA verbs QP attributes: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

void *progress_cpu_proxy(void *args_)
{
	struct cpu_proxy_args *args = (struct cpu_proxy_args *)args_;

	printf("Thread CPU proxy progress is running... %ld\n", ACCESS_ONCE_64b(*args->exit_flag));

	while (ACCESS_ONCE_64b(*args->exit_flag) == 0) {
		doca_gpu_verbs_cpu_proxy_progress(args->qp_cpu_main);
		if (args->qp_cpu_companion)
			doca_gpu_verbs_cpu_proxy_progress(args->qp_cpu_companion);
	}

	return NULL;
}

size_t get_page_size(void)
{
	long ret = sysconf(_SC_PAGESIZE);
	if (ret == -1)
		return 4096; // 4KB, default Linux page size

	return (size_t)ret;
}

/*
 * Register a GPU buffer with Data Direct
 *
 * @resources [in]: DOCA Verbs resources
 * @buf [in]: GPU buffer
 * @size [in]: Buffer size
 * @access_flags [in]: Access flags of the memory region
 * @mr [out]: Memory region
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t reg_data_direct_mr(struct verbs_resources *resources,
				       void *buf,
				       size_t size,
				       int access_flags,
				       struct ibv_mr **mr)
{
#if GPUNETIO_VERBS_DATA_DIRECT_SUPPORTED
	const char *cuda_error_name = NULL;
	CUresult cu_status;
	int dmabuf_fd = -1;

	cu_status = cuMemGetHandleForAddressRange((void *)&dmabuf_fd,
						  (CUdeviceptr)buf,
						  size,
						  CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,
						  CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE);
	if (cu_status != CUDA_SUCCESS) {
		(void)cuGetErrorName(cu_status, &cuda_error_name);
		DOCA_LOG_ERR("Failed to export GPU buffer %p for Data Direct: %s",
			     buf,
			     cuda_error_name == NULL ? "unknown CUDA error" : cuda_error_name);
		return DOCA_ERROR_DRIVER;
	}

	*mr = mlx5dv_reg_dmabuf_mr(resources->pd,
				   0,
				   size,
				   (uint64_t)buf,
				   dmabuf_fd,
				   access_flags,
				   MLX5DV_REG_DMABUF_ACCESS_DATA_DIRECT);
	/* The MR holds its own reference to the dmabuf */
	close(dmabuf_fd);
	if (*mr == NULL) {
		DOCA_LOG_ERR("Failed to register GPU buffer %p with Data Direct: %s", buf, strerror(errno));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
#else
	(void)resources;
	(void)buf;
	(void)size;
	(void)access_flags;
	(void)mr;
	DOCA_LOG_ERR("Data Direct requires CUDA 12.8 or newer and Data Direct-capable libmlx5 headers");
	return DOCA_ERROR_NOT_SUPPORTED;
#endif
}

doca_error_t verbs_reg_gpu_mr(struct verbs_resources *resources,
			      void *buf,
			      size_t size,
			      int access_flags,
			      struct ibv_mr **mr)
{
	int dmabuf_fd;

	*mr = NULL;

	if (resources->cfg->enable_data_direct)
		return reg_data_direct_mr(resources, buf, size, access_flags, mr);

	/* Try with dmabuf mapping first. If it doesn't work, fallback to legacy nvidia-peermem method. */
	if (doca_gpu_dmabuf_fd(resources->gpu_dev, buf, size, &dmabuf_fd) == DOCA_SUCCESS) {
		*mr = ibv_reg_dmabuf_mr(resources->pd, 0, size, (uint64_t)buf, dmabuf_fd, access_flags);
		/* The MR holds its own reference to the dmabuf */
		close(dmabuf_fd);
	}

	if (*mr == NULL)
		*mr = ibv_reg_mr(resources->pd, buf, size, access_flags);

	if (*mr == NULL) {
		DOCA_LOG_ERR("Failed to register GPU buffer %p of %zu bytes: %s", buf, size, strerror(errno));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}

doca_error_t verbs_copy_gpu_to_host(void *host_buf, const void *gpu_buf, size_t size)
{
	cudaError_t cuda_ret;

	cuda_ret = cudaDeviceFlushGPUDirectRDMAWrites(cudaFlushGPUDirectRDMAWritesTargetCurrentDevice,
						      cudaFlushGPUDirectRDMAWritesToOwner);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Failed to flush GPUDirect RDMA writes: %s", cudaGetErrorString(cuda_ret));
		return DOCA_ERROR_DRIVER;
	}

	cuda_ret = cudaMemcpy(host_buf, gpu_buf, size, cudaMemcpyDeviceToHost);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Failed to copy GPU buffer %p to host memory: %s", gpu_buf, cudaGetErrorString(cuda_ret));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}
