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
#include <doca_gpunetio_dev_verbs_twosided.cuh>

#include "verbs_common.h"

DOCA_LOG_REGISTER(GPU_VERBS_SAMPLE::CUDA_KERNEL);

template <enum doca_gpu_dev_verbs_exec_scope scope>
__global__ void client(struct doca_gpu_dev_verbs_qp *qp,
			   uint32_t start_iters,
			   uint32_t num_iters,
		       uint32_t data_size,
		       uint8_t *src_buf,
		       uint32_t src_buf_mkey,
		       uint64_t *src_flag,
		       uint32_t src_flag_mkey)
{
	doca_gpu_dev_verbs_ticket_t out_ticket;
	uint32_t tidx = threadIdx.x + (blockIdx.x * blockDim.x);
	uint32_t lane_idx = doca_gpu_dev_verbs_get_lane_id();

	for (uint32_t iter_idx = blockIdx.x * blockDim.x + threadIdx.x; iter_idx < num_iters; iter_idx += (blockDim.x * gridDim.x)) {
		// Wait for RDMA Write from server, only 1 thread per block
		if (threadIdx.x == 0) {
			while (DOCA_GPUNETIO_VOLATILE(src_flag[tidx]) != (uint64_t)(start_iters + iter_idx + 1))
					continue;
		}
		__syncthreads();

		doca_gpu_dev_verbs_fence_acquire<DOCA_GPUNETIO_VERBS_SYNC_SCOPE_SYS>();

		// All threads post their RDMA Send
		doca_gpu_dev_verbs_send<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU,
					DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO,
					scope>(
			qp,
			doca_gpu_dev_verbs_addr{.addr = (uint64_t)(src_buf + tidx),
									.key = src_buf_mkey},
			data_size,
			&out_ticket);
	}
	__syncthreads();

	// Poll CQE for latest Send WQE per-block to ensure everything is ok
	if (threadIdx.x == 0)
		doca_gpu_dev_verbs_wait(qp);
}

template <enum doca_gpu_dev_verbs_exec_scope scope>
__global__ void server(struct doca_gpu_dev_verbs_qp *qp,
			   uint32_t start_iters,
			   uint32_t num_iters,
		       uint32_t data_size,
		       uint8_t *src_buf,
		       uint32_t src_buf_mkey,
			   uint64_t *src_flag,
			   uint32_t src_flag_mkey,
		       uint64_t *dst_flag,
		       uint32_t dst_flag_mkey,
		       uint64_t *dump_flag,
		       uint32_t dump_flag_mkey)
{
	doca_gpu_dev_verbs_ticket_t out_ticket;
	uint32_t lane_idx = doca_gpu_dev_verbs_get_lane_id();
	uint32_t tidx = threadIdx.x + (blockIdx.x * blockDim.x);
	uint64_t wqe_idx;
	struct doca_gpu_dev_verbs_wqe *wqe_ptr;

	for (uint32_t iter_idx = tidx; iter_idx < num_iters; iter_idx += (blockDim.x * gridDim.x)) {

		doca_gpu_dev_verbs_recv<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU, DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO, scope>(
			qp,
			doca_gpu_dev_verbs_addr{.addr = (uint64_t)(src_buf + (data_size * tidx)), .key = (uint32_t)src_buf_mkey},
			data_size,
			&out_ticket);

		// Wait for all the recv to be posted
		__syncthreads();

		// Post RDMA write to notify Recv has been posted. One write per block from thread 0 is enough
		if (threadIdx.x == 0) {
			src_flag[tidx] = start_iters + iter_idx + 1;
			wqe_idx = doca_gpu_dev_verbs_reserve_wq_slots(qp, 1);
			wqe_ptr = doca_gpu_dev_verbs_get_wqe_ptr(qp, wqe_idx);
			doca_gpu_dev_verbs_wqe_prepare_write(qp,
								wqe_ptr,
								wqe_idx,
								DOCA_GPUNETIO_MLX5_OPCODE_RDMA_WRITE,
								DOCA_GPUNETIO_MLX5_WQE_CTRL_CQ_UPDATE,
								0,
								(uint64_t)(dst_flag + tidx),
								dst_flag_mkey,
								(uint64_t)(src_flag + tidx),
								src_flag_mkey,
								sizeof(uint64_t));
			doca_gpu_dev_verbs_mark_wqes_ready(qp, wqe_idx, wqe_idx);
			doca_gpu_dev_verbs_submit(qp, wqe_idx + 1);

			/*
			 * First thread in block waits for all recv posted in this iteration.
			 * As an alternative, doca_gpu_dev_verbs_recv_wait() can be called with a specific out_ticket.
			 * This way a specific CUDA thread can wait on a specific Recv WQE. 
			 *
			 * pre-Hopper GPU memory regions require to ensure the memory consistency before returning.
			 */
#if __CUDA_ARCH__ < 900
			doca_gpu_dev_verbs_recv_wait<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU, DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO, DOCA_GPUNETIO_VERBS_MCST_ENABLED>(
										qp, doca_gpu_dev_verbs_addr{.addr = (uint64_t)dump_flag, .key = dump_flag_mkey});
#else
			doca_gpu_dev_verbs_recv_wait<DOCA_GPUNETIO_VERBS_RESOURCE_SHARING_MODE_GPU, DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO, DOCA_GPUNETIO_VERBS_MCST_DISABLED>(
										qp, doca_gpu_dev_verbs_addr{.addr = 0, .key = 0});
#endif
		}

		__syncthreads();
	}
}

extern "C" {

doca_error_t gpunetio_verbs_two_sided_bw(cudaStream_t stream,
					 struct doca_gpu_dev_verbs_qp *qp,
					 uint32_t start_iters,
					 uint32_t num_iters,
					 uint32_t cuda_blocks,
					 uint32_t cuda_threads,
					 uint32_t data_size,
					 uint8_t *src_buf,
					 uint32_t src_buf_mkey,
					 uint64_t *src_flag,
					 uint32_t src_flag_mkey,
					 uint64_t *dst_flag,
					 uint32_t dst_flag_mkey,
					 uint64_t *dump_flag,
					 uint32_t dump_flag_mkey,
					 enum doca_gpu_dev_verbs_exec_scope scope,
					 bool is_client)
{
	cudaError_t result = cudaSuccess;

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (is_client) {
		if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD)
			client<DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   start_iters,
									   num_iters,
									   data_size,
									   src_buf,
									   src_buf_mkey,
									   src_flag,
									   src_flag_mkey);
		else if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP)
			client<DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   start_iters,
									   num_iters,
									   data_size,
									   src_buf,
									   src_buf_mkey,
									   src_flag,
									   src_flag_mkey);
	} else {
		if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD)
			server<DOCA_GPUNETIO_VERBS_EXEC_SCOPE_THREAD>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   start_iters,
									   num_iters,
									   data_size,
									   src_buf,
									   src_buf_mkey,
										src_flag,
									   src_flag_mkey,
									   dst_flag,
									   dst_flag_mkey,
									   dump_flag,
									   dump_flag_mkey);
		else if (scope == DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP)
			server<DOCA_GPUNETIO_VERBS_EXEC_SCOPE_WARP>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   start_iters,
									   num_iters,
									   data_size,
									   src_buf,
									   src_buf_mkey,
										src_flag,
									   src_flag_mkey,
									   dst_flag,
									   dst_flag_mkey,
									   dump_flag,
									   dump_flag_mkey);
	}

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} //extern "C"
