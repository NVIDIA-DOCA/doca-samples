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
#include <doca_gpunetio_dev_eth_rxq.cuh>
#include <doca_log.h>
#include "gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME::KERNEL);

__global__ void receive_packets(struct doca_gpu_eth_rxq *eth_rxq_gpu, uint32_t *exit_cond)
{
	doca_error_t ret;
	struct doca_gpu_buf *buf_ptr = NULL;
	uintptr_t buf_addr;
	uint64_t buf_idx;

	__shared__ uint32_t rx_pkt_num;
	__shared__ uint64_t rx_buf_idx;

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		ret = doca_gpu_dev_eth_rxq_receive_block(eth_rxq_gpu,
							 MAX_RX_NUM_PKTS,
							 MAX_RX_TIMEOUT_NS,
							 &rx_pkt_num,
							 &rx_buf_idx);
		/* If any thread returns receive error, the whole execution stops */
		if (ret != DOCA_SUCCESS) {
			if (threadIdx.x == 0) {
				/*
				 * printf in CUDA kernel may be a good idea only to report critical errors or debugging.
				 * If application prints this message on the console, something bad happened and
				 * applications needs to exit
				 */
				printf("Receive UDP kernel error %d rxpkts %d error %d\n", ret, rx_pkt_num, ret);
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
			}
			break;
		}

		if (rx_pkt_num == 0)
			continue;

		buf_idx = threadIdx.x;
		while (buf_idx < rx_pkt_num) {
			doca_gpu_dev_eth_rxq_get_buf(eth_rxq_gpu, rx_buf_idx + buf_idx, &buf_ptr);
			doca_gpu_dev_buf_get_addr(buf_ptr, &buf_addr);

			printf("Thread %d received UDP packet with Eth src %02x:%02x:%02x:%02x:%02x:%02x - Eth dst %02x:%02x:%02x:%02x:%02x:%02x\n",
			       threadIdx.x,
			       ((uint8_t *)buf_addr)[0],
			       ((uint8_t *)buf_addr)[1],
			       ((uint8_t *)buf_addr)[2],
			       ((uint8_t *)buf_addr)[3],
			       ((uint8_t *)buf_addr)[4],
			       ((uint8_t *)buf_addr)[5],
			       ((uint8_t *)buf_addr)[6],
			       ((uint8_t *)buf_addr)[7],
			       ((uint8_t *)buf_addr)[8],
			       ((uint8_t *)buf_addr)[9],
			       ((uint8_t *)buf_addr)[10],
			       ((uint8_t *)buf_addr)[11]);

			/* Add packet processing function here. */

			buf_idx += blockDim.x;
		}
		__syncthreads();
	}
}

extern "C" {

doca_error_t kernel_receive_packets(cudaStream_t stream, struct rxq_queue *rxq, uint32_t *gpu_exit_condition)
{
	cudaError_t result = cudaSuccess;

	if (rxq == NULL || gpu_exit_condition == NULL) {
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
	receive_packets<<<1, CUDA_BLOCK_THREADS, 0, stream>>>(rxq->eth_rxq_gpu, gpu_exit_condition);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
