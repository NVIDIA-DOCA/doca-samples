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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>

#include "flow_common.h"
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_MULT_FLOODING_CASES);

enum action_idx {
	ACT_IDX_NONE,
	ACT_IDX_ENCAP,
	ACT_IDX_MODIFY,
	ACT_IDX_MAX,
};

enum ingress_entries {
	INGRESS_ENT_A,
	INGRESS_ENT_B,
	INGRESS_ENT_MAX,
};

enum hash_enteries {
	HASH_CLONE_0,
	HASH_CLONE_1,
	HASH_CLONE_2,
	HASH_CLONE_MAX,
};

enum flooding_case_type {
	FLOODING_CASE_TYPE_A,
	FLOODING_CASE_TYPE_B,
};

#define NB_INGRESS_ROOT INGRESS_ENT_MAX
#define NB_INGRESS_HASH HASH_CLONE_MAX
#define NB_INGRESS_FWD_PIPE 5
#define MAX_CLONES HASH_CLONE_MAX
#define NB_TOTAL (NB_INGRESS_ROOT + NB_INGRESS_HASH + NB_INGRESS_FWD_PIPE)

struct fwd_info {
	uint32_t src_idx;
	uint32_t clone_idx;
	enum action_idx act_idx;
	uint16_t fwd_dest_port;
};

struct flooding_case {
	enum flooding_case_type case_id;
	doca_be32_t classifier_addr;
	uint32_t clone_count;
	struct fwd_info fwd_clone_info[MAX_CLONES];
};

/*
 * This table describes how user can flood the traffic to different dest
 * by one flooding pipe with case_id and clone_idx.
 * User can put any action and fwd to the expected case_id and clone_idx,
 * the flooding next pipe will match with case_id and clone_idx then
 * perform the needed action and fwd.
 *
 */
struct flooding_case flooding_table[] = {
	{
		.case_id = FLOODING_CASE_TYPE_A,
		.classifier_addr = BE_IPV4_ADDR(1, 2, 3, 4),
		.clone_count = 3,
		.fwd_clone_info =
			{
				{.src_idx = FLOODING_CASE_TYPE_A,
				 .clone_idx = 0,
				 .act_idx = ACT_IDX_ENCAP,
				 .fwd_dest_port = 0},
				{.src_idx = FLOODING_CASE_TYPE_A,
				 .clone_idx = 1,
				 .act_idx = ACT_IDX_MODIFY,
				 .fwd_dest_port = 1},
				{.src_idx = FLOODING_CASE_TYPE_A,
				 .clone_idx = 2,
				 .act_idx = ACT_IDX_NONE,
				 .fwd_dest_port = 2},
			},
	},
	{
		.case_id = FLOODING_CASE_TYPE_B,
		.classifier_addr = BE_IPV4_ADDR(1, 2, 3, 5),
		.clone_count = 2,
		.fwd_clone_info =
			{
				{.src_idx = FLOODING_CASE_TYPE_B,
				 .clone_idx = 0,
				 .act_idx = ACT_IDX_NONE,
				 .fwd_dest_port = 0},
				{.src_idx = FLOODING_CASE_TYPE_B,
				 .clone_idx = 1,
				 .act_idx = ACT_IDX_NONE,
				 .fwd_dest_port = 2},
			},
	},
};

static struct doca_flow_pipe *ingress_root_pipe;
static struct doca_flow_pipe *ingress_fwd_pipe;
static struct doca_flow_pipe *hash_pipe;

static struct doca_flow_pipe_entry *ingress_root_entries[NB_INGRESS_ROOT];
static struct doca_flow_pipe_entry *ingress_hash_entries[NB_INGRESS_HASH];
static struct doca_flow_pipe_entry *ingress_fwd_entries[NB_INGRESS_FWD_PIPE];

/* Context structure for statistics printing */
struct hash_mult_flooding_stats_context {
	struct doca_flow_pipe_entry **ingress_fwd_entries;
};

/*
 * Print hash multi flooding statistics
 *
 * @ingress_fwd_entries [in]: array of ingress fwd entries
 */
static void print_hash_mult_flooding_stats(struct doca_flow_pipe_entry *ingress_fwd_entries[])
{
	doca_error_t result;
	struct doca_flow_resource_query query_stats;
	int entry_idx;

	/* dump entries counters */
	DOCA_LOG_INFO("FWD pipe statistics");
	for (entry_idx = 0; entry_idx < NB_INGRESS_FWD_PIPE; entry_idx++) {
		result = doca_flow_resource_query_entry(ingress_fwd_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Entry in index: %d, total packets: %ld, total bytes: %ld",
			      entry_idx,
			      query_stats.counter.total_pkts,
			      query_stats.counter.total_bytes);
	}
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: hash_mult_flooding_stats_context structure
 */
static void print_hash_mult_flooding_stats_wrapper(void *context)
{
	struct hash_mult_flooding_stats_context *ctx = (struct hash_mult_flooding_stats_context *)context;
	print_hash_mult_flooding_stats(ctx->ingress_fwd_entries);
}

/*
 * Create DOCA Flow forward pipe on the switch port.
 * The forward pipe forwards packet based on case index and flooding index.
 *
 * @port [in]: port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_fwd_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_actions actions_none = {0};
	struct doca_flow_actions actions_encap = {0};
	struct doca_flow_actions actions_modify = {0};
	struct doca_flow_actions *actions_arr[ACT_IDX_MAX];
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	match_mask.meta.u32[0] = UINT32_MAX;
	match_mask.meta.u32[1] = UINT32_MAX;

	/* build basic outer VXLAN encap data*/
	actions_encap.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions_encap.encap_cfg.is_l2 = true;
	SET_MAC_ADDR(actions_encap.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions_encap.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions_encap.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions_encap.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions_encap.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions_encap.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions_encap.encap_cfg.encap.outer.ip4.flags_fragment_offset = 0xffff;
	actions_encap.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions_encap.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
	actions_encap.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	actions_encap.encap_cfg.encap.tun.vxlan_tun_id = 0xffffffff;
	actions_encap.encap_cfg.encap.tun.vxlan_tun_rsvd1 = 0xff;
	actions_arr[ACT_IDX_ENCAP] = &actions_encap;

	actions_modify.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions_modify.outer.ip4.src_ip = 0xffffffff;
	actions_arr[ACT_IDX_MODIFY] = &actions_modify;

	actions_arr[ACT_IDX_NONE] = &actions_none;

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = UINT16_MAX;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "INGRESS_FWD_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_FWD_PIPE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, ACT_IDX_MAX);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow hash pipe on the switch port.
 * The flooding dest will be ingress forward pipe in order to demonstrate
 * flooding to ingress forward pipe and match based on different case index
 * and flooding index.
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_hash_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd, fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	doca_error_t result;

	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	/* set mask value */
	actions.meta.u32[0] = UINT32_MAX;
	actions_arr[0] = &actions;

	result = set_flow_pipe_cfg(pipe_cfg, "HASH_PIPE", DOCA_FLOW_PIPE_HASH, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_HASH);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_hash_map_algorithm(pipe_cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg algorithm: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = ingress_fwd_pipe;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow ingress root classifier pipe with source ip match on the switch port.
 * The pipe will set the source case index to the meta based on the source IP.
 *
 * @sw_port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_classifier_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;

	fwd.type = DOCA_FLOW_FWD_HASH_PIPE;
	fwd.hash_pipe.pipe = hash_pipe;
	fwd.hash_pipe.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	/* set mask value */
	actions.meta.u32[1] = UINT32_MAX;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "INGRESS_ROOT_CLASSIFIER_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_ROOT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow root pipe entries to the pipes.
 *
 * @pipe [in]: pipe of the entry
 * @entries [in]: pipe of the entries
 * @nb_entries [in]: pipe of the entries
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_ingress_root_classifier_pipe_entries(struct doca_flow_pipe *pipe,
							     struct doca_flow_pipe_entry **entries,
							     uint32_t nb_entries,
							     struct entries_status *status)
{
	doca_error_t result;
	struct doca_flow_match match;
	uint32_t flags = DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH;
	uint32_t entry_index = 0;
	struct doca_flow_actions actions = {0};

	memset(&match, 0, sizeof(match));
	for (entry_index = 0; entry_index < nb_entries; entry_index++) {
		memset(&actions, 0, sizeof(actions));
		/* last entry should be inserted with DOCA_FLOW_ENTRY_FLAGS_NO_WAIT flag */
		if (entry_index == nb_entries - 1)
			flags = DOCA_FLOW_ENTRY_FLAGS_NO_WAIT;

		match.outer.ip4.src_ip = flooding_table[entry_index].classifier_addr;
		/* set classifier index meta value */
		actions.meta.u32[1] = flooding_table[entry_index].case_id;
		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match,
							0,
							&actions,
							NULL,
							NULL,
							flags,
							status,
							&entries[entry_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add root pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow hash pipe entries to the pipes.
 *
 * @pipe [in]: pipe of the entry
 * @entries [in]: pipe of the entries
 * @nb_entries [in]: pipe of the entries
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_hash_pipe_entries(struct doca_flow_pipe *pipe,
					  struct doca_flow_pipe_entry **entries,
					  uint32_t nb_entries,
					  struct entries_status *status)
{
	doca_error_t result;
	uint32_t flags = DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH;
	uint32_t entry_index = 0;
	struct doca_flow_actions actions = {0};

	for (entry_index = 0; entry_index < nb_entries; entry_index++) {
		memset(&actions, 0, sizeof(actions));
		/* last entry should be inserted with DOCA_FLOW_ENTRY_FLAGS_NO_WAIT flag */
		if (entry_index == nb_entries - 1)
			flags = DOCA_FLOW_ENTRY_FLAGS_NO_WAIT;

		/* set pipe meta value */
		actions.meta.u32[0] = entry_index;
		result = doca_flow_pipe_hash_add_entry(0,
						       pipe,
						       entry_index,
						       0,
						       &actions,
						       NULL,
						       NULL,
						       flags,
						       status,
						       &entries[entry_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add hash pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow pipe entries to the fwd pipe.
 * The miss packets will be dropped per-pipe's fwd_miss configuration.
 *
 * @pipe [in]: pipe of the entry
 * @entries [in]: pipe of the entries
 * @fcase [in]: flooding case
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_fwd_pipe_entries(struct doca_flow_pipe *pipe,
					 struct doca_flow_pipe_entry **entries,
					 struct flooding_case *fcase,
					 struct entries_status *status)
{
	doca_error_t result;
	struct doca_flow_match match;
	uint32_t flags = DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH;
	uint32_t entry_index = 0;
	struct doca_flow_actions actions, *action_ptr;
	struct doca_flow_fwd fwd;
	uint32_t act_idx = 0;
	doca_be32_t encap_dst_ip_addr = BE_IPV4_ADDR(81, 81, 81, 81);
	doca_be32_t encap_src_ip_addr = BE_IPV4_ADDR(11, 21, 31, 41);
	doca_be16_t encap_flags_fragment_offset = DOCA_HTOBE16(DOCA_FLOW_IP4_FLAG_DONT_FRAGMENT);
	uint8_t encap_ttl = 17;
	doca_be32_t encap_vxlan_tun_id = DOCA_HTOBE32(0xadadad);
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	struct fwd_info *fwd_info_array = &fcase->fwd_clone_info[0];
	uint32_t nb_entries = fcase->clone_count;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));
	for (entry_index = 0; entry_index < nb_entries; entry_index++) {
		memset(&actions, 0, sizeof(actions));
		/* last entry should be inserted with DOCA_FLOW_ENTRY_FLAGS_NO_WAIT flag */
		if (entry_index == nb_entries - 1)
			flags = DOCA_FLOW_ENTRY_FLAGS_NO_WAIT;

		action_ptr = NULL;
		act_idx = ACT_IDX_NONE;
		match.meta.u32[0] = fwd_info_array[entry_index].clone_idx;
		match.meta.u32[1] = fwd_info_array[entry_index].src_idx;
		if (fwd_info_array[entry_index].act_idx == ACT_IDX_ENCAP) {
			act_idx = ACT_IDX_ENCAP;
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
			actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
			actions.encap_cfg.encap.outer.ip4.src_ip = encap_src_ip_addr;
			actions.encap_cfg.encap.outer.ip4.dst_ip = encap_dst_ip_addr;
			actions.encap_cfg.encap.outer.ip4.flags_fragment_offset = encap_flags_fragment_offset;
			actions.encap_cfg.encap.outer.ip4.ttl = encap_ttl;
			actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
			actions.encap_cfg.encap.tun.vxlan_tun_id = encap_vxlan_tun_id;
			actions.encap_cfg.encap.tun.vxlan_tun_rsvd1 = 100;
			action_ptr = &actions;
		} else if (fwd_info_array[entry_index].act_idx == ACT_IDX_MODIFY) {
			act_idx = ACT_IDX_MODIFY;
			actions.outer.ip4.src_ip = BE_IPV4_ADDR(2, 2, 3, 4);
			action_ptr = &actions;
		}

		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = fwd_info_array[entry_index].fwd_dest_port;

		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match,
							act_idx,
							action_ptr,
							NULL,
							&fwd,
							flags,
							status,
							&entries[entry_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add hash pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_mult_flooding_cases sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_mult_flooding_cases(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct entries_status status;
	doca_error_t result;

	memset(&status, 0, sizeof(status));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = NB_TOTAL; /* counter per entry */

	result = init_doca_flow(nb_queues, "switch,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NB_TOTAL));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	result = create_ingress_fwd_pipe(doca_flow_port_switch_get(NULL), &ingress_fwd_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress fwd pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_fwd_pipe_entries(ingress_fwd_pipe, &ingress_fwd_entries[0], &flooding_table[0], &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to ingress fwd pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_fwd_pipe_entries(ingress_fwd_pipe,
				      &ingress_fwd_entries[flooding_table[0].clone_count],
				      &flooding_table[1],
				      &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to ingress fwd pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_ingress_hash_pipe(doca_flow_port_switch_get(NULL), &hash_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_hash_pipe_entries(hash_pipe, &ingress_hash_entries[0], NB_INGRESS_HASH, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to hash pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_ingress_classifier_pipe(doca_flow_port_switch_get(NULL), &ingress_root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_ingress_root_classifier_pipe_entries(ingress_root_pipe,
							  &ingress_root_entries[0],
							  NB_INGRESS_ROOT,
							  &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to hash pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_entries_process(doca_flow_port_switch_get(NULL), 0, DEFAULT_TIMEOUT_US, NB_TOTAL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	if (status.nb_processed != NB_TOTAL || status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_BAD_STATE;
	}

	/* Setup statistics context and wait for packets */
	struct hash_mult_flooding_stats_context stats_ctx = {.ingress_fwd_entries = ingress_fwd_entries};

	flow_wait_for_packets(15, print_hash_mult_flooding_stats_wrapper, &stats_ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
