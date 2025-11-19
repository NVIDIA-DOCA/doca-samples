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
#include <doca_gpunetio_dev_eth_txq.cuh>

#include "common.h"
#include "packets.h"
#include "filters.cuh"

DOCA_LOG_REGISTER(GPU_SANITY::KernelHttpServer);

static __device__ void http_set_mac_addr(struct eth_ip_tcp_hdr *hdr,
					 const uint16_t *src_bytes,
					 const uint16_t *dst_bytes)
{
	((uint16_t *)hdr->l2_hdr.s_addr_bytes)[0] = src_bytes[0];
	((uint16_t *)hdr->l2_hdr.s_addr_bytes)[1] = src_bytes[1];
	((uint16_t *)hdr->l2_hdr.s_addr_bytes)[2] = src_bytes[2];

	((uint16_t *)hdr->l2_hdr.d_addr_bytes)[0] = dst_bytes[0];
	((uint16_t *)hdr->l2_hdr.d_addr_bytes)[1] = dst_bytes[1];
	((uint16_t *)hdr->l2_hdr.d_addr_bytes)[2] = dst_bytes[2];
}

__global__ void cuda_kernel_http_server(uint32_t *exit_cond,
					struct doca_gpu_eth_txq *txq0,
					struct doca_gpu_eth_txq *txq1,
					struct doca_gpu_eth_txq *txq2,
					struct doca_gpu_eth_txq *txq3,
					int sem_num,
					struct doca_gpu_semaphore_gpu *sem_http0,
					struct doca_gpu_semaphore_gpu *sem_http1,
					struct doca_gpu_semaphore_gpu *sem_http2,
					struct doca_gpu_semaphore_gpu *sem_http3,
					struct doca_gpu_buf_arr *buf_arr_gpu_page_index,
					uint32_t nbytes_page_index,
					struct doca_gpu_buf_arr *buf_arr_gpu_page_contacts,
					uint32_t nbytes_page_contacts,
					struct doca_gpu_buf_arr *buf_arr_gpu_page_not_found,
					uint32_t nbytes_page_not_found)
{
	doca_error_t ret;
	struct doca_gpu_eth_txq *txq = NULL;
	struct doca_gpu_semaphore_gpu *sem_http = NULL;
	struct doca_gpu_buf *buf = NULL;
	uintptr_t buf_addr;
	struct eth_ip_tcp_hdr *hdr;
	enum doca_gpu_semaphore_status status;
	struct info_http *http_global;
	uint8_t *payload;
	uint32_t base_pkt_len = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);
	uint32_t nbytes_page = 0, buf_mkey;
	const uint32_t lane_id = doca_gpu_dev_eth_get_lane_id();
	uint32_t warp_id = threadIdx.x / WARP_SIZE;
	uint32_t sem_http_idx = lane_id;
	uint64_t doca_gpu_buf_idx = lane_id;
	doca_gpu_dev_eth_ticket_t out_ticket;

	if (warp_id == 0) {
		txq = txq0;
		sem_http = sem_http0;
	} else if (warp_id == 1) {
		txq = txq1;
		sem_http = sem_http1;
	} else if (warp_id == 2) {
		txq = txq2;
		sem_http = sem_http2;
	} else if (warp_id == 3) {
		txq = txq3;
		sem_http = sem_http3;
	} else
		return;

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		if (txq && sem_http) {
			ret = doca_gpu_dev_semaphore_get_status(sem_http, sem_http_idx, &status);
			if (ret != DOCA_SUCCESS) {
				if (lane_id == 0) {
					printf("HTTP server semaphore wait error %d Block %d error %d\n",
					       ret,
					       warp_id,
					       ret);
					DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
				}
				break;
			}

			if (status == DOCA_GPU_SEMAPHORE_STATUS_READY) {
				ret = doca_gpu_dev_semaphore_get_custom_info_addr(sem_http,
										  sem_http_idx,
										  (void **)&http_global);
				if (ret != DOCA_SUCCESS) {
					printf("TCP Error %d doca_gpu_dev_semaphore_get_custom_info_addr block %d thread %d\n",
					       ret,
					       warp_id,
					       lane_id);
					DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
					break;
				}

				if (http_global->page == HTTP_GET_INDEX) {
					doca_gpu_dev_buf_get_buf(buf_arr_gpu_page_index, doca_gpu_buf_idx, &buf);
					nbytes_page = nbytes_page_index;
				} else if (http_global->page == HTTP_GET_CONTACTS) {
					doca_gpu_dev_buf_get_buf(buf_arr_gpu_page_contacts, doca_gpu_buf_idx, &buf);
					nbytes_page = nbytes_page_contacts;
				} else {
					doca_gpu_dev_buf_get_buf(buf_arr_gpu_page_not_found, doca_gpu_buf_idx, &buf);
					nbytes_page = nbytes_page_not_found;
				}

				doca_gpu_dev_buf_get_addr(buf, &buf_addr);
				doca_gpu_dev_buf_get_mkey(buf, &buf_mkey);

				raw_to_tcp(buf_addr, &hdr, &payload);
				http_set_mac_addr(hdr,
						  (uint16_t *)http_global->eth_dst_addr_bytes,
						  (uint16_t *)http_global->eth_src_addr_bytes);
				hdr->l3_hdr.src_addr = http_global->ip_dst_addr;
				hdr->l3_hdr.dst_addr = http_global->ip_src_addr;
				hdr->l4_hdr.src_port = http_global->tcp_dst_port;
				hdr->l4_hdr.dst_port = http_global->tcp_src_port;
				hdr->l4_hdr.sent_seq = http_global->tcp_recv_ack;
				uint32_t prev_pkt_sz = BYTE_SWAP16(http_global->ip_total_length) -
						       sizeof(struct ipv4_hdr) -
						       ((http_global->tcp_dt_off >> 4) * sizeof(uint32_t));
				hdr->l4_hdr.recv_ack =
					BYTE_SWAP32(BYTE_SWAP32(http_global->tcp_sent_seq) + prev_pkt_sz);
				hdr->l4_hdr.cksum = 0;

				// DOCA_GPUNETIO_ETH_SYNC_SCOPE_CTA: this CUDA kernel has only 1 CUDA block, each warp a different QP
				// DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD: Not all threads in the warp may get an HTTP message to send
				doca_gpu_dev_eth_txq_send<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_CTA,
										DOCA_GPUNETIO_ETH_SYNC_SCOPE_CTA,
										DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
										DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD>(txq, buf_addr, buf_mkey, base_pkt_len + nbytes_page,
											DOCA_GPUNETIO_ETH_SEND_FLAG_NONE, &out_ticket);

				ret = doca_gpu_dev_semaphore_set_status(sem_http,
									sem_http_idx,
									DOCA_GPU_SEMAPHORE_STATUS_DONE);
				if (ret != DOCA_SUCCESS) {
					printf("Error %d doca_gpu_dev_eth_txq_send_enqueue_strong block %d thread %d\n",
					       ret,
					       warp_id,
					       lane_id);
					DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
					break;
				}

				sem_http_idx = (sem_http_idx + WARP_SIZE) % sem_num;
				doca_gpu_buf_idx = (doca_gpu_buf_idx + WARP_SIZE) % TX_BUF_NUM;
			}
		}
		__syncwarp();
	}
}

extern "C" {

doca_error_t kernel_http_server(cudaStream_t stream,
				uint32_t *exit_cond,
				struct rxq_tcp_queues *tcp_queues,
				struct txq_http_queues *http_queues)
{
	cudaError_t result = cudaSuccess;

	if (tcp_queues == NULL || tcp_queues->numq == 0 || exit_cond == 0) {
		DOCA_LOG_ERR("kernel_receive_icmp invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	/*
	 * Assume no more than MAX_QUEUE (4) receive queues
	 */
	cuda_kernel_http_server<<<1, tcp_queues->numq * WARP_SIZE, 0, stream>>>(
		exit_cond,
		http_queues->eth_txq_gpu[0],
		http_queues->eth_txq_gpu[1],
		http_queues->eth_txq_gpu[2],
		http_queues->eth_txq_gpu[3],
		tcp_queues->nums,
		tcp_queues->sem_http_gpu[0],
		tcp_queues->sem_http_gpu[1],
		tcp_queues->sem_http_gpu[2],
		tcp_queues->sem_http_gpu[3],
		http_queues->buf_page_index.buf_arr_gpu,
		http_queues->buf_page_index.pkt_nbytes,
		http_queues->buf_page_contacts.buf_arr_gpu,
		http_queues->buf_page_contacts.pkt_nbytes,
		http_queues->buf_page_not_found.buf_arr_gpu,
		http_queues->buf_page_not_found.pkt_nbytes);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
