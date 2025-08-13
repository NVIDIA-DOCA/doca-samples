/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <doca_gpunetio_dev_eth_txq.cuh>
#include <doca_log.h>
#include "gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SIMPLE_SEND::KERNEL);

__global__ void send_packets(struct doca_gpu_eth_txq *eth_txq_gpu,
			     struct doca_gpu_buf_arr *buf_arr_gpu,
			     const uint32_t pkt_size,
			     const uint32_t inflight_sends,
			     uint32_t *exit_cond)
{
	struct doca_gpu_buf *buf_ptr = NULL;
	uint64_t doca_gpu_buf_idx = threadIdx.x;
	uint32_t position;
	__shared__ uint32_t num_completed;
	uint32_t curr_position, mask_max_position;

	/* Just for simplicity, every thread always send the same buffer */
	doca_gpu_dev_buf_get_buf(buf_arr_gpu, doca_gpu_buf_idx, &buf_ptr);

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		doca_gpu_dev_eth_txq_get_info(eth_txq_gpu, &curr_position, &mask_max_position);
		position = (curr_position + threadIdx.x) & mask_max_position;
		/*
		 * Only last packet has the NOTIFY flag just to ensure the whole burts of packets in the iteration
		 * has been correctly sent.
		 */
		if ((position % (inflight_sends - 1)) == 0) {
			/* Just re-use a variable to avoid wasting registers memory */
			num_completed = 1;
			doca_gpu_dev_eth_txq_send_enqueue_weak(eth_txq_gpu,
							       buf_ptr,
							       pkt_size,
							       position,
							       DOCA_GPU_SEND_FLAG_NOTIFY);
		} else
			doca_gpu_dev_eth_txq_send_enqueue_weak(eth_txq_gpu, buf_ptr, pkt_size, position, 0);
		__syncthreads();

		/* First thread in warp flushes send queue */
		if (threadIdx.x == 0) {
			doca_gpu_dev_eth_txq_commit_weak(eth_txq_gpu, blockDim.x);
			doca_gpu_dev_eth_txq_push(eth_txq_gpu);
			if (num_completed == 1)
				doca_gpu_dev_eth_txq_wait_completion(eth_txq_gpu,
								     1,
								     DOCA_GPU_ETH_TXQ_WAIT_FLAG_B,
								     &num_completed);
			num_completed = 0;
		}
		__syncthreads();
	}
}

extern "C" {

doca_error_t kernel_send_packets(cudaStream_t stream, struct txq_queue *txq, uint32_t *gpu_exit_condition)
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

	/* For simplicity launch 1 CUDA block with 32 CUDA threads */
	send_packets<<<1, txq->cuda_threads, 0, stream>>>(txq->eth_txq_gpu,
							  txq->buf_arr_gpu,
							  txq->pkt_size,
							  txq->inflight_sends,
							  gpu_exit_condition);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
