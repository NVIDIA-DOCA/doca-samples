/*
 * Copyright (c) 2021 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_gre.h>
#include <rte_gtp.h>
#include <rte_vxlan.h>

#include <doca_log.h>

#include "simple_fwd_pkt.h"

DOCA_LOG_REGISTER(SIMPLE_FWD_PKT);

#define GTP_ESPN_FLAGS_ON(p) (p & 0x7) /* A macro for setting GTP ESPN flags on */
#define GTP_EXT_FLAGS_ON(p) (p & 0x4)  /* A macro for setting GTP EXT flags on */

uint8_t *simple_fwd_pinfo_outer_mac_dst(struct simple_fwd_pkt_info *pinfo)
{
	return ((struct rte_ether_hdr *)pinfo->outer.l2)->dst_addr.addr_bytes;
}

uint8_t *simple_fwd_pinfo_outer_mac_src(struct simple_fwd_pkt_info *pinfo)
{
	return ((struct rte_ether_hdr *)pinfo->outer.l2)->src_addr.addr_bytes;
}

doca_be32_t simple_fwd_pinfo_outer_ipv4_dst(struct simple_fwd_pkt_info *pinfo)
{
	return ((struct rte_ipv4_hdr *)pinfo->outer.l3)->dst_addr;
}

doca_be32_t simple_fwd_pinfo_outer_ipv4_src(struct simple_fwd_pkt_info *pinfo)
{
	return ((struct rte_ipv4_hdr *)pinfo->outer.l3)->src_addr;
}

doca_be32_t simple_fwd_pinfo_inner_ipv4_dst(struct simple_fwd_pkt_info *pinfo)
{
	return ((struct rte_ipv4_hdr *)pinfo->inner.l3)->dst_addr;
}

doca_be32_t simple_fwd_pinfo_inner_ipv4_src(struct simple_fwd_pkt_info *pinfo)
{
	return ((struct rte_ipv4_hdr *)pinfo->inner.l3)->src_addr;
}

/*
 * Extracts the source port address from the packet's info based on layer 4 type
 *
 * @fmt [in]: the packet format as represented in the application
 * @return: source port
 *
 * @NOTE: the returned value is converted to big endian
 */
static doca_be16_t simple_fwd_pinfo_src_port(struct simple_fwd_pkt_format *fmt)
{
	switch (fmt->l4_type) {
	case DOCA_FLOW_PROTO_TCP:
		return ((struct rte_tcp_hdr *)fmt->l4)->src_port;
	case DOCA_FLOW_PROTO_UDP:
		return ((struct rte_udp_hdr *)fmt->l4)->src_port;
	default:
		return 0;
	}
}

/*
 * Extracts the destination port address from the packet's info based on layer 4 type
 *
 * @fmt [in]: the packet format as represented in the application
 * @return: destination port
 *
 * @NOTE: the returned value is converted to big endian
 */
static doca_be16_t simple_fwd_pinfo_dst_port(struct simple_fwd_pkt_format *fmt)
{
	switch (fmt->l4_type) {
	case DOCA_FLOW_PROTO_TCP:
		return ((struct rte_tcp_hdr *)fmt->l4)->dst_port;
	case DOCA_FLOW_PROTO_UDP:
		return ((struct rte_udp_hdr *)fmt->l4)->dst_port;
	default:
		return 0;
	}
}

doca_be16_t simple_fwd_pinfo_inner_src_port(struct simple_fwd_pkt_info *pinfo)
{
	return simple_fwd_pinfo_src_port(&pinfo->inner);
}

doca_be16_t simple_fwd_pinfo_inner_dst_port(struct simple_fwd_pkt_info *pinfo)
{
	return simple_fwd_pinfo_dst_port(&pinfo->inner);
}

doca_be16_t simple_fwd_pinfo_outer_src_port(struct simple_fwd_pkt_info *pinfo)
{
	return simple_fwd_pinfo_src_port(&pinfo->outer);
}

doca_be16_t simple_fwd_pinfo_outer_dst_port(struct simple_fwd_pkt_info *pinfo)
{
	return simple_fwd_pinfo_dst_port(&pinfo->outer);
}

/*
 * Parse the packet and set the packet format as represented in the application
 *
 * @data [in]: packet raw data
 * @len [in]: length of the packet raw data in bytes
 * @l2 [in]: whther or not to set the data pointer in layer 2 field in the packet representation in the application\
 * @fmt [out]: the parsed packet as should be represented in the application fo further processing
 * @return: 0 on success, negative value otherwise
 */
static int simple_fwd_parse_pkt_format(uint8_t *data, int len, bool l2, struct simple_fwd_pkt_format *fmt)
{
	struct rte_ether_hdr *eth = NULL;
	struct rte_ipv4_hdr *iphdr;
	int l3_off = 0;
	int l4_off = 0;
	int l7_off = 0;

	fmt->l2 = data;
	if (l2) {
		eth = (struct rte_ether_hdr *)data;
		fmt->l2 = data;
		switch (rte_be_to_cpu_16(eth->ether_type)) {
		case RTE_ETHER_TYPE_IPV4:
			l3_off = sizeof(struct rte_ether_hdr);
			break;
		case RTE_ETHER_TYPE_IPV6:
			l3_off = sizeof(struct rte_ether_hdr);
			fmt->l3_type = IPV6;
			return -1;
		case RTE_ETHER_TYPE_ARP:
			return -1;
		default:
			DOCA_LOG_WARN("Unsupported L2 type 0x%x", eth->ether_type);
			return -1;
		}
	}

	iphdr = (struct rte_ipv4_hdr *)(data + l3_off);
	if ((iphdr->version_ihl >> 4) != 4)
		return -1;
	if (iphdr->src_addr == 0 || iphdr->dst_addr == 0)
		return -1;
	fmt->l3 = (data + l3_off);
	fmt->l3_type = IPV4;
	l4_off = l3_off + rte_ipv4_hdr_len(iphdr);
	fmt->l4 = data + l4_off;
	switch (iphdr->next_proto_id) {
	case DOCA_FLOW_PROTO_TCP: {
		struct rte_tcp_hdr *tcphdr = (struct rte_tcp_hdr *)(data + l4_off);

		l7_off = l4_off + ((tcphdr->data_off & 0xf0) >> 2);
		if (l7_off > len)
			return -1;
		fmt->l4_type = DOCA_FLOW_PROTO_TCP;
		fmt->l7 = (data + l7_off);
		break;
	}
	case DOCA_FLOW_PROTO_UDP: {
		struct rte_udp_hdr *udphdr = (struct rte_udp_hdr *)(data + l4_off);

		l7_off = l4_off + sizeof(*udphdr);
		fmt->l4_type = DOCA_FLOW_PROTO_UDP;
		if (l7_off > len)
			return -1;
		fmt->l7 = (data + l7_off);
		break;
	}
	case DOCA_FLOW_PROTO_GRE:
		fmt->l4_type = DOCA_FLOW_PROTO_GRE;
		break;
	case IPPROTO_ICMP:
		fmt->l4_type = IPPROTO_ICMP;
		break;
	default:
		DOCA_LOG_INFO("Unsupported L4 %d", iphdr->next_proto_id);
		return -1;
	}
	return 0;
}

/*
 * Parse the packet tunneling info
 *
 * @pinfo [in/out]: the packet representation in the application
 * @return: 0 on success, negative value otherwise
 */
static int simple_fwd_parse_is_tun(struct simple_fwd_pkt_info *pinfo)
{
	if (pinfo->outer.l3_type != IPV4)
		return 0;

	if (pinfo->outer.l4_type == DOCA_FLOW_PROTO_GRE) {
		int optional_off = 0;
		struct rte_gre_hdr *gre_hdr = (struct rte_gre_hdr *)pinfo->outer.l4;
		if (gre_hdr->c)
			return -1;
		if (gre_hdr->k) {
			optional_off += 4;
			pinfo->tun.gre_key = *(uint32_t *)(pinfo->outer.l4 + sizeof(struct rte_gre_hdr));
			pinfo->tun.l2 = true;
		}
		if (gre_hdr->s)
			optional_off += 4;
		pinfo->tun_type = DOCA_FLOW_TUN_GRE;
		pinfo->tun.proto = gre_hdr->proto;
		return sizeof(struct rte_gre_hdr) + optional_off;
	}

	if (pinfo->outer.l4_type == DOCA_FLOW_PROTO_UDP) {
		struct rte_udp_hdr *udphdr = (struct rte_udp_hdr *)pinfo->outer.l4;
		uint8_t *udp_data = pinfo->outer.l4 + sizeof(struct rte_udp_hdr);

		switch (rte_cpu_to_be_16(udphdr->dst_port)) {
		case DOCA_FLOW_VXLAN_DEFAULT_PORT: {
			struct rte_vxlan_gpe_hdr *vxlanhdr = (struct rte_vxlan_gpe_hdr *)udp_data;

			if (vxlanhdr->vx_flags & 0x08) {
				/*need to check if this gpe*/
				pinfo->tun_type = DOCA_FLOW_TUN_VXLAN;
				pinfo->tun.vni = vxlanhdr->vx_vni;
				pinfo->tun.l2 = true;
			}
			return sizeof(struct rte_vxlan_gpe_hdr) + sizeof(struct rte_udp_hdr);
		}
		case DOCA_FLOW_GTPU_DEFAULT_PORT: {
			int off = sizeof(struct rte_gtp_hdr) + sizeof(struct rte_udp_hdr);
			struct rte_gtp_hdr *gtphdr = (struct rte_gtp_hdr *)udp_data;

			pinfo->tun_type = DOCA_FLOW_TUN_GTPU;
			pinfo->tun.teid = gtphdr->teid;
			pinfo->tun.gtp_msg_type = gtphdr->msg_type;
			pinfo->tun.gtp_flags = gtphdr->gtp_hdr_info;
			pinfo->tun.l2 = false;
			if (GTP_ESPN_FLAGS_ON(pinfo->tun.gtp_flags))
				off += 4;
			DOCA_LOG_DBG("GTP tun = %u", rte_cpu_to_be_32(pinfo->tun.teid));
			return off;
		}
		default:
			return 0;
		}
	}
	return 0;
}

int simple_fwd_parse_packet(uint8_t *data, int len, struct simple_fwd_pkt_info *pinfo)
{
	int off = 0;
	int inner_off = 0;

	if (!pinfo) {
		DOCA_LOG_ERR("Pinfo =%p", pinfo);
		return -1;
	}
	pinfo->len = len;
	if (simple_fwd_parse_pkt_format(data, len, true, &pinfo->outer))
		return -1;

	off = simple_fwd_parse_is_tun(pinfo);
	if (pinfo->tun_type == DOCA_FLOW_TUN_NONE || off < 0)
		return 0;

	switch (pinfo->tun_type) {
	case DOCA_FLOW_TUN_GRE:
		inner_off = (pinfo->outer.l4 - data) + off;
		if (simple_fwd_parse_pkt_format(data + inner_off, len - inner_off, false, &pinfo->inner))
			return -1;
		break;
	case DOCA_FLOW_TUN_VXLAN:
		inner_off = (pinfo->outer.l4 - data) + off;
		if (simple_fwd_parse_pkt_format(data + inner_off, len - inner_off, pinfo->tun.l2, &pinfo->inner))
			return -1;
		break;
	case DOCA_FLOW_TUN_GTPU:
		inner_off = (pinfo->outer.l4 - data) + off;
		if (simple_fwd_parse_pkt_format(data + inner_off, len - inner_off, pinfo->tun.l2, &pinfo->inner))
			return -1;
		break;
	default:
		break;
	}
	return 0;
}

void simple_fwd_pinfo_decap(struct simple_fwd_pkt_info *pinfo)
{
	switch (pinfo->tun_type) {
	case DOCA_FLOW_TUN_GRE:
		DOCA_LOG_ERR("Decap for GRE is not supported");
		break;
	case DOCA_FLOW_TUN_VXLAN:
		pinfo->outer.l2 = pinfo->inner.l2;
		pinfo->outer.l3 = pinfo->inner.l3;
		pinfo->outer.l4 = pinfo->inner.l4;
		pinfo->outer.l7 = pinfo->inner.l7;
		pinfo->tun_type = DOCA_FLOW_TUN_NONE;
		break;
	default:
		break;
	}
}
