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

#include "ip_frag_dp.h"
#include <flow_common.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_dpdk.h>
#include <dpdk_utils.h>
#include <packet_parser.h>

#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>
#include <rte_ip_frag.h>
#include <rte_cycles.h>
#include <rte_mempool.h>

#include <stdbool.h>

#define IP_FRAG_MAX_PKT_BURST 32
#define IP_FRAG_BURST_PREFETCH (IP_FRAG_MAX_PKT_BURST / 8)
#define IP_FRAG_FLUSH_THRESHOLD 16

#define IP_FRAG_TBL_BUCKET_SIZE 4

DOCA_LOG_REGISTER(IP_FRAG::DP);

struct ip_frag_sw_counters {
	uint64_t frags_rx;	/* Fragments received that need reassembly */
	uint64_t whole;		/* Whole packets (either reassembled or not fragmented) */
	uint64_t mtu_fits_rx;	/* Packets received that are within the MTU size and dont require fragmentation */
	uint64_t mtu_exceed_rx; /* Packets received that exceed the MTU size and require fragmentation */
	uint64_t frags_gen;	/* Fragments generated from packets that exceed the MTU size */
	uint64_t err;		/* Errors */
};

struct ip_frag_wt_data {
	const struct ip_frag_config *cfg;			  /* Application config */
	uint16_t queue_id;					  /* Queue id */
	struct ip_frag_sw_counters sw_counters[IP_FRAG_PORT_NUM]; /* SW counters */
	struct rte_eth_dev_tx_buffer *tx_buffer;		  /* TX buffer */
	uint64_t tx_buffer_err;					  /* TX buffer error counter */
	struct rte_ip_frag_tbl *frag_tbl;			  /* Fragmentation table */
	struct rte_mempool *indirect_pool;			  /* Indirect memory pool */
	struct rte_ip_frag_death_row death_row;			  /* Fragmentation table expired fragments death row */
} __rte_aligned(RTE_CACHE_LINE_SIZE);

bool force_stop = false;

/*
 * Drop the packet and increase error counters.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: incoming packet port id
 * @pkt [in]: packet to drop
 */
static void ip_frag_pkt_err_drop(struct ip_frag_wt_data *wt_data, uint16_t rx_port_id, struct rte_mbuf *pkt)
{
	wt_data->sw_counters[rx_port_id].err++;
	rte_pktmbuf_free(pkt);
}

/*
 * Parse the packet.
 *
 * @pkt_type [out]: incoming packet type
 * @pkt [in]: pointer to the pkt
 * @parse_ctx [out]: pointer to the parser context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_pkt_parse(enum parser_pkt_type *pkt_type,
				      struct rte_mbuf *pkt,
				      struct tun_parser_ctx *parse_ctx)
{
	uint8_t *data_beg = rte_pktmbuf_mtod(pkt, uint8_t *);
	uint8_t *data_end = data_beg + rte_pktmbuf_data_len(pkt);

	switch (*pkt_type) {
	case PARSER_PKT_TYPE_TUNNELED:
		return tunnel_parse(data_beg, data_end, parse_ctx);
	case PARSER_PKT_TYPE_PLAIN:
		return plain_parse(data_beg, data_end, &parse_ctx->inner);
	case PARSER_PKT_TYPE_UNKNOWN:
		return unknown_parse(data_beg, data_end, parse_ctx, pkt_type);
	default:
		assert(0);
		return DOCA_ERROR_INVALID_VALUE;
	}
}

/*
 * Prepare packet mbuf for reassembly.
 *
 * @pkt [in]: packet
 * @l2_len [in]: L2 header length
 * @l3_len [in]: L3 header length
 * @flags [in]: mbuf flags to set
 */
static void ip_frag_pkt_reassemble_prepare(struct rte_mbuf *pkt, size_t l2_len, size_t l3_len, uint64_t flags)
{
	pkt->l2_len = l2_len;
	pkt->l3_len = l3_len;
	pkt->ol_flags |= flags;
}

/*
 * Calculate and set IPv4 header checksum
 *
 * @hdr [in]: IPv4 header
 */
static void ip_frag_ipv4_hdr_cksum(struct rte_ipv4_hdr *hdr)
{
	hdr->hdr_checksum = 0;
	hdr->hdr_checksum = rte_ipv4_cksum(hdr);
}

/*
 * Calculate and set any required network-layer checksums for the parsed headers
 *
 * @ctx [in]: pointer to the parser network-layer context
 */
static void ip_frag_network_cksum(struct network_parser_ctx *ctx)
{
	if (ctx->ip_version == DOCA_FLOW_PROTO_IPV4)
		ip_frag_ipv4_hdr_cksum(ctx->ipv4_hdr);
}

/*
 * Calculate and set any required network-layer checksums for the headers
 *
 * @wt_data [in]: worker thread data
 * @pkt [in]: packet
 * @l2_len [in]: length of L2 header
 * @l3_len [in]: length of L3 header
 * @ipv4_hdr [in]: header to calculate the checksum for
 */
static void ip_frag_ipv4_cksum_handle(struct ip_frag_wt_data *wt_data,
				      struct rte_mbuf *pkt,
				      uint64_t l2_len,
				      uint64_t l3_len,
				      struct rte_ipv4_hdr *ipv4_hdr)
{
	if (wt_data->cfg->hw_cksum) {
		pkt->l2_len = l2_len;
		pkt->l3_len = l3_len;
		pkt->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
	} else {
		ip_frag_ipv4_hdr_cksum(ipv4_hdr);
	}
}

/*
 * Calculate and set any required network-layer checksums for the parsed headers
 *
 * @wt_data [in]: worker thread data
 * @pkt [in]: packet
 * @link_ctx [in]: pointer to the parser link-layer context
 * @network_ctx [in]: pointer to the parser network-layer context
 */
static void ip_frag_network_cksum_handle(struct ip_frag_wt_data *wt_data,
					 struct rte_mbuf *pkt,
					 struct link_parser_ctx *link_ctx,
					 struct network_parser_ctx *network_ctx)
{
	if (network_ctx->ip_version != DOCA_FLOW_PROTO_IPV4)
		return;
	ip_frag_ipv4_cksum_handle(wt_data, pkt, link_ctx->len, network_ctx->len, network_ctx->ipv4_hdr);
}

/*
 * Handle UDP checksum
 *
 * @wt_data [in]: worker thread data
 * @pkt [in]: packet
 * @link_ctx [in]: pointer to the parser link-layer context
 * @network_ctx [in]: pointer to the parser network-layer context
 * @transport_ctx [in]: pointer to the parser transport-layer context
 */
static void ip_frag_udp_cksum_handle(struct ip_frag_wt_data *wt_data,
				     struct rte_mbuf *pkt,
				     struct link_parser_ctx *link_ctx,
				     struct network_parser_ctx *network_ctx,
				     struct transport_parser_ctx *transport_ctx)
{
	if (network_ctx->ip_version == DOCA_FLOW_PROTO_IPV4) {
		/* UDP checksum is optional according to the spec */
		transport_ctx->udp_hdr->dgram_cksum = 0;
	} else {
		if (wt_data->cfg->hw_cksum) {
			pkt->l2_len = link_ctx->len;
			pkt->l3_len = network_ctx->len;
			transport_ctx->udp_hdr->dgram_cksum = rte_ipv6_phdr_cksum(network_ctx->ipv6.hdr, pkt->ol_flags);
			pkt->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_UDP_CKSUM;
		} else {
			/* UDP checksum can be omitted for tunnel headers IETF RFC 6935 */
			transport_ctx->udp_hdr->dgram_cksum = 0;
		}
	}
}

/*
 * Fixup the packet headers after reassembly. This involves fixing any length fields that may have become outdated and
 * recalculating the necessary checksums (potentially zeroing them out when allowed by the spec)
 *
 * @wt_data [in]: worker thread data
 * @pkt_type [in]: incoming packet type
 * @pkt [in]: packet
 * @parse_ctx [in]: pointer to the parser context
 */
static void ip_frag_pkt_fixup(struct ip_frag_wt_data *wt_data,
			      enum parser_pkt_type pkt_type,
			      struct rte_mbuf *pkt,
			      struct tun_parser_ctx *parse_ctx)
{
	assert(pkt_type == PARSER_PKT_TYPE_TUNNELED || pkt_type == PARSER_PKT_TYPE_PLAIN);

	if (pkt->ol_flags & wt_data->cfg->mbuf_flag_inner_modified) {
		if (pkt_type == PARSER_PKT_TYPE_TUNNELED) {
			/* Payload has been modified, need to fix the encapsulation accordingly going from inner to
			 * outer protocols since changing inner data may affect outer's checksums. Fix GTPU payload
			 * length first (which includes optional fields)... */
			parse_ctx->gtp_ctx.gtp_hdr->plen =
				rte_cpu_to_be_16(rte_pktmbuf_pkt_len(pkt) -
						 (parse_ctx->link_ctx.len + parse_ctx->network_ctx.len +
						  parse_ctx->transport_ctx.len + sizeof(*parse_ctx->gtp_ctx.gtp_hdr)));

			/* ...then fix UDP total length... */
			parse_ctx->transport_ctx.udp_hdr->dgram_len = rte_cpu_to_be_16(
				rte_pktmbuf_pkt_len(pkt) - (parse_ctx->link_ctx.len + parse_ctx->network_ctx.len));

			if (parse_ctx->network_ctx.ip_version == DOCA_FLOW_PROTO_IPV4) {
				/* ...and fix the IP total length which requires recalculating header checksum in case
				 * of IPv4... */
				parse_ctx->network_ctx.ipv4_hdr->total_length =
					rte_cpu_to_be_16(rte_pktmbuf_pkt_len(pkt) - parse_ctx->link_ctx.len);
				ip_frag_network_cksum_handle(wt_data,
							     pkt,
							     &parse_ctx->link_ctx,
							     &parse_ctx->network_ctx);
			} else {
				/* ...or just IP payload length field in case of IPv6.. */
				parse_ctx->network_ctx.ipv6.hdr->payload_len =
					rte_cpu_to_be_16(rte_pktmbuf_pkt_len(pkt) - parse_ctx->link_ctx.len -
							 sizeof(*parse_ctx->network_ctx.ipv6.hdr));
			}

			/*  ...either recalculate or zero-out encapsulation UDP checksum... */
			ip_frag_udp_cksum_handle(wt_data,
						 pkt,
						 &parse_ctx->link_ctx,
						 &parse_ctx->network_ctx,
						 &parse_ctx->transport_ctx);

			/* ...fix payload network-level header checksum, if necessary */
			ip_frag_network_cksum(&parse_ctx->inner.network_ctx);
		} else {
			ip_frag_network_cksum_handle(wt_data,
						     pkt,
						     &parse_ctx->inner.link_ctx,
						     &parse_ctx->inner.network_ctx);
		}
	} else if (pkt->ol_flags & wt_data->cfg->mbuf_flag_outer_modified) {
		ip_frag_network_cksum_handle(wt_data, pkt, &parse_ctx->link_ctx, &parse_ctx->network_ctx);
	}
}

/*
 * Flatten chained mbuf to a single contiguous mbuf segment
 *
 * @pkt [in]: chained mbuf head
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_pkt_flatten(struct rte_mbuf *pkt)
{
	uint16_t tail_size = rte_pktmbuf_pkt_len(pkt) - rte_pktmbuf_data_len(pkt);
	struct rte_mbuf *tail = pkt->next;
	struct rte_mbuf *tmp;
	uint16_t seg_len;
	char *dst;

	if (tail_size > rte_pktmbuf_tailroom(pkt)) {
		DOCA_LOG_DBG("Resulting packet size %u doesn't fit into tailroom size %u",
			     tail_size,
			     rte_pktmbuf_tailroom(pkt));
		return DOCA_ERROR_TOO_BIG;
	}

	pkt->next = NULL;
	pkt->nb_segs = 1;
	pkt->pkt_len = pkt->data_len;

	while (tail) {
		seg_len = rte_pktmbuf_data_len(tail);
		dst = rte_pktmbuf_append(pkt, seg_len);
		assert(dst); /* already verified that tail fits */
		memcpy(dst, rte_pktmbuf_mtod(tail, void *), seg_len);

		tmp = tail->next;
		rte_pktmbuf_free_seg(tail);
		tail = tmp;
	}

	return DOCA_SUCCESS;
}

/*
 * Push a packet with fragmented outer IP header to the frag table.
 *
 * @wt_data [in]: worker thread data
 * @pkt [in]: packet
 * @parse_ctx [in]: pointer to the parser context
 * @rx_ts [in]: burst reception timestamp
 * @return: fully reassembled packet or NULL pointer if more frags are expected
 */
static struct rte_mbuf *ip_frag_pkt_reassemble_push_outer(struct ip_frag_wt_data *wt_data,
							  struct rte_mbuf *pkt,
							  struct tun_parser_ctx *parse_ctx,
							  uint64_t rx_ts)
{
	ip_frag_pkt_reassemble_prepare(pkt,
				       parse_ctx->link_ctx.len,
				       parse_ctx->network_ctx.len,
				       wt_data->cfg->mbuf_flag_outer_modified);

	return parse_ctx->network_ctx.ip_version == DOCA_FLOW_PROTO_IPV4 ?
		       rte_ipv4_frag_reassemble_packet(wt_data->frag_tbl,
						       &wt_data->death_row,
						       pkt,
						       rx_ts,
						       parse_ctx->network_ctx.ipv4_hdr) :
		       rte_ipv6_frag_reassemble_packet(wt_data->frag_tbl,
						       &wt_data->death_row,
						       pkt,
						       rx_ts,
						       parse_ctx->network_ctx.ipv6.hdr,
						       parse_ctx->network_ctx.ipv6.frag_ext);
}

/*
 * Push a packet with fragmented inner IP header to the frag table.
 *
 * @wt_data [in]: worker thread data
 * @pkt_type [in]: incoming packet type
 * @pkt [in]: packet
 * @parse_ctx [in]: pointer to the parser context
 * @rx_ts [in]: burst reception timestamp
 * @return: fully reassembled packet or NULL pointer if more frags are expected
 */
static struct rte_mbuf *ip_frag_pkt_reassemble_push_inner(struct ip_frag_wt_data *wt_data,
							  enum parser_pkt_type pkt_type,
							  struct rte_mbuf *pkt,
							  struct tun_parser_ctx *parse_ctx,
							  uint64_t rx_ts)
{
	ip_frag_pkt_reassemble_prepare(pkt,
				       pkt_type == PARSER_PKT_TYPE_PLAIN    ? parse_ctx->inner.link_ctx.len :
									      /* For tunneled packets we treat the
										 whole encapsulation as  L2 for the
										 purpose of reassembly library. */
									      parse_ctx->len - parse_ctx->inner.len,
				       parse_ctx->inner.network_ctx.len,
				       wt_data->cfg->mbuf_flag_inner_modified);

	return parse_ctx->inner.network_ctx.ip_version == DOCA_FLOW_PROTO_IPV4 ?
		       rte_ipv4_frag_reassemble_packet(wt_data->frag_tbl,
						       &wt_data->death_row,
						       pkt,
						       rx_ts,
						       parse_ctx->inner.network_ctx.ipv4_hdr) :
		       rte_ipv6_frag_reassemble_packet(wt_data->frag_tbl,
						       &wt_data->death_row,
						       pkt,
						       rx_ts,
						       parse_ctx->inner.network_ctx.ipv6.hdr,
						       parse_ctx->inner.network_ctx.ipv6.frag_ext);
}
/*
 * Set necessary mbuf fields and push the packet to the frag table.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @pkt_type [in]: incoming packet type
 * @pkt [in]: packet
 * @parse_ctx [in]: pointer to the parser context
 * @rx_ts [in]: burst reception timestamp
 * @whole_pkt [out]: resulting reassembled packet
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_pkt_reassemble_push(struct ip_frag_wt_data *wt_data,
						uint16_t rx_port_id,
						enum parser_pkt_type pkt_type,
						struct rte_mbuf *pkt,
						struct tun_parser_ctx *parse_ctx,
						uint64_t rx_ts,
						struct rte_mbuf **whole_pkt)
{
	struct rte_mbuf *res;
	doca_error_t ret;

	assert(pkt_type == PARSER_PKT_TYPE_TUNNELED || pkt_type == PARSER_PKT_TYPE_PLAIN);

	if (parse_ctx->network_ctx.frag)
		res = ip_frag_pkt_reassemble_push_outer(wt_data, pkt, parse_ctx, rx_ts);
	else if (parse_ctx->inner.network_ctx.frag)
		res = ip_frag_pkt_reassemble_push_inner(wt_data, pkt_type, pkt, parse_ctx, rx_ts);
	else
		return DOCA_ERROR_INVALID_VALUE;

	if (!res)
		return DOCA_ERROR_AGAIN;

	if (!wt_data->cfg->mbuf_chain && !rte_pktmbuf_is_contiguous(res)) {
		ret = ip_frag_pkt_flatten(res);
		if (ret != DOCA_SUCCESS) {
			ip_frag_pkt_err_drop(wt_data, rx_port_id, pkt);
			return ret;
		}
	}

	*whole_pkt = res;
	return DOCA_SUCCESS;
}

/*
 * Reassemble the packet.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: outgoing packet port id
 * @pkt_type [in]: incoming packet type
 * @pkt [in]: packet
 * @rx_ts [in]: burst reception timestamp
 */
static void ip_frag_pkt_reassemble(struct ip_frag_wt_data *wt_data,
				   uint16_t rx_port_id,
				   uint16_t tx_port_id,
				   enum parser_pkt_type pkt_type,
				   struct rte_mbuf *pkt,
				   uint64_t rx_ts)
{
	struct tun_parser_ctx parse_ctx;
	enum parser_pkt_type inferred_pkt_type;
	doca_error_t ret;
	bool reparse;

	do {
		reparse = false;
		/* For fragmented packet parser can't correctly deduce rx type on the first pass so reset it on
		 * each reparse iteration. */
		inferred_pkt_type = pkt_type;
		memset(&parse_ctx, 0, sizeof(parse_ctx));

		ret = ip_frag_pkt_parse(&inferred_pkt_type, pkt, &parse_ctx);
		switch (ret) {
		case DOCA_SUCCESS:
			wt_data->sw_counters[rx_port_id].whole++;
			ip_frag_pkt_fixup(wt_data, inferred_pkt_type, pkt, &parse_ctx);
			rte_eth_tx_buffer(tx_port_id, wt_data->queue_id, wt_data->tx_buffer, pkt);
			break;

		case DOCA_ERROR_AGAIN:
			wt_data->sw_counters[rx_port_id].frags_rx++;
			ret = ip_frag_pkt_reassemble_push(wt_data,
							  rx_port_id,
							  inferred_pkt_type,
							  pkt,
							  &parse_ctx,
							  rx_ts,
							  &pkt);
			if (ret == DOCA_SUCCESS) {
				reparse = true;
			} else if (ret != DOCA_ERROR_AGAIN) {
				DOCA_LOG_ERR("Unexpected packet fragmentation");
				ip_frag_pkt_err_drop(wt_data, rx_port_id, pkt);
			}
			break;

		default:
			ip_frag_pkt_err_drop(wt_data, rx_port_id, pkt);
			DOCA_LOG_DBG("Failed to parse packet status %u", ret);
			break;
		}
	} while (reparse);
}

/*
 * Reassemble the packet burst buffering any resulting packets.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: send port id
 * @pkt_type [in]: incoming packet type
 * @pkts [in]: packet burst
 * @pkts_cnt [in]: number of packets in the burst
 * @rx_ts [in]: burst reception timestamp
 */
static void ip_frag_pkts_reassemble(struct ip_frag_wt_data *wt_data,
				    uint16_t rx_port_id,
				    uint16_t tx_port_id,
				    enum parser_pkt_type pkt_type,
				    struct rte_mbuf *pkts[],
				    int pkts_cnt,
				    uint64_t rx_ts)
{
	int i; /* prefetch calculation can yield negative values */

	for (i = 0; i < IP_FRAG_BURST_PREFETCH && i < pkts_cnt; i++)
		rte_prefetch0(rte_pktmbuf_mtod(pkts[i], void *));

	for (i = 0; i < (pkts_cnt - IP_FRAG_BURST_PREFETCH); i++) {
		rte_prefetch0(rte_pktmbuf_mtod(pkts[i + IP_FRAG_BURST_PREFETCH], void *));
		ip_frag_pkt_reassemble(wt_data, rx_port_id, tx_port_id, pkt_type, pkts[i], rx_ts);
	}

	for (; i < pkts_cnt; i++)
		ip_frag_pkt_reassemble(wt_data, rx_port_id, tx_port_id, pkt_type, pkts[i], rx_ts);
}

/*
 * Receive a burst of packets on rx port, reassemble any fragments and send resulting packets on the tx port.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: send port id
 * @pkt_type [in]: incoming packet type
 */
static void ip_frag_wt_reassemble(struct ip_frag_wt_data *wt_data,
				  uint16_t rx_port_id,
				  uint16_t tx_port_id,
				  enum parser_pkt_type pkt_type)
{
	struct rte_mbuf *pkts[IP_FRAG_MAX_PKT_BURST];
	uint16_t pkts_cnt;

	pkts_cnt = rte_eth_rx_burst(rx_port_id, wt_data->queue_id, pkts, IP_FRAG_MAX_PKT_BURST);
	if (likely(pkts_cnt)) {
		ip_frag_pkts_reassemble(wt_data, rx_port_id, tx_port_id, pkt_type, pkts, pkts_cnt, rte_rdtsc());
		rte_eth_tx_buffer_flush(tx_port_id, wt_data->queue_id, wt_data->tx_buffer);
	} else {
		rte_ip_frag_table_del_expired_entries(wt_data->frag_tbl, &wt_data->death_row, rte_rdtsc());
	}
	rte_ip_frag_free_death_row(&wt_data->death_row, IP_FRAG_BURST_PREFETCH);
}

static int32_t ip_frag_mbuf_fragment(struct ip_frag_wt_data *wt_data,
				     struct conn_parser_ctx *parse_ctx,
				     struct rte_mbuf *pkt_in,
				     struct rte_mbuf **pkts_out,
				     uint16_t pkts_out_max,
				     uint16_t mtu,
				     struct rte_mempool *direct_pool,
				     struct rte_mempool *indirect_pool)
{
	if (parse_ctx->network_ctx.ip_version == DOCA_FLOW_PROTO_IPV4)
		return wt_data->cfg->mbuf_chain ?
			       rte_ipv4_fragment_packet(pkt_in, pkts_out, pkts_out_max, mtu, direct_pool, indirect_pool) :
			       rte_ipv4_fragment_copy_nonseg_packet(pkt_in, pkts_out, pkts_out_max, mtu, direct_pool);
	else
		return wt_data->cfg->mbuf_chain ? rte_ipv6_fragment_packet(pkt_in,
									   pkts_out,
									   pkts_out_max,
									   mtu,
									   direct_pool,
									   indirect_pool) :
						  -EOPNOTSUPP;
}

/*
 * Fragment the packet, if necessary, and buffer resulting packets.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: outgoing packet port id
 * @pkt [in]: packet
 */
static void ip_frag_pkt_fragment(struct ip_frag_wt_data *wt_data,
				 uint16_t rx_port_id,
				 uint16_t tx_port_id,
				 struct rte_mbuf *pkt)
{
	struct rte_eth_dev_tx_buffer *tx_buffer = wt_data->tx_buffer;
	uint8_t eth_hdr_copy[RTE_PKTMBUF_HEADROOM];
	struct conn_parser_ctx parse_ctx;
	size_t eth_hdr_len;
	void *eth_hdr_new;
	doca_error_t ret;
	int num_frags;
	int i;

	memset(&parse_ctx, 0, sizeof(parse_ctx));
	/* We only fragment the outer header and don't care about parsing encapsulation, so always treat the packet as
	 * non-encapsulated. */
	ret = plain_parse(rte_pktmbuf_mtod(pkt, uint8_t *),
			  rte_pktmbuf_mtod(pkt, uint8_t *) + rte_pktmbuf_data_len(pkt),
			  &parse_ctx);
	if (ret != DOCA_SUCCESS) {
		ip_frag_pkt_err_drop(wt_data, rx_port_id, pkt);
		DOCA_LOG_DBG("Failed to parse packet status %u", ret);
		return;
	}

	if (rte_pktmbuf_pkt_len(pkt) <= wt_data->cfg->mtu) {
		wt_data->sw_counters[rx_port_id].mtu_fits_rx++;
		rte_eth_tx_buffer(tx_port_id, wt_data->queue_id, tx_buffer, pkt);
		return;
	}

	wt_data->sw_counters[rx_port_id].mtu_exceed_rx++;
	eth_hdr_len = parse_ctx.link_ctx.len;
	if (sizeof(eth_hdr_copy) < eth_hdr_len) {
		ip_frag_pkt_err_drop(wt_data, rx_port_id, pkt);
		DOCA_LOG_ERR("Ethernet header size %lu too big", eth_hdr_len);
		return;
	}
	memcpy(eth_hdr_copy, parse_ctx.link_ctx.eth, eth_hdr_len);
	rte_pktmbuf_adj(pkt, eth_hdr_len);

	num_frags = ip_frag_mbuf_fragment(wt_data,
					  &parse_ctx,
					  pkt,
					  &tx_buffer->pkts[tx_buffer->length],
					  tx_buffer->size - tx_buffer->length,
					  wt_data->cfg->mtu - eth_hdr_len,
					  pkt->pool,
					  wt_data->indirect_pool);
	if (num_frags < 0) {
		ip_frag_pkt_err_drop(wt_data, rx_port_id, pkt);
		DOCA_LOG_ERR("RTE fragmentation failed with code: %d", -num_frags);
		return;
	}
	rte_pktmbuf_free(pkt);

	for (i = tx_buffer->length; i < tx_buffer->length + num_frags; i++) {
		pkt = tx_buffer->pkts[i];
		if (parse_ctx.network_ctx.ip_version == DOCA_FLOW_PROTO_IPV4)
			ip_frag_ipv4_cksum_handle(wt_data,
						  pkt,
						  eth_hdr_len,
						  rte_ipv4_hdr_len(rte_pktmbuf_mtod(pkt, struct rte_ipv4_hdr *)),
						  rte_pktmbuf_mtod(pkt, struct rte_ipv4_hdr *));

		eth_hdr_new = rte_pktmbuf_prepend(pkt, eth_hdr_len);
		assert(eth_hdr_new);
		memcpy(eth_hdr_new, eth_hdr_copy, eth_hdr_len);
	}

	tx_buffer->length += num_frags;
	wt_data->sw_counters[rx_port_id].frags_gen += num_frags;
}

/*
 * Fragment the packet burst buffering any resulting packets. Flush the tx buffer when it gets path threshold packets.
 * Flush the tx buffer when it gets path threshold packets.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: outgoing packet port id
 * @pkts [in]: packet burst
 * @pkts_cnt [in]: number of packets in the burst
 */
static void ip_frag_pkts_fragment(struct ip_frag_wt_data *wt_data,
				  uint16_t rx_port_id,
				  uint16_t tx_port_id,
				  struct rte_mbuf *pkts[],
				  int pkts_cnt)
{
	struct rte_eth_dev_tx_buffer *tx_buffer = wt_data->tx_buffer;
	int i; /* prefetch calculation can yield negative values */

	for (i = 0; i < IP_FRAG_BURST_PREFETCH && i < pkts_cnt; i++)
		rte_prefetch0(rte_pktmbuf_mtod(pkts[i], void *));

	for (i = 0; i < (pkts_cnt - IP_FRAG_BURST_PREFETCH); i++) {
		rte_prefetch0(rte_pktmbuf_mtod(pkts[i + IP_FRAG_BURST_PREFETCH], void *));
		ip_frag_pkt_fragment(wt_data, rx_port_id, tx_port_id, pkts[i]);
		if (tx_buffer->size - tx_buffer->length < IP_FRAG_FLUSH_THRESHOLD)
			rte_eth_tx_buffer_flush(tx_port_id, wt_data->queue_id, tx_buffer);
	}

	for (; i < pkts_cnt; i++) {
		ip_frag_pkt_fragment(wt_data, rx_port_id, tx_port_id, pkts[i]);
		if (tx_buffer->size - tx_buffer->length < IP_FRAG_FLUSH_THRESHOLD)
			rte_eth_tx_buffer_flush(tx_port_id, wt_data->queue_id, tx_buffer);
	}
}

/*
 * Receive a burst of packets on rx port, fragment any larger than MTU, and send resulting packets on the tx port.
 *
 * @wt_data [in]: worker thread data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: send port id
 */
static void ip_frag_wt_fragment(struct ip_frag_wt_data *wt_data, uint16_t rx_port_id, uint16_t tx_port_id)
{
	struct rte_mbuf *pkts[IP_FRAG_MAX_PKT_BURST];
	uint16_t pkts_cnt;

	pkts_cnt = rte_eth_rx_burst(rx_port_id, wt_data->queue_id, pkts, IP_FRAG_MAX_PKT_BURST);
	ip_frag_pkts_fragment(wt_data, rx_port_id, tx_port_id, pkts, pkts_cnt);
	rte_eth_tx_buffer_flush(tx_port_id, wt_data->queue_id, wt_data->tx_buffer);
}

/*
 * Worker thread main run loop
 *
 * @param [in]: Array of thread data structures
 * @return: 0 on success and system error code otherwise
 */
static int ip_frag_wt_thread_main(void *param)
{
	struct ip_frag_wt_data *wt_data_arr = param;
	struct ip_frag_wt_data *wt_data = &wt_data_arr[rte_lcore_id()];

	while (!force_stop) {
		switch (wt_data->cfg->mode) {
		case IP_FRAG_MODE_BIDIR:
			ip_frag_wt_reassemble(wt_data,
					      IP_FRAG_PORT_REASSEMBLE_0,
					      IP_FRAG_PORT_FRAGMENT_0,
					      PARSER_PKT_TYPE_UNKNOWN);
			ip_frag_wt_fragment(wt_data, IP_FRAG_PORT_FRAGMENT_0, IP_FRAG_PORT_REASSEMBLE_0);
			break;
		case IP_FRAG_MODE_MULTIPORT:
			ip_frag_wt_reassemble(wt_data,
					      IP_FRAG_PORT_REASSEMBLE_0,
					      IP_FRAG_PORT_REASSEMBLE_1,
					      PARSER_PKT_TYPE_TUNNELED);
			ip_frag_wt_reassemble(wt_data,
					      IP_FRAG_PORT_FRAGMENT_0,
					      IP_FRAG_PORT_FRAGMENT_1,
					      PARSER_PKT_TYPE_PLAIN);
			ip_frag_wt_fragment(wt_data, IP_FRAG_PORT_REASSEMBLE_1, IP_FRAG_PORT_REASSEMBLE_0);
			ip_frag_wt_fragment(wt_data, IP_FRAG_PORT_FRAGMENT_1, IP_FRAG_PORT_FRAGMENT_0);
			break;
		default:
			DOCA_LOG_ERR("Unsupported application mode: %u", wt_data->cfg->mode);
			return EINVAL;
		};
	}

	return 0;
}

/*
 * Allocate and initialize ip_frag mbuf fragmentation flags
 *
 * @cfg [in]: application config
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_mbuf_flags_init(struct ip_frag_config *cfg)
{
	static const struct rte_mbuf_dynflag flag_outer_desc = {
		.name = "ip_frag outer",
	};
	static const struct rte_mbuf_dynflag flag_inner_desc = {
		.name = "ip_frag inner",
	};
	int flag_outer;
	int flag_inner;

	flag_outer = rte_mbuf_dynflag_register(&flag_outer_desc);
	if (flag_outer < 0) {
		DOCA_LOG_ERR("Failed to register mbuf outer fragmentation flag with code: %d", -flag_outer);
		return DOCA_ERROR_NO_MEMORY;
	}

	flag_inner = rte_mbuf_dynflag_register(&flag_inner_desc);
	if (flag_inner < 0) {
		DOCA_LOG_ERR("Failed to register mbuf inner fragmentation flag with code: %d", -flag_inner);
		return DOCA_ERROR_NO_MEMORY;
	}

	cfg->mbuf_flag_outer_modified = RTE_BIT64(flag_outer);
	cfg->mbuf_flag_inner_modified = RTE_BIT64(flag_inner);
	return DOCA_SUCCESS;
}

/*
 * Initialize indirect fragmentation mempools
 *
 * @nb_queues [in]: number of device queues
 * @indirect_pools [out]: Per-socket array of indirect fragmentation mempools
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_indirect_pool_init(uint16_t nb_queues, struct rte_mempool *indirect_pools[])
{
	char mempool_name[RTE_MEMPOOL_NAMESIZE];
	unsigned socket;
	unsigned lcore;

	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		socket = rte_lcore_to_socket_id(lcore);

		if (!indirect_pools[socket]) {
			snprintf(mempool_name, sizeof(mempool_name), "Indirect mempool %u", socket);
			indirect_pools[socket] = rte_pktmbuf_pool_create(mempool_name,
									 NUM_MBUFS * nb_queues,
									 MBUF_CACHE_SIZE,
									 0,
									 0,
									 socket);
			if (!indirect_pools[socket]) {
				DOCA_LOG_ERR("Failed to allocate indirect mempool for socket %u", socket);
				return DOCA_ERROR_NO_MEMORY;
			}

			DOCA_LOG_DBG("Indirect mempool for socket %u initialized", socket);
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Cleanup and free FRAG fastpath data
 *
 * @wt_data_arr [in]: worker thread data array
 */
static void ip_frag_wt_data_cleanup(struct ip_frag_wt_data *wt_data_arr)
{
	struct ip_frag_wt_data *wt_data;
	unsigned lcore;

	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		wt_data = &wt_data_arr[lcore];

		if (wt_data->frag_tbl)
			rte_ip_frag_table_destroy(wt_data->frag_tbl);
		rte_free(wt_data->tx_buffer);
	}

	rte_free(wt_data_arr);
}

/*
 * Allocate and initialize ip_frag worker thread data
 *
 * @cfg [in]: application config
 * @indirect_pools [in]: Per-socket array of indirect fragmentation mempools
 * @wt_data_arr_out [out]: worker thread data array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_wt_data_init(const struct ip_frag_config *cfg,
					 struct rte_mempool *indirect_pools[],
					 struct ip_frag_wt_data **wt_data_arr_out)
{
	struct ip_frag_wt_data *wt_data_arr;
	struct ip_frag_wt_data *wt_data;
	uint16_t queue_id = 0;
	doca_error_t ret;
	unsigned lcore;

	wt_data_arr = rte_calloc("Worker data", RTE_MAX_LCORE, sizeof(*wt_data_arr), _Alignof(typeof(*wt_data_arr)));
	if (!wt_data_arr) {
		DOCA_LOG_ERR("Failed to allocate worker thread data array");
		return DOCA_ERROR_NO_MEMORY;
	}

	RTE_LCORE_FOREACH(lcore)
	{
		wt_data = &wt_data_arr[lcore];
		wt_data->cfg = cfg;
		wt_data->indirect_pool = indirect_pools[rte_lcore_to_socket_id(lcore)];
		wt_data->queue_id = queue_id++;

		wt_data->tx_buffer = rte_zmalloc_socket("TX buffer",
							RTE_ETH_TX_BUFFER_SIZE(IP_FRAG_MAX_PKT_BURST),
							RTE_CACHE_LINE_SIZE,
							rte_lcore_to_socket_id(lcore));
		if (!wt_data->tx_buffer) {
			DOCA_LOG_ERR("Failed to allocate worker thread tx buffer");
			ret = DOCA_ERROR_NO_MEMORY;
			goto cleanup;
		}
		rte_eth_tx_buffer_init(wt_data->tx_buffer, IP_FRAG_MAX_PKT_BURST);
		rte_eth_tx_buffer_set_err_callback(wt_data->tx_buffer,
						   rte_eth_tx_buffer_count_callback,
						   &wt_data->tx_buffer_err);

		wt_data->frag_tbl =
			rte_ip_frag_table_create(cfg->frag_tbl_size,
						 IP_FRAG_TBL_BUCKET_SIZE,
						 cfg->frag_tbl_size,
						 ((rte_get_tsc_hz() + MS_PER_S - 1) / MS_PER_S) * cfg->frag_tbl_timeout,
						 rte_lcore_to_socket_id(lcore));
		if (!wt_data->frag_tbl) {
			DOCA_LOG_ERR("Failed to allocate worker thread fragmentation table");
			ret = DOCA_ERROR_NO_MEMORY;
			goto cleanup;
		}

		DOCA_LOG_DBG("Worker thread %u data initialized", lcore);
	}

	*wt_data_arr_out = wt_data_arr;
	return DOCA_SUCCESS;

cleanup:
	ip_frag_wt_data_cleanup(wt_data_arr);
	return ret;
}

/*
 * Print SW debug counters of each worker
 *
 * @ctx [in]: Ip_frag context
 * @wt_data_arr [in]: worker thread data array
 */
static void ip_frag_sw_counters_print(struct ip_frag_ctx *ctx, struct ip_frag_wt_data *wt_data_arr)
{
	struct ip_frag_sw_counters sw_sum_port = {0};
	struct ip_frag_sw_counters sw_sum = {0};
	struct ip_frag_sw_counters *sw_counters;
	struct ip_frag_wt_data *wt_data;
	unsigned lcore;
	int port_id;

	DOCA_LOG_INFO("//////////////////// SW COUNTERS ////////////////////");

	for (port_id = 0; port_id < ctx->num_ports; port_id++) {
		DOCA_LOG_INFO("== Port %d:", port_id);
		memset(&sw_sum_port, 0, sizeof(sw_sum_port));

		RTE_LCORE_FOREACH(lcore)
		{
			wt_data = &wt_data_arr[lcore];
			sw_counters = &wt_data->sw_counters[port_id];

			DOCA_LOG_INFO(
				"Core sw %3u     frags_rx=%-8lu whole=%-8lu mtu_fits_rx=%-8lu mtu_exceed_rx=%-8lu frags_gen=%-8lu err=%-8lu",
				lcore,
				sw_counters->frags_rx,
				sw_counters->whole,
				sw_counters->mtu_fits_rx,
				sw_counters->mtu_exceed_rx,
				sw_counters->frags_gen,
				sw_counters->err);

			sw_sum_port.frags_rx += sw_counters->frags_rx;
			sw_sum_port.whole += sw_counters->whole;
			sw_sum_port.mtu_fits_rx += sw_counters->mtu_fits_rx;
			sw_sum_port.mtu_exceed_rx += sw_counters->mtu_exceed_rx;
			sw_sum_port.frags_gen += sw_counters->frags_gen;
			sw_sum_port.err += sw_counters->err;
		}

		DOCA_LOG_INFO(
			"TOTAL sw port %d frags_rx=%-8lu whole=%-8lu mtu_fits_rx=%-8lu mtu_exceed_rx=%-8lu frags_gen=%-8lu err=%-8lu",
			port_id,
			sw_sum_port.frags_rx,
			sw_sum_port.whole,
			sw_sum_port.mtu_fits_rx,
			sw_sum_port.mtu_exceed_rx,
			sw_sum_port.frags_gen,
			sw_sum_port.err);

		sw_sum.frags_rx += sw_sum_port.frags_rx;
		sw_sum.whole += sw_sum_port.whole;
		sw_sum.mtu_fits_rx += sw_sum_port.mtu_fits_rx;
		sw_sum.mtu_exceed_rx += sw_sum_port.mtu_exceed_rx;
		sw_sum.frags_gen += sw_sum_port.frags_gen;
		sw_sum.err += sw_sum_port.err;
	}

	DOCA_LOG_INFO("== Total:");
	DOCA_LOG_INFO(
		"TOTAL sw        frags_rx=%-8lu whole=%-8lu mtu_fits_rx=%-8lu mtu_exceed_rx=%-8lu frags_gen=%-8lu err=%-8lu",
		sw_sum.frags_rx,
		sw_sum.whole,
		sw_sum.mtu_fits_rx,
		sw_sum.mtu_exceed_rx,
		sw_sum.frags_gen,
		sw_sum.err);
}

/*
 * Print TX buffer errors counter of each worker
 *
 * @wt_data_arr [in]: worker thread data array
 */
static void ip_frag_tx_buffer_error_print(struct ip_frag_wt_data *wt_data_arr)
{
	struct ip_frag_wt_data *wt_data;
	uint64_t sum = 0;
	unsigned lcore;

	DOCA_LOG_INFO("//////////////////// TX BUFFER ERROR ////////////////////");

	RTE_LCORE_FOREACH(lcore)
	{
		wt_data = &wt_data_arr[lcore];

		DOCA_LOG_INFO("Core tx_buffer %3u err=%lu", lcore, wt_data->tx_buffer_err);

		sum += wt_data->tx_buffer_err;
	}

	DOCA_LOG_INFO("TOTAL tx_buffer    err=%lu", sum);
}

/*
 * Print SW debug counters of each worker
 *
 * @wt_data_arr [in]: worker thread data array
 */
static void ip_frag_tbl_stats_print(struct ip_frag_wt_data *wt_data_arr)
{
	struct ip_frag_wt_data *wt_data;
	unsigned lcore;

	DOCA_LOG_INFO("//////////////////// FRAG TABLE STATS ////////////////////");

	RTE_LCORE_FOREACH(lcore)
	{
		wt_data = &wt_data_arr[lcore];

		DOCA_LOG_INFO("Core %3u:", lcore);
		rte_ip_frag_table_statistics_dump(stdout, wt_data->frag_tbl);
	}
}
/*
 * Print debug counters of each worker thread
 *
 * @ctx [in]: Ip_frag context
 * @wt_data_arr [in]: worker thread data array
 */
static void ip_frag_debug_counters_print(struct ip_frag_ctx *ctx, struct ip_frag_wt_data *wt_data_arr)
{
	DOCA_LOG_INFO("");
	rte_mbuf_dyn_dump(stdout);
	DOCA_LOG_INFO("");
	ip_frag_tbl_stats_print(wt_data_arr);
	DOCA_LOG_INFO("");
	ip_frag_tx_buffer_error_print(wt_data_arr);
	DOCA_LOG_INFO("");
	ip_frag_sw_counters_print(ctx, wt_data_arr);
	DOCA_LOG_INFO("");
}

/*
 * Create a flow pipe
 *
 * @pipe_cfg [in]: Ip_Frag pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_pipe_create(struct ip_frag_pipe_cfg *pipe_cfg, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, pipe_cfg->port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, pipe_cfg->name, DOCA_FLOW_PIPE_BASIC, pipe_cfg->is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_domain(cfg, pipe_cfg->domain);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(cfg, pipe_cfg->num_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg num_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (pipe_cfg->match != NULL) {
		result = doca_flow_pipe_cfg_set_match(cfg, pipe_cfg->match, pipe_cfg->match_mask);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	result = doca_flow_pipe_create(cfg, pipe_cfg->fwd, pipe_cfg->fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IP_FRAG pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Create RSS flow pipe
 *
 * @ctx [in]: Ip_frag context
 * @port_id [in]: port to create pipe at
 * @pipe_name [in]: name of the created pipe
 * @is_root [in]: flag indicating root pipe
 * @flags [in]: pipe RSS flags
 * @pipe_miss [in]: forward miss pipe
 * @pipe_out [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_rss_pipe_create(struct ip_frag_ctx *ctx,
					    uint16_t port_id,
					    char *pipe_name,
					    bool is_root,
					    uint32_t flags,
					    struct doca_flow_pipe *pipe_miss,
					    struct doca_flow_pipe **pipe_out)
{
	uint16_t rss_queues[RTE_MAX_LCORE];
	struct doca_flow_match match_pipe = {
		.parser_meta.outer_l3_type = (flags & DOCA_FLOW_RSS_IPV4) ? DOCA_FLOW_L3_META_IPV4 :
									    DOCA_FLOW_L3_META_IPV6,
	};
	const int num_of_entries = 1;
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_RSS,
				    .rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
				    .rss.queues_array = rss_queues,
				    .rss.nr_queues = ctx->num_queues,
				    .rss.outer_flags = flags};
	struct doca_flow_fwd fwd_miss = {0};
	struct ip_frag_pipe_cfg pipe_cfg = {.port_id = port_id,
					    .port = ctx->ports[port_id],
					    .domain = DOCA_FLOW_PIPE_DOMAIN_DEFAULT,
					    .name = pipe_name,
					    .is_root = is_root,
					    .num_entries = num_of_entries,
					    .match = &match_pipe,
					    .match_mask = &match_pipe,
					    .fwd = &fwd};
	struct entries_status status = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t ret;
	int i;

	for (i = 0; i < ctx->num_queues; ++i)
		rss_queues[i] = i;

	if (pipe_miss) {
		fwd_miss.type = DOCA_FLOW_FWD_PIPE;
		fwd_miss.next_pipe = pipe_miss;
		pipe_cfg.fwd_miss = &fwd_miss;
	}

	ret = ip_frag_pipe_create(&pipe_cfg, &pipe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rss pipe: %s", doca_error_get_descr(ret));
		return ret;
	}

	ret = doca_flow_pipe_add_entry(0, pipe, NULL, 0, NULL, NULL, NULL, 0, &status, NULL);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add rss entry: %s", doca_error_get_descr(ret));
		return ret;
	}

	ret = doca_flow_entries_process(pipe_cfg.port, 0, DEFAULT_TIMEOUT_US, pipe_cfg.num_entries);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries on port %u: %s", port_id, doca_error_get_descr(ret));
		return ret;
	}

	if (status.nb_processed != num_of_entries || status.failure) {
		DOCA_LOG_ERR("Failed to process port %u entries", port_id);
		return DOCA_ERROR_BAD_STATE;
	}

	*pipe_out = pipe;
	return DOCA_SUCCESS;
}

/*
 * Create RSS pipe for each port
 *
 * @ctx [in] Ip_Frag context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ip_frag_rss_pipes_create(struct ip_frag_ctx *ctx)
{
	doca_error_t ret;
	int port_id;

	for (port_id = 0; port_id < ctx->num_ports; port_id++) {
		ret = ip_frag_rss_pipe_create(ctx,
					      port_id,
					      "RSS_IPV4_PIPE",
					      false,
					      DOCA_FLOW_RSS_IPV4,
					      NULL,
					      &ctx->pipes[port_id][IP_FRAG_RSS_PIPE_IPV4]);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create RSS IPv4 pipe: %s", doca_error_get_descr(ret));
			return ret;
		}

		ret = ip_frag_rss_pipe_create(ctx,
					      port_id,
					      "RSS_IPV6_PIPE",
					      true,
					      DOCA_FLOW_RSS_IPV6,
					      ctx->pipes[port_id][IP_FRAG_RSS_PIPE_IPV4],
					      &ctx->pipes[port_id][IP_FRAG_RSS_PIPE_IPV6]);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create RSS IPv6 pipe: %s", doca_error_get_descr(ret));
			return ret;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Fill DOCA device array to use during port starting.
 *
 * @ctx [in] Ip_Frag context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t fill_device_array(struct ip_frag_ctx *ctx)
{
	doca_error_t ret;
	int port_id;

	for (port_id = 0; port_id < ctx->num_ports; port_id++) {
		ret = doca_dpdk_port_as_dev(port_id, &ctx->dev_arr[port_id]);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get device for port %d: %s", port_id, doca_error_get_descr(ret));
			return ret;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t ip_frag(struct ip_frag_config *cfg, struct application_dpdk_config *dpdk_cfg)
{
	struct rte_mempool *indirect_pools[RTE_MAX_NUMA_NODES] = {NULL};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct ip_frag_ctx ctx = {
		.num_ports = dpdk_cfg->port_config.nb_ports,
		.num_queues = dpdk_cfg->port_config.nb_queues,
	};
	uint32_t actions_mem_size[RTE_MAX_ETHPORTS];
	struct ip_frag_wt_data *wt_data_arr;
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT};
	doca_error_t ret;

	ret = init_doca_flow(ctx.num_queues, "vnf,hws", &resource, nr_shared_resources);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(ret));
		return ret;
	}

	ret = ip_frag_mbuf_flags_init(cfg);
	if (ret != DOCA_SUCCESS)
		goto cleanup_doca_flow;

	ret = ip_frag_indirect_pool_init(ctx.num_queues, indirect_pools);
	if (ret != DOCA_SUCCESS)
		goto cleanup_doca_flow;

	ret = ip_frag_wt_data_init(cfg, indirect_pools, &wt_data_arr);
	if (ret != DOCA_SUCCESS)
		goto cleanup_doca_flow;

	ret = fill_device_array(&ctx);
	if (ret != DOCA_SUCCESS)
		goto cleanup_doca_flow;

	resource.nr_rss = 2; /* IPv4 + IPv6 */
	ret = init_doca_flow_ports(ctx.num_ports, ctx.ports, false, ctx.dev_arr, actions_mem_size, &resource);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(ret));
		goto cleanup_wt_data;
	}

	ret = ip_frag_rss_pipes_create(&ctx);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to pipes: %s", doca_error_get_descr(ret));
		goto cleanup_ports;
	}

	DOCA_LOG_INFO("Initialization finished, starting data path");
	if (rte_eal_mp_remote_launch(ip_frag_wt_thread_main, wt_data_arr, CALL_MAIN)) {
		DOCA_LOG_ERR("Failed to launch worker threads");
		goto cleanup_ports;
	}
	rte_eal_mp_wait_lcore();

	ip_frag_debug_counters_print(&ctx, wt_data_arr);
cleanup_ports:
	ret = stop_doca_flow_ports(ctx.num_ports, ctx.ports);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop doca flow ports: %s", doca_error_get_descr(ret));
cleanup_wt_data:
	ip_frag_wt_data_cleanup(wt_data_arr);
cleanup_doca_flow:
	doca_flow_destroy();
	return ret;
}
