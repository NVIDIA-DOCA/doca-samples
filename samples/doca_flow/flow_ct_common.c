/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

doca_error_t init_doca_flow_switch_ports_with_ct(struct flow_devs_manager devs_manager[],
						 int nb_managers,
						 struct doca_flow_port *ports[],
						 int nb_ports,
						 uint32_t actions_mem_size[],
						 struct flow_resources *resources,
						 const struct ct_port_cfg *ct_cfg)
{
	return init_doca_flow_switch_ports_internal(devs_manager,
						    nb_managers,
						    ports,
						    nb_ports,
						    actions_mem_size,
						    resources,
						    ct_cfg);
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
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  1,
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
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  1,
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
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  1,
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
							  *pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  1,
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
						  *pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  2,
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
				  const struct doca_flow_fwd *fwd_origin,
				  const struct doca_flow_fwd *fwd_reply,
				  uint32_t timeout_s,
				  struct entries_status *ct_status,
				  struct doca_flow_pipe_entry **entry)
{
	doca_error_t result;
	bool conn_found = false;

	if (port == NULL || pipe == NULL) {
		DOCA_LOG_ERR("Invalid input parameters");
		return DOCA_ERROR_INVALID_VALUE;
	}

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
					fwd_origin,
					fwd_reply,
					timeout_s,
					ct_status,
					*entry);
	if (result != DOCA_SUCCESS) {
		doca_error_t rollback_result;

		DOCA_LOG_ERR("Failed to add CT pipe an entry: %s", doca_error_get_descr(result));
		if (*entry != NULL && (prepare_flags & DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS) != 0) {
			rollback_result = doca_flow_ct_entry_prepare_rollback(ct_queue, pipe, *entry);
			if (rollback_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to rollback prepared CT entry: %s",
					     doca_error_get_descr(rollback_result));
			}
			*entry = NULL;
		}
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
