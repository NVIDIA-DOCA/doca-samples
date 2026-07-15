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

#include <rte_ip_frag.h>
#include <rte_ether.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_gtp.h>
#include <rte_ip.h>

#include <doca_flow_net.h>
#include <doca_bitfield.h>
#include <doca_log.h>

#include "packet_parser.h"

DOCA_LOG_REGISTER(PACKET_PARSER);

doca_error_t link_parse(uint8_t *data, uint8_t *data_end, struct link_parser_ctx *ctx)
{
	struct rte_ether_hdr *eth;
	uint16_t next_proto;

	eth = (struct rte_ether_hdr *)data;
	if (data + sizeof(*eth) > data_end) {
		DOCA_LOG_DBG("Error parsing Ethernet header");
		return DOCA_ERROR_INVALID_VALUE;
	}

	next_proto = rte_be_to_cpu_16(eth->ether_type);
	if (next_proto != DOCA_FLOW_ETHER_TYPE_IPV4 && next_proto != DOCA_FLOW_ETHER_TYPE_IPV6) {
		DOCA_LOG_DBG("Unsupported L3 type 0x%x", eth->ether_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx->len = sizeof(*eth);
	ctx->next_proto = next_proto;
	ctx->eth = eth;
	return DOCA_SUCCESS;
}

/*
 * Parse IPv6 header and its extension headers, accumulating their total length in the process
 *
 * @data [in]: pointer to the start of the data
 * @data_end [in]: pointer to the end of the data
 * @ctx [out]: pointer to the parser context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t ipv6_hdr_parse(const uint8_t *data, const uint8_t *data_end, struct network_parser_ctx *ctx)
{
	struct rte_ipv6_hdr *ipv6_hdr = (struct rte_ipv6_hdr *)data;
	struct rte_ipv6_fragment_ext *ipv6_frag_ext = NULL;
	size_t total_len = sizeof(*ipv6_hdr);
	uint8_t next_proto = IPPROTO_NONE;
	size_t ext_len;
	int proto;

	if (data + total_len > data_end) {
		DOCA_LOG_DBG("Error parsing IPv6 header");
		return DOCA_ERROR_INVALID_VALUE;
	}
	proto = ipv6_hdr->proto;

	while (proto != -EINVAL) {
		if (proto == IPPROTO_FRAGMENT)
			ipv6_frag_ext = (struct rte_ipv6_fragment_ext *)(data + total_len);

		next_proto = proto;
		ext_len = 0;
		proto = rte_ipv6_get_next_ext(data + total_len, proto, &ext_len);
		total_len += ext_len;
		if (data + total_len > data_end) {
			DOCA_LOG_DBG("Error parsing IPv6 header with extensions");
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	ctx->next_proto = next_proto;
	ctx->ipv6.hdr = ipv6_hdr;
	if (ipv6_frag_ext) {
		ctx->frag = true;
		ctx->ipv6.frag_ext = ipv6_frag_ext;
	}
	ctx->len = total_len;

	return DOCA_SUCCESS;
}

doca_error_t network_parse(uint8_t *data, uint8_t *data_end, uint16_t expected_proto, struct network_parser_ctx *ctx)
{
	struct rte_ipv4_hdr *ipv4_hdr;
	uint16_t version;
	doca_error_t ret;
	uint8_t hdr_len;

	if (data == data_end) {
		DOCA_LOG_DBG("Error parsing IP header");
		return DOCA_ERROR_INVALID_VALUE;
	}
	version = *data >> 4;

	switch (version) {
	case 4:
		if (expected_proto && expected_proto != DOCA_FLOW_ETHER_TYPE_IPV4) {
			DOCA_LOG_DBG("Expected IPv4 header, got version 0x%x", version);
			return DOCA_ERROR_INVALID_VALUE;
		}
		ctx->ip_version = DOCA_FLOW_PROTO_IPV4;

		ipv4_hdr = (struct rte_ipv4_hdr *)data;
		if (data + sizeof(*ipv4_hdr) > data_end) {
			DOCA_LOG_DBG("Error parsing IPv4 header");
			return DOCA_ERROR_INVALID_VALUE;
		}
		hdr_len = rte_ipv4_hdr_len(ipv4_hdr);
		if (data + hdr_len > data_end) {
			DOCA_LOG_DBG("Error parsing IPv4 header options");
			return DOCA_ERROR_INVALID_VALUE;
		}

		ctx->len = hdr_len;
		ctx->next_proto = ipv4_hdr->next_proto_id;
		ctx->frag = rte_ipv4_frag_pkt_is_fragmented(ipv4_hdr);
		ctx->ipv4_hdr = ipv4_hdr;
		break;
	case 6:
		if (expected_proto && expected_proto != DOCA_FLOW_ETHER_TYPE_IPV6) {
			DOCA_LOG_DBG("Expected IPv6 header, got version 0x%x", version);
			return DOCA_ERROR_INVALID_VALUE;
		}
		ctx->ip_version = DOCA_FLOW_PROTO_IPV6;

		ret = ipv6_hdr_parse(data, data_end, ctx);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_DBG("Error parsing IPv6 header");
			return DOCA_ERROR_INVALID_VALUE;
		}
		break;
	default:
		DOCA_LOG_DBG("Unsupported IP version 0x%x", version);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

doca_error_t transport_parse(uint8_t *data, uint8_t *data_end, uint8_t proto, struct transport_parser_ctx *ctx)
{
	struct rte_tcp_hdr *tcp_hdr;
	struct rte_udp_hdr *udp_hdr;
	uint8_t hdr_len;

	switch (proto) {
	case DOCA_FLOW_PROTO_TCP:
		tcp_hdr = (struct rte_tcp_hdr *)data;

		if (data + sizeof(*tcp_hdr) > data_end) {
			DOCA_LOG_DBG("Error parsing TCP header");
			return DOCA_ERROR_INVALID_VALUE;
		}
		hdr_len = rte_tcp_hdr_len(tcp_hdr);
		if (data + hdr_len > data_end) {
			DOCA_LOG_DBG("Error parsing TCP options");
			return DOCA_ERROR_INVALID_VALUE;
		}

		ctx->len = hdr_len;
		ctx->tcp_hdr = tcp_hdr;
		break;
	case DOCA_FLOW_PROTO_UDP:
		udp_hdr = (struct rte_udp_hdr *)data;

		if (data + sizeof(*udp_hdr) > data_end) {
			DOCA_LOG_DBG("Error parsing UDP header");
			return DOCA_ERROR_INVALID_VALUE;
		}

		ctx->len = sizeof(*udp_hdr);
		ctx->udp_hdr = udp_hdr;
		break;
	default:
		DOCA_LOG_WARN("Unsupported L4 %d", proto);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	ctx->proto = proto;
	return DOCA_SUCCESS;
}

doca_error_t gtpu_parse(uint8_t *data, uint8_t *data_end, struct gtp_parser_ctx *ctx)
{
	struct rte_gtp_psc_type0_hdr *gtpext_hdr = NULL;
	struct rte_gtp_hdr_ext_word *gtpopt_hdr = NULL;
	struct rte_gtp_hdr *gtp_hdr;
	uint8_t *data_beg = data;
	size_t gtpext_len;

	gtp_hdr = (struct rte_gtp_hdr *)data;
	if (data + sizeof(*gtp_hdr) > data_end) {
		DOCA_LOG_DBG("Error parsing GTPU header");
		return DOCA_ERROR_INVALID_VALUE;
	}
	data += sizeof(*gtp_hdr);

	if (gtp_hdr->ver != 1) {
		DOCA_LOG_WARN("Unsupported GTPU version %hhu", gtp_hdr->ver);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (gtp_hdr->e || gtp_hdr->s || gtp_hdr->pn) {
		gtpopt_hdr = (struct rte_gtp_hdr_ext_word *)data;
		if (data + sizeof(*gtpopt_hdr) > data_end) {
			DOCA_LOG_DBG("Error parsing GTPU option header");
			return DOCA_ERROR_INVALID_VALUE;
		}
		data += sizeof(*gtpopt_hdr);

		if (gtp_hdr->e) {
			/* Only support single fixed 1-word sized 0x85-type extension */
			if (gtpopt_hdr->next_ext != 0x85) {
				DOCA_LOG_WARN("Unsupported GTPU extension %hhu", gtpopt_hdr->next_ext);
				return DOCA_ERROR_NOT_SUPPORTED;
			}

			gtpext_hdr = (struct rte_gtp_psc_type0_hdr *)data;
			/* +1 due to dynamic array at the end of the struct */
			gtpext_len = sizeof(*gtpext_hdr) + 1;
			if (data + gtpext_len > data_end) {
				DOCA_LOG_DBG("Error parsing GTPU extension header");
				return DOCA_ERROR_INVALID_VALUE;
			}
			data += gtpext_len;

			if (gtpext_hdr->ext_hdr_len != 1) {
				DOCA_LOG_WARN("GTPU extension sizes more than 1 word are not supported %u",
					      gtpext_hdr->ext_hdr_len != 1);
				return DOCA_ERROR_NOT_SUPPORTED;
			}

			if (gtpext_hdr->data[0]) {
				DOCA_LOG_WARN("Chained GTPU extensions are not supported, type %hhu",
					      gtpext_hdr->data[0]);
				return DOCA_ERROR_NOT_SUPPORTED;
			}
		}
	}

	ctx->len = data - data_beg;
	ctx->gtp_hdr = gtp_hdr;
	ctx->opt_hdr = gtpopt_hdr;
	ctx->ext_hdr = gtpext_hdr;
	return DOCA_SUCCESS;
}

doca_error_t conn_parse(uint8_t *data, uint8_t *data_end, struct conn_parser_ctx *ctx)
{
	doca_error_t ret;

	ret = network_parse(data + ctx->len, data_end, ctx->link_ctx.next_proto, &ctx->network_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->network_ctx.len;
	if (ctx->network_ctx.frag)
		/* Parse again after the defragmentation */
		return DOCA_ERROR_AGAIN;

	ret = transport_parse(data + ctx->len, data_end, ctx->network_ctx.next_proto, &ctx->transport_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->transport_ctx.len;

	return DOCA_SUCCESS;
}

doca_error_t plain_parse(uint8_t *data, uint8_t *data_end, struct conn_parser_ctx *ctx)
{
	doca_error_t ret;

	ret = link_parse(data + ctx->len, data_end, &ctx->link_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->link_ctx.len;

	return conn_parse(data, data_end, ctx);
}

doca_error_t tunnel_parse(uint8_t *data, uint8_t *data_end, struct tun_parser_ctx *ctx)
{
	doca_error_t ret;

	ret = link_parse(data + ctx->len, data_end, &ctx->link_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->link_ctx.len;

	ret = network_parse(data + ctx->len, data_end, ctx->link_ctx.next_proto, &ctx->network_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->network_ctx.len;
	if (ctx->network_ctx.frag)
		/* Parse again after the defragmentation */
		return DOCA_ERROR_AGAIN;

	ret = transport_parse(data + ctx->len, data_end, ctx->network_ctx.next_proto, &ctx->transport_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->transport_ctx.len;

	ret = gtpu_parse(data + ctx->len, data_end, &ctx->gtp_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->gtp_ctx.len;

	ret = conn_parse(data + ctx->len, data_end, &ctx->inner);
	if (ret == DOCA_SUCCESS || ret == DOCA_ERROR_AGAIN)
		ctx->len += ctx->inner.len;
	return ret;
}

doca_error_t unknown_parse(uint8_t *data,
			   uint8_t *data_end,
			   struct tun_parser_ctx *ctx,
			   enum parser_pkt_type *parser_pkt_type)
{
	struct transport_parser_ctx transport_ctx = {0};
	struct network_parser_ctx network_ctx = {0};
	struct link_parser_ctx link_ctx = {0};
	doca_error_t ret;
	size_t len = 0;

	ret = link_parse(data + len, data_end, &link_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	len += link_ctx.len;

	ret = network_parse(data + len, data_end, link_ctx.next_proto, &network_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	len += network_ctx.len;
	/* Parse again after the defragmentation */
	if (network_ctx.frag) {
		/*
		 * We can't determine whether a fragmented packet is tunnel-encapsulated so we treat it as incoming from
		 * non-encapsulated until reparse.
		 */
		*parser_pkt_type = PARSER_PKT_TYPE_PLAIN;
		ctx->inner.link_ctx = link_ctx;
		ctx->inner.network_ctx = network_ctx;
		ctx->inner.len += len;
		ctx->len += len;
		return DOCA_ERROR_AGAIN;
	}

	ret = transport_parse(data + len, data_end, network_ctx.next_proto, &transport_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	len += transport_ctx.len;
	if (transport_ctx.proto != DOCA_FLOW_PROTO_UDP ||
	    transport_ctx.udp_hdr->dst_port != DOCA_HTOBE16(DOCA_FLOW_GTPU_DEFAULT_PORT)) {
		*parser_pkt_type = PARSER_PKT_TYPE_PLAIN;
		ctx->inner.link_ctx = link_ctx;
		ctx->inner.network_ctx = network_ctx;
		ctx->inner.transport_ctx = transport_ctx;
		ctx->inner.len += len;
		ctx->len += len;
		return DOCA_SUCCESS;
	}

	*parser_pkt_type = PARSER_PKT_TYPE_TUNNELED;
	ctx->link_ctx = link_ctx;
	ctx->network_ctx = network_ctx;
	ctx->transport_ctx = transport_ctx;
	ctx->len += len;

	ret = gtpu_parse(data + ctx->len, data_end, &ctx->gtp_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;
	ctx->len += ctx->gtp_ctx.len;

	ret = conn_parse(data + ctx->len, data_end, &ctx->inner);
	if (ret == DOCA_SUCCESS || ret == DOCA_ERROR_AGAIN)
		ctx->len += ctx->inner.len;
	return ret;
}
