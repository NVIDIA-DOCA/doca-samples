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

#include <stdlib.h>
#include <string.h>

#include <doca_gpunetio_dev_buf.cuh>
#include <doca_gpunetio_dev_eth_txq.cuh>
#include <doca_gpunetio_dev_eth_rxq.cuh>

#include "common.h"
#include "packets.h"
#include "filters.cuh"

DOCA_LOG_REGISTER(GPU_SANITY::KernelReceiveIcmp);

static __device__ void icmp_swap_mac_addr(struct eth_ip_icmp_hdr *hdr)
{
	uint16_t addr_bytes[3];

	addr_bytes[0] = ((uint16_t *)hdr->l2_hdr.s_addr_bytes)[0];
	addr_bytes[1] = ((uint16_t *)hdr->l2_hdr.s_addr_bytes)[1];
	addr_bytes[2] = ((uint16_t *)hdr->l2_hdr.s_addr_bytes)[2];

	((uint16_t *)hdr->l2_hdr.s_addr_bytes)[0] = ((uint16_t *)hdr->l2_hdr.d_addr_bytes)[0];
	((uint16_t *)hdr->l2_hdr.s_addr_bytes)[1] = ((uint16_t *)hdr->l2_hdr.d_addr_bytes)[1];
	((uint16_t *)hdr->l2_hdr.s_addr_bytes)[2] = ((uint16_t *)hdr->l2_hdr.d_addr_bytes)[2];

	((uint16_t *)hdr->l2_hdr.d_addr_bytes)[0] = addr_bytes[0];
	((uint16_t *)hdr->l2_hdr.d_addr_bytes)[1] = addr_bytes[1];
	((uint16_t *)hdr->l2_hdr.d_addr_bytes)[2] = addr_bytes[2];
}

static __device__ void icmp_swap_ip_addr(struct eth_ip_icmp_hdr *hdr)
{
	uint32_t tmp;

	tmp = hdr->l3_hdr.src_addr;
	hdr->l3_hdr.src_addr = hdr->l3_hdr.dst_addr;
	hdr->l3_hdr.dst_addr = tmp;
}

static __device__ uint16_t icmp_checksum(const uint16_t *icmph, int len)
{
	uint32_t sum = 0;
	uint16_t odd_byte;

	while (len > 1) {
		sum += *icmph++;
		len -= 2;
	}

	if (len == 1) {
		*(uint8_t *)(&odd_byte) = *(uint8_t *)icmph;
		sum += odd_byte;
	}

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (~sum);
}

__global__ void cuda_kernel_receive_icmp(uint32_t *exit_cond,
					 struct doca_gpu_eth_rxq *rxq,
					 struct doca_gpu_eth_txq *txq)
{
	__shared__ uint32_t rx_pkt_num;
	__shared__ uint64_t rx_buf_idx;

	doca_error_t ret;
	uint64_t buf_idx = 0;
	uintptr_t buf_addr;
	struct doca_gpu_buf *buf_ptr;
	struct eth_ip_icmp_hdr *hdr;
	uint8_t *payload;
	uint32_t nbytes;
	uint32_t lane_id = threadIdx.x % WARP_SIZE;
	uint32_t warp_id = threadIdx.x / WARP_SIZE;

	if (warp_id > 0)
		return;

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		ret = doca_gpu_dev_eth_rxq_receive_warp(rxq,
							MAX_RX_NUM_PKTS_ICMP,
							MAX_RX_TIMEOUT_NS_ICMP,
							&rx_pkt_num,
							&rx_buf_idx);
		/* If any thread returns receive error, the whole execution stops */
		if (ret != DOCA_SUCCESS) {
			if (lane_id == 0) {
				/*
				 * printf in CUDA kernel may be a good idea only to report critical errors or debugging.
				 * If application prints this message on the console, something bad happened and
				 * applications needs to exit
				 */
				printf("Receive ICMP kernel error %d warp %d lane %d error %d\n",
				       ret,
				       warp_id,
				       rx_pkt_num,
				       ret);
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
			}
			break;
		}

		if (rx_pkt_num == 0)
			continue;

		buf_idx = lane_id;
		while (buf_idx < rx_pkt_num) {
			doca_gpu_dev_eth_rxq_get_buf(rxq, rx_buf_idx + buf_idx, &buf_ptr);
			doca_gpu_dev_buf_get_addr(buf_ptr, &buf_addr);

			raw_to_icmp(buf_addr, &hdr, &payload);

			if (hdr->l4_hdr.type == ICMP_ECHO_REQUEST && hdr->l4_hdr.code == 0) {
				icmp_swap_mac_addr(hdr);
				icmp_swap_ip_addr(hdr);
				hdr->l3_hdr.time_to_live = 128;
				hdr->l3_hdr.hdr_checksum = 0;
				nbytes = BYTE_SWAP16(hdr->l3_hdr.total_length) + sizeof(struct ether_hdr);

				hdr->l4_hdr.type = ICMP_ECHO_REPLY;
				hdr->l4_hdr.cksum = 0;
				hdr->l4_hdr.cksum =
					icmp_checksum((uint16_t *)&(hdr->l4_hdr),
						      nbytes - (sizeof(struct ether_hdr) - sizeof(struct ipv4_hdr)));
				/* Will translate in a notification caught by DOCA PE on the CPU side. */
				doca_gpu_dev_eth_txq_send_enqueue_strong(txq,
									 buf_ptr,
									 nbytes,
									 DOCA_GPU_SEND_FLAG_NOTIFY);
			} else
				printf("Unknown ICMP type %d code %d id %d seq %d\n",
				       hdr->l4_hdr.type,
				       hdr->l4_hdr.code,
				       BYTE_SWAP16(hdr->l4_hdr.ident),
				       BYTE_SWAP16(hdr->l4_hdr.seq_nb));

			buf_idx += WARP_SIZE;
		}
		__syncwarp();

		if (lane_id == 0) {
			doca_gpu_dev_eth_txq_commit_strong(txq);
			doca_gpu_dev_eth_txq_push(txq);
		}
		__syncwarp();
	}
}

extern "C" {

doca_error_t kernel_receive_icmp(cudaStream_t stream, uint32_t *exit_cond, struct rxq_icmp_queues *icmp_queues)
{
	cudaError_t result = cudaSuccess;

	if (exit_cond == 0 || icmp_queues == NULL || icmp_queues->numq == 0 || icmp_queues->numq > MAX_QUEUES_ICMP) {
		DOCA_LOG_ERR("kernel_receive_icmp invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	/* Assume MAX_QUEUES_ICMP == 1 */
	cuda_kernel_receive_icmp<<<1, WARP_SIZE, 0, stream>>>(exit_cond,
							      icmp_queues->eth_rxq_gpu[0],
							      icmp_queues->eth_txq_gpu[0]);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
