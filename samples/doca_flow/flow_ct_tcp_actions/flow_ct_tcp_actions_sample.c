/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>
#include <doca_bitfield.h>

#include "flow_ct_common.h"
#include <flow_common.h>
#include "flow_switch_common.h"

#define PACKET_BURST 128
#define NB_COUNTERS 6

static uint16_t sessions = 0;
static struct doca_flow_pipe_entry *rss_entry;
static struct doca_flow_pipe_entry *egress_entry;
static struct doca_flow_pipe_entry *tcp_flags_filter_entry;
static struct doca_flow_pipe_entry *ct_miss_entry;
static struct doca_flow_pipe_entry *ct_entry;

DOCA_LOG_REGISTER(FLOW_CT_TCP);

/**
 * PUT NIC in MP-ESWITCH mode
 *
 * Root pipe -> CT pipe	-> CT miss pipe -> rss pipe -> SYN: create CT rule
 * 		        -> tcp flags filter pipe -> egress pipe -> port 1
 * 						 -> rss pipe -> FIN/RST: delete CT rule
 * CT rule actions:
 * 	origin: set pkt_meta to 1
 * 	reply: NAT(reverse L3 addresses and L4 ports)
 *
 * Input: port 0 and port 1, IPv4(dst=1.1.1.1) TCP packets with SYN flag set
 * 	# send SYN packet will create a new CT rule
 * 	sendp(Ether(dst="2")/IP(src="1.1.1.1",dst="2.2.2.2")/TCP(dport=1,sport=2,flags="S"),iface="eth2")
 * 	# wait 5 seconds
 * 	# send TCP known packet with reverse L3 addresses and L4 ports which match reply direction of CT rule
 * 	sendp(Ether(dst="2")/IP(dst="1.1.1.1",src="2.2.2.2")/TCP(sport=1,dport=2,flags=""),iface="eth2")
 * Output: Known TCP session on port 1
 * 	# modified by CT reply direction rule action(reverse L3 addresses and L4 ports)
 * 	IPv4 TCP 1.1.1.1:2 > 2.2.2.2:1
 */

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
	struct doca_flow_monitor monitor;
	uint16_t rss_queues[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

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
	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
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
	result = doca_flow_pipe_add_entry(0, *pipe, &match, NULL, NULL, &fwd, 0, status, &rss_entry);
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
	struct doca_flow_monitor monitor;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "EGRESS_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
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
	result = doca_flow_pipe_add_entry(0, *pipe, &match, NULL, NULL, &fwd, 0, status, &egress_entry);
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
 * Create CT miss pipe
 *
 * @port [in]: Pipe port
 * @fwd_pipe [in]: Forward pipe pointer
 * @status [in]: user context for adding entry
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ct_miss_pipe(struct doca_flow_port *port,
					struct doca_flow_pipe *fwd_pipe,
					struct entries_status *status,
					struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "CT_MISS_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_pipe;

	result = doca_flow_pipe_create(cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create CT miss pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_add_entry(0, *pipe, &match, NULL, NULL, &fwd, 0, status, &ct_miss_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add CT miss pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process CT miss entry: %s", doca_error_get_descr(result));

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
	struct doca_flow_monitor monitor;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	/* Match on non SYN, FIN and RST packets */
	match.outer.tcp.flags = 0xff;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TRANSPORT;

	mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TRANSPORT;
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
	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
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
	result = doca_flow_pipe_add_entry(0, *pipe, &match, NULL, NULL, NULL, 0, status, &tcp_flags_filter_entry);
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
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ct_pipe(struct doca_flow_port *port,
				   struct doca_flow_pipe *fwd_pipe,
				   struct doca_flow_pipe *fwd_miss_pipe,
				   struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_actions meta_action;
	struct doca_flow_actions nat_action;
	struct doca_flow_actions meta_action_mask;
	struct doca_flow_actions nat_action_mask;
	struct doca_flow_actions *actions_list[] = {&meta_action, &nat_action};
	struct doca_flow_actions *actions_mask_list[] = {&meta_action_mask, &nat_action_mask};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd));
	memset(&meta_action, 0, sizeof(meta_action));
	memset(&nat_action, 0, sizeof(nat_action));
	memset(&meta_action_mask, 0, sizeof(meta_action_mask));
	memset(&nat_action_mask, 0, sizeof(nat_action_mask));

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_pipe;

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = fwd_miss_pipe;

	meta_action.action_idx = 0;
	meta_action.meta.pkt_meta = UINT32_MAX;
	meta_action.meta.u32[0] = UINT32_MAX;

	nat_action.action_idx = 1;
	nat_action.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	nat_action.outer.ip4.src_ip = UINT32_MAX;
	nat_action.outer.ip4.dst_ip = UINT32_MAX;
	nat_action.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	nat_action.outer.transport.src_port = UINT16_MAX;
	nat_action.outer.transport.dst_port = UINT16_MAX;

	meta_action_mask.action_idx = 0;
	meta_action_mask.meta.pkt_meta = DOCA_HTOBE32(0xffff);
	meta_action_mask.meta.u32[0] = DOCA_HTOBE32(0xffff);

	nat_action_mask.action_idx = 1;
	nat_action_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	nat_action_mask.outer.ip4.src_ip = UINT32_MAX;
	nat_action_mask.outer.ip4.dst_ip = UINT32_MAX;
	nat_action_mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	nat_action_mask.outer.transport.src_port = UINT16_MAX;
	nat_action_mask.outer.transport.dst_port = UINT16_MAX;

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
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(cfg, actions_list, actions_mask_list, NULL, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

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
 * Dequeue packets from DPDK queues, parse and update CT tables with new connection 5 tuple
 *
 * @port [in]: Port id to which an entry should be inserted
 * @ct_queue [in]: DOCA Flow CT queue number
 * @ct_status [in]: User context for adding CT entry
 * @shared_handle [in]: Shared action handle
 * @entry [in/out]: CT entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t process_packets(struct doca_flow_port *port,
				    uint16_t ct_queue,
				    struct entries_status *ct_status,
				    uint32_t shared_handle,
				    struct doca_flow_pipe_entry **entry)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_ct_match match_o;
	struct doca_flow_ct_match match_r;
	struct doca_flow_ct_actions shared_action_o, action_r;
	uint32_t flags = DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
			 DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY | DOCA_FLOW_CT_ENTRY_FLAGS_COUNTER_ORIGIN |
			 DOCA_FLOW_CT_ENTRY_FLAGS_COUNTER_REPLY;
	uint8_t tcp_state;
	doca_error_t result;
	int rc, i, nb_packets, nb_process = 0;

	memset(&match_o, 0, sizeof(match_o));
	memset(&match_r, 0, sizeof(match_r));
	memset(&action_r, 0, sizeof(action_r));
	memset(&shared_action_o, 0, sizeof(shared_action_o));

	shared_action_o.resource_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	shared_action_o.action_handle = shared_handle;

	action_r.resource_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	action_r.data.action_idx = 1;

	rc = rte_flow_dynf_metadata_register();
	if (unlikely(rc)) {
		DOCA_LOG_ERR("Enable metadata failed");
		return DOCA_ERROR_BAD_STATE;
	}

	nb_packets = rte_eth_rx_burst(0, 0, packets, PACKET_BURST);
	if (nb_packets == 0) {
		DOCA_LOG_INFO("Sample didn't receive packets to process");
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("%d packets received on rx_burst()", nb_packets);
	for (i = 0; i < PACKET_BURST && i < nb_packets; i++) {
		parse_packet(packets[i], &match_o, &match_r, &tcp_state);
		if (tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_SYN) {
			if (sessions > 0) {
				DOCA_LOG_INFO("Already have one alive session, cannot handle more, skip");
				continue;
			}
			action_r.data.l4_port.src_port = match_r.ipv4.l4_port.dst_port;
			action_r.data.l4_port.dst_port = match_r.ipv4.l4_port.src_port;
			action_r.data.ip4.src_ip = match_r.ipv4.dst_ip;
			action_r.data.ip4.dst_ip = match_r.ipv4.src_ip;

			/* Allocate CT entry */
			result = doca_flow_ct_entry_prepare(ct_queue,
							    NULL,
							    DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS,
							    &match_o,
							    0,
							    &match_r,
							    0,
							    entry,
							    NULL);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to prepare CT entry\n");
				return result;
			}
			result = doca_flow_ct_add_entry(ct_queue,
							NULL,
							flags,
							&match_o,
							&match_r,
							&shared_action_o,
							&action_r,
							0,
							0,
							0,
							ct_status,
							*entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add CT pipe an entry: %s", doca_error_get_descr(result));
				return result;
			}
			sessions++;
			nb_process++;
			while (ct_status->nb_processed != nb_process) {
				result = doca_flow_ct_entries_process(port, ct_queue, 0, 0, NULL);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to process Flow CT entries: %s",
						     doca_error_get_descr(result));
					return result;
				}

				if (ct_status->failure) {
					DOCA_LOG_ERR("Flow CT entries process returned with a failure");
					return DOCA_ERROR_BAD_STATE;
				}
			}
			DOCA_LOG_INFO("TCP session was created");
			DOCA_LOG_INFO("waiting for 'FIN'/'RST' packet to arrive before ending the session");
		} else if (tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_FIN || tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_RST) {
			if (sessions == 0) {
				DOCA_LOG_INFO("No alive session to destroy, skip destroy");
				continue;
			}
			result = doca_flow_ct_rm_entry(ct_queue, NULL, flags, *entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to remove CT pipe entry: %s", doca_error_get_descr(result));
				return result;
			}
			/*process entries*/
			result = flow_ct_queue_reserve(port, ct_queue, ct_status, 0);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
				return result;
			}
			*entry = NULL;
			sessions--;
			DOCA_LOG_INFO("TCP session was ended");
		} else {
			DOCA_LOG_WARN("Sample is only able to process 'SYN', 'FIN' and 'RST' packets");
			continue;
		}
		rte_flow_dynf_metadata_set(packets[i], 1);
		packets[i]->ol_flags |= RTE_MBUF_DYNFLAG_TX_METADATA;
		rte_eth_tx_burst(0, 0, &packets[i], 1);
	}
	ct_status->nb_processed = 0;

	return DOCA_SUCCESS;
}

/*
 * Print counters for all pipes
 *
 * @ct_queue [in]: DOCA Flow CT queue number
 * @ct_pipe [in]: CT pipe
 */
static doca_error_t print_pipe_counters(uint16_t ct_queue, struct doca_flow_pipe *ct_pipe)
{
	struct doca_flow_resource_query query_stats, miss_stats;
	doca_error_t result;
	uint64_t last_hit_time;
	memset(&query_stats, 0, sizeof(query_stats));
	memset(&miss_stats, 0, sizeof(miss_stats));

	printf("\n");
	if (ct_entry) {
		result = doca_flow_ct_query_entry(ct_queue,
						  ct_pipe,
						  DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT,
						  ct_entry,
						  &query_stats,
						  &miss_stats,
						  &last_hit_time);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query CT pipe counter: %s", doca_error_get_descr(result));
			return result;
		}
		printf("CT pipe entry: origin_pkts=%ld reply_pkts=%ld\n",
		       query_stats.counter.total_pkts,
		       miss_stats.counter.total_pkts);
	}

	result = doca_flow_resource_query_entry(tcp_flags_filter_entry, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query TCP flags filter pipe counter: %s", doca_error_get_descr(result));
		return result;
	}
	printf("Known packets TCP flags filter pipe pkts: %ld\n", query_stats.counter.total_pkts);

	result = doca_flow_resource_query_entry(ct_miss_entry, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query CT miss pipe counter: %s", doca_error_get_descr(result));
		return result;
	}
	printf("Unknown packets miss pipe pkts: %ld\n", query_stats.counter.total_pkts);

	result = doca_flow_resource_query_entry(rss_entry, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query RSS pipe counter: %s", doca_error_get_descr(result));
		return result;
	}
	printf("RSS pipe pkts: %ld\n", query_stats.counter.total_pkts);

	result = doca_flow_resource_query_entry(egress_entry, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query Egress pipe counter: %s", doca_error_get_descr(result));
		return result;
	}
	printf("Egress pipe pkts: %ld\n", query_stats.counter.total_pkts);
	printf("\n");

	return DOCA_SUCCESS;
}

/*
 * Run flow_ct_tcp sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ct_tcp_actions(uint16_t nb_queues, struct flow_switch_ctx *ctx)
{
	const int nb_ports = 2, nb_entries = 7;
	struct flow_resources resource;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_pipe *egress_pipe, *ct_miss_pipe, *tcp_flags_filter_pipe, *rss_pipe, *root_pipe;
	struct doca_flow_pipe *ct_pipe = NULL;
	struct doca_flow_port *ports[nb_ports];
	struct doca_flow_ct_actions shared_action;
	struct doca_flow_meta o_zone_mask, r_zone_mask;
	struct doca_flow_ct_meta o_modify_mask, r_modify_mask;
	uint32_t actions_mem_size[nb_ports];
	struct entries_status ctrl_status, ct_status;
	uint32_t ct_flags, nb_arm_queues = 1, nb_ctrl_queues = 1, nb_user_actions = 4096, nb_ipv4_sessions = 1024,
			   shared_action_handle, nb_ipv6_sessions = 0; /* On BF2 should always be 0 */
	uint16_t ct_queue = nb_queues;
	doca_error_t result;

	memset(&ctrl_status, 0, sizeof(ctrl_status));
	memset(&ct_status, 0, sizeof(ct_status));
	memset(&resource, 0, sizeof(resource));
	memset(&shared_action, 0, sizeof(shared_action));

	resource.nr_counters = NB_COUNTERS;

	result = init_doca_flow(nb_queues, "switch,hws,isolated", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	/* Dont use zone masking */
	memset(&o_zone_mask, 0, sizeof(o_zone_mask));
	memset(&o_modify_mask, 0, sizeof(o_modify_mask));
	memset(&r_zone_mask, 0, sizeof(r_zone_mask));
	memset(&r_modify_mask, 0, sizeof(r_modify_mask));

	ct_flags = DOCA_FLOW_CT_FLAG_NO_AGING;
	result = init_doca_flow_ct(ct_flags,
				   nb_arm_queues,
				   nb_ctrl_queues,
				   nb_user_actions,
				   NULL,
				   nb_ipv4_sessions,
				   nb_ipv6_sessions,
				   nb_ipv4_sessions + nb_ipv6_sessions,
				   0,
				   false,
				   &o_zone_mask,
				   &o_modify_mask,
				   false,
				   &r_zone_mask,
				   &r_modify_mask);
	if (result != DOCA_SUCCESS) {
		doca_flow_destroy();
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(nb_entries));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_ct_destroy();
		doca_flow_destroy();
		return result;
	}

	result = create_rss_pipe(ports[0], &ctrl_status, &rss_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_egress_pipe(ports[0], 1, &ctrl_status, &egress_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_tcp_flags_filter_pipe(ports[0], &ctrl_status, egress_pipe, rss_pipe, &tcp_flags_filter_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_miss_pipe(ports[0], rss_pipe, &ctrl_status, &ct_miss_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_pipe(ports[0], tcp_flags_filter_pipe, ct_miss_pipe, &ct_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	shared_action.data.action_idx = 0;
	shared_action.data.meta.flow.pkt_meta = DOCA_HTOBE32(1);
	shared_action.data.meta.flow.u32[0] = DOCA_HTOBE32(1);

	result = doca_flow_ct_actions_add_shared(ct_queue, ct_pipe, &shared_action, 1, &shared_action_handle);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_root_pipe(ports[0], true, false, DOCA_FLOW_L4_META_TCP, ct_pipe, &ctrl_status, &root_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup_shared_res;

	result = doca_flow_entries_process(ports[0], 0, DEFAULT_TIMEOUT_US, nb_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process Flow CT entries: %s", doca_error_get_descr(result));
		goto cleanup_shared_res;
	}

	if (ctrl_status.nb_processed != nb_entries || ctrl_status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		result = DOCA_ERROR_BAD_STATE;
		goto cleanup_shared_res;
	}

	DOCA_LOG_INFO("Wait few seconds for 'SYN' packet to arrive");

	sleep(5);
	result = process_packets(ports[0], ct_queue, &ct_status, shared_action_handle, &ct_entry);
	if (result != DOCA_SUCCESS)
		goto dump;

	sleep(7);
	process_packets(ports[0], ct_queue, &ct_status, shared_action_handle, &ct_entry);
	result = DOCA_SUCCESS;

dump:
	/* Print counters for all pipes */
	print_pipe_counters(ct_queue, ct_pipe);
cleanup_shared_res:
	doca_flow_ct_actions_rm_shared(ct_queue, ct_pipe, &shared_action_handle, 1);
cleanup:
	cleanup_procedure(ct_pipe, nb_ports, ports);
	return result;
}
