/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef UPF_ACCEL_PRINT_HEADER_H_
#define UPF_ACCEL_PRINT_HEADER_H_

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_gtp.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "upf_accel.h"

#ifndef IPv6_BYTES
#define IPv6_BYTES_FMT "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x"
#define IPv6_BYTES(a) \
	(a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5], (a)[6], (a)[7], (a)[8], (a)[9], (a)[10], (a)[11], (a)[12], \
		(a)[13], (a)[14], (a)[15]
#endif

/**
 * Print an L2 header
 *
 * @packet [in]: packet mbuf
 * @cur_offset [in]: current offset in the packet
 * @pkt_proto_types [in]: packet type
 * @return: size of the L2 header
 */
uint32_t upf_accel_print_l2_header(const struct rte_mbuf *packet, uint32_t cur_offset, uint32_t pkt_proto_types);

/**
 * Print an L3 header
 *
 * @packet [in]: packet mbuf
 * @cur_offset [in]: current offset in the packet
 * @l3_proto [in]: L3 protocol type
 * @return: size of the L3 header
 */
uint32_t upf_accel_print_l3_header(const struct rte_mbuf *packet, uint32_t cur_offset, uint8_t l3_proto);

/**
 * Print an L4 header
 *
 * @packet [in]: packet mbuf
 * @cur_offset [in]: current offset in the packet
 * @l4_proto [in]: L4 protocol type
 * @return: size of the L4 header
 */
uint32_t upf_accel_print_l4_header(const struct rte_mbuf *packet, uint32_t cur_offset, uint8_t l4_proto);

/**
 * Print a GTPU header
 *
 * @packet [in]: packet mbuf
 * @cur_offset [in]: current offset in the packet
 * @return: size of the GTPU header
 */
uint32_t upf_accel_print_gtpu_header(const struct rte_mbuf *packet, uint32_t cur_offset);

/**
 * Print the header information of a packet
 *
 * @pkt [in]: packet mbuf
 * @match [in]: match information
 * @pkt_type [in]: packet type
 */
void upf_accel_print_header_info(const struct rte_mbuf *pkt,
				 const struct upf_accel_match_8t *match,
				 const enum parser_pkt_type pkt_type);

#endif /* UPF_ACCEL_PRINT_HEADER_H_ */