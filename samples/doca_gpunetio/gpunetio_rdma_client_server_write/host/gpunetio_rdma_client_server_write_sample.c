/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <string.h>

#include <doca_log.h>
#include <doca_error.h>
#include <doca_argp.h>

#include "rdma_common.h"
#include "common.h"

DOCA_LOG_REGISTER(GPURDMA::SAMPLE);

#define SLEEP_IN_NANOS (10 * 1000)
#define NUM_CONN 2

struct rdma_resources resources = {0};
struct rdma_mmap_obj server_local_mmap_obj_A[NUM_CONN] = {0};
struct rdma_mmap_obj client_local_mmap_obj_B[NUM_CONN] = {0};
struct rdma_mmap_obj client_local_mmap_obj_C[NUM_CONN] = {0};
struct rdma_mmap_obj client_local_mmap_obj_F[NUM_CONN] = {0};
struct doca_mmap *server_remote_mmap_F[NUM_CONN];
struct doca_mmap *client_remote_mmap_A[NUM_CONN];
const uint32_t access_params = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE;
uint8_t *server_local_buf_A_gpu[NUM_CONN];
uint8_t *server_local_buf_A_cpu[NUM_CONN];
uint8_t *client_local_buf_B_gpu[NUM_CONN];
uint8_t *client_local_buf_B_cpu[NUM_CONN];
uint8_t *client_local_buf_C_gpu[NUM_CONN];
uint8_t *client_local_buf_C_cpu[NUM_CONN];
uint8_t *client_local_buf_F[NUM_CONN];
struct buf_arr_obj server_local_buf_arr_A[NUM_CONN] = {0};
struct buf_arr_obj server_remote_buf_arr_F[NUM_CONN] = {0};
struct buf_arr_obj client_remote_buf_arr_A[NUM_CONN] = {0};
struct buf_arr_obj client_local_buf_arr_B[NUM_CONN] = {0};
struct buf_arr_obj client_local_buf_arr_C[NUM_CONN] = {0};
struct buf_arr_obj client_local_buf_arr_F[NUM_CONN] = {0};
cudaStream_t cstream;
int oob_sock_fd = -1;
int oob_client_sock = -1;

/*
 * Create local and remote mmap and buffer array for server
 *
 * @oob_sock_fd [in]: socket fd
 * @resources [in]: rdma resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_memory_local_remote_server(int oob_sock_fd,
						      struct rdma_resources *resources,
						      int conn_idx,
						      cudaStream_t stream)
{
	void *server_remote_export_F = NULL;
	size_t server_remote_export_F_len;
	doca_error_t result;
	cudaError_t cuda_err;

	/* Buffer A */
	/* Register local source buffer obtain an object representing the memory */
	result = doca_gpu_mem_alloc(resources->gpudev,
				    (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_A,
				    4096,
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&server_local_buf_A_gpu[conn_idx],
				    (void **)&server_local_buf_A_cpu[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc failed: %s", doca_error_get_descr(result));
		goto error;
	}

	cuda_err = cudaMemsetAsync(server_local_buf_A_gpu[conn_idx], 0x1, GPU_BUF_NUM * GPU_BUF_SIZE_A, stream);
	if (cuda_err != cudaSuccess) {
		DOCA_LOG_ERR("Can't CUDA memset buffer A: %d", cuda_err);
		goto error;
	}

	server_local_mmap_obj_A[conn_idx].doca_device = resources->doca_device;
	server_local_mmap_obj_A[conn_idx].permissions = access_params;
	server_local_mmap_obj_A[conn_idx].memrange_addr = server_local_buf_A_gpu[conn_idx];
	server_local_mmap_obj_A[conn_idx].memrange_len = (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_A;

	DOCA_LOG_INFO("Create local server mmap A context");
	result = create_mmap(resources->gpudev, &server_local_mmap_obj_A[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_mmap failed: %s", doca_error_get_descr(result));
		goto error;
	}

	/* Application does out-of-band passing of exported mmap to remote side and receiving exported mmap */
	DOCA_LOG_INFO("Send exported mmap A to remote client");
	if (send(oob_sock_fd, &server_local_mmap_obj_A[conn_idx].export_len, sizeof(size_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send exported mmap");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	if (send(oob_sock_fd,
		 server_local_mmap_obj_A[conn_idx].rdma_export,
		 server_local_mmap_obj_A[conn_idx].export_len,
		 0) < 0) {
		DOCA_LOG_ERR("Failed to send exported mmap");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	DOCA_LOG_INFO("Receive client mmap F export");
	if (recv(oob_sock_fd, &server_remote_export_F_len, sizeof(size_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote connection details");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	server_remote_export_F = calloc(1, server_remote_export_F_len);
	if (server_remote_export_F == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for remote mmap export");
		result = DOCA_ERROR_NO_MEMORY;
		goto error;
	}

	if (recv(oob_sock_fd, server_remote_export_F, server_remote_export_F_len, 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote connection details");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	result = doca_mmap_create_from_export(NULL,
					      server_remote_export_F,
					      server_remote_export_F_len,
					      resources->doca_device,
					      &server_remote_mmap_F[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_create_from_export failed: %s", doca_error_get_descr(result));
		goto error;
	}

	/* create local and remote buf arrays */
	server_local_buf_arr_A[conn_idx].gpudev = resources->gpudev;
	server_local_buf_arr_A[conn_idx].mmap = server_local_mmap_obj_A[conn_idx].mmap;
	server_local_buf_arr_A[conn_idx].num_elem = GPU_BUF_NUM;
	server_local_buf_arr_A[conn_idx].elem_size = GPU_BUF_SIZE_A;

	DOCA_LOG_INFO("Create local DOCA buf array context A");
	result = create_buf_arr_on_gpu(&server_local_buf_arr_A[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_buf_arr_on_gpu failed: %s", doca_error_get_descr(result));
		goto error;
	}

	server_remote_buf_arr_F[conn_idx].gpudev = resources->gpudev;
	server_remote_buf_arr_F[conn_idx].mmap = server_remote_mmap_F[conn_idx];
	server_remote_buf_arr_F[conn_idx].num_elem = 1;
	server_remote_buf_arr_F[conn_idx].elem_size = (size_t)(GPU_BUF_NUM * GPU_BUF_SIZE_F);

	DOCA_LOG_INFO("Create remote DOCA buf array context F");
	result = create_buf_arr_on_gpu(&server_remote_buf_arr_F[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_buf_arr_on_gpu failed: %s", doca_error_get_descr(result));
		doca_buf_arr_destroy(server_local_buf_arr_A[conn_idx].buf_arr);
		goto error;
	}

	free(server_remote_export_F);

	return DOCA_SUCCESS;

error:
	if (server_remote_export_F)
		free(server_remote_export_F);

	return result;
}

/*
 * Create local and remote mmap and buffer array for client
 *
 * @oob_sock_fd [in]: socket fd
 * @resources [in]: rdma resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_memory_local_remote_client(int oob_sock_fd,
						      struct rdma_resources *resources,
						      int conn_idx,
						      cudaStream_t stream)
{
	void *client_remote_export_A = NULL;
	size_t client_remote_export_A_len;
	doca_error_t result;
	cudaError_t cuda_err;

	DOCA_LOG_INFO("Alloc local client mmap B context");
	/* Buffer B - 512B */
	/* Register local source buffer obtain an object representing the memory */
	result = doca_gpu_mem_alloc(resources->gpudev,
				    (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_B,
				    4096,
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&client_local_buf_B_gpu[conn_idx],
				    (void **)&client_local_buf_B_cpu[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc failed: %s", doca_error_get_descr(result));
		goto error;
	}

	DOCA_LOG_INFO("Memset local client mmap B context");

	cuda_err = cudaMemsetAsync(client_local_buf_B_gpu[conn_idx], 0x2, GPU_BUF_NUM * GPU_BUF_SIZE_B, stream);
	if (cuda_err != cudaSuccess) {
		DOCA_LOG_ERR("Can't CUDA memset buffer B: %d", cuda_err);
		goto error;
	}

	client_local_mmap_obj_B[conn_idx].doca_device = resources->doca_device;
	client_local_mmap_obj_B[conn_idx].permissions = access_params;
	client_local_mmap_obj_B[conn_idx].memrange_addr = client_local_buf_B_gpu[conn_idx];
	client_local_mmap_obj_B[conn_idx].memrange_len = (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_B;

	/* create local mmap object */
	DOCA_LOG_INFO("Create local client mmap B context");
	result = create_mmap(resources->gpudev, &client_local_mmap_obj_B[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_mmap failed: %s", doca_error_get_descr(result));
		goto error;
	}

	/* Buffer C - 512B */
	/* Register local source buffer obtain an object representing the memory */
	result = doca_gpu_mem_alloc(resources->gpudev,
				    (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_C,
				    4096,
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&client_local_buf_C_gpu[conn_idx],
				    (void **)&client_local_buf_C_cpu[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc failed: %s", doca_error_get_descr(result));
		goto error;
	}

	cuda_err = cudaMemsetAsync(client_local_buf_C_gpu[conn_idx], 0x3, GPU_BUF_NUM * GPU_BUF_SIZE_C, stream);
	if (cuda_err != cudaSuccess) {
		DOCA_LOG_ERR("Can't CUDA memset buffer C: %d", cuda_err);
		goto error;
	}

	client_local_mmap_obj_C[conn_idx].doca_device = resources->doca_device;
	client_local_mmap_obj_C[conn_idx].permissions = access_params;
	client_local_mmap_obj_C[conn_idx].memrange_addr = client_local_buf_C_gpu[conn_idx];
	client_local_mmap_obj_C[conn_idx].memrange_len = (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_C;

	/* create local mmap object */
	DOCA_LOG_INFO("Create local client mmap C context");
	result = create_mmap(resources->gpudev, &client_local_mmap_obj_C[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_mmap failed: %s", doca_error_get_descr(result));
		goto error;
	}

	/* Buffer F - 4B */
	/* Register local source buffer obtain an object representing the memory */
	result = doca_gpu_mem_alloc(resources->gpudev,
				    (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_F,
				    4096,
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&client_local_buf_F[conn_idx],
				    NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc failed: %s", doca_error_get_descr(result));
		goto error;
	}

	client_local_mmap_obj_F[conn_idx].doca_device = resources->doca_device;
	client_local_mmap_obj_F[conn_idx].permissions = access_params;
	client_local_mmap_obj_F[conn_idx].memrange_addr = client_local_buf_F[conn_idx];
	client_local_mmap_obj_F[conn_idx].memrange_len = (size_t)GPU_BUF_NUM * GPU_BUF_SIZE_F;

	/* create local mmap object */
	DOCA_LOG_INFO("Create local client mmap F context");
	result = create_mmap(resources->gpudev, &client_local_mmap_obj_F[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_mmap failed: %s", doca_error_get_descr(result));
		goto error;
	}

	/* Application does out-of-band passing of exported mmap to remote side and receiving exported mmap */

	/* Receive server remote A */
	DOCA_LOG_INFO("Receive remote mmap A export from server");
	if (recv(oob_sock_fd, &client_remote_export_A_len, sizeof(size_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote connection details");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	client_remote_export_A = calloc(1, client_remote_export_A_len);
	if (client_remote_export_A == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for remote mmap export");
		result = DOCA_ERROR_NO_MEMORY;
		goto error;
	}

	if (recv(oob_sock_fd, client_remote_export_A, client_remote_export_A_len, 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote connection details");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	result = doca_mmap_create_from_export(NULL,
					      client_remote_export_A,
					      client_remote_export_A_len,
					      resources->doca_device,
					      &client_remote_mmap_A[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_create_from_export failed: %s", doca_error_get_descr(result));
		goto error;
	}

	/* Send client local F */
	DOCA_LOG_INFO("Send exported mmap F to remote server");
	if (send(oob_sock_fd, &client_local_mmap_obj_F[conn_idx].export_len, sizeof(size_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send exported mmap");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	if (send(oob_sock_fd,
		 client_local_mmap_obj_F[conn_idx].rdma_export,
		 client_local_mmap_obj_F[conn_idx].export_len,
		 0) < 0) {
		DOCA_LOG_ERR("Failed to send exported mmap");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto error;
	}

	/* create local and remote buf arrays */
	client_local_buf_arr_B[conn_idx].gpudev = resources->gpudev;
	client_local_buf_arr_B[conn_idx].mmap = client_local_mmap_obj_B[conn_idx].mmap;
	client_local_buf_arr_B[conn_idx].num_elem = GPU_BUF_NUM;
	client_local_buf_arr_B[conn_idx].elem_size = GPU_BUF_SIZE_B;

	/* create local buf array object */
	DOCA_LOG_INFO("Create local DOCA buf array context B");
	result = create_buf_arr_on_gpu(&client_local_buf_arr_B[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_buf_arr_on_gpu failed: %s", doca_error_get_descr(result));
		goto error;
	}

	client_local_buf_arr_C[conn_idx].gpudev = resources->gpudev;
	client_local_buf_arr_C[conn_idx].mmap = client_local_mmap_obj_C[conn_idx].mmap;
	client_local_buf_arr_C[conn_idx].num_elem = GPU_BUF_NUM;
	client_local_buf_arr_C[conn_idx].elem_size = GPU_BUF_SIZE_C;

	/* create local buf array object */
	DOCA_LOG_INFO("Create local DOCA buf array context C");
	result = create_buf_arr_on_gpu(&client_local_buf_arr_C[conn_idx]);
	if (result != DOCA_SUCCESS) {
		doca_buf_arr_destroy(client_local_buf_arr_B[conn_idx].buf_arr);
		DOCA_LOG_ERR("Function create_buf_arr_on_gpu failed: %s", doca_error_get_descr(result));
		goto error;
	}

	client_local_buf_arr_F[conn_idx].gpudev = resources->gpudev;
	client_local_buf_arr_F[conn_idx].mmap = client_local_mmap_obj_F[conn_idx].mmap;
	client_local_buf_arr_F[conn_idx].num_elem = 1;
	client_local_buf_arr_F[conn_idx].elem_size = (size_t)(GPU_BUF_NUM * GPU_BUF_SIZE_F);

	/* create local buf array object */
	DOCA_LOG_INFO("Create local DOCA buf array context F");
	result = create_buf_arr_on_gpu(&client_local_buf_arr_F[conn_idx]);
	if (result != DOCA_SUCCESS) {
		doca_buf_arr_destroy(client_local_buf_arr_B[conn_idx].buf_arr);
		DOCA_LOG_ERR("Function create_buf_arr_on_gpu failed: %s", doca_error_get_descr(result));
		goto error;
	}

	client_remote_buf_arr_A[conn_idx].gpudev = resources->gpudev;
	client_remote_buf_arr_A[conn_idx].mmap = client_remote_mmap_A[conn_idx];
	client_remote_buf_arr_A[conn_idx].num_elem = GPU_BUF_NUM;
	client_remote_buf_arr_A[conn_idx].elem_size = GPU_BUF_SIZE_A;

	/* create remote buf array object */
	DOCA_LOG_INFO("Create remote DOCA buf array context");
	result = create_buf_arr_on_gpu(&client_remote_buf_arr_A[conn_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_buf_arr_on_gpu failed: %s", doca_error_get_descr(result));
		doca_buf_arr_destroy(client_local_buf_arr_B[conn_idx].buf_arr);
		doca_buf_arr_destroy(client_local_buf_arr_C[conn_idx].buf_arr);
		goto error;
	}

	free(client_remote_export_A);

	return DOCA_SUCCESS;

error:
	if (client_remote_export_A)
		free(client_remote_export_A);

	return result;
}

/*
 * Destroy local and remote mmap and buffer array, server side
 *
 * @resources [in]: rdma resources
 */
static void destroy_memory_local_remote_server(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;

	for (int conn_idx = 0; conn_idx < NUM_CONN; conn_idx++) {
		if (server_local_mmap_obj_A[conn_idx].mmap) {
			result = doca_mmap_destroy(server_local_mmap_obj_A[conn_idx].mmap);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_mmap_destroy failed: %s", doca_error_get_descr(result));
		}

		if (server_remote_mmap_F[conn_idx]) {
			result = doca_mmap_destroy(server_remote_mmap_F[conn_idx]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_mmap_destroy failed: %s", doca_error_get_descr(result));
		}

		if (server_local_buf_A_gpu[conn_idx]) {
			result = doca_gpu_mem_free(resources->gpudev, server_local_buf_A_gpu[conn_idx]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_gpu_mem_free failed: %s", doca_error_get_descr(result));
		}

		if (server_local_buf_arr_A[conn_idx].buf_arr) {
			result = doca_buf_arr_destroy(server_local_buf_arr_A[conn_idx].buf_arr);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_buf_arr_destroy failed: %s", doca_error_get_descr(result));
		}

		if (server_remote_buf_arr_F[conn_idx].buf_arr) {
			result = doca_buf_arr_destroy(server_remote_buf_arr_F[conn_idx].buf_arr);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_buf_arr_destroy failed: %s", doca_error_get_descr(result));
		}
	}
}

/*
 * Destroy local and remote mmap and buffer array, client side
 *
 * @resources [in]: rdma resources
 */
static void destroy_memory_local_remote_client(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;

	for (int conn_idx = 0; conn_idx < NUM_CONN; conn_idx++) {
		if (client_local_mmap_obj_B[conn_idx].mmap) {
			result = doca_mmap_destroy(client_local_mmap_obj_B[conn_idx].mmap);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_mmap_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_local_mmap_obj_C[conn_idx].mmap) {
			result = doca_mmap_destroy(client_local_mmap_obj_C[conn_idx].mmap);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_mmap_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_local_mmap_obj_F[conn_idx].mmap) {
			result = doca_mmap_destroy(client_local_mmap_obj_F[conn_idx].mmap);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_mmap_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_remote_mmap_A[conn_idx]) {
			result = doca_mmap_destroy(client_remote_mmap_A[conn_idx]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_mmap_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_local_buf_B_gpu[conn_idx]) {
			result = doca_gpu_mem_free(resources->gpudev, client_local_buf_B_gpu[conn_idx]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_gpu_mem_free failed: %s", doca_error_get_descr(result));
		}

		if (client_local_buf_C_gpu[conn_idx]) {
			result = doca_gpu_mem_free(resources->gpudev, client_local_buf_C_gpu[conn_idx]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_gpu_mem_free failed: %s", doca_error_get_descr(result));
		}

		if (client_local_buf_F[conn_idx]) {
			result = doca_gpu_mem_free(resources->gpudev, client_local_buf_F[conn_idx]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_gpu_mem_free failed: %s", doca_error_get_descr(result));
		}

		if (client_local_buf_arr_B[conn_idx].buf_arr) {
			result = doca_buf_arr_destroy(client_local_buf_arr_B[conn_idx].buf_arr);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_buf_arr_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_local_buf_arr_C[conn_idx].buf_arr) {
			result = doca_buf_arr_destroy(client_local_buf_arr_C[conn_idx].buf_arr);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_buf_arr_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_local_buf_arr_F[conn_idx].buf_arr) {
			result = doca_buf_arr_destroy(client_local_buf_arr_F[conn_idx].buf_arr);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_buf_arr_destroy failed: %s", doca_error_get_descr(result));
		}

		if (client_remote_buf_arr_A[conn_idx].buf_arr) {
			result = doca_buf_arr_destroy(client_remote_buf_arr_A[conn_idx].buf_arr);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Function doca_buf_arr_destroy failed: %s", doca_error_get_descr(result));
		}
	}
}

/*
 * Server side of the RDMA write
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_write_server(struct rdma_config *cfg)
{
	struct doca_rdma_connection *connection = NULL;
	const uint32_t rdma_permissions = access_params;
	doca_error_t result, tmp_result;
	void *remote_conn_details = NULL;
	size_t remote_conn_details_len = 0;
	cudaError_t cuda_ret;
	int ret = 0;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Allocate resources */
	result = create_rdma_resources(cfg, rdma_permissions, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate RDMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get GPU RDMA handle */
	result = doca_rdma_get_gpu_handle(resources.rdma, &(resources.gpu_rdma));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get RDMA GPU handler: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	/* Setup OOB connection */
	ret = oob_connection_server_setup(&oob_sock_fd, &oob_client_sock);
	if (ret < 0) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto destroy_resources;
	}

	if (!cfg->use_rdma_cm) {
		/* Export connection details */
		result = doca_rdma_export(resources.rdma,
					  &(resources.connection_details),
					  &(resources.conn_det_len),
					  &connection);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to export RDMA with connection details");
			goto close_connection;
		}

		/* Application does out-of-band passing of rdma address to remote side and receiving remote address */
		DOCA_LOG_INFO("Send connection details to remote peer size %zd str %s",
			      resources.conn_det_len,
			      (char *)resources.connection_details);
		if (send(oob_client_sock, &resources.conn_det_len, sizeof(size_t), 0) < 0) {
			DOCA_LOG_ERR("Failed to send connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		if (send(oob_client_sock, resources.connection_details, resources.conn_det_len, 0) < 0) {
			DOCA_LOG_ERR("Failed to send connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		DOCA_LOG_INFO("Receive remote connection details");
		if (recv(oob_client_sock, &remote_conn_details_len, sizeof(size_t), 0) < 0) {
			DOCA_LOG_ERR("Failed to receive remote connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		if (remote_conn_details_len <= 0 || remote_conn_details_len >= (size_t)-1) {
			DOCA_LOG_ERR("Received wrong remote connection details");
			result = DOCA_ERROR_NO_MEMORY;
			goto close_connection;
		}

		remote_conn_details = calloc(1, remote_conn_details_len);
		if (remote_conn_details == NULL) {
			DOCA_LOG_ERR("Failed to allocate memory for remote connection details");
			result = DOCA_ERROR_NO_MEMORY;
			goto close_connection;
		}

		if (recv(oob_client_sock, remote_conn_details, remote_conn_details_len, 0) < 0) {
			DOCA_LOG_ERR("Failed to receive remote connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		/* Connect local rdma to the remote rdma */
		DOCA_LOG_INFO("Connect DOCA RDMA to remote RDMA");
		result = doca_rdma_connect(resources.rdma, remote_conn_details, remote_conn_details_len, connection);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_rdma_connect failed: %s", doca_error_get_descr(result));
			goto close_connection;
		}

		free(remote_conn_details);
		remote_conn_details = NULL;
	} else { /* Case of RDMA CM */
		result = doca_rdma_start_listen_to_port(resources.rdma, cfg->cm_port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Server failed to call doca_rdma_start_listen_to_port: %s",
				     doca_error_get_descr(result));
			goto close_connection;
		}

		resources.server_listen_active = true;

		DOCA_LOG_INFO("Server is waiting for new connections using RDMA CM");
		/* Wait for a new connection */
		while ((!resources.connection_established) && (!resources.connection_error)) {
			if (doca_pe_progress(resources.pe) == 0)
				nanosleep(&ts, &ts);
		}

		if (resources.connection_error) {
			DOCA_LOG_ERR("Failed to connect to remote peer, connection error");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		DOCA_LOG_INFO("Server - Connection 1 is established");
	}

	cuda_ret = cudaStreamCreateWithFlags(&cstream, cudaStreamNonBlocking);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", cuda_ret);
		result = DOCA_ERROR_DRIVER;
		goto close_connection;
	}

	result = create_memory_local_remote_server(oob_client_sock, &resources, 0, cstream);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_memory_local_remote_server failed: %s", doca_error_get_descr(result));
		goto close_connection;
	}

	DOCA_LOG_INFO("Before launching CUDA kernel, buffer array A is:");
	for (int idx = 0; idx < 4; idx++) {
		DOCA_LOG_INFO("Buffer %d -> offset 0: %x%x%x%x | offset %d: %x%x%x%x",
			      idx,
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 0],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 1],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 2],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 3],
			      GPU_BUF_SIZE_B,
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 0],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 1],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 2],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 3]);
	}

	result = kernel_write_server(cstream,
				     resources.gpu_rdma,
				     server_local_buf_arr_A[0].gpu_buf_arr,
				     server_remote_buf_arr_F[0].gpu_buf_arr,
				     0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function kernel_write_server failed: %s", doca_error_get_descr(result));
		goto close_connection;
	}

	if (cfg->use_rdma_cm) {
		/* Wait for a new connection */
		while ((!resources.connection2_established) && (!resources.connection2_error)) {
			if (doca_pe_progress(resources.pe) == 0)
				nanosleep(&ts, &ts);
		}

		if (resources.connection2_error) {
			DOCA_LOG_ERR("Failed to connect to remote peer, connection error");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		DOCA_LOG_INFO("Server - Connection 2 is established");

		result = create_memory_local_remote_server(oob_client_sock, &resources, 1, cstream);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function create_memory_local_remote_server failed: %s",
				     doca_error_get_descr(result));
			goto close_connection;
		}

		DOCA_LOG_INFO("Server - Connection 2 memory info exchanged");

		/* Differently from client, here the server uses the same stream for the two CUDA kernels */
		result = kernel_write_server(cstream,
					     resources.gpu_rdma,
					     server_local_buf_arr_A[1].gpu_buf_arr,
					     server_remote_buf_arr_F[1].gpu_buf_arr,
					     1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_server failed: %s", doca_error_get_descr(result));
			goto close_connection;
		}
	}

	cudaStreamSynchronize(cstream);

	DOCA_LOG_INFO("After launching CUDA kernel, buffer array A is:");
	for (int idx = 0; idx < 4; idx++) {
		DOCA_LOG_INFO("Buffer %d -> offset 0: %x%x%x%x | offset %d: %x%x%x%x",
			      idx,
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 0],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 1],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 2],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + 3],
			      GPU_BUF_SIZE_B,
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 0],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 1],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 2],
			      server_local_buf_A_cpu[0][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 3]);
	}

	if (cfg->use_rdma_cm) {
		DOCA_LOG_INFO("After launching CUDA kernel for connection 2, buffer array A is:");
		for (int idx = 0; idx < 4; idx++) {
			DOCA_LOG_INFO("Buffer %d -> offset 0: %x%x%x%x | offset %d: %x%x%x%x",
				      idx,
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + 0],
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + 1],
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + 2],
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + 3],
				      GPU_BUF_SIZE_B,
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 0],
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 1],
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 2],
				      server_local_buf_A_cpu[1][(GPU_BUF_SIZE_A * idx) + GPU_BUF_SIZE_B + 3]);
		}
	}

	oob_connection_server_close(oob_sock_fd, oob_client_sock);

	destroy_memory_local_remote_server(&resources);

	result = destroy_rdma_resources(&resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;

close_connection:
	oob_connection_server_close(oob_sock_fd, oob_client_sock);

destroy_resources:

	destroy_memory_local_remote_server(&resources);

	tmp_result = destroy_rdma_resources(&resources);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA resources: %s", doca_error_get_descr(tmp_result));

	if (remote_conn_details)
		free(remote_conn_details);

	return result;
}

/*
 * Client side of the RDMA write
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_write_client(struct rdma_config *cfg)
{
	struct doca_rdma_connection *connection = NULL;
	const uint32_t rdma_permissions = access_params;
	doca_error_t result, temp_result;
	cudaError_t cuda_ret;
	void *remote_conn_details = NULL;
	size_t remote_conn_details_len = 0;
	int ret = 0;
	union doca_data connection_data;
	uint32_t *cpu_exit_flag;
	uint32_t *gpu_exit_flag;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Allocate resources */
	result = create_rdma_resources(cfg, rdma_permissions, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate RDMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get GPU RDMA handle */
	result = doca_rdma_get_gpu_handle(resources.rdma, &(resources.gpu_rdma));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get RDMA GPU handler: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	/* Setup OOB connection */
	ret = oob_connection_client_setup(cfg->server_ip_addr, &oob_sock_fd);
	if (ret < 0) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer");
		result = DOCA_ERROR_CONNECTION_ABORTED;
		goto destroy_resources;
	}

	if (!cfg->use_rdma_cm) {
		/* Export connection details */
		result = doca_rdma_export(resources.rdma,
					  &(resources.connection_details),
					  &(resources.conn_det_len),
					  &connection);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to export RDMA with connection details");
			goto close_connection;
		}

		/* Application does out-of-band passing of rdma address to remote side and receiving remote address */
		DOCA_LOG_INFO("Receive remote connection details");
		if (recv(oob_sock_fd, &remote_conn_details_len, sizeof(size_t), 0) < 0) {
			DOCA_LOG_ERR("Failed to receive remote connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		if (remote_conn_details_len <= 0 || remote_conn_details_len >= (size_t)-1) {
			DOCA_LOG_ERR("Received wrong remote connection details");
			result = DOCA_ERROR_NO_MEMORY;
			goto close_connection;
		}

		remote_conn_details = calloc(1, remote_conn_details_len);
		if (remote_conn_details == NULL) {
			DOCA_LOG_ERR("Failed to allocate memory for remote connection details");
			result = DOCA_ERROR_NO_MEMORY;
			goto close_connection;
		}

		if (recv(oob_sock_fd, remote_conn_details, remote_conn_details_len, 0) < 0) {
			DOCA_LOG_ERR("Failed to receive remote connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		DOCA_LOG_INFO("Send connection details to remote peer size %zd str %s",
			      resources.conn_det_len,
			      (char *)resources.connection_details);
		if (send(oob_sock_fd, &resources.conn_det_len, sizeof(size_t), 0) < 0) {
			DOCA_LOG_ERR("Failed to send connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		if (send(oob_sock_fd, resources.connection_details, resources.conn_det_len, 0) < 0) {
			DOCA_LOG_ERR("Failed to send connection details");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		/* Connect local rdma to the remote rdma */
		DOCA_LOG_INFO("Connect DOCA RDMA to remote RDMA");
		result = doca_rdma_connect(resources.rdma, remote_conn_details, remote_conn_details_len, connection);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_rdma_connect failed: %s", doca_error_get_descr(result));
			goto close_connection;
		}

		free(remote_conn_details);
		remote_conn_details = NULL;
	} else { /* Case of RDMA CM */
		result = doca_rdma_addr_create(cfg->cm_addr_type, cfg->cm_addr, cfg->cm_port, &resources.cm_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create rdma cm connection address %s", doca_error_get_descr(result));
			goto close_connection;
		}

		connection_data.ptr = (void *)&resources;
		result = doca_rdma_connect_to_addr(resources.rdma, resources.cm_addr, connection_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Client failed to call doca_rdma_connect_to_addr %s",
				     doca_error_get_descr(result));
			goto close_connection;
		}

		DOCA_LOG_INFO("Client is waiting for a connection establishment");
		/* Wait for a new connection */
		while ((!resources.connection_established) && (!resources.connection_error)) {
			if (doca_pe_progress(resources.pe) == 0)
				nanosleep(&ts, &ts);
		}

		if (resources.connection_error) {
			DOCA_LOG_ERR("Failed to connect to remote peer, connection error");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		DOCA_LOG_INFO("Client - Connection 1 is established");
	}

	result = create_memory_local_remote_client(oob_sock_fd, &resources, 0, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_memory_local_remote_client failed: %s", doca_error_get_descr(result));
		goto close_connection;
	}

	result = doca_gpu_mem_alloc(resources.gpudev,
				    sizeof(uint32_t),
				    4096,
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&gpu_exit_flag,
				    (void **)&cpu_exit_flag);
	if (result != DOCA_SUCCESS || gpu_exit_flag == NULL || cpu_exit_flag == NULL) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc returned %s", doca_error_get_descr(result));
		goto close_connection;
	}
	cpu_exit_flag[0] = 0;

	cuda_ret = cudaStreamCreateWithFlags(&cstream, cudaStreamNonBlocking);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", cuda_ret);
		result = DOCA_ERROR_DRIVER;
		goto close_connection;
	}

	/* First client kernel on default CUDA stream */
	result = kernel_write_client(0,
				     resources.gpu_rdma,
				     client_local_buf_arr_B[0].gpu_buf_arr,
				     client_local_buf_arr_C[0].gpu_buf_arr,
				     client_local_buf_arr_F[0].gpu_buf_arr,
				     client_remote_buf_arr_A[0].gpu_buf_arr,
				     0,
				     gpu_exit_flag);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(result));
		goto close_connection;
	}

	if (cfg->use_rdma_cm) {
		DOCA_LOG_INFO("Establishing connection 2..");

		/* Establish a new connection while the CUDA kernel working on first connection is still running */
		result = doca_rdma_connect_to_addr(resources.rdma, resources.cm_addr, connection_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Client failed to call doca_rdma_connect_to_addr %s",
				     doca_error_get_descr(result));
			goto close_connection;
		}

		DOCA_LOG_INFO("Client is waiting for a connection establishment");
		/* Wait for a new connection */
		while ((!resources.connection2_established) && (!resources.connection2_error)) {
			if (doca_pe_progress(resources.pe) == 0)
				nanosleep(&ts, &ts);
		}

		if (resources.connection2_error) {
			DOCA_LOG_ERR("Failed to connect to remote peer, connection error");
			result = DOCA_ERROR_CONNECTION_ABORTED;
			goto close_connection;
		}

		DOCA_LOG_INFO("Client - Connection 2 is established");

		result = create_memory_local_remote_client(oob_sock_fd, &resources, 1, cstream);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function create_memory_local_remote_client failed: %s",
				     doca_error_get_descr(result));
			goto close_connection;
		}

		DOCA_LOG_INFO("Client - Connection 2 memory info exchanged");

		/* Second client kernel on non-default CUDA stream */
		result = kernel_write_client(cstream,
					     resources.gpu_rdma,
					     client_local_buf_arr_B[1].gpu_buf_arr,
					     client_local_buf_arr_C[1].gpu_buf_arr,
					     client_local_buf_arr_F[1].gpu_buf_arr,
					     client_remote_buf_arr_A[1].gpu_buf_arr,
					     1,
					     gpu_exit_flag);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function kernel_write_client failed: %s", doca_error_get_descr(result));
			goto close_connection;
		}
	}

	DOCA_LOG_INFO("Client, terminate kernels");
	DOCA_GPUNETIO_VOLATILE(*cpu_exit_flag) = 1;
	cudaStreamSynchronize(0);

	if (cfg->use_rdma_cm) {
		cudaStreamSynchronize(cstream);
		cudaStreamDestroy(cstream);
	}

	oob_connection_client_close(oob_sock_fd);

	destroy_memory_local_remote_client(&resources);

	result = destroy_rdma_resources(&resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;

close_connection:
	oob_connection_client_close(oob_sock_fd);

destroy_resources:

	destroy_memory_local_remote_client(&resources);

	temp_result = destroy_rdma_resources(&resources);
	if (temp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA resources: %s", doca_error_get_descr(temp_result));

	if (remote_conn_details)
		free(remote_conn_details);

	return result;
}
