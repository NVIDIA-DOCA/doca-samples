/*
 * Copyright (c) 2025-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "doca_types.h"
#include <doca_log.h>
#include <doca_bitfield.h>

#include "doca_flow.h"
#include "doca_flow_net.h"

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_IP_IN_IP);

/* The number of seconds app waits for traffic to come */
#define WAITING_TIME 10

#define NB_ENCAP_ACTIONS (2)
#define NB_DECAP_ACTIONS (1)

#define NB_PORT_FWD_PIPE_ENTRIES (1)
#define NB_ENCAP_PIPE_ENTRIES (2)
#define NB_DECAP_PIPE_ENTRIES (2)
#define NB_INGRESS_PIPE_ENTRIES (NB_PORT_FWD_PIPE_ENTRIES + NB_DECAP_PIPE_ENTRIES)
#define NB_EGRESS_PIPE_ENTRIES (NB_ENCAP_PIPE_ENTRIES)
#define TOTAL_ENTRIES (NB_INGRESS_PIPE_ENTRIES + NB_EGRESS_PIPE_ENTRIES)

#define NEXT_HEADER_IPV4 (4)
#define NEXT_HEADER_IPV6 (41)

/*
 * Create DOCA Flow ingress pipe with transport layer match and set pkt meta value.
 *
 * @port [in]: port of the pipe.
 * @dest_pipe [in]: dest pipe.
 * @dest_port_id [in]: port ID of the pipe.
 * @is_root [in]: whether this is the root pipe for the port
 * @status [in]: user context for adding entries.
 * @nb_entries [out]: pointer to put into number of entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_port_fwd_pipe(struct doca_flow_port *port,
					 struct doca_flow_pipe *dest_pipe,
					 int dest_port_id,
					 bool is_root,
					 struct entries_status *status,
					 struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_entry *entry;
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	enum doca_flow_flags_type flags;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "PORT_FWD_PIPE", DOCA_FLOW_PIPE_BASIC, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Forwarding traffic to other port */
	if (dest_pipe) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = dest_pipe;
	} else {
		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = dest_port_id;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create port forwarding pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	flags = DOCA_FLOW_NO_WAIT;
	result = doca_flow_pipe_add_entry(0, *pipe, &match, 0, &actions, NULL, NULL, flags, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add port forwarding entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries with example encap values.
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_encap_pipe_entries(struct doca_flow_pipe *pipe,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry *encap_entries[])
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	doca_error_t result;

	/* The mac address and IPv6 address digits are reversed for the inner-type=IPv6 case */
	uint8_t smac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dmac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	doca_be32_t ipv6_src[] = {htobe32(0x11115555), htobe32(0x22226666), htobe32(0x33337777), htobe32(0x44448888)};
	doca_be32_t ipv6_dst[] = {htobe32(0xaaaaeeee), htobe32(0xbbbbffff), htobe32(0xcccc0000), htobe32(0xdddd9999)};

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, smac[0], smac[1], smac[2], smac[3], smac[4], smac[5]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.src_ip, ipv6_src[0], ipv6_src[1], ipv6_src[2], ipv6_src[3]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.dst_ip, ipv6_dst[0], ipv6_dst[1], ipv6_dst[2], ipv6_dst[3]);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &encap_entries[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry matching on ipv4: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, smac[5], smac[4], smac[3], smac[2], smac[1], smac[0]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, dmac[5], dmac[4], dmac[3], dmac[2], dmac[1], dmac[0]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.src_ip, ipv6_src[3], ipv6_src[2], ipv6_src[1], ipv6_src[0]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.dst_ip, ipv6_dst[3], ipv6_dst[2], ipv6_dst[1], ipv6_dst[0]);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 1, &actions, NULL, NULL, 0, status, &encap_entries[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry matching on ipv6: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe on EGRESS domain with match on the packet meta and encap action with changeable values.
 *
 * @port [in]: port of the pipe.
 * @port_id [in]: port_id for forwarding
 * @status [in]: user context for adding entries.
 * @nb_entries [out]: pointer to put into number of entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_encap_pipe(struct doca_flow_port *port, uint32_t port_id, struct doca_flow_pipe **encap_pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor mon;
	struct doca_flow_actions actions_ipv4, actions_ipv6, *actions_arr[NB_ENCAP_ACTIONS];
	struct doca_flow_fwd fwd, fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&mon, 0, sizeof(mon));
	memset(&actions_ipv4, 0, sizeof(actions_ipv4));
	memset(&actions_ipv6, 0, sizeof(actions_ipv6));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* Match on inner L3 type */
	match.parser_meta.outer_l3_type = UINT32_MAX;
	match_mask.parser_meta.outer_l3_type = UINT32_MAX;

	mon.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	/* Encap with IPv6 tunnel - most fields are changeable */
	actions_ipv4.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions_ipv4.encap_cfg.is_l2 = false;
	SET_MAC_ADDR(actions_ipv4.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions_ipv4.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions_ipv4.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	SET_IPV6_ADDR(actions_ipv4.encap_cfg.encap.outer.ip6.src_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
	SET_IPV6_ADDR(actions_ipv4.encap_cfg.encap.outer.ip6.dst_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
	actions_ipv4.encap_cfg.encap.outer.ip6.hop_limit = 64;
	actions_ipv4.encap_cfg.encap.outer.ip6.traffic_class = 0xdb;
	actions_ipv4.encap_cfg.encap.outer.ip6.next_proto = NEXT_HEADER_IPV4;
	actions_ipv4.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_IP_IN_IP;

	actions_ipv6 = actions_ipv4;
	actions_ipv6.encap_cfg.encap.outer.ip6.next_proto = NEXT_HEADER_IPV6;

	actions_arr[0] = &actions_ipv4;
	actions_arr[1] = &actions_ipv6;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "ENCAP_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_ENCAP_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &mon);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ENCAP_ACTIONS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Forwarding traffic to the wire */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, encap_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IP-in-IP encap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries with example decap values.
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_decap_pipe_entries(struct doca_flow_pipe *pipe,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry *decap_entries[])
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	doca_error_t result;

	uint8_t smac[] = {0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc}; /* digits reversed for inner-type IPv6 */
	uint8_t dmac[] = {0xdd, 0xee, 0xff, 0x11, 0x22, 0x33}; /* digits reversed for inner-type IPv6 */

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.outer.ip6.next_proto = NEXT_HEADER_IPV4;
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac, smac[0], smac[1], smac[2], smac[3], smac[4], smac[5]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &decap_entries[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry matching on ipv4: %s", doca_error_get_descr(result));
		return result;
	}

	match.outer.ip6.next_proto = NEXT_HEADER_IPV6;
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac, smac[5], smac[4], smac[3], smac[2], smac[1], smac[0]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac, dmac[5], dmac[4], dmac[3], dmac[2], dmac[1], dmac[0]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV6);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &decap_entries[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry matching on ipv6: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe with match on the next_proto and decap action with changeable values.
 *
 * @port [in]: port of the pipe.
 * @port_id [in]: port_id for forwarding
 * @status [in]: user context for adding entries.
 * @nb_entries [out]: pointer to put into number of entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_decap_pipe(struct doca_flow_port *port,
				      struct doca_flow_pipe *next_pipe,
				      struct doca_flow_pipe **decap_pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor mon;
	struct doca_flow_actions actions, *actions_arr[NB_DECAP_ACTIONS];
	struct doca_flow_fwd fwd, fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mon, 0, sizeof(mon));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* Match on outer L3 type and next_proto */
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match.outer.ip6.next_proto = UINT8_MAX;

	mon.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	/* Encap with IPv6 tunnel - most fields are changeable */
	actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions.decap_cfg.eth.type = UINT16_MAX;

	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = set_flow_pipe_cfg(pipe_cfg, "DECAP_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	/* keep the default domain */
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_ENCAP_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &mon);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_DECAP_ACTIONS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, decap_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IP-in-IP decap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Prepare egress domain pipeline.
 *
 * @ingress_port [in]: pointer to the ingress port.
 * @egress_port [in]: pointer to the pair/egress port.
 * @egress_port_id [in]: the ID of pair port.
 * @status [in]: updated on entry creation/destruction
 * @encap_entries [out]: entries created
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t prepare_encap_pipeline(struct doca_flow_port *ingress_port,
					   struct doca_flow_port *egress_port,
					   uint32_t egress_port_id,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry *encap_entries[])
{
	struct doca_flow_pipe *port_fwd_pipe, *encap_pipe;
	doca_error_t result;

	status->nb_processed = 0;

	result = create_encap_pipe(egress_port, egress_port_id, &encap_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encap pipe with entries: %s", doca_error_get_descr(result));
		return result;
	}
	result = add_encap_pipe_entries(encap_pipe, status, encap_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to encap pipe: %s", doca_error_get_descr(result));
		return result;
	}
	result = flow_process_entries(egress_port, status, NB_EGRESS_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process encap entries: %s", doca_error_get_descr(result));
		return result;
	}

	status->nb_processed = 0;

	result = create_port_fwd_pipe(ingress_port, encap_pipe, egress_port_id, true, status, &port_fwd_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create port fwd pipe with entries: %s", doca_error_get_descr(result));
		return result;
	}
	result = flow_process_entries(ingress_port, status, NB_PORT_FWD_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process encap entries: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Prepare ingress domain pipeline.
 *
 * @ingress_port [in]: pointer to port.
 * @egress_port_id [in]: pair port ID.
 * @status [in]: updated on entry creation/destruction
 * @decap_entries [out]: entries created
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t prepare_decap_pipeline(struct doca_flow_port *ingress_port,
					   int egress_port_id,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry *decap_entries[])
{
	struct doca_flow_pipe *port_fwd_pipe, *decap_pipe;
	doca_error_t result;

	status->nb_processed = 0;

	result = create_port_fwd_pipe(ingress_port, NULL, egress_port_id, false, status, &port_fwd_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create port forwarding pipe with entries: %s", doca_error_get_descr(result));
		return result;
	}

	result = create_decap_pipe(ingress_port, port_fwd_pipe, &decap_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decap pipe with entries: %s", doca_error_get_descr(result));
		return result;
	}
	result = add_decap_pipe_entries(decap_pipe, status, decap_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to decap pipe: %s", doca_error_get_descr(result));
		return result;
	}
	result = flow_process_entries(ingress_port, status, NB_INGRESS_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process decap entries: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_ip_in_ip sample
 *
 * @nb_queues [in]: number of queues the sample will use.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */

/* Context structure for statistics printing */
struct ip_in_ip_stats_context {
	struct doca_flow_pipe_entry **encap_entries;
	struct doca_flow_pipe_entry **decap_entries;
};

/*
 * Print IP in IP statistics
 *
 * @encap_entries [in]: array of encap entries
 * @decap_entries [in]: array of decap entries
 */
static void print_ip_in_ip_stats(struct doca_flow_pipe_entry *encap_entries[],
				 struct doca_flow_pipe_entry *decap_entries[])
{
	doca_error_t result;
	struct doca_flow_resource_query encap_ipv4_query_stats, decap_ipv4_query_stats, encap_ipv6_query_stats,
		decap_ipv6_query_stats;

	result = doca_flow_resource_query_entry(encap_entries[0], &encap_ipv4_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query entry counter: %s", doca_error_get_descr(result));
	}
	result = doca_flow_resource_query_entry(decap_entries[0], &decap_ipv4_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query entry counter: %s", doca_error_get_descr(result));
	}
	result = doca_flow_resource_query_entry(encap_entries[1], &encap_ipv6_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query entry counter: %s", doca_error_get_descr(result));
	}
	result = doca_flow_resource_query_entry(decap_entries[1], &decap_ipv6_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query entry counter: %s", doca_error_get_descr(result));
	}
	DOCA_LOG_INFO("Encap: counted %ld IPv4 packets, %ld IPv6 packets",
		      encap_ipv4_query_stats.counter.total_pkts,
		      encap_ipv6_query_stats.counter.total_pkts);
	DOCA_LOG_INFO("Decap: counted %ld IPv4 packets, %ld IPv6 packets",
		      decap_ipv4_query_stats.counter.total_pkts,
		      decap_ipv6_query_stats.counter.total_pkts);
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: ip_in_ip_stats_context structure
 */
static void print_ip_in_ip_stats_wrapper(void *context)
{
	struct ip_in_ip_stats_context *ctx = (struct ip_in_ip_stats_context *)context;
	print_ip_in_ip_stats(ctx->encap_entries, ctx->decap_entries);
}

doca_error_t flow_ip_in_ip(int nb_queues)
{
	const int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	doca_error_t result;
	uint32_t raw_port_id = 0;		   /* sends and receives "raw" (not encapped) packets */
	uint32_t tunnel_port_id = raw_port_id ^ 1; /* sends and receives "tunnel" packets (IP-in-IP encap) */
	struct doca_flow_pipe_entry *encap_entries[NB_ENCAP_PIPE_ENTRIES];
	struct doca_flow_pipe_entry *decap_entries[NB_DECAP_PIPE_ENTRIES];
	struct entries_status entries_status = {0};

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = nb_ports * NB_PORT_FWD_PIPE_ENTRIES + NB_DECAP_PIPE_ENTRIES + NB_ENCAP_PIPE_ENTRIES;
	resource.nr_encap = NB_ENCAP_PIPE_ENTRIES;
	resource.nr_decap = NB_DECAP_PIPE_ENTRIES;
	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(TOTAL_ENTRIES));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	result = prepare_encap_pipeline(ports[raw_port_id],
					ports[tunnel_port_id],
					tunnel_port_id,
					&entries_status,
					encap_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare egress pipeline: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = prepare_decap_pipeline(ports[tunnel_port_id], /* packet arrives here, is decapped */
					raw_port_id,	       /* packet is forwarded here */
					&entries_status,
					decap_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare ingress pipeline: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* Setup statistics context and wait for packets */
	struct ip_in_ip_stats_context stats_ctx = {.encap_entries = encap_entries, .decap_entries = decap_entries};

	flow_wait_for_packets(WAITING_TIME, print_ip_in_ip_stats_wrapper, &stats_ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
