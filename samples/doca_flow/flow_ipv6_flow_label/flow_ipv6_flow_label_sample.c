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

#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "doca_types.h"
#include <doca_log.h>
#include <doca_bitfield.h>

#include "doca_flow.h"
#include "doca_flow_net.h"

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_IPV6_FLOW_LABEL);

/* The number of seconds app waits for traffic to come */
#define WAITING_TIME 10
#define NB_INGRESS_PIPE_ENTRIES (4)
#define NB_MODIFY_PIPE_ENTRIES (2)
#define NB_ENCAP_PIPE_ENTRIES (2)
#define NB_EGRESS_PIPE_ENTRIES (NB_MODIFY_PIPE_ENTRIES + NB_ENCAP_PIPE_ENTRIES)
#define TOTAL_ENTRIES (NB_INGRESS_PIPE_ENTRIES + NB_EGRESS_PIPE_ENTRIES)

struct doca_flow_pipe *egress_pipe[2];

/*
 * Create DOCA Flow ingress pipe with transport layer match and set pkt meta value.
 *
 * @port [in]: port of the pipe.
 * @port_id [in]: port ID of the pipe.
 * @status [in]: user context for adding entries.
 * @nb_entries [out]: pointer to put into number of entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_pipe(struct doca_flow_port *port,
					int port_id,
					struct entries_status *status,
					uint32_t *nb_entries)
{
	struct doca_flow_pipe *pipe;
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

	/* Transport layer match */
	match.parser_meta.outer_l4_type = 0xffffffff;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TRANSPORT;
	match.outer.transport.src_port = 0xffff;
	match.outer.transport.dst_port = DOCA_HTOBE16(80);

	/* Set meta data to match on the egress domain */
	actions.meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MATCH_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 4);
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
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_pipe[port_id ^ 1];

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress selector pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	match.outer.transport.src_port = DOCA_HTOBE16(1234);
	actions.meta.pkt_meta = DOCA_HTOBE32(1);
	flags = DOCA_FLOW_WAIT_FOR_BATCH;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, flags, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP entry with source port 1234: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, flags, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP entry with source port 1234: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	match.outer.transport.src_port = DOCA_HTOBE16(5678);
	actions.meta.pkt_meta = DOCA_HTOBE32(2);
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, flags, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP entry with source port 5678: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	flags = DOCA_FLOW_NO_WAIT;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, flags, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP entry with source port 5678: %s", doca_error_get_descr(result));
		return result;
	}

	*nb_entries = 4;
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
static doca_error_t add_encap_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t mac2[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	doca_be32_t ipv6_1[] = {htobe32(0x11115555), htobe32(0x22226666), htobe32(0x33337777), htobe32(0x44448888)};
	doca_be32_t ipv6_2[] = {htobe32(0xaaaaeeee), htobe32(0xbbbbffff), htobe32(0xcccc0000), htobe32(0xdddd9999)};

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.meta.pkt_meta = DOCA_HTOBE32(1);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, mac1[0], mac1[1], mac1[2], mac1[3], mac1[4], mac1[5]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, mac2[0], mac2[1], mac2[2], mac2[3], mac2[4], mac2[5]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.src_ip, ipv6_1[0], ipv6_1[1], ipv6_1[2], ipv6_1[3]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.dst_ip, ipv6_2[0], ipv6_2[1], ipv6_2[2], ipv6_2[3]);

	result = doca_flow_mpls_label_encode(0x12345, 0, 0, 0, &actions.encap_cfg.encap.tun.mpls[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to encode MPLS first label for first entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_mpls_label_encode(0x6789a, 0, 0, 0, &actions.encap_cfg.encap.tun.mpls[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to encode MPLS second label for first entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_mpls_label_encode(0xbcdef, 0, 0, 1, &actions.encap_cfg.encap.tun.mpls[2]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to encode MPLS third label for first entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry matching on pkt meta 1: %s", doca_error_get_descr(result));
		return result;
	}

	match.meta.pkt_meta = DOCA_HTOBE32(2);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, mac1[5], mac1[4], mac1[3], mac1[2], mac1[1], mac1[0]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, mac2[5], mac2[4], mac2[3], mac2[2], mac2[1], mac2[0]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.src_ip, ipv6_2[0], ipv6_2[1], ipv6_2[2], ipv6_2[3]);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.dst_ip, ipv6_1[0], ipv6_1[1], ipv6_1[2], ipv6_1[3]);

	result = doca_flow_mpls_label_encode(0x44444, 0, 0, 0, &actions.encap_cfg.encap.tun.mpls[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to encode MPLS first label for second entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_mpls_label_encode(0x77777, 0, 0, 0, &actions.encap_cfg.encap.tun.mpls[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to encode MPLS second label for second entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_mpls_label_encode(0xccccc, 0, 0, 1, &actions.encap_cfg.encap.tun.mpls[2]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to encode MPLS third label for second entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry matching on pkt meta 2: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe on EGRESS domain with match on the packet meta and encap action with changeable values.
 *
 * @port [in]: port of the pipe.
 * @port_id [in]: pipe port ID
 * @next_pipe [in]: pointer to modify pipe for forwarding to.
 * @status [in]: user context for adding entries.
 * @nb_entries [out]: pointer to put into number of entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_encap_pipe(struct doca_flow_port *port,
				      int port_id,
				      struct doca_flow_pipe *next_pipe,
				      struct entries_status *status,
				      uint32_t *nb_entries)
{
	struct doca_flow_pipe *pipe;
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* Match on pkt meta */
	match_mask.meta.pkt_meta = UINT32_MAX;

	/* Encap with IPv6 and MPLS tunnel - most fields are changeable */
	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.encap_cfg.is_l2 = false;
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.src_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
	SET_IPV6_ADDR(actions.encap_cfg.encap.outer.ip6.dst_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
	actions.encap_cfg.encap.outer.ip6.hop_limit = 64;
	actions.encap_cfg.encap.outer.ip6.traffic_class = 0xdb;
	actions.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_MPLS_DEFAULT_PORT);
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_MPLS_O_UDP;
	actions.encap_cfg.encap.tun.mpls[0].label = 0xffffffff;
	actions.encap_cfg.encap.tun.mpls[1].label = 0xffffffff;
	actions.encap_cfg.encap.tun.mpls[2].label = 0xffffffff;
	actions_arr[0] = &actions;

	/* MPLS is supported as L3 tunnel only */

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
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Forwarding traffic to update pipe */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create MPLS encap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = add_encap_pipe_entries(pipe, status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to encap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	egress_pipe[port_id] = pipe;

	*nb_entries = 2;
	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Fill the descriptor which describes copy to outer IPv6 flow label field.
 *
 * @src_str [in]: the source field string.
 * @src_bit_offset [in]: the source field bit offset.
 * @desc [out]: the descriptor structure to fill.
 * @descs [out]: the descriptors array structure contains the above desc.
 */
static void fill_copy_to_outer_ipv6_fl_desc(const char *src_str,
					    uint32_t src_bit_offset,
					    struct doca_flow_action_desc *desc,
					    struct doca_flow_action_descs *descs)
{
	descs->desc_array = desc;
	descs->nb_action_desc = 1;

	desc->type = DOCA_FLOW_ACTION_COPY;
	desc->field_op.src.field_string = src_str;
	desc->field_op.src.bit_offset = src_bit_offset;
	desc->field_op.dst.field_string = "outer.ipv6.flow_label";
	desc->field_op.dst.bit_offset = 0;
	desc->field_op.width = 20;
}

/*
 * Create DOCA Flow pipe on EGRESS domain with match on the inner L3 type and update IPv6 flow label accordingly.
 *
 * @port [in]: port of the pipe
 * @port_id [in]: pipe port ID
 * @pipe [out]: created pipe pointer
 * @nb_entries [out]: pointer to put into number of entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_modify_pipe(struct doca_flow_port *port,
				       int port_id,
				       struct doca_flow_pipe **pipe,
				       uint32_t *nb_entries)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_action_desc ip4_desc;
	struct doca_flow_action_desc ip6_desc;
	struct doca_flow_action_descs ip4_descs;
	struct doca_flow_action_descs ip6_descs;
	struct doca_flow_action_descs *descs_arr[2];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&fwd, 0, sizeof(fwd));

	/* Match on inner L3 type */
	match.parser_meta.inner_l3_type = UINT32_MAX;
	match_mask.parser_meta.inner_l3_type = UINT32_MAX;

	/*
	 * Update outer IPv6 flow label according to original packet L3 type:
	 * IPv6 - copy flow label inner to outer.
	 * IPv4 - copy from metadata containing a new flow label calculated from this packet.
	 */
	fill_copy_to_outer_ipv6_fl_desc("meta.data", 0, &ip4_desc, &ip4_descs);
	fill_copy_to_outer_ipv6_fl_desc("inner.ipv6.flow_label", 0, &ip6_desc, &ip6_descs);
	descs_arr[0] = &ip4_descs;
	descs_arr[1] = &ip6_descs;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "UPDATE_IPV6_FL_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, NULL, NULL, descs_arr, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	*nb_entries = 2;

	/* Forwarding traffic to the wire */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries updating outer IPv6
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_modify_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV4;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add IPv4 entry to modify pipe: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV6;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 1, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add IPv6 entry to modify pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Prepare egress domain pipeline.
 *
 * @pair_port [in]: pointer to the pair port.
 * @pair_port_id [in]: the ID of pair port.
 * @status [in]: user context for adding entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t prepare_egress_pipeline(struct doca_flow_port *pair_port,
					    int pair_port_id,
					    struct entries_status *status)
{
	struct doca_flow_pipe *pipe;
	uint32_t total_entries = 0;
	uint32_t nb_pipe_entries;
	doca_error_t result;

	result = create_modify_pipe(pair_port, pair_port_id, &pipe, &nb_pipe_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create outer updating pipe: %s", doca_error_get_descr(result));
		return result;
	}
	total_entries += nb_pipe_entries;

	memset(status, 0, sizeof(*status));

	result = add_modify_pipe_entries(pipe, status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to outer updating pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = create_encap_pipe(pair_port, pair_port_id, pipe, status, &nb_pipe_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encap pipe with entries: %s", doca_error_get_descr(result));
		return result;
	}
	total_entries += nb_pipe_entries;

	result = flow_process_entries(pair_port, status, total_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process egress entries: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Prepare ingress domain pipeline.
 *
 * @port [in]: pointer to port.
 * @port_id [in]: port ID.
 * @status [in]: user context for adding entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t prepare_ingress_pipeline(struct doca_flow_port *port, int port_id, struct entries_status *status)
{
	uint32_t num_of_entries;
	doca_error_t result;

	memset(status, 0, sizeof(*status));

	result = create_ingress_pipe(port, port_id, status, &num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress pipe with entries: %s", doca_error_get_descr(result));
		return result;
	}

	result = flow_process_entries(port, status, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process ingress entries: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_ipv6_flow_label sample.
 *
 * @nb_queues [in]: number of queues the sample will use.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ipv6_flow_label(int nb_queues)
{
	const int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct entries_status egress_status;
	struct entries_status ingress_status;
	doca_error_t result;
	int port_id;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_encap = 2;
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

	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = prepare_egress_pipeline(ports[port_id ^ 1], port_id ^ 1, &egress_status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to prepare egress pipeline: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = prepare_ingress_pipeline(ports[port_id], port_id, &ingress_status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to prepare ingress pipeline: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	DOCA_LOG_INFO("Wait %u seconds for packets to arrive", WAITING_TIME);
	sleep(WAITING_TIME);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
