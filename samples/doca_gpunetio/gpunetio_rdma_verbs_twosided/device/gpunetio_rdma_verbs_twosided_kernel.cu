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
#include <doca_gpunetio_dev_rdma_verbs_twosided.cuh>

#include "rdma_verbs_common.h"

DOCA_LOG_REGISTER(GPU_VERBS_SAMPLE::CUDA_KERNEL);

#define KERNEL_DEBUG_TIMES 1

#if KERNEL_DEBUG_TIMES == 1
#define DOCA_GPUNETIO_DEVICE_GET_TIME(globaltimer) asm volatile("mov.u64 %0, %globaltimer;" : "=l"(globaltimer))
#endif

__global__ void client(struct doca_gpu_dev_rdma_verbs_qp *qp,
		       uint32_t num_iters,
		       uint32_t data_size,
		       uint8_t *src_buf,
		       uint32_t src_buf_mkey,
		       uint64_t *src_flag,
		       uint32_t src_flag_mkey,
		       uint8_t *dst_buf,
		       uint32_t dst_buf_mkey,
		       uint64_t *dst_flag,
		       uint32_t dst_flag_mkey)
{
	uint32_t wqe_idx = 0, cqe_idx = qp->cq_sq->cqe_ci;
	struct doca_gpu_dev_rdma_verbs_wqe *wqe_ptr;

	wqe_idx = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(qp->sq_wqe_pi, threadIdx.x);
	__syncthreads();

	for (int iter_idx = 0; iter_idx < num_iters; iter_idx++) {
		wqe_ptr = doca_gpu_dev_rdma_verbs_get_wqe_ptr(qp, wqe_idx);

		printf("Thread %d wqe_idx %d waiting for server addr %p iter %d/%d msg size %d\n",
				threadIdx.x, wqe_idx, (void *)src_flag, iter_idx, num_iters, data_size);

		while (DOCA_GPUNETIO_VOLATILE(src_flag[threadIdx.x]) != (uint64_t)(iter_idx + 1))
			;
		printf("Thread %d received ack %ld/%d posting send %lx val %x, %d bytes %x mkey qpn %x\n",
		       threadIdx.x,
		       DOCA_GPUNETIO_VOLATILE(src_flag[threadIdx.x]), (iter_idx + 1),
		       (uint64_t)(src_buf + (data_size * threadIdx.x)),
		       *(src_buf + (data_size * threadIdx.x)),
		       data_size,
		       src_buf_mkey,
		       qp->sq_num);

		doca_gpu_dev_rdma_verbs_wqe_prepare_send(qp,
							 wqe_ptr,
							 wqe_idx,
							 MLX5_OPCODE_SEND,
							 DOCA_GPUNETIO_MLX5_WQE_CTRL_CQ_UPDATE,
							 0, // immediate
							 (uint64_t)(src_buf + (data_size * threadIdx.x)),
							 src_buf_mkey,
							 data_size);

		// doca_gpu_dev_rdma_verbs_wqe_post<DOCA_GPUNETIO_WQE_RMEM>(qp, wqe, wqe_idx, 3);
		__syncthreads();

		if (threadIdx.x == 0) {
			doca_gpu_dev_rdma_verbs_submit_inc<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
							   DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM,
							   DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp, blockDim.x);

			if (doca_gpu_dev_rdma_verbs_poll_cq_at<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU,
							       DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp->cq_sq, cqe_idx) !=
			    0) {
				printf("Error CQE!\n");
			}
			cqe_idx = doca_gpu_dev_rdma_verbs_cqe_idx_inc_mask(cqe_idx, 1);
		}
		wqe_idx = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(wqe_idx, blockDim.x);
		__syncthreads();

		// doca_gpu_dev_rdma_verbs_wqe_post_atomic(MLX5_OPCODE_ATOMIC_CS)
	}
}

__global__ void server(struct doca_gpu_dev_rdma_verbs_qp *qp,
		       uint32_t num_iters,
		       uint32_t data_size,
		       uint8_t *src_buf,
		       uint32_t src_buf_mkey,
		       uint64_t *src_flag,
		       uint32_t src_flag_mkey,
		       uint8_t *dst_buf,
		       uint32_t dst_buf_mkey,
		       uint64_t *dst_flag,
		       uint32_t dst_flag_mkey)
{
	uint32_t wqe_idx = 0;
	// WQE and CQE are prepared by app so it can decide the memory to use (registers or shared)
	struct doca_gpu_dev_rdma_verbs_addr laddr;
	doca_gpu_dev_rdma_verbs_ticket_t out_ticket;
	struct doca_gpu_dev_rdma_verbs_wqe *wqe_ptr;
	uint64_t val[1];
	wqe_idx = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(qp->sq_wqe_pi, threadIdx.x);
	// wqe_idx_rq = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(qp->rq_wqe_pi, threadIdx.x);
	__syncthreads();

	for (int iter_idx = 0; iter_idx < num_iters; iter_idx++) {
		laddr.addr = (uint64_t)(src_buf + (data_size * threadIdx.x));
		laddr.key = src_buf_mkey;

		doca_gpu_dev_rdma_verbs_recv<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU,
					     DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM,
					     DOCA_GPUNETIO_RDMA_VERBS_EXEC_SCOPE_THREAD>(qp,
											 laddr,
											 data_size,
											 &out_ticket);
		// rwqe = doca_gpu_dev_rdma_verbs_get_rwqe_ptr(qp, wqe_idx);

		// doca_gpu_dev_rdma_verbs_wqe_prepare_recv(qp, rwqe, (uint64_t)(src_buf + (data_size * threadIdx.x)),
		// src_buf_mkey, data_size);
		// doca_gpu_dev_rdma_verbs_submit<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
		// 								nic_handler,
		// 								DOCA_GPUNETIO_RDMA_VERBS_QP_RQ>(qp,
		// blockDim.x);

		printf("Thread %d posted recv qpn %x ticket %lx\n", threadIdx.x, qp->rq_num, out_ticket);

		// Write inline
		val[0] = iter_idx + 1;
		wqe_ptr = doca_gpu_dev_rdma_verbs_get_wqe_ptr(qp, wqe_idx);
		doca_gpu_dev_rdma_verbs_wqe_prepare_write_inl(qp,
							      wqe_ptr,
							      wqe_idx,
							      DOCA_GPUNETIO_MLX5_WQE_CTRL_CQ_ERROR_UPDATE,
							      (uint64_t)(&dst_flag[threadIdx.x]),
							      dst_flag_mkey,
							      (uint64_t)(val),
							      sizeof(uint64_t));

		printf("Thread %d posted write inline qpn %x wqe %d\n", threadIdx.x, qp->sq_num, wqe_idx);

		__syncthreads();

		if (threadIdx.x == 0) {
			doca_gpu_dev_rdma_verbs_submit_inc<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_EXCLUSIVE,
							   DOCA_GPUNETIO_RDMA_VERBS_NIC_HANDLER_GPU_SM,
							   DOCA_GPUNETIO_RDMA_VERBS_QP_SQ>(qp, blockDim.x);
		}

		if (doca_gpu_dev_rdma_verbs_poll_cq_at<DOCA_GPUNETIO_RDMA_VERBS_RESOURCE_SHARING_MODE_GPU,
						       DOCA_GPUNETIO_RDMA_VERBS_QP_RQ>(qp->cq_rq, out_ticket) != 0) {
			printf("Error CQE!\n");
		}

		printf("Thread %d recv done\n", threadIdx.x);

		wqe_idx = doca_gpu_dev_rdma_verbs_wqe_idx_inc_mask(wqe_idx, blockDim.x);
		__syncthreads();
	}
	// doca_gpu_dev_rdma_verbs_wqe_post_atomic(MLX5_OPCODE_ATOMIC_CS)
}

extern "C" {

doca_error_t gpunetio_rdma_verbs_client_server(cudaStream_t stream,
					       struct doca_gpu_dev_rdma_verbs_qp *qp,
					       uint32_t num_iters,
					       uint32_t cuda_threads,
					       uint32_t data_size,
					       uint8_t *src_buf,
					       uint32_t src_buf_mkey,
					       uint64_t *src_flag,
					       uint32_t src_flag_mkey,
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
		client<<<1, cuda_threads, 0, stream>>>(qp,
						       num_iters,
						       data_size,
						       src_buf,
						       src_buf_mkey,
						       src_flag,
						       src_flag_mkey,
						       dst_buf,
						       dst_buf_mkey,
						       dst_flag,
						       dst_flag_mkey);
	else
		server<<<1, cuda_threads, 0, stream>>>(qp,
						       num_iters,
						       data_size,
						       src_buf,
						       src_buf_mkey,
						       src_flag,
						       src_flag_mkey,
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
