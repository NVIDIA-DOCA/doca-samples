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

#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <doca_bitfield.h>
#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_flow.h>
#include <doca_flow_definitions.h>
#include <doca_flow_ria.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ib.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include <flow_common.h>
#include <flow_switch_common.h>

#include "flow_eth_common.h"

DOCA_LOG_REGISTER(FLOW_RIA);

#define NB_PORTS (2)

/* Sample only sets up a single receive queue. */
#define NB_QUEUES (1)

/* Send queue depth, bounds how many forwarded packets can be in flight at once. */
#define TXQ_NB_SEND_TASKS (1024)

/* Logical queue id of the send queue, unique among the send queues of the device. */
#define TXQ_QUEUE_ID (0)

/* Only one metadata index is used (index 0). Its meaning is different on each domain:
 * - Ingress domain (Rx): RIA status pipe writes the RIA index, RIA status, RIA valid bit and source port ID.
 * - Egress domain (Tx): Holds the egress port ID.
 */
#define RIA_METADATA_NUM (1)

#define PKT_META_BIT_OFFSET (offsetof(struct doca_flow_meta, pkt_meta) << 3)

/* BTH opcodes, RC transport (transport bits [7:5] are zero). */
#define BTH_OPCODE_RDMA_WRITE_ONLY (0x0a)
#define BTH_OPCODE_ACKNOWLEDGE (0x11)

/* BTH.flags1 holds the Acknowledge Request bit followed by 7 reserved bits. */
#define BTH_FLAGS1_ACKREQ (0x80)

/* RIA table capacity. Allocating more distinct PSNs than this without releasing reports MISS. */
#define RIA_NUM_INDEXES (65536)

/* Number of RIA statuses the status pipe classifies, see enum doca_flow_external_action_ria_status. */
#define RIA_NB_STATUSES (4)

/* Number of RIA operations the sample exercises: ALLOC and DEALLOC. */
#define RIA_NB_OPERATIONS (2)

#define RIA_STATUS_PIPE_ENTRIES (NB_PORTS * RIA_NB_STATUSES)
#define FWD_PIPE_ENTRIES (NB_PORTS)
#define RIA_PIPE_ENTRIES (RIA_NB_OPERATIONS)
#define ROOT_PIPE_ENTRIES (RIA_NB_OPERATIONS)
#define EGRESS_PIPE_ENTRIES (NB_PORTS)

/*
 * Packet meta layout on Rx side:
 * - bits [0:15]  - RIA index, copied in action descs.
 * - bits [16:23] - source port ID
 * - bits [28:30] - RIA status.
 * - bit  [31]    - RIA status and index valid.
 */
#define PKT_META_RIA_VALID (1u << 31)
#define PKT_META_RIA_STATUS_SHIFT (28)
#define PKT_META_RIA_STATUS_MASK (0x7u)
#define PKT_META_RIA_INDEX_MASK (0x0000ffffu)
#define PKT_META_SOURCE_PORT_SHIFT (16)
#define PKT_META_SOURCE_PORT_MASK (0xffu)

static_assert(RIA_NUM_INDEXES - 1 <= PKT_META_RIA_INDEX_MASK,
	      "Allocated RIA index range should fit in packet meta mask");

static inline bool pkt_meta_is_ria_valid(uint32_t pkt_meta)
{
	return !!(pkt_meta & PKT_META_RIA_VALID);
}

static inline enum doca_flow_external_action_ria_status pkt_meta_extract_ria_status(uint32_t pkt_meta)
{
	return (pkt_meta >> PKT_META_RIA_STATUS_SHIFT) & PKT_META_RIA_STATUS_MASK;
}

static inline uint32_t pkt_meta_extract_ria_index(uint32_t pkt_meta)
{
	return pkt_meta & PKT_META_RIA_INDEX_MASK;
}

static inline uint16_t pkt_meta_extract_source_port(uint32_t pkt_meta)
{
	return (pkt_meta >> PKT_META_SOURCE_PORT_SHIFT) & PKT_META_SOURCE_PORT_MASK;
}

/* Extended match struct carrying the RIA execution status, registered as a custom field. */
struct custom_match_ria {
	struct doca_flow_match base;
	uint8_t ria_status;
};

static_assert(offsetof(struct custom_match_ria, base) == 0,
	      "custom_match_ria: 'base' must be the first field to stay usable as a doca_flow_match");

#define RIA_STATUS_SIZE (sizeof(((struct custom_match_ria *)0)->ria_status))
#define RIA_STATUS_OFF (offsetof(struct custom_match_ria, ria_status))

/* Description of one RIA operation the sample exercises, and the BTH pattern that triggers it. */
struct ria_entry_spec {
	/* Which operation should be executed. */
	enum doca_flow_external_action_ria_operation operation;

	/* Which ordered list of the RIA pipe is responsible for this operation. */
	uint32_t entry_idx;

	/* BTH opcode that triggers this operation. */
	uint8_t bth_opcode;

	/* BTH.flags1 value that triggers this operation, matched under a BTH_FLAGS1_ACKREQ mask. */
	uint8_t bth_flags1;

	/* Human readable operation string. */
	const char *operation_str;
};

static const struct ria_entry_spec ria_entry_specs[] = {
	{DOCA_FLOW_EXTERNAL_ACTION_RIA_OP_ALLOC, 0, BTH_OPCODE_RDMA_WRITE_ONLY, BTH_FLAGS1_ACKREQ, "ALLOC"},
	{DOCA_FLOW_EXTERNAL_ACTION_RIA_OP_DEALLOC, 1, BTH_OPCODE_ACKNOWLEDGE, 0, "DEALLOC"},
};

static_assert(ARRAY_DIM(ria_entry_specs) == RIA_NB_OPERATIONS, "RIA_NB_OPERATIONS must match ria_entry_specs");

/* RIA execution statuses the status pipe classifies, in the order the entries are created. */
static const enum doca_flow_external_action_ria_status ria_statuses[RIA_NB_STATUSES] = {
	DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_ALLOC_NEW_POINTER,
	DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_HIT,
	DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_MISS,
	DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_HW_ERROR,
};

static volatile sig_atomic_t force_quit;

/*
 * Signal handler, stops the packet processing loop
 *
 * @signum [in]: received signal number
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		force_quit = 1;
}

/*
 * Create the pipe that forwards traffic which did not trigger a RIA operation
 *
 * Matches the ingress port only and sends it to RSS so that the
 * application can transmit it on the peer port.
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_fwd_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct custom_match_ria match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	uint16_t rss_queues[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	/* Ingress port is changeable per entry. */
	match.base.parser_meta.port_id = UINT16_MAX;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "FWD_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, FWD_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add one entry per ingress port to the forwarding pipe
 *
 * @pipe [in]: pipe of the entries
 * @entries [out]: created entries, indexed by ingress port
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_fwd_pipe_entries(struct doca_flow_pipe *pipe,
					 struct doca_flow_pipe_entry *entries[FWD_PIPE_ENTRIES],
					 struct entries_status *status)
{
	struct custom_match_ria match;
	struct doca_flow_monitor monitor;
	doca_error_t result;
	uint16_t port_id;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	for (port_id = 0; port_id < FWD_PIPE_ENTRIES; ++port_id) {
		match.base.parser_meta.port_id = port_id;

		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match.base,
							0,
							NULL,
							&monitor,
							NULL,
							0,
							status,
							&entries[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add FWD_PIPE entry for port %u: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create the pipe that classifies the result of a RIA operation
 *
 * Matches the ingress port and the RIA execution status, copies the allocated RIA index into
 * pkt_meta, sets RIA status and RIA valid bit in pkt_meta, then sends it to RSS.
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ria_status_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct custom_match_ria match;
	struct custom_match_ria mask;
	struct doca_flow_actions actions;
	struct doca_flow_actions actions_mask;
	struct doca_flow_actions *actions_arr[1];
	struct doca_flow_actions *actions_mask_arr[1];
	struct doca_flow_action_descs action_descs;
	struct doca_flow_action_descs *action_descs_p[1];
	struct doca_flow_action_desc action_desc[1];
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	uint16_t rss_queues[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&actions, 0, sizeof(actions));
	memset(&actions_mask, 0, sizeof(actions_mask));
	memset(&action_desc, 0, sizeof(action_desc));
	memset(&action_descs, 0, sizeof(action_descs));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	/* Ingress port and RIA status are changeable per entry. */
	match.base.parser_meta.port_id = UINT16_MAX;
	mask.base.parser_meta.port_id = UINT16_MAX;
	match.ria_status = UINT8_MAX;
	mask.ria_status = UINT8_MAX;

	actions.meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = &actions;
	actions_mask.meta.pkt_meta =
		DOCA_HTOBE32(PKT_META_RIA_VALID | (PKT_META_RIA_STATUS_MASK << PKT_META_RIA_STATUS_SHIFT) |
			     (PKT_META_SOURCE_PORT_MASK << PKT_META_SOURCE_PORT_SHIFT));
	actions_mask_arr[0] = &actions_mask;

	action_desc[0].type = DOCA_FLOW_ACTION_COPY;
	action_desc[0].field_op.src.field_string = "parser_meta.ria.idx";
	action_desc[0].field_op.src.bit_offset = 0;
	action_desc[0].field_op.dst.field_string = "meta.data";
	action_desc[0].field_op.dst.bit_offset = PKT_META_BIT_OFFSET;
	action_desc[0].field_op.width = 16;

	action_descs.nb_action_desc = ARRAY_DIM(action_desc);
	action_descs.desc_array = action_desc;
	action_descs_p[0] = &action_descs;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RIA_STATUS_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, &mask.base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg,
						actions_arr,
						actions_mask_arr,
						action_descs_p,
						ARRAY_DIM(actions_arr));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, RIA_STATUS_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add one entry per (ingress port, RIA status) pair to the RIA status pipe
 *
 * @pipe [in]: pipe of the entries
 * @entries [out]: created entries, indexed by port_id * RIA_NB_STATUSES + status
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_ria_status_pipe_entries(struct doca_flow_pipe *pipe,
						struct doca_flow_pipe_entry *entries[RIA_STATUS_PIPE_ENTRIES],
						struct entries_status *status)
{
	struct custom_match_ria match;
	struct doca_flow_actions actions;
	struct doca_flow_monitor monitor;
	doca_error_t result;
	uint16_t port_id;
	uint32_t i;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	for (port_id = 0; port_id < NB_PORTS; ++port_id) {
		for (i = 0; i < RIA_NB_STATUSES; ++i) {
			uint32_t entry_idx = port_id * RIA_NB_STATUSES + i;

			match.base.parser_meta.port_id = port_id;
			match.ria_status = ria_statuses[i];
			actions.meta.pkt_meta =
				DOCA_HTOBE32(PKT_META_RIA_VALID | (ria_statuses[i] << PKT_META_RIA_STATUS_SHIFT) |
					     (port_id << PKT_META_SOURCE_PORT_SHIFT));

			result = doca_flow_pipe_basic_add_entry(0,
								pipe,
								&match.base,
								0,
								&actions,
								&monitor,
								NULL,
								0,
								status,
								&entries[entry_idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add RIA_STATUS_PIPE entry for port %u status %u: %s",
					     port_id,
					     ria_statuses[i],
					     doca_error_get_descr(result));
				return result;
			}
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create the ordered list pipe executing the RIA operations
 *
 * Each ordered list first builds the RIA tag from BTH.PSN and then runs one RIA operation.
 *
 * @port [in]: port of the pipe
 * @ria_external_actions [in]: RIA action handles, indexed like ria_entry_specs
 * @next_pipe [in]: pipe the packets are forwarded to after the RIA operation
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ria_pipe(struct doca_flow_port *port,
				    void *ria_external_actions[RIA_NB_OPERATIONS],
				    struct doca_flow_pipe *next_pipe,
				    struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd;
	struct doca_flow_action_desc action_desc[1];
	struct doca_flow_action_descs action_descs;
	struct doca_flow_ordered_list_element elements[RIA_NB_OPERATIONS][2];
	struct doca_flow_ordered_list ordered_list[RIA_NB_OPERATIONS];
	struct doca_flow_ordered_list *ordered_lists[RIA_NB_OPERATIONS];
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;
	uint32_t i;

	memset(&fwd, 0, sizeof(fwd));
	memset(&action_desc, 0, sizeof(action_desc));
	memset(&action_descs, 0, sizeof(action_descs));
	memset(elements, 0, sizeof(elements));
	memset(ordered_list, 0, sizeof(ordered_list));
	memset(ordered_lists, 0, sizeof(ordered_lists));

	/*
	 * Build the RIA tag out of the 24-bit BTH.PSN. Tag bit offsets address the 32-bit tag
	 * registers, so a single field starts at offset 0 and the resource tag_size is 4 bytes.
	 */
	action_desc[0].type = DOCA_FLOW_ACTION_COPY;
	action_desc[0].field_op.src.field_string = "outer.bth.psn";
	action_desc[0].field_op.src.bit_offset = 0;
	action_desc[0].field_op.dst.field_string = "meta.ria_tag";
	action_desc[0].field_op.dst.bit_offset = 0;
	action_desc[0].field_op.width = 24;

	action_descs.nb_action_desc = ARRAY_DIM(action_desc);
	action_descs.desc_array = action_desc;

	for (i = 0; i < RIA_NB_OPERATIONS; ++i) {
		elements[i][0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
		elements[i][0].action_descs = &action_descs;
		elements[i][1].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		elements[i][1].external_actions = ria_external_actions[i];

		ordered_list[i].idx = ria_entry_specs[i].entry_idx;
		ordered_list[i].size = ARRAY_DIM(elements[i]);
		ordered_list[i].elements = elements[i];

		ordered_lists[i] = &ordered_list[i];
	}

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = set_flow_pipe_cfg(pipe_cfg, "RIA_PIPE", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, RIA_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg number of entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, ARRAY_DIM(ordered_lists));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg ordered lists: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add one entry per ordered list to the RIA pipe
 *
 * @pipe [in]: pipe of the entries
 * @entries [out]: created entries, indexed like ria_entry_specs
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_ria_pipe_entries(struct doca_flow_pipe *pipe,
					 struct doca_flow_pipe_entry *entries[RIA_PIPE_ENTRIES],
					 struct entries_status *status)
{
	struct doca_flow_ordered_list_element elements[2];
	struct doca_flow_ordered_list ordered_list;
	doca_error_t result;
	uint32_t i;

	memset(elements, 0, sizeof(elements));
	memset(&ordered_list, 0, sizeof(ordered_list));

	elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements[1].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;

	ordered_list.size = ARRAY_DIM(elements);
	ordered_list.elements = elements;

	for (i = 0; i < RIA_NB_OPERATIONS; ++i) {
		ordered_list.idx = ria_entry_specs[i].entry_idx;

		result = doca_flow_pipe_ordered_list_add_entry(0,
							       pipe,
							       ria_entry_specs[i].entry_idx,
							       &ordered_list,
							       NULL,
							       0,
							       status,
							       &entries[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add RIA_PIPE entry for %s: %s",
				     ria_entry_specs[i].operation_str,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create the root pipe classifying RoCEv2 traffic by BTH opcode
 *
 * Packets carrying an opcode that triggers a RIA operation are forwarded to the matching ordered
 * list of the RIA pipe. Everything else misses to the plain forwarding pipe.
 *
 * @port [in]: port of the pipe
 * @ria_pipe [in]: ordered list pipe executing the RIA operations
 * @fwd_pipe [in]: pipe handling traffic that does not trigger a RIA operation
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port,
				     struct doca_flow_pipe *ria_pipe,
				     struct doca_flow_pipe *fwd_pipe,
				     struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct custom_match_ria match;
	struct custom_match_ria mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	match.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.base.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.base.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match.base.outer.roce_v2.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_ROCEV2_DEFAULT_PORT);
	match.base.outer.roce_v2.bth.opcode = UINT8_MAX;
	match.base.outer.roce_v2.bth.flags1 = UINT8_MAX;

	mask.base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	mask.base.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	mask.base.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	mask.base.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	mask.base.outer.roce_v2.udp.l4_port.dst_port = UINT16_MAX;
	mask.base.outer.roce_v2.bth.opcode = UINT8_MAX;
	mask.base.outer.roce_v2.bth.flags1 = BTH_FLAGS1_ACKREQ;

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ria_pipe;
	fwd.ordered_list_pipe.idx = UINT32_MAX;

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = fwd_pipe;

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
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, &mask.base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, ROOT_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add one entry per RIA operation to the root pipe
 *
 * @pipe [in]: pipe of the entries
 * @ria_pipe [in]: ordered list pipe executing the RIA operations
 * @entries [out]: created entries, indexed like ria_entry_specs
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_root_pipe_entries(struct doca_flow_pipe *pipe,
					  struct doca_flow_pipe *ria_pipe,
					  struct doca_flow_pipe_entry *entries[ROOT_PIPE_ENTRIES],
					  struct entries_status *status)
{
	struct custom_match_ria match;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	uint32_t i;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.base.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match.base.outer.roce_v2.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_ROCEV2_DEFAULT_PORT);

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ria_pipe;

	for (i = 0; i < RIA_NB_OPERATIONS; ++i) {
		match.base.outer.roce_v2.bth.opcode = ria_entry_specs[i].bth_opcode;
		match.base.outer.roce_v2.bth.flags1 = ria_entry_specs[i].bth_flags1;
		fwd.ordered_list_pipe.idx = ria_entry_specs[i].entry_idx;

		result =
			doca_flow_pipe_basic_add_entry(0, pipe, &match.base, 0, NULL, NULL, &fwd, 0, status, &entries[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add ROOT_PIPE entry for %s: %s",
				     ria_entry_specs[i].operation_str,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Convert a RIA execution status to a human readable string
 *
 * @ria_status [in]: RIA execution status
 * @return: string representation of the status
 */
static const char *ria_status_to_string(uint32_t ria_status)
{
	switch ((enum doca_flow_external_action_ria_status)ria_status) {
	case DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_ALLOC_NEW_POINTER:
		return "ALLOC_NEW_POINTER";
	case DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_HIT:
		return "HIT";
	case DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_MISS:
		return "MISS";
	case DOCA_FLOW_EXTERNAL_ACTION_RIA_STATUS_HW_ERROR:
		return "HW_ERROR";
	default:
		return "UNKNOWN";
	}
}

/*
 * Convert a BTH opcode to the RIA operation it triggers
 *
 * @bth_opcode [in]: BTH opcode taken from the received packet
 * @return: string representation of the operation, "NONE" if the opcode triggers no operation
 */
static const char *bth_opcode_to_operation_string(uint8_t bth_opcode)
{
	uint32_t i;

	for (i = 0; i < RIA_NB_OPERATIONS; ++i) {
		if (ria_entry_specs[i].bth_opcode == bth_opcode)
			return ria_entry_specs[i].operation_str;
	}

	return "NONE";
}

/*
 * Read a 3 byte big endian BTH field as a host order value
 *
 * @field [in]: 3 byte field, most significant byte first
 * @return: host order value of the field
 */
static uint32_t bth_24bit_to_cpu(const uint8_t field[3])
{
	return ((uint32_t)field[0] << 16) | ((uint32_t)field[1] << 8) | field[2];
}

/*
 * Locate the BTH header of a received RoCEv2 packet
 *
 * @data [in]: first byte of the received packet
 * @data_len [in]: length of the received packet
 * @return: pointer to the BTH header, NULL if the packet is not a long enough IPv4 RoCEv2 packet
 */
static const struct rte_ib_bth *packet_get_bth(const uint8_t *data, size_t data_len)
{
	const struct rte_ether_hdr *eth;
	const struct rte_ipv4_hdr *ipv4;
	const struct rte_udp_hdr *udp;
	size_t ipv4_hdr_len;
	size_t bth_offset;

	if (data_len < sizeof(*eth) + sizeof(*ipv4))
		return NULL;

	eth = (const struct rte_ether_hdr *)data;
	if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4)
		return NULL;

	ipv4 = (const struct rte_ipv4_hdr *)(eth + 1);
	if (ipv4->next_proto_id != IPPROTO_UDP)
		return NULL;

	/* Options may stretch the IPv4 header, so the UDP header offset comes from its IHL field. */
	ipv4_hdr_len = rte_ipv4_hdr_len(ipv4);
	if (ipv4_hdr_len < sizeof(*ipv4))
		return NULL;

	bth_offset = sizeof(*eth) + ipv4_hdr_len + sizeof(*udp);
	if (data_len < bth_offset + sizeof(struct rte_ib_bth))
		return NULL;

	udp = (const struct rte_udp_hdr *)((const uint8_t *)ipv4 + ipv4_hdr_len);
	if (rte_be_to_cpu_16(udp->dst_port) != DOCA_FLOW_ROCEV2_DEFAULT_PORT)
		return NULL;

	return (const struct rte_ib_bth *)(udp + 1);
}

/*
 * Release a received packet buffer back to the receive queue
 *
 * @packet [in]: received packet
 */
static void free_packet(struct doca_buf *packet)
{
	(void)doca_buf_dec_refcount(packet, NULL);
}

/*
 * Release a completed send task together with the packet it carried
 *
 * @task_send [in]: completed send task
 */
static void release_send_task(struct doca_eth_txq_task_send *task_send)
{
	struct doca_buf *packet = NULL;

	if (doca_eth_txq_task_send_get_packet(task_send, &packet) == DOCA_SUCCESS)
		free_packet(packet);

	doca_task_free(doca_eth_txq_task_send_as_task(task_send));
}

/*
 * Handle a packet that was transmitted successfully
 *
 * @task_send [in]: completed send task
 * @task_user_data [in]: user data attached to the task, unused
 * @ctx_user_data [in]: user data attached to the context, unused
 */
static void tx_success_cb(struct doca_eth_txq_task_send *task_send,
			  union doca_data task_user_data,
			  union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	release_send_task(task_send);
}

/*
 * Handle a packet that could not be transmitted
 *
 * @task_send [in]: failed send task
 * @task_user_data [in]: user data attached to the task, unused
 * @ctx_user_data [in]: user data attached to the context, unused
 */
static void tx_error_cb(struct doca_eth_txq_task_send *task_send,
			union doca_data task_user_data,
			union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	DOCA_LOG_WARN("Failed to transmit a packet, dropping it");
	release_send_task(task_send);
}

/*
 * Forward a received packet out on the peer port
 *
 * The destination port travels to the egress pipe in the send task metadata. The packet buffer is
 * released by the send task completion callback, or right here when the task cannot be submitted.
 *
 * @txq [in]: send queue of the switch proxy port
 * @packet [in]: received packet, consumed by this function
 * @egress_port [in]: port the packet is forwarded to
 */
static void forward_packet(struct doca_eth_txq *txq, struct doca_buf *packet, uint16_t egress_port)
{
	union doca_data task_user_data = {0};
	struct doca_eth_txq_task_send *task_send;
	uint32_t *metadata_array;
	doca_error_t result;

	result = doca_eth_txq_task_send_alloc_init(txq, packet, task_user_data, &task_send);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to allocate a send task for port %u, dropping the packet: %s",
			      egress_port,
			      doca_error_get_descr(result));
		free_packet(packet);
		return;
	}

	result = doca_eth_txq_task_send_get_metadata_array(task_send, &metadata_array);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to get the send task metadata, dropping the packet: %s",
			      doca_error_get_descr(result));
		doca_task_free(doca_eth_txq_task_send_as_task(task_send));
		free_packet(packet);
		return;
	}
	metadata_array[0] = egress_port;

	result = doca_task_submit(doca_eth_txq_task_send_as_task(task_send));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to submit a send task for port %u, dropping the packet: %s",
			      egress_port,
			      doca_error_get_descr(result));
		doca_task_free(doca_eth_txq_task_send_as_task(task_send));
		free_packet(packet);
	}
}

/*
 * Log a received packet and forward it out on the peer port
 *
 * @txq [in]: send queue of the switch proxy port
 * @packet [in]: received packet, consumed by this function
 * @flow_tag [in]: flow tag received with packet, will contain ingress port ID
 * @pkt_meta [in]: packet metadata, holding the RIA index and status
 */
static void process_packet(struct doca_eth_txq *txq, struct doca_buf *packet, uint32_t pkt_meta)
{
	const struct rte_ib_bth *bth;
	uint16_t ingress_port;
	uint16_t egress_port;
	uint32_t marked_port;
	doca_error_t result;
	size_t data_len;
	void *data;

	marked_port = pkt_meta_extract_source_port(pkt_meta);
	if (marked_port >= NB_PORTS) {
		DOCA_LOG_WARN("Received a packet marked with an out of range ingress port %u, dropping it",
			      marked_port);
		free_packet(packet);
		return;
	}

	ingress_port = (uint16_t)marked_port;
	egress_port = ingress_port ^ 1;

	result = doca_buf_get_data(packet, &data);
	if (result == DOCA_SUCCESS)
		result = doca_buf_get_data_len(packet, &data_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to access a received packet, dropping it: %s", doca_error_get_descr(result));
		free_packet(packet);
		return;
	}

	bth = packet_get_bth(data, data_len);
	if (bth == NULL) {
		DOCA_LOG_INFO("Port %u: non RoCEv2 packet, forwarding to port %u", ingress_port, egress_port);
	} else if (pkt_meta_is_ria_valid(pkt_meta)) {
		DOCA_LOG_INFO(
			"Port %u: opcode=0x%02x qpn=0x%06x psn=0x%06x ackreq=%u, %s status=%s ria_idx=%u, forwarding to port %u",
			ingress_port,
			bth->opcode,
			bth_24bit_to_cpu(bth->dst_qp),
			bth_24bit_to_cpu(bth->psn),
			bth->a,
			bth_opcode_to_operation_string(bth->opcode),
			ria_status_to_string(pkt_meta_extract_ria_status(pkt_meta)),
			pkt_meta_extract_ria_index(pkt_meta),
			egress_port);
	} else {
		DOCA_LOG_INFO("Port %u: opcode=0x%02x qpn=0x%06x psn=0x%06x ackreq=%u, no RIA, forwarding to port %u",
			      ingress_port,
			      bth->opcode,
			      bth_24bit_to_cpu(bth->dst_qp),
			      bth_24bit_to_cpu(bth->psn),
			      bth->a,
			      egress_port);
	}

	forward_packet(txq, packet, egress_port);
}

/*
 * Handle a batch of received packets
 *
 * In switch mode every RSS forwarded packet is delivered on the queues of the switch proxy port,
 * regardless of which port it ingressed on. The ingress port is recovered from the flow tag.
 *
 * @event_batch [in]: managed receive event batch reporting the packets
 * @packets_count [in]: number of packets the batch reports
 * @event_batch_user_data [in]: user data of the batch, holding the send queue
 * @status [in]: status of the batch, always a success for this callback
 * @pkt_array [in]: received packets, consumed by this function
 */
static void rx_success_cb(struct doca_eth_rxq_event_batch_managed_mempool_recv *event_batch,
			  uint16_t packets_count,
			  doca_error_t status,
			  struct doca_buf **pkt_array,
			  union doca_data event_batch_user_data)
{
	struct doca_eth_txq *txq = (struct doca_eth_txq *)event_batch_user_data.ptr;
	const uint32_t *metadata_array;
	doca_error_t result;
	uint16_t i;

	(void)status;

	result = doca_eth_rxq_event_batch_managed_mempool_recv_get_metadata_array(event_batch, &metadata_array);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the flow tag and metadata of a received batch, dropping %u packets: %s",
			     packets_count,
			     doca_error_get_descr(result));
		for (i = 0; i < packets_count; ++i)
			free_packet(pkt_array[i]);
		return;
	}

	for (i = 0; i < packets_count; ++i)
		process_packet(
			txq,
			pkt_array[i],
			doca_eth_rxq_event_batch_metadata_array_get_metadata(metadata_array, RIA_METADATA_NUM, i, 0));
}

/*
 * Report a failure of the receive queue
 *
 * @event_batch [in]: managed receive event batch that failed
 * @packets_count [in]: number of packets the batch reports
 * @event_batch_user_data [in]: user data of the batch, unused
 * @status [in]: status of the batch
 * @pkt_array [in]: received packets, unused
 */
static void rx_error_cb(struct doca_eth_rxq_event_batch_managed_mempool_recv *event_batch,
			uint16_t packets_count,
			doca_error_t status,
			struct doca_buf **pkt_array,
			union doca_data event_batch_user_data)
{
	(void)event_batch;
	(void)packets_count;
	(void)event_batch_user_data;
	(void)pkt_array;

	DOCA_LOG_ERR("Failed to receive packets: %s", doca_error_get_descr(status));
}

/*
 * Receive, log and forward packets until the sample is asked to stop
 *
 * @pe [in]: progress engine driving the receive and the send queue
 */
static void process_packets(struct doca_pe *pe)
{
	while (!force_quit)
		(void)doca_pe_progress(pe);

	DOCA_LOG_INFO("Stop request received, exiting the packet processing loop");
}

/*
 * Create and start the send queue the sample forwards packets with
 *
 * @dev [in]: DOCA device of the switch proxy port
 * @pe [in]: progress engine the send queue is connected to
 * @txq [out]: created send queue
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_txq(struct doca_dev *dev, struct doca_pe *pe, struct doca_eth_txq **txq)
{
	struct doca_ctx *txq_ctx;
	doca_error_t rollback_result;
	doca_error_t result;

	result = doca_eth_txq_create(dev, TXQ_NB_SEND_TASKS, txq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the send queue: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_txq_set_type(*txq, DOCA_ETH_TXQ_TYPE_REGULAR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the send queue type: %s", doca_error_get_descr(result));
		goto destroy_txq;
	}

	/* The destination port of a forwarded packet reaches the egress pipe in this metadata. */
	result = doca_eth_txq_set_metadata_num(*txq, RIA_METADATA_NUM);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the send queue metadata number: %s", doca_error_get_descr(result));
		goto destroy_txq;
	}

	result = doca_eth_txq_task_send_set_conf(*txq, tx_success_cb, tx_error_cb, TXQ_NB_SEND_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure the send tasks: %s", doca_error_get_descr(result));
		goto destroy_txq;
	}

	txq_ctx = doca_eth_txq_as_ctx(*txq);
	if (txq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to get the context of the send queue");
		result = DOCA_ERROR_BAD_STATE;
		goto destroy_txq;
	}

	result = doca_pe_connect_ctx(pe, txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect the send queue to the progress engine: %s",
			     doca_error_get_descr(result));
		goto destroy_txq;
	}

	result = doca_ctx_start(txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start the send queue: %s", doca_error_get_descr(result));
		goto destroy_txq;
	}

	result = doca_eth_txq_apply_queue_id(*txq, TXQ_QUEUE_ID);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply the send queue id: %s", doca_error_get_descr(result));
		(void)doca_ctx_stop(txq_ctx);
		goto destroy_txq;
	}

	return DOCA_SUCCESS;

destroy_txq:
	rollback_result = doca_eth_txq_destroy(*txq);
	if (rollback_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy the send queue on rollback: %s", doca_error_get_descr(rollback_result));
	*txq = NULL;
	return result;
}

/*
 * Stop and destroy the send queue
 *
 * Stopping is asynchronous while send tasks are still in flight, so the progress engine is driven
 * until the context goes idle and every packet buffer has been released.
 *
 * @txq [in]: send queue to destroy, may be NULL
 * @pe [in]: progress engine the send queue is connected to
 */
static void destroy_txq(struct doca_eth_txq *txq, struct doca_pe *pe)
{
	enum doca_ctx_states state;
	struct doca_ctx *txq_ctx;
	doca_error_t result;

	if (txq == NULL)
		return;

	txq_ctx = doca_eth_txq_as_ctx(txq);
	if (txq_ctx != NULL && doca_ctx_stop(txq_ctx) == DOCA_ERROR_IN_PROGRESS) {
		do {
			(void)doca_pe_progress(pe);
		} while (doca_ctx_get_state(txq_ctx, &state) == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE);
	}

	result = doca_eth_txq_destroy(txq);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy the send queue: %s", doca_error_get_descr(result));
}

/*
 * Create the egress root pipe forwarding transmitted packets to their destination port
 *
 * A packet the application transmits enters the switch in the egress domain, which is rooted in
 * this pipe, so it is never classified by the RIA pipeline again. The destination port is the
 * packet metadata the send task carried.
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_egress_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct custom_match_ria match;
	struct custom_match_ria mask;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&fwd, 0, sizeof(fwd));

	/* Destination port is changeable per entry. */
	match.base.meta.pkt_meta = UINT32_MAX;
	mask.base.meta.pkt_meta = UINT32_MAX;

	/* Placeholder, every entry overrides the destination port. */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = UINT16_MAX;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "EGRESS_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, &mask.base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, EGRESS_PIPE_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add one entry per destination port to the egress pipe
 *
 * @pipe [in]: pipe of the entries
 * @entries [out]: created entries, indexed by destination port
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_egress_pipe_entries(struct doca_flow_pipe *pipe,
					    struct doca_flow_pipe_entry *entries[EGRESS_PIPE_ENTRIES],
					    struct entries_status *status)
{
	struct custom_match_ria match;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	uint16_t port_id;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	fwd.type = DOCA_FLOW_FWD_PORT;

	for (port_id = 0; port_id < EGRESS_PIPE_ENTRIES; ++port_id) {
		match.base.meta.pkt_meta = DOCA_HTOBE32(port_id);
		fwd.port_id = port_id;

		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match.base,
							0,
							NULL,
							NULL,
							&fwd,
							0,
							status,
							&entries[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add EGRESS_PIPE entry for port %u: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize DOCA Flow with the custom field definitions the sample needs
 *
 * Registers the RIA execution status as a match field.
 * Without the RIA syndrome definition, matching the result of a RIA operation is disabled.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @resource [in]: DOCA Flow resource configuration
 * @nr_shared_resources [in]: shared resource configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_doca_flow_with_ria_defs(int nb_queues,
						 struct flow_resources *resource,
						 uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES])
{
	struct doca_flow_definitions_cfg *defs_cfg;
	struct doca_flow_definitions *defs;
	doca_error_t cleanup_result;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions config: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_flow_definitions_create(defs_cfg, &defs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions: %s", doca_error_get_descr(result));
		goto destroy_defs_cfg;
	}
	result = doca_flow_definitions_add_field(defs,
						 "match.packet.parser_meta.ria.syndrome",
						 RIA_STATUS_OFF,
						 RIA_STATUS_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add RIA syndrome definition: %s", doca_error_get_descr(result));
		goto destroy_defs;
	}

	result = init_doca_flow_with_defs(nb_queues, "switch", resource, nr_shared_resources, defs);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));

destroy_defs:
	doca_flow_definitions_destroy(defs);
destroy_defs_cfg:
	cleanup_result = doca_flow_definitions_cfg_destroy(defs_cfg);
	if (cleanup_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy definitions config: %s", doca_error_get_descr(cleanup_result));
	return result;
}

/*
 * Print the counters of every classification entry
 *
 * @ria_status_entries [in]: RIA status pipe entries, indexed by port_id * RIA_NB_STATUSES + status
 * @fwd_entries [in]: forwarding pipe entries, indexed by ingress port
 */
static void print_stats(struct doca_flow_pipe_entry *ria_status_entries[RIA_STATUS_PIPE_ENTRIES],
			struct doca_flow_pipe_entry *fwd_entries[FWD_PIPE_ENTRIES])
{
	struct doca_flow_resource_query query;
	doca_error_t result;
	uint16_t port_id;
	uint32_t i;

	for (port_id = 0; port_id < NB_PORTS; ++port_id) {
		for (i = 0; i < RIA_NB_STATUSES; ++i) {
			result = doca_flow_resource_query_entry(ria_status_entries[port_id * RIA_NB_STATUSES + i],
								&query);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query RIA_STATUS_PIPE entry: %s", doca_error_get_descr(result));
				return;
			}
			DOCA_LOG_INFO("Port %u RIA status %s: total_pkts=%" PRIu64 " total_bytes=%" PRIu64,
				      port_id,
				      ria_status_to_string(ria_statuses[i]),
				      query.counter.total_pkts,
				      query.counter.total_bytes);
		}

		result = doca_flow_resource_query_entry(fwd_entries[port_id], &query);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query FWD_PIPE entry: %s", doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Port %u no RIA: total_pkts=%" PRIu64 " total_bytes=%" PRIu64,
			      port_id,
			      query.counter.total_pkts,
			      query.counter.total_bytes);
	}
}

/*
 * Run flow_ria sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @devs_manager [in]: array of DOCA devices for the switch ports
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_ria(int nb_queues, int nb_ports, struct flow_devs_manager devs_manager[], struct flow_switch_ctx *ctx)
{
	const uint32_t num_of_entries =
		ROOT_PIPE_ENTRIES + RIA_PIPE_ENTRIES + RIA_STATUS_PIPE_ENTRIES + FWD_PIPE_ENTRIES + EGRESS_PIPE_ENTRIES;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_external_resource_ria_cfg ria_resource_cfg = {0};
	struct doca_flow_external_resource_ria *ria_resource = NULL;
	struct doca_flow_external_action_ria_pipe_action_cfg ria_action_cfg = {0};
	void *ria_actions[RIA_NB_OPERATIONS] = {0};
	struct flow_eth_common_dev_context *dev_ctx = NULL;
	struct flow_eth_common_rx_cfg eth_cfg = {0};
	struct doca_flow_pipe *fwd_pipe = NULL;
	struct doca_flow_pipe *ria_status_pipe = NULL;
	struct doca_flow_pipe *ria_pipe = NULL;
	struct doca_flow_pipe *root_pipe = NULL;
	struct doca_flow_pipe *egress_pipe = NULL;
	struct doca_flow_pipe_entry *fwd_entries[FWD_PIPE_ENTRIES];
	struct doca_flow_pipe_entry *ria_status_entries[RIA_STATUS_PIPE_ENTRIES];
	struct doca_flow_pipe_entry *ria_entries[RIA_PIPE_ENTRIES];
	struct doca_flow_pipe_entry *root_entries[ROOT_PIPE_ENTRIES];
	struct doca_flow_pipe_entry *egress_entries[EGRESS_PIPE_ENTRIES];
	struct doca_flow_port *ports[NB_PORTS];
	uint32_t actions_mem_size[NB_PORTS];
	struct doca_eth_txq *txq = NULL;
	union doca_data rx_user_data;
	struct doca_flow_port *switch_port;
	struct entries_status status;
	struct doca_pe *pe = NULL;
	doca_error_t result;
	uint32_t i;

	if (nb_ports != NB_PORTS) {
		DOCA_LOG_ERR("Sample requires exactly %d ports, got %d", NB_PORTS, nb_ports);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (nb_queues != NB_QUEUES) {
		DOCA_LOG_ERR("Sample requires exactly %d queue, got %d", NB_QUEUES, nb_queues);
		return DOCA_ERROR_INVALID_VALUE;
	}

	memset(&status, 0, sizeof(status));

	/* The receive and the send queue share one progress engine, the sample is single threaded. */
	result = doca_pe_create(&pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = create_txq(devs_manager[0].doca_dev, pe, &txq);
	if (result != DOCA_SUCCESS)
		goto destroy_pe;

	/*
	 * The receive queues carry the flow tag and one metadata per packet, which is how the
	 * ingress port, the RIA status and the RIA index reach this sample.
	 */
	rx_user_data.ptr = txq;
	flow_eth_common_set_dev_cfg(nb_queues, true, rx_user_data, rx_success_cb, rx_error_cb, &eth_cfg);
	result = flow_eth_common_create_dev_resources(devs_manager[0].doca_dev, pe, &eth_cfg, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA ETH resources: %s", doca_error_get_descr(result));
		goto stop_txq;
	}

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = RIA_STATUS_PIPE_ENTRIES + FWD_PIPE_ENTRIES;
	result = init_doca_flow_with_ria_defs(nb_queues, &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS)
		goto stop_txq;

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_switch_ports(devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto destroy_flow;
	}

	switch_port = doca_flow_port_switch_get(ports[0]);

	/*
	 * A RIA table is bound to the device context of the port it is created on. Creating it on
	 * the switch proxy port makes it usable by every pipe of this sample, which is what lets a
	 * DEALLOC release the index a preceding ALLOC created on the opposite direction.
	 */
	ria_resource_cfg.tag_size = 4;
	ria_resource_cfg.index_base = 0;
	ria_resource_cfg.num_of_indexes = RIA_NUM_INDEXES;
	result = doca_flow_external_resource_ria_create(&ria_resource_cfg, switch_port, &ria_resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RIA resource: %s", doca_error_get_descr(result));
		goto stop_ports;
	}

	ria_action_cfg.resource = ria_resource;
	for (i = 0; i < RIA_NB_OPERATIONS; ++i) {
		ria_action_cfg.operation = ria_entry_specs[i].operation;
		result = doca_flow_external_action_ria_pipe_action_create(&ria_action_cfg, &ria_actions[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create RIA %s action: %s",
				     ria_entry_specs[i].operation_str,
				     doca_error_get_descr(result));
			goto destroy_ria_actions;
		}
	}

	/* Create RSS pipe for traffic that does not go through RIA action. */
	result = create_fwd_pipe(switch_port, &fwd_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create FWD_PIPE: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}
	result = add_fwd_pipe_entries(fwd_pipe, fwd_entries, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create FWD_PIPE entries: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}

	/* Create RSS pipe for traffic on which RIA action was executed and report status/index through pkt_meta. */
	result = create_ria_status_pipe(switch_port, &ria_status_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RIA_STATUS_PIPE: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}
	result = add_ria_status_pipe_entries(ria_status_pipe, ria_status_entries, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RIA_STATUS_PIPE entries: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}

	/* Create pipe for executing RIA ALLOC/DEALLOC operations. */
	result = create_ria_pipe(switch_port, ria_actions, ria_status_pipe, &ria_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RIA_PIPE: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}
	result = add_ria_pipe_entries(ria_pipe, ria_entries, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RIA_PIPE entries: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}

	/* Create root pipe which forwards WRITE with AckReq or ACKNOWLEDGE packets to RIA. */
	result = create_root_pipe(switch_port, ria_pipe, fwd_pipe, &root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ROOT_PIPE: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}
	result = add_root_pipe_entries(root_pipe, ria_pipe, root_entries, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ROOT_PIPE entries: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}

	/* Create egress root pipe which sends the packets this sample transmits to their port. */
	result = create_egress_pipe(switch_port, &egress_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create EGRESS_PIPE: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}
	result = add_egress_pipe_entries(egress_pipe, egress_entries, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create EGRESS_PIPE entries: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}

	result = flow_process_entries(switch_port, &status, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		goto destroy_pipes;
	}

	DOCA_LOG_INFO("RIA tag is the 24-bit BTH PSN, RIA table holds up to %d indexes", RIA_NUM_INDEXES);
	for (i = 0; i < RIA_NB_OPERATIONS; ++i)
		DOCA_LOG_INFO("RoCEv2 packets with BTH opcode 0x%02x and AckReq %u trigger the %s operation",
			      ria_entry_specs[i].bth_opcode,
			      !!(ria_entry_specs[i].bth_flags1 & BTH_FLAGS1_ACKREQ),
			      ria_entry_specs[i].operation_str);
	DOCA_LOG_INFO("Forwarding RoCEv2 traffic between the two ports. Press Ctrl+C to stop.");

	force_quit = 0;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	process_packets(pe);

	print_stats(ria_status_entries, fwd_entries);

destroy_pipes:
	doca_flow_pipe_destroy(egress_pipe);
	doca_flow_pipe_destroy(root_pipe);
	doca_flow_pipe_destroy(ria_pipe);
	doca_flow_pipe_destroy(ria_status_pipe);
	doca_flow_pipe_destroy(fwd_pipe);
destroy_ria_actions:
	for (i = 0; i < RIA_NB_OPERATIONS; ++i) {
		if (ria_actions[i] != NULL)
			doca_flow_external_action_ria_pipe_action_destroy(ria_actions[i]);
	}
	doca_flow_external_resource_ria_destroy(ria_resource);
stop_ports:
	stop_doca_flow_ports(nb_ports, ports);
destroy_flow:
	doca_flow_destroy();
stop_txq:
	/* Drains the send tasks still in flight, so it must run before the receive queues are gone. */
	destroy_txq(txq, pe);
	flow_eth_common_destroy_dev_resources(dev_ctx);
destroy_pe:
	doca_pe_destroy(pe);
	return result;
}
