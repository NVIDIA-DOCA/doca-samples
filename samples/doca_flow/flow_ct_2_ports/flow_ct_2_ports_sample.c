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

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>

#include "flow_ct_common.h"
#include <flow_common.h>
#include "flow_switch_common.h"

#define PACKET_BURST 128

DOCA_LOG_REGISTER(FLOW_CT_2_PORTS);

/*
 * Create pipe and entry
 *
 * @port [in]: Pipe port
 * @status [in]: User context for adding entry
 * @pipe_name [in]: Name for the pipe
 * @fwd [in]: Forward configuration
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_pipe_and_entry(struct doca_flow_port *port,
					  struct entries_status *status,
					  const char *pipe_name,
					  struct doca_flow_fwd *fwd,
					  struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, pipe_name, DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(cfg, fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RSS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_add_entry(0, *pipe, NULL, NULL, NULL, NULL, 0, status, NULL);
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
 * @fwd_match_pipe [in]: Forward match pipe pointer
 * @fwd_miss_pipe [in]: Forward miss pipe pointer
 * @pipe [out]: Created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ct_pipe(struct doca_flow_port *port,
				   struct doca_flow_pipe *fwd_match_pipe,
				   struct doca_flow_pipe *fwd_miss_pipe,
				   struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

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

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_match_pipe;

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
 * Create counter pipe to count all packets
 *
 * @port [in]: Pipe port
 * @fwd_pipe [in]: Next pipe pointer
 * @status [in]: User context for adding entry
 * @pipe_name [in]: Name for the counter pipe
 * @pipe [out]: Created pipe pointer
 * @entry [out]: Created pipe entry pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_counter_pipe(struct doca_flow_port *port,
					struct doca_flow_pipe *fwd_pipe,
					struct entries_status *status,
					const char *pipe_name,
					struct doca_flow_pipe **pipe,
					struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, pipe_name, DOCA_FLOW_PIPE_BASIC, false);
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

	/* Since match is empty (matches all packets), we don't need fwd_miss */
	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create counter pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_add_entry(0, *pipe, NULL, NULL, &monitor, NULL, 0, status, entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add counter pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process counter entry: %s", doca_error_get_descr(result));

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
 * @port [in]: Port to which an entry should be inserted
 * @port_id [in]: Port id to which packet can be received
 * @ct_queue [in]: DOCA Flow CT queue number
 * @ct_pipe [in]: Pipe of CT
 * @ct_status [in]: User context for adding CT entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t process_packets(struct doca_flow_port *port,
				    uint16_t port_id,
				    uint16_t ct_queue,
				    struct doca_flow_pipe *ct_pipe,
				    struct entries_status *ct_status)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_ct_match match_o;
	struct doca_flow_ct_match match_r;
	struct doca_flow_pipe_entry *entry;
	uint32_t flags;
	doca_error_t result;
	int i, entries, nb_packets = 0;
	bool conn_found = false;

	memset(&match_o, 0, sizeof(match_o));
	memset(&match_r, 0, sizeof(match_r));

	DOCA_LOG_INFO("send UDP packet with DIP 1.1.1.1 on port %u. wait a few seconds for packet to arrive", port_id);
	do {
		nb_packets = rte_eth_rx_burst(port_id, 0, packets, PACKET_BURST);
	} while (nb_packets == 0);

	DOCA_LOG_INFO("%d packets received", nb_packets);

	entries = 0;
	DOCA_LOG_INFO("Sample received %d packets on port %d", nb_packets, port_id);
	for (i = 0; i < PACKET_BURST && i < nb_packets; i++) {
		parse_packet(packets[i], &match_o, &match_r);
		flags = DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS | DOCA_FLOW_CT_ENTRY_FLAGS_DUP_FILTER_ORIGIN |
			DOCA_FLOW_CT_ENTRY_FLAGS_DUP_FILTER_REPLY;
		/* Allocate CT entry */
		result = doca_flow_ct_entry_prepare(ct_queue,
						    ct_pipe,
						    flags,
						    &match_o,
						    packets[i]->hash.rss,
						    &match_r,
						    packets[i]->hash.rss,
						    &entry,
						    &conn_found);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to prepare CT entry\n");
			return result;
		}

		if (!conn_found) {
			flags = DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
				DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY;
			result = doca_flow_ct_add_entry(ct_queue,
							ct_pipe,
							flags,
							&match_o,
							&match_r,
							NULL,
							NULL,
							0,
							0,
							0,
							ct_status,
							entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add CT pipe an entry: %s", doca_error_get_descr(result));
				return result;
			}
			entries++;
		}
	}

	DOCA_LOG_INFO("%d CT connections created", nb_packets);

	while (ct_status->nb_processed != entries) {
		result = doca_flow_ct_entries_process(port, ct_queue, 0, 0, NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process Flow CT entries: %s", doca_error_get_descr(result));
			return result;
		}

		if (ct_status->failure) {
			DOCA_LOG_ERR("Flow CT entries process returned with a failure");
			return DOCA_ERROR_BAD_STATE;
		}
	}

	DOCA_LOG_INFO("%d CT connections processed\n", ct_status->nb_processed);

	return DOCA_SUCCESS;
}

/*
 * Connection Tracking Sample with Direct Port Forwarding
 *
 * This sample demonstrates DOCA Flow Connection Tracking (CT) with two different
 * packet processing paths:
 *
 * 1. CT MATCH Path (Direct Port Forwarding):
 *    Known connections -> Direct forwarding to port_forward_pipe
 *
 * 2. CT MISS Path (Software Processing):
 *    Unknown connections -> counter_miss_pipe -> rss_software_pipe -> queue 0 (Software RX) -> CPU processing
 *
 * With direct port forwarding, packets that match CT entries are sent straight to the port for transmission,
 * while packets that miss are redirected to software for further handling.
 *
 * flow_ct_2_ports
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow device context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ct_2_ports(uint16_t nb_queues, struct flow_dev_ctx *ctx)
{
	const int nb_entries = 6;
	int nb_ports = ctx->nb_ports;
	struct flow_resources resource;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_pipe *rss_software_pipes[nb_ports]; /* RSS pipe for miss packets -> software RX queue */
	struct doca_flow_pipe *port_forward_pipes[nb_ports]; /* Port fwd pipe for match packets -> direct port fwd */
	struct doca_flow_pipe *counter_miss_pipes[nb_ports]; /* Counter pipe for CT miss packets */
	struct doca_flow_pipe *ct_pipes[nb_ports];	     /* Connection tracking pipe */
	struct doca_flow_pipe *udp_pipes[nb_ports];	     /* UDP root pipe */
	struct doca_flow_fwd port_forward_fwd, rss_software_fwd;
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_meta o_zone_mask, r_zone_mask;
	struct doca_flow_ct_meta o_modify_mask, r_modify_mask;
	struct entries_status ctrl_status, ct_status;
	uint32_t ct_flags, nb_arm_queues = 1, nb_ctrl_queues = 1, nb_user_actions = 0, nb_ipv4_sessions = 1024,
			   nb_ipv6_sessions = 0; /* On BF2 should always be 0 */
	uint16_t ct_queue = nb_queues;
	struct doca_flow_pipe_entry *counter_miss_entries[nb_ports];
	doca_error_t result;
	int i;

	/*
	 * Queue configuration:
	 * - Total queues configured for DOCA Flow: nb_queues (passed as parameter)
	 * - RSS pipe uses only 1 queue (NR_QUEUES=1) for CT miss packets
	 * - Queue 0: Used by RSS pipe for software processing of CT miss packets
	 * - Queue 1 and higher: Available but unused in this sample
	 */
	uint16_t NR_QUEUES = 1;
	uint16_t SOFTWARE_RX_QUEUE = 0;

	memset(&resource, 0, sizeof(resource));
	memset(rss_software_pipes, 0, sizeof(struct doca_flow_pipe *) * nb_ports);
	memset(port_forward_pipes, 0, sizeof(struct doca_flow_pipe *) * nb_ports);
	memset(counter_miss_pipes, 0, sizeof(struct doca_flow_pipe *) * nb_ports);
	memset(ct_pipes, 0, sizeof(struct doca_flow_pipe *) * nb_ports);
	memset(udp_pipes, 0, sizeof(struct doca_flow_pipe *) * nb_ports);

	/* Port forward configuration */
	port_forward_fwd.type = DOCA_FLOW_FWD_PORT;

	/* RSS configuration */
	rss_software_fwd.type = DOCA_FLOW_FWD_RSS;
	rss_software_fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	rss_software_fwd.rss.nr_queues = NR_QUEUES;
	rss_software_fwd.rss.queues_array = &SOFTWARE_RX_QUEUE;
	rss_software_fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;

	resource.nr_counters = 1;

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
	result = init_doca_flow_switch_ports(ctx->devs_manager, ctx->nb_devs, ports, nb_ports, actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_ct_destroy();
		doca_flow_destroy();
		return result;
	}

	for (i = 0; i < nb_ports; i++) {
		memset(&ctrl_status, 0, sizeof(ctrl_status));

		/* Create RSS pipe for CT miss packets (software processing) */
		result = create_pipe_and_entry(ports[i],
					       &ctrl_status,
					       "RSS_SOFTWARE_PIPE",
					       &rss_software_fwd,
					       &rss_software_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		/* Create port forward pipe for CT match packets (direct port forwarding) */
		port_forward_fwd.port_id = i;
		result = create_pipe_and_entry(ports[i],
					       &ctrl_status,
					       "PORT_FORWARD_PIPE",
					       &port_forward_fwd,
					       &port_forward_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		/* Create counter pipe for CT miss packets (CT cannot count miss packets) */
		result = create_counter_pipe(ports[i],
					     rss_software_pipes[i],
					     &ctrl_status,
					     "COUNTER_MISS_PIPE",
					     &counter_miss_pipes[i],
					     &counter_miss_entries[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		/* Create CT pipe: match->port_forward (direct) and miss->counter_miss */
		result = create_ct_pipe(ports[i], port_forward_pipes[i], counter_miss_pipes[i], &ct_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = create_ct_root_pipe(ports[i],
					     true,
					     false,
					     DOCA_FLOW_L4_META_UDP,
					     ct_pipes[i],
					     &ctrl_status,
					     &udp_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		if (ctrl_status.nb_processed != nb_entries || ctrl_status.failure) {
			DOCA_LOG_ERR("Failed to process control path entries");
			result = DOCA_ERROR_BAD_STATE;
			goto cleanup;
		}
	}

	DOCA_LOG_INFO("Please send same UDP packets to see the CT entries being created\n");
	for (i = 0; i < nb_ports; i++) {
		memset(&ct_status, 0, sizeof(ct_status));
		result = process_packets(ports[i], i, ct_queue, ct_pipes[i], &ct_status);
		if (result != DOCA_SUCCESS)
			goto cleanup;
	}

	sleep(3);

cleanup:
	for (i = 0; i < nb_ports; i++) {
		if (udp_pipes[i] != NULL)
			doca_flow_pipe_destroy(udp_pipes[i]);
		if (ct_pipes[i] != NULL)
			doca_flow_pipe_destroy(ct_pipes[i]);
		if (port_forward_pipes[i] != NULL)
			doca_flow_pipe_destroy(port_forward_pipes[i]);
		if (counter_miss_pipes[i] != NULL)
			doca_flow_pipe_destroy(counter_miss_pipes[i]);
		if (rss_software_pipes[i] != NULL)
			doca_flow_pipe_destroy(rss_software_pipes[i]);
	}
	cleanup_procedure(NULL, nb_ports, ports);
	return result;
}
