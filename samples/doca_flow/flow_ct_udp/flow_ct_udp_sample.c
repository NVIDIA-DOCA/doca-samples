/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <sys/time.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>

#include "flow_ct_common.h"
#include <flow_common.h>
#include "flow_switch_common.h"

#define PACKET_BURST 128
#define MIN(a, b) ((a) < (b) ? (a) : (b))

DOCA_LOG_REGISTER(FLOW_CT_UDP);

/*
 * Create RSS pipe
 *
 * @port [in]: Pipe port
 * @status [in]: User context for adding entry
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
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_create(cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RSS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_add_entry(0, *pipe, &match, 0, NULL, NULL, &fwd, 0, status, NULL);
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
	struct doca_flow_match mask;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
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
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add CT pipe: %s", doca_error_get_descr(result));
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Create VxLAN encapsulation pipe
 *
 * @port [in]: Pipe port
 * @port_id [in]: Forward port ID
 * @status [in]: User context for adding entry
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_vxlan_encap_pipe(struct doca_flow_port *port,
					    int port_id,
					    struct entries_status *status,
					    struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_actions *actions_list[] = {&actions};
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	actions.encap_cfg.encap.tun.vxlan_tun_id = 0xffffffff;
	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.encap_cfg.is_l2 = true;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "VXLAN_ENCAP_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_list, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VxLAN Encap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	memset(&actions, 0, sizeof(actions));
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);
	actions.encap_cfg.encap.outer.ip4.src_ip = BE_IPV4_ADDR(11, 21, 31, 41);
	actions.encap_cfg.encap.outer.ip4.dst_ip = BE_IPV4_ADDR(81, 81, 81, 81);
	actions.encap_cfg.encap.outer.ip4.ttl = 17;
	actions.encap_cfg.encap.tun.vxlan_tun_id = DOCA_HTOBE32(0xadadad);

	result = doca_flow_pipe_add_entry(0, *pipe, &match, 0, &actions, NULL, NULL, 0, status, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add VxLAN Encap pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process UDP entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create pipe to count packets based on 5 tuple match
 *
 * @port [in]: Pipe port
 * @fwd_pipe [in]: Next pipe pointer
 * @status [in]: User context for adding entry
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_count_pipe(struct doca_flow_port *port,
				      struct doca_flow_pipe *fwd_pipe,
				      struct entries_status *status,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* 5 tuple match */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;
	match.outer.udp.l4_port.src_port = 0xffff;
	match.outer.udp.l4_port.dst_port = 0xffff;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "COUNT_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_pipe;

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = fwd_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create count pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	memset(&match, 0, sizeof(match));
	match.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 2, 3, 4);
	match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(80);
	match.outer.udp.l4_port.src_port = DOCA_HTOBE16(1234);

	result = doca_flow_pipe_add_entry(0, *pipe, &match, 0, NULL, NULL, NULL, 0, status, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add count pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process count entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Parse UDP packet to update CT tables
 *
 * @packet [in]: Packet to parse
 * @match_o [out]: Origin match struct to fill
 * @match_r [out]: Reply match struct to fill
 */
static void parse_packet(struct rte_mbuf *packet,
			 struct doca_flow_ct_match *match_o,
			 struct doca_flow_ct_match *match_r)
{
	uint8_t *l4_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	const struct rte_udp_hdr *udp_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));

	match_o->ipv4.src_ip = ipv4_hdr->src_addr;
	match_o->ipv4.dst_ip = ipv4_hdr->dst_addr;
	match_r->ipv4.src_ip = match_o->ipv4.dst_ip;
	match_r->ipv4.dst_ip = match_o->ipv4.src_ip;

	l4_hdr = (typeof(l4_hdr))ipv4_hdr + rte_ipv4_hdr_len(ipv4_hdr);
	udp_hdr = (typeof(udp_hdr))l4_hdr;

	match_o->ipv4.l4_port.src_port = udp_hdr->src_port;
	match_o->ipv4.l4_port.dst_port = udp_hdr->dst_port;
	match_r->ipv4.l4_port.src_port = match_o->ipv4.l4_port.dst_port;
	match_r->ipv4.l4_port.dst_port = match_o->ipv4.l4_port.src_port;

	match_o->ipv4.next_proto = DOCA_FLOW_PROTO_UDP;
	match_r->ipv4.next_proto = DOCA_FLOW_PROTO_UDP;
}

/*
 * Dequeue packets from DPDK queues, parse and update CT tables with new connection 5 tuple
 *
 * @port [in]: Port id to which an entry should be inserted
 * @ct_queue [in]: DOCA Flow CT queue number
 * @ct_status [in]: User context for adding CT entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t process_packets(struct doca_flow_port *port, uint16_t ct_queue, struct entries_status *ct_status)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_ct_match match_o;
	struct doca_flow_ct_match match_r;
	struct doca_flow_pipe_entry *entry;
	uint32_t entry_flags, prepare_flags;
	doca_error_t result;
	int i, nb_packets = 0, total_packets_processed = 0;
	uint64_t timeout_s = 5; /* Timeout in seconds */
	time_t end_time, max_end_time;

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
		total_packets_processed += nb_packets;
		/* Updated timeout */
		end_time = MIN(time(NULL) + 2, max_end_time);

		DOCA_LOG_INFO("Sample received %d packets", nb_packets);
		for (i = 0; i < PACKET_BURST && i < nb_packets; i++) {
			parse_packet(packets[i], &match_o, &match_r);
			prepare_flags = DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS |
					DOCA_FLOW_CT_ENTRY_FLAGS_DUP_FILTER_ORIGIN |
					DOCA_FLOW_CT_ENTRY_FLAGS_DUP_FILTER_REPLY;
			entry_flags = DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
				      DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY;
			result = flow_ct_create_entry(port,
						      ct_queue,
						      NULL,
						      prepare_flags,
						      entry_flags,
						      &match_o,
						      &match_r,
						      packets[i]->hash.rss,
						      packets[i]->hash.rss,
						      NULL,
						      NULL,
						      0,
						      0,
						      0,
						      ct_status,
						      &entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create CT entry\n");
				return result;
			}
			DOCA_LOG_INFO(
				"Entry %d matches on the 5-tuple of the incoming packet. Reply direction matches on the inversed origin direction",
				i);
		}
	} while (time(NULL) < end_time);

	if (total_packets_processed == 0) {
		DOCA_LOG_ERR("Sample didn't receive packets within 5 seconds timeout");
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_ct_udp sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ct_udp(uint16_t nb_queues, struct flow_switch_ctx *ctx)
{
	const int nb_ports = 1, nb_entries = 6;
	struct flow_resources resource;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_pipe *rss_pipe, *encap_pipe, *count_pipe, *ct_pipe = NULL, *udp_pipe;
	struct doca_flow_port *ports[nb_ports];
	struct doca_flow_meta o_zone_mask, r_zone_mask;
	struct doca_flow_ct_meta o_modify_mask, r_modify_mask;
	uint32_t actions_mem_size[nb_ports];
	struct entries_status ctrl_status, ct_status;
	uint32_t ct_flags, nb_arm_queues = 1, nb_ctrl_queues = 1, nb_user_actions = 0, nb_ipv4_sessions = 1024,
			   nb_ipv6_sessions = 0; /* On BF2 should always be 0 */
	uint16_t ct_queue = nb_queues;
	doca_error_t result;

	memset(&ctrl_status, 0, sizeof(ctrl_status));
	memset(&ct_status, 0, sizeof(ct_status));
	memset(&resource, 0, sizeof(resource));

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = 1;
	resource.nr_rss = 1;
	resource.nr_encap = 1;

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

	ct_flags = DOCA_FLOW_CT_FLAG_NO_AGING | DOCA_FLOW_CT_FLAG_NO_COUNTER;
	result = init_doca_flow_ct(ct_flags,
				   nb_arm_queues,
				   nb_ctrl_queues,
				   nb_user_actions,
				   NULL,
				   nb_ipv4_sessions,
				   nb_ipv6_sessions,
				   0,
				   DUP_FILTER_CONN_NUM,
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
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_ct_destroy();
		doca_flow_destroy();
		return result;
	}

	result = create_rss_pipe(ports[0], &ctrl_status, &rss_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_vxlan_encap_pipe(ports[0], 0, &ctrl_status, &encap_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_count_pipe(ports[0], rss_pipe, &ctrl_status, &count_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_pipe(ports[0], encap_pipe, count_pipe, &ct_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_root_pipe(ports[0], true, false, DOCA_FLOW_L4_META_UDP, ct_pipe, &ctrl_status, &udp_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	if (ctrl_status.nb_processed != nb_entries || ctrl_status.failure) {
		DOCA_LOG_ERR("Failed to process control path entries");
		result = DOCA_ERROR_BAD_STATE;
		goto cleanup;
	}

	DOCA_LOG_INFO("Wait a few seconds for packets to arrive");
	result = process_packets(ports[0], ct_queue, &ct_status);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	DOCA_LOG_INFO("Send packets of the same session. Wait a few seconds for packets to arrive");
	sleep(5);

cleanup:
	cleanup_procedure(ct_pipe, nb_ports, ports);
	return result;
}
