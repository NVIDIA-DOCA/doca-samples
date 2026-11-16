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

/* CT Firewall IPv4 fragment reassembly (keyed by zone and uplink). */

#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include <doca_log.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::FRAG);

_Static_assert(CT_FW_FRAG_MAX_LEN + RTE_ETHER_HDR_LEN + 2 * sizeof(struct rte_vlan_hdr) <=
		       CT_FW_MBUF_SIZE - RTE_PKTMBUF_HEADROOM,
	       "a reassembled datagram fits one mbuf");

#define FRAG_UNIT 8u	   /* offset unit */
#define TCP_DATA_OFF_MIN 5 /* words, no options */

static struct ct_fw_frag_dgram *dgram_find(struct ct_fw_frag_table *table, const struct ct_fw_frag_key *key)
{
	uint32_t i;

	for (i = 0; i < CT_FW_FRAG_MAX_DGRAMS; i++)
		if (table->d[i].in_use && memcmp(&table->d[i].key, key, sizeof(*key)) == 0)
			return &table->d[i];
	return NULL;
}

static struct ct_fw_frag_dgram *dgram_new(struct ct_fw_frag_table *table,
					  const struct ct_fw_frag_key *key,
					  uint64_t now)
{
	uint32_t i;

	for (i = 0; i < CT_FW_FRAG_MAX_DGRAMS; i++) {
		struct ct_fw_frag_dgram *dg = &table->d[i];

		if (dg->in_use)
			continue;
		memset(dg, 0, sizeof(*dg));
		dg->key = *key;
		dg->first_tsc = now;
		dg->in_use = true;
		table->nb_in_use++;
		return dg;
	}
	return NULL;
}

static bool dgram_complete(const struct ct_fw_frag_dgram *dg)
{
	uint8_t i;

	if (dg->l4_total == CT_FW_FRAG_L4_TOTAL_UNKNOWN || dg->nb == 0 || dg->frag[0].off != 0 ||
	    dg->frag[dg->nb - 1].end != dg->l4_total)
		return false;
	for (i = 1; i < dg->nb; i++)
		if (dg->frag[i].off != dg->frag[i - 1].end)
			return false;
	return true;
}

/* True when the fragment breaks the datagram's final extent. */
static bool beyond_end(const struct ct_fw_frag_dgram *dg, const struct ct_fw_frag_info *info)
{
	uint8_t i;

	if (info->last) {
		if (dg->l4_total != CT_FW_FRAG_L4_TOTAL_UNKNOWN)
			return dg->l4_total != info->end;
		/* No final fragment held yet. */
		for (i = 0; i < dg->nb; i++)
			if (dg->frag[i].end >= info->end)
				return true;
		return false;
	}
	return dg->l4_total != CT_FW_FRAG_L4_TOTAL_UNKNOWN && info->end >= dg->l4_total;
}

bool ct_fw_frag_parse(const struct rte_mbuf *m, uint32_t port_code, struct ct_fw_frag_info *info)
{
	const uint8_t *data = rte_pktmbuf_mtod(m, const uint8_t *);
	const uint8_t *data_end = data + rte_pktmbuf_data_len(m);
	const struct rte_ipv4_hdr *ip;
	const uint8_t *l3;
	uint16_t ether_type;
	uint16_t ihl, total, len, frag;
	uint32_t zone;

	memset(info, 0, sizeof(*info));
	if (m->nb_segs != 1 || !ct_fw_frame_zone(data, data_end, &zone) ||
	    !ct_fw_eth_l3(data, data_end, &ether_type, &l3) || ether_type != RTE_ETHER_TYPE_IPV4)
		return false;
	ip = (const struct rte_ipv4_hdr *)l3;
	if (l3 + sizeof(*ip) > data_end || ip->version != IPVERSION)
		return false;
	ihl = rte_ipv4_hdr_len(ip);
	/* The reassembled header is rebuilt from this one. */
	if (ihl < sizeof(*ip) || l3 + ihl > data_end || rte_ipv4_cksum(ip) != 0)
		return false;
	total = rte_be_to_cpu_16(ip->total_length);
	if (total <= ihl || l3 + total > data_end)
		return false;
	if (ip->next_proto_id != DOCA_FLOW_PROTO_TCP && ip->next_proto_id != DOCA_FLOW_PROTO_UDP)
		return false;
	len = (uint16_t)(total - ihl);
	frag = rte_be_to_cpu_16(ip->fragment_offset);
	info->last = !(frag & RTE_IPV4_HDR_MF_FLAG);
	info->off = (uint16_t)((frag & RTE_IPV4_HDR_OFFSET_MASK) * FRAG_UNIT);
	if (!info->last && len % FRAG_UNIT != 0)
		return false;
	if (info->off + len + ihl > CT_FW_FRAG_MAX_LEN) {
		info->over_len = (uint32_t)info->off + len + ihl;
		return false;
	}
	info->end = (uint16_t)(info->off + len);
	if (info->off == 0) {
		const uint8_t *l4 = l3 + ihl;

		if (ip->next_proto_id == DOCA_FLOW_PROTO_UDP) {
			info->short_first = len < sizeof(struct rte_udp_hdr);
		} else {
			const struct rte_tcp_hdr *tcp = (const struct rte_tcp_hdr *)l4;
			uint8_t data_off;

			info->short_first = len < sizeof(*tcp);
			if (!info->short_first) {
				data_off = (uint8_t)(tcp->data_off >> 4);
				info->short_first = data_off < TCP_DATA_OFF_MIN || len < (uint16_t)(data_off * 4);
			}
		}
	}
	info->key.zone = zone;
	info->key.port_code = port_code;
	info->key.src = ip->src_addr;
	info->key.dst = ip->dst_addr;
	info->key.ip_id = ip->packet_id;
	info->key.proto = ip->next_proto_id;
	return true;
}

void ct_fw_frag_done(struct ct_fw_frag_table *table, struct ct_fw_frag_dgram *dg)
{
	if (!dg->in_use)
		return;
	memset(dg, 0, sizeof(*dg));
	table->nb_in_use--;
}

void ct_fw_frag_drop(struct ct_fw_frag_table *table, struct ct_fw_frag_dgram *dg, uint64_t *drops)
{
	uint8_t i;

	for (i = 0; i < dg->nb; i++) {
		if (dg->frag[i].m == NULL)
			continue;
		rte_pktmbuf_free(dg->frag[i].m);
		if (drops != NULL)
			CT_FW_STAT_INC(*drops);
	}
	ct_fw_frag_done(table, dg);
}

enum ct_fw_frag_result ct_fw_frag_insert(struct ct_fw_frag_table *table,
					 const struct ct_fw_frag_info *info,
					 struct rte_mbuf *m,
					 uint64_t now_tsc,
					 uint64_t *drops,
					 struct ct_fw_frag_dgram **dg_out)
{
	struct ct_fw_frag_dgram *dg = dgram_find(table, &info->key);
	uint8_t pos;

	*dg_out = NULL;
	if (dg == NULL)
		dg = dgram_new(table, &info->key, now_tsc);
	/* No eviction: a full table refuses new datagrams. */
	if (dg == NULL) {
		if (ct_fw_log_due(now_tsc, &table->full_log_tsc))
			DOCA_LOG_WARN("Fragment table full (%u of %u datagrams); new fragmented datagrams are dropped",
				      table->nb_in_use,
				      CT_FW_FRAG_MAX_DGRAMS);
		goto drop_one;
	}
	for (pos = 0; pos < dg->nb && dg->frag[pos].off < info->off; pos++)
		;
	/* An exact copy is dropped alone. */
	if (pos < dg->nb && dg->frag[pos].off == info->off && dg->frag[pos].end == info->end &&
	    info->last == (dg->l4_total == info->end))
		goto drop_one;
	if (dg->nb == CT_FW_FRAG_MAX_PER_DGRAM) {
		if (ct_fw_log_due(now_tsc, &table->cap_log_tsc))
			DOCA_LOG_WARN(
				"Fragment limit per datagram reached (%u of %u buffered); incoming fragment and datagram dropped",
				dg->nb,
				CT_FW_FRAG_MAX_PER_DGRAM);
		goto drop_dgram;
	}
	if (info->short_first || beyond_end(dg, info) || (pos > 0 && dg->frag[pos - 1].end > info->off) ||
	    (pos < dg->nb && dg->frag[pos].off < info->end))
		goto drop_dgram;
	memmove(&dg->frag[pos + 1], &dg->frag[pos], (size_t)(dg->nb - pos) * sizeof(dg->frag[0]));
	dg->frag[pos].m = m;
	dg->frag[pos].off = info->off;
	dg->frag[pos].end = info->end;
	dg->nb++;
	if (info->last)
		dg->l4_total = info->end;
	if (!dgram_complete(dg))
		return CT_FW_FRAG_HELD;
	*dg_out = dg;
	return CT_FW_FRAG_COMPLETE;

drop_dgram:
	ct_fw_frag_drop(table, dg, drops);
drop_one:
	rte_pktmbuf_free(m);
	CT_FW_STAT_INC(*drops);
	return CT_FW_FRAG_DROPPED;
}

void ct_fw_frag_flush(struct ct_fw_frag_table *table)
{
	uint32_t i;

	for (i = 0; i < CT_FW_FRAG_MAX_DGRAMS; i++)
		if (table->d[i].in_use)
			ct_fw_frag_drop(table, &table->d[i], NULL);
}

uint32_t ct_fw_frag_expire(struct ct_fw_frag_table *table, uint64_t now_tsc, uint64_t timeout_cycles, uint64_t *drops)
{
	uint32_t i, n = 0;

	for (i = 0; i < CT_FW_FRAG_MAX_DGRAMS && table->nb_in_use != 0; i++) {
		struct ct_fw_frag_dgram *dg = &table->d[i];

		if (dg->in_use && now_tsc - dg->first_tsc > timeout_cycles) {
			ct_fw_frag_drop(table, dg, drops);
			n++;
		}
	}
	return n;
}

struct rte_mbuf *ct_fw_frag_wrap(const struct ct_fw_frag_dgram *dg, struct rte_mempool *pool, uint32_t *over_len)
{
	const struct rte_mbuf *first = dg->frag[0].m;
	const uint8_t *data = rte_pktmbuf_mtod(first, const uint8_t *);
	const uint8_t *l3;
	struct rte_ipv4_hdr *ip;
	struct rte_mbuf *w;
	uint16_t ether_type, l2, ihl;
	uint8_t *out;
	uint8_t i;

	*over_len = 0;
	if (!ct_fw_eth_l3(data, data + rte_pktmbuf_data_len(first), &ether_type, &l3))
		return NULL;
	l2 = (uint16_t)(l3 - data);
	ihl = rte_ipv4_hdr_len((const struct rte_ipv4_hdr *)l3);
	/* The first fragment's options can take the whole past the cap. */
	if (ihl + dg->l4_total > CT_FW_FRAG_MAX_LEN) {
		*over_len = (uint32_t)ihl + dg->l4_total;
		return NULL;
	}
	w = rte_pktmbuf_alloc(pool);
	if (w == NULL)
		return NULL;
	out = (uint8_t *)rte_pktmbuf_append(w, (uint16_t)(l2 + ihl + dg->l4_total));
	if (out == NULL) {
		rte_pktmbuf_free(w);
		return NULL;
	}
	memcpy(out, data, l2 + ihl);
	for (i = 0; i < dg->nb; i++) {
		const uint8_t *fdata = rte_pktmbuf_mtod(dg->frag[i].m, const uint8_t *);
		const uint8_t *fl3;
		uint16_t fihl;

		/* Each fragment's own L3 offset: its tags may differ. */
		if (!ct_fw_eth_l3(fdata, fdata + rte_pktmbuf_data_len(dg->frag[i].m), &ether_type, &fl3)) {
			rte_pktmbuf_free(w);
			return NULL;
		}
		fihl = rte_ipv4_hdr_len((const struct rte_ipv4_hdr *)fl3);
		memcpy(out + l2 + ihl + dg->frag[i].off, fl3 + fihl, dg->frag[i].end - dg->frag[i].off);
	}
	ip = (struct rte_ipv4_hdr *)(out + l2);
	ip->total_length = rte_cpu_to_be_16((uint16_t)(ihl + dg->l4_total));
	ip->fragment_offset &= rte_cpu_to_be_16((uint16_t) ~(RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK));
	ip->hdr_checksum = 0;
	ip->hdr_checksum = rte_ipv4_cksum(ip);
	if (rte_flow_dynf_metadata_avail()) {
		w->ol_flags |= rte_flow_dynf_metadata_mask;
		*RTE_FLOW_DYNF_METADATA(w) = *RTE_FLOW_DYNF_METADATA(first);
	}
	return w;
}
