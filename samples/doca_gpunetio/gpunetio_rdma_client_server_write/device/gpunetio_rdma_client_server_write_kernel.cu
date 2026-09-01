/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_gpunetio_dev_buf.cuh>
#include <doca_gpunetio_dev_rdma.cuh>

#include "rdma_common.h"

#define RECV_POSTED 1

DOCA_LOG_REGISTER(GPUNETIO_RDMA_KERNEL);

__global__ void kernel_client(struct doca_gpu_dev_rdma *rdma_gpu,
			      struct doca_gpu_buf_arr *client_local_buf_arr_B,
			      struct doca_gpu_buf_arr *client_local_buf_arr_C,
			      struct doca_gpu_buf_arr *client_local_buf_arr_F,
			      struct doca_gpu_buf_arr *client_remote_buf_arr_A,
			      uint32_t connection_index,
			      uint32_t *exit_flag)
{
	doca_error_t result;
	struct doca_gpu_buf *local_buf_B;
	struct doca_gpu_buf *local_buf_C;
	struct doca_gpu_buf *local_buf_F;
	struct doca_gpu_buf *remote_buf_A;
	uintptr_t local_addr_F;
	int buf_index = 0;
	uint32_t num_ops;

	/*
	 * ******************************************
	 * Flag buffer local: retrieve address
	 * ******************************************
	 */
	/* Get remote/local buffer F info to communicate to client once all RDMA Recv for a given buffer have been
	 * posted */
	doca_gpu_dev_buf_get_buf(client_local_buf_arr_F, 0, &local_buf_F);
	doca_gpu_dev_buf_get_addr(local_buf_F, &local_addr_F);

	/*
	 * ******************************************
	 * Buffer0 : A0 = B0 + C0 with RDMA Write Imm
	 * Buffer1 : A1 = B1 + C1 with RDMA Send Imm
	 * Buffer2 : A2 = B2 + C2 with RDMA Write Imm
	 * Buffer3 : A3 = B3 + C3 with RDMA Send Imm
	 * ******************************************
	 */
	for (buf_index = 0; buf_index < GPU_BUF_NUM; buf_index++) {
		doca_gpu_dev_buf_get_buf(client_local_buf_arr_B, buf_index, &local_buf_B);
		doca_gpu_dev_buf_get_buf(client_local_buf_arr_C, buf_index, &local_buf_C);
		doca_gpu_dev_buf_get_buf(client_remote_buf_arr_A, buf_index, &remote_buf_A);
		/* Wait for server to update flag[0] = 1 to ensure all RDMA Recv have been posted */
		if (threadIdx.x == 0) {
			printf("Client waiting on flag %lx for server to post RDMA Recvs\n",
			       &(((uint8_t *)local_addr_F)[buf_index]));
			while (DOCA_GPUNETIO_VOLATILE(((uint8_t *)local_addr_F)[buf_index]) != RECV_POSTED)
				__threadfence_block();
		}
		__syncthreads();

		if (buf_index % 2 == 0) {
			/* Each CUDA thread posts RDMA Write Imm from a different buffer */
			printf("Thread %d post rdma write imm %d\n", threadIdx.x, buf_index + threadIdx.x);
			if (threadIdx.x == 0) {
				result = doca_gpu_dev_rdma_write_strong(rdma_gpu,
									connection_index,
									remote_buf_A,
									0,
									local_buf_B,
									0,
									GPU_BUF_SIZE_B,
									buf_index + threadIdx.x,
									DOCA_GPU_RDMA_WRITE_FLAG_IMM);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_write_strong\n", result);
			} else {
				result = doca_gpu_dev_rdma_write_strong(rdma_gpu,
									connection_index,
									remote_buf_A,
									GPU_BUF_SIZE_B,
									local_buf_C,
									0,
									GPU_BUF_SIZE_C,
									buf_index + threadIdx.x,
									DOCA_GPU_RDMA_WRITE_FLAG_IMM);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_write_strong\n", result);
			}
		} else {
			/* Each CUDA thread posts RDMA Send Imm from a different buffer */
			printf("Thread %d post rdma send imm %d\n", threadIdx.x, buf_index + threadIdx.x);
			if (threadIdx.x == 0) {
				result = doca_gpu_dev_rdma_send_strong(rdma_gpu,
								       connection_index,
								       local_buf_B,
								       0,
								       GPU_BUF_SIZE_B,
								       buf_index + threadIdx.x,
								       DOCA_GPU_RDMA_SEND_FLAG_IMM);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_send_strong\n", result);
			} else {
				result = doca_gpu_dev_rdma_send_strong(rdma_gpu,
								       connection_index,
								       local_buf_C,
								       0,
								       GPU_BUF_SIZE_C,
								       buf_index + threadIdx.x,
								       DOCA_GPU_RDMA_SEND_FLAG_IMM);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_send_strong\n", result);
			}
		}
		__syncthreads();

		if (threadIdx.x == 0) {
			result = doca_gpu_dev_rdma_commit_strong(rdma_gpu, connection_index);
			if (result != DOCA_SUCCESS)
				printf("Error %d doca_gpu_dev_rdma_push\n", result);
		}
	}

	if (threadIdx.x == 0) {
		/* Ensure all the previous send/writes have been actually done before exit. */
		result = doca_gpu_dev_rdma_wait_all(rdma_gpu, &num_ops);
		if (result != DOCA_SUCCESS)
			printf("Error %d doca_gpu_dev_rdma_push\n", result);

		printf("Client posted and completed %d RDMA commits on connection %d. Waiting on the exit flag.\n",
		       num_ops,
		       connection_index);

		while (DOCA_GPUNETIO_VOLATILE(*exit_flag) == 0)
			;
	}

	__syncthreads();

	return;
}

__global__ void kernel_server(struct doca_gpu_dev_rdma *rdma_gpu,
			      struct doca_gpu_buf_arr *server_local_buf_arr_A,
			      struct doca_gpu_buf_arr *server_remote_buf_arr_F,
			      uint32_t connection_index)
{
	doca_error_t result;
	struct doca_gpu_buf *remote_buf_F;
	uint8_t local_flag_inl[GPU_BUF_SIZE_F];
	struct doca_gpu_buf *local_buf_A;
	// enum doca_rdma_opcode opcode;
	int buf_index = 0;
	uint32_t num_ops;
	struct doca_gpu_dev_rdma_r *rdma_gpu_r;
	uint32_t imm_val[2];
	uint32_t conn_idx[2];

	result = doca_gpu_dev_rdma_get_recv(rdma_gpu, &rdma_gpu_r);
	if (result != DOCA_SUCCESS)
		printf("Error %d doca_gpu_dev_buf_get_buf\n", result);

	/*
	 * ********************************************
	 * Flag buffer local & remote: retrieve address
	 * ********************************************
	 */
	/* Prepare inline local flag and get remote buffer F info to communicate to client once all RDMA Recv for a
	 * given buffer have been posted */
	local_flag_inl[0] = 1;
	doca_gpu_dev_buf_get_buf(server_remote_buf_arr_F, 0, &remote_buf_F);

	/*
	 * ******************************************
	 * Buffer0 : A0 = B0 + C0 with RDMA Write Imm
	 * Buffer1 : A1 = B1 + C1 with RDMA Send Imm
	 * Buffer2 : A2 = B2 + C2 with RDMA Write Imm
	 * Buffer3 : A3 = B3 + C3 with RDMA Send Imm
	 * ******************************************
	 */
	for (buf_index = 0; buf_index < GPU_BUF_NUM; buf_index++) {
		/* Each CUDA thread prepares in parallel a receive on different offset for buffer */
		if (buf_index % 2 == 0) {
			/* RDMA Recv for RDMA Write Imm */
			result = doca_gpu_dev_rdma_recv_strong(rdma_gpu_r, NULL, 0, 0, 0);
			if (result != DOCA_SUCCESS)
				printf("Error %d doca_gpu_dev_rdma_recv_strong\n", result);
		} else {
			/* RDMA Recv for RDMA Send Imm */
			doca_gpu_dev_buf_get_buf(server_local_buf_arr_A, buf_index, &local_buf_A);
			/* For simplicity, assumption is that GPU_BUF_SIZE_B == GPU_BUF_SIZE_C */
			result = doca_gpu_dev_rdma_recv_strong(rdma_gpu_r,
							       local_buf_A,
							       GPU_BUF_SIZE_B,
							       (threadIdx.x * GPU_BUF_SIZE_B),
							       0);
			if (result != DOCA_SUCCESS)
				printf("Error %d doca_gpu_dev_rdma_recv_strong\n", result);
		}

		/* Wait all CUDA threads to post their receive */
		__threadfence_block();
		__syncthreads();

		if (threadIdx.x == 0) {
			/* Only 1 CUDA thread can commit the receive ops just posted */
			result = doca_gpu_dev_rdma_recv_commit_strong(rdma_gpu_r);
			if (result != DOCA_SUCCESS)
				printf("Error %d doca_gpu_dev_rdma_recv_commit_strong\n", result);

			/* As only 1 RDMA Write is required here, same thread can enqueue and push the write op */
			if (buf_index % 2 == 0) {
				/* Strong inline mode */
				result = doca_gpu_dev_rdma_write_inline_strong(rdma_gpu,
									       connection_index,
									       remote_buf_F,
									       buf_index,
									       local_flag_inl,
									       GPU_BUF_SIZE_F,
									       0,
									       DOCA_GPU_RDMA_WRITE_FLAG_NONE);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_write_strong\n", result);

				result = doca_gpu_dev_rdma_commit_strong(rdma_gpu, connection_index);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_commit\n", result);
			} else {
				/* Weak inline mode */
				result = doca_gpu_dev_rdma_write_inline_weak(rdma_gpu,
									     connection_index,
									     remote_buf_F,
									     buf_index,
									     local_flag_inl,
									     GPU_BUF_SIZE_F,
									     0,
									     DOCA_GPU_RDMA_WRITE_FLAG_NONE,
									     buf_index);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_write_strong\n", result);

				result = doca_gpu_dev_rdma_commit_weak(rdma_gpu, connection_index, 1);
				if (result != DOCA_SUCCESS)
					printf("Error %d doca_gpu_dev_rdma_commit\n", result);
			}

			/* Each CUDA thread waits in parallel on a different RDMA receive operation */
			result = doca_gpu_dev_rdma_recv_wait_all(rdma_gpu_r,
								 DOCA_GPU_RDMA_RECV_WAIT_FLAG_B,
								 &num_ops,
								 imm_val,
								 conn_idx);
			if (result != DOCA_SUCCESS)
				printf("Error %d doca_gpu_dev_rdma_recv_wait\n", result);

			printf("RDMA Recv %d ops completed with immediate values %d and %d! Connections %d and %d\n",
			       num_ops,
			       imm_val[0],
			       imm_val[1],
			       conn_idx[0],
			       conn_idx[1]);
		}

		__syncthreads();
	}

	return;
}

extern "C" {

doca_error_t kernel_write_server(cudaStream_t stream,
				 struct doca_gpu_dev_rdma *rdma_gpu,
				 struct doca_gpu_buf_arr *server_local_buf_arr_A,
				 struct doca_gpu_buf_arr *server_remote_buf_arr_F,
				 uint32_t connection_index)
{
	cudaError_t result = cudaSuccess;

	if (rdma_gpu == NULL || server_local_buf_arr_A == NULL || server_remote_buf_arr_F == NULL) {
		DOCA_LOG_ERR("kernel_write_server invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (result != cudaSuccess) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	kernel_server<<<1, GPU_NUM_OP_X_BUF, 0, stream>>>(rdma_gpu,
							  server_local_buf_arr_A,
							  server_remote_buf_arr_F,
							  connection_index);
	result = cudaGetLastError();
	if (result != cudaSuccess) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

doca_error_t kernel_write_client(cudaStream_t stream,
				 struct doca_gpu_dev_rdma *rdma_gpu,
				 struct doca_gpu_buf_arr *client_local_buf_arr_B,
				 struct doca_gpu_buf_arr *client_local_buf_arr_C,
				 struct doca_gpu_buf_arr *client_local_buf_arr_F,
				 struct doca_gpu_buf_arr *client_remote_buf_arr_A,
				 uint32_t connection_index,
				 uint32_t *exit_flag)
{
	cudaError_t result = cudaSuccess;

	if (rdma_gpu == NULL || client_local_buf_arr_B == NULL || client_local_buf_arr_C == NULL ||
	    client_local_buf_arr_F == NULL || client_remote_buf_arr_A == NULL || exit_flag == NULL) {
		DOCA_LOG_ERR("kernel_write_client invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	kernel_client<<<1, GPU_NUM_OP_X_BUF, 0, stream>>>(rdma_gpu,
							  client_local_buf_arr_B,
							  client_local_buf_arr_C,
							  client_local_buf_arr_F,
							  client_remote_buf_arr_A,
							  connection_index,
							  exit_flag);

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
