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

#include <time.h>
#include <unistd.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>

#include "flow_ct_common.h"
#include <flow_common.h>
#include "flow_switch_common.h"

#define PACKET_BURST 128
#define MIN(a, b) ((a) < (b) ? (a) : (b))
/* CT counters are queried once a second, allow a few cycles for a pending update to be reported */
#define STATS_UPDATE_TIMEOUT_S 5
#define STATS_UPDATE_POLL_US (100 * 1000)

DOCA_LOG_REGISTER(FLOW_CT_TCP_STATS_UPDATES);

/* Represents an IPv4 address a.b.c.d */
union ip4_addr {
	doca_be32_t ip;
	struct {
		uint8_t a;
		uint8_t b;
		uint8_t c;
		uint8_t d;
	};
};

/*
 * Per-entry private data, allocated by CT and handed back to the stats update callback.
 * The callback receives no entry handle, so the connection identity is stored here.
 */
struct ct_entry_priv_data {
	union ip4_addr origin_addr; /* IPv4 address of origin (src) */
	union ip4_addr reply_addr;  /* IPv4 address of reply (src) */
	doca_be16_t origin_port;    /* Origin TCP port (src) */
	doca_be16_t reply_port;	    /* Reply TCP port (src) */
};

static uint16_t sessions;
/* Written by the CT aging thread, read by the main thread */
static uint32_t nb_stats_updates;

/*
 * Create RSS pipe
 *
 * @port [in]: Pipe port
 * @status [in]: user context for adding entry
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_rss_pipe(struct doca_flow_port *port,
				    struct entries_status *status,
				    struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	uint16_t rss_queues[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "RSS_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* RSS queue - send matched traffic to queue 0  */
	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_create(cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RSS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_basic_add_entry(0, *pipe, &match, 0, NULL, NULL, &fwd, 0, status, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add RSS pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process RSS entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Create egress pipe
 *
 * @port [in]: Pipe port
 * @port_id [in]: Next pipe port id
 * @status [in]: user context for adding entry
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_egress_pipe(struct doca_flow_port *port,
				       int port_id,
				       struct entries_status *status,
				       struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "EGRESS_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	result = doca_flow_pipe_create(cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create EGRESS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_basic_add_entry(0, *pipe, &match, 0, NULL, NULL, &fwd, 0, status, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add EGRESS pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process EGRESS entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Create DOCA Flow TCP state pipe to filter state on known TCP session
 *
 * @port [in]: Pipe port
 * @status [in]: User context for adding entry
 * @fwd_pipe [in]: Forward pipe
 * @fwd_miss_pipe [in]: Forward miss pipe
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_tcp_flags_filter_pipe(struct doca_flow_port *port,
						 struct entries_status *status,
						 struct doca_flow_pipe *fwd_pipe,
						 struct doca_flow_pipe *fwd_miss_pipe,
						 struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* Match on non SYN, FIN and RST packets */
	match.outer.tcp.flags = 0xff;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;

	mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	mask.outer.tcp.flags = DOCA_FLOW_MATCH_TCP_FLAG_SYN | DOCA_FLOW_MATCH_TCP_FLAG_FIN |
			       DOCA_FLOW_MATCH_TCP_FLAG_RST;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "TCP_FLAGS_FILTER_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, &mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_pipe;

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = fwd_miss_pipe;

	result = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create TCP_FLAGS_FILTER pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	match.outer.tcp.flags = 0;
	result = doca_flow_pipe_basic_add_entry(0, *pipe, &match, 0, NULL, NULL, NULL, 0, status, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create TCP flags filter pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process TCP flags filter entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Create CT pipe
 *
 * @port [in]: Pipe port
 * @fwd_pipe [in]: Forward pipe pointer
 * @fwd_miss_pipe [in]: Forward miss pipe pointer
 * @nb_ipv4_sessions [in]: Number of IPv4 sessions
 * @nb_ipv6_sessions [in]: Number of IPv6 sessions
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ct_pipe(struct doca_flow_port *port,
				   struct doca_flow_pipe *fwd_pipe,
				   struct doca_flow_pipe *fwd_miss_pipe,
				   uint32_t nb_ipv4_sessions,
				   uint32_t nb_ipv6_sessions,
				   struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd));

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "CT_PIPE", DOCA_FLOW_PIPE_CT, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ct_connections(cfg, nb_ipv4_sessions, nb_ipv6_sessions, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CT connections: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ct_max_connections_per_zone(cfg, CT_DEFAULT_MAX_ZONE_SESSIONS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CT max connections per zone: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	/* CT allocates this amount of private data per entry, used here to hold the connection identity */
	result = doca_flow_pipe_cfg_set_ct_entry_private_data_size(cfg, sizeof(struct ct_entry_priv_data));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CT entry private data size: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_pipe;

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = fwd_miss_pipe;

	result = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create CT pipe: %s", doca_error_get_descr(result));
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Parse TCP packet to update CT tables
 *
 * @packet [in]: Packet to parse
 * @match_o [out]: Origin match struct to fill
 * @match_r [out]: Reply match struct to fill
 * @tcp_state [out]: Packet TCP state
 */
static void parse_packet(struct rte_mbuf *packet,
			 struct doca_flow_ct_match *match_o,
			 struct doca_flow_ct_match *match_r,
			 uint8_t *tcp_state)
{
	uint8_t *l4_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	const struct rte_tcp_hdr *tcp_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));

	match_o->ipv4.src_ip = ipv4_hdr->src_addr;
	match_o->ipv4.dst_ip = ipv4_hdr->dst_addr;
	match_r->ipv4.src_ip = match_o->ipv4.dst_ip;
	match_r->ipv4.dst_ip = match_o->ipv4.src_ip;

	l4_hdr = (typeof(l4_hdr))ipv4_hdr + rte_ipv4_hdr_len(ipv4_hdr);
	tcp_hdr = (typeof(tcp_hdr))l4_hdr;

	match_o->ipv4.l4_port.src_port = tcp_hdr->src_port;
	match_o->ipv4.l4_port.dst_port = tcp_hdr->dst_port;
	match_r->ipv4.l4_port.src_port = match_o->ipv4.l4_port.dst_port;
	match_r->ipv4.l4_port.dst_port = match_o->ipv4.l4_port.src_port;

	match_o->ipv4.next_proto = DOCA_FLOW_PROTO_TCP;
	match_r->ipv4.next_proto = DOCA_FLOW_PROTO_TCP;

	*tcp_state = tcp_hdr->tcp_flags;
}

/*
 * Add a CT entry with stats updates enabled, storing the connection identity in its private data
 *
 * The private data must be filled between the prepare and the add: once the entry is offloaded
 * the aging thread may invoke the stats update callback on it at any time.
 *
 * @port [in]: Port id to which an entry should be inserted
 * @ct_queue [in]: DOCA Flow CT queue number
 * @ct_pipe [in]: DOCA Flow CT pipe
 * @match_o [in]: Origin match
 * @match_r [in]: Reply match
 * @ct_status [in]: User context for adding CT entry
 * @entry [out]: CT entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_ct_entry(struct doca_flow_port *port,
				 uint16_t ct_queue,
				 struct doca_flow_pipe *ct_pipe,
				 struct doca_flow_ct_match *match_o,
				 struct doca_flow_ct_match *match_r,
				 struct entries_status *ct_status,
				 struct doca_flow_pipe_entry **entry)
{
	const uint32_t entry_flags = DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
				     DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY | DOCA_FLOW_CT_ENTRY_FLAGS_STATS_UPDATES |
				     DOCA_FLOW_CT_ENTRY_FLAGS_COUNTER_ORIGIN | DOCA_FLOW_CT_ENTRY_FLAGS_COUNTER_REPLY;
	struct ct_entry_priv_data *entry_info;
	bool conn_found = false;
	doca_error_t result;

	result = doca_flow_ct_entry_prepare(ct_queue,
					    ct_pipe,
					    DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS,
					    match_o,
					    0,
					    match_r,
					    0,
					    entry,
					    &conn_found);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare CT entry: %s", doca_error_get_descr(result));
		return result;
	}

	/* The connection is already offloaded, its private data was filled when it was created */
	if (conn_found)
		return DOCA_SUCCESS;

	entry_info = (struct ct_entry_priv_data *)doca_flow_ct_entry_get_priv_data(ct_queue, ct_pipe, *entry);
	if (entry_info == NULL) {
		DOCA_LOG_ERR("Failed to get CT entry private data");
		(void)doca_flow_ct_entry_prepare_rollback(ct_queue, ct_pipe, *entry);
		*entry = NULL;
		return DOCA_ERROR_BAD_STATE;
	}
	entry_info->origin_addr.ip = match_o->ipv4.src_ip;
	entry_info->reply_addr.ip = match_o->ipv4.dst_ip;
	entry_info->origin_port = match_o->ipv4.l4_port.src_port;
	entry_info->reply_port = match_o->ipv4.l4_port.dst_port;

	result = doca_flow_ct_add_entry(ct_queue,
					ct_pipe,
					entry_flags,
					match_o,
					match_r,
					NULL,
					NULL,
					NULL,
					NULL,
					0,
					ct_status,
					*entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add CT pipe entry: %s", doca_error_get_descr(result));
		(void)doca_flow_ct_entry_prepare_rollback(ct_queue, ct_pipe, *entry);
		*entry = NULL;
		return result;
	}

	return flow_ct_queue_reserve(port, ct_queue, ct_status, 0);
}

/*
 * Wait for the CT aging thread to report the session counters once more
 *
 * The counters are queried once a second, so the last traffic on the session - including the
 * 'FIN'/'RST' packet itself, which is counted on its way to the CPU - is only reported on a later
 * query cycle. Waiting for a report newer than those already seen keeps those final counters from
 * being lost when the entry is removed. That pending delta is what normally ends the wait; the
 * timeout only expires if it was already reported in the moment before this call.
 *
 * @timeout_s [in]: Maximum time to wait, in seconds
 */
static void wait_for_stats_update(uint32_t timeout_s)
{
	uint32_t seen = __atomic_load_n(&nb_stats_updates, __ATOMIC_RELAXED);
	time_t end_time = time(NULL) + timeout_s;

	while (__atomic_load_n(&nb_stats_updates, __ATOMIC_RELAXED) == seen && time(NULL) < end_time)
		usleep(STATS_UPDATE_POLL_US);

	if (__atomic_load_n(&nb_stats_updates, __ATOMIC_RELAXED) == seen)
		DOCA_LOG_WARN("No further stats update within %u seconds, final counters may already be reported",
			      timeout_s);
}

/*
 * Dequeue packets from DPDK queues, parse and update CT tables with new connection 5 tuple
 *
 * @port [in]: Port id to which an entry should be inserted
 * @ct_queue [in]: DOCA Flow CT queue number
 * @ct_pipe [in]: DOCA Flow CT pipe
 * @ct_status [in]: User context for adding CT entry
 * @entry [in/out]: CT entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t process_packets(struct doca_flow_port *port,
				    uint16_t ct_queue,
				    struct doca_flow_pipe *ct_pipe,
				    struct entries_status *ct_status,
				    struct doca_flow_pipe_entry **entry)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_ct_match match_o;
	struct doca_flow_ct_match match_r;
	uint8_t tcp_state;
	doca_error_t result;
	int i, nb_packets, total_valid_packets = 0;
	uint32_t timeout_s = 5; /* Timeout in seconds */
	time_t now, end_time, max_end_time;

	memset(&match_o, 0, sizeof(match_o));
	memset(&match_r, 0, sizeof(match_r));

	max_end_time = time(NULL) + timeout_s; /* Absolute maximum timeout */
	end_time = max_end_time;	       /* Current timeout */
	do {
		nb_packets = rte_eth_rx_burst(0, 0, packets, PACKET_BURST);
		if (nb_packets == 0) {
			/* No packets received, continue immediately without blocking */
			continue;
		}

		DOCA_LOG_INFO("%d packets received on rx_burst()", nb_packets);
		for (i = 0; i < PACKET_BURST && i < nb_packets; i++) {
			parse_packet(packets[i], &match_o, &match_r, &tcp_state);
			if (tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_SYN) {
				if (sessions > 0) {
					DOCA_LOG_INFO("Already have one entry, skip");
					rte_pktmbuf_free(packets[i]);
					continue;
				}
				total_valid_packets++;
				/* Single time() read: MIN() evaluates its first argument twice */
				now = time(NULL);
				end_time = MIN(now + 2, max_end_time);

				result = add_ct_entry(port, ct_queue, ct_pipe, &match_o, &match_r, ct_status, entry);
				if (result != DOCA_SUCCESS)
					return result;
				sessions++;
				DOCA_LOG_INFO("TCP session was created");
			} else if (tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_FIN ||
				   tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_RST) {
				if (sessions == 0) {
					DOCA_LOG_INFO("No entry to remove, skip");
					rte_pktmbuf_free(packets[i]);
					continue;
				}
				total_valid_packets++;
				/* Single time() read: MIN() evaluates its first argument twice */
				now = time(NULL);
				end_time = MIN(now + 2, max_end_time);
				wait_for_stats_update(STATS_UPDATE_TIMEOUT_S);
				result = doca_flow_ct_rm_entry(ct_queue,
							       ct_pipe,
							       DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT,
							       *entry);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to remove CT pipe entry: %s",
						     doca_error_get_descr(result));
					return result;
				}
				result = flow_ct_queue_reserve(port, ct_queue, ct_status, 0);
				if (result != DOCA_SUCCESS)
					return result;
				sessions--;
				DOCA_LOG_INFO("TCP session was ended");
			} else {
				DOCA_LOG_WARN("Sample is only able to process 'SYN', 'FIN' and 'RST' packets");
				rte_pktmbuf_free(packets[i]);
				continue;
			}
			if (rte_eth_tx_burst(0, 0, &packets[i], 1) != 1)
				rte_pktmbuf_free(packets[i]);
		}
	} while (time(NULL) < end_time);

	if (total_valid_packets == 0) {
		if (sessions == 0) {
			DOCA_LOG_ERR("Sample didn't receive SYN packets within %u seconds timeout", timeout_s);
		} else {
			DOCA_LOG_ERR("Sample didn't receive 'FIN'/'RST' packets within %u seconds timeout", timeout_s);
		}
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

/*
 * CT stats update callback, invoked on the CT aging thread whenever a session counter changes
 *
 * @pipe [in]: Not being used
 * @priv_data [in]: Private data allocated by CT for the entry
 * @stats_origin [in]: Origin direction counter
 * @stats_reply [in]: Reply direction counter
 */
static void stats_update_cb(struct doca_flow_pipe *pipe,
			    void *priv_data,
			    struct doca_flow_resource_query *stats_origin,
			    struct doca_flow_resource_query *stats_reply)
{
	const struct ct_entry_priv_data *entry_info = (const struct ct_entry_priv_data *)priv_data;

	(void)pipe;

	DOCA_LOG_INFO(
		"Flow %u.%u.%u.%u:%u <-> %u.%u.%u.%u:%u stats update - origin: %lu/%lu reply: %lu/%lu (pkts/bytes)",
		entry_info->origin_addr.a,
		entry_info->origin_addr.b,
		entry_info->origin_addr.c,
		entry_info->origin_addr.d,
		be16toh(entry_info->origin_port),
		entry_info->reply_addr.a,
		entry_info->reply_addr.b,
		entry_info->reply_addr.c,
		entry_info->reply_addr.d,
		be16toh(entry_info->reply_port),
		stats_origin->counter.total_pkts,
		stats_origin->counter.total_bytes,
		stats_reply->counter.total_pkts,
		stats_reply->counter.total_bytes);

	__atomic_fetch_add(&nb_stats_updates, 1, __ATOMIC_RELAXED);
}

/*
 * Run flow_ct_tcp_stats_updates sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ct_tcp_stats_updates(uint16_t nb_queues, struct flow_switch_ctx *ctx)
{
	const int nb_ports = ctx->devs_ctx.devs_manager[0].nb_reps > 0 ? 2 : 1;
	const int nb_entries = 6;
	struct flow_resources resource;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_pipe_entry *tcp_entry;
	struct doca_flow_pipe *egress_pipe, *tcp_flags_filter_pipe, *rss_pipe, *tcp_pipe;
	struct doca_flow_pipe *ct_pipe = NULL;
	struct doca_flow_port *ports[nb_ports];
	struct doca_flow_meta o_zone_mask, r_zone_mask;
	struct doca_flow_ct_meta o_modify_mask, r_modify_mask;
	uint32_t actions_mem_size[nb_ports];
	struct entries_status ctrl_status, ct_status;
	uint32_t nb_ct_queues = 1, nb_ctrl_queues = 1, nb_ipv4_sessions = 1024, nb_ipv6_sessions = 0; /* On BF2 should
													  always be 0 */
	struct ct_port_cfg ct_port_cfg = {.ct_aging_core = nb_ct_queues + 1,
					  .zone_match_mask = {&o_zone_mask, &r_zone_mask},
					  .modify_mask = {&o_modify_mask, &r_modify_mask},
					  .stats_update_cb = stats_update_cb};
	uint16_t ct_queue = nb_queues;
	doca_error_t result;

	memset(&ctrl_status, 0, sizeof(ctrl_status));
	memset(&ct_status, 0, sizeof(ct_status));
	memset(&resource, 0, sizeof(resource));

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = 1;
	resource.nr_ct_counters = (nb_ipv4_sessions + nb_ipv6_sessions) * 2; /* 2 counters per session */
	resource.nr_rss = 1;

	struct ct_init_cfg ct_cfg = {.ct_queues = nb_ct_queues, .ct_ctrl_queues = nb_ctrl_queues};
	result = init_doca_flow_with_ct(nb_queues, "switch,expert", &resource, nr_shared_resources, &ct_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	/* Don't use zone masking */
	memset(&o_zone_mask, 0, sizeof(o_zone_mask));
	memset(&o_modify_mask, 0, sizeof(o_modify_mask));
	memset(&r_zone_mask, 0, sizeof(r_zone_mask));
	memset(&r_modify_mask, 0, sizeof(r_modify_mask));

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(nb_entries));
	result = init_doca_flow_switch_ports_with_ct(ctx->devs_ctx.devs_manager,
						     ctx->devs_ctx.nb_devs,
						     ports,
						     nb_ports,
						     actions_mem_size,
						     &resource,
						     &ct_port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		destroy_doca_flow();
		return result;
	}

	result = create_rss_pipe(ports[0], &ctrl_status, &rss_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_egress_pipe(ports[0], nb_ports > 1 ? 1 : 0, &ctrl_status, &egress_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_tcp_flags_filter_pipe(ports[0], &ctrl_status, egress_pipe, rss_pipe, &tcp_flags_filter_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result =
		create_ct_pipe(ports[0], tcp_flags_filter_pipe, rss_pipe, nb_ipv4_sessions, nb_ipv6_sessions, &ct_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_root_pipe(ports[0], true, false, DOCA_FLOW_L4_META_TCP, ct_pipe, &ctrl_status, &tcp_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = doca_flow_entries_process(ports[0], 0, DEFAULT_TIMEOUT_US, nb_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process Flow CT entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	if (ctrl_status.nb_processed != nb_entries || ctrl_status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		result = DOCA_ERROR_BAD_STATE;
		goto cleanup;
	}

	DOCA_LOG_INFO("Wait a few seconds for 'SYN' packets to arrive");
	result = process_packets(ports[0], ct_queue, ct_pipe, &ct_status, &tcp_entry);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	DOCA_LOG_INFO("Waiting for 'FIN'/'RST' packets to arrive before ending the session");
	result = process_packets(ports[0], ct_queue, ct_pipe, &ct_status, &tcp_entry);
	if (result != DOCA_SUCCESS)
		goto cleanup;

cleanup:
	cleanup_procedure(ct_pipe, nb_ports, ports);
	return result;
}
