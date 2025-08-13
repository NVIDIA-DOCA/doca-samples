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

#include "../gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME::KERNEL);

__global__ void send_wait_on_time(struct doca_gpu_eth_txq *eth_txq_gpu,
				  struct doca_gpu_buf_arr *buf_arr_gpu,
				  uint64_t *intervals_gpu)
{
	doca_error_t result;
	struct doca_gpu_buf *buf_ptr = NULL;
	uint32_t lane_id = threadIdx.x % WARP_SIZE;
	uint32_t warp_id = threadIdx.x / WARP_SIZE;
	uint64_t doca_gpu_buf_idx = lane_id;
	__shared__ uint32_t exit_cond[1];

	/* For simplicity, only 1 warp is allowed */
	if (warp_id > 0)
		return;

	if (lane_id == 0)
		DOCA_GPUNETIO_VOLATILE(exit_cond[0]) = 0;

	for (int idx = 0; idx < NUM_BURST_SEND && exit_cond[0] == 0; idx++) {
		/* First thread in warp enqueue time barrier */
		if (lane_id == 0) {
			result = doca_gpu_dev_eth_txq_wait_time_enqueue_strong(eth_txq_gpu, intervals_gpu[idx], 0);
			if (result != DOCA_SUCCESS) {
				printf("Error %d doca gpunetio enqueue wait on time thread %d\n", result, lane_id);
				DOCA_GPUNETIO_VOLATILE(exit_cond[0]) = 1;
			}
		}
		__syncwarp();

		/* All threads in the warp enqueue a different packet (32 packets per burst) */
		doca_gpu_dev_buf_get_buf(buf_arr_gpu, doca_gpu_buf_idx, &buf_ptr);

		doca_gpu_dev_eth_txq_send_enqueue_strong(eth_txq_gpu, buf_ptr, PACKET_SIZE, 0);
		__syncwarp();

		/* First thread in warp flushes send queue */
		if (lane_id == 0) {
			doca_gpu_dev_eth_txq_commit_strong(eth_txq_gpu);
			doca_gpu_dev_eth_txq_push(eth_txq_gpu);
		}
		__syncwarp();

		doca_gpu_buf_idx += WARP_SIZE;
	}
}

extern "C" {

doca_error_t kernel_send_wait_on_time(cudaStream_t stream, struct txq_queue *txq, uint64_t *intervals_gpu)
{
	cudaError_t result = cudaSuccess;

	if (txq == NULL || intervals_gpu == NULL) {
		DOCA_LOG_ERR("kernel_receive_icmp invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	send_wait_on_time<<<1, WARP_SIZE, 0, stream>>>(txq->eth_txq_gpu, txq->txbuf.buf_arr_gpu, intervals_gpu);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
