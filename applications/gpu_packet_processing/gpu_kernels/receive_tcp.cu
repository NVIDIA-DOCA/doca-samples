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

#include <stdlib.h>
#include <string.h>

#include <doca_gpunetio_dev_buf.cuh>
#include <doca_gpunetio_dev_sem.cuh>
#include <doca_gpunetio_dev_eth_rxq.cuh>

#include "common.h"
#include "packets.h"
#include "filters.cuh"

DOCA_LOG_REGISTER(GPU_SANITY::KernelReceiveTcp);

static __device__ void report_http_info(struct info_http *http_global, struct eth_ip_tcp_hdr *hdr, uint8_t *payload)
{
	/* Ethernet info */
	((uint16_t *)http_global->eth_src_addr_bytes)[0] = ((uint16_t *)hdr->l2_hdr.s_addr_bytes)[0];
	((uint16_t *)http_global->eth_src_addr_bytes)[1] = ((uint16_t *)hdr->l2_hdr.s_addr_bytes)[1];
	((uint16_t *)http_global->eth_src_addr_bytes)[2] = ((uint16_t *)hdr->l2_hdr.s_addr_bytes)[2];

	((uint16_t *)http_global->eth_dst_addr_bytes)[0] = ((uint16_t *)hdr->l2_hdr.d_addr_bytes)[0];
	((uint16_t *)http_global->eth_dst_addr_bytes)[1] = ((uint16_t *)hdr->l2_hdr.d_addr_bytes)[1];
	((uint16_t *)http_global->eth_dst_addr_bytes)[2] = ((uint16_t *)hdr->l2_hdr.d_addr_bytes)[2];

	/* IP info */
	http_global->ip_src_addr = hdr->l3_hdr.src_addr;
	http_global->ip_dst_addr = hdr->l3_hdr.dst_addr;
	http_global->ip_total_length = hdr->l3_hdr.total_length;

	/* TCP info */
	http_global->tcp_src_port = hdr->l4_hdr.src_port;
	http_global->tcp_dst_port = hdr->l4_hdr.dst_port;
	http_global->tcp_dt_off = hdr->l4_hdr.data_off;
	http_global->tcp_sent_seq = hdr->l4_hdr.sent_seq;
	http_global->tcp_recv_ack = hdr->l4_hdr.recv_ack;
}

static __device__ enum http_page_get get_http_page_type(const uint8_t *payload)
{
	/* index */
	if (payload[5] == 'i' && payload[6] == 'n' && payload[7] == 'd' && payload[8] == 'e' && payload[9] == 'x' &&
	    payload[10] == '.')
		return HTTP_GET_INDEX;
	/* contacts */
	if (payload[5] == 'c' && payload[6] == 'o' && payload[7] == 'n' && payload[8] == 't' && payload[9] == 'a' &&
	    payload[10] == 'c' && payload[11] == 't' && payload[12] == 's' && payload[13] == '.')
		return HTTP_GET_CONTACTS;
	/* 404 not found */
	return HTTP_GET_NOT_FOUND;
}

__global__ void cuda_kernel_receive_tcp(uint32_t *exit_cond,
					struct doca_gpu_eth_rxq *rxq0,
					struct doca_gpu_eth_rxq *rxq1,
					struct doca_gpu_eth_rxq *rxq2,
					struct doca_gpu_eth_rxq *rxq3,
					int sem_num,
					struct doca_gpu_semaphore_gpu *sem_stats0,
					struct doca_gpu_semaphore_gpu *sem_stats1,
					struct doca_gpu_semaphore_gpu *sem_stats2,
					struct doca_gpu_semaphore_gpu *sem_stats3,
					struct doca_gpu_semaphore_gpu *sem_http0,
					struct doca_gpu_semaphore_gpu *sem_http1,
					struct doca_gpu_semaphore_gpu *sem_http2,
					struct doca_gpu_semaphore_gpu *sem_http3,
					bool http_server)
{
	__shared__ uint64_t out_first_pkt_idx;
	__shared__ uint32_t out_pkt_num;
	__shared__ struct stats_tcp stats_sh;
	__shared__ int sem_http_idx;

	doca_error_t ret;
	struct doca_gpu_eth_rxq *rxq = NULL;
	struct doca_gpu_semaphore_gpu *sem_stats = NULL;
	struct doca_gpu_semaphore_gpu *sem_http = NULL;
	struct stats_tcp stats_thread;
	struct stats_tcp *stats_global;
	struct info_http *http_global;
	struct eth_ip_tcp_hdr *hdr;
	uintptr_t buf_addr;
	uint64_t buf_idx = 0;
	uint32_t lane_id = doca_gpu_dev_eth_get_lane_id();
	uint32_t sem_stats_idx = 0;
	uint8_t *payload;

	if (blockIdx.x == 0) {
		rxq = rxq0;
		sem_stats = sem_stats0;
		sem_http = sem_http0;
	} else if (blockIdx.x == 1) {
		rxq = rxq1;
		sem_stats = sem_stats1;
		sem_http = sem_http1;
	} else if (blockIdx.x == 2) {
		rxq = rxq2;
		sem_stats = sem_stats2;
		sem_http = sem_http2;
	} else if (blockIdx.x == 3) {
		rxq = rxq3;
		sem_stats = sem_stats3;
		sem_http = sem_http3;
	} else
		return;

	if (threadIdx.x == 0) {
		DOCA_GPUNETIO_VOLATILE(stats_sh.http) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.http_head) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.http_get) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.http_post) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_syn) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_fin) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_ack) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.others) = 0;
		DOCA_GPUNETIO_VOLATILE(sem_http_idx) = 0;
	}
	__syncthreads();

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		stats_thread.http = 0;
		stats_thread.http_head = 0;
		stats_thread.http_get = 0;
		stats_thread.http_post = 0;
		stats_thread.tcp_syn = 0;
		stats_thread.tcp_fin = 0;
		stats_thread.tcp_ack = 0;
		stats_thread.others = 0;

		ret = doca_gpu_dev_eth_rxq_recv<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK,
									DOCA_GPUNETIO_ETH_MCST_AUTO,
									DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO>(
								rxq,
								MAX_RX_NUM_PKTS,
								MAX_RX_TIMEOUT_NS,
								&out_first_pkt_idx,
								&out_pkt_num,
								NULL);
		/* If any thread returns receive error, the whole execution stops */
		if (ret != DOCA_SUCCESS) {
			if (threadIdx.x == 0) {
				/*
				 * printf in CUDA kernel may be a good idea only to report critical errors or debugging.
				 * If application prints this message on the console, something bad happened and
				 * applications needs to exit
				 */
				printf("Receive TCP kernel error %d Block %d rxpkts %d error %d\n",
				       ret,
				       blockIdx.x,
				       out_pkt_num,
				       ret);
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
			}
			break;
		}

		if (out_pkt_num == 0)
			continue;

		buf_idx = threadIdx.x;
		while (buf_idx < out_pkt_num) {
			buf_addr = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, out_first_pkt_idx + buf_idx);
			raw_to_tcp(buf_addr, &hdr, &payload);

			/* Priority to GET for the HTTP server mode */
			if (filter_is_http_get(payload)) {
				/* Pass HTTP info to the */
				if (http_server) {
					int idx_tmp = atomicAdd(&sem_http_idx, 1);
					idx_tmp = idx_tmp % sem_num;
					ret = doca_gpu_dev_semaphore_get_custom_info_addr(sem_http,
											  idx_tmp,
											  (void **)&http_global);
					if (ret != DOCA_SUCCESS) {
						printf("TCP Error %d doca_gpu_dev_semaphore_get_custom_info_addr block %d thread %d\n",
						       ret,
						       blockIdx.x,
						       threadIdx.x);
						DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
						break;
					}

					report_http_info(http_global, hdr, payload);
					/* Use proper value here */
					http_global->page = get_http_page_type(payload);
					__threadfence();
					doca_gpu_dev_semaphore_set_status(sem_http,
									  idx_tmp,
									  DOCA_GPU_SEMAPHORE_STATUS_READY);
				}
				stats_thread.http_get++;
			} else if (filter_is_http_head(payload))
				stats_thread.http_head++;
			else if (filter_is_http_post(payload))
				stats_thread.http_post++;
			else if (filter_is_http(payload))
				stats_thread.http++;
			else if (filter_is_tcp_fin(&(hdr->l4_hdr)))
				stats_thread.tcp_fin++;
			else if (filter_is_tcp_syn(&(hdr->l4_hdr)))
				stats_thread.tcp_syn++;
			else if (filter_is_tcp_ack(&(hdr->l4_hdr)))
				stats_thread.tcp_ack++;
			else
				stats_thread.others++;

			/* TCP header may vary so a newer smaller packet may not overwrite old longer packet */
			wipe_packet_32b(payload);
			buf_idx += blockDim.x;
		}

#pragma unroll
		for (int offset = 16; offset > 0; offset /= 2) {
			stats_thread.http += __shfl_down_sync(WARP_FULL_MASK, stats_thread.http, offset);
			stats_thread.http_head += __shfl_down_sync(WARP_FULL_MASK, stats_thread.http_head, offset);
			stats_thread.http_get += __shfl_down_sync(WARP_FULL_MASK, stats_thread.http_get, offset);
			stats_thread.http_post += __shfl_down_sync(WARP_FULL_MASK, stats_thread.http_post, offset);
			stats_thread.tcp_syn += __shfl_down_sync(WARP_FULL_MASK, stats_thread.tcp_syn, offset);
			stats_thread.tcp_fin += __shfl_down_sync(WARP_FULL_MASK, stats_thread.tcp_fin, offset);
			stats_thread.tcp_ack += __shfl_down_sync(WARP_FULL_MASK, stats_thread.tcp_ack, offset);
			stats_thread.others += __shfl_down_sync(WARP_FULL_MASK, stats_thread.others, offset);

			__syncwarp();
		}

		if (lane_id == 0) {
			atomicAdd_block(&(stats_sh.http), stats_thread.http);
			atomicAdd_block(&(stats_sh.http_head), stats_thread.http_head);
			atomicAdd_block(&(stats_sh.http_get), stats_thread.http_get);
			atomicAdd_block(&(stats_sh.http_post), stats_thread.http_post);
			atomicAdd_block(&(stats_sh.tcp_syn), stats_thread.tcp_syn);
			atomicAdd_block(&(stats_sh.tcp_fin), stats_thread.tcp_fin);
			atomicAdd_block(&(stats_sh.tcp_ack), stats_thread.tcp_ack);
			atomicAdd_block(&(stats_sh.others), stats_thread.others);
		}
		__syncthreads();

		if (threadIdx.x == 0 && out_pkt_num > 0) {
			ret = doca_gpu_dev_semaphore_get_custom_info_addr(sem_stats,
									  sem_stats_idx,
									  (void **)&stats_global);
			if (ret != DOCA_SUCCESS) {
				printf("TCP Error %d doca_gpu_dev_semaphore_get_custom_info_addr block %d thread %d\n",
				       ret,
				       blockIdx.x,
				       threadIdx.x);
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
				break;
			}

			DOCA_GPUNETIO_VOLATILE(stats_global->http) = DOCA_GPUNETIO_VOLATILE(stats_sh.http);
			DOCA_GPUNETIO_VOLATILE(stats_global->http_head) = DOCA_GPUNETIO_VOLATILE(stats_sh.http_head);
			DOCA_GPUNETIO_VOLATILE(stats_global->http_get) = DOCA_GPUNETIO_VOLATILE(stats_sh.http_get);
			DOCA_GPUNETIO_VOLATILE(stats_global->http_post) = DOCA_GPUNETIO_VOLATILE(stats_sh.http_post);
			DOCA_GPUNETIO_VOLATILE(stats_global->tcp_syn) = DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_syn);
			DOCA_GPUNETIO_VOLATILE(stats_global->tcp_fin) = DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_fin);
			DOCA_GPUNETIO_VOLATILE(stats_global->tcp_ack) = DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_ack);
			DOCA_GPUNETIO_VOLATILE(stats_global->others) = DOCA_GPUNETIO_VOLATILE(stats_sh.others);
			DOCA_GPUNETIO_VOLATILE(stats_global->total) = out_pkt_num;

			doca_gpu_dev_semaphore_set_status(sem_stats, sem_stats_idx, DOCA_GPU_SEMAPHORE_STATUS_READY);
			__threadfence_system();

			sem_stats_idx = (sem_stats_idx + 1) % sem_num;

			DOCA_GPUNETIO_VOLATILE(stats_sh.http) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.http_head) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.http_get) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.http_post) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_syn) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_fin) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.tcp_ack) = 0;
			DOCA_GPUNETIO_VOLATILE(stats_sh.others) = 0;
		}

		__syncthreads();
	}
}

extern "C" {

doca_error_t kernel_receive_tcp(cudaStream_t stream,
				uint32_t *exit_cond,
				struct rxq_tcp_queues *tcp_queues,
				bool http_server)
{
	cudaError_t result = cudaSuccess;

	if (exit_cond == 0 || tcp_queues == NULL || tcp_queues->numq == 0) {
		DOCA_LOG_ERR("kernel_receive_tcp invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	/* Assume MAX_QUEUES == 4 */
	cuda_kernel_receive_tcp<<<tcp_queues->numq, CUDA_THREADS, 0, stream>>>(exit_cond,
									       tcp_queues->eth_rxq_gpu[0],
									       tcp_queues->eth_rxq_gpu[1],
									       tcp_queues->eth_rxq_gpu[2],
									       tcp_queues->eth_rxq_gpu[3],
									       tcp_queues->nums,
									       tcp_queues->sem_gpu[0],
									       tcp_queues->sem_gpu[1],
									       tcp_queues->sem_gpu[2],
									       tcp_queues->sem_gpu[3],
									       tcp_queues->sem_http_gpu[0],
									       tcp_queues->sem_http_gpu[1],
									       tcp_queues->sem_http_gpu[2],
									       tcp_queues->sem_http_gpu[3],
									       http_server);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
