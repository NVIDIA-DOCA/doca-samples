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

/* CT Firewall ARP and ICMP under IPv4 NAT. */

#include <string.h>
#include <arpa/inet.h>

#include <rte_arp.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_icmp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include <doca_log.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::NAT_OTHER);

#define ICMP_ERROR_QUOTE_MIN 8 /* L4 bytes an ICMP error quotes at least */
#define TCP_CKSUM_END 18       /* quoted TCP bytes that hold the checksum */

/* IPv4 --dnat and --snat; 0 when not set. */
struct nat_v4 {
	rte_be32_t real;
	rte_be32_t snat;
};

static struct nat_v4 nat_v4_of(const struct ct_fw_config *cfg)
{
	struct nat_v4 n;

	memcpy(&n.real, cfg->nat[CT_FW_FAMILY_IPV4].new_dst, sizeof(n.real));
	memcpy(&n.snat, cfg->nat[CT_FW_FAMILY_IPV4].new_src, sizeof(n.snat));
	return n;
}

static void emit_outcome(struct ct_fw_worker *worker, struct rte_mbuf *m, enum ct_fw_outcome outcome, uint64_t *ctr)
{
	struct ct_fw_verdict v;

	memset(&v, 0, sizeof(v));
	v.outcome = outcome;
	v.drop_ctr = ctr;
	ct_fw_emit(worker, m, &v);
}

static void nat_drop(struct ct_fw_worker *worker, struct rte_mbuf *m, const char *why)
{
	DOCA_LOG_DBG("Dropped under NAT: %s", why);
	emit_outcome(worker, m, CT_FW_DROP, &worker->stats.nat_other_drops);
}

/* Sent as it is now. */
static void forward_other(struct ct_fw_worker *worker, struct rte_mbuf *m)
{
	CT_FW_STAT_INC(worker->stats.other_forwarded);
	emit_outcome(worker, m, CT_FW_FORWARD_UNTRANSLATED, NULL);
}

static void arp_copy_failed(struct ct_fw_worker *worker)
{
	CT_FW_STAT_INC(worker->stats.nat_other_drops);
	if (ct_fw_log_due(worker->now_tsc, &worker->nat_other.fail_log_tsc))
		DOCA_LOG_WARN("Dropped a copy of an ARP reply under NAT: no packet buffer");
}

/* Sends a copy as it is now; a failed copy is counted as a drop. */
static void forward_copy(struct ct_fw_worker *worker, struct rte_mbuf *m)
{
	struct rte_mbuf *copy = rte_pktmbuf_copy(m, m->pool, 0, UINT32_MAX);

	if (copy == NULL)
		arp_copy_failed(worker);
	else
		emit_outcome(worker, copy, CT_FW_FORWARD_UNTRANSLATED, NULL);
}

static const char *ip4_str(rte_be32_t ip, char *buf, socklen_t len)
{
	const char *s = inet_ntop(AF_INET, &ip, buf, len);

	return s != NULL ? s : "?";
}

static struct ct_fw_snat_owner *snat_owner_find(struct ct_fw_nat_other *st, uint32_t zone)
{
	uint32_t i;

	for (i = 0; i < CT_FW_MAX_SNAT_OWNERS; i++)
		if (st->owner[i].in_use && st->owner[i].zone == zone)
			return &st->owner[i];
	return NULL;
}

/* At most one refusal warning per second. */
static void snat_refuse_log(struct ct_fw_worker *worker,
			    uint32_t zone,
			    const struct rte_ether_addr *mac,
			    rte_be32_t src_ip,
			    const struct ct_fw_snat_owner *owner)
{
	struct ct_fw_nat_other *st = &worker->nat_other;
	char ip[INET_ADDRSTRLEN], own_ip[INET_ADDRSTRLEN];

	if (!ct_fw_log_due(worker->now_tsc, &st->refuse_log_tsc))
		return;
	if (owner == NULL) {
		DOCA_LOG_WARN("--snat: dropped traffic from " RTE_ETHER_ADDR_PRT_FMT
			      " (%s) in zone %u: too many zones (limit %u)",
			      RTE_ETHER_ADDR_BYTES(mac),
			      ip4_str(src_ip, ip, sizeof(ip)),
			      zone,
			      CT_FW_MAX_SNAT_OWNERS);
		return;
	}
	DOCA_LOG_WARN("--snat: dropped traffic from " RTE_ETHER_ADDR_PRT_FMT
		      " (%s) in zone %u: the address belongs to " RTE_ETHER_ADDR_PRT_FMT " (%s)",
		      RTE_ETHER_ADDR_BYTES(mac),
		      ip4_str(src_ip, ip, sizeof(ip)),
		      zone,
		      RTE_ETHER_ADDR_BYTES(&owner->mac),
		      ip4_str(owner->ip, own_ip, sizeof(own_ip)));
}

static bool pending_live(const struct ct_fw_worker *worker, const struct ct_fw_arp_pending *p)
{
	return p->in_use && worker->now_tsc - p->tsc <= (uint64_t)CT_FW_ARP_PENDING_S * rte_get_timer_hz();
}

/* Eviction rank of a slot for a new request: free or expired first, then answered, then unanswered. */
static int pending_rank(const struct ct_fw_worker *worker, const struct ct_fw_arp_pending *p)
{
	if (!pending_live(worker, p))
		return 0;
	return p->answered ? 1 : 2;
}

/* One slot per (zone, client, asked); a repeated request re-arms its slot. A full table evicts an answered request
 * (the oldest) before an unanswered one (the oldest). */
static void pending_add(struct ct_fw_worker *worker,
			struct ct_fw_arp_pending *table,
			uint32_t zone,
			rte_be32_t client,
			rte_be32_t asked)
{
	struct ct_fw_arp_pending *slot = NULL;
	uint32_t i;

	for (i = 0; i < CT_FW_ARP_PENDING; i++) {
		struct ct_fw_arp_pending *p = &table[i];

		if (pending_live(worker, p) && p->zone == zone && p->client == client && p->asked == asked) {
			slot = p;
			break;
		}
		if (slot == NULL || pending_rank(worker, p) < pending_rank(worker, slot) ||
		    (pending_rank(worker, p) == pending_rank(worker, slot) && p->tsc < slot->tsc))
			slot = p;
	}
	memset(slot, 0, sizeof(*slot));
	slot->zone = zone;
	slot->client = client;
	slot->asked = asked;
	slot->tsc = worker->now_tsc;
	slot->in_use = true;
}

static bool pending_find(const struct ct_fw_worker *worker,
			 const struct ct_fw_arp_pending *table,
			 uint32_t zone,
			 rte_be32_t client,
			 rte_be32_t asked)
{
	uint32_t i;

	for (i = 0; i < CT_FW_ARP_PENDING; i++)
		if (pending_live(worker, &table[i]) && table[i].zone == zone && table[i].client == client &&
		    table[i].asked == asked)
			return true;
	return false;
}

/* One reply copy per unanswered address the client asked for. Answered requests stay until they expire, so a
 * duplicate reply (the server answers each forwarded request) is freed without counting as a drop. */
static void arp_answer_pending(struct ct_fw_worker *worker, struct rte_mbuf *m, struct rte_arp_hdr *arp, uint32_t zone)
{
	struct ct_fw_arp_pending *match[CT_FW_ARP_PENDING];
	rte_be32_t client = arp->arp_data.arp_tip;
	uint32_t nb = 0, nb_answered = 0;
	uint32_t i;

	for (i = 0; i < CT_FW_ARP_PENDING; i++) {
		struct ct_fw_arp_pending *p = &worker->nat_other.pending[i];

		if (!pending_live(worker, p) || p->zone != zone || p->client != client)
			continue;
		if (p->answered)
			nb_answered++;
		else
			match[nb++] = p;
	}
	if (nb == 0 && nb_answered == 0) {
		nat_drop(worker, m, "ARP reply to no forwarded request");
		return;
	}
	if (nb == 0) {
		DOCA_LOG_DBG("Duplicate ARP reply under NAT freed");
		emit_outcome(worker, m, CT_FW_DROP, NULL);
		return;
	}
	CT_FW_STAT_INC(worker->stats.other_forwarded);
	for (i = 0; i < nb; i++) {
		struct rte_mbuf *out = i + 1 == nb ? m : rte_pktmbuf_copy(m, m->pool, 0, UINT32_MAX);
		struct rte_arp_hdr *oa;

		/* No buffer: keep the request unanswered. */
		if (out == NULL) {
			arp_copy_failed(worker);
			continue;
		}
		oa = (struct rte_arp_hdr *)(rte_pktmbuf_mtod(out, uint8_t *) +
					    ((uint8_t *)arp - rte_pktmbuf_mtod(m, uint8_t *)));
		oa->arp_data.arp_sip = match[i]->asked;
		emit_outcome(worker, out, CT_FW_FORWARD_UNTRANSLATED, NULL);
		match[i]->answered = true;
	}
}

/* The server's ARP for --snat follows the owner's latest ARP. */
static void snat_owner_set_ip(struct ct_fw_worker *worker, struct ct_fw_snat_owner *owner, rte_be32_t ip)
{
	char buf[INET_ADDRSTRLEN];

	if (owner->ip == ip)
		return;
	owner->ip = ip;
	if (ct_fw_log_due(worker->now_tsc, &owner->ip_log_tsc))
		DOCA_LOG_INFO("--snat: client " RTE_ETHER_ADDR_PRT_FMT " in zone %u now answers ARP at %s",
			      RTE_ETHER_ADDR_BYTES(&owner->mac),
			      owner->zone,
			      ip4_str(ip, buf, sizeof(buf)));
}

/* IPv4-over-Ethernet ARP only. */
static void nat_arp_v4(struct ct_fw_worker *worker, struct rte_mbuf *m, const uint8_t *l3, uint32_t zone)
{
	const uint8_t *data_end = rte_pktmbuf_mtod(m, const uint8_t *) + rte_pktmbuf_data_len(m);
	struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(uintptr_t)l3;
	struct nat_v4 nat = nat_v4_of(&worker->app->config);
	bool snat = ct_fw_snat_enabled(&worker->app->config, CT_FW_PORT_CODE(CT_FW_FIRST_UPLINK));
	const struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
	struct ct_fw_snat_owner *owner;
	uint16_t op;

	if (l3 + sizeof(*arp) > data_end || arp->arp_hardware != rte_cpu_to_be_16(RTE_ARP_HRD_ETHER) ||
	    arp->arp_protocol != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) || arp->arp_hlen != RTE_ETHER_ADDR_LEN ||
	    arp->arp_plen != sizeof(rte_be32_t)) {
		forward_other(worker, m);
		return;
	}
	op = rte_be_to_cpu_16(arp->arp_opcode);

	if (ct_fw_mbuf_port_code(m) == CT_FW_PORT_CODE(CT_FW_FIRST_UPLINK)) {
		rte_be32_t sender = arp->arp_data.arp_sip;

		/* Probes and gratuitous ARP pass unchanged; the owner's gratuitous ARP names its address. */
		if (sender == 0 || sender == arp->arp_data.arp_tip) {
			owner = snat ? snat_owner_find(&worker->nat_other, zone) : NULL;
			if (sender != 0 && owner != NULL && rte_is_same_ether_addr(&owner->mac, &eth->src_addr))
				snat_owner_set_ip(worker, owner, sender);
			forward_other(worker, m);
			return;
		}
		if (op == RTE_ARP_OP_REQUEST) {
			if (snat) {
				if (!ct_fw_snat_owner_allowed(worker, zone, &eth->src_addr, sender)) {
					nat_drop(worker, m, "ARP from a second --snat client");
					return;
				}
				owner = ct_fw_snat_owner_claim(worker, zone, &eth->src_addr, sender);
				snat_owner_set_ip(worker, owner, sender);
				arp->arp_data.arp_sip = nat.snat;
			}
			if (nat.real != 0) {
				pending_add(worker, worker->nat_other.pending, zone, sender, arp->arp_data.arp_tip);
				arp->arp_data.arp_tip = nat.real;
			}
		} else if (op == RTE_ARP_OP_REPLY && snat) {
			owner = snat_owner_find(&worker->nat_other, zone);
			if (owner != NULL && rte_is_same_ether_addr(&owner->mac, &eth->src_addr) &&
			    sender == owner->ip) {
				/* A host that asked for the client's own address also gets it unchanged. */
				if (pending_find(worker,
						 worker->nat_other.own_asked,
						 zone,
						 arp->arp_data.arp_tip,
						 sender))
					forward_copy(worker, m);
				arp->arp_data.arp_sip = nat.snat;
			}
		}
		forward_other(worker, m);
		return;
	}

	/* From the server side: the --snat address stands for its client host. */
	if (snat && arp->arp_data.arp_tip == nat.snat) {
		owner = snat_owner_find(&worker->nat_other, zone);
		if (owner == NULL) {
			nat_drop(worker, m, "ARP for the --snat address before any client used it");
			return;
		}
		arp->arp_data.arp_tip = owner->ip;
	} else if (snat && op == RTE_ARP_OP_REQUEST) {
		owner = snat_owner_find(&worker->nat_other, zone);
		if (owner != NULL && arp->arp_data.arp_tip == owner->ip)
			pending_add(worker,
				    worker->nat_other.own_asked,
				    zone,
				    arp->arp_data.arp_sip,
				    arp->arp_data.arp_tip);
	}
	if (op == RTE_ARP_OP_REPLY && nat.real != 0) {
		if (arp->arp_data.arp_sip != nat.real) {
			nat_drop(worker, m, "ARP reply not from the --dnat host");
			return;
		}
		arp_answer_pending(worker, m, arp, zone);
		return;
	}
	forward_other(worker, m);
}

/* Echo: a software-only connection by identifier. */
static void nat_echo_v4(struct ct_fw_worker *worker,
			struct rte_mbuf *m,
			const struct rte_ipv4_hdr *ip,
			const struct rte_icmp_hdr *icmp,
			uint32_t icmp_len,
			uint32_t zone)
{
	const struct ct_fw_config *cfg = &worker->app->config;
	uint32_t port_code = ct_fw_mbuf_port_code(m);
	bool request = icmp->icmp_type == CT_FW_ICMP_ECHO_REQUEST;
	struct ct_fw_key key, rkey;
	struct ct_fw_verdict v;
	bool from_client = port_code == CT_FW_PORT_CODE(CT_FW_FIRST_UPLINK);
	struct ct_fw_conn *conn = NULL;

	memset(&key, 0, sizeof(key));
	key.family = CT_FW_FAMILY_IPV4;
	key.protocol = DOCA_FLOW_PROTO_ICMP;
	key.zone = zone;
	memcpy(key.src, &ip->src_addr, CT_FW_IPV4_ADDR_BYTES);
	memcpy(key.dst, &ip->dst_addr, CT_FW_IPV4_ADDR_BYTES);
	if (request)
		key.src_port = rte_be_to_cpu_16(icmp->icmp_ident);
	else
		key.dst_port = rte_be_to_cpu_16(icmp->icmp_ident);

	/* Pings open from the client side; replies match the return tuple. */
	if (request && from_client)
		conn = ct_fw_conn_lookup(&worker->table, &key);
	else if (!request && !from_client)
		conn = ct_fw_conn_lookup_reply(&worker->table, &key);
	if (conn != NULL && (conn->free_pending || conn->state != CT_FW_CONN_ECHO))
		conn = NULL;
	if (conn == NULL) {
		/* An untracked reply would show the real address. */
		if (!(request && from_client)) {
			if (request || from_client)
				forward_other(worker, m);
			else
				nat_drop(worker, m, "echo reply to no tracked ping");
			return;
		}
		/* A new ping takes state (and the --snat address) only with a valid checksum. */
		if (rte_raw_cksum(icmp, icmp_len) != 0xffff) {
			nat_drop(worker, m, "new ping with a bad checksum");
			return;
		}
		if (ct_fw_snat_enabled(cfg, port_code) &&
		    !ct_fw_snat_owner_allowed(worker,
					      zone,
					      &rte_pktmbuf_mtod(m, const struct rte_ether_hdr *)->src_addr,
					      ip->src_addr)) {
			nat_drop(worker, m, "ping from a second --snat client");
			return;
		}
		ct_fw_conn_nat_reply_key(cfg, &key, port_code, &rkey);
		conn = ct_fw_conn_lookup_reply(&worker->table, &rkey);
		if (conn != NULL && !conn->free_pending) {
			nat_drop(worker, m, "ping identifier in use toward the same host");
			return;
		}
		conn = worker->echo_conns < CT_FW_MAX_ECHO ? ct_fw_conn_open(worker, &key) : NULL;
		if (conn == NULL) {
			/* A full connection table warns where it is detected. */
			if (worker->echo_conns >= CT_FW_MAX_ECHO &&
			    ct_fw_log_due(worker->now_tsc, &worker->nat_other.fail_log_tsc))
				DOCA_LOG_WARN("Dropped a new ping under NAT: %u pings tracked (limit)", CT_FW_MAX_ECHO);
			emit_outcome(worker, m, CT_FW_DROP, &worker->stats.pool_exhausted);
			return;
		}
		conn->state = CT_FW_CONN_ECHO;
		conn->origin_port_code = port_code;
		worker->echo_conns++;
		ct_fw_conn_reply_index_add(&worker->table, conn);
		if (ct_fw_snat_enabled(cfg, port_code))
			ct_fw_snat_owner_claim(worker,
					       zone,
					       &rte_pktmbuf_mtod(m, const struct rte_ether_hdr *)->src_addr,
					       ip->src_addr);
	}
	conn->last_seen_tsc = worker->now_tsc;
	/* Translated by the send. */
	memset(&v, 0, sizeof(v));
	v.outcome = CT_FW_FORWARD;
	v.nat = ct_fw_conn_nat(cfg, conn)->enabled;
	if (v.nat)
		ct_fw_nat_translate(cfg, conn, !request, v.src, v.dst);
	CT_FW_STAT_INC(worker->stats.other_forwarded);
	ct_fw_emit(worker, m, &v);
}

/* Fix the quoted L4 checksum if the quote reaches it. */
static void quoted_l4_fix(uint8_t proto,
			  uint8_t *l4,
			  uint32_t l4_len,
			  const struct rte_ipv4_hdr *old_ip,
			  const struct rte_ipv4_hdr *new_ip)
{
	const uint8_t *old_s = (const uint8_t *)&old_ip->src_addr, *old_d = (const uint8_t *)&old_ip->dst_addr;
	const uint8_t *new_s = (const uint8_t *)&new_ip->src_addr, *new_d = (const uint8_t *)&new_ip->dst_addr;

	if (proto == DOCA_FLOW_PROTO_UDP) {
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)l4;

		udp->dgram_cksum = ct_fw_cksum_adjust_addr(udp->dgram_cksum,
							   old_s,
							   old_d,
							   new_s,
							   new_d,
							   CT_FW_IPV4_ADDR_BYTES,
							   true);
	} else if (proto == DOCA_FLOW_PROTO_TCP && l4_len >= TCP_CKSUM_END) {
		struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)l4;

		tcp->cksum =
			ct_fw_cksum_adjust_addr(tcp->cksum, old_s, old_d, new_s, new_d, CT_FW_IPV4_ADDR_BYTES, false);
	}
}

/* ICMPv4 error of a tracked connection. */
static void nat_icmp_error_v4(struct ct_fw_worker *worker,
			      struct rte_mbuf *m,
			      struct rte_ipv4_hdr *ip,
			      struct rte_icmp_hdr *icmp,
			      uint32_t icmp_len,
			      uint32_t zone)
{
	uint8_t *quote = (uint8_t *)(icmp + 1);
	uint32_t quote_len = icmp_len - sizeof(*icmp);
	struct rte_ipv4_hdr *qip = (struct rte_ipv4_hdr *)quote;
	uint8_t o_src[CT_FW_IPV6_ADDR_BYTES], o_dst[CT_FW_IPV6_ADDR_BYTES];
	struct rte_ipv4_hdr old_ip, old_q;
	struct ct_fw_key q, e;
	struct ct_fw_conn *conn;
	bool is_reply, wrong_uplink;
	uint32_t qihl;
	uint8_t *ql4;

	if (rte_raw_cksum(icmp, icmp_len) != 0xffff) {
		nat_drop(worker, m, "ICMP error with a bad checksum");
		return;
	}
	if (quote_len < sizeof(*qip) || (qip->version_ihl >> 4) != IPVERSION) {
		nat_drop(worker, m, "ICMP error without a quoted IPv4 header");
		return;
	}
	qihl = rte_ipv4_hdr_len(qip);
	if (qihl < sizeof(*qip) || quote_len < qihl + ICMP_ERROR_QUOTE_MIN ||
	    (rte_be_to_cpu_16(qip->fragment_offset) & RTE_IPV4_HDR_OFFSET_MASK) != 0 || ip->dst_addr != qip->src_addr) {
		nat_drop(worker, m, "ICMP error with a malformed quote");
		return;
	}
	ql4 = quote + qihl;

	memset(&q, 0, sizeof(q));
	q.family = CT_FW_FAMILY_IPV4;
	q.protocol = qip->next_proto_id;
	q.zone = zone;
	memcpy(q.src, &qip->src_addr, CT_FW_IPV4_ADDR_BYTES);
	memcpy(q.dst, &qip->dst_addr, CT_FW_IPV4_ADDR_BYTES);
	if (q.protocol == DOCA_FLOW_PROTO_TCP || q.protocol == DOCA_FLOW_PROTO_UDP) {
		const struct rte_udp_hdr *ports = (const struct rte_udp_hdr *)ql4;

		q.src_port = rte_be_to_cpu_16(ports->src_port);
		q.dst_port = rte_be_to_cpu_16(ports->dst_port);
	} else if (q.protocol == DOCA_FLOW_PROTO_ICMP && ql4[0] == CT_FW_ICMP_ECHO_REQUEST) {
		q.src_port = rte_be_to_cpu_16(((const struct rte_icmp_hdr *)ql4)->icmp_ident);
	} else if (q.protocol == DOCA_FLOW_PROTO_ICMP && ql4[0] == CT_FW_ICMP_ECHO_REPLY) {
		q.dst_port = rte_be_to_cpu_16(((const struct rte_icmp_hdr *)ql4)->icmp_ident);
	} else {
		nat_drop(worker, m, "ICMP error quoting an untracked protocol");
		return;
	}

	/* The error travels against the quoted packet. */
	e = q;
	memcpy(e.src, q.dst, sizeof(e.src));
	memcpy(e.dst, q.src, sizeof(e.dst));
	e.src_port = q.dst_port;
	e.dst_port = q.src_port;
	conn = ct_fw_conn_find_bidir(&worker->table, &e, ct_fw_mbuf_port_code(m), &is_reply, &wrong_uplink);
	if (conn == NULL || wrong_uplink) {
		nat_drop(worker, m, "ICMP error of no tracked connection");
		return;
	}

	ct_fw_nat_translate(&worker->app->config, conn, is_reply, o_src, o_dst);
	old_ip = *ip;
	old_q = *qip;
	/* A router keeps its own address. */
	if (ip->src_addr == qip->dst_addr)
		memcpy(&ip->src_addr, o_src, CT_FW_IPV4_ADDR_BYTES);
	memcpy(&ip->dst_addr, o_dst, CT_FW_IPV4_ADDR_BYTES);
	memcpy(&qip->src_addr, o_dst, CT_FW_IPV4_ADDR_BYTES);
	memcpy(&qip->dst_addr, o_src, CT_FW_IPV4_ADDR_BYTES);

	quoted_l4_fix(q.protocol, ql4, quote_len - qihl, &old_q, qip);
	qip->hdr_checksum = ct_fw_cksum_adjust_addr(qip->hdr_checksum,
						    (const uint8_t *)&old_q.src_addr,
						    (const uint8_t *)&old_q.dst_addr,
						    (const uint8_t *)&qip->src_addr,
						    (const uint8_t *)&qip->dst_addr,
						    CT_FW_IPV4_ADDR_BYTES,
						    false);
	ip->hdr_checksum = ct_fw_cksum_adjust_addr(ip->hdr_checksum,
						   (const uint8_t *)&old_ip.src_addr,
						   (const uint8_t *)&old_ip.dst_addr,
						   (const uint8_t *)&ip->src_addr,
						   (const uint8_t *)&ip->dst_addr,
						   CT_FW_IPV4_ADDR_BYTES,
						   false);
	icmp->icmp_cksum = 0;
	icmp->icmp_cksum = (uint16_t)~rte_raw_cksum(icmp, icmp_len);
	forward_other(worker, m);
}

static void nat_icmp_v4(struct ct_fw_worker *worker, struct rte_mbuf *m, const uint8_t *l3, uint32_t zone)
{
	const uint8_t *data_end = rte_pktmbuf_mtod(m, const uint8_t *) + rte_pktmbuf_data_len(m);
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(uintptr_t)l3;
	struct rte_icmp_hdr *icmp;
	uint32_t ihl, total;

	ihl = rte_ipv4_hdr_len(ip);
	total = rte_be_to_cpu_16(ip->total_length);
	if (ip->version != IPVERSION || ihl < sizeof(*ip) || total < ihl + sizeof(*icmp) || l3 + total > data_end) {
		nat_drop(worker, m, "malformed ICMP");
		return;
	}
	icmp = (struct rte_icmp_hdr *)((uint8_t *)ip + ihl);
	switch (icmp->icmp_type) {
	case CT_FW_ICMP_ECHO_REQUEST:
	case CT_FW_ICMP_ECHO_REPLY:
		nat_echo_v4(worker, m, ip, icmp, total - ihl, zone);
		break;
	case CT_FW_ICMP_DEST_UNREACHABLE:
	case CT_FW_ICMP_TTL_EXCEEDED:
	case CT_FW_ICMP_PARAM_PROBLEM:
		nat_icmp_error_v4(worker, m, ip, icmp, total - ihl, zone);
		break;
	default:
		nat_drop(worker, m, "ICMP type not translated");
		break;
	}
}

bool ct_fw_snat_owner_allowed(struct ct_fw_worker *worker,
			      uint32_t zone,
			      const struct rte_ether_addr *mac,
			      rte_be32_t src_ip)
{
	struct ct_fw_nat_other *st = &worker->nat_other;
	struct ct_fw_snat_owner *owner = snat_owner_find(st, zone);
	uint32_t i;

	if (owner != NULL) {
		if (rte_is_same_ether_addr(&owner->mac, mac))
			return true;
		snat_refuse_log(worker, zone, mac, src_ip, owner);
		return false;
	}
	for (i = 0; i < CT_FW_MAX_SNAT_OWNERS && st->owner[i].in_use; i++)
		;
	if (i == CT_FW_MAX_SNAT_OWNERS) {
		snat_refuse_log(worker, zone, mac, src_ip, NULL);
		return false;
	}
	return true;
}

struct ct_fw_snat_owner *ct_fw_snat_owner_claim(struct ct_fw_worker *worker,
						uint32_t zone,
						const struct rte_ether_addr *mac,
						rte_be32_t src_ip)
{
	struct ct_fw_nat_other *st = &worker->nat_other;
	struct ct_fw_snat_owner *owner = snat_owner_find(st, zone);
	char ip[INET_ADDRSTRLEN];
	uint32_t i;

	if (owner != NULL)
		return owner;
	for (i = 0; i < CT_FW_MAX_SNAT_OWNERS && st->owner[i].in_use; i++)
		;
	if (i == CT_FW_MAX_SNAT_OWNERS)
		return NULL;
	owner = &st->owner[i];
	owner->zone = zone;
	rte_ether_addr_copy(mac, &owner->mac);
	owner->ip = src_ip;
	owner->ip_log_tsc = 0;
	owner->in_use = true;
	DOCA_LOG_WARN("--snat: client " RTE_ETHER_ADDR_PRT_FMT " (%s) in zone %u uses the address until restart",
		      RTE_ETHER_ADDR_BYTES(mac),
		      ip4_str(src_ip, ip, sizeof(ip)),
		      zone);
	return owner;
}

bool ct_fw_nat_other(struct ct_fw_worker *worker, struct rte_mbuf *m)
{
	const uint8_t *data = rte_pktmbuf_mtod(m, const uint8_t *);
	const uint8_t *data_end = data + rte_pktmbuf_data_len(m);
	const uint8_t *l3;
	uint16_t ether_type;
	uint32_t zone;

	if (!worker->app->config.nat[CT_FW_FAMILY_IPV4].enabled || !ct_fw_eth_l3(data, data_end, &ether_type, &l3))
		return false;
	/* IPv6 (neighbor discovery, ICMPv6) is not translated. */
	if (ether_type == RTE_ETHER_TYPE_IPV6)
		return false;
	if (!ct_fw_frame_zone(data, data_end, &zone)) {
		emit_outcome(worker, m, CT_FW_DROP, &worker->stats.parse_drops);
		return true;
	}
	if (ether_type == RTE_ETHER_TYPE_ARP)
		nat_arp_v4(worker, m, l3, zone);
	else
		nat_icmp_v4(worker, m, l3, zone);
	return true;
}
