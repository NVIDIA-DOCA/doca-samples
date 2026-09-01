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

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#include <doca_bitfield.h>
#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_TRIM);

#define TRIM_RESOURCE_NUM 1
#define NB_PORTS 2
/* Each port has up to 4 entries (ingress/post-trim x port/RSS), each with one non-shared counter */
#define NB_COUNTERS_PER_PORT 4
#define RSS_QUEUE_ID 0
#define WAIT_SECS 10
#define PKT_BURST_SIZE 32
#define FLOW_TRIM_IPV4_FULL_MASK 0xffffffff
#define FLOW_TRIM_FIRST_DST_IP BE_IPV4_ADDR(8, 8, 8, 8)
#define FLOW_TRIM_SECOND_DST_IP BE_IPV4_ADDR(9, 9, 9, 9)
#define FLOW_TRIM_FIRST_DST_IP_STR "8.8.8.8"
#define FLOW_TRIM_SECOND_DST_IP_STR "9.9.9.9"
#define FLOW_TRIM_WORKER_SLEEP_US 10000
#define FLOW_TRIM_GET_PORT_PEER_ID(port_id) ((port_id) ^ 1)
#define FLOW_TRIM_SRC_MAC_PORT_BYTE 5

struct trim_port_ctx {
	uint32_t trim_id;
	struct doca_flow_pipe *post_trim_pipe;
	struct doca_flow_pipe *ingress_pipe;
	struct doca_flow_pipe_entry *post_trim_port_entry;
	struct doca_flow_pipe_entry *post_trim_rss_entry;
	struct doca_flow_pipe_entry *ingress_port_entry;
	struct doca_flow_pipe_entry *ingress_rss_entry;
};

static doca_error_t create_post_trim_pipe(struct doca_flow_port *port,
					  struct doca_flow_pipe **pipe,
					  bool is_switch_mode)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dst_ip = FLOW_TRIM_IPV4_FULL_MASK;
	match_mask.outer.ip4.dst_ip = FLOW_TRIM_IPV4_FULL_MASK;
	SET_MAC_ADDR(match.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	match_mask.outer.eth.src_mac[FLOW_TRIM_SRC_MAC_PORT_BYTE] = UINT8_MAX;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create post-trim pipe cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "POST_TRIM_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set post-trim pipe cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg,
					       is_switch_mode ? DOCA_FLOW_PIPE_DOMAIN_DEFAULT :
								DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set post-trim pipe domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set post-trim pipe match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set post-trim pipe monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t add_post_trim_port_entry(struct doca_flow_pipe *pipe,
					     uint16_t port_id,
					     struct entries_status *status,
					     struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.outer.ip4.dst_ip = FLOW_TRIM_FIRST_DST_IP;
	match.outer.eth.src_mac[FLOW_TRIM_SRC_MAC_PORT_BYTE] = (uint8_t)port_id;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = FLOW_TRIM_GET_PORT_PEER_ID(port_id);

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, &monitor, &fwd, 0, status, entry);
}

static doca_error_t add_post_trim_rss_entry(struct doca_flow_pipe *pipe,
					    uint16_t port_id,
					    uint16_t *rss_queues,
					    int nr_queues,
					    struct entries_status *status,
					    struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.outer.ip4.dst_ip = FLOW_TRIM_SECOND_DST_IP;
	match.outer.eth.src_mac[FLOW_TRIM_SRC_MAC_PORT_BYTE] = (uint8_t)port_id;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.nr_queues = nr_queues;

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, &monitor, &fwd, 0, status, entry);
}

static doca_error_t create_shared_resource_trim(struct doca_flow_port *port,
						struct doca_flow_pipe *post_trim_pipe,
						uint32_t trim_size,
						uint32_t *trim_id)
{
	struct doca_flow_shared_resource_cfg res_cfg = {0};
	doca_error_t result;

	res_cfg.trim_cfg.size = trim_size;
	res_cfg.trim_cfg.target.type = DOCA_FLOW_FWD_PIPE;
	res_cfg.trim_cfg.target.next_pipe = post_trim_pipe;

	result = doca_flow_port_shared_resource_get(port, DOCA_FLOW_SHARED_RESOURCE_TRIM, trim_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate trim_id: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_shared_resource_set_cfg(port, DOCA_FLOW_SHARED_RESOURCE_TRIM, *trim_id, &res_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set trim shared resource cfg: %s", doca_error_get_descr(result));
		(void)doca_flow_port_shared_resource_put(port, DOCA_FLOW_SHARED_RESOURCE_TRIM, *trim_id);
		return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t create_ingress_trim_pipe(struct doca_flow_port *port,
					     uint32_t trim_id,
					     struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions, actions_mask, *actions_arr[NB_ACTIONS_ARR], *actions_mask_arr[NB_ACTIONS_ARR];
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&actions_mask, 0, sizeof(actions_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.port_id = UINT16_MAX;
	match_mask.parser_meta.port_id = UINT16_MAX;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dst_ip = FLOW_TRIM_IPV4_FULL_MASK;
	match_mask.outer.ip4.dst_ip = FLOW_TRIM_IPV4_FULL_MASK;
	/* The sample intentionally overwrites `src_mac[last_byte]` with ingress `port_id` to allow post-trim pipe
	 * matching/routing, that packet captures will therefore show a modified src MAC. */
	SET_MAC_ADDR(actions.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions_mask.outer.eth.src_mac[FLOW_TRIM_SRC_MAC_PORT_BYTE] = UINT8_MAX;
	actions_arr[0] = &actions;
	actions_mask_arr[0] = &actions_mask;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress pipe cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "TRIM_INGRESS_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ingress pipe cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ingress pipe match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_mask_arr, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ingress pipe actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ingress pipe monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_TRIM;
	fwd.trim.trim_id = trim_id;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t add_ingress_trim_entry(struct doca_flow_pipe *pipe,
					   uint16_t port_id,
					   doca_be32_t dst_ip,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_monitor monitor;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&monitor, 0, sizeof(monitor));

	match.parser_meta.port_id = port_id;
	match.outer.ip4.dst_ip = dst_ip;
	actions.outer.eth.src_mac[FLOW_TRIM_SRC_MAC_PORT_BYTE] = (uint8_t)port_id;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, &actions, &monitor, NULL, 0, status, entry);
}

static const char *l4_type_to_str(uint8_t proto)
{
	switch (proto) {
	case IPPROTO_TCP:
		return "TCP";
	case IPPROTO_UDP:
		return "UDP";
	case IPPROTO_ICMP:
		return "ICMP";
	case IPPROTO_ESP:
		return "ESP";
	default:
		return "OTHER";
	}
}

static void worker_process_packets(uint16_t port_id)
{
	struct rte_mbuf *packets[PKT_BURST_SIZE];
	uint32_t nb_rx;
	uint32_t i;

	nb_rx = rte_eth_rx_burst(port_id, RSS_QUEUE_ID, packets, PKT_BURST_SIZE);
	if (nb_rx == 0)
		return;

	for (i = 0; i < nb_rx; i++) {
		struct rte_mbuf *mbuf = packets[i];
		const struct rte_ether_hdr *eth;
		const struct rte_ipv4_hdr *ip4;
		uint16_t ip_total_len;
		uint32_t actual_len;
		uint32_t original_len;
		uint8_t l4_proto = 0;

		eth = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
		actual_len = rte_pktmbuf_pkt_len(mbuf);
		if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) &&
		    actual_len >= sizeof(*eth) + sizeof(*ip4)) {
			ip4 = (const struct rte_ipv4_hdr *)(eth + 1);
			ip_total_len = rte_be_to_cpu_16(ip4->total_length);
			original_len = (uint32_t)ip_total_len + (uint32_t)sizeof(*eth);
			l4_proto = ip4->next_proto_id;
		} else {
			original_len = actual_len;
		}

		DOCA_LOG_INFO("port=%u captured L4=%s original_size=%u actual_size=%u",
			      port_id,
			      l4_type_to_str(l4_proto),
			      original_len,
			      actual_len);
		rte_pktmbuf_free(mbuf);
	}
}

static doca_error_t query_entry_and_log(struct doca_flow_pipe_entry *entry,
					const char *label,
					struct doca_flow_resource_query *prev_stats)
{
	struct doca_flow_resource_query stats;
	doca_error_t result;

	if (entry == NULL)
		return DOCA_SUCCESS;

	memset(&stats, 0, sizeof(stats));
	result = doca_flow_resource_query_entry(entry, &stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query %s counter: %s", label, doca_error_get_descr(result));
		return result;
	}

	if (prev_stats->counter.total_pkts == stats.counter.total_pkts) {
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("counter[%s]: total_pkts=%" PRIu64 " total_bytes=%" PRIu64,
		      label,
		      stats.counter.total_pkts,
		      stats.counter.total_bytes);
	*prev_stats = stats;
	return DOCA_SUCCESS;
}

doca_error_t flow_trim(int nb_queues,
		       bool is_switch_mode,
		       uint32_t trim_size,
		       struct flow_devs_manager devs_manager[],
		       uint16_t nb_devs)
{
	int nb_ports = NB_PORTS;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[NB_PORTS] = {0};
	struct doca_flow_port *switch_port = NULL;
	struct trim_port_ctx *flow_port_ctx;
	struct doca_flow_port *flow_port;
	uint32_t actions_mem_size[NB_PORTS];
	struct trim_port_ctx port_ctx[NB_PORTS] = {0};
	struct entries_status status;
	uint16_t rss_queues[1] = {RSS_QUEUE_ID};
	const int entries_per_port_vnf = 2;
	const int entries_per_port_switch = 4;
	int entries_per_port;
	int nb_pipeline_ports = NB_PORTS;
	doca_error_t result;
	uint64_t deadline_sec;
	int port_id;
	const char *mode_str = is_switch_mode ? "switch,hws" : "vnf,hws";
	char label[64];
	struct doca_flow_resource_query prev_stats[NB_PORTS][NB_COUNTERS_PER_PORT] = {0};

	resource.nr_trim = TRIM_RESOURCE_NUM;
	resource.nr_counters = is_switch_mode ? NB_COUNTERS_PER_PORT * NB_PORTS : NB_COUNTERS_PER_PORT;
	resource.nr_rss = 1;
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	result = init_doca_flow(nb_queues, mode_str, &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	if (is_switch_mode) {
		entries_per_port = entries_per_port_switch;
		ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(entries_per_port_switch * nb_ports));
		result =
			init_doca_flow_switch_ports(devs_manager, nb_devs, ports, nb_ports, actions_mem_size, &resource);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init DOCA Switch ports: %s", doca_error_get_descr(result));
			doca_flow_destroy();
			return result;
		}
		switch_port = doca_flow_port_switch_get(ports[0]);
		if (switch_port == NULL) {
			DOCA_LOG_ERR("Failed to get switch port");
			doca_flow_destroy();
			return DOCA_ERROR_BAD_STATE;
		}
		nb_pipeline_ports = 1;
	} else {
		entries_per_port = entries_per_port_vnf;
		ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(entries_per_port_vnf));
		result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init DOCA VNF ports: %s", doca_error_get_descr(result));
			doca_flow_destroy();
			return result;
		}
	}

	/* First create the post-trim pipes on all ports to allow cross-port fwd post trim in VNF mode */
	for (port_id = 0; port_id < nb_pipeline_ports; port_id++) {
		flow_port = switch_port != NULL ? switch_port : ports[port_id];

		result = create_post_trim_pipe(flow_port, &port_ctx[port_id].post_trim_pipe, is_switch_mode);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create post-trim pipe: %s", doca_error_get_descr(result));
			goto cleanup;
		}
	}

	for (port_id = 0; port_id < nb_pipeline_ports; port_id++) {
		struct doca_flow_pipe *post_trim_pipe;
		if (switch_port != NULL) {
			flow_port = switch_port;
			flow_port_ctx = &port_ctx[0];
			post_trim_pipe = port_ctx[0].post_trim_pipe;
		} else {
			flow_port = ports[port_id];
			flow_port_ctx = &port_ctx[port_id];
			post_trim_pipe = port_ctx[FLOW_TRIM_GET_PORT_PEER_ID(port_id)].post_trim_pipe;
		}

		result = create_shared_resource_trim(flow_port, post_trim_pipe, trim_size, &flow_port_ctx->trim_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create trim shared resource: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		result = create_ingress_trim_pipe(flow_port, flow_port_ctx->trim_id, &flow_port_ctx->ingress_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ingress trim pipe: %s", doca_error_get_descr(result));
			goto cleanup;
		}
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		if (switch_port != NULL) {
			flow_port = switch_port;
			flow_port_ctx = &port_ctx[0];
		} else {
			flow_port = ports[port_id];
			flow_port_ctx = &port_ctx[port_id];
		}

		memset(&status, 0, sizeof(status));

		result = add_post_trim_port_entry(flow_port_ctx->post_trim_pipe,
						  (switch_port != NULL) ? port_id : FLOW_TRIM_GET_PORT_PEER_ID(port_id),
						  &status,
						  &port_ctx[port_id].post_trim_port_entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add post-trim port entry: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		if (is_switch_mode) {
			/* In switch mode, we can add RSS entry to the post-trim pipe */
			result = add_post_trim_rss_entry(flow_port_ctx->post_trim_pipe,
							 port_id,
							 rss_queues,
							 1,
							 &status,
							 &port_ctx[port_id].post_trim_rss_entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add post-trim RSS entry: %s", doca_error_get_descr(result));
				goto cleanup;
			}
		} else {
			/* In VNF mode, we can't RSS from EGRESS domain */
		}

		result = add_ingress_trim_entry(flow_port_ctx->ingress_pipe,
						port_id,
						FLOW_TRIM_FIRST_DST_IP,
						&status,
						&port_ctx[port_id].ingress_port_entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add ingress trim entry for " FLOW_TRIM_FIRST_DST_IP_STR ": %s",
				     doca_error_get_descr(result));
			goto cleanup;
		}

		if (is_switch_mode) {
			/* Add entry for the traffic going to the RSS queue */
			result = add_ingress_trim_entry(flow_port_ctx->ingress_pipe,
							port_id,
							FLOW_TRIM_SECOND_DST_IP,
							&status,
							&port_ctx[port_id].ingress_rss_entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add ingress trim entry for " FLOW_TRIM_SECOND_DST_IP_STR ": %s",
					     doca_error_get_descr(result));
				goto cleanup;
			}
		}

		result = doca_flow_entries_process(flow_port, 0, DEFAULT_TIMEOUT_US, entries_per_port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		if (status.nb_processed != entries_per_port || status.failure) {
			DOCA_LOG_ERR("Failed to process entries on port %d", port_id);
			result = DOCA_ERROR_BAD_STATE;
			goto cleanup;
		}
	}

	DOCA_LOG_INFO("Wait %d seconds for packets to arrive (trim size %u bytes, RX queue %u on each port)",
		      WAIT_SECS,
		      trim_size,
		      RSS_QUEUE_ID);

	deadline_sec = (uint64_t)time(NULL) + WAIT_SECS;
	while ((uint64_t)time(NULL) < deadline_sec) {
		usleep(FLOW_TRIM_WORKER_SLEEP_US);
		for (port_id = 0; port_id < nb_ports; port_id++) {
			if (port_id < nb_pipeline_ports)
				worker_process_packets((uint16_t)port_id);

			snprintf(label,
				 sizeof(label),
				 "port=%d pre  ingress_trim_pipe(" FLOW_TRIM_FIRST_DST_IP_STR ")",
				 port_id);
			(void)query_entry_and_log(port_ctx[port_id].ingress_port_entry, label, &prev_stats[port_id][0]);

			snprintf(label,
				 sizeof(label),
				 "port=%d post post_trim_pipe(" FLOW_TRIM_FIRST_DST_IP_STR "->FWD_PORT)",
				 port_id);
			(void)query_entry_and_log(port_ctx[port_id].post_trim_port_entry,
						  label,
						  &prev_stats[port_id][1]);

			snprintf(label,
				 sizeof(label),
				 "port=%d pre  ingress_trim_pipe(" FLOW_TRIM_SECOND_DST_IP_STR ")",
				 port_id);
			(void)query_entry_and_log(port_ctx[port_id].ingress_rss_entry, label, &prev_stats[port_id][2]);

			snprintf(label,
				 sizeof(label),
				 "port=%d post post_trim_pipe(" FLOW_TRIM_SECOND_DST_IP_STR "->FWD_RSS)",
				 port_id);
			(void)query_entry_and_log(port_ctx[port_id].post_trim_rss_entry,
						  label,
						  &prev_stats[port_id][3]);
		}
	}

cleanup:
	stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
