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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <doca_log.h>
#include <doca_gpunetio.h>
#include <doca_gpunetio_dev_verbs_counter.cuh>

#include "verbs_common.h"

DOCA_LOG_REGISTER(GPU_VERBS_SAMPLE::CUDA_KERNEL);

template <bool is_client>
__global__ void put_counter_kernel(struct doca_gpu_dev_verbs_qp *qp_main,
				   struct doca_gpu_dev_verbs_qp *qp_companion,
				   uint32_t start_iters,
				   uint32_t num_iters,
				   uint32_t data_size,
				   uint8_t *src_buf,
				   uint32_t src_buf_mkey,
				   uint64_t *src_flag,
				   uint32_t src_flag_mkey,
				   uint64_t *prev_flag_buf,
				   uint32_t prev_flag_buf_mkey,
				   uint8_t *dst_buf,
				   uint32_t dst_buf_mkey,
				   uint64_t *dst_flag,
				   uint32_t dst_flag_mkey)
{
	uint32_t tidx = threadIdx.x + (blockIdx.x * blockDim.x);

	for (int iter_idx = 0; iter_idx < num_iters; iter_idx++) {
		if (is_client) {
			doca_gpu_dev_verbs_put_counter<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
						       DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO>(
				qp_main,
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(dst_buf + (data_size * tidx)),
							.key = (uint32_t)dst_buf_mkey},
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(src_buf + (data_size * tidx)),
							.key = (uint32_t)src_buf_mkey},
				(size_t)(data_size),
				qp_companion,
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(&dst_flag[threadIdx.x]),
							.key = (uint32_t)dst_flag_mkey},
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(&prev_flag_buf[threadIdx.x]),
							.key = (uint32_t)prev_flag_buf_mkey},
				(uint64_t)1);

			while (DOCA_GPUNETIO_VOLATILE(src_flag[threadIdx.x]) != (uint64_t)(start_iters + iter_idx + 1))
				continue;

			doca_gpu_dev_verbs_fence_acquire<DOCA_GPUNETIO_VERBS_SYNC_SCOPE_SYS>();
		} else {
			while (DOCA_GPUNETIO_VOLATILE(src_flag[threadIdx.x]) != (uint64_t)(start_iters + iter_idx + 1))
				continue;

			doca_gpu_dev_verbs_fence_acquire<DOCA_GPUNETIO_VERBS_SYNC_SCOPE_SYS>();

			doca_gpu_dev_verbs_put_counter<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
						       DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO>(
				qp_main,
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(dst_buf + (data_size * tidx)),
							.key = (uint32_t)dst_buf_mkey},
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(src_buf + (data_size * tidx)),
							.key = (uint32_t)src_buf_mkey},
				(size_t)(data_size),
				qp_companion,
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(&dst_flag[threadIdx.x]),
							.key = (uint32_t)dst_flag_mkey},
				doca_gpu_dev_verbs_addr{.addr = (uint64_t)(&prev_flag_buf[threadIdx.x]),
							.key = (uint32_t)prev_flag_buf_mkey},
				(uint64_t)1);
		}
	}

	// Poll CQE for latest WQE to ensure everything is ok.
	doca_gpu_dev_verbs_wait<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
				DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO>(qp_main);
}

extern "C" {

doca_error_t gpunetio_verbs_put_counter_lat(cudaStream_t stream,
					    struct doca_gpu_dev_verbs_qp *qp_main,
					    struct doca_gpu_dev_verbs_qp *qp_companion,
					    uint32_t start_iters,
					    uint32_t num_iters,
					    uint32_t cuda_threads,
					    uint32_t data_size,
					    uint8_t *src_buf,
					    uint32_t src_buf_mkey,
					    uint64_t *src_flag,
					    uint32_t src_flag_mkey,
					    uint64_t *prev_flag_buf,
					    uint32_t prev_flag_buf_mkey,
					    uint8_t *dst_buf,
					    uint32_t dst_buf_mkey,
					    uint64_t *dst_flag,
					    uint32_t dst_flag_mkey,
					    bool is_client)
{
	cudaError_t result = cudaSuccess;

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (is_client)
		put_counter_kernel<true><<<1, cuda_threads, 0, stream>>>(qp_main,
									 qp_companion,
									 start_iters,
									 num_iters,
									 data_size,
									 src_buf,
									 src_buf_mkey,
									 src_flag,
									 src_flag_mkey,
									 prev_flag_buf,
									 prev_flag_buf_mkey,
									 dst_buf,
									 dst_buf_mkey,
									 dst_flag,
									 dst_flag_mkey);
	else
		put_counter_kernel<false><<<1, cuda_threads, 0, stream>>>(qp_main,
									  qp_companion,
									  start_iters,
									  num_iters,
									  data_size,
									  src_buf,
									  src_buf_mkey,
									  src_flag,
									  src_flag_mkey,
									  prev_flag_buf,
									  prev_flag_buf_mkey,
									  dst_buf,
									  dst_buf_mkey,
									  dst_flag,
									  dst_flag_mkey);

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}
}
