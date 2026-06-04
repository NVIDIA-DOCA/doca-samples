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

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_srv6.h>
#include <doca_flow_net.h>
#include <rte_ethdev.h>
#include "doca_flow_definitions.h"

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_SRV6);

#define NB_ORDERED_LISTS (3)
#define NB_SRV6_SIDS (2)
#define NEXT_PROTO_ROUTING_HEADER (43)
#define ROUTING_TYPE (4)
#define NR_ENTRIES (32)
#define NB_MAX_OL_ENTRIES (8)
#define NB_FLOW_ENTRIES (4)
#define SEGMENTS_LEFT_BIT_WIDTH (8)
#define SID_BIT_WIDTH (128)
#define NB_MAX_SRH (16)
#define WAIT_SEC (5)
#define PACKET_BURST (8)
#define NB_COUNTERS (16)
#define NB_OL_ENTRIES (2)
#define PRIORITY0 (0)
#define PRIORITY1 (1)
#define PRIORITY2 (2)
#define PRIORITY3 (3)

enum {
	WIRE_PORT = 0,
	VF1_PORT = 1,
	VF2_PORT = 2,
};

struct srv6_match {
	struct doca_flow_match base;
	uint8_t segments_left;
};

struct srv6_actions {
	struct doca_flow_actions base;
	uint8_t segments_left;
};

/* Context structure for statistics printing */
struct ordered_list_stats_context {
	struct doca_flow_pipe_entry *ol_entries[NB_MAX_OL_ENTRIES];
	int nb_ol_entries;
	struct doca_flow_pipe_entry *classifier_entries[NB_MAX_OL_ENTRIES];
	int nb_classifier_entries;
};
static struct ordered_list_stats_context stats_ctx;
struct doca_flow_external_action_srv6_entry *srv6_entry;

static_assert(offsetof(struct srv6_match, base) == 0,
	      "srv6_match: 'base' must be the first field to preserve compatibility with legacy doca_flow_match");
static_assert(offsetof(struct srv6_actions, base) == 0,
	      "srv6_actions: 'base' must be the first field to preserve compatibility with legacy doca_flow_actions");

/*
 * Initialize DOCA Flow with SRv6 definition fields for segments_left match and action.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t init_doca_flow_with_srv6_defs(int nb_queues)
{
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT,
					  .nr_counters = NB_COUNTERS,
					  .nr_rss = 1};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_definitions_cfg *defs_cfg = NULL;
	struct doca_flow_definitions *definitions = NULL;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions configuration structure: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_create(defs_cfg, &definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions object: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_cfg_destroy(defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy definitions configuration structure: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	defs_cfg = NULL;

	result = doca_flow_definitions_add_field(definitions,
						 "match.packet.outer.srv6.segments_left",
						 offsetof(struct srv6_match, segments_left),
						 sizeof(((struct srv6_match *)0)->segments_left));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add match segments_left field: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "actions.packet.outer.srv6.segments_left",
						 offsetof(struct srv6_actions, segments_left),
						 sizeof(((struct srv6_actions *)0)->segments_left));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add actions segments_left field: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = init_doca_flow_with_defs(nb_queues, "switch", &resource, nr_shared_resources, definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	doca_flow_definitions_destroy(definitions);
	return DOCA_SUCCESS;

cleanup:
	doca_flow_definitions_destroy(definitions);
	if (defs_cfg)
		doca_flow_definitions_cfg_destroy(defs_cfg);
	return result;
}

/*
 * Create ordered list pipe with three lists for SRv6 PUSH & decrement+copy
 *
 * @port [in]: port of the pipe
 * @push_action [in]: SRv6 PUSH pipe action handle
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t ordered_list_pipe_create(struct doca_flow_port *port,
					     void *push_action,
					     struct doca_flow_pipe **pipe)
{
	struct doca_flow_ordered_list_element elements_list0[3] = {0};
	struct doca_flow_ordered_list_element elements_list1[4] = {0};
	struct doca_flow_ordered_list ordered_list0 = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_ordered_list ordered_list1 = {0};
	struct doca_flow_ordered_list *ordered_lists[NB_OL_ENTRIES] = {0};

	struct srv6_actions pipe_actions_add = {
		.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6,
		.segments_left = UINT8_MAX,
	};
	struct doca_flow_action_desc action_descs_add = {
		.type = DOCA_FLOW_ACTION_ADD,
		.field_op.dst.field_string = "outer.srv6.segments_left",
		.field_op.dst.bit_offset = 0,
		.field_op.width = SEGMENTS_LEFT_BIT_WIDTH,
	};
	struct doca_flow_action_desc action_descs_copy = {
		.type = DOCA_FLOW_ACTION_COPY,
		.field_op.src.field_string = "outer.srv6.segments",
		.field_op.src.bit_offset = 0,
		.field_op.dst.field_string = "outer.ipv6.dst_ip",
		.field_op.dst.bit_offset = 0,
		.field_op.width = SID_BIT_WIDTH,
	};
	struct doca_flow_action_descs descs_list_copy = {
		.desc_array = &action_descs_copy,
		.nb_action_desc = 1,
	};
	struct doca_flow_action_descs descs_list_add = {
		.desc_array = &action_descs_add,
		.nb_action_desc = 1,
	};
	uint32_t nb_ordered_lists = 0;
	uint32_t size;
	doca_error_t result;

	/* List 0 (PUSH): external SRv6 PUSH + copy SID to IPv6 DIP -> fwd VF1 */

	/*
	 * Copy the active SID to the IPv6 destination address.
	 * DOCA stores SRH segments in traversal order: segments[0] is the
	 * first hop (active when segments_left == n-1 after PUSH), so
	 * bit_offset 0 references the active SID.
	 */
	size = 0;
	elements_list0[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
	elements_list0[size++].external_actions = push_action;
	elements_list0[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	elements_list0[size++].monitor = &monitor;
	elements_list0[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements_list0[size++].action_descs = &descs_list_copy;

	ordered_list0.idx = 0;
	ordered_list0.size = size;
	ordered_list0.elements = elements_list0;

	/* List 1 (DECREMENT + COPY): seg_left-1 + copy SID to IPv6 DIP -> fwd VF2 */
	size = 0;
	elements_list1[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements_list1[size].actions = &pipe_actions_add.base;
	elements_list1[size].actions_mask = &pipe_actions_add.base;
	elements_list1[size++].action_descs = &descs_list_add;
	elements_list1[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	elements_list1[size++].monitor = &monitor;
	elements_list1[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements_list1[size++].action_descs = &descs_list_copy;

	ordered_list1.idx = 1;
	ordered_list1.size = size;
	ordered_list1.elements = elements_list1;

	/* Create ordered list pipe */
	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SRV6_ORDERED_LIST_PIPE", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NR_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	ordered_lists[nb_ordered_lists++] = &ordered_list0;
	ordered_lists[nb_ordered_lists++] = &ordered_list1;
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg ordered lists: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 0xffff;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create doca_flow ordered_list pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * srv6_action_classifier pipe: classifies by the following matches
 * 1. match IPv6 && next_header == 43 && segments_left == 0, forward to RSS
 * 2. match IPv6 && next_header == 43, forward to ordered list pipe idx 1 (decrement + copy)
 * 3. match IPv6, forward to ordered list pipe idx 0 (PUSH SRH)
 * 5. match all, forward DROP
 *
 * @port [in]: port of the pipe
 * @srv6_action_classifier [in]: control pipe for routing header packets
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t srv6_action_classifier_pipe_create(struct doca_flow_port *port,
						       struct doca_flow_pipe **srv6_action_classifier)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "srv6_action_classifier", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NR_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, srv6_action_classifier);
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	return result;
}

static doca_error_t add_srv6_action_classifier_entries(struct doca_flow_pipe *ol_pipe,
						       struct doca_flow_pipe *srv6_action_classifier,
						       struct entries_status *status)
{
	struct srv6_match match = {0};
	struct srv6_match match_mask = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	uint16_t rss_queues[1] = {0};
	doca_error_t result;

	/* Priority 0: match IPv6 && next_header == 43 && segments_left == 0, forward to RSS */
	match.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match.base.outer.ip6.next_proto = NEXT_PROTO_ROUTING_HEADER;
	match.segments_left = 0;
	match_mask.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match_mask.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match_mask.base.outer.ip6.next_proto = 0xff;
	match_mask.segments_left = UINT8_MAX;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV6;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_control_add_entry(0,
						  srv6_action_classifier,
						  &match.base,
						  &match_mask.base,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  PRIORITY0,
						  &fwd,
						  status,
						  &stats_ctx.classifier_entries[stats_ctx.nb_classifier_entries]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry with priority 0 to srv6_action_classifier pipe: %s",
			     doca_error_get_descr(result));
		return result;
	}
	stats_ctx.nb_classifier_entries++;

	/* Priority 1: match IPv6 && next_header == 43, forward to ordered list pipe idx 1 (decrement + copy) */

	match.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match.base.outer.ip6.next_proto = NEXT_PROTO_ROUTING_HEADER;
	match.segments_left = 0;
	match_mask.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match_mask.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match_mask.base.outer.ip6.next_proto = 0xff;
	match_mask.segments_left = 0;

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ol_pipe;
	fwd.ordered_list_pipe.idx = 1;
	result = doca_flow_pipe_control_add_entry(0,
						  srv6_action_classifier,
						  &match.base,
						  &match_mask.base,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  PRIORITY1,
						  &fwd,
						  status,
						  &stats_ctx.classifier_entries[stats_ctx.nb_classifier_entries]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry with priority 1 to srv6_action_classifier pipe: %s",
			     doca_error_get_descr(result));
		return result;
	}
	stats_ctx.nb_classifier_entries++;

	/* priority 2: match IPv6, forward to ordered list pipe idx 0 (PUSH SRH) */

	match.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.base.outer.l3_type = 0;
	match.base.outer.ip6.next_proto = 0;
	match.segments_left = 0;
	match_mask.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match_mask.base.outer.l3_type = 0;
	match_mask.base.outer.ip6.next_proto = 0;
	match_mask.segments_left = 0;

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ol_pipe;
	fwd.ordered_list_pipe.idx = 0;
	result = doca_flow_pipe_control_add_entry(0,
						  srv6_action_classifier,
						  &match.base,
						  &match_mask.base,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  PRIORITY2,
						  &fwd,
						  status,
						  &stats_ctx.classifier_entries[stats_ctx.nb_classifier_entries]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry with priority 0 to srv6_action_classifier pipe: %s",
			     doca_error_get_descr(result));
		return result;
	}
	stats_ctx.nb_classifier_entries++;

	/* priority 3: match all, drop */
	fwd.type = DOCA_FLOW_FWD_DROP;
	result = doca_flow_pipe_control_add_entry(0,
						  srv6_action_classifier,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  PRIORITY3,
						  &fwd,
						  status,
						  &stats_ctx.classifier_entries[stats_ctx.nb_classifier_entries]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry with priority 3 to srv6_action_classifier pipe: %s",
			     doca_error_get_descr(result));
		return result;
	}
	stats_ctx.nb_classifier_entries++;

	return DOCA_SUCCESS;
}

/*
 * Add entries to the ordered list pipe.
 * - idx 0 (PUSH): provides SRH data with a segment list
 * - idx 1 (decrement + copy): no per-entry data needed
 *
 * @ol_pipe [in]: ordered list pipe
 * @status [in]: user context for adding entries
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_ordered_list_entries(struct doca_flow_pipe *ol_pipe, struct entries_status *status)
{
	const uint32_t srv6_entry_len =
		sizeof(struct doca_flow_external_action_srv6_entry) + NB_SRV6_SIDS * sizeof(struct doca_flow_ipv6_addr);
	struct doca_flow_ordered_list_element elements_list0[3] = {0};
	struct doca_flow_ordered_list_element elements_list1[3] = {0};
	struct doca_flow_ordered_list entry_list = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_header_ipv6_srh *srh;
	uint32_t size;
	doca_error_t result;

	srv6_entry = (struct doca_flow_external_action_srv6_entry *)calloc(1, srv6_entry_len);
	if (!srv6_entry) {
		DOCA_LOG_ERR("Failed to allocate srv6 entry");
		return DOCA_ERROR_NO_MEMORY;
	}
	srh = &srv6_entry->srh;

	/*
	 * Entry for list idx 0 (PUSH SRH, copy SID to DIP).
	 * The EXTERNAL_ACTIONS element needs per-entry SRH data for the PUSH operation.
	 */
	srh->base.next_header = 0;
	srh->base.hdr_ext_len = (NB_SRV6_SIDS * sizeof(struct doca_flow_ipv6_addr)) / 8;
	srh->base.routing_type = ROUTING_TYPE;
	srh->base.segments_left = NB_SRV6_SIDS - 1;
	srh->base.last_entry = NB_SRV6_SIDS - 1;
	srh->base.flags = 0;
	srh->base.tag = 0;

	/* SID 0 (active segment - will be copied to IPv6 DIP) */
	SET_IPV6_ADDR(srh->segments[0].addr,
		      DOCA_HTOBE32(0x20010db8),
		      DOCA_HTOBE32(0x00000001),
		      DOCA_HTOBE32(0x00000000),
		      DOCA_HTOBE32(0x00000001));
	/* SID 1 */
	SET_IPV6_ADDR(srh->segments[1].addr,
		      DOCA_HTOBE32(0x20010db8),
		      DOCA_HTOBE32(0x00000002),
		      DOCA_HTOBE32(0x00000000),
		      DOCA_HTOBE32(0x00000002));

	size = 0;
	elements_list0[size].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
	elements_list0[size++].external_actions = srv6_entry;
	elements_list0[size++].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	elements_list0[size++].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;

	entry_list.idx = 0;
	entry_list.size = size;
	entry_list.elements = elements_list0;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = VF1_PORT;

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       ol_pipe,
						       0,
						       &entry_list,
						       &fwd,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       &stats_ctx.ol_entries[stats_ctx.nb_ol_entries]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ordered list entry idx 0 (PUSH): %s", doca_error_get_descr(result));
		goto free_srv6_entry;
	}
	stats_ctx.nb_ol_entries++;

	/* Entry for list idx 1 (decrement segments_left + copy SID to DIP) */
	size = 0;
	elements_list1[size++].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements_list1[size++].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	elements_list1[size++].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	entry_list.idx = 1;
	entry_list.size = size;
	entry_list.elements = elements_list1;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = VF2_PORT;

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       ol_pipe,
						       1,
						       &entry_list,
						       &fwd,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       &stats_ctx.ol_entries[stats_ctx.nb_ol_entries]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ordered list entry idx 1 (DEC+COPY): %s", doca_error_get_descr(result));
		goto free_srv6_entry;
	}
	stats_ctx.nb_ol_entries++;

free_srv6_entry:
	free(srv6_entry);
	srv6_entry = NULL;
	return result;
}

/*
 * function for statistics printing compatible with flow_wait_for_packets
 */
static void print_ordered_list_stats(void *context)
{
	(void)context;

	int i;
	doca_error_t result;
	struct doca_flow_resource_query query_results[10];

	/* Query and print ordered_list entries counters (PUSH) */
	if (stats_ctx.nb_ol_entries != NB_OL_ENTRIES) {
		DOCA_LOG_INFO("Wrong number of ordered list entries %d", stats_ctx.nb_ol_entries);
		return;
	}

	DOCA_LOG_INFO("--------------------------------------------------------------");
	for (i = 0; i < stats_ctx.nb_ol_entries; i++) {
		DOCA_LOG_INFO("%s Node", i == 0 ? "Originator" : "Intermediate");
		memset(&query_results, 0, sizeof(query_results));
		result = doca_flow_resource_query_entry(stats_ctx.ol_entries[i], query_results);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query ordered list entry%d counter: %s",
				     i,
				     doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Packets: %lu, Bytes: %lu",
			      query_results[0].counter.total_pkts,
			      query_results[0].counter.total_bytes);
	}
	DOCA_LOG_INFO("--------------------------------------------------------------");

	for (i = 0; i < stats_ctx.nb_classifier_entries; i++) {
		DOCA_LOG_INFO("classifier entry%d", i);
		memset(&query_results, 0, sizeof(query_results));
		result = doca_flow_resource_query_entry(stats_ctx.classifier_entries[i], query_results);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query classifier entry%d counter: %s", i, doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Packets: %lu, Bytes: %lu",
			      query_results[0].counter.total_pkts,
			      query_results[0].counter.total_bytes);
	}
	DOCA_LOG_INFO("--------------------------------------------------------------");
}

static void process_packets(void)
{
	struct rte_mbuf *packets[PACKET_BURST];
	int port_id = 0;
	int queue_index = 0;
	int nb_packets;
	int i;

	nb_packets = rte_eth_rx_burst(port_id, queue_index, packets, PACKET_BURST);

	for (i = 0; i < nb_packets; i++) {
		/* perform POP SRH in software. HW will support POP SRH in next release */
		rte_pktmbuf_free(packets[i]);
	}
}

/*
 * Run flow_srv6 sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_srv6(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT,
					  .nr_counters = NB_COUNTERS,
					  .nr_rss = 1};
	struct entries_status status = {0};
	struct doca_flow_pipe *ol_pipe;
	struct doca_flow_pipe *srv6_action_classifier;
	struct doca_flow_external_action_srv6_pipe_action_cfg push_cfg = {0};
	void *push_action = NULL;
	const uint32_t nb_srh = NB_MAX_SRH;
	doca_error_t result;

	result = init_doca_flow_with_srv6_defs(nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow with SRv6 definitions: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NR_ENTRIES) + SRV6_ACTIONS_MEM_SIZE(nb_srh));
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

	push_cfg.op = DOCA_FLOW_EXT_ACT_SRV6_OP_PUSH;
	push_cfg.srh_size =
		sizeof(struct doca_flow_header_ipv6_srh_base) + NB_SRV6_SIDS * sizeof(struct doca_flow_ipv6_addr);
	result = doca_flow_external_action_srv6_pipe_action_create(&push_cfg, &push_action);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create SRv6 PUSH pipe action: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/* Create pipes bottom-up: ordered list -> srv6_action_classifier -> IPv6 match */
	result = ordered_list_pipe_create(ports[0], push_action, &ol_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ordered list pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = srv6_action_classifier_pipe_create(ports[0], &srv6_action_classifier);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create srv6_action_classifier pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = add_ordered_list_entries(ol_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ordered list entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = add_srv6_action_classifier_entries(ol_pipe, srv6_action_classifier, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add srv6_action_classifier entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = flow_process_entries(ports[0], &status, NB_FLOW_ENTRIES + NB_OL_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	flow_wait_for_packets(WAIT_SEC, print_ordered_list_stats, &stats_ctx);

	/* Process packets from RSS */
	process_packets();

cleanup:
	if (srv6_entry)
		free(srv6_entry);
	if (push_action)
		doca_flow_external_action_srv6_pipe_action_destroy(push_action);
	stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
