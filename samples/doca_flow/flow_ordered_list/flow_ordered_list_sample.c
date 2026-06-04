/*
 * Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_definitions.h>

#include <flow_common.h>
#include "doca_bitfield.h"

DOCA_LOG_REGISTER(FLOW_ORDERED_LIST);

#define FLOW_ORDERED_LIST_STATS_WAIT_SEC 10

/* Actions with ordered_list_fwd_idx for meta forwarding mode */
struct ordered_list_entry_actions {
	struct doca_flow_actions base;
	uint32_t ordered_list_fwd_idx;
};

/*
 * Create definitions with ordered_list_fwd_idx for meta forwarding mode.
 *
 * @defs [out]: created definitions
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t init_ordered_list_definitions(struct doca_flow_definitions **defs)
{
	struct doca_flow_definitions_cfg *defs_cfg = NULL;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions cfg: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	result = doca_flow_definitions_create(defs_cfg, defs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	result = doca_flow_definitions_add_field(*defs,
						 "actions.packet.meta.ordered_list_fwd_idx",
						 offsetof(struct ordered_list_entry_actions, ordered_list_fwd_idx),
						 sizeof(uint32_t));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ordered_list_fwd_idx field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(*defs);
		goto cleanup;
	}
cleanup:
	if (defs_cfg != NULL)
		doca_flow_definitions_cfg_destroy(defs_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe with changeable 5 tuple match as root
 *
 * @port [in]: port of the pipe
 * @next_pipe [in]: ordered list pipe
 * @use_meta_fwd_mode [in]: true for meta forwarding
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t create_root_pipe(struct doca_flow_port *port,
			      struct doca_flow_pipe *next_pipe,
			      bool use_meta_fwd_mode,
			      struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct ordered_list_entry_actions meta_actions;
	struct ordered_list_entry_actions meta_actions_mask;
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_actions *actions_mask_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&meta_actions, 0, sizeof(meta_actions));
	memset(&meta_actions_mask, 0, sizeof(meta_actions_mask));
	memset(&fwd, 0, sizeof(fwd));

	/* 5 tuple match */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = UINT32_MAX;
	match.outer.ip4.dst_ip = UINT32_MAX;
	match.outer.tcp.l4_port.src_port = UINT16_MAX;
	match.outer.tcp.l4_port.dst_port = UINT16_MAX;

	if (use_meta_fwd_mode) {
		/* ordered_list_entry_actions: template ordered_list_fwd_idx=0, mask marks field for modify */
		meta_actions.ordered_list_fwd_idx = DOCA_HTOBE32(0);
		meta_actions_mask.ordered_list_fwd_idx = DOCA_HTOBE32(UINT32_MAX);
		actions_arr[0] = (struct doca_flow_actions *)&meta_actions;
		actions_mask_arr[0] = (struct doca_flow_actions *)&meta_actions_mask;
	} else {
		actions_arr[0] = &actions;
	}

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "ROOT_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg,
						actions_arr,
						use_meta_fwd_mode ? actions_mask_arr : NULL,
						NULL,
						NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (use_meta_fwd_mode) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = next_pipe;
	} else {
		fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
		fwd.ordered_list_pipe.pipe = next_pipe;
		fwd.ordered_list_pipe.idx = UINT32_MAX;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the root pipe that forwards the traffic to ordered list pipe entries
 *
 * @pipe [in]: pipe of the entries
 * @next_pipe [in]: ordered list pipe to forward the matched traffic
 * @use_meta_fwd_mode [in]: true for meta forwarding
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t add_root_pipe_entries(struct doca_flow_pipe *pipe,
				   struct doca_flow_pipe *next_pipe,
				   bool use_meta_fwd_mode,
				   struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct ordered_list_entry_actions entry_actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;
	const uint32_t nb_root_entries = 4;
	doca_be32_t dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8);
	doca_be16_t dst_port = DOCA_HTOBE16(80);
	doca_be16_t src_port = DOCA_HTOBE16(1234);
	uint32_t idx;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&entry_actions, 0, sizeof(entry_actions));

	match.outer.ip4.dst_ip = dst_ip_addr;
	match.outer.tcp.l4_port.dst_port = dst_port;
	match.outer.tcp.l4_port.src_port = src_port;

	if (use_meta_fwd_mode) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = next_pipe;
	} else {
		fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
		fwd.ordered_list_pipe.pipe = next_pipe;
	}

	for (idx = 0; idx < nb_root_entries; idx++) {
		uint8_t src_octet = (uint8_t)(idx + 1);

		match.outer.ip4.src_ip = BE_IPV4_ADDR(src_octet, src_octet, src_octet, src_octet);
		if (use_meta_fwd_mode)
			entry_actions.ordered_list_fwd_idx = DOCA_HTOBE32(idx);
		else
			fwd.ordered_list_pipe.idx = idx;
		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match,
							0,
							use_meta_fwd_mode ? &entry_actions.base : NULL,
							NULL,
							&fwd,
							0,
							status,
							&entry);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow ordered list pipe with two lists
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @use_meta_fwd_mode [in]: true for meta forwarding
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t create_ordered_list_pipe(struct doca_flow_port *port,
				      int port_id,
				      bool use_meta_fwd_mode,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd;
	const int nb_ordered_lists = 2;
	struct doca_flow_monitor counter;
	struct doca_flow_monitor counter_2;
	struct doca_flow_monitor meter;
	struct doca_flow_actions actions;
	struct doca_flow_actions actions_mask;
	struct ordered_list_entry_actions meta_actions;
	struct ordered_list_entry_actions meta_actions_mask;
	struct doca_flow_nat64_action nat64;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_ordered_list ordered_list_0;
	struct doca_flow_ordered_list ordered_list_1;
	struct doca_flow_ordered_list_element element_0;
	struct doca_flow_ordered_list_element element_1;
	struct doca_flow_ordered_list_element element_2;
	struct doca_flow_ordered_list_element element_3;
	struct doca_flow_ordered_list_element element_4;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	doca_error_t result;

	memset(&fwd, 0, sizeof(fwd));
	memset(&counter, 0, sizeof(counter));
	memset(&counter_2, 0, sizeof(counter_2));
	memset(&meter, 0, sizeof(meter));
	memset(&actions, 0, sizeof(actions));
	memset(&actions_mask, 0, sizeof(actions_mask));
	memset(&meta_actions, 0, sizeof(meta_actions));
	memset(&meta_actions_mask, 0, sizeof(meta_actions_mask));
	memset(&nat64, 0, sizeof(nat64));
	memset(&ordered_list_0, 0, sizeof(ordered_list_0));
	memset(&ordered_list_1, 0, sizeof(ordered_list_1));
	memset(&element_0, 0, sizeof(element_0));
	memset(&element_1, 0, sizeof(element_1));
	memset(&element_2, 0, sizeof(element_2));
	memset(&element_3, 0, sizeof(element_3));
	memset(&element_4, 0, sizeof(element_4));

	ordered_lists[0] = &ordered_list_0;
	ordered_lists[1] = &ordered_list_1;

	element_0.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	if (use_meta_fwd_mode) {
		/* Meta mode: engine expects extended layout (ordered_list_fwd_idx) when definitions are registered */
		element_0.actions = &meta_actions.base;
		element_0.actions_mask = &meta_actions_mask.base;
	} else {
		element_0.actions = &actions;
		element_0.actions_mask = &actions_mask;
	}
	element_1.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	element_1.monitor = &counter;
	element_2.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_NAT64;
	element_2.nat64 = &nat64;
	element_3.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	element_3.monitor = &counter_2;
	element_4.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	element_4.monitor = &meter;

	ordered_list_0.idx = 0;
	ordered_list_0.size = 3;
	ordered_list_0.elements = (struct doca_flow_ordered_list_element[]){element_4, element_1, element_0};

	ordered_list_1.idx = 1;
	ordered_list_1.size = 4;
	ordered_list_1.elements = (struct doca_flow_ordered_list_element[]){element_0, element_3, element_2, element_3};

	/* monitor with changeable shared counter ID */
	counter.counter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	counter.shared_counter.shared_counter_id = UINT32_MAX;

	counter_2.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	/* monitor with changeable shared meter ID */
	meter.meter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	meter.shared_meter.shared_meter_id = UINT32_MAX;

	/* modify src ip (ordered_list_fwd_idx unused for list elements) */
	if (use_meta_fwd_mode) {
		meta_actions.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		meta_actions.base.outer.ip4.src_ip = BE_IPV4_ADDR(192, 168, 0, 0);
		meta_actions_mask.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		meta_actions_mask.base.outer.ip4.src_ip = BE_IPV4_ADDR(255, 255, 0, 0);
	} else {
		actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		actions.outer.ip4.src_ip = BE_IPV4_ADDR(192, 168, 0, 0);
		actions_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		actions_mask.outer.ip4.src_ip = BE_IPV4_ADDR(255, 255, 0, 0);
	}

	nat64.original_l3_type = DOCA_FLOW_L3_TYPE_IP4;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "ORDERED_LIST_PIPE", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, FLOW_COMMON_PIPE_RULES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg number of entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (use_meta_fwd_mode) {
		result = doca_flow_pipe_cfg_set_ordered_list_fwd_mode(pipe_cfg, DOCA_FLOW_ORDERED_LIST_FWD_MODE_META);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set ordered list fwd mode: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the ordered list pipe.
 *
 * @pipe [in]: pipe of the entries
 * @shared_cntr_ids [in]: shared counter IDs to use
 * @shared_meter_id [in]: shared meter ID to use for ordered_list_0 entries
 * @use_meta_fwd_mode [in]: true for meta forwarding (must match pipe template layout)
 * @status [in]: user context for adding entry
 * @entries_list1 [out]: array to store pointers to entries using ordered_list_1
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t add_ordered_list_pipe_entries(struct doca_flow_pipe *pipe,
					   uint32_t *shared_cntr_ids,
					   uint32_t shared_meter_id,
					   bool use_meta_fwd_mode,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry **entries_list1)
{
	struct doca_flow_pipe_entry *entry1;
	struct doca_flow_pipe_entry *entry2;
	struct doca_flow_pipe_entry *entry3;
	struct doca_flow_pipe_entry *entry4;
	struct doca_flow_ordered_list ordered_list_0;
	struct doca_flow_ordered_list ordered_list_1;
	struct doca_flow_ordered_list_element element_0;
	struct doca_flow_ordered_list_element element_1;
	struct doca_flow_ordered_list_element element_2;
	struct doca_flow_ordered_list_element element_3;
	struct doca_flow_ordered_list_element element_4;
	struct doca_flow_monitor counter;
	struct doca_flow_monitor counter_2;
	struct doca_flow_monitor meter;
	struct doca_flow_actions actions;
	struct doca_flow_actions actions_mask;
	struct ordered_list_entry_actions meta_actions;
	struct ordered_list_entry_actions meta_actions_mask;
	struct doca_flow_nat64_action nat64;
	doca_error_t result;

	element_0.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	if (use_meta_fwd_mode) {
		element_0.actions = &meta_actions.base;
		element_0.actions_mask = &meta_actions_mask.base;
	} else {
		element_0.actions = &actions;
		element_0.actions_mask = &actions_mask;
	}
	element_1.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	element_1.monitor = &counter;
	element_2.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_NAT64;
	element_2.nat64 = &nat64;
	element_3.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	element_3.monitor = &counter_2;
	element_4.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	element_4.monitor = &meter;

	ordered_list_0.idx = 0;
	ordered_list_0.size = 3;
	ordered_list_0.elements = (struct doca_flow_ordered_list_element[]){element_4, element_1, element_0};

	/* first entry with shared counter ID in idx = 0*/
	counter.counter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	counter.shared_counter.shared_counter_id = shared_cntr_ids[0];
	/* shared meter */
	meter.meter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	meter.shared_meter.shared_meter_id = shared_meter_id;

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       pipe,
						       0,
						       &ordered_list_0,
						       NULL,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       &entry1);
	if (result != DOCA_SUCCESS)
		return result;

	ordered_list_1.idx = 1;
	ordered_list_1.size = 4;
	ordered_list_1.elements = (struct doca_flow_ordered_list_element[]){element_0, element_3, element_2, element_3};

	/* second entry uses ordered_list_1 with non-shared counters*/
	result = doca_flow_pipe_ordered_list_add_entry(0,
						       pipe,
						       1,
						       &ordered_list_1,
						       NULL,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       &entry2);
	if (result != DOCA_SUCCESS)
		return result;
	entries_list1[0] = entry2; /* Store for later querying */

	/* third entry with shared counter ID in idx = 1*/
	ordered_list_0.idx = 0;
	counter.shared_counter.shared_counter_id = shared_cntr_ids[1];
	result = doca_flow_pipe_ordered_list_add_entry(0,
						       pipe,
						       2,
						       &ordered_list_0,
						       NULL,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       &entry3);
	if (result != DOCA_SUCCESS)
		return result;

	/* fourth entry uses ordered_list_1 at index 3 and non-shared counters */
	ordered_list_1.idx = 1;
	result = doca_flow_pipe_ordered_list_add_entry(0,
						       pipe,
						       3,
						       &ordered_list_1,
						       NULL,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       &entry4);
	if (result != DOCA_SUCCESS)
		return result;
	entries_list1[1] = entry4; /* Store for later querying */

	return DOCA_SUCCESS;
}

/*
 * Run flow_ordered_list sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */

/* Context structure for statistics printing */
struct ordered_list_stats_context {
	int nb_ports;
	struct doca_flow_port **ports;
	int nb_shared_counters;
	uint32_t (*shared_counter_ids)[2];	     /* 2D array pointer */
	struct doca_flow_pipe_entry **entries_list1; /* Entries using ordered_list_1 */
	int nb_entries_list1;			     /* Number of entries using ordered_list_1 per port */
};

/*
 * Print ordered list statistics
 *
 * @nb_ports [in]: number of ports
 * @ports [in]: array of DOCA flow ports
 * @nb_shared_counters [in]: number of shared counters
 * @shared_counter_ids [in]: 2D array of shared counter IDs
 * @entries_list1 [in]: array of entries using ordered_list_1
 * @nb_entries_list1 [in]: number of entries per port using ordered_list_1
 */
static void print_ordered_list_stats(int nb_ports,
				     struct doca_flow_port *ports[],
				     int nb_shared_counters,
				     uint32_t shared_counter_ids[][nb_shared_counters],
				     struct doca_flow_pipe_entry **entries_list1,
				     int nb_entries_list1)
{
	doca_error_t result;
	int port_id, shared_res_idx, entry_idx, counter_idx;
	struct doca_flow_resource_query query_results[nb_ports][nb_shared_counters];
	const int nb_counters_per_entry = 2;

	/* Query and print shared counters */
	DOCA_LOG_INFO("=== Shared Counter Statistics ===");

	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = doca_flow_port_shared_resources_query(ports[port_id],
							       DOCA_FLOW_SHARED_RESOURCE_COUNTER,
							       &shared_counter_ids[port_id][0],
							       query_results[port_id],
							       nb_shared_counters);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query shared counter resource: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return;
		}
	}

	for (shared_res_idx = 0; shared_res_idx < nb_shared_counters; shared_res_idx++) {
		for (port_id = 0; port_id < nb_ports; port_id++) {
			DOCA_LOG_INFO("Port %d - Shared Counter %d:", port_id, shared_res_idx);
			DOCA_LOG_INFO("  Total bytes: %ld", query_results[port_id][shared_res_idx].counter.total_bytes);
			DOCA_LOG_INFO("  Total packets: %ld",
				      query_results[port_id][shared_res_idx].counter.total_pkts);
		}
	}

	/* Query and print ordered_list_1 entry counters (non-shared) */
	DOCA_LOG_INFO("=== Ordered List Entry Statistics ===");
	for (port_id = 0; port_id < nb_ports; port_id++) {
		for (entry_idx = 0; entry_idx < nb_entries_list1; entry_idx++) {
			struct doca_flow_pipe_entry *entry = entries_list1[port_id * nb_entries_list1 + entry_idx];
			struct doca_flow_resource_query entry_query_results[nb_counters_per_entry];

			if (entry == NULL)
				continue;

			memset(entry_query_results, 0, sizeof(entry_query_results));
			result = doca_flow_resource_query_entry(entry, entry_query_results);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query entry %d on port %d: %s",
					     entry_idx,
					     port_id,
					     doca_error_get_descr(result));
				continue;
			}

			DOCA_LOG_INFO("Port %d - Entry %d Counter:", port_id, entry_idx);
			for (counter_idx = 0; counter_idx < nb_counters_per_entry; counter_idx++) {
				DOCA_LOG_INFO("  Counter %d - Total bytes: %ld, Total packets: %ld",
					      counter_idx,
					      entry_query_results[counter_idx].counter.total_bytes,
					      entry_query_results[counter_idx].counter.total_pkts);
			}
		}
	}
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: ordered_list_stats_context structure
 */
static void print_ordered_list_stats_wrapper(void *context)
{
	struct ordered_list_stats_context *ctx = (struct ordered_list_stats_context *)context;
	print_ordered_list_stats(ctx->nb_ports,
				 ctx->ports,
				 ctx->nb_shared_counters,
				 ctx->shared_counter_ids,
				 ctx->entries_list1,
				 ctx->nb_entries_list1);
}

doca_error_t flow_ordered_list(int nb_queues, bool use_meta_fwd_mode)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *root_pipe;
	struct doca_flow_pipe *ordered_list_pipe;
	struct doca_flow_definitions *defs = NULL;
	int shared_res_idx, nb_shared_counters = 2;
	int nb_entries_list1 = 2;
	uint32_t shared_counter_ids[nb_ports][nb_shared_counters];
	struct doca_flow_pipe_entry *entries_list1[nb_ports * nb_entries_list1];
	struct doca_flow_shared_resource_cfg cfg = {0};
	struct doca_flow_resource_meter_cfg meter_cfg = {0};
	struct ordered_list_stats_context stats_ctx = {0};
	int port_id;
	struct entries_status status;
	int num_of_entries = 8;
	uint32_t shared_meter_ids[nb_ports];
	doca_error_t result;
	bool flow_initialized = false;
	bool ports_initialized = false;

	memset(entries_list1, 0, sizeof(entries_list1));

	if (use_meta_fwd_mode) {
		result = init_ordered_list_definitions(&defs);
		if (result != DOCA_SUCCESS)
			return result;
	}

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = 2;
	resource.nr_counters += num_of_entries;
	resource.nr_meters = 2;
	nr_shared_resources[DOCA_FLOW_SHARED_RESOURCE_COUNTER] = nb_shared_counters;
	nr_shared_resources[DOCA_FLOW_SHARED_RESOURCE_METER] = 1;
	if (use_meta_fwd_mode) {
		result = init_doca_flow_with_defs(nb_queues, "vnf,hws", &resource, nr_shared_resources, defs);
	} else {
		result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	flow_initialized = true;

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	ports_initialized = true;

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));

		result = create_ordered_list_pipe(ports[port_id], port_id, use_meta_fwd_mode, &ordered_list_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ordered list pipe: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		for (shared_res_idx = 0; shared_res_idx < nb_shared_counters; shared_res_idx++) {
			result = doca_flow_port_shared_resource_get(ports[port_id],
								    DOCA_FLOW_SHARED_RESOURCE_COUNTER,
								    &shared_counter_ids[port_id][shared_res_idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get shared counter id from port %d", port_id);
				goto cleanup;
			}

			result = doca_flow_port_shared_resource_set_cfg(ports[port_id],
									DOCA_FLOW_SHARED_RESOURCE_COUNTER,
									shared_counter_ids[port_id][shared_res_idx],
									&cfg);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to configure shared counter to port %d", port_id);
				goto cleanup;
			}
		}

		result = doca_flow_port_shared_resource_get(ports[port_id],
							    DOCA_FLOW_SHARED_RESOURCE_METER,
							    &shared_meter_ids[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get shared meter id from port %d", port_id);
			goto cleanup;
		}
		meter_cfg.limit_type = DOCA_FLOW_METER_LIMIT_TYPE_BYTES;
		meter_cfg.color_mode = DOCA_FLOW_METER_COLOR_MODE_BLIND;
		meter_cfg.alg = DOCA_FLOW_METER_ALGORITHM_TYPE_RFC2697;
		meter_cfg.cir = 1000000;
		meter_cfg.cbs = 100000;
		meter_cfg.rfc2697.ebs = 0;
		cfg.meter_cfg = meter_cfg;
		result = doca_flow_port_shared_resource_set_cfg(ports[port_id],
								DOCA_FLOW_SHARED_RESOURCE_METER,
								shared_meter_ids[port_id],
								&cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure shared meter to port %d", port_id);
			goto cleanup;
		}

		result = add_ordered_list_pipe_entries(ordered_list_pipe,
						       &shared_counter_ids[port_id][0],
						       shared_meter_ids[port_id],
						       use_meta_fwd_mode,
						       &status,
						       &entries_list1[port_id * nb_entries_list1]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add ordered list pipe entries: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		result = create_root_pipe(ports[port_id], ordered_list_pipe, use_meta_fwd_mode, &root_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
			goto cleanup;
		}
		result = add_root_pipe_entries(root_pipe, ordered_list_pipe, use_meta_fwd_mode, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add root pipe entries: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		if (status.nb_processed != num_of_entries || status.failure) {
			DOCA_LOG_ERR("Failed to process entries");
			result = DOCA_ERROR_BAD_STATE;
			goto cleanup;
		}
	}

	/* Setup statistics context and wait for packets */
	stats_ctx.nb_ports = nb_ports;
	stats_ctx.ports = ports;
	stats_ctx.nb_shared_counters = nb_shared_counters;
	stats_ctx.shared_counter_ids = shared_counter_ids;
	stats_ctx.entries_list1 = entries_list1;
	stats_ctx.nb_entries_list1 = nb_entries_list1;

	flow_wait_for_packets(FLOW_ORDERED_LIST_STATS_WAIT_SEC, print_ordered_list_stats_wrapper, &stats_ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	ports_initialized = false;
	flow_initialized = false;
	doca_flow_destroy();
	if (defs != NULL) {
		doca_flow_definitions_destroy(defs);
		defs = NULL;
	}
	return result;

cleanup:
	if (ports_initialized) {
		stop_doca_flow_ports(nb_ports, ports);
		ports_initialized = false;
	}
	if (flow_initialized) {
		doca_flow_destroy();
		flow_initialized = false;
	}
	if (defs != NULL) {
		doca_flow_definitions_destroy(defs);
		defs = NULL;
	}
	return result;
}
