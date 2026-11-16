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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <doca_log.h>
#include <doca_gpunetio_dev_verbs_counter.cuh>

#include "verbs_common.h"

DOCA_LOG_REGISTER(GPU_VERBS_PUT_COUNTER::CUDA_KERNEL);

template <enum doca_gpu_dev_verbs_exec_scope scope>
__global__ void put_counter_bw(struct doca_gpu_dev_verbs_qp *qp_main,
			       struct doca_gpu_dev_verbs_qp *qp_companion,
			       uint32_t num_iters,
			       uint64_t expected_last_fetch,
			       uint32_t data_size,
			       uint8_t *src_buf,
			       uint32_t src_buf_mkey,
			       uint64_t *prev_flag_buf,
			       uint32_t prev_flag_buf_mkey,
			       uint8_t *dst_buf,
			       uint32_t dst_buf_mkey,
			       uint64_t *dst_flag,
			       uint32_t dst_flag_mkey)
{
	uint32_t tidx = threadIdx.x + (blockIdx.x * blockDim.x);
	uint64_t final_val;

	for (uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x; idx < num_iters; idx += (blockDim.x * gridDim.x)) {
		doca_gpu_dev_verbs_put_counter<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU,
					       DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO,
					       scope>(
			qp_main,
			doca_gpu_dev_verbs_addr{.addr = (uint64_t)(dst_buf + (data_size * tidx)),
						.key = (uint32_t)dst_buf_mkey},
			doca_gpu_dev_verbs_addr{.addr = (uint64_t)(src_buf + (data_size * tidx)),
						.key = (uint32_t)src_buf_mkey},
			(size_t)(data_size),
			qp_companion,
			doca_gpu_dev_verbs_addr{.addr = (uint64_t)(&dst_flag[tidx]), .key = (uint32_t)dst_flag_mkey},
			doca_gpu_dev_verbs_addr{.addr = (uint64_t)(&prev_flag_buf[tidx]),
						.key = (uint32_t)prev_flag_buf_mkey},
			(uint64_t)1);
	}

	// Check signal atomic fetch add correctness: wait until the last fetch-add of this thread (lane 0 in warp
	// scope) has returned the expected value, the remote flag value right before that last increment. The value
	// must be exact: the buffer still holds the last fetch of the previous launch on the same flags.
	if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD) {
		do {
			final_val =
				doca_gpu_dev_verbs_atomic_read<uint64_t, DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU>(
					&prev_flag_buf[tidx]);
			doca_gpu_dev_verbs_fence_acquire<DOCA_GPUNETIO_VERBS_SYNC_SCOPE_SYS>();
		} while (final_val != expected_last_fetch);
	} else {
		if (doca_gpu_dev_verbs_get_lane_id() == 0) {
			do {
				final_val =
					doca_gpu_dev_verbs_atomic_read<uint64_t,
								       DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU>(
						&prev_flag_buf[tidx]);
				doca_gpu_dev_verbs_fence_acquire<DOCA_GPUNETIO_VERBS_SYNC_SCOPE_SYS>();
			} while (final_val != expected_last_fetch);
		}
		__syncwarp();
	}

	// Poll CQE to ensure writes and counter updates are ok: the counter fetch-adds are posted on the companion QP,
	// so the kernel (and the measured time) ends only once both QPs have completed
	__syncthreads();
	if (threadIdx.x == 0) {
		doca_gpu_dev_verbs_wait<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU,
					DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO>(qp_main);
		doca_gpu_dev_verbs_wait<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU,
					DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO>(qp_companion);
	}
}

extern "C" {

doca_error_t gpunetio_verbs_put_counter_bw(cudaStream_t stream,
					   struct doca_gpu_dev_verbs_qp *qp_main,
					   struct doca_gpu_dev_verbs_qp *qp_companion,
					   uint32_t num_iters,
					   uint32_t cuda_blocks,
					   uint32_t cuda_threads,
					   uint32_t data_size,
					   uint32_t flag_start,
					   uint8_t *src_buf,
					   uint32_t src_buf_mkey,
					   uint64_t *prev_flag_buf,
					   uint32_t prev_flag_buf_mkey,
					   uint8_t *dst_buf,
					   uint32_t dst_buf_mkey,
					   uint64_t *dst_flag,
					   uint32_t dst_flag_mkey,
					   enum doca_gpu_dev_verbs_exec_scope scope)
{
	cudaError_t result = cudaSuccess;
	/* Each thread (warp in warp scope) increments its remote flag once per iteration */
	const uint64_t expected_last_fetch = (uint64_t)flag_start + (num_iters / (cuda_blocks * cuda_threads)) - 1;

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD)
		put_counter_bw<DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD>
			<<<cuda_blocks, cuda_threads, 0, stream>>>(qp_main,
								   qp_companion,
								   num_iters,
								   expected_last_fetch,
								   data_size,
								   src_buf,
								   src_buf_mkey,
								   prev_flag_buf,
								   prev_flag_buf_mkey,
								   dst_buf,
								   dst_buf_mkey,
								   dst_flag,
								   dst_flag_mkey);
	else if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP)
		put_counter_bw<DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP>
			<<<cuda_blocks, cuda_threads, 0, stream>>>(qp_main,
								   qp_companion,
								   num_iters,
								   expected_last_fetch,
								   data_size,
								   src_buf,
								   src_buf_mkey,
								   prev_flag_buf,
								   prev_flag_buf_mkey,
								   dst_buf,
								   dst_buf_mkey,
								   dst_flag,
								   dst_flag_mkey);
	else {
		DOCA_LOG_ERR("Invalid exec scope %d, must be thread or warp", scope);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}
}
