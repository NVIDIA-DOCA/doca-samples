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

#include <doca_gpunetio_dev_eth_rxq.cuh>
#include <doca_log.h>
#include "gpunetio_common.h"

#define DOCA_GPUNETIO_SIMPLE_RECEIVE_DEBUG 0

DOCA_LOG_REGISTER(GPUNETIO_SIMPLE_RECEIVE::KERNEL);

template <enum doca_gpu_dev_eth_exec_scope exec_scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>
__global__ void receive_packets(struct doca_gpu_eth_rxq *rxq, uint32_t *exit_cond, uint64_t *tot_pkts)
{
	doca_error_t ret;
	__shared__ uint64_t out_first_pkt_idx;
	__shared__ uint32_t out_pkt_num;
	__shared__ struct doca_gpu_dev_eth_rxq_attr out_attr[MAX_RX_NUM_PKTS];
	uint32_t buf_idx;
	uint64_t tot_pkts_ = 0;

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD) {
			if (threadIdx.x == 0) {
				ret = doca_gpu_dev_eth_rxq_recv<exec_scope,
												DOCA_GPUNETIO_ETH_MCST_AUTO,
												DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
												true>(
								rxq,
								MAX_RX_NUM_PKTS,
								MAX_RX_TIMEOUT_NS,
								&out_first_pkt_idx,
								&out_pkt_num,
								out_attr);
				/* If thread returns receive error, the whole execution stops */
				if (ret != DOCA_SUCCESS) {
					/*
					* printf in CUDA kernel may be a good idea only to report critical errors or debugging.
					* If application prints this message on the console, something bad happened and
					* applications needs to exit
					*/
					printf("Receive UDP kernel error %d rxpkts %d error %d\n", ret, out_pkt_num, ret);
					DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
					*tot_pkts = 0;
				}
			}
			__syncthreads();
		} else {
			ret = doca_gpu_dev_eth_rxq_recv<exec_scope,
											DOCA_GPUNETIO_ETH_MCST_AUTO,
											DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
											true>(
							rxq,
							MAX_RX_NUM_PKTS,
							MAX_RX_TIMEOUT_NS,
							&out_first_pkt_idx,
							&out_pkt_num,
							out_attr);
			/* If any thread returns receive error, the whole execution stops */
			if (ret != DOCA_SUCCESS) {
				if (threadIdx.x == 0) {
					/*
					* printf in CUDA kernel may be a good idea only to report critical errors or debugging.
					* If application prints this message on the console, something bad happened and
					* applications needs to exit
					*/
					printf("Receive UDP kernel error %d rxpkts %d error %d\n", ret, out_pkt_num, ret);
					DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
					*tot_pkts = 0;
				}
				break;
			}
		}

		if (out_pkt_num == 0)
			continue;

		buf_idx = threadIdx.x;
		while (buf_idx < out_pkt_num) {

#if DOCA_GPUNETIO_SIMPLE_RECEIVE_DEBUG == 1
			uint64_t addr = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, out_first_pkt_idx + buf_idx);
			printf("Thread %d received first id %ld addr %lx tot %d UDP packet with Eth src %02x:%02x:%02x:%02x:%02x:%02x - Eth dst %02x:%02x:%02x:%02x:%02x:%02x - Bytes %d - TS %lx\n",
			       threadIdx.x, out_first_pkt_idx, addr, out_pkt_num,
			       ((uint8_t *)addr)[0], ((uint8_t *)addr)[1], ((uint8_t *)addr)[2], ((uint8_t *)addr)[3],
				   ((uint8_t *)addr)[4], ((uint8_t *)addr)[5], ((uint8_t *)addr)[6], ((uint8_t *)addr)[7],
			       ((uint8_t *)addr)[8], ((uint8_t *)addr)[9], ((uint8_t *)addr)[10], ((uint8_t *)addr)[11],
					out_attr[buf_idx].bytes, out_attr[buf_idx].timestamp_ns
				);
#endif

			/* Add packet processing function here. */
			buf_idx += blockDim.x;
		}

		if (threadIdx.x == 0)
			tot_pkts_ += out_pkt_num;
		__syncthreads();
	}

	if (threadIdx.x == 0) {
		*tot_pkts = tot_pkts_;
		__threadfence_system();
	}
}

extern "C" {

doca_error_t kernel_receive_packets(cudaStream_t stream, struct rxq_queue *rxq, enum doca_gpu_dev_eth_exec_scope exec_scope, uint32_t *gpu_exit_condition, uint64_t *tot_pkts)
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

	/* Assuming CUDA_BLOCK_THREADS == 32 CUDA Threads == 1 WARP for simplicity */
	if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD)
		receive_packets<DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD><<<1, CUDA_BLOCK_THREADS, 0, stream>>>(rxq->eth_rxq_gpu, gpu_exit_condition, tot_pkts);
	if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP)
		receive_packets<DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP><<<1, CUDA_BLOCK_THREADS, 0, stream>>>(rxq->eth_rxq_gpu, gpu_exit_condition, tot_pkts);
	if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK)
		receive_packets<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK><<<1, CUDA_BLOCK_THREADS, 0, stream>>>(rxq->eth_rxq_gpu, gpu_exit_condition, tot_pkts);

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
