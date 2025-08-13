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

#include "verbs_common.h"

DOCA_LOG_REGISTER(VERBS_TWO_SIDED);

#define RESULT_LINE "------------------------------------------------------------------------------------\n"
#define RESULT_FMT_G " #bytes     #iterations    BW average[MB/sec]   MsgRate[Mpps]    CUDA Kernel[ms]"
#define REPORT_FMT_EXT " %-7u    	%-7u           %-7.6lf            %-7.6lf            %-7.6f"

cudaStream_t cstream = NULL;
int message_size[NUM_MSG_SIZE] = {1, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
volatile bool server_force_quit = false;

/*
 * Server validates data from client at the end of the test
 */
static void client_validate_test(struct verbs_resources *resources)
{
	uint8_t *buffer;
	cudaError_t res_cuda;

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		buffer = (uint8_t *)calloc(resources->cuda_threads * message_size[idx], sizeof(uint8_t));
		if (buffer == NULL) {
			DOCA_LOG_ERR("Error memory");
			return;
		}

		res_cuda = cudaMemcpy(buffer,
				      resources->data_buf[idx],
				      resources->cuda_threads * message_size[idx],
				      cudaMemcpyDefault);
		if (res_cuda != cudaSuccess) {
			DOCA_LOG_ERR("Function CUDA Memcpy failed with %s", cudaGetErrorString(res_cuda));
			free(buffer);
			return;
		}

		for (int pos = 0; pos < (int)resources->cuda_threads * message_size[idx]; pos++) {
			if (buffer[pos] != (idx + 1)) {
				DOCA_LOG_ERR("Validation error: buffer %d pos %d has invalid data %d\n",
					     idx,
					     pos,
					     buffer[pos]);

				free(buffer);
				return;
			}
		}
		free(buffer);
	}

	DOCA_LOG_WARN("Data validation successfull! Data received correctly from server\n");
}

static doca_error_t destroy_local_memory_objects(struct verbs_resources *resources)
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

	if (resources->dump_flag_mr) {
		ret = ibv_dereg_mr(resources->dump_flag_mr);
		if (ret != 0) {
			DOCA_LOG_ERR("ibv_dereg_mr failed with error=%d", ret);
			return DOCA_ERROR_DRIVER;
		}
	}

	if (resources->dump_flag_buf)
		doca_gpu_mem_free(resources->gpu_dev, (void *)resources->dump_flag_buf);

	return DOCA_SUCCESS;
}

static doca_error_t create_local_memory_object(struct verbs_resources *resources)
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
			goto exit_error;
		}

		cudaMemset(resources->data_buf[idx], idx + 1, resources->cuda_threads * message_size[idx]);

		resources->data_mr[idx] =
			ibv_reg_mr(resources->pd,
				   resources->data_buf[idx],
				   (size_t)(resources->cuda_threads * message_size[idx]),
				   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (resources->data_mr[idx] == NULL) {
			status = DOCA_ERROR_DRIVER;
			DOCA_LOG_ERR("Failed to create data mr: %s", doca_error_get_descr(status));
			goto exit_error;
		}

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
			goto exit_error;
		}

		cudaMemset(resources->flag_buf[idx], 0, resources->cuda_threads * sizeof(uint64_t));

		resources->flag_mr[idx] = ibv_reg_mr(resources->pd,
						     resources->flag_buf[idx],
						     (size_t)resources->cuda_threads * sizeof(uint64_t),
						     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
							     IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
		if (resources->flag_mr[idx] == NULL) {
			status = DOCA_ERROR_DRIVER;
			DOCA_LOG_ERR("Failed to create data mr: %s", doca_error_get_descr(status));
			goto exit_error;
		}
	}

	status = doca_gpu_mem_alloc(resources->gpu_dev,
				    sizeof(uint64_t),
				    4096,
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&(resources->dump_flag_buf),
				    NULL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate GPU dump memory buffer of size = %zd (%zd x %d)",
			     sizeof(uint64_t) * resources->cuda_threads,
			     sizeof(uint64_t),
			     resources->cuda_threads);
		goto exit_error;
	}

	resources->dump_flag_mr = ibv_reg_mr(resources->pd,
					     resources->dump_flag_buf,
					     sizeof(uint64_t),
					     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (resources->dump_flag_mr == NULL) {
		status = DOCA_ERROR_DRIVER;
		DOCA_LOG_ERR("Failed to create data mr: %s", doca_error_get_descr(status));
		goto exit_error;
	}

	return DOCA_SUCCESS;

exit_error:
	destroy_local_memory_objects(resources);
	return status;
}

static doca_error_t exchange_params_with_remote_peer(struct verbs_resources *resources)
{
	if (resources->cfg->is_server) {
		// Server sends local info
		for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
			uint64_t local_addr = (uint64_t)resources->flag_buf[idx];
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
			uint64_t local_addr = (uint64_t)resources->flag_buf[idx];
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

doca_error_t verbs_server(struct verbs_config *cfg)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct verbs_resources resources = {0};
	int server_sock_fd = -1;
	cudaError_t cuda_ret;
	CUresult cu_result;
	CUevent e_start = NULL, e_end = NULL;
	float et_ms = 0.0f;
	const unsigned long format_factor = 0x100000; // -> MBS
	// 125000000;
	const unsigned long num_messages = cfg->num_iters * NUM_QP;
	struct doca_gpu_dev_verbs_qp *qp_gpu;

	resources.conn_socket = -1;
	resources.num_iters = cfg->num_iters;
	resources.cuda_threads = cfg->cuda_threads;
	resources.nic_handler = cfg->nic_handler;
	resources.scope = (enum doca_gpu_dev_verbs_exec_scope)cfg->exec_scope;
	resources.qp_group = false;

	status = create_verbs_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local rdma resources: %s", doca_error_get_descr(status));
		return status;
	}

	status = create_local_memory_object(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local memory resources: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = oob_verbs_connection_server_setup(&server_sock_fd, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	status = connect_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect DOCA verbs QP: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	cuda_ret = cudaStreamCreateWithFlags(&cstream, cudaStreamNonBlocking);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", cuda_ret);
		status = DOCA_ERROR_DRIVER;
		goto close_connection;
	}

	cu_result = cuEventCreate(&e_start, CU_EVENT_BLOCKING_SYNC);
	if (cu_result) {
		DOCA_LOG_ERR("Function cuEventCreate for e_start %d", cu_result);
		status = DOCA_ERROR_DRIVER;
		goto destroy_events;
	}

	cu_result = cuEventCreate(&e_end, CU_EVENT_BLOCKING_SYNC);
	if (cu_result) {
		DOCA_LOG_ERR("Function cuEventCreate for e_end %d", cu_result);
		cuEventDestroy(e_start);
		status = DOCA_ERROR_DRIVER;
		goto destroy_events;
	}

	DOCA_LOG_INFO(
		"Launching gpunetio_verbs_twosided_bw kernel with %d CUDA Blocks, %d CUDA threads each, %d total number of iterations, %d iterations per cuda thread %d cpu proxy, %d shared mode",
		VERBS_CUDA_BLOCK,
		resources.cuda_threads / VERBS_CUDA_BLOCK,
		resources.num_iters,
		resources.num_iters / resources.cuda_threads, // check this is ok
		resources.nic_handler,
		resources.scope);

	printf(RESULT_LINE);
	printf(RESULT_FMT_G);
	printf("\n");
	printf(RESULT_LINE);

	status = doca_gpu_verbs_get_qp_dev(resources.qp->qp_gverbs, &qp_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_gpu_verbs_get_qp_dev failed");
		goto destroy_events;
	}

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		/* Warmup per size*/
		status = gpunetio_verbs_two_sided_bw(cstream,
						     qp_gpu,
						     0,
						     resources.num_iters,
						     VERBS_CUDA_BLOCK,
						     resources.cuda_threads / VERBS_CUDA_BLOCK,
						     message_size[idx],
						     resources.data_buf[idx],
						     htobe32(resources.data_mr[idx]->lkey),
						     resources.flag_buf[idx],
						     htobe32(resources.flag_mr[idx]->lkey),
						     (uint64_t *)(resources.remote_flag_buf[idx]),
						     htobe32(resources.remote_flag_mkey[idx]),
						     (uint64_t *)(resources.dump_flag_buf),
						     htobe32(resources.dump_flag_mr->lkey),
						     resources.scope,
						     false);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(status));
			goto destroy_events;
		}

		cudaStreamSynchronize(cstream);

		cu_result = cuEventRecord(e_start, cstream);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventRecord for e_start");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		status = gpunetio_verbs_two_sided_bw(cstream,
						     qp_gpu,
						     resources.num_iters,
						     resources.num_iters,
						     VERBS_CUDA_BLOCK,
						     resources.cuda_threads / VERBS_CUDA_BLOCK,
						     message_size[idx],
						     resources.data_buf[idx],
						     htobe32(resources.data_mr[idx]->lkey),
						     resources.flag_buf[idx],
						     htobe32(resources.flag_mr[idx]->lkey),
						     (uint64_t *)(resources.remote_flag_buf[idx]),
						     htobe32(resources.remote_flag_mkey[idx]),
						     (uint64_t *)(resources.dump_flag_buf),
						     htobe32(resources.dump_flag_mr->lkey),
						     resources.scope,
						     false);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(status));
			goto destroy_events;
		}

		cu_result = cuEventRecord(e_end, cstream);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventRecord for e_end");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		cu_result = cuEventSynchronize(e_end);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventSynchronize for e_end");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		cu_result = cuEventElapsedTime(&et_ms, e_start, e_end);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventElapsedTime");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		// Check calculation is the same as in case of perftest
		double bw = (double)((message_size[idx] * num_messages) / et_ms * 1000.0f / format_factor);
		double msgrate = (double)(num_messages / et_ms * 1000.0f / 1000000.0f);

		printf(REPORT_FMT_EXT, message_size[idx], resources.num_iters, bw, msgrate, (double)et_ms);

		printf("\n");
	}

destroy_events:
	cudaStreamSynchronize(cstream);

	if (cstream != NULL)
		cudaStreamDestroy(cstream);
	if (e_start != NULL)
		cuEventDestroy(e_start);
	if (e_end != NULL)
		cuEventDestroy(e_end);

close_connection:
	oob_verbs_connection_client_close(resources.conn_socket);

server_cleanup:
	tmp_status = destroy_local_memory_objects(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	tmp_status = destroy_verbs_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	return status;
}

doca_error_t verbs_client(struct verbs_config *cfg)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct verbs_resources resources = {0};
	cudaError_t cuda_ret;
	CUresult cu_result;
	CUevent e_start = NULL, e_end = NULL;
	float et_ms = 0.0f;
	const unsigned long format_factor = 0x100000; // -> MBS
	// 125000000;
	const unsigned long num_messages = cfg->num_iters * NUM_QP;
	struct doca_gpu_dev_verbs_qp *qp_gpu;

	resources.conn_socket = -1;
	resources.num_iters = cfg->num_iters;
	resources.cuda_threads = cfg->cuda_threads;
	resources.nic_handler = cfg->nic_handler;
	resources.scope = (enum doca_gpu_dev_verbs_exec_scope)cfg->exec_scope;
	resources.qp_group = false;

	status = create_verbs_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA verbs resources: %s", doca_error_get_descr(status));
		return status;
	}

	status = create_local_memory_object(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local memory resources: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = oob_verbs_connection_client_setup(cfg->server_ip_addr, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	status = connect_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect DOCA verbs QP: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	cuda_ret = cudaStreamCreateWithFlags(&cstream, cudaStreamNonBlocking);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", cuda_ret);
		status = DOCA_ERROR_DRIVER;
		goto close_connection;
	}

	cu_result = cuEventCreate(&e_start, CU_EVENT_BLOCKING_SYNC);
	if (cu_result) {
		DOCA_LOG_ERR("Function cuEventCreate for e_start %d", cu_result);
		status = DOCA_ERROR_DRIVER;
		goto destroy_events;
	}

	cu_result = cuEventCreate(&e_end, CU_EVENT_BLOCKING_SYNC);
	if (cu_result) {
		DOCA_LOG_ERR("Function cuEventCreate for e_end %d", cu_result);
		cuEventDestroy(e_start);
		status = DOCA_ERROR_DRIVER;
		goto destroy_events;
	}

	DOCA_LOG_INFO(
		"Launching gpunetio_verbs_twosided_bw kernel with %d CUDA Blocks, %d CUDA threads each, %d total number of iterations, %d iterations per cuda thread %d cpu proxy, %d shared mode",
		VERBS_CUDA_BLOCK,
		resources.cuda_threads / VERBS_CUDA_BLOCK,
		resources.num_iters,
		resources.num_iters / resources.cuda_threads, // check this is ok
		resources.nic_handler,
		resources.scope);

	printf(RESULT_LINE);
	printf(RESULT_FMT_G);
	printf("\n");
	printf(RESULT_LINE);

	status = doca_gpu_verbs_get_qp_dev(resources.qp->qp_gverbs, &qp_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_gpu_verbs_get_qp_dev failed");
		goto destroy_events;
	}

	for (int idx = 0; idx < NUM_MSG_SIZE; idx++) {
		/* Warmup per size*/
		status = gpunetio_verbs_two_sided_bw(cstream,
						     qp_gpu,
						     0,
						     resources.num_iters,
						     VERBS_CUDA_BLOCK,
						     resources.cuda_threads / VERBS_CUDA_BLOCK,
						     message_size[idx],
						     resources.data_buf[idx],
						     htobe32(resources.data_mr[idx]->lkey),
						     resources.flag_buf[idx],
						     htobe32(resources.flag_mr[idx]->lkey),
						     (uint64_t *)(resources.remote_flag_buf[idx]),
						     htobe32(resources.remote_flag_mkey[idx]),
						     (uint64_t *)(resources.dump_flag_buf),
						     htobe32(resources.dump_flag_mr->lkey),
						     resources.scope,
						     true);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(status));
			goto destroy_events;
		}

		cudaStreamSynchronize(cstream);

		cu_result = cuEventRecord(e_start, cstream);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventRecord for e_start");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		status = gpunetio_verbs_two_sided_bw(cstream,
						     qp_gpu,
						     resources.num_iters,
						     resources.num_iters,
						     VERBS_CUDA_BLOCK,
						     resources.cuda_threads / VERBS_CUDA_BLOCK,
						     message_size[idx],
						     resources.data_buf[idx],
						     htobe32(resources.data_mr[idx]->lkey),
						     resources.flag_buf[idx],
						     htobe32(resources.flag_mr[idx]->lkey),
						     (uint64_t *)(resources.remote_flag_buf[idx]),
						     htobe32(resources.remote_flag_mkey[idx]),
						     resources.prev_flag_buf[idx],
						     htobe32(resources.dump_flag_mr->lkey),
						     resources.scope,
						     true);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(status));
			goto destroy_events;
		}

		cu_result = cuEventRecord(e_end, cstream);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventRecord for e_end");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		cu_result = cuEventSynchronize(e_end);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventSynchronize for e_end");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		cu_result = cuEventElapsedTime(&et_ms, e_start, e_end);
		if (cu_result) {
			DOCA_LOG_ERR("Error in cuEventElapsedTime");
			status = DOCA_ERROR_DRIVER;
			goto destroy_events;
		}

		// Check calculation is the same as in case of perftest
		double bw = (double)((message_size[idx] * num_messages) / et_ms * 1000.0f / format_factor);
		double msgrate = (double)(num_messages / et_ms * 1000.0f / 1000000.0f);

		printf(REPORT_FMT_EXT, message_size[idx], resources.num_iters, bw, msgrate, (double)et_ms);

		printf("\n");
	}

	client_validate_test(&resources);

destroy_events:
	cudaStreamSynchronize(cstream);

	if (cstream != NULL)
		cudaStreamDestroy(cstream);
	if (e_start != NULL)
		cuEventDestroy(e_start);
	if (e_end != NULL)
		cuEventDestroy(e_end);

close_connection:
	oob_verbs_connection_client_close(resources.conn_socket);

client_cleanup:
	tmp_status = destroy_local_memory_objects(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	tmp_status = destroy_verbs_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	return status;
}
