/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <unistd.h>

#include <doca_log.h>
#include <doca_error.h>
#include <doca_argp.h>
#include <doca_pe.h>

#include "rdma_verbs_common.h"
#include "common.h"

DOCA_LOG_REGISTER(GPURDMAVERBS::COMMON);

static uint32_t align_up_uint32(uint32_t value, uint32_t alignment)
{
	uint64_t remainder = (value % alignment);

	if (remainder == 0)
		return value;

	return (uint32_t)(value + (alignment - remainder));
}

/*
 * OOB connection to exchange RDMA info - server side
 *
 * @oob_sock_fd [out]: Socket FD
 * @oob_client_sock [out]: Client socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_rdma_verbs_connection_server_setup(int *oob_sock_fd, int *oob_client_sock)
{
	struct sockaddr_in server_addr = {0}, client_addr = {0};
	unsigned int client_size = 0;
	int enable = 1;
	int oob_sock_fd_ = 0;
	int oob_client_sock_ = 0;

	/* Create socket */
	oob_sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (oob_sock_fd_ < 0) {
		DOCA_LOG_ERR("Error while creating socket %d", oob_sock_fd_);
		return -1;
	}
	DOCA_LOG_INFO("Socket created successfully");

	if (setsockopt(oob_sock_fd_, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable))) {
		DOCA_LOG_ERR("Error setting socket options");
		close(oob_sock_fd_);
		return -1;
	}

	/* Set port and IP: */
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(2000);
	server_addr.sin_addr.s_addr = INADDR_ANY; /* listen on any interface */

	/* Bind to the set port and IP: */
	if (bind(oob_sock_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		DOCA_LOG_ERR("Couldn't bind to the port");
		close(oob_sock_fd_);
		return -1;
	}
	DOCA_LOG_INFO("Done with binding");

	/* Listen for clients: */
	if (listen(oob_sock_fd_, 1) < 0) {
		DOCA_LOG_ERR("Error while listening");
		close(oob_sock_fd_);
		return -1;
	}
	DOCA_LOG_INFO("Listening for incoming connections");

	/* Accept an incoming connection: */
	client_size = sizeof(client_addr);
	oob_client_sock_ = accept(oob_sock_fd_, (struct sockaddr *)&client_addr, &client_size);
	if (oob_client_sock_ < 0) {
		DOCA_LOG_ERR("Can't accept socket connection %d", oob_client_sock_);
		close(oob_sock_fd_);
		return -1;
	}

	*(oob_sock_fd) = oob_sock_fd_;
	*(oob_client_sock) = oob_client_sock_;

	DOCA_LOG_INFO("Client connected at IP: %s and port: %i",
		      inet_ntoa(client_addr.sin_addr),
		      ntohs(client_addr.sin_port));

	return 0;
}

/*
 * OOB connection to exchange RDMA info - server side closure
 *
 * @oob_sock_fd [in]: Socket FD
 * @oob_client_sock [in]: Client socket FD
 * @return: positive integer on success and -1 otherwise
 */
void oob_rdma_verbs_connection_server_close(int oob_sock_fd, int oob_client_sock)
{
	if (oob_client_sock > 0)
		close(oob_client_sock);

	if (oob_sock_fd > 0)
		close(oob_sock_fd);
}

/*
 * OOB connection to exchange RDMA info - client side
 *
 * @server_ip [in]: Server IP address to connect
 * @oob_sock_fd [out]: Socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_rdma_verbs_connection_client_setup(const char *server_ip, int *oob_sock_fd)
{
	struct sockaddr_in server_addr = {0};
	int oob_sock_fd_;

	/* Create socket */
	oob_sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (oob_sock_fd_ < 0) {
		DOCA_LOG_ERR("Unable to create socket");
		return -1;
	}
	DOCA_LOG_INFO("Socket created successfully");

	/* Set port and IP the same as server-side: */
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(2000);
	server_addr.sin_addr.s_addr = inet_addr(server_ip);

	/* Send connection request to server: */
	if (connect(oob_sock_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		close(oob_sock_fd_);
		DOCA_LOG_ERR("Unable to connect to server at %s", server_ip);
		return -1;
	}
	DOCA_LOG_INFO("Connected with server successfully");

	*oob_sock_fd = oob_sock_fd_;
	return 0;
}

/*
 * OOB connection to exchange RDMA info - client side closure
 *
 * @oob_sock_fd [in]: Socket FD
 * @return: positive integer on success and -1 otherwise
 */
void oob_rdma_verbs_connection_client_close(int oob_sock_fd)
{
	if (oob_sock_fd > 0)
		close(oob_sock_fd);
}

static struct doca_rdma_verbs_context *open_ib_device(char *name)
{
	int nb_ibdevs = 0;
	struct ibv_device **ibdev_list = ibv_get_device_list(&nb_ibdevs);
	struct doca_rdma_verbs_context *context;

	if ((ibdev_list == NULL) || (nb_ibdevs == 0)) {
		DOCA_LOG_ERR("Failed to get RDMA devices list, ibdev_list null");
		return NULL;
	}

	for (int i = 0; i < nb_ibdevs; i++) {
		if (strncmp(ibv_get_device_name(ibdev_list[i]), name, strlen(name)) == 0) {
			struct ibv_device *dev_handle = ibdev_list[i];
			ibv_free_device_list(ibdev_list);

			if (doca_rdma_verbs_bridge_rdma_verbs_context_create(dev_handle,
									     DOCA_RDMA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
									     &context) != DOCA_SUCCESS)
				return NULL;

			return context;
		}
	}

	ibv_free_device_list(ibdev_list);

	return NULL;
}

static uint32_t calc_cq_external_umem_size(uint32_t queue_size)
{
	uint32_t umem_size;
	uint32_t aligned_ring_buffer_size = 0;

	if (queue_size != 0)
		aligned_ring_buffer_size = (uint32_t)(queue_size * sizeof(struct mlx5_cqe64));
	aligned_ring_buffer_size = align_up_uint32(aligned_ring_buffer_size, SYSTEM_PAGE_SIZE);
	umem_size = aligned_ring_buffer_size + RDMA_VERBS_TEST_DBR_SIZE;

	return align_up_uint32(umem_size, SYSTEM_PAGE_SIZE);
}

static void mlx5_init_cqes(struct mlx5_cqe64 *cqes, uint32_t nb_cqes)
{
	for (uint32_t cqe_idx = 0; cqe_idx < nb_cqes; cqe_idx++)
		cqes[cqe_idx].op_own = (MLX5_CQE_INVALID << DOCA_GPUNETIO_RDMA_VERBS_MLX5_CQE_OPCODE_SHIFT) |
				       MLX5_CQE_OWNER_MASK;
}

static doca_error_t create_verbs_cq(struct doca_rdma_verbs_context *verbs_context,
				    struct doca_gpu *gpu_dev,
				    struct doca_dev *dev,
				    void **gpu_umem_dev_ptr,
				    struct doca_umem **gpu_umem,
				    struct doca_rdma_verbs_cq **verbs_cq)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	cudaError_t status_cuda = cudaSuccess;
	struct doca_rdma_verbs_cq_attr *verbs_cq_attr = NULL;
	struct doca_rdma_verbs_cq *new_cq = NULL;
	struct mlx5_cqe64 *cq_ring_haddr = NULL;
	uint32_t external_umem_size = 0;

	status = doca_rdma_verbs_cq_attr_create(&verbs_cq_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs cq attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_rdma_verbs_cq_attr_set_external_datapath_en(verbs_cq_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs cq external datapath en: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	external_umem_size = calc_cq_external_umem_size(RDMA_VERBS_TEST_QUEUE_SIZE);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to calc external umem size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_gpu_mem_alloc(gpu_dev,
				    external_umem_size,
				    SYSTEM_PAGE_SIZE,
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)gpu_umem_dev_ptr,
				    NULL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc gpu memory for external umem cq: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	cq_ring_haddr = (struct mlx5_cqe64 *)(calloc(RDMA_VERBS_TEST_QUEUE_SIZE, sizeof(struct mlx5_cqe64)));
	if (cq_ring_haddr == NULL) {
		DOCA_LOG_ERR("Failed to allocate cq host ring buffer memory for initialization");
		status = DOCA_ERROR_NO_MEMORY;
		goto destroy_resources;
	}

	mlx5_init_cqes(cq_ring_haddr, RDMA_VERBS_TEST_QUEUE_SIZE);

	status_cuda = cudaMemcpy((*gpu_umem_dev_ptr),
				 (void *)(cq_ring_haddr),
				 RDMA_VERBS_TEST_QUEUE_SIZE * sizeof(struct mlx5_cqe64),
				 cudaMemcpyDefault);
	if (status_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Failed to cudaMempy gpu cq cq ring buffer");
		goto destroy_resources;
	}

	free(cq_ring_haddr);

	status = doca_umem_gpu_create(gpu_dev,
				      dev,
				      (*gpu_umem_dev_ptr),
				      external_umem_size,
				      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					      DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC,
				      gpu_umem);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create gpu umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_cq_attr_set_external_umem(verbs_cq_attr, *gpu_umem, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs cq external umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_cq_attr_set_cq_size(verbs_cq_attr, RDMA_VERBS_TEST_QUEUE_SIZE);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs cq size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_cq_attr_set_entry_size(verbs_cq_attr, DOCA_RDMA_VERBS_CQ_ENTRY_SIZE_64);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs cq entry size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_cq_attr_set_cq_overrun(verbs_cq_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs cq size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_cq_create(verbs_context, verbs_cq_attr, &new_cq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs cq: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_cq_attr_destroy(verbs_cq_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca rdma verbs cq attributes: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	*verbs_cq = new_cq;

	return DOCA_SUCCESS;

destroy_resources:
	if (new_cq != NULL) {
		tmp_status = doca_rdma_verbs_cq_destroy(new_cq);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs cq: %s", doca_error_get_descr(tmp_status));
	}

	if (verbs_cq_attr != NULL) {
		tmp_status = doca_rdma_verbs_cq_attr_destroy(verbs_cq_attr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs cq attributes: %s",
				     doca_error_get_descr(tmp_status));
	}

	if (*gpu_umem != NULL) {
		tmp_status = doca_umem_destroy(*gpu_umem);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy gpu ring buffer umem: %s", doca_error_get_descr(tmp_status));
	}

	if (cq_ring_haddr) {
		free(cq_ring_haddr);
	}

	if ((*gpu_umem_dev_ptr) != 0) {
		tmp_status = doca_gpu_mem_free(gpu_dev, (*gpu_umem_dev_ptr));
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy gpu memory of cq ring buffer: %s",
				     doca_error_get_descr(tmp_status));
	}

	return status;
}

static doca_error_t create_verbs_ah(struct doca_rdma_verbs_context *verbs_context,
				    uint32_t gid_index,
				    enum doca_rdma_verbs_addr_type addr_type,
				    struct doca_rdma_verbs_ah **verbs_ah)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_rdma_verbs_ah *new_ah = NULL;

	status = doca_rdma_verbs_ah_create(verbs_context, &new_ah);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs ah: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_rdma_verbs_ah_set_addr_type(new_ah, addr_type);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address type: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	status = doca_rdma_verbs_ah_set_sgid_index(new_ah, gid_index);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sgid index: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	status = doca_rdma_verbs_ah_set_hop_limit(new_ah, RDMA_VERBS_TEST_HOP_LIMIT);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set hop limit: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	*verbs_ah = new_ah;

	return DOCA_SUCCESS;

destroy_verbs_ah:
	tmp_status = doca_rdma_verbs_ah_destroy(new_ah);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy doca rdma verbs AH: %s", doca_error_get_descr(tmp_status));

	return status;
}

static uint32_t calc_qp_external_umem_size(uint32_t rq_size, uint32_t sq_size)
{
	uint32_t rq_ring_size = 0;
	uint32_t sq_ring_size = 0;

	if (rq_size != 0)
		rq_ring_size = (uint32_t)(rq_size * sizeof(struct mlx5_wqe_data_seg));
	if (sq_size != 0)
		sq_ring_size = (uint32_t)(sq_size * sizeof(struct doca_gpu_dev_rdma_verbs_wqe));

	uint32_t wqe_sz = align_up_uint32(rq_ring_size + sq_ring_size, SYSTEM_PAGE_SIZE);

	return align_up_uint32(wqe_sz + RDMA_VERBS_TEST_DBR_SIZE, SYSTEM_PAGE_SIZE);
}

static doca_error_t create_verbs_qp(struct doca_rdma_verbs_context *verbs_context,
				    struct doca_gpu *gpu_dev,
				    struct doca_dev *dev,
				    struct doca_rdma_verbs_pd *verbs_pd,
				    struct doca_rdma_verbs_cq *verbs_cq_rq,
				    struct doca_rdma_verbs_cq *verbs_cq_sq,
				    uint32_t qp_rq_size,
				    uint32_t qp_sq_size,
				    void **gpu_umem_dev_ptr,
				    struct doca_umem **gpu_umem,
				    struct doca_rdma_verbs_qp **verbs_qp)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_rdma_verbs_qp_init_attr *verbs_qp_init_attr = NULL;
	struct doca_rdma_verbs_qp *new_qp = NULL;
	struct doca_uar *external_uar;
	uint32_t external_umem_size = 0;

	status = doca_rdma_verbs_qp_init_attr_create(&verbs_qp_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs qp attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_rdma_verbs_qp_init_attr_set_external_datapath_en(verbs_qp_init_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs external datapath en: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	external_umem_size = calc_qp_external_umem_size(qp_rq_size, qp_sq_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to calc external umem size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_gpu_mem_alloc(gpu_dev,
				    external_umem_size,
				    SYSTEM_PAGE_SIZE,
				    DOCA_GPU_MEM_TYPE_GPU,
				    gpu_umem_dev_ptr,
				    NULL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc gpu memory for external umem qp: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_umem_gpu_create(gpu_dev,
				      dev,
				      (*gpu_umem_dev_ptr),
				      external_umem_size,
				      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					      DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC,
				      gpu_umem);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create gpu umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_external_umem(verbs_qp_init_attr, *gpu_umem, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs qp external umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_external_dbr_umem(
		verbs_qp_init_attr,
		*gpu_umem,
		(qp_rq_size * sizeof(struct mlx5_wqe_data_seg)) +
			(qp_sq_size * sizeof(struct doca_gpu_dev_rdma_verbs_wqe)));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs qp external dbr umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_pd(verbs_qp_init_attr, verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs PD: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_sq_wr(verbs_qp_init_attr, qp_sq_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_rq_wr(verbs_qp_init_attr, qp_rq_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_qp_type(verbs_qp_init_attr, DOCA_RDMA_VERBS_QP_TYPE_RC);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set QP type: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_send_cq(verbs_qp_init_attr, verbs_cq_sq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs CQ: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_receive_cq(verbs_qp_init_attr, verbs_cq_rq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca rdma verbs CQ: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_set_send_max_sges(verbs_qp_init_attr, RDMA_VERBS_TEST_MAX_SEND_SEGS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set send_max_sges: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status =
		doca_rdma_verbs_qp_init_attr_set_receive_max_sges(verbs_qp_init_attr, RDMA_VERBS_TEST_MAX_RECEIVE_SEGS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set receive_max_sges: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_uar_create(dev, DOCA_UAR_ALLOCATION_TYPE_NONCACHE_DEDICATED, &external_uar);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to doca_uar_create NC DEDICATED");
#if CUDA_VERSION >= 12020
		status = doca_uar_create(dev, DOCA_UAR_ALLOCATION_TYPE_NONCACHE_DEDICATED, &external_uar);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to doca_uar_create NC");
		} else {
			DOCA_LOG_ERR("UAR created with DOCA_UAR_ALLOCATION_TYPE_NONCACHE_DEDICATED");
		}
#endif
	}

	if (status != DOCA_SUCCESS) {
		status = doca_uar_create(dev, DOCA_UAR_ALLOCATION_TYPE_BLUEFLAME, &external_uar);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to doca_uar_create NC");
			goto destroy_uar;
		}
	}

	status = doca_rdma_verbs_qp_init_attr_set_external_uar(verbs_qp_init_attr, external_uar);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set receive_max_sges");
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_create(verbs_context, verbs_qp_init_attr, &new_qp);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs QP: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_rdma_verbs_qp_init_attr_destroy(verbs_qp_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca rdma verbs QP attributes: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	*verbs_qp = new_qp;

	return DOCA_SUCCESS;

destroy_uar:
	if (external_uar != NULL)
		doca_uar_destroy(external_uar);

destroy_resources:
	if (new_qp != NULL) {
		tmp_status = doca_rdma_verbs_qp_destroy(new_qp);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs QP: %s", doca_error_get_descr(tmp_status));
	}

	if (verbs_qp_init_attr != NULL) {
		tmp_status = doca_rdma_verbs_qp_init_attr_destroy(verbs_qp_init_attr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs QP attributes: %s",
				     doca_error_get_descr(tmp_status));
	}

	if (*gpu_umem != NULL) {
		tmp_status = doca_umem_destroy(*gpu_umem);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy gpu umem: %s", doca_error_get_descr(tmp_status));
	}

	if ((*gpu_umem_dev_ptr) != 0) {
		tmp_status = doca_gpu_mem_free(gpu_dev, (*gpu_umem_dev_ptr));
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy gpu memory of umem: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

/*
 * Create and initialize DOCA RDMA Verbs resources
 *
 * @cfg [in]: Configuration parameters
 * @resources [in/out]: DOCA RDMA resources to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_rdma_verbs_resources(struct rdma_verbs_config *cfg, struct rdma_verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	union ibv_gid rgid;
	int ret = 0;
	struct ibv_port_attr port_attr;

	resources->cfg = cfg;

	status = doca_gpu_create(cfg->gpu_pcie_addr, &resources->gpu_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pf doca gpu context: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	resources->verbs_context = open_ib_device(cfg->nic_device_name);
	if (resources->verbs_context == NULL) {
		DOCA_LOG_ERR("Failed to create doca network context");
		goto destroy_resources;
	}

	status = doca_rdma_verbs_pd_create(resources->verbs_context, &resources->verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs pd: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	resources->pd = doca_rdma_verbs_bridge_rdma_verbs_pd_get_ibv_pd(resources->verbs_pd);
	if (resources->pd == NULL) {
		DOCA_LOG_ERR("Failed to get ibv_pd");
		goto destroy_resources;
	}

	status = doca_rdma_bridge_open_dev_from_pd(resources->pd, &resources->dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs pd: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = create_verbs_cq(resources->verbs_context,
				 resources->gpu_dev,
				 resources->dev,
				 &resources->gpu_cq_rq_umem_dev_ptr,
				 &resources->gpu_cq_rq_umem,
				 &resources->verbs_cq_rq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs cq: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}
	resources->local_cq_rq_number = doca_rdma_verbs_cq_get_cqn(resources->verbs_cq_rq);

	status = create_verbs_cq(resources->verbs_context,
				 resources->gpu_dev,
				 resources->dev,
				 &resources->gpu_cq_sq_umem_dev_ptr,
				 &resources->gpu_cq_sq_umem,
				 &resources->verbs_cq_sq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs cq: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}
	resources->local_cq_sq_number = doca_rdma_verbs_cq_get_cqn(resources->verbs_cq_sq);

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
		status = create_verbs_ah(resources->verbs_context,
					 cfg->gid_index,
					 DOCA_RDMA_VERBS_ADDR_TYPE_IB_GRH, // NO_GRH
					 &resources->verbs_ah);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca rdma verbs ah");
			goto destroy_resources;
		}
		resources->lid = port_attr.lid;
	} else {
		status = create_verbs_ah(resources->verbs_context,
					 cfg->gid_index,
					 DOCA_RDMA_VERBS_ADDR_TYPE_IPv4,
					 &resources->verbs_ah);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca rdma verbs ah");
			goto destroy_resources;
		}
	}

	status = create_verbs_qp(resources->verbs_context,
				 resources->gpu_dev,
				 resources->dev,
				 resources->verbs_pd,
				 resources->verbs_cq_rq,
				 resources->verbs_cq_sq,
				 RDMA_VERBS_TEST_QUEUE_SIZE,
				 RDMA_VERBS_TEST_QUEUE_SIZE,
				 &resources->gpu_qp_umem_dev_ptr,
				 &resources->gpu_qp_umem,
				 &resources->verbs_qp);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca rdma verbs qp: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}
	resources->local_qp_number = doca_rdma_verbs_qp_get_qpn(resources->verbs_qp);

	return DOCA_SUCCESS;

destroy_resources:
	tmp_status = destroy_rdma_verbs_resources(resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy resources: %s", doca_error_get_descr(tmp_status));
	}
	return status;
}

/*
 * Destroy DOCA RDMA resources
 *
 * @resources [in]: DOCA RDMA resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_rdma_verbs_resources(struct rdma_verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS;

	if (resources->verbs_qp) {
		if (resources->qp_gpu) {
			status = doca_gpu_rdma_verbs_unexport_qp(resources->gpu_dev, resources->qp_cpu);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy doca gpu thread argument cq memory: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}

		status = doca_rdma_verbs_qp_destroy(resources->verbs_qp);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs QP: %s", doca_error_get_descr(status));
			return status;
		}

		if (resources->gpu_qp_umem != NULL) {
			status = doca_umem_destroy(resources->gpu_qp_umem);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy gpu qp umem: %s", doca_error_get_descr(status));
				return status;
			}
		}

		if (resources->gpu_qp_umem_dev_ptr != 0) {
			status = doca_gpu_mem_free(resources->gpu_dev, resources->gpu_qp_umem_dev_ptr);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy gpu memory of qp ring buffer: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}
	}

	if (resources->verbs_ah) {
		status = doca_rdma_verbs_ah_destroy(resources->verbs_ah);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs AH: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_cq_rq) {
		status = doca_rdma_verbs_cq_destroy(resources->verbs_cq_rq);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs CQ: %s", doca_error_get_descr(status));
			return status;
		}

		if (resources->gpu_cq_rq_umem != NULL) {
			status = doca_umem_destroy(resources->gpu_cq_rq_umem);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy gpu cq ring buffer umem: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}

		if (resources->gpu_cq_rq_umem_dev_ptr != 0) {
			status = doca_gpu_mem_free(resources->gpu_dev, resources->gpu_cq_rq_umem_dev_ptr);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy gpu memory of cq ring buffer: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}
	}

	if (resources->verbs_cq_sq) {
		status = doca_rdma_verbs_cq_destroy(resources->verbs_cq_sq);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs CQ: %s", doca_error_get_descr(status));
			return status;
		}

		if (resources->gpu_cq_sq_umem != NULL) {
			status = doca_umem_destroy(resources->gpu_cq_sq_umem);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy gpu cq ring buffer umem: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}

		if (resources->gpu_cq_sq_umem_dev_ptr != 0) {
			status = doca_gpu_mem_free(resources->gpu_dev, resources->gpu_cq_sq_umem_dev_ptr);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy gpu memory of cq ring buffer: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}
	}

	if (resources->verbs_pd) {
		status = doca_rdma_verbs_pd_destroy(resources->verbs_pd);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs PD: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_context) {
		status = doca_rdma_verbs_context_destroy(resources->verbs_context);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca rdma verbs Context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->gpu_dev) {
		status = doca_gpu_destroy(resources->gpu_dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy pf doca gpu context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dev) {
		status = doca_dev_close(resources->dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close pf device: %s", doca_error_get_descr(status));
			return status;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Connect a DOCA RDMA Verbs QP to a remote one
 *
 * @resources [in]: DOCA RDMA Verbs resources with the QP
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t connect_rdma_verbs_qp(struct rdma_verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_rdma_verbs_qp_attr *rdma_verbs_qp_attr = NULL;

	status = doca_rdma_verbs_ah_set_gid(resources->verbs_ah, resources->remote_gid);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set remote gid: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_rdma_verbs_ah_set_dlid(resources->verbs_ah, resources->dlid);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set dlid");
		return status;
	}

	status = doca_rdma_verbs_qp_attr_create(&rdma_verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA doca rdma verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_rdma_verbs_qp_attr_set_path_mtu(rdma_verbs_qp_attr, DOCA_MTU_SIZE_1K_BYTES);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set path MTU: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_rq_psn(rdma_verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ PSN: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_sq_psn(rdma_verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ PSN: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_port_num(rdma_verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port number: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_ack_timeout(rdma_verbs_qp_attr, 14);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ACK timeout: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_retry_cnt(rdma_verbs_qp_attr, 7);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set retry counter: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_rnr_retry(rdma_verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RNR retry: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_min_rnr_timer(rdma_verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set minimum RNR timer: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_next_state(rdma_verbs_qp_attr, DOCA_RDMA_VERBS_QP_STATE_INIT);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_allow_remote_write(rdma_verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote write: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_allow_remote_read(rdma_verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote read: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_ah_attr(rdma_verbs_qp_attr, resources->verbs_ah);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address handle: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_dest_qp_num(rdma_verbs_qp_attr, resources->remote_qp_number);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set destination QP number: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_modify(
		resources->verbs_qp,
		rdma_verbs_qp_attr,
		DOCA_RDMA_VERBS_QP_ATTR_NEXT_STATE | DOCA_RDMA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
			DOCA_RDMA_VERBS_QP_ATTR_ALLOW_REMOTE_READ | DOCA_RDMA_VERBS_QP_ATTR_PKEY_INDEX |
			DOCA_RDMA_VERBS_QP_ATTR_PORT_NUM);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_next_state(rdma_verbs_qp_attr, DOCA_RDMA_VERBS_QP_STATE_RTR);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_modify(resources->verbs_qp,
					   rdma_verbs_qp_attr,
					   DOCA_RDMA_VERBS_QP_ATTR_NEXT_STATE | DOCA_RDMA_VERBS_QP_ATTR_RQ_PSN |
						   DOCA_RDMA_VERBS_QP_ATTR_DEST_QP_NUM |
						   DOCA_RDMA_VERBS_QP_ATTR_PATH_MTU | DOCA_RDMA_VERBS_QP_ATTR_AH_ATTR |
						   DOCA_RDMA_VERBS_QP_ATTR_MIN_RNR_TIMER);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_set_next_state(rdma_verbs_qp_attr, DOCA_RDMA_VERBS_QP_STATE_RTS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_modify(resources->verbs_qp,
					   rdma_verbs_qp_attr,
					   DOCA_RDMA_VERBS_QP_ATTR_NEXT_STATE | DOCA_RDMA_VERBS_QP_ATTR_SQ_PSN |
						   DOCA_RDMA_VERBS_QP_ATTR_ACK_TIMEOUT |
						   DOCA_RDMA_VERBS_QP_ATTR_RETRY_CNT |
						   DOCA_RDMA_VERBS_QP_ATTR_RNR_RETRY);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_rdma_verbs_qp_attr_destroy(rdma_verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy RDMA doca rdma verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	DOCA_LOG_INFO("QP has been successfully connected and ready to use");

	return DOCA_SUCCESS;

destroy_verbs_qp_attr:
	tmp_status = doca_rdma_verbs_qp_attr_destroy(rdma_verbs_qp_attr);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy RDMA doca rdma verbs QP attributes: %s",
			     doca_error_get_descr(tmp_status));
	}

	return status;
}

doca_error_t export_datapath_attr_in_gpu(struct rdma_verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS;

	status = doca_gpu_rdma_verbs_export_cq(resources->gpu_dev,
					       resources->verbs_cq_rq,
					       &resources->cq_rq_cpu,
					       &resources->cq_rq_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create GPU handler for CQ RQ: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_gpu_rdma_verbs_export_cq(resources->gpu_dev,
					       resources->verbs_cq_sq,
					       &resources->cq_sq_cpu,
					       &resources->cq_sq_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create GPU handler for CQ RQ: %s", doca_error_get_descr(status));
		doca_gpu_rdma_verbs_unexport_cq(resources->gpu_dev, resources->cq_rq_cpu);
		return status;
	}

	status = doca_gpu_rdma_verbs_export_qp(resources->gpu_dev,
					       resources->dev,
					       resources->verbs_qp,
					       (resources->enable_cpu_proxy == true ? 1 : 0),
					       resources->gpu_qp_umem_dev_ptr,
					       resources->cq_sq_gpu,
					       resources->cq_rq_gpu,
					       &resources->qp_cpu,
					       &resources->qp_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create GPU verbs QP: %s", doca_error_get_descr(status));
		doca_gpu_rdma_verbs_unexport_cq(resources->gpu_dev, resources->cq_rq_cpu);
		doca_gpu_rdma_verbs_unexport_cq(resources->gpu_dev, resources->cq_sq_cpu);
		return status;
	}

	return status;
}

void *progress_cpu_proxy(void *args_)
{
	struct cpu_proxy_args *args = (struct cpu_proxy_args *)args_;

	printf("Thread CPU proxy progress is running... %ld\n", ACCESS_ONCE_64b(*args->exit_flag));

	while (ACCESS_ONCE_64b(*args->exit_flag) == 0) {
		doca_gpu_rdma_verbs_cpu_proxy_progress(args->qp_cpu);
	}

	return NULL;
}
