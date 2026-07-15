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

#ifndef DOCA_GPUNETIO_FILTERS_H
#define DOCA_GPUNETIO_FILTERS_H

#include "common.h"
#include "packets.h"

#define ACK_MASK (0x00 | TCP_FLAG_ACK)

__device__ __inline__ int raw_to_udp(const uintptr_t buf_addr, struct eth_ip_udp_hdr **hdr, uint8_t **payload)
{
	(*hdr) = (struct eth_ip_udp_hdr *)buf_addr;
	(*payload) = (uint8_t *)(buf_addr + sizeof(struct eth_ip_udp_hdr));

	return 0;
}

__device__ __inline__ int raw_to_tcp(const uintptr_t buf_addr, struct eth_ip_tcp_hdr **hdr, uint8_t **payload)
{
	(*hdr) = (struct eth_ip_tcp_hdr *)buf_addr;
	(*payload) = (uint8_t *)(buf_addr + sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) +
				 ((*hdr)->l4_hdr.dt_off * sizeof(int)));

	return 0;
}

__device__ __inline__ int raw_to_icmp(const uintptr_t buf_addr, struct eth_ip_icmp_hdr **hdr, uint8_t **payload)
{
	(*hdr) = (struct eth_ip_icmp_hdr *)buf_addr;
	(*payload) = (uint8_t *)(buf_addr + sizeof(struct eth_ip_icmp_hdr));

	return 0;
}

__device__ __inline__ int wipe_packet_32b(uint8_t *payload)
{
#pragma unroll
	for (int idx = 0; idx < 32; idx++)
		payload[idx] = 0;

	return 0;
}

/* TCP */
__device__ __inline__ int filter_is_http(const uint8_t *pld)
{
	/* HTTP/1.1 */
	if (pld[0] != 'H')
		return 0;
	if (pld[1] == 'T' && pld[2] == 'T' && pld[3] == 'P' && pld[4] == '/' && pld[5] == '1' && pld[6] == '.' &&
	    pld[7] == '1')
		return 1;
	return 0;
}

__device__ __inline__ int filter_is_http_get(const uint8_t *pld)
{
	/* GET / */
	if (pld[0] != 'G')
		return 0;
	if (pld[1] == 'E' && pld[2] == 'T' && pld[3] == WHITESPACE_ASCII && pld[4] == '/')
		return 1;
	return 0;
}

__device__ __inline__ int filter_is_http_post(const uint8_t *pld)
{
	/* POST / */
	if (pld[0] != 'P')
		return 0;
	if (pld[1] == 'O' && pld[2] == 'S' && pld[3] == 'T' && pld[4] == WHITESPACE_ASCII && pld[5] == '/')
		return 1;
	return 0;
}

__device__ __inline__ int filter_is_http_head(const uint8_t *pld)
{
	/* HEAD / */
	if (pld[0] != 'H' || pld[1] != 'E')
		return 0;
	if (pld[2] == 'A' && pld[3] == 'D' && pld[4] == WHITESPACE_ASCII && pld[5] == '/')
		return 1;
	return 0;
}

__device__ __inline__ int filter_is_tcp_syn(const struct tcp_hdr *l4_hdr)
{
	return l4_hdr->tcp_flags & TCP_FLAG_SYN;
}

__device__ __inline__ int filter_is_tcp_fin(const struct tcp_hdr *l4_hdr)
{
	return l4_hdr->tcp_flags & TCP_FLAG_FIN;
}

__device__ __inline__ int filter_is_tcp_ack(const struct tcp_hdr *l4_hdr)
{
	return l4_hdr->tcp_flags & ACK_MASK;
}

/* UDP */
__device__ __inline__ int filter_is_dns(const struct udp_hdr *l4_hdr, const uint8_t *pld)
{
	/* Dig deeper into query flags: https://stackoverflow.com/questions/7565300/identifying-dns-packets */
	if (BYTE_SWAP16(l4_hdr->dst_port) == DNS_POST)
		return 1;

	return 0;
}

__device__ __inline__ unsigned long long _gputimestamp()
{
	unsigned long long globaltimer;
	// 64-bit GPU global nanosecond timer
	asm volatile("mov.u64 %0, %globaltimer;" : "=l"(globaltimer));
	return globaltimer;
}

#endif /* DOCA_GPUNETIO_FILTERS_H */