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
#define MARK_MASK 0x00ffffff
#define PKT_MARK DOCA_HTOBE32(0x123456 & MARK_MASK) /* 24 bits mark */
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static uint16_t sessions = 0;

DOCA_LOG_REGISTER(FLOW_CT_TCP_WIRE_TO_WIRE);

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
		return result;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_add_entry(0, *pipe, &match, 0, NULL, NULL, &fwd, 0, status, NULL);
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
	struct doca_flow_actions actions;
	struct doca_flow_actions *actions_arr[1];
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd));
	memset(&actions, 0, sizeof(actions));

	actions.meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = &actions;

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
	result = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CT pipe actions: %s", doca_error_get_descr(result));
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
	struct rte_ipv4_hdr *ipv4_hdr;
	const struct rte_tcp_hdr *tcp_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));

	match_o->ipv4.src_ip = ipv4_hdr->src_addr;
	match_o->ipv4.dst_ip = ipv4_hdr->dst_addr;
	match_r->ipv4.src_ip = match_o->ipv4.dst_ip;
	match_r->ipv4.dst_ip = match_o->ipv4.src_ip;

	tcp_hdr = (typeof(tcp_hdr))(ipv4_hdr + 1);

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
 * @entry [in/out]: CT entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t process_packets(struct doca_flow_port *port,
				    uint16_t ct_queue,
				    struct entries_status *ct_status,
				    struct doca_flow_pipe_entry **entries)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_ct_match match_o;
	struct doca_flow_ct_match match_r;
	struct doca_flow_ct_actions actions;
	uint32_t prepare_flags = DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS;
	uint32_t entry_flags = DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
			       DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY;
	uint8_t tcp_state;
	doca_error_t result;
	int rc, i, nb_packets, total_valid_packets = 0;
	uint64_t timeout_s = 5; /* Timeout in seconds */
	time_t end_time, max_end_time;

	memset(&match_o, 0, sizeof(match_o));
	memset(&match_r, 0, sizeof(match_r));
	memset(&actions, 0, sizeof(actions));

	rc = rte_flow_dynf_metadata_register();
	if (unlikely(rc)) {
		DOCA_LOG_ERR("Enable metadata failed");
		return DOCA_ERROR_BAD_STATE;
	}

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
					DOCA_LOG_INFO("Already have one alive session, cannot handle more, skip");
					continue;
				}
				total_valid_packets++;

				actions.data.meta.mark = PKT_MARK;
				actions.resource_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
				actions.data.action_idx = 0;
				/* Updated timeout */
				end_time = MIN(time(NULL) + 2, max_end_time);
				result = flow_ct_create_entry(port,
							      ct_queue,
							      NULL,
							      prepare_flags,
							      entry_flags,
							      &match_o,
							      &match_r,
							      0,
							      0,
							      &actions,
							      NULL,
							      0,
							      0,
							      0,
							      ct_status,
							      entries);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to create CT entry\n");
					return result;
				}
				sessions++;
				DOCA_LOG_INFO("TCP session was created");
				DOCA_LOG_INFO(
					"Entry %d matches on the 5-tuple of the incoming packet. Reply direction matches on the inversed origin direction",
					i);
			} else if (tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_FIN ||
				   tcp_state & DOCA_FLOW_MATCH_TCP_FLAG_RST) {
				if (sessions == 0) {
					DOCA_LOG_INFO("No alive session to destroy, skip destroy");
					continue;
				}
				total_valid_packets++;
				/* Updated timeout */
				end_time = MIN(time(NULL) + 2, max_end_time);
				result = doca_flow_ct_rm_entry(ct_queue, NULL, entry_flags, *entries);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to remove CT pipe entry: %s",
						     doca_error_get_descr(result));
					return result;
				}
				/*process entries*/
				result = flow_ct_queue_reserve(port, ct_queue, ct_status, 0);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
					return result;
				}
				sessions--;
				DOCA_LOG_INFO("TCP session was ended");
			} else {
				if (sessions == 0) {
					DOCA_LOG_WARN("No alive session, can't process data packets");
					continue;
				}
				/* extract mark from packet */
				DOCA_LOG_INFO("Mark value: 0x%x",
					      DOCA_FLOW_CT_MARK_FROM_DPDK(packets[i]->hash.fdir.hi));
			}
			rte_flow_dynf_metadata_set(packets[i], 1);
			packets[i]->ol_flags |= RTE_MBUF_DYNFLAG_TX_METADATA;
			rte_eth_tx_burst(0, 0, &packets[i], 1);
		}
	} while (time(NULL) < end_time);

	if (total_valid_packets == 0) {
		if (sessions == 0) {
			DOCA_LOG_ERR("Sample didn't receive SYN packets within 5 seconds timeout");
		} else {
			DOCA_LOG_ERR("Sample didn't receive 'FIN'/'RST' packets within 5 seconds timeout");
		}
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_ct_tcp_wire_to_wire sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ct_tcp_wire_to_wire(uint16_t nb_queues, struct flow_switch_ctx *ctx)
{
	const int nb_ports = ctx->devs_ctx.devs_manager[0].nb_reps > 0 ? 2 : 1;
	const int nb_entries = 5;
	struct flow_resources resource;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_pipe_entry *ct_entry;
	struct doca_flow_pipe *egress_pipe, *rss_pipe, *tcp_pipe;
	struct doca_flow_pipe *ct_pipe = NULL;
	struct doca_flow_port *ports[nb_ports];
	struct doca_flow_meta o_zone_mask, r_zone_mask;
	struct doca_flow_ct_meta mark_mask = {.mark = MARK_MASK};
	uint32_t actions_mem_size[nb_ports];
	struct entries_status ctrl_status, ct_status;
	uint32_t ct_flags, nb_arm_queues = 1, nb_ctrl_queues = 1, nb_user_actions = 2048, nb_ipv4_sessions = 1024,
			   nb_ipv6_sessions = 0; /* On BF2 should always be 0 */
	uint16_t ct_queue = nb_queues;
	doca_error_t result;

	memset(&ctrl_status, 0, sizeof(ctrl_status));
	memset(&ct_status, 0, sizeof(ct_status));
	memset(&resource, 0, sizeof(resource));

	resource.nr_counters = 1;

	result = init_doca_flow(nb_queues, "switch,hws,isolated,expert", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	/* Dont use zone masking */
	memset(&o_zone_mask, 0, sizeof(o_zone_mask));
	memset(&r_zone_mask, 0, sizeof(r_zone_mask));

	ct_flags = DOCA_FLOW_CT_FLAG_NO_AGING | DOCA_FLOW_CT_FLAG_WIRE_TO_WIRE;
	result = init_doca_flow_ct(ct_flags,
				   nb_arm_queues,
				   nb_ctrl_queues,
				   nb_user_actions,
				   NULL,
				   nb_ipv4_sessions,
				   nb_ipv6_sessions,
				   0,
				   0,
				   false,
				   &o_zone_mask,
				   &mark_mask,
				   false,
				   &r_zone_mask,
				   &mark_mask);
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

	result = create_egress_pipe(ports[0], nb_ports > 1 ? 1 : 0, &ctrl_status, &egress_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = create_ct_pipe(ports[0], rss_pipe, rss_pipe, &ct_pipe);
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
	result = process_packets(ports[0], ct_queue, &ct_status, &ct_entry);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	DOCA_LOG_INFO("Waiting for 'FIN'/'RST' packets to arrive before ending the session");
	result = process_packets(ports[0], ct_queue, &ct_status, &ct_entry);
	if (result != DOCA_SUCCESS)
		goto cleanup;

cleanup:
	cleanup_procedure(ct_pipe, nb_ports, ports);
	return result;
}
