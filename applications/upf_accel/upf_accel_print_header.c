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

#include <doca_log.h>

#include "upf_accel_print_header.h"

DOCA_LOG_REGISTER(UPF_ACCEL::PRINT_HEADER);

uint32_t upf_accel_print_l2_header(const struct rte_mbuf *packet, uint32_t cur_offset, uint32_t pkt_proto_types)
{
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ether_hdr *, cur_offset);
	char dmac_buf[RTE_ETHER_ADDR_FMT_SIZE];
	char smac_buf[RTE_ETHER_ADDR_FMT_SIZE];

	if (!(pkt_proto_types & RTE_PTYPE_L2_ETHER)) {
		DOCA_LOG_DBG("Not an Ethernet packet");
		return 0;
	}

	if (rte_pktmbuf_headroom(packet) < sizeof(struct rte_ether_hdr)) {
		DOCA_LOG_DBG("Not enough headroom for Ethernet header");
		return 0;
	}

	rte_ether_format_addr(dmac_buf, RTE_ETHER_ADDR_FMT_SIZE, &eth_hdr->dst_addr);
	rte_ether_format_addr(smac_buf, RTE_ETHER_ADDR_FMT_SIZE, &eth_hdr->src_addr);
	DOCA_LOG_DBG("Ethernet Layer: dmac=%s, smac=%s, ether_type=0x%04x",
		     dmac_buf,
		     smac_buf,
		     htonl(eth_hdr->ether_type) >> 16);

	return sizeof(struct rte_ether_hdr);
}

/**
 * Print an IPV4 address
 *
 * @dip [in]: destination IP address
 * @sip [in]: source IP address
 */
static void upf_accel_print_ipv4_addr(const rte_be32_t dip, const rte_be32_t sip)
{
	DOCA_LOG_DBG("IPv4 Layer: dip=%d.%d.%d.%d, sip=%d.%d.%d.%d",
		     (dip & 0xff000000) >> 24,
		     (dip & 0x00ff0000) >> 16,
		     (dip & 0x0000ff00) >> 8,
		     (dip & 0x000000ff),
		     (sip & 0xff000000) >> 24,
		     (sip & 0x00ff0000) >> 16,
		     (sip & 0x0000ff00) >> 8,
		     (sip & 0x000000ff));
}

/**
 * Print an IPV6 address
 *
 * @dst_addr [in]: destination IP address
 * @src_addr [in]: source IP address
 */
static void upf_accel_print_ipv6_addr(const uint8_t dst_addr[16], const uint8_t src_addr[16])
{
	DOCA_LOG_DBG("IPv6 Layer: dip=" IPv6_BYTES_FMT ", sip=" IPv6_BYTES_FMT,
		     IPv6_BYTES(dst_addr),
		     IPv6_BYTES(src_addr));
}

uint32_t upf_accel_print_l3_header(const struct rte_mbuf *packet, uint32_t cur_offset, uint8_t l3_proto)
{
	const struct rte_ipv4_hdr *ipv4_hdr;
	const struct rte_ipv6_hdr *ipv6_hdr;

	switch (l3_proto) {
	case DOCA_FLOW_L3_TYPE_IP4:
		if (rte_pktmbuf_headroom(packet) < sizeof(struct rte_ipv4_hdr)) {
			DOCA_LOG_DBG("Not enough headroom for IPv4 header");
			return 0;
		}
		ipv4_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, cur_offset);

		upf_accel_print_ipv4_addr(htonl(ipv4_hdr->dst_addr), htonl(ipv4_hdr->src_addr));

		return sizeof(struct rte_ipv4_hdr);
	case DOCA_FLOW_L3_TYPE_IP6:
		if (rte_pktmbuf_headroom(packet) < sizeof(struct rte_ipv6_hdr)) {
			DOCA_LOG_DBG("Not enough headroom for IPv6 header");
			return 0;
		}
		ipv6_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ipv6_hdr *, cur_offset);

		upf_accel_print_ipv6_addr(ipv6_hdr->dst_addr, ipv6_hdr->src_addr);
		if (ipv6_hdr->proto != IPPROTO_UDP && ipv6_hdr->proto != IPPROTO_TCP) {
			DOCA_LOG_DBG("Unidentified next header");
			return 0;
		}

		return sizeof(struct rte_ipv6_hdr);
	default:
		DOCA_LOG_DBG("Unsupported L3 protocol");
		return 0;
	}
}

uint32_t upf_accel_print_l4_header(const struct rte_mbuf *packet, uint32_t cur_offset, uint8_t l4_proto)
{
	const struct rte_udp_hdr *udp_hdr;
	const struct rte_tcp_hdr *tcp_hdr;

	switch (l4_proto) {
	case IPPROTO_UDP:
		if (rte_pktmbuf_headroom(packet) < sizeof(struct rte_udp_hdr)) {
			DOCA_LOG_DBG("Not enough headroom for UDP header");
			return 0;
		}
		udp_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_udp_hdr *, cur_offset);
		DOCA_LOG_DBG("UDP Layer: dport=%u, sport=%u",
			     rte_be_to_cpu_16(udp_hdr->dst_port),
			     rte_be_to_cpu_16(udp_hdr->src_port));

		return sizeof(struct rte_udp_hdr);
	case IPPROTO_TCP:
		if (rte_pktmbuf_headroom(packet) < sizeof(struct rte_tcp_hdr)) {
			DOCA_LOG_DBG("Not enough headroom for TCP header");
			return 0;
		}
		tcp_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_tcp_hdr *, cur_offset);
		DOCA_LOG_DBG("TCP Layer: dport=%u, sport=%u",
			     rte_be_to_cpu_16(tcp_hdr->dst_port),
			     rte_be_to_cpu_16(tcp_hdr->src_port));

		return sizeof(struct rte_tcp_hdr);
	default:
		DOCA_LOG_DBG("Unsupported L4 protocol");
		return 0;
	}
}

uint32_t upf_accel_print_gtpu_header(const struct rte_mbuf *packet, uint32_t cur_offset)
{
	uint32_t hdr_len = sizeof(struct rte_gtp_hdr);
	const struct rte_gtp_hdr *gtp_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_gtp_hdr *, cur_offset);
	const uint32_t teid = rte_be_to_cpu_32(gtp_hdr->teid);
	struct rte_gtp_psc_generic_hdr *gtp_psc_hdr;

	if (!gtp_hdr->e) {
		if (rte_pktmbuf_headroom(packet) < hdr_len) {
			DOCA_LOG_DBG("Not enough headroom for GTPU header");
			return 0;
		}
		DOCA_LOG_DBG("4G GTPU: TEID %u", rte_be_to_cpu_32(gtp_hdr->teid));

		return hdr_len;
	} else {
		hdr_len += sizeof(struct rte_gtp_hdr_ext_word);
		if (rte_pktmbuf_headroom(packet) < hdr_len + sizeof(struct rte_gtp_psc_generic_hdr)) {
			DOCA_LOG_DBG("Not enough headroom for GTPU header");
			return 0;
		}
		gtp_psc_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_gtp_psc_generic_hdr *, hdr_len);
		DOCA_LOG_DBG("5G GTPU: teid=%u, qfi=%u", teid, gtp_psc_hdr->qfi);

		hdr_len += sizeof(struct rte_gtp_psc_generic_hdr) + 1;
		return hdr_len;
	}
}

void upf_accel_print_header_info(const struct rte_mbuf *pkt,
				 const struct upf_accel_match_8t *match,
				 const enum parser_pkt_type pkt_type)
{
	uint32_t pkt_proto_types = pkt->packet_type;
	bool is_encap = RTE_ETH_IS_TUNNEL_PKT(pkt_proto_types);
	uint32_t cur_offset = 0;
	uint32_t ret = 0;

	DOCA_LOG_DBG("Core %u, type %u", rte_lcore_id(), pkt_type);
	ret = upf_accel_print_l2_header(pkt, cur_offset, pkt_proto_types);
	if (!ret) {
		DOCA_LOG_DBG("Unsupported L2 header");
		return;
	}
	cur_offset += ret;

	if (is_encap) {
		ret = upf_accel_print_l3_header(pkt, cur_offset, match->outer.ip_version);
		if (!ret) {
			DOCA_LOG_DBG("Unsupported L3 protocol");
			return;
		}
		cur_offset += ret;
		ret = upf_accel_print_l4_header(pkt, cur_offset, IPPROTO_UDP);
		if (!ret) {
			DOCA_LOG_DBG("Unsupported L4 protocol");
			return;
		}
		cur_offset += ret;
		ret = upf_accel_print_gtpu_header(pkt, cur_offset);
		if (!ret) {
			DOCA_LOG_DBG("Unsupported GTPU header");
			return;
		}
		cur_offset += ret;
	}

	ret = upf_accel_print_l3_header(pkt, cur_offset, match->inner.ip_version);
	if (!ret) {
		DOCA_LOG_DBG("Unsupported L3 protocol");
		return;
	}
	cur_offset += ret;
	ret = upf_accel_print_l4_header(pkt, cur_offset, match->inner.ip_proto);
	if (!ret) {
		DOCA_LOG_DBG("Unsupported L4 protocol");
		return;
	}
}