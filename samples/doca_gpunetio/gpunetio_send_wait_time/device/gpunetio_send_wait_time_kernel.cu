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

#include "../gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME::KERNEL);

__global__ void send_wait_on_time(struct doca_gpu_eth_txq *txq,
				  uint8_t *pkt_buff_addr,
				  const uint32_t pkt_buff_mkey,
				  const size_t pkt_size,
				  uint64_t *intervals_gpu)
{
	doca_error_t status;
	doca_gpu_dev_eth_ticket_t out_ticket;
	enum doca_gpu_eth_send_flags flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NONE;
	uint64_t addr;
	uint32_t num_completed;
	__shared__ uint32_t exit_cond[1];
	const uint64_t burst_size = pkt_size * blockDim.x;

	if (threadIdx.x == (blockDim.x - 1))
		flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;
	if (threadIdx.x == 0)
		DOCA_GPUNETIO_VOLATILE(*exit_cond) = 0;
	__syncthreads();

	for (int idx = 0; idx < NUM_BURST_SEND && exit_cond[0] == 0; idx++) {
		addr = ((uint64_t)pkt_buff_addr) + (uint64_t)(burst_size * idx) + (uint64_t)(pkt_size * threadIdx.x);
		// Only one block is using this QP, function can use CTA scope
		doca_gpu_dev_eth_txq_wait_send<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_CTA,
					       DOCA_GPUNETIO_ETH_SYNC_SCOPE_CTA,
					       DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
					       DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>(txq,
										   intervals_gpu[idx],
										   addr,
										   pkt_buff_mkey,
										   pkt_size,
										   flags,
										   &out_ticket);

		// __syncthreads already present in wait_send with BLOCK scope
		if (threadIdx.x == 0) {
			status = doca_gpu_dev_eth_txq_poll_completion<DOCA_GPUNETIO_ETH_CQ_POLL_LAST>(
				txq,
				1,
				DOCA_GPUNETIO_ETH_WAIT_FLAG_B,
				&num_completed);
			if (status != DOCA_SUCCESS)
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
		}
		__syncthreads();
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

	/* For simplicity, 1 thread per packet in burst */
	send_wait_on_time<<<1, NUM_PACKETS_X_BURST, 0, stream>>>(txq->eth_txq_gpu,
								 txq->pkt_buff_addr,
								 txq->pkt_buff_mkey,
								 PACKET_SIZE,
								 intervals_gpu);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
