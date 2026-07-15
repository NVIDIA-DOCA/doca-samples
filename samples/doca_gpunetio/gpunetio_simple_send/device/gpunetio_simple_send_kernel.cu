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

#include <doca_gpunetio_dev_eth_txq.cuh>
#include <doca_log.h>
#include "gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SIMPLE_SEND::KERNEL);

template <enum doca_gpu_dev_eth_exec_scope exec_scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>
__global__ void send_packets_shared_qp_enabled(struct doca_gpu_eth_txq *txq,
					       uint8_t *pkt_buff_addr,
					       const uint32_t pkt_buff_mkey,
					       const size_t pkt_size,
					       uint32_t *exit_cond)
{
	uint32_t num_completed, count_cqe;
	doca_error_t status;
	doca_gpu_dev_eth_ticket_t out_ticket;
	enum doca_gpu_eth_send_flags flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NONE;
	/* For simplicity, every thread always send the same buffer */
	uint64_t addr = ((uint64_t)pkt_buff_addr) + (uint64_t)(pkt_size * threadIdx.x);

	/*
	 * To maximize the throughput, the notification flag can be set at MAX_SQ_DESCR_NUM / 2.
	 * In this example, for demonstration purposes, the notification is set by every last thread
	 * relative to the selected scope.
	 */
	if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD) {
		/* In thread scope, every thread requests a CQE. */
		count_cqe = blockDim.x;
		flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;
	} else if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP) {
		/* In warp scope, every last thread in warp requests a CQE. */
		count_cqe = blockDim.x / DOCA_GPUNETIO_ETH_WARP_SIZE;
		if (doca_gpu_dev_eth_get_lane_id() == (DOCA_GPUNETIO_ETH_WARP_SIZE - 1))
			flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;
	} else {
		/* In block scope, only the last thread in block requests a CQE. */
		count_cqe = 1;
		if (threadIdx.x == (blockDim.x - 1))
			flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;
	}

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		doca_gpu_dev_eth_txq_send<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_GPU,
					  DOCA_GPUNETIO_ETH_SYNC_SCOPE_GPU,
					  DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
					  exec_scope>(txq, addr, pkt_buff_mkey, pkt_size, flags, &out_ticket);

		// __syncthreads already present in send with BLOCK scope
		if (exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK)
			__syncthreads();

		if (threadIdx.x == 0) {
			status = doca_gpu_dev_eth_txq_poll_completion<DOCA_GPUNETIO_ETH_CQ_POLL_LAST>(
				txq,
				count_cqe,
				DOCA_GPUNETIO_ETH_WAIT_FLAG_B,
				&num_completed);
			if (status != DOCA_SUCCESS)
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
		}
		__syncthreads();
	}
}

__global__ void send_packets_shared_qp_disabled(struct doca_gpu_eth_txq *txq,
						uint8_t *pkt_buff_addr,
						const uint32_t pkt_buff_mkey,
						const size_t pkt_size,
						uint32_t *exit_cond)
{
	uint64_t wqe_idx = threadIdx.x, cqe_idx = 0;
	doca_error_t status;
	enum doca_gpu_eth_send_flags flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NONE;
	struct doca_gpu_dev_eth_txq_wqe *wqe_ptr;
	/* For simplicity, every thread always send the same buffer */
	uint64_t addr = ((uint64_t)pkt_buff_addr) + (uint64_t)(pkt_size * threadIdx.x);

	/*
	 * To maximize the throughput, the notification flag can be set at MAX_SQ_DESCR_NUM / 2.
	 * In this example, for demonstration purposes, the notification is set by the last thread
	 * in the block.
	 */
	if (threadIdx.x == (blockDim.x - 1))
		flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		wqe_ptr = doca_gpu_dev_eth_txq_get_wqe_ptr(txq, wqe_idx);
		doca_gpu_dev_eth_txq_wqe_prepare_send(txq, wqe_ptr, wqe_idx, addr, pkt_buff_mkey, pkt_size, flags);
		__syncthreads();

		if (threadIdx.x == (blockDim.x - 1)) {
			/*
			 * Generic fucntion to update the dbrec and ring db checking if UAR is on GPU or CPU.
			 * If these checks are not needed, a lower level combination of API can be used to reduce latency/
			 * As an example, to ring GPU DB:
			 * doca_priv_gpu_dev_eth_txq_update_dbr(txq, wqe_idx + 1);
			 * doca_gpu_dev_eth_txq_ring_db<sync_scope>(txq, wqe_idx + 1);
			 */
			doca_gpu_dev_eth_txq_submit(txq, wqe_idx + 1);
			status = doca_gpu_dev_eth_txq_poll_completion_at<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_GPU,
									 DOCA_GPUNETIO_ETH_SYNC_SCOPE_CTA>(
				txq,
				cqe_idx,
				DOCA_GPUNETIO_ETH_WAIT_FLAG_B);
			if (status != DOCA_SUCCESS)
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
			cqe_idx++;
		}
		__syncthreads();

		wqe_idx += blockDim.x;
	}
}

extern "C" {

doca_error_t kernel_send_packets(cudaStream_t stream,
				 struct txq_queue *txq,
				 uint32_t *gpu_exit_condition,
				 bool shared_qp,
				 enum doca_gpu_dev_eth_exec_scope exec_scope)
{
	cudaError_t result = cudaSuccess;

	if (txq == NULL || gpu_exit_condition == NULL) {
		DOCA_LOG_ERR("kernel_receive_icmp invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (shared_qp) {
		if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD)
			send_packets_shared_qp_enabled<DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD>
				<<<SHARED_QP_BLOCKS, txq->cuda_threads / SHARED_QP_BLOCKS, 0, stream>>>(
					txq->eth_txq_gpu,
					txq->pkt_buff_addr,
					txq->pkt_buff_mkey,
					txq->pkt_size,
					gpu_exit_condition);
		if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP)
			send_packets_shared_qp_enabled<DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP>
				<<<SHARED_QP_BLOCKS, txq->cuda_threads / SHARED_QP_BLOCKS, 0, stream>>>(
					txq->eth_txq_gpu,
					txq->pkt_buff_addr,
					txq->pkt_buff_mkey,
					txq->pkt_size,
					gpu_exit_condition);
		if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK)
			send_packets_shared_qp_enabled<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>
				<<<SHARED_QP_BLOCKS, txq->cuda_threads / SHARED_QP_BLOCKS, 0, stream>>>(
					txq->eth_txq_gpu,
					txq->pkt_buff_addr,
					txq->pkt_buff_mkey,
					txq->pkt_size,
					gpu_exit_condition);
	} else {
		send_packets_shared_qp_disabled<<<1, txq->cuda_threads, 0, stream>>>(txq->eth_txq_gpu,
										     txq->pkt_buff_addr,
										     txq->pkt_buff_mkey,
										     txq->pkt_size,
										     gpu_exit_condition);
	}

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
