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
#include <doca_gpunetio_dev_rdma_verbs_qp.cuh>
#include <doca_gpunetio_dev_rdma_verbs_cq.cuh>

#include "rdma_verbs_common.h"

DOCA_LOG_REGISTER(GPU_VERBS_SAMPLE::CUDA_KERNEL);

#define KERNEL_DEBUG_TIMES 0

template <bool is_client, enum doca_gpu_dev_rdma_verbs_nic_handler nic_handler>
__global__ void write_lat(struct doca_gpu_dev_rdma_verbs_qp *qp,
			  uint32_t num_iters,
			  uint32_t size,
			  uint8_t *local_poll_buf,
			  uint32_t local_poll_mkey,
			  uint8_t *local_post_buf,
			  uint32_t local_post_mkey,
			  uint8_t *dst_buf,
			  uint32_t dst_mkey)
{
	uint32_t wqe_idx = 0, cqe_idx = qp->cq_sq->cqe_ci;
	uint64_t scnt = 0;
	uint64_t rcnt = 0;
	// WQE and CQE are prepared by app so it can decide the memory to use (registers or shared)
	struct doca_gpu_dev_rdma_verbs_wqe *wqe_ptr;
	enum doca_gpu_dev_rdma_verbs_wqe_ctrl_flags cflag = DOCA_GPUNETIO_MLX5_WQE_CTRL_CQ_ERROR_UPDATE;

#if KERNEL_DEBUG_TIMES == 1
	unsigned long long write_start, write_end, dbrecs_start, dbrecs_end, wait_cqe;
#endif

	if (threadIdx.x == (blockDim.x - 1))
		cflag = DOCA_GPUNETIO_MLX5_WQE_CTRL_CQ_UPDATE;

	wqe_idx = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(qp->sq_wqe_pi, threadIdx.x);
	__syncthreads();

	while (scnt < num_iters || rcnt < num_iters) {
		if (rcnt < num_iters && (scnt >= 1 || is_client == true)) {
			++rcnt;
			while (DOCA_GPUNETIO_VOLATILE(local_poll_buf[size * threadIdx.x]) != (uint8_t)rcnt)
				;
		}
		__threadfence_block();

		if (scnt < num_iters) {
			++scnt;
			DOCA_GPUNETIO_VOLATILE(local_post_buf[size * threadIdx.x]) = (uint8_t)scnt;

#if KERNEL_DEBUG_TIMES == 1
			write_start = doca_gpu_dev_rdma_verbs_query_globaltimer();
#endif

			wqe_ptr = doca_gpu_dev_rdma_verbs_get_wqe_ptr(qp, wqe_idx);
			doca_gpu_dev_rdma_verbs_wqe_prepare_write(qp,
								  wqe_ptr,
								  wqe_idx,
								  MLX5_OPCODE_RDMA_WRITE,
								  cflag,
								  0, // immediate
								  (uint64_t)(dst_buf + (size * threadIdx.x)),
								  dst_mkey,
								  (uint64_t)(local_post_buf + (size * threadIdx.x)),
								  local_post_mkey,
								  size);

#if KERNEL_DEBUG_TIMES == 1
			write_end = doca_gpu_dev_rdma_verbs_query_globaltimer();
#endif

			__syncthreads();

			if (threadIdx.x == (blockDim.x - 1)) {
#if KERNEL_DEBUG_TIMES == 1
				dbrecs_start = doca_gpu_dev_rdma_verbs_query_globaltimer();
#endif

				doca_gpu_dev_rdma_verbs_submit<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
							       nic_handler,
							       DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(
					qp,
					(wqe_idx + 1) & DOCA_GPUNETIO_RDMA_VERBS_WQE_PI_MASK);
				/* Alternatively:
				 * doca_gpu_dev_rdma_verbs_submit_inc<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
				 *								nic_handler,
				 *								DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp,
				 *blockDim.x);
				 */

#if KERNEL_DEBUG_TIMES == 1
				dbrecs_end = doca_gpu_dev_rdma_verbs_query_globaltimer();
#endif

				/* Wait for final CQE in block of iterations */
				if (doca_gpu_dev_rdma_verbs_poll_cq_at<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU,
								       DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp->cq_sq,
												       cqe_idx) != 0) {
					printf("Error CQE!\n");
				}

#if KERNEL_DEBUG_TIMES == 1
				wait_cqe = doca_gpu_dev_rdma_verbs_query_globaltimer();
				printf("scnt %ld prep write %ld ns, sync all %ld ns, ring dbs %ld ns waiting cqe %ld ns\n",
				       scnt,
				       write_end - write_start,
				       dbrecs_start - write_end,
				       dbrecs_end - dbrecs_start,
				       wait_cqe - dbrecs_start);
#endif

				cqe_idx = doca_gpu_dev_rdma_verbs_cqe_idx_inc_mask(cqe_idx, 1);
			}
			wqe_idx = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(wqe_idx, blockDim.x);
			__syncthreads();
		}
	}
}

extern "C" {

doca_error_t gpunetio_rdma_verbs_write_lat(cudaStream_t stream,
					   struct doca_gpu_dev_rdma_verbs_qp *qp,
					   uint32_t num_iters,
					   uint32_t cuda_blocks,
					   uint32_t cuda_threads,
					   uint32_t size,
					   uint8_t *local_poll_buf,
					   uint32_t local_poll_mkey,
					   uint8_t *local_post_buf,
					   uint32_t local_post_mkey,
					   uint8_t *dst_buf,
					   uint32_t dst_mkey,
					   bool is_cpu_proxy,
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
		if (is_cpu_proxy) {
			write_lat<true, DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_CPU_PROXY>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   num_iters,
									   size,
									   local_poll_buf,
									   local_poll_mkey,
									   local_post_buf,
									   local_post_mkey,
									   dst_buf,
									   dst_mkey);
		} else {
			write_lat<true, DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   num_iters,
									   size,
									   local_poll_buf,
									   local_poll_mkey,
									   local_post_buf,
									   local_post_mkey,
									   dst_buf,
									   dst_mkey);
		}
	} else {
		if (is_cpu_proxy) {
			write_lat<false, DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_CPU_PROXY>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   num_iters,
									   size,
									   local_poll_buf,
									   local_poll_mkey,
									   local_post_buf,
									   local_post_mkey,
									   dst_buf,
									   dst_mkey);
		} else {
			write_lat<false, DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   num_iters,
									   size,
									   local_poll_buf,
									   local_poll_mkey,
									   local_post_buf,
									   local_post_mkey,
									   dst_buf,
									   dst_mkey);
		}
	}

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}
}