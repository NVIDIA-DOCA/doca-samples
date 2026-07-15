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

#include "doca_flow_ct.h"
#include <doca_log.h>
#include <doca_bitfield.h>
#include <doca_dpdk.h>

#include <dpdk_utils.h>

#include "flow_common.h"
#include "flow_ct_common.h"

DOCA_LOG_REGISTER(FLOW_CT_COMMON);

#define FNV1A_32_OFFSET (uint32_t)2166136261
#define FNV1A_32_PRIME (uint32_t)16777619
/*
 * FNV1A 32 bit hash calculation function
 *
 * @buf [in]: Input buffer to calculates hash on it's byte
 * @len [in]: Bytes size of the input buffer
 * @hash [in]: FNV1A_32_OFFSET or previous hash calculation
 * @return: FNV1A hash calculation of the buffer
 */
static uint32_t fnv1a_32bit_hash(const void *buf, size_t len, uint32_t hash)
{
	const uint8_t *bytes = (const uint8_t *)buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= (uint32_t)bytes[i];
		hash *= FNV1A_32_PRIME;
	}

	return hash;
}

doca_error_t flow_ct_register_params(void)
{
	return register_doca_flow_switch_params();
}

doca_error_t init_doca_flow_ct(uint32_t flags,
			       uint32_t nb_arm_queues,
			       uint32_t nb_ctrl_queues,
			       uint32_t nb_user_actions,
			       doca_flow_ct_entry_finalize_cb entry_finalize_cb,
			       uint32_t nb_ipv4_sessions,
			       uint32_t nb_ipv6_sessions,
			       uint32_t nb_asym_counter,
			       uint32_t dup_filter_sz,
			       bool o_match_inner,
			       struct doca_flow_meta *o_zone_mask,
			       struct doca_flow_ct_meta *o_modify_mask,
			       bool r_match_inner,
			       struct doca_flow_meta *r_zone_mask,
			       struct doca_flow_ct_meta *r_modify_mask)
{
	struct doca_flow_ct_cfg *ct_cfg;
	doca_error_t result;

	if (o_zone_mask == NULL || o_modify_mask == NULL) {
		DOCA_LOG_ERR("Origin masks can't be null");
		return DOCA_ERROR_INVALID_VALUE;
	} else if (r_zone_mask == NULL || r_modify_mask == NULL) {
		DOCA_LOG_ERR("Reply masks can't be null");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_flow_ct_cfg_create(&ct_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA Flow CT config: %s", doca_error_get_name(result));
		return result;
	}

	doca_flow_ct_cfg_set_flags(ct_cfg, flags);
	doca_flow_ct_cfg_set_queues(ct_cfg, nb_arm_queues);
	doca_flow_ct_cfg_set_ctrl_queues(ct_cfg, nb_ctrl_queues);
	doca_flow_ct_cfg_set_user_actions(ct_cfg, nb_user_actions);
	doca_flow_ct_cfg_set_aging_core(ct_cfg, nb_arm_queues + 1);
	doca_flow_ct_cfg_set_entry_finalize_cb(ct_cfg, entry_finalize_cb);
	doca_flow_ct_cfg_set_connections(ct_cfg, nb_ipv4_sessions, nb_ipv6_sessions, 0);
	doca_flow_ct_cfg_set_counter_asymmetric(ct_cfg, nb_asym_counter);
	doca_flow_ct_cfg_set_dup_filter_size(ct_cfg, dup_filter_sz);
	doca_flow_ct_cfg_set_direction(ct_cfg, false, o_match_inner, o_zone_mask, o_modify_mask);
	doca_flow_ct_cfg_set_direction(ct_cfg, true, r_match_inner, r_zone_mask, r_modify_mask);
	doca_flow_ct_cfg_set_max_connections_per_zone(ct_cfg, 8192 * 1024);

	result = doca_flow_ct_init(ct_cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to initialize DOCA Flow CT: %s", doca_error_get_name(result));

	doca_flow_ct_cfg_destroy(ct_cfg);

	return result;
}

doca_error_t flow_ct_capable(struct doca_devinfo *dev_info)
{
	return doca_flow_ct_cap_is_dev_supported(dev_info);
}

uint32_t flow_ct_hash_6tuple(const struct doca_flow_ct_match *match, doca_be32_t zone_field, bool is_ipv6)
{
	uint32_t hash = FNV1A_32_OFFSET;
	uint32_t zone;

	if (is_ipv6) {
		zone = DOCA_BE32_GET(match->ipv6.metadata, zone_field);
		hash = fnv1a_32bit_hash(match->ipv6.src_ip, 4, hash);
		hash = fnv1a_32bit_hash(match->ipv6.dst_ip, 4, hash);
		hash = fnv1a_32bit_hash(&match->ipv6.l4_port.dst_port, 1, hash);
		hash = fnv1a_32bit_hash(&match->ipv6.l4_port.src_port, 1, hash);
		hash = fnv1a_32bit_hash(&match->ipv6.next_proto, 1, hash);
		hash = fnv1a_32bit_hash(&zone, 1, hash);
	} else {
		zone = DOCA_BE32_GET(match->ipv4.metadata, zone_field);
		hash = fnv1a_32bit_hash(&match->ipv4.src_ip, 1, hash);
		hash = fnv1a_32bit_hash(&match->ipv4.dst_ip, 1, hash);
		hash = fnv1a_32bit_hash(&match->ipv4.l4_port.dst_port, 1, hash);
		hash = fnv1a_32bit_hash(&match->ipv4.l4_port.src_port, 1, hash);
		hash = fnv1a_32bit_hash(&match->ipv4.next_proto, 1, hash);
		hash = fnv1a_32bit_hash(&zone, 1, hash);
	}

	return hash;
}

void cleanup_procedure(struct doca_flow_pipe *ct_pipe, int nb_ports, struct doca_flow_port *ports[])
{
	doca_error_t result;

	if (ct_pipe != NULL)
		doca_flow_pipe_destroy(ct_pipe);

	result = stop_doca_flow_ports(nb_ports, ports);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop doca flow ports: %s", doca_error_get_descr(result));

	doca_flow_ct_destroy();
	doca_flow_destroy();
}

doca_error_t create_ct_root_pipe(struct doca_flow_port *port,
				 bool is_ipv4,
				 bool is_ipv6,
				 enum doca_flow_l4_meta l4_type,
				 struct doca_flow_pipe *fwd_pipe,
				 struct entries_status *status,
				 struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&fwd, 0, sizeof(fwd));

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "root", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_pipe;

	/* Match IPv4 and IPv6 TCP packets */
	if (is_ipv4) {
		memset(&match, 0, sizeof(match));
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		match.parser_meta.outer_l4_type = l4_type;
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.outer.ip4.dst_ip = BE_IPV4_ADDR(1, 1, 1, 1);
		result = doca_flow_pipe_control_add_entry(0,
							  1,
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &fwd,
							  status,
							  NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add root pipe IPv4 entry: %s", doca_error_get_descr(result));
			return result;
		}

		match.outer.ip4.dst_ip = 0;
		match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 1, 1, 1);
		result = doca_flow_pipe_control_add_entry(0,
							  1,
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &fwd,
							  status,
							  NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add root pipe IPv4 entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (is_ipv6) {
		memset(&match, 0, sizeof(match));
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		match.parser_meta.outer_l4_type = l4_type;
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		match.outer.ip6.dst_ip[0] = 0x01010101;
		match.outer.ip6.dst_ip[1] = 0x01010101;
		match.outer.ip6.dst_ip[2] = 0x01010101;
		match.outer.ip6.dst_ip[3] = 0x01010101;
		result = doca_flow_pipe_control_add_entry(0,
							  1,
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &fwd,
							  status,
							  NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add IPv6 root pipe entry: %s", doca_error_get_descr(result));
			return result;
		}

		memset(match.outer.ip6.dst_ip, 0, sizeof(match.outer.ip6.dst_ip));
		match.outer.ip6.src_ip[0] = 0x01010101;
		match.outer.ip6.src_ip[1] = 0x01010101;
		match.outer.ip6.src_ip[2] = 0x01010101;
		match.outer.ip6.src_ip[3] = 0x01010101;
		result = doca_flow_pipe_control_add_entry(0,
							  1,
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &fwd,
							  status,
							  NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add IPv6 root pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	/* Drop non TCP packets */
	fwd.type = DOCA_FLOW_FWD_DROP;
	memset(&match, 0, sizeof(match));
	result = doca_flow_pipe_control_add_entry(0,
						  2,
						  *pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &fwd,
						  status,
						  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add root pipe drop entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process root entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

doca_error_t flow_ct_create_entry(struct doca_flow_port *port,
				  uint16_t ct_queue,
				  struct doca_flow_pipe *pipe,
				  uint32_t prepare_flags,
				  uint32_t entry_flags,
				  struct doca_flow_ct_match *match_origin,
				  struct doca_flow_ct_match *match_reply,
				  uint32_t hash_origin,
				  uint32_t hash_reply,
				  const struct doca_flow_ct_actions *actions_origin,
				  const struct doca_flow_ct_actions *actions_reply,
				  uint32_t fwd_handle_origin,
				  uint32_t fwd_handle_reply,
				  uint32_t timeout_s,
				  struct entries_status *ct_status,
				  struct doca_flow_pipe_entry **entry)
{
	doca_error_t result;
	bool conn_found = false;

	/* Allocate CT entry */
	result = doca_flow_ct_entry_prepare(ct_queue,
					    pipe,
					    prepare_flags,
					    match_origin,
					    hash_origin,
					    match_reply,
					    hash_reply,
					    entry,
					    &conn_found);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare CT entry\n");
		return result;
	}

	if (conn_found)
		return DOCA_SUCCESS;

	/* Add CT entry */
	result = doca_flow_ct_add_entry(ct_queue,
					pipe,
					entry_flags,
					match_origin,
					match_reply,
					actions_origin,
					actions_reply,
					fwd_handle_origin,
					fwd_handle_reply,
					timeout_s,
					ct_status,
					*entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add CT pipe an entry: %s", doca_error_get_descr(result));
		return result;
	}

	if ((entry_flags & DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT) != 0) {
		/*process entries*/
		result = flow_ct_queue_reserve(port, ct_queue, ct_status, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t flow_ct_queue_reserve(struct doca_flow_port *port,
				   uint16_t ct_queue,
				   struct entries_status *status,
				   uint32_t room)
{
	doca_error_t result;

	if (room == 0)
		room = CT_DEFAULT_QUEUE_DEPTH;
	else if (room > CT_DEFAULT_QUEUE_DEPTH) {
		DOCA_LOG_ERR("Expecting room is bigger than default queue depth");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_flow_ct_entries_process(port, ct_queue, room, room, NULL);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
	if (status->failure) {
		DOCA_LOG_ERR("Failed to process entries, status is not success");
		return DOCA_ERROR_BAD_STATE;
	}
	return DOCA_SUCCESS;
}
