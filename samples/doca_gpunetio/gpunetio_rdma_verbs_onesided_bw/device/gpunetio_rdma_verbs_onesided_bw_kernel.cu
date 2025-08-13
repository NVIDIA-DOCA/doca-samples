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
#include <doca_gpunetio_dev_rdma_verbs_onesided.cuh>

#include "rdma_verbs_common.h"

DOCA_LOG_REGISTER(GPU_VERBS_SAMPLE::CUDA_KERNEL);

#define KERNEL_DEBUG_TIMES 0

#if KERNEL_DEBUG_TIMES == 1
#define DOCA_GPUNETIO_DEVICE_GET_TIME(globaltimer) asm volatile("mov.u64 %0, %globaltimer;" : "=l"(globaltimer))
#endif

template <enum doca_gpu_dev_rdma_verbs_nic_handler nic_handler, enum doca_gpu_dev_rdma_verbs_exec_scope exec_scope>
__global__ void ops_bw(struct doca_gpu_dev_rdma_verbs_qp *qp,
		       uint32_t num_iters,
		       uint32_t size,
		       uint8_t *src_buf,
		       uint32_t src_mkey,
		       uint8_t *dst_buf,
		       uint32_t dst_mkey,
		       uint64_t *src_flag,
		       uint32_t src_flag_mkey,
		       uint64_t *dst_flag,
		       uint32_t dst_flag_mkey)
{
	struct doca_gpu_dev_rdma_verbs_addr laddr;
	struct doca_gpu_dev_rdma_verbs_addr raddr;
	doca_gpu_dev_rdma_verbs_ticket_t out_ticket;
	uint32_t lane_idx = doca_gpu_dev_rdma_verbs_get_lane_id();

#if KERNEL_DEBUG_TIMES == 1
	unsigned long long step1 = 0, step2 = 0, step3 = 0;
#endif

	// if (threadIdx.x == 0)
	// 	printf("src_flag %lx dst_flag %lx\n", src_flag, dst_flag);

	for (uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x; idx < num_iters; idx += (blockDim.x * gridDim.x)) {
#if KERNEL_DEBUG_TIMES == 1
		DOCA_GPUNETIO_DEVICE_GET_TIME(step1);
#endif

		laddr.addr = (uint64_t)(src_buf + (size * blockIdx.x * blockDim.x + size * threadIdx.x));
		laddr.key = src_mkey;
		raddr.addr = (uint64_t)(dst_buf + (size * blockIdx.x * blockDim.x + size * threadIdx.x));
		raddr.key = dst_mkey;

		doca_gpu_dev_rdma_verbs_put<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU, nic_handler, exec_scope>(
			qp,
			raddr,
			laddr,
			size,
			&out_ticket);

#if KERNEL_DEBUG_TIMES == 1
		DOCA_GPUNETIO_DEVICE_GET_TIME(step2);
#endif

		if (exec_scope == DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_THREAD) {
			if (doca_gpu_dev_rdma_verbs_poll_cq_at<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU,
							       DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp->cq_sq, out_ticket) !=
			    0) {
				printf("Error CQE!\n");
			}

			// while ( doca_gpu_dev_rdma_verbs_cq_poll_cqe(cq_sq, last_wqe_id, &cqe) == 0);
			// doca_gpu_dev_verbs_poll_cq(cq_sq, 1);
		}

		if (exec_scope == DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_WARP) {
			if (lane_idx == 0) {
				if (doca_gpu_dev_rdma_verbs_poll_cq_at<
					    DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU,
					    DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp->cq_sq, out_ticket) != 0) {
					printf("Error CQE!\n");
				}
			}
			// while ( doca_gpu_dev_rdma_verbs_cq_poll_cqe(cq_sq, last_wqe_id, &cqe) == 0);
		}
		__syncthreads();
		// printf("Block %d thread %d passed sync\n", blockIdx.x, threadIdx.x);

#if KERNEL_DEBUG_TIMES == 1
		if (threadIdx.x == 0)
			printf("iteration %d src_buf %lx size %d dst_buf %lx put %ld ns, poll %ld ns\n",
			       idx,
			       src_buf,
			       size,
			       dst_buf,
			       step2 - step1,
			       step3 - step2);
#endif
	}
}

extern "C" {

doca_error_t gpunetio_rdma_verbs_ops_bw(cudaStream_t stream,
					struct doca_gpu_dev_rdma_verbs_qp *qp,
					uint32_t cuda_threads_iters,
					uint32_t cuda_blocks,
					uint32_t cuda_threads,
					uint32_t size,
					uint8_t *src_buf,
					uint32_t src_mkey,
					uint8_t *dst_buf,
					uint32_t dst_mkey,
					uint64_t *src_flag,
					uint32_t src_flag_mkey,
					uint64_t *dst_flag,
					uint32_t dst_flag_mkey,
					bool is_cpu_proxy,
					enum doca_gpu_dev_rdma_verbs_exec_scope exec_scope)
{
	cudaError_t result = cudaSuccess;

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (is_cpu_proxy) {
		if (exec_scope == DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_THREAD)
			ops_bw<DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_CPU_PROXY,
			       DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_THREAD>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   cuda_threads_iters,
									   size,
									   src_buf,
									   src_mkey,
									   dst_buf,
									   dst_mkey,
									   src_flag,
									   src_flag_mkey,
									   dst_flag,
									   dst_flag_mkey);
		if (exec_scope == DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_WARP)
			ops_bw<DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_CPU_PROXY, DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_WARP>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   cuda_threads_iters,
									   size,
									   src_buf,
									   src_mkey,
									   dst_buf,
									   dst_mkey,
									   src_flag,
									   src_flag_mkey,
									   dst_flag,
									   dst_flag_mkey);
	} else {
		if (exec_scope == DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_THREAD)
			ops_bw<DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM, DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_THREAD>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   cuda_threads_iters,
									   size,
									   src_buf,
									   src_mkey,
									   dst_buf,
									   dst_mkey,
									   src_flag,
									   src_flag_mkey,
									   dst_flag,
									   dst_flag_mkey);
		if (exec_scope == DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_WARP)
			ops_bw<DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM, DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_WARP>
				<<<cuda_blocks, cuda_threads, 0, stream>>>(qp,
									   cuda_threads_iters,
									   size,
									   src_buf,
									   src_mkey,
									   dst_buf,
									   dst_mkey,
									   src_flag,
									   src_flag_mkey,
									   dst_flag,
									   dst_flag_mkey);
	}

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}
}
