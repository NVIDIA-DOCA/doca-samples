/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

/* CT Firewall datapath worker, one per CT queue. */

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_flow.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>

#include <doca_log.h>

#include <flow_ct_common.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::WORKER);

/* Software traffic of both uplinks arrives on the switch manager port. */
#define SW_DPDK_PORT 0

static void key_reverse(const struct ct_fw_key *in, struct ct_fw_key *out)
{
	*out = *in;
	memcpy(out->src, in->dst, sizeof(out->src));
	memcpy(out->dst, in->src, sizeof(out->dst));
	out->src_port = in->dst_port;
	out->dst_port = in->src_port;
}

static struct ct_fw_conn *live_conn(struct ct_fw_conn *conn)
{
	return (conn != NULL && !conn->free_pending) ? conn : NULL;
}

/* An entry waiting for its profile write does not act yet. */
static bool entry_may_act(const struct ct_fw_conn *conn)
{
	return conn->state == CT_FW_CONN_OFFLOADED || (conn->entry != NULL && !conn->wv_op_inflight);
}

/* Origin on the opening uplink, reply on the other. */
static bool on_expected_uplink(const struct ct_fw_conn *conn, uint32_t port_code, bool is_reply)
{
	uint32_t want;

	if (port_code == CT_FW_PORT_CODE_NONE || conn->origin_port_code == CT_FW_PORT_CODE_NONE)
		return true;
	want = is_reply ? CT_FW_PORT_CODE_OTHER(conn->origin_port_code) : conn->origin_port_code;
	return port_code == want;
}

/* True on the expected uplink; else keep the first candidate. */
static bool bidir_candidate(struct ct_fw_conn *conn,
			    bool is_reply,
			    uint32_t port_code,
			    struct ct_fw_conn **first,
			    bool *first_reply)
{
	if (conn == NULL)
		return false;
	if (on_expected_uplink(conn, port_code, is_reply))
		return true;
	if (*first == NULL) {
		*first = conn;
		*first_reply = is_reply;
	}
	return false;
}

/* Until the entry is live, a NAT reply arrives on the translated tuple. */
struct ct_fw_conn *ct_fw_conn_find_bidir(struct ct_fw_conn_table *table,
					 const struct ct_fw_key *key,
					 uint32_t port_code,
					 bool *is_reply,
					 bool *wrong_uplink)
{
	struct ct_fw_conn *conn = live_conn(ct_fw_conn_lookup(table, key));
	struct ct_fw_conn *first = NULL;
	bool first_reply = false;
	struct ct_fw_key rev;

	*wrong_uplink = false;
	*is_reply = false;
	if (bidir_candidate(conn, false, port_code, &first, &first_reply))
		return conn;
	conn = live_conn(ct_fw_conn_lookup_reply(table, key));
	*is_reply = true;
	if (bidir_candidate(conn, true, port_code, &first, &first_reply))
		return conn;
	key_reverse(key, &rev);
	conn = live_conn(ct_fw_conn_lookup(table, &rev));
	if (conn != NULL && !entry_may_act(conn) && !ct_fw_conn_reply_is_plain_reverse(table, conn))
		conn = NULL;
	if (bidir_candidate(conn, true, port_code, &first, &first_reply))
		return conn;
	conn = live_conn(ct_fw_conn_lookup_reply(table, &rev));
	*is_reply = false;
	if (bidir_candidate(conn, false, port_code, &first, &first_reply))
		return conn;
	*is_reply = first_reply;
	*wrong_uplink = first != NULL;
	return first;
}

bool ct_fw_ipv4_is_fragment(uint16_t fragment_offset_be)
{
	uint16_t frag = rte_be_to_cpu_16(fragment_offset_be);

	return (frag & (RTE_IPV4_HDR_OFFSET_MASK | RTE_IPV4_HDR_MF_FLAG)) != 0;
}

bool ct_fw_tcp_opener_allowed(uint8_t tcp_flags, bool *is_new_syn)
{
	uint8_t opener = tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG);

	*is_new_syn = (opener == RTE_TCP_SYN_FLAG);
	if (*is_new_syn)
		return true;
	return (tcp_flags & RTE_TCP_ACK_FLAG) &&
	       !(tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG));
}

bool ct_fw_tcp_synack_allowed(uint8_t tcp_flags)
{
	return (tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG)) ==
	       (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG);
}

void ct_fw_syn_refresh(struct ct_fw_tcp_obs *syn, const struct ct_fw_tcp_obs *seg)
{
	uint16_t payload_len = seg->seq == syn->seq && syn->payload_len > seg->payload_len ? syn->payload_len :
											     seg->payload_len;

	*syn = *seg;
	syn->payload_len = payload_len;
}

bool ct_fw_frame_is_other(const uint8_t *data, const uint8_t *data_end)
{
	const uint8_t *l3;
	uint16_t ether_type;

	if (!ct_fw_eth_l3(data, data_end, &ether_type, &l3))
		return false;
	if (ether_type == RTE_ETHER_TYPE_ARP)
		return true;
	if (ether_type == RTE_ETHER_TYPE_IPV4) {
		const struct rte_ipv4_hdr *ip = (const struct rte_ipv4_hdr *)l3;

		return l3 + sizeof(*ip) <= data_end && !ct_fw_ipv4_is_fragment(ip->fragment_offset) &&
		       ip->next_proto_id == DOCA_FLOW_PROTO_ICMP;
	}
	if (ether_type == RTE_ETHER_TYPE_IPV6) {
		const struct rte_ipv6_hdr *ip = (const struct rte_ipv6_hdr *)l3;

		return l3 + sizeof(*ip) <= data_end && ip->proto == DOCA_FLOW_PROTO_ICMP6;
	}
	return false;
}

bool ct_fw_halfopen_rst_ok(const struct ct_fw_tcp_obs *syn, const struct ct_fw_parsed_pkt *pkt, bool is_reply)
{
	if (is_reply)
		return (pkt->tcp_flags & RTE_TCP_ACK_FLAG) && ct_fw_synack_acks_syn(syn, pkt->ack);
	return ct_fw_synack_acks_syn(syn, pkt->seq);
}

bool ct_fw_synack_acks_syn(const struct ct_fw_tcp_obs *syn, uint32_t ack)
{
	return (uint32_t)(ack - (syn->seq + 1)) <= syn->payload_len;
}

bool ct_fw_eth_l3(const uint8_t *data, const uint8_t *data_end, uint16_t *ether_type, const uint8_t **l3)
{
	const struct rte_ether_hdr *eth = (const struct rte_ether_hdr *)data;
	const uint8_t *next = data + sizeof(*eth);
	uint16_t type;
	int tags;

	if (next > data_end)
		return false;
	type = rte_be_to_cpu_16(eth->ether_type);
	for (tags = 0; tags < CT_FW_MAX_VLAN_TAGS && type == RTE_ETHER_TYPE_VLAN; tags++) {
		const struct rte_vlan_hdr *vlan = (const struct rte_vlan_hdr *)next;

		next += sizeof(*vlan);
		if (next > data_end)
			return false;
		type = rte_be_to_cpu_16(vlan->eth_proto);
	}
	*ether_type = type;
	*l3 = next;
	return true;
}

static bool is_vlan_ether_type(uint16_t type)
{
	return type == RTE_ETHER_TYPE_VLAN || type == RTE_ETHER_TYPE_QINQ;
}

bool ct_fw_frame_zone(const uint8_t *data, const uint8_t *data_end, uint32_t *zone)
{
	const struct rte_ether_hdr *eth = (const struct rte_ether_hdr *)data;
	const struct rte_vlan_hdr *outer = (const struct rte_vlan_hdr *)(data + sizeof(*eth));
	const struct rte_vlan_hdr *inner = outer + 1;
	uint16_t type;

	if ((const uint8_t *)outer > data_end)
		return false;
	type = rte_be_to_cpu_16(eth->ether_type);
	if (!is_vlan_ether_type(type)) {
		*zone = 0;
		return true;
	}
	if ((const uint8_t *)inner > data_end)
		return false;
	if (!is_vlan_ether_type(rte_be_to_cpu_16(outer->eth_proto))) {
		/* An 802.1ad tag alone is zone 0. */
		*zone = type == RTE_ETHER_TYPE_VLAN ? rte_be_to_cpu_16(outer->vlan_tci) & CT_FW_VLAN_VID_MASK : 0;
		return true;
	}
	/* Only two 802.1Q tags are supported. */
	if (type != RTE_ETHER_TYPE_VLAN || rte_be_to_cpu_16(outer->eth_proto) != RTE_ETHER_TYPE_VLAN ||
	    (const uint8_t *)(inner + 1) > data_end || is_vlan_ether_type(rte_be_to_cpu_16(inner->eth_proto)))
		return false;
	*zone = (rte_be_to_cpu_16(outer->vlan_tci) & CT_FW_VLAN_VID_MASK) |
		(uint32_t)(rte_be_to_cpu_16(inner->vlan_tci) & CT_FW_VLAN_VID_MASK) << CT_FW_VLAN_VID_BITS;
	return true;
}

/* A frame shorter than its IP length is refused. */
bool ct_fw_parse_packet(struct rte_mbuf *m, uint32_t zone, bool ipv6_ct, struct ct_fw_parsed_pkt *out)
{
	const uint8_t *data = rte_pktmbuf_mtod(m, const uint8_t *);
	const uint8_t *data_end = data + rte_pktmbuf_data_len(m);
	const uint8_t *l3;
	const uint8_t *l4;
	uint16_t ether_type;
	uint16_t l4_span = 0; /* L4 bytes per the IP header */
	uint8_t proto;

	memset(out, 0, sizeof(*out));
	out->key.zone = zone;

	if (!ct_fw_eth_l3(data, data_end, &ether_type, &l3))
		return false;

	if (ether_type == RTE_ETHER_TYPE_IPV4) {
		const struct rte_ipv4_hdr *ip = (const struct rte_ipv4_hdr *)l3;
		uint16_t ihl;
		uint16_t total;

		if (l3 + sizeof(*ip) > data_end || ip->version != IPVERSION)
			return false;
		ihl = rte_ipv4_hdr_len(ip);
		if (ihl < sizeof(*ip) || l3 + ihl > data_end)
			return false;
		out->key.family = CT_FW_FAMILY_IPV4;
		if (ct_fw_ipv4_is_fragment(ip->fragment_offset)) {
			out->is_fragment = true;
			return false;
		}
		total = rte_be_to_cpu_16(ip->total_length);
		memcpy(out->key.src, &ip->src_addr, 4);
		memcpy(out->key.dst, &ip->dst_addr, 4);
		proto = ip->next_proto_id;
		l4 = l3 + ihl;
		l4_span = total > ihl ? (uint16_t)(total - ihl) : 0;
	} else if (ether_type == RTE_ETHER_TYPE_IPV6 && ipv6_ct) {
		const struct rte_ipv6_hdr *ip = (const struct rte_ipv6_hdr *)l3;

		if (l3 + sizeof(*ip) > data_end || (rte_be_to_cpu_32(ip->vtc_flow) >> 28) != CT_FW_IPV6_VERSION)
			return false;
		out->key.family = CT_FW_FAMILY_IPV6;
		memcpy(out->key.src, &ip->src_addr, 16);
		memcpy(out->key.dst, &ip->dst_addr, 16);
		proto = ip->proto;
		if (proto == IPPROTO_FRAGMENT) {
			out->is_fragment = true;
			return false;
		}
		l4 = l3 + sizeof(*ip);
		l4_span = rte_be_to_cpu_16(ip->payload_len); /* no other extension headers assumed */
	} else {
		return false;
	}

	/* The IP length excludes frame padding. */
	if ((size_t)(data_end - l4) < l4_span)
		return false;

	out->key.protocol = proto;
	if (proto == DOCA_FLOW_PROTO_TCP) {
		const struct rte_tcp_hdr *tcp = (const struct rte_tcp_hdr *)l4;
		uint16_t tcp_hdr;

		if (l4 + sizeof(*tcp) > data_end)
			return false;
		tcp_hdr = (uint16_t)(((tcp->data_off >> 4) & 0x0f) * 4);
		if (tcp_hdr < sizeof(*tcp) || l4 + tcp_hdr > data_end)
			return false;
		if (l4_span < tcp_hdr)
			return false;
		out->is_tcp = true;
		out->key.src_port = rte_be_to_cpu_16(tcp->src_port);
		out->key.dst_port = rte_be_to_cpu_16(tcp->dst_port);
		out->tcp_flags = tcp->tcp_flags;
		out->seq = rte_be_to_cpu_32(tcp->sent_seq);
		out->ack = rte_be_to_cpu_32(tcp->recv_ack);
		out->window = rte_be_to_cpu_16(tcp->rx_win);
		out->payload_len = (uint16_t)(l4_span - tcp_hdr);

		/* Window-scale and SACK-permitted options. */
		out->ws = CT_FW_WS_DISABLED;
		out->sack_seen = false;
		{
			const uint8_t *opt = l4 + sizeof(*tcp);
			const uint8_t *opt_end = l4 + tcp_hdr;

			while (opt < opt_end) {
				uint8_t kind = opt[0];
				uint8_t len;

				if (kind == 0) /* end of option list */
					break;
				if (kind == 1) { /* no-op padding */
					opt++;
					continue;
				}
				if (opt + 1 >= opt_end) /* every other kind needs a length byte */
					break;
				len = opt[1];
				if (len < 2 || opt + len > opt_end)
					break;
				if (kind == 3 && len == 3)
					out->ws = opt[2] > 14 ? 14 : opt[2]; /* RFC 7323 caps the shift at 14 */
				else if (kind == 4 && len == 2)
					out->sack_seen = true;
				opt += len;
			}
		}
	} else if (proto == DOCA_FLOW_PROTO_UDP) {
		const struct rte_udp_hdr *udp = (const struct rte_udp_hdr *)l4;

		if (l4 + sizeof(*udp) > data_end || l4_span < sizeof(*udp))
			return false;
		out->key.src_port = rte_be_to_cpu_16(udp->src_port);
		out->key.dst_port = rte_be_to_cpu_16(udp->dst_port);
	} else {
		return false;
	}

	out->valid = true;
	return true;
}

static void record_obs(struct ct_fw_tcp_obs *obs, const struct ct_fw_parsed_pkt *pkt)
{
	obs->seq = pkt->seq;
	obs->ack = pkt->ack;
	obs->window = pkt->window;
	obs->payload_len = pkt->payload_len;
	obs->flags = pkt->tcp_flags;
	obs->ws = pkt->ws;
	obs->sack_seen = pkt->sack_seen;
}

static uint32_t cksum_add(uint32_t sum, const uint8_t *p, size_t len, bool invert)
{
	size_t i;

	for (i = 0; i + 1 < len; i += 2) {
		uint16_t word = (uint16_t)(p[i] << 8 | p[i + 1]);

		sum += invert ? (uint16_t)~word : word;
	}
	return sum;
}

uint16_t ct_fw_cksum_adjust_addr(uint16_t cksum,
				 const uint8_t *old_src,
				 const uint8_t *old_dst,
				 const uint8_t *new_src,
				 const uint8_t *new_dst,
				 size_t addr_len,
				 bool udp)
{
	uint32_t sum;
	uint16_t hc = rte_be_to_cpu_16(cksum);

	if (udp && cksum == 0)
		return 0;
	/* HC' = ~(~HC + ~m + m') */
	sum = (uint16_t)~hc;
	sum = cksum_add(sum, old_src, addr_len, true);
	sum = cksum_add(sum, old_dst, addr_len, true);
	sum = cksum_add(sum, new_src, addr_len, false);
	sum = cksum_add(sum, new_dst, addr_len, false);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	hc = (uint16_t)~sum;
	if (udp && hc == 0)
		hc = 0xffff;
	return rte_cpu_to_be_16(hc);
}

/* Addresses and checksums; L4 on the first fragment only. */
static bool rewrite_fragment(struct rte_ipv4_hdr *ip4,
			     uint16_t ihl,
			     const uint8_t *data_end,
			     const uint8_t *src,
			     const uint8_t *dst)
{
	uint8_t old_src[CT_FW_IPV4_ADDR_BYTES], old_dst[CT_FW_IPV4_ADDR_BYTES];
	uint8_t *l4 = (uint8_t *)ip4 + ihl;
	bool first = (rte_be_to_cpu_16(ip4->fragment_offset) & RTE_IPV4_HDR_OFFSET_MASK) == 0;
	bool udp = ip4->next_proto_id == DOCA_FLOW_PROTO_UDP;
	uint8_t *cksum = NULL;
	rte_be16_t ck;

	if (first && ip4->next_proto_id == DOCA_FLOW_PROTO_TCP && l4 + sizeof(struct rte_tcp_hdr) <= data_end)
		cksum = l4 + offsetof(struct rte_tcp_hdr, cksum);
	else if (first && udp && l4 + sizeof(struct rte_udp_hdr) <= data_end)
		cksum = l4 + offsetof(struct rte_udp_hdr, dgram_cksum);
	else if (first)
		return false;
	memcpy(old_src, &ip4->src_addr, sizeof(old_src));
	memcpy(old_dst, &ip4->dst_addr, sizeof(old_dst));
	memcpy(&ip4->src_addr, src, CT_FW_IPV4_ADDR_BYTES);
	memcpy(&ip4->dst_addr, dst, CT_FW_IPV4_ADDR_BYTES);
	ip4->hdr_checksum =
		ct_fw_cksum_adjust_addr(ip4->hdr_checksum, old_src, old_dst, src, dst, CT_FW_IPV4_ADDR_BYTES, false);
	if (cksum != NULL) {
		memcpy(&ck, cksum, sizeof(ck));
		ck = ct_fw_cksum_adjust_addr(ck, old_src, old_dst, src, dst, CT_FW_IPV4_ADDR_BYTES, udp);
		memcpy(cksum, &ck, sizeof(ck));
	}
	return true;
}

bool ct_fw_rewrite_addrs(struct rte_mbuf *m, const uint8_t *src, const uint8_t *dst)
{
	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
	uint8_t *data_end = data + rte_pktmbuf_data_len(m);
	uint8_t old_src[CT_FW_IPV6_ADDR_BYTES], old_dst[CT_FW_IPV6_ADDR_BYTES];
	size_t addr_len;
	const uint8_t *l3;
	struct rte_ipv4_hdr *ip4 = NULL;
	struct rte_ipv6_hdr *ip6 = NULL;
	uint16_t ether_type;
	uint16_t l4_len; /* per the IP header */
	uint8_t *l4;
	uint8_t proto;

	if (!ct_fw_eth_l3(data, data_end, &ether_type, &l3))
		return false;

	if (ether_type == RTE_ETHER_TYPE_IPV4) {
		uint16_t ihl;
		uint16_t total;

		ip4 = (struct rte_ipv4_hdr *)(uintptr_t)l3;
		if ((uint8_t *)(ip4 + 1) > data_end)
			return false;
		ihl = rte_ipv4_hdr_len(ip4);
		if (ihl < sizeof(*ip4) || (uint8_t *)ip4 + ihl > data_end)
			return false;
		if (ct_fw_ipv4_is_fragment(ip4->fragment_offset))
			return rewrite_fragment(ip4, ihl, data_end, src, dst);
		total = rte_be_to_cpu_16(ip4->total_length);
		if (total < ihl)
			return false;
		l4_len = (uint16_t)(total - ihl);
		addr_len = CT_FW_IPV4_ADDR_BYTES;
		memcpy(old_src, &ip4->src_addr, addr_len);
		memcpy(old_dst, &ip4->dst_addr, addr_len);
		memcpy(&ip4->src_addr, src, addr_len);
		memcpy(&ip4->dst_addr, dst, addr_len);
		proto = ip4->next_proto_id;
		l4 = (uint8_t *)ip4 + ihl;
		ip4->hdr_checksum =
			ct_fw_cksum_adjust_addr(ip4->hdr_checksum, old_src, old_dst, src, dst, addr_len, false);
	} else if (ether_type == RTE_ETHER_TYPE_IPV6) {
		ip6 = (struct rte_ipv6_hdr *)(uintptr_t)l3;
		if ((uint8_t *)(ip6 + 1) > data_end)
			return false;
		l4_len = rte_be_to_cpu_16(ip6->payload_len); /* no extension headers assumed */
		addr_len = CT_FW_IPV6_ADDR_BYTES;
		memcpy(old_src, &ip6->src_addr, addr_len);
		memcpy(old_dst, &ip6->dst_addr, addr_len);
		memcpy(&ip6->src_addr, src, addr_len);
		memcpy(&ip6->dst_addr, dst, addr_len);
		proto = ip6->proto;
		l4 = (uint8_t *)(ip6 + 1);
	} else {
		return false;
	}

	if (l4 + l4_len > data_end)
		return false;

	/* The L4 checksum covers the addresses; adjusted, so a bad one stays bad. */
	if (proto == DOCA_FLOW_PROTO_TCP) {
		struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)l4;

		if ((uint8_t *)(tcp + 1) > data_end)
			return false;
		tcp->cksum = ct_fw_cksum_adjust_addr(tcp->cksum, old_src, old_dst, src, dst, addr_len, false);
	} else if (proto == DOCA_FLOW_PROTO_UDP) {
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)l4;

		if ((uint8_t *)(udp + 1) > data_end)
			return false;
		/* A checksum of 0 (none in IPv4, invalid in IPv6) stays 0. */
		udp->dgram_cksum =
			ct_fw_cksum_adjust_addr(udp->dgram_cksum, old_src, old_dst, src, dst, addr_len, true);
	} else if (ip4 != NULL && proto == DOCA_FLOW_PROTO_ICMP) {
		/* The ICMP checksum has no pseudo-header: nothing to adjust. */
	} else {
		return false;
	}
	return true;
}

/* Send to the other uplink; clear the reinject mark. */
static void uplink_tx(struct ct_fw_worker *worker, struct rte_mbuf *m)
{
	/* Nothing is sent after a queue failure. */
	if (ct_fw_failed(worker)) {
		rte_pktmbuf_free(m);
		return;
	}
	if (rte_flow_dynf_metadata_avail()) {
		uint32_t v = *RTE_FLOW_DYNF_METADATA(m) & ~CT_FW_META_REINJECT_MASK;
		uint32_t in = v & CT_FW_META_PORT_MASK;

		if (in != CT_FW_PORT_CODE_NONE)
			v = (v & ~CT_FW_META_PORT_MASK) | CT_FW_PORT_CODE_OTHER(in);
		m->ol_flags |= rte_flow_dynf_metadata_mask;
		*RTE_FLOW_DYNF_METADATA(m) = v;
	}
	if (rte_eth_tx_burst(SW_DPDK_PORT, worker->rss_queue, &m, 1) == 1)
		CT_FW_STAT_INC(worker->stats.tx_packets);
	else
		rte_pktmbuf_free(m);
}

/* Translate before the slot can be released. */
static void admit(const struct ct_fw_worker *worker,
		  struct ct_fw_verdict *v,
		  const struct ct_fw_conn *conn,
		  bool is_reply)
{
	const struct ct_fw_config *cfg = &worker->app->config;

	v->outcome = CT_FW_FORWARD;
	v->nat = ct_fw_conn_nat(cfg, conn)->enabled;
	v->drop_ctr = NULL;
	if (v->nat)
		ct_fw_nat_translate(cfg, conn, is_reply, v->src, v->dst);
}

/* Untracked packets are never translated. */
static void admit_untranslated(struct ct_fw_verdict *v)
{
	v->outcome = CT_FW_FORWARD_UNTRANSLATED;
	v->nat = false;
	v->drop_ctr = NULL;
}

/* ctr NULL: already counted, or not counted. */
static void refuse(struct ct_fw_verdict *v, uint64_t *ctr)
{
	v->outcome = CT_FW_DROP;
	v->drop_ctr = ctr;
}

void ct_fw_emit(struct ct_fw_worker *worker, struct rte_mbuf *m, const struct ct_fw_verdict *v)
{
	switch (v->outcome) {
	case CT_FW_FORWARD:
		/* A packet that cannot be translated is not sent. */
		if (v->nat && !ct_fw_rewrite_addrs(m, v->src, v->dst)) {
			rte_pktmbuf_free(m);
			return;
		}
		uplink_tx(worker, m);
		return;
	case CT_FW_FORWARD_UNTRANSLATED:
		uplink_tx(worker, m);
		return;
	case CT_FW_DROP:
		if (v->drop_ctr != NULL)
			CT_FW_STAT_INC(*v->drop_ctr);
		rte_pktmbuf_free(m);
		return;
	case CT_FW_HELD:
	case CT_FW_CONSUMED:
		return;
	}
}

uint32_t ct_fw_mbuf_port_code(struct rte_mbuf *m)
{
	if (rte_flow_dynf_metadata_avail())
		return *RTE_FLOW_DYNF_METADATA(m) & CT_FW_META_PORT_MASK;
	return CT_FW_PORT_CODE_NONE;
}

static uint32_t mbuf_zone(struct rte_mbuf *m)
{
	if (rte_flow_dynf_metadata_avail())
		return ct_fw_meta_zone(*RTE_FLOW_DYNF_METADATA(m));
	return 0;
}

static bool counting_full(struct ct_fw_worker *worker)
{
	if (worker->counting < CT_FW_MAX_COUNTING)
		return false;
	if (ct_fw_log_due(worker->now_tsc, &worker->counting_log_tsc))
		DOCA_LOG_WARN("Worker %u: %u connections counting or being picked up (limit %u); new ones are dropped",
			      worker->worker_id,
			      worker->counting,
			      CT_FW_MAX_COUNTING);
	return true;
}

/* Drop packets of a connection whose failed install is still owed back. */
static bool refuse_if_retained(struct ct_fw_worker *worker, const struct ct_fw_conn *conn, struct ct_fw_verdict *v)
{
	if (conn == NULL || !ct_fw_conn_retained(conn))
		return false;
	refuse(v, &worker->stats.install_drops);
	return true;
}

/* Fail closed. */
static void refuse_untracked(struct ct_fw_worker *worker, struct ct_fw_verdict *v)
{
	refuse(v, &worker->stats.pool_exhausted);
}

/* Drop a new connection from a second --snat client. */
static bool snat_refused(struct ct_fw_worker *worker,
			 struct rte_mbuf *m,
			 const struct ct_fw_parsed_pkt *pkt,
			 struct ct_fw_verdict *v)
{
	rte_be32_t src;

	if (pkt->key.family != CT_FW_FAMILY_IPV4 || !ct_fw_snat_enabled(&worker->app->config, ct_fw_mbuf_port_code(m)))
		return false;
	memcpy(&src, pkt->key.src, sizeof(src));
	if (ct_fw_snat_owner_allowed(worker,
				     pkt->key.zone,
				     &rte_pktmbuf_mtod(m, const struct rte_ether_hdr *)->src_addr,
				     src))
		return false;
	refuse(v, &worker->stats.nat_other_drops);
	return true;
}

/* An admitted connection claims --snat. */
static void snat_claim(struct ct_fw_worker *worker,
		       struct rte_mbuf *m,
		       const struct ct_fw_parsed_pkt *pkt,
		       const struct ct_fw_verdict *v)
{
	rte_be32_t src;

	if ((v->outcome != CT_FW_FORWARD && v->outcome != CT_FW_HELD) || pkt->key.family != CT_FW_FAMILY_IPV4 ||
	    !ct_fw_snat_enabled(&worker->app->config, ct_fw_mbuf_port_code(m)))
		return;
	memcpy(&src, pkt->key.src, sizeof(src));
	ct_fw_snat_owner_claim(worker, pkt->key.zone, &rte_pktmbuf_mtod(m, const struct rte_ether_hdr *)->src_addr, src);
}

/* Errors a later attempt may not hit. */
static bool install_retryable(doca_error_t result)
{
	return result == DOCA_ERROR_FULL || result == DOCA_ERROR_AGAIN || result == DOCA_ERROR_IN_PROGRESS ||
	       result == DOCA_ERROR_BAD_STATE;
}

/* Refuse the packet, or mark it consumed when the install already dropped it; keep a half-open slot on a retryable
 * error. */
static void refuse_uninstalled(struct ct_fw_worker *worker,
			       struct ct_fw_verdict *v,
			       bool owns_pkt,
			       struct ct_fw_conn *conn,
			       doca_error_t result,
			       bool keep_if_retryable)
{
	/* FULL was counted where it was refused (pool_exhausted or wv_limit_drops). */
	if (owns_pkt)
		refuse(v, result == DOCA_ERROR_FULL ? NULL : &worker->stats.install_drops);
	else
		v->outcome = CT_FW_CONSUMED;
	if (!(keep_if_retryable && install_retryable(result)))
		ct_fw_conn_release(worker, conn);
}

/* Hold the connection half-open until its SYN-ACK. */
static void handle_new_syn(struct ct_fw_worker *worker,
			   struct rte_mbuf *m,
			   const struct ct_fw_parsed_pkt *pkt,
			   struct ct_fw_verdict *v)
{
	struct ct_fw_conn *conn;

	conn = ct_fw_conn_open(worker, &pkt->key);
	if (conn == NULL) {
		/* Fail closed; the client retries once there is room. */
		refuse_untracked(worker, v);
		return;
	}
	conn->state = CT_FW_CONN_HALF_OPEN;
	conn->last_seen_tsc = worker->now_tsc;
	conn->origin_port_code = ct_fw_mbuf_port_code(m); /* the opening uplink decides translation */
	ct_fw_conn_reply_index_add(&worker->table, conn);
	record_obs(&conn->syn, pkt);
	admit(worker, v, conn, false);
}

static void handle_synack(struct ct_fw_worker *worker,
			  struct rte_mbuf *m,
			  struct ct_fw_conn *conn,
			  const struct ct_fw_parsed_pkt *pkt,
			  struct ct_fw_verdict *v)
{
	doca_error_t result;

	if (!ct_fw_synack_acks_syn(&conn->syn, pkt->ack)) {
		refuse(v, &worker->stats.state_drops);
		return;
	}
	conn->last_seen_tsc = worker->now_tsc;
	record_obs(&conn->synack, pkt);

	/* --offload-after: install once the threshold is reached. */
	if (worker->app->config.offload_after != 0) {
		if (counting_full(worker)) {
			refuse_untracked(worker, v);
			ct_fw_conn_release(worker, conn);
		} else {
			conn->state = CT_FW_CONN_COUNTING;
			conn->install_mode = CT_FW_INSTALL_THRESHOLD;
			conn->counted_dirs = CT_FW_COUNT_DIR_ORIGIN | CT_FW_COUNT_DIR_REPLY;
			/* The bare SYN acknowledged nothing, so seed it from the SYN-ACK. */
			conn->syn.ack = pkt->seq + 1;
			/* TCP Fast Open: data on the SYN and SYN-ACK has passed already. */
			conn->bytes_origin = conn->syn.payload_len;
			conn->bytes_reply = conn->synack.payload_len;
			worker->counting++;
			admit(worker, v, conn, true);
		}
		return;
	}

	/* Held until the entry is live; the submit may already release it. */
	conn->held_clone = m;
	conn->held_forward = true;
	v->outcome = CT_FW_HELD;
	result = ct_fw_conn_install(worker, conn);
	if (ct_fw_failed(worker))
		return;
	if (result != DOCA_SUCCESS) {
		refuse_uninstalled(worker, v, ct_fw_conn_take_held(conn) != NULL, conn, result, true);
		return;
	}
	/* Live on return (--sync or a fast completion); otherwise the completion sends it. */
	if (conn->state == CT_FW_CONN_OFFLOADED) {
		if (ct_fw_conn_take_held(conn) != NULL)
			admit(worker, v, conn, true);
		else
			v->outcome = CT_FW_CONSUMED;
	}
}

/* A replay skips the Zone stage: set the zone here. */
static void mark_reinject(struct ct_fw_worker *worker, struct rte_mbuf *clone, uint32_t zone)
{
	struct doca_flow_meta zone_meta;

	memset(&zone_meta, 0, sizeof(zone_meta));
	doca_flow_ct_meta_prepare(worker->app->ports[0], &zone_meta, zone, false);
	clone->ol_flags |= rte_flow_dynf_metadata_mask;
	*RTE_FLOW_DYNF_METADATA(clone) = rte_be_to_cpu_32(zone_meta.pkt_meta) | CT_FW_META_REINJECT_MASK;
}

/* A marked clone keeps the wire addresses. */
static void reinject_tx(struct ct_fw_worker *worker, struct rte_mbuf *clone)
{
	if (rte_eth_tx_burst(SW_DPDK_PORT, worker->rss_queue, &clone, 1) == 1)
		CT_FW_STAT_INC(worker->stats.reinjections);
	else
		rte_pktmbuf_free(clone);
}

static void hold_reinject_clone(struct ct_fw_worker *worker, struct ct_fw_conn *conn, struct rte_mbuf *m)
{
	struct rte_mbuf *clone;

	ct_fw_conn_drop_held(worker, conn);
	if (!rte_flow_dynf_metadata_avail())
		return;
	clone = rte_pktmbuf_copy(m, m->pool, 0, UINT32_MAX);
	if (clone == NULL)
		return;
	mark_reinject(worker, clone, conn->key.zone);
	conn->held_clone = clone;
}

/* Forward a held SYN-ACK; reinject a clone. */
static void replay_held_clone(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	bool forward = conn->held_forward;
	struct rte_mbuf *held = ct_fw_conn_take_held(conn);
	struct ct_fw_verdict v;

	if (held == NULL)
		return;
	if (!forward) {
		reinject_tx(worker, held);
		return;
	}
	admit(worker, &v, conn, true);
	ct_fw_emit(worker, held, &v);
}

static void replay_or_release(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (conn->state == CT_FW_CONN_OFFLOADED)
		replay_held_clone(worker, conn);
	else if (conn->state != CT_FW_CONN_ADOPTING && conn->state != CT_FW_CONN_INSTALLING)
		ct_fw_conn_drop_held(worker, conn);
}

static void drain_pending_replay(struct ct_fw_worker *worker)
{
	uint32_t i;

	for (i = 0; i < worker->nb_pending_replay && !ct_fw_failed(worker); i++) {
		struct ct_fw_conn *conn = ct_fw_conn_lookup(&worker->table, &worker->pending_replay[i]);

		/* A released slot already freed its clone. */
		if (conn == NULL || conn->free_pending || conn->state != CT_FW_CONN_OFFLOADED)
			continue;
		if (conn->remove_requested)
			ct_fw_conn_drop_held(worker, conn);
		else
			replay_held_clone(worker, conn);
	}
	worker->nb_pending_replay = 0;
}

static void dispatch_tracked(struct ct_fw_worker *worker,
			     struct rte_mbuf *m,
			     struct ct_fw_conn *conn,
			     const struct ct_fw_parsed_pkt *pkt,
			     bool is_reply,
			     struct ct_fw_verdict *v);

/* Counted until both directions are seen; each direction seeds its own side. */
static void handle_adopt(struct ct_fw_worker *worker,
			 struct rte_mbuf *m,
			 const struct ct_fw_parsed_pkt *pkt,
			 struct ct_fw_verdict *v)
{
	struct ct_fw_conn *conn;

	if (counting_full(worker)) {
		refuse_untracked(worker, v);
		return;
	}
	conn = ct_fw_conn_open(worker, &pkt->key);
	if (conn == NULL) {
		refuse_untracked(worker, v);
		return;
	}
	conn->state = CT_FW_CONN_COUNTING;
	conn->install_mode = CT_FW_INSTALL_ADOPT;
	conn->last_seen_tsc = worker->now_tsc;
	conn->origin_port_code = ct_fw_mbuf_port_code(m);
	/* A reply resolves here via its translated tuple. */
	ct_fw_conn_reply_index_add(&worker->table, conn);
	record_obs(&conn->syn, pkt);
	worker->counting++;
	/* The first segment's payload counts too. */
	dispatch_tracked(worker, m, conn, pkt, false, v);
}

/* Queued since the entry went live or was re-enabled: until a burst polled after the settle time empties the queue,
 * or a ring and a burst are read after it. */
static bool backlog_open(const struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (conn->backlog_burst == 0)
		return false;
	/* Misses still in flight: wait the settle time, then count a ring and a burst from there. */
	if (!conn->backlog_settled) {
		if (rte_get_timer_cycles() - conn->backlog_tsc < worker->backlog_settle)
			return true;
		conn->backlog_settled = true;
		conn->backlog_rx = worker->stats.rx_packets;
	}
	/* A short burst counts only if it polled after the settle time (if any). */
	if ((worker->rx_drained > conn->backlog_burst &&
	     (worker->backlog_settle == 0 ||
	      (int64_t)(worker->rx_drained_tsc - conn->backlog_tsc) >= (int64_t)worker->backlog_settle)) ||
	    worker->stats.rx_packets - conn->backlog_rx >= CT_FW_BACKLOG_PKTS)
		conn->backlog_burst = 0;
	return conn->backlog_burst != 0;
}

/* Hardware misses data forwarded around a re-enable (owed, in flight) or queued before an entry went live. */
static bool data_guard(const struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (conn->state != CT_FW_CONN_OFFLOADED)
		return false;
	if (conn->reenable_owed || conn->wv_op_inflight)
		return true;
	return backlog_open(worker, conn);
}

/* --sync: rewrite a disabled entry now; a refusal leaves the re-enable owed. */
static void reenable_sync(struct ct_fw_worker *worker,
			  struct ct_fw_conn *conn,
			  const struct doca_flow_ct_tcp_window_validation *queried)
{
	bool rewritten = false;

	conn->reenable_owed = ct_fw_conn_reenable(worker, conn, queried, &rewritten) != DOCA_SUCCESS;
	if (rewritten)
		ct_fw_reenable_landed(worker, conn);
}

/* A SYN on a tuple with an entry: retry an owed removal, else query; a closed entry is removed, a live one kept. */
static void reuse_check(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	struct doca_flow_ct_tcp_window_validation prof;

	if (conn->remove_requested) {
		if (conn->entry == NULL || ct_fw_conn_teardown_entry(worker, conn))
			ct_fw_conn_release(worker, conn);
		return;
	}
	if (worker->app->config.sync_mode != CT_FW_SYNC) {
		ct_fw_conn_reenable_async(worker, conn, true);
		return;
	}
	if (ct_fw_conn_query_profile(worker, conn, &prof) != DOCA_SUCCESS)
		conn->reenable_owed = true;
	else if (!ct_fw_profile_closed(&prof))
		reenable_sync(worker, conn, &prof);
	else if (ct_fw_conn_teardown_entry(worker, conn))
		ct_fw_conn_release(worker, conn);
}

static void handle_tracked(struct ct_fw_worker *worker,
			   struct rte_mbuf *m,
			   struct ct_fw_conn *conn,
			   const struct ct_fw_parsed_pkt *pkt,
			   bool is_reply,
			   struct ct_fw_verdict *v)
{
	if (conn->state == CT_FW_CONN_HALF_OPEN) {
		bool bare_syn;

		/* Only the opener's SYN or a matching RST passes. */
		(void)ct_fw_tcp_opener_allowed(pkt->tcp_flags, &bare_syn);
		if (!(bare_syn && !is_reply) &&
		    !((pkt->tcp_flags & RTE_TCP_RST_FLAG) && ct_fw_halfopen_rst_ok(&conn->syn, pkt, is_reply))) {
			refuse(v, &worker->stats.state_drops);
			return;
		}
		/* A new SYN may bring a new ISN; under --snat only the owner's. */
		if (bare_syn && !is_reply) {
			struct ct_fw_tcp_obs seg;

			if (snat_refused(worker, m, pkt, v))
				return;
			record_obs(&seg, pkt);
			ct_fw_syn_refresh(&conn->syn, &seg);
		}
	}
	conn->last_seen_tsc = worker->now_tsc;

	/* Any SYN on a tuple that still has an entry is refused until that entry is removed (the peer retransmits). */
	if (conn->state == CT_FW_CONN_OFFLOADED && (pkt->tcp_flags & RTE_TCP_SYN_FLAG) &&
	    (conn->remove_requested || worker->app->config.enable_window_validation)) {
		reuse_check(worker, conn);
		refuse(v, &worker->stats.reuse_drops);
		return;
	}

	/* Removal owed: retry it. */
	if (conn->state == CT_FW_CONN_OFFLOADED && conn->remove_requested) {
		bool removed = conn->entry == NULL || ct_fw_conn_teardown_entry(worker, conn);

		admit(worker, v, conn, is_reply);
		if (removed)
			ct_fw_conn_release(worker, conn);
		return;
	}

	if (conn->state == CT_FW_CONN_COUNTING) {
		struct ct_fw_tcp_obs seg;
		bool bare_syn;

		if (pkt->tcp_flags & RTE_TCP_RST_FLAG) {
			admit(worker, v, conn, is_reply);
			ct_fw_conn_release(worker, conn);
			return;
		}
		/* After both FINs only a new opener SYN reopens the tuple. */
		if (conn->fin_dirs == CT_FW_COUNT_DIR_BOTH && !is_reply &&
		    ct_fw_tcp_opener_allowed(pkt->tcp_flags, &bare_syn) && bare_syn) {
			if (snat_refused(worker, m, pkt, v))
				return;
			ct_fw_conn_release(worker, conn);
			handle_new_syn(worker, m, pkt, v);
			snat_claim(worker, m, pkt, v);
			return;
		}

		record_obs(&seg, pkt);
		/* At the window-validation slot limit the install waits: forwarded as while counting, the next segment
		 * retries. */
		if (ct_fw_conn_count_segment(conn, &seg, is_reply, worker->app->config.offload_after) &&
		    ct_fw_install_admitted(worker, &worker->stats.wv_limit_defers)) {
			doca_error_t result;

			/* Hardware state must catch up with the stream. */
			if (conn->install_mode == CT_FW_INSTALL_ADOPT)
				hold_reinject_clone(worker, conn, m);
			result = ct_fw_conn_install_midstream(worker, conn, conn->install_mode, is_reply);
			if (ct_fw_failed(worker)) {
				refuse(v, NULL);
				return;
			}
			/* The release paths free the clone anyway. */
			replay_or_release(worker, conn);
			if (result != DOCA_SUCCESS) {
				refuse_uninstalled(worker, v, true, conn, result, false);
				return;
			}
			/* --sync: segments queued during the wait missed the entry too. */
			if (conn->state == CT_FW_CONN_OFFLOADED && worker->app->config.sync_mode == CT_FW_SYNC)
				ct_fw_backlog_arm(worker, conn);
		}
		admit(worker, v, conn, is_reply);
		return;
	}

	/* Until the entry is live only an RST passes. */
	if ((conn->state == CT_FW_CONN_INSTALLING || conn->state == CT_FW_CONN_ADOPTING) &&
	    !(pkt->tcp_flags & RTE_TCP_RST_FLAG)) {
		refuse(v, &worker->stats.install_drops);
		return;
	}

	/* Data and FIN wait for the re-enable and the backlog; each one retries an owed re-enable. */
	if ((pkt->payload_len != 0 || (pkt->tcp_flags & RTE_TCP_FIN_FLAG)) && !(pkt->tcp_flags & RTE_TCP_RST_FLAG) &&
	    data_guard(worker, conn)) {
		if (conn->reenable_owed && !conn->wv_op_inflight) {
			if (worker->app->config.sync_mode == CT_FW_SYNC)
				reenable_sync(worker, conn, NULL);
			else
				ct_fw_conn_reenable_async(worker, conn, false);
		}
		refuse(v, &worker->stats.install_drops);
		return;
	}

	/* RST tears down at once; FIN is left to hardware and aging. */
	if (pkt->tcp_flags & RTE_TCP_RST_FLAG) {
		/* The install completion tears it down. */
		if (conn->state == CT_FW_CONN_INSTALLING || conn->state == CT_FW_CONN_ADOPTING) {
			conn->remove_requested = true;
			admit(worker, v, conn, is_reply);
			return;
		}
		/* Removal owed: keep the slot; a SYN-ACK waiting for replay is never sent. */
		if (conn->state == CT_FW_CONN_OFFLOADED && conn->entry != NULL &&
		    !ct_fw_conn_teardown_entry(worker, conn)) {
			ct_fw_conn_drop_held(worker, conn);
			admit(worker, v, conn, is_reply);
			return;
		}
		admit(worker, v, conn, is_reply);
		ct_fw_conn_release(worker, conn);
		return;
	}

	/* No verdict in the punt: query, re-enable only if disabled. Fragments always reach software, so they are no
	 * sign of a disabled entry. */
	if (conn->state == CT_FW_CONN_OFFLOADED && !(pkt->tcp_flags & RTE_TCP_FIN_FLAG) && !pkt->from_fragments &&
	    worker->app->config.enable_window_validation) {
		if (worker->app->config.sync_mode == CT_FW_SYNC)
			reenable_sync(worker, conn, NULL);
		else
			ct_fw_conn_reenable_async(worker, conn, false);
	}
	admit(worker, v, conn, is_reply);
}

static void dispatch_tracked(struct ct_fw_worker *worker,
			     struct rte_mbuf *m,
			     struct ct_fw_conn *conn,
			     const struct ct_fw_parsed_pkt *pkt,
			     bool is_reply,
			     struct ct_fw_verdict *v)
{
	bool synack = (pkt->tcp_flags & RTE_TCP_SYN_FLAG) && (pkt->tcp_flags & RTE_TCP_ACK_FLAG);

	if (conn->state == CT_FW_CONN_HALF_OPEN && is_reply && synack) {
		if (!ct_fw_tcp_synack_allowed(pkt->tcp_flags)) {
			refuse(v, &worker->stats.state_drops);
			return;
		}
		handle_synack(worker, m, conn, pkt, v);
	} else {
		handle_tracked(worker, m, conn, pkt, is_reply, v);
	}
}

/* No handshake: install at once. */
static void handle_new_udp(struct ct_fw_worker *worker,
			   struct rte_mbuf *m,
			   const struct ct_fw_parsed_pkt *pkt,
			   struct ct_fw_verdict *v)
{
	struct ct_fw_conn *conn;
	doca_error_t result;

	conn = ct_fw_conn_open(worker, &pkt->key);
	if (conn == NULL) {
		refuse_untracked(worker, v);
		return;
	}
	conn->last_seen_tsc = worker->now_tsc;
	conn->origin_port_code = ct_fw_mbuf_port_code(m);
	ct_fw_conn_reply_index_add(&worker->table, conn);

	/* Admit first: the install may release the slot. */
	admit(worker, v, conn, false);
	result = ct_fw_conn_install_udp(worker, conn);
	if (ct_fw_failed(worker)) {
		refuse(v, NULL);
		return;
	}
	/* On success the slot stays, so an early datagram adds no second entry. */
	if (result != DOCA_SUCCESS)
		refuse_uninstalled(worker, v, true, conn, result, false);
}

static void handle_udp_tracked(struct ct_fw_worker *worker,
			       struct ct_fw_conn *conn,
			       bool is_reply,
			       struct ct_fw_verdict *v)
{
	admit(worker, v, conn, is_reply);
	if (conn->state == CT_FW_CONN_OFFLOADED && conn->remove_requested && ct_fw_conn_teardown_entry(worker, conn))
		ct_fw_conn_release(worker, conn);
}

void ct_fw_drain_completions(struct ct_fw_worker *worker)
{
	/* Owed adds first: oldest before new completions. */
	ct_fw_retry_owed_adds(worker);
	ct_fw_wv_drain(worker);
	drain_pending_replay(worker);
}

void ct_fw_judge_packet(struct ct_fw_worker *worker,
			struct rte_mbuf *m,
			const struct ct_fw_parsed_pkt *pkt,
			struct ct_fw_verdict *v)
{
	struct ct_fw_conn *conn;
	bool is_reply = false;
	bool wrong_uplink;

	memset(v, 0, sizeof(*v));
	v->outcome = CT_FW_DROP;
	/* A reassembled segment never opens or resets. */
	if (pkt->from_fragments && pkt->is_tcp && (pkt->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_RST_FLAG)))
		return;

	/* Refused packets do not count as activity. */
	conn = ct_fw_conn_find_bidir(&worker->table, &pkt->key, ct_fw_mbuf_port_code(m), &is_reply, &wrong_uplink);
	/* Matched only on the wrong uplink: not admitted, the connection left untouched. */
	if (wrong_uplink) {
		refuse(v, &worker->stats.wrong_uplink_drops);
		return;
	}
	if (refuse_if_retained(worker, conn, v))
		return;
	v->was_live = conn != NULL && conn->state == CT_FW_CONN_OFFLOADED;

	if (!pkt->is_tcp) {
		if (conn == NULL) {
			if (!snat_refused(worker, m, pkt, v)) {
				handle_new_udp(worker, m, pkt, v);
				snat_claim(worker, m, pkt, v);
			}
		} else {
			conn->last_seen_tsc = worker->now_tsc;
			handle_udp_tracked(worker, conn, is_reply, v);
		}
		return;
	}

	if (conn == NULL) {
		bool is_new_syn;

		/* Fragments never adopt. */
		if (pkt->from_fragments)
			return;
		if (!ct_fw_tcp_opener_allowed(pkt->tcp_flags, &is_new_syn))
			refuse(v, &worker->stats.state_drops);
		else if (snat_refused(worker, m, pkt, v))
			return;
		else if (is_new_syn)
			handle_new_syn(worker, m, pkt, v);
		else
			handle_adopt(worker, m, pkt, v);
		snat_claim(worker, m, pkt, v);
		return;
	}

	dispatch_tracked(worker, m, conn, pkt, is_reply, v);
}

/* Replay so hardware sees the connection: TCP advances the window, both refresh aging. */
static bool reinject_reassembled(struct ct_fw_worker *worker, const struct ct_fw_parsed_pkt *pkt, struct rte_mbuf *w)
{
	struct ct_fw_conn *conn;
	bool is_reply, wrong_uplink;

	if (ct_fw_failed(worker))
		return false;
	conn = ct_fw_conn_find_bidir(&worker->table, &pkt->key, ct_fw_mbuf_port_code(w), &is_reply, &wrong_uplink);
	return conn != NULL && !wrong_uplink && conn->state == CT_FW_CONN_OFFLOADED && conn->entry != NULL &&
	       !conn->remove_requested;
}

/* The originals follow the reassembled packet's verdict. */
static void frag_judge(struct ct_fw_worker *worker, struct ct_fw_frag_dgram *dg)
{
	struct ct_fw_parsed_pkt pkt;
	struct ct_fw_verdict v;
	uint32_t over_len = 0;
	struct rte_mbuf *w = ct_fw_frag_wrap(dg, dg->frag[0].m->pool, &over_len);
	uint8_t i;

	if (w == NULL && over_len != 0) {
		if (ct_fw_log_due(worker->now_tsc, &worker->frags.len_log_tsc))
			DOCA_LOG_WARN("Worker %u: reassembled IPv4 length %u exceeds %u; datagram dropped",
				      worker->worker_id,
				      over_len,
				      (unsigned int)CT_FW_FRAG_MAX_LEN);
	} else if (w == NULL && ct_fw_log_due(worker->now_tsc, &worker->frags.buf_log_tsc)) {
		DOCA_LOG_WARN(
			"Worker %u: reassembly-buffer construction failed (%u of %u buffers free); datagram dropped",
			worker->worker_id,
			rte_mempool_avail_count(dg->frag[0].m->pool),
			dg->frag[0].m->pool->size);
	}
	if (w == NULL || !ct_fw_parse_packet(w, dg->key.zone, worker->app->has_ipv6_ct, &pkt) || !pkt.valid) {
		rte_pktmbuf_free(w);
		ct_fw_frag_drop(&worker->frags, dg, &worker->stats.frag_drops);
		return;
	}
	pkt.from_fragments = true;
	ct_fw_judge_packet(worker, w, &pkt, &v);
	/* Nothing keeps a reassembled packet (no SYN or SYN-ACK gets here); a release build drops the fragments and
	 * leaves w to its owner. */
	assert(v.outcome != CT_FW_HELD && v.outcome != CT_FW_CONSUMED);
	if (v.outcome == CT_FW_HELD || v.outcome == CT_FW_CONSUMED) {
		ct_fw_frag_drop(&worker->frags, dg, &worker->stats.frag_drops);
		return;
	}
	if (v.outcome != CT_FW_FORWARD) {
		rte_pktmbuf_free(w);
		ct_fw_frag_drop(&worker->frags, dg, &worker->stats.frag_drops);
		return;
	}
	for (i = 0; i < dg->nb; i++) {
		ct_fw_emit(worker, dg->frag[i].m, &v);
		dg->frag[i].m = NULL;
	}
	CT_FW_STAT_INC(worker->stats.frag_reassembled);
	/* The datagram that installs is not replayed (an adoption replays its own trigger). The replay is the whole
	 * datagram (up to CT_FW_FRAG_MAX_LEN), so it can exceed the port MTU; the send path must take it to the egress
	 * root, where it is dropped after CT and never sent on the wire. */
	if (v.was_live && reinject_reassembled(worker, &pkt, w)) {
		mark_reinject(worker, w, dg->key.zone);
		reinject_tx(worker, w);
	} else {
		rte_pktmbuf_free(w);
	}
	ct_fw_frag_done(&worker->frags, dg);
}

static void frag_input(struct ct_fw_worker *worker, struct rte_mbuf *m)
{
	struct ct_fw_frag_info info;
	struct ct_fw_frag_dgram *dg;

	if (!ct_fw_frag_parse(m, ct_fw_mbuf_port_code(m), &info)) {
		if (info.over_len != 0 && ct_fw_log_due(worker->now_tsc, &worker->frags.len_log_tsc))
			DOCA_LOG_WARN("Worker %u: dropped a fragment reaching %u bytes of its datagram (limit %u)",
				      worker->worker_id,
				      info.over_len,
				      (unsigned int)CT_FW_FRAG_MAX_LEN);
		CT_FW_STAT_INC(worker->stats.frag_drops);
		rte_pktmbuf_free(m);
		return;
	}
	if (ct_fw_frag_insert(&worker->frags, &info, m, worker->now_tsc, &worker->stats.frag_drops, &dg) ==
	    CT_FW_FRAG_COMPLETE)
		frag_judge(worker, dg);
}

void ct_fw_process_packet(struct ct_fw_worker *worker, struct rte_mbuf *m)
{
	struct ct_fw_parsed_pkt pkt;
	struct ct_fw_verdict v;

	CT_FW_STAT_INC(worker->stats.rx_packets);
	/* ARP and ICMP are translated under IPv4 NAT, else forwarded untracked. */
	if (ct_fw_frame_is_other(rte_pktmbuf_mtod(m, const uint8_t *),
				 rte_pktmbuf_mtod(m, const uint8_t *) + rte_pktmbuf_data_len(m))) {
		if (ct_fw_nat_other(worker, m))
			return;
		CT_FW_STAT_INC(worker->stats.other_forwarded);
		admit_untranslated(&v);
		ct_fw_emit(worker, m, &v);
		return;
	}
	if (!ct_fw_parse_packet(m, mbuf_zone(m), worker->app->has_ipv6_ct, &pkt) || !pkt.valid) {
		if (pkt.is_fragment && pkt.key.family == CT_FW_FAMILY_IPV4) {
			frag_input(worker, m);
			return;
		}
		if (pkt.is_fragment)
			CT_FW_STAT_INC(worker->stats.frag_drops);
		else
			CT_FW_STAT_INC(worker->stats.parse_drops);
		rte_pktmbuf_free(m);
		return;
	}
	ct_fw_judge_packet(worker, m, &pkt, &v);
	ct_fw_emit(worker, m, &v);
}

bool ct_fw_worker_service(struct ct_fw_worker *worker, const struct ct_fw_loop_cycles *cyc)
{
	struct ct_fw_ctx *app = worker->app;

	/* No handler holds a connection pointer now. */
	ct_fw_conn_drain_pending_free(worker);
	if (worker->nb_owed_rm != 0)
		ct_fw_retry_owed_rm(worker);

	ct_fw_queue_poll(worker);
	ct_fw_drain_completions(worker);
	if (ct_fw_failed(worker))
		return false;
	/* The round's sweep is spread over loop iterations, one slice each. */
	if (worker->sweep_left != 0) {
		ct_fw_conn_sweep(worker, worker->now_tsc, cyc->idle, cyc->unconfirmed, cyc->watchdog);
		if (ct_fw_failed(worker))
			return false;
	}

	if (worker->now_tsc >= worker->sweep_deadline_tsc) {
		uint32_t calls;

		/* A late round is not caught up. */
		worker->sweep_deadline_tsc = worker->now_tsc + cyc->sweep;
		for (calls = 0; calls < CT_FW_AGING_CALLS_PER_ROUND; calls++) {
			int aged;

			ct_fw_queue_poll(worker);
			if (ct_fw_failed(worker))
				break;
			aged = doca_flow_aging_handle(app->ports[0],
						      worker->ct_queue,
						      CT_FW_AGING_QUOTA_US,
						      CT_FW_AGING_BURST);
			ct_fw_conn_drain_pending_free(worker);
			if (worker->nb_owed_rm != 0)
				ct_fw_retry_owed_rm(worker);
			ct_fw_drain_completions(worker);
			if (aged < CT_FW_AGING_CYCLE_DONE)
				DOCA_LOG_DBG("Aging handle returned %d", aged);
			if (aged < 0 || ct_fw_failed(worker))
				break; /* 0 may mean a new cycle started */
		}
		if (ct_fw_failed(worker))
			return false;
		(void)ct_fw_frag_expire(&worker->frags, worker->now_tsc, cyc->frag, &worker->stats.frag_drops);
		ct_fw_conn_sweep_round(worker, worker->now_tsc, cyc->watchdog);
	}
	return !ct_fw_failed(worker);
}

int ct_fw_worker_loop(void *arg)
{
	struct ct_fw_worker *worker = arg;
	struct ct_fw_ctx *app = worker->app;
	struct rte_mbuf *pkts[CT_FW_PACKET_BURST];
	uint64_t hz = rte_get_timer_hz();
	uint32_t unconfirmed_s = ct_fw_unconfirmed_timeout_s(app->config.aging_timeout_s);
	struct ct_fw_loop_cycles cyc = {
		.idle = hz * app->config.aging_timeout_s,
		.unconfirmed = hz * unconfirmed_s,
		.sweep = hz * CT_FW_SWEEP_INTERVAL_S,
		.watchdog = hz * CT_FW_OP_WATCHDOG_S,
		.frag = hz * CT_FW_FRAG_TIMEOUT_S,
	};

	worker->sweep_budget = ct_fw_sweep_budget(worker->table.capacity, unconfirmed_s);
	worker->backlog_settle = hz * CT_FW_BACKLOG_SETTLE_US / 1000000;

	DOCA_LOG_INFO("Worker %u started on lcore %u (CT queue %u, RX queue %u)",
		      worker->worker_id,
		      worker->lcore,
		      worker->ct_queue,
		      worker->rss_queue);

	worker->now_tsc = rte_get_timer_cycles();
	worker->sweep_deadline_tsc = worker->now_tsc + cyc.sweep;
	ct_fw_worker_async_init(worker);

	while (!*app->force_quit) {
		uint64_t poll_tsc = rte_get_timer_cycles();
		uint16_t nb_rx = rte_eth_rx_burst(SW_DPDK_PORT, worker->rss_queue, pkts, CT_FW_PACKET_BURST);
		uint16_t i;

		worker->now_tsc = rte_get_timer_cycles();
		worker->rx_bursts++;
		for (i = 0; i < nb_rx && !ct_fw_failed(worker); i++)
			ct_fw_process_packet(worker, pkts[i]);
		if (nb_rx < CT_FW_PACKET_BURST) {
			worker->rx_drained = worker->rx_bursts;
			worker->rx_drained_tsc = poll_tsc;
		}
		/* After a queue failure, free the rest. */
		if (ct_fw_failed(worker)) {
			rte_pktmbuf_free_bulk(&pkts[i], nb_rx - i);
			break;
		}
		if (!ct_fw_worker_service(worker, &cyc))
			break;
	}

	DOCA_LOG_INFO("Worker %u stopping: rx %" PRIu64 " tx %" PRIu64 " installed %" PRIu64,
		      worker->worker_id,
		      worker->stats.rx_packets,
		      worker->stats.tx_packets,
		      worker->stats.conns_installed);
	return 0;
}
