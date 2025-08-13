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

#include "rdma_verbs_common.h"

DOCA_LOG_REGISTER(RDMA_VERBS_WRITE_LAT);

#define RESULT_LINE "----------------------------------------------------------------------------------------------\n"
#define RESULT_FMT_LAT " #bytes 	#iterations    Unidirectional[usec]    Bidirectional[usec]    CUDA Kernel[usec]"
#define REPORT_FMT_LAT " %-7lu 		%-7d          %-7.2f       		%-7.2f    	 	%-7.2f"

cudaStream_t cstream;
int message_size[NUM_MSG_SIZE] = {1, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
volatile bool server_force_quit = false;

static doca_error_t create_local_memory_object(struct rdma_verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS;

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		status = doca_gpu_mem_alloc(resources->gpu_dev,
					    (size_t)(resources->cuda_threads * message_size[idx]),
					    4096,
					    DOCA_GPU_MEM_TYPE_GPU,
					    (void **)&(resources->data_buf[idx]),
					    NULL);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate GPU memory buffer %d of size = %d (%d x %d)",
				     idx,
				     resources->cuda_threads * message_size[idx],
				     message_size[idx],
				     resources->cuda_threads);
			return DOCA_ERROR_NO_MEMORY;
		}

		cudaMemset(resources->data_buf[idx], idx + 1, resources->cuda_threads * message_size[idx]);

		resources->data_mr[idx] =
			ibv_reg_mr(resources->pd,
				   resources->data_buf[idx],
				   (size_t)(resources->cuda_threads * message_size[idx]),
				   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (resources->data_mr[idx] == NULL) {
			DOCA_LOG_ERR("Failed to create data mr: %s", doca_error_get_descr(status));
			doca_gpu_mem_free(resources->gpu_dev, (void *)resources->data_buf[idx]);
			return status;
		}

		printf("Data buf %d - %lx - size %zd threads %d msize %d mkey %x\n",
		       message_size[idx],
		       (uintptr_t)resources->data_buf[idx],
		       (size_t)(resources->cuda_threads * message_size[idx]),
		       resources->cuda_threads,
		       message_size[idx],
		       resources->data_mr[idx]->lkey);

		status = doca_gpu_mem_alloc(resources->gpu_dev,
					    (size_t)(resources->cuda_threads * sizeof(uint64_t)),
					    4096,
					    DOCA_GPU_MEM_TYPE_GPU,
					    (void **)&(resources->flag_buf[idx]),
					    NULL);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate GPU memory buffer %d of size = %zd (%zd x %d)",
				     idx,
				     sizeof(uint64_t) * resources->cuda_threads,
				     sizeof(uint64_t),
				     resources->cuda_threads);
			return DOCA_ERROR_NO_MEMORY;
		}

		cudaMemset(resources->flag_buf[idx], 0, resources->cuda_threads * sizeof(uint64_t));

		resources->flag_mr[idx] =
			ibv_reg_mr(resources->pd,
				   resources->flag_buf[idx],
				   (size_t)resources->cuda_threads * sizeof(uint64_t),
				   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (resources->flag_mr[idx] == NULL) {
			DOCA_LOG_ERR("Failed to create data mr: %s", doca_error_get_descr(status));
			doca_gpu_mem_free(resources->gpu_dev, (void *)resources->flag_buf[idx]);
			return status;
		}

		printf("remote flag %d - %lx\n", idx, (uintptr_t)resources->flag_buf[idx]);
	}

	return DOCA_SUCCESS;
}

static doca_error_t destroy_local_memory_objects(struct rdma_verbs_resources *resources)
{
	int ret = 0;

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		if (resources->data_mr[idx]) {
			ret = ibv_dereg_mr(resources->data_mr[idx]);
			if (ret != 0) {
				DOCA_LOG_ERR("ibv_dereg_mr failed with error=%d", ret);
				return DOCA_ERROR_DRIVER;
			}
		}

		if (resources->data_buf[idx])
			doca_gpu_mem_free(resources->gpu_dev, (void *)resources->data_buf[idx]);

		if (resources->flag_mr[idx]) {
			ret = ibv_dereg_mr(resources->flag_mr[idx]);
			if (ret != 0) {
				DOCA_LOG_ERR("ibv_dereg_mr failed with error=%d", ret);
				return DOCA_ERROR_DRIVER;
			}
		}

		if (resources->flag_buf[idx])
			doca_gpu_mem_free(resources->gpu_dev, (void *)resources->flag_buf[idx]);
	}

	return DOCA_SUCCESS;
}

static doca_error_t exchange_params_with_remote_peer(struct rdma_verbs_resources *resources)
{
	if (resources->cfg->is_server) {
		// Server sends local info
		for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
			uint64_t local_addr = (uint64_t)resources->data_buf[idx];
			if (send(resources->conn_socket, &local_addr, sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local buffer address");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (send(resources->conn_socket, &resources->data_mr[idx]->rkey, sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local MKEY");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			local_addr = (uint64_t)resources->flag_buf[idx];
			if (send(resources->conn_socket, &local_addr, sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local buffer address");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (send(resources->conn_socket, &resources->flag_mr[idx]->rkey, sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local MKEY");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}
		}

		for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
			if (recv(resources->conn_socket, &resources->remote_data_buf[idx], sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote buffer address ");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (recv(resources->conn_socket, &resources->remote_data_mkey[idx], sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote MKEY, err = %d", errno);
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (recv(resources->conn_socket, &resources->remote_flag_buf[idx], sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote buffer address ");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (recv(resources->conn_socket, &resources->remote_flag_mkey[idx], sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote MKEY, err = %d", errno);
				return DOCA_ERROR_CONNECTION_ABORTED;
			}
		}

	} else {
		// Client waits for server info
		for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
			if (recv(resources->conn_socket, &resources->remote_data_buf[idx], sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote buffer address ");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (recv(resources->conn_socket, &resources->remote_data_mkey[idx], sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote MKEY, err = %d", errno);
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (recv(resources->conn_socket, &resources->remote_flag_buf[idx], sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote buffer address ");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (recv(resources->conn_socket, &resources->remote_flag_mkey[idx], sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to receive remote MKEY, err = %d", errno);
				return DOCA_ERROR_CONNECTION_ABORTED;
			}
		}

		for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
			uint64_t local_addr = (uint64_t)resources->data_buf[idx];
			if (send(resources->conn_socket, &local_addr, sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local buffer address");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (send(resources->conn_socket, &resources->data_mr[idx]->rkey, sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local MKEY");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			local_addr = (uint64_t)resources->flag_buf[idx];
			if (send(resources->conn_socket, &local_addr, sizeof(uint64_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local buffer address");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}

			if (send(resources->conn_socket, &resources->flag_mr[idx]->rkey, sizeof(uint32_t), 0) < 0) {
				DOCA_LOG_ERR("Failed to send local MKEY");
				return DOCA_ERROR_CONNECTION_ABORTED;
			}
		}
	}

	if (send(resources->conn_socket, &resources->local_qp_number, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local QP number");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->remote_qp_number, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote QP number, err = %d", errno);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (send(resources->conn_socket, &resources->gid.raw, sizeof(resources->gid.raw), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local GID address");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->remote_gid.raw, sizeof(resources->gid.raw), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote GID address, err = %d", errno);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (send(resources->conn_socket, &resources->lid, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local GID address");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->dlid, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote GID address, err = %d", errno);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	return DOCA_SUCCESS;
}

doca_error_t rdma_verbs_server(struct rdma_verbs_config *cfg)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct rdma_verbs_resources resources = {0};
	int server_sock_fd = -1;
	cudaError_t cuda_ret;

	resources.conn_socket = -1;
	resources.num_iters = cfg->num_iters;
	resources.cuda_threads = cfg->cuda_threads;

	status = create_rdma_verbs_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local rdma resources: %s", doca_error_get_descr(status));
		return status;
	}

	status = create_local_memory_object(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local memory resources: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = oob_rdma_verbs_connection_server_setup(&server_sock_fd, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = connect_rdma_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect RDMA doca rdma verbs QP: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = export_datapath_attr_in_gpu(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init datapath attr in gpu: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	cuda_ret = cudaStreamCreateWithFlags(&cstream, cudaStreamNonBlocking);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", cuda_ret);
		status = DOCA_ERROR_DRIVER;
		goto close_connection;
	}

	DOCA_LOG_INFO(
		"Launching gpunetio_rdma_verbs_client_server server kernel with %d num QP, %d CUDA threads, %d total number of iterations, %d iterations per cuda thread",
		NUM_QP,
		resources.cuda_threads,
		resources.num_iters,
		resources.num_iters / resources.cuda_threads // check this is ok
	);

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		/* Warmup per size*/
		status = gpunetio_rdma_verbs_client_server(cstream,
							   resources.qp_gpu,
							   resources.num_iters,
							   resources.cuda_threads,
							   message_size[idx],
							   resources.data_buf[idx],
							   CPU_TO_BE32(resources.data_mr[idx]->lkey),
							   resources.flag_buf[idx],
							   CPU_TO_BE32(resources.flag_mr[idx]->lkey),
							   (uint8_t *)(resources.remote_data_buf[idx]),
							   CPU_TO_BE32(resources.remote_data_mkey[idx]),
							   (uint64_t *)(resources.remote_flag_buf[idx]),
							   CPU_TO_BE32(resources.remote_flag_mkey[idx]),
							   false);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(status));
			goto destroy_stream;
		}
	}

	cudaStreamSynchronize(cstream);

destroy_stream:
	cudaStreamDestroy(cstream);

close_connection:
	oob_rdma_verbs_connection_client_close(resources.conn_socket);

server_cleanup:
	tmp_status = destroy_local_memory_objects(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	tmp_status = destroy_rdma_verbs_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	return status;
}

doca_error_t rdma_verbs_client(struct rdma_verbs_config *cfg)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct rdma_verbs_resources resources = {0};
	cudaError_t cuda_ret;

	resources.conn_socket = -1;
	resources.num_iters = cfg->num_iters;
	resources.cuda_threads = cfg->cuda_threads;

	status = create_rdma_verbs_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA doca rdma verbs resources: %s", doca_error_get_descr(status));
		return status;
	}

	status = create_local_memory_object(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local memory resources: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = oob_rdma_verbs_connection_client_setup(cfg->server_ip_addr, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = connect_rdma_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect RDMA doca rdma verbs QP: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = export_datapath_attr_in_gpu(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init datapath attr in gpu: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	cuda_ret = cudaStreamCreateWithFlags(&cstream, cudaStreamNonBlocking);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", cuda_ret);
		status = DOCA_ERROR_DRIVER;
		goto close_connection;
	}

	DOCA_LOG_INFO(
		"Launching gpunetio_rdma_verbs_write_lat client kernel with %d num QP, %d CUDA threads, %d total number of iterations, %d iterations per cuda thread",
		NUM_QP,
		resources.cuda_threads,
		resources.num_iters,
		resources.num_iters / resources.cuda_threads // check this is ok
	);

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		/* Warmup per size*/
		status = gpunetio_rdma_verbs_client_server(cstream,
							   resources.qp_gpu,
							   resources.num_iters,
							   resources.cuda_threads,
							   message_size[idx],
							   resources.data_buf[idx],
							   CPU_TO_BE32(resources.data_mr[idx]->lkey),
							   resources.flag_buf[idx],
							   CPU_TO_BE32(resources.flag_mr[idx]->lkey),
							   (uint8_t *)(resources.remote_data_buf[idx]),
							   CPU_TO_BE32(resources.remote_data_mkey[idx]),
							   (uint64_t *)(resources.remote_flag_buf[idx]),
							   CPU_TO_BE32(resources.remote_flag_mkey[idx]),
							   true);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(status));
			goto destroy_stream;
		}
	}

	cudaStreamSynchronize(cstream);

destroy_stream:
	cudaStreamDestroy(cstream);

close_connection:
	oob_rdma_verbs_connection_client_close(resources.conn_socket);

client_cleanup:
	tmp_status = destroy_local_memory_objects(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	tmp_status = destroy_rdma_verbs_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	return status;
}
