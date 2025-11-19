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
#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_PARSER_META);

#define MAX_ENTRIES 250
#define NUMBER_OF_PORTS 2

/*
 * Flow Classification Table
 * ------------------------
 * This table defines all possible packet flows and their handling rules.
 * Each row represents a unique flow pattern with its associated actions.
 *
 * Priority ordering (lower number = higher priority):
 * 1. Valid flows (forward) - Normal packet processing - Highest priority
 * 2. Fragmented flows (RSS) - Need special handling - Medium priority
 * 3. Invalid checksum flows (drop) - Discard packets with incorrect checksum
 * 4. Invalid length flows (drop) - Discard packets with invalid length
 */
enum integrity_flow_priority {
	FLOW_PRIORITY_VALID = 0,
	FLOW_PRIORITY_FRAGMENTED = 1,
	FLOW_PRIORITY_INVALID_CSUM = 2,
	FLOW_PRIORITY_NOT_OK = 3,
};

/*
 * Create DOCA Flow control pipe with parser_meta checks
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_parser_meta_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "PARSER_META_PIPE", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/* Structure to hold packet attributes */
struct packet_attrs {
	enum doca_flow_l3_meta l3_type;
	enum doca_flow_l4_meta l4_type;
	uint8_t ip_fragmented;
	uint8_t l3_ok;
	uint8_t ip4_checksum_ok;
	uint8_t l4_ok;
	uint8_t l4_checksum_ok;
	char description[256];
};

/* Structure to hold a complete flow */
struct flow_entry {
	char description[512];
	struct packet_attrs outer;
	struct packet_attrs inner;
	bool has_inner;
	uint32_t entry_index;
};

/* Main structure to hold all flows */
struct flow_database {
	struct flow_entry *flows;
	size_t count;
	size_t capacity;
};

/* Global flow database array - one per port */
static struct flow_database g_flow_db[NUMBER_OF_PORTS];

/* Flow table entries */
static const struct packet_attrs base_packet_attrs[] = {
	/* Basic flows */
	{.l3_type = DOCA_FLOW_L3_META_NONE,
	 .l4_type = DOCA_FLOW_L4_META_NONE,
	 .ip_fragmented = 0,
	 .l3_ok = 0,
	 .ip4_checksum_ok = 0,
	 .l4_ok = 0,
	 .l4_checksum_ok = 0,
	 .description = "VALID L2"},
	{.l3_type = DOCA_FLOW_L3_META_IPV4,
	 .l4_type = DOCA_FLOW_L4_META_TCP,
	 .ip_fragmented = 0,
	 .l3_ok = 1,
	 .ip4_checksum_ok = 1,
	 .l4_ok = 1,
	 .l4_checksum_ok = 1,
	 .description = "VALID IPV4 TCP"},
	{.l3_type = DOCA_FLOW_L3_META_IPV4,
	 .l4_type = DOCA_FLOW_L4_META_UDP,
	 .ip_fragmented = 0,
	 .l3_ok = 1,
	 .ip4_checksum_ok = 1,
	 .l4_ok = 1,
	 .l4_checksum_ok = 1,
	 .description = "VALID IPV4 UDP"},
	{.l3_type = DOCA_FLOW_L3_META_IPV4,
	 .l4_type = DOCA_FLOW_L4_META_NONE,
	 .ip_fragmented = 0,
	 .l3_ok = 1,
	 .ip4_checksum_ok = 1,
	 .l4_ok = 0,
	 .l4_checksum_ok = 0,
	 .description = "VALID IPV4"},
	{.l3_type = DOCA_FLOW_L3_META_IPV6,
	 .l4_type = DOCA_FLOW_L4_META_TCP,
	 .ip_fragmented = 0,
	 .l3_ok = 1,
	 .ip4_checksum_ok = 0,
	 .l4_ok = 1,
	 .l4_checksum_ok = 1,
	 .description = "VALID IPV6 TCP"},
	{.l3_type = DOCA_FLOW_L3_META_IPV6,
	 .l4_type = DOCA_FLOW_L4_META_UDP,
	 .ip_fragmented = 0,
	 .l3_ok = 1,
	 .ip4_checksum_ok = 0,
	 .l4_ok = 1,
	 .l4_checksum_ok = 1,
	 .description = "VALID IPV6 UDP"},
	{.l3_type = DOCA_FLOW_L3_META_IPV6,
	 .l4_type = DOCA_FLOW_L4_META_NONE,
	 .ip_fragmented = 0,
	 .l3_ok = 1,
	 .ip4_checksum_ok = 0,
	 .l4_ok = 0,
	 .l4_checksum_ok = 0,
	 .description = "VALID IPV6"},
};

/*
 * Check if the flow is valid for a tunnel.
 *
 * A flow is valid for a tunnel if it is not TCP.
 *
 * @param flow The flow entry to check.
 * @return True if the flow is valid for a tunnel, false otherwise.
 */
static bool valid_for_tunnel(const struct packet_attrs *flow)
{
	return flow->l4_type != DOCA_FLOW_L4_META_TCP && flow->l3_type != DOCA_FLOW_L3_META_NONE;
}

/* Initialize the flow database with your existing flows */
static doca_error_t init_flow_database(int number_of_ports, int max_entries)
{
	/* Initialize the flow database for each port with fixed capacity */
	for (int i = 0; i < number_of_ports; i++) {
		g_flow_db[i].flows = calloc(max_entries, sizeof(struct flow_entry));
		if (g_flow_db[i].flows == NULL) {
			/* Clean up any previously allocated databases */
			for (int j = 0; j < i; j++) {
				free(g_flow_db[j].flows);
				g_flow_db[j].flows = NULL;
			}
			return DOCA_ERROR_NO_MEMORY;
		}
		g_flow_db[i].count = 0;
		g_flow_db[i].capacity = max_entries;
	}

	DOCA_LOG_INFO("Initialized flow databases for %d ports", number_of_ports);
	return DOCA_SUCCESS;
}

/* Add a flow to the database */
static doca_error_t flow_database_add(struct flow_database *db, struct flow_entry *flow)
{
	assert(db->count <= db->capacity);
	if (db->count == db->capacity) {
		DOCA_LOG_ERR("Flow database is full");
		return DOCA_ERROR_NO_MEMORY;
	}

	db->flows[db->count] = *flow;
	flow->entry_index = db->count;
	db->count++;

	DOCA_LOG_INFO("Added flow %d: %s", flow->entry_index, flow->description);
	DOCA_LOG_INFO("\tOuter: L3=%d, L4=%d, Frag=%d, L3_OK=%d, L4_OK=%d, IPV4_CSUM=%d, L4_CSUM=%d",
		      flow->outer.l3_type,
		      flow->outer.l4_type,
		      flow->outer.ip_fragmented,
		      flow->outer.l3_ok,
		      flow->outer.l4_ok,
		      flow->outer.ip4_checksum_ok,
		      flow->outer.l4_checksum_ok);
	if (flow->has_inner) {
		DOCA_LOG_INFO("\tInner: L3=%d, L4=%d, Frag=%d, L3_OK=%d, L4_OK=%d, IPV4_CSUM=%d, L4_CSUM=%d",
			      flow->inner.l3_type,
			      flow->inner.l4_type,
			      flow->inner.ip_fragmented,
			      flow->inner.l3_ok,
			      flow->inner.l4_ok,
			      flow->inner.ip4_checksum_ok,
			      flow->inner.l4_checksum_ok);
	}

	return DOCA_SUCCESS;
}

/* Get a flow by its entry index */
static struct flow_entry *flow_database_get_by_index(struct flow_database *db, uint32_t index)
{
	assert(db->count <= db->capacity);
	if (index >= db->count) {
		DOCA_LOG_ERR("Flow index out of bounds: index %u, count %zu", index, db->count);
		return NULL;
	}
	return &db->flows[index];
}

/* Destroy the flow database and free allocated memory */
static void flow_database_destroy(struct flow_database *db)
{
	if (db == NULL) {
		return;
	}

	/* Free the flows array if it exists */
	if (db->flows != NULL) {
		free(db->flows);
		db->flows = NULL;
	}

	/* Reset the database state */
	db->count = 0;
	db->capacity = 0;
}

/* Destroy all flow databases */
static void flow_database_destroy_all(void)
{
	for (int i = 0; i < NUMBER_OF_PORTS; i++) {
		flow_database_destroy(&g_flow_db[i]);
	}
}

/*
 * Add DOCA Flow pipe entries for flow table combinations
 * Creates entries for the following flow types (index 0-30):
 * - Index 0: L2
 * - Index 1: IPv4/TCP
 * - Index 2: IPv4/UDP
 * - Index 3: IPv4
 * - Index 4: IPv6/TCP
 * - Index 5: IPv6/UDP
 * - Index 6: IPv6
 * - Index 7: IPv4/UDP/IPv4/TCP
 * - Index 8: IPv4/UDP/IPv4/UDP
 * - Index 9: IPv4/UDP/IPv4
 * - Index 10: IPv4/UDP/IPv6/TCP
 * - Index 11: IPv4/UDP/IPv6/UDP
 * - Index 12: IPv4/UDP/IPv6
 * - Index 13: IPv4/IPv4/TCP
 * - Index 14: IPv4/IPv4/UDP
 * - Index 15: IPv4/IPv4
 * - Index 16: IPv4/IPv6/TCP
 * - Index 17: IPv4/IPv6/UDP
 * - Index 18: IPv4/IPv6
 * - Index 19: IPv6/UDP/IPv4/TCP
 * - Index 20: IPv6/UDP/IPv4/UDP
 * - Index 21: IPv6/UDP/IPv4
 * - Index 22: IPv6/UDP/IPv6/TCP
 * - Index 23: IPv6/UDP/IPv6/UDP
 * - Index 24: IPv6/UDP/IPv6
 * - Index 25: IPv6/IPv4/TCP
 * - Index 26: IPv6/IPv4/UDP
 * - Index 27: IPv6/IPv4
 * - Index 28: IPv6/IPv6/TCP
 * - Index 29: IPv6/IPv6/UDP
 * - Index 30: IPv6/IPv6
 *
 * @pipe [in]: pipe of the entry
 * @port_id [in]: port ID of the pipe
 * @status [in]: user context for adding entry
 * @entry [out]: pointer to store the first entry
 * @match_mask [in]: match mask for the entries
 * @fwd [in]: forward configuration
 * @monitor [in]: monitor configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_valid_flows_table_entries(struct doca_flow_pipe *pipe,
						  int port_id,
						  struct entries_status *status,
						  struct doca_flow_pipe_entry *(*entries)[MAX_ENTRIES])
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	uint8_t priority = FLOW_PRIORITY_VALID;
	doca_error_t result;
	int entry_idx = g_flow_db[port_id].count;

	/* Initialize match mask for all fields we want to check */
	memset(&match_mask, 0, sizeof(match_mask));
	match_mask.parser_meta = (struct doca_flow_parser_meta){
		.outer_l3_type = UINT32_MAX,
		.outer_l4_type = UINT32_MAX,
		.outer_ip_fragmented = UINT8_MAX,
		.outer_l3_ok = UINT8_MAX,
		.outer_ip4_checksum_ok = UINT8_MAX,
		.outer_l4_ok = UINT8_MAX,
		.outer_l4_checksum_ok = UINT8_MAX,
		.inner_l3_type = UINT32_MAX,
		.inner_l4_type = UINT32_MAX,
		.inner_ip_fragmented = UINT8_MAX,
		.inner_l3_ok = UINT8_MAX,
		.inner_ip4_checksum_ok = UINT8_MAX,
		.inner_l4_ok = UINT8_MAX,
		.inner_l4_checksum_ok = UINT8_MAX,
	};

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	/* Add entries for each flow in base_packet_attrs */
	const size_t base_packet_attrs_size = sizeof(base_packet_attrs) / sizeof(base_packet_attrs[0]);

	/* create entries for non-tunnel flows */
	for (size_t outer_idx = 0; outer_idx < base_packet_attrs_size; outer_idx++) {
		struct packet_attrs packet = base_packet_attrs[outer_idx];
		match.parser_meta = (struct doca_flow_parser_meta){
			.outer_l3_type = packet.l3_type,
			.outer_l4_type = packet.l4_type,
			.outer_ip_fragmented = packet.ip_fragmented,
			.outer_l3_ok = packet.l3_ok,
			.outer_ip4_checksum_ok = packet.ip4_checksum_ok,
			.outer_l4_ok = packet.l4_ok,
			.outer_l4_checksum_ok = packet.l4_checksum_ok,
		};

		/* Build flow entry and add to database */
		struct flow_entry new_flow = {.outer = packet, .has_inner = false, .entry_index = entry_idx};
		strncpy(new_flow.description, packet.description, sizeof(new_flow.description) - 1);
		new_flow.description[sizeof(new_flow.description) - 1] = '\0';

		result = flow_database_add(&g_flow_db[port_id], &new_flow);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add flow to database for port %d: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}

		/* Initialize monitor with unique counter for this entry */
		memset(&monitor, 0, sizeof(monitor));
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

		result = doca_flow_pipe_control_add_entry(0,
							  priority,
							  pipe,
							  &match,
							  &match_mask,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &monitor,
							  &fwd,
							  status,
							  &((*entries)[entry_idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry for flow %zu (%s), %s",
				     outer_idx,
				     base_packet_attrs[outer_idx].description,
				     doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_INFO("Added entry for flow %zu (%s) - L3: %d, L4: %d, Frag: %d, L3_OK: %d, L4_OK: %d",
			      outer_idx,
			      base_packet_attrs[outer_idx].description,
			      base_packet_attrs[outer_idx].l3_type,
			      base_packet_attrs[outer_idx].l4_type,
			      base_packet_attrs[outer_idx].ip_fragmented,
			      base_packet_attrs[outer_idx].l3_ok,
			      base_packet_attrs[outer_idx].l4_ok);
		entry_idx++;
	}

	for (size_t outer_idx = 0; outer_idx < base_packet_attrs_size; outer_idx++) {
		/* Outer packet loop */
		for (size_t inner_idx = 1; inner_idx < base_packet_attrs_size; inner_idx++) {
			memset(&match, 0, sizeof(match));

			if (!valid_for_tunnel(&base_packet_attrs[outer_idx]))
				continue;

			struct packet_attrs outer_packet = base_packet_attrs[outer_idx];
			struct packet_attrs inner_packet = base_packet_attrs[inner_idx];
			/* Set outer and inner match fields based on base_packet_attrs entries */
			match.parser_meta = (struct doca_flow_parser_meta){
				.outer_l3_type = outer_packet.l3_type,
				.outer_l4_type = outer_packet.l4_type,
				.outer_ip_fragmented = outer_packet.ip_fragmented,
				.outer_l3_ok = outer_packet.l3_ok,
				.outer_ip4_checksum_ok = outer_packet.ip4_checksum_ok,
				.outer_l4_ok = outer_packet.l4_ok,
				.outer_l4_checksum_ok = outer_packet.l4_checksum_ok,
				.inner_l3_type = inner_packet.l3_type,
				.inner_l4_type = inner_packet.l4_type,
				.inner_ip_fragmented = inner_packet.ip_fragmented,
				.inner_l3_ok = inner_packet.l3_ok,
				.inner_ip4_checksum_ok = inner_packet.ip4_checksum_ok,
				.inner_l4_ok = inner_packet.l4_ok,
				.inner_l4_checksum_ok = inner_packet.l4_checksum_ok,
			};

			struct flow_entry new_flow_entry = {.outer = outer_packet,
							    .inner = inner_packet,
							    .has_inner = true,
							    .entry_index = entry_idx};

			/* Build flow entry and add to database */
			snprintf(new_flow_entry.description,
				 sizeof(new_flow_entry.description),
				 "%s/%s",
				 outer_packet.description,
				 inner_packet.description);

			/* Add flow to both ports' databases */
			result = flow_database_add(&g_flow_db[port_id], &new_flow_entry);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add flow to database for port %d: %s",
					     port_id,
					     doca_error_get_descr(result));
				return result;
			}

			/* Initialize monitor with unique counter for this entry */
			memset(&monitor, 0, sizeof(monitor));
			monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

			/* Add entry to pipe */
			result = doca_flow_pipe_control_add_entry(0,
								  priority,
								  pipe,
								  &match,
								  &match_mask,
								  NULL,
								  NULL,
								  NULL,
								  NULL,
								  &monitor,
								  &fwd,
								  status,
								  &((*entries)[entry_idx])); /* Only store first entry
											      */
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add entry for outer flow %zu (%s) and inner flow %zu (%s): %s",
					     outer_idx,
					     base_packet_attrs[outer_idx].description,
					     inner_idx,
					     base_packet_attrs[inner_idx].description,
					     doca_error_get_descr(result));
				return result;
			}
			entry_idx++;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * New function: add_fragmented_flows_table_entries
 *
 * @pipe [in]: pipe of the entry
 * @port_id [in]: port ID of the pipe
 * @status [in]: user context for adding entry
 * @entry [out]: pointer to store the first entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_fragmented_flows_table_entries(struct doca_flow_pipe *pipe,
						       int port_id,
						       struct entries_status *status,
						       struct doca_flow_pipe_entry *(*entries)[MAX_ENTRIES])
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	uint8_t priority = FLOW_PRIORITY_FRAGMENTED;
	doca_error_t result;

	/* Initialize match mask for all fields */
	memset(&match_mask, 0, sizeof(match_mask));
	match_mask.parser_meta.outer_ip_fragmented = UINT8_MAX;
	match_mask.parser_meta.inner_ip_fragmented = UINT8_MAX;

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	uint16_t rss_queues[1];
	rss_queues[0] = 0;
	fwd.rss.nr_queues = 1;
	fwd.rss.queues_array = rss_queues;

	/* Priority 1, Entry 0: Fragmented packet - Outer fragmented = 1, Inner fragmented = 0 */
	memset(&match, 0, sizeof(match));
	match.parser_meta.outer_ip_fragmented = 1;
	match.parser_meta.inner_ip_fragmented = 0;

	struct flow_entry flow_entry0;
	memset(&flow_entry0, 0, sizeof(flow_entry0));
	snprintf(flow_entry0.description, sizeof(flow_entry0.description), "Fragmented packet");
	flow_entry0.outer.ip_fragmented = 1;
	flow_entry0.inner.ip_fragmented = 0;

	memset(&monitor, 0, sizeof(monitor));
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  pipe,
						  &match,
						  &match_mask,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  &fwd,
						  status,
						  &((*entries)[g_flow_db[port_id].count]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry for fragmented flow_0: %s", doca_error_get_descr(result));
		return result;
	}

	result = flow_database_add(&g_flow_db[port_id], &flow_entry0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add fragmented flow_0 to database");
		return result;
	}

	/* Priority 1, Entry 1: Fragmented inner packet - Outer fragmented = 0, Inner fragmented = 1 */
	memset(&match, 0, sizeof(match));
	match.parser_meta.outer_ip_fragmented = 0;
	match.parser_meta.inner_ip_fragmented = 1;

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	static uint16_t rss_queues_inner[1];
	rss_queues_inner[0] = port_id;
	fwd.rss.nr_queues = 1;
	fwd.rss.queues_array = rss_queues_inner;

	struct flow_entry flow_entry1;
	memset(&flow_entry1, 0, sizeof(flow_entry1));
	snprintf(flow_entry1.description, sizeof(flow_entry1.description), "Fragmented inner packet");
	flow_entry1.outer.ip_fragmented = 0;
	flow_entry1.inner.ip_fragmented = 1;
	flow_entry1.has_inner = true;

	memset(&monitor, 0, sizeof(monitor));
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  pipe,
						  &match,
						  &match_mask,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  &fwd,
						  status,
						  &((*entries)[g_flow_db[port_id].count]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry for fragmented flow_1: %s", doca_error_get_descr(result));
		return result;
	}

	result = flow_database_add(&g_flow_db[port_id], &flow_entry1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add fragmented flow_1 to database");
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * New function: add_invalid_csum_flows_table_entries
 * Adds DOCA Flow pipe entries for invalid flows due to checksum errors
 * using the existing packet_attrs and flow_entry structures.
 * Priority is 2 and fwd action is DROP.
 */
static doca_error_t add_invalid_csum_flows_table_entries(struct doca_flow_pipe *pipe,
							 int port_id,
							 struct entries_status *status,
							 struct doca_flow_pipe_entry *(*entries)[MAX_ENTRIES])
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	uint8_t priority = FLOW_PRIORITY_INVALID_CSUM;
	doca_error_t result;

	/* Define an array of invalid flow entries using packet_attrs and flow_entry.
	 * For each entry, the outer field is filled according to the table.
	 * If an inner flow is provided (i.e. not NONE), we fill the inner attributes and set has_inner to true.
	 */
	static struct flow_entry invalid_entries[] = {
		/* invalid_flow_0: IPV4: IPV4 checksum error */
		{.description = "IPV4: IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_1: IPV4/TCP: IPV4 checksum error */
		{.description = "IPV4/TCP: IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = false},
		/* invalid_flow_2: IPV4/UDP: IPV4 checksum error */
		{.description = "IPV4/UDP: IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = false},
		/* invalid_flow_3: IPV4/TCP: IPV4 and TCP checksum error */
		{.description = "IPV4/TCP: IPV4 and TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_4: IPV4/UDP: IPV4 and UDP checksum error */
		{.description = "IPV4/UDP: IPV4 and UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_5: IPV4/TCP: TCP checksum error */
		{.description = "IPV4/TCP: TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_6: IPV4/UDP: UDP checksum error */
		{.description = "IPV4/UDP: UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_7: IPV6/TCP: TCP checksum error */
		{.description = "IPV6/TCP: TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_8: IPV6/UDP: UDP checksum error */
		{.description = "IPV6/UDP: UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = false},
		/* invalid_flow_9: IPV4/UDP/IPV4: Outer IPV4 and outer UDP and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4: Outer IPV4 and outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_10: IPV4/UDP/IPV4: Outer IPV4 and outer UDP checksum error */
		{.description = "IPV4/UDP/IPV4: Outer IPV4 and outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_11: IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP and inner IPV4 and inner TCP checksum
		   error */
		{.description =
			 "IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP and inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_12: IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_13: IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_14: IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 and outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_15: IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP and inner IPV4 and inner UDP checksum
		   error */
		{.description =
			 "IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP and inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_16: IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_17: IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_18: IPV4/UDP/IPV4/UDP: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 and outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_19: IPV4/UDP/IPV6: Outer IPV4 and outer UDP checksum error */
		{.description = "IPV4/UDP/IPV6: Outer IPV4 and outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_20: IPV4/UDP/IPV6/TCP: Outer IPV4 and outer UDP and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Outer IPV4 and outer UDP and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_21: IPV4/UDP/IPV6/TCP: Outer IPV4 and outer UDP checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Outer IPV4 and outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_22: IPV4/UDP/IPV6/UDP: Outer IPV4 and outer UDP and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Outer IPV4 and outer UDP and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_23: IPV4/UDP/IPV6/UDP: Outer IPV4 and outer UDP checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Outer IPV4 and outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_24: IPV4/UDP/IPV4: Outer UDP and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4: Outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_25: IPV4/UDP/IPV4: Outer UDP checksum error */
		{.description = "IPV4/UDP/IPV4: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_26: IPV4/UDP/IPV4/TCP: Outer UDP and inner IPV4 and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer UDP and inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_27: IPV4/UDP/IPV4/TCP: Outer UDP and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer UDP and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_28: IPV4/UDP/IPV4/TCP: Outer UDP and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_29: IPV4/UDP/IPV4/TCP: Outer UDP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_30: IPV4/UDP/IPV4/UDP: Outer UDP and inner IPV4 and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer UDP and inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_31: IPV4/UDP/IPV4/UDP: Outer UDP and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer UDP and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_32: IPV4/UDP/IPV4/UDP: Outer UDP and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_33: IPV4/UDP/IPV4/UDP: Outer UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_34: IPV4/UDP/IPV6: Outer UDP checksum error */
		{.description = "IPV4/UDP/IPV6: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_35: IPV4/UDP/IPV6/TCP: Outer UDP and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Outer UDP and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_36: IPV4/UDP/IPV6/TCP: Outer UDP checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_37: IPV4/UDP/IPV6/UDP: Outer UDP and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Outer UDP and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_38: IPV4/UDP/IPV6/UDP: Outer UDP checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_39: IPV4/UDP/IPV4: Outer IPV4 and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4: Outer IPV4 and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_40: IPV4/UDP/IPV4: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_41: IPV4/UDP/IPV4/TCP: Outer IPV4 and inner IPV4 and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 and inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_42: IPV4/UDP/IPV4/TCP: Outer IPV4 and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_43: IPV4/UDP/IPV4/TCP: Outer IPV4 and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_44: IPV4/UDP/IPV4/TCP: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_45: IPV4/UDP/IPV4/UDP: Outer IPV4 and inner IPV4 and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 and inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_46: IPV4/UDP/IPV4/UDP: Outer IPV4 and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_47: IPV4/UDP/IPV4/UDP: Outer IPV4 and inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_48: IPV4/UDP/IPV4/UDP: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_49: IPV4/UDP/IPV6: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV6: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_50: IPV4/UDP/IPV6/TCP: Outer IPV4 and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Outer IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_51: IPV4/UDP/IPV6/TCP: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_52: IPV4/UDP/IPV6/UDP: Outer IPV4 and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Outer IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_53: IPV4/UDP/IPV6/UDP: Outer IPV4 checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_54: IPV4/UDP/IPV4: Inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_55: IPV4/UDP/IPV4/TCP: Inner IPV4 and inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_56: IPV4/UDP/IPV4/TCP: Inner TCP checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_57: IPV4/UDP/IPV4/TCP: Inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/TCP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_58: IPV4/UDP/IPV4/UDP: Inner IPV4 and inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_59: IPV4/UDP/IPV4/UDP: Inner UDP checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_60: IPV4/UDP/IPV4/UDP: Inner IPV4 checksum error */
		{.description = "IPV4/UDP/IPV4/UDP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_61: IPV4/UDP/IPV6/TCP: Inner TCP checksum error */
		{.description = "IPV4/UDP/IPV6/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_62: IPV4/UDP/IPV6/UDP: Inner UDP checksum error */
		{.description = "IPV4/UDP/IPV6/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_63: IPV6/UDP/IPV4: Outer UDP and inner IPV4 checksum error */
		{.description = "IPV6/UDP/IPV4: Outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_64: IPV6/UDP/IPV4: Outer UDP checksum error */
		{.description = "IPV6/UDP/IPV4: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_65: IPV6/UDP/IPV4/TCP: Outer UDP and inner IPV4 and inner TCP checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Outer UDP and inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_66: IPV6/UDP/IPV4/TCP: Outer UDP and inner TCP checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Outer UDP and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_67: IPV6/UDP/IPV4/TCP: Outer UDP and inner IPV4 checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_68: IPV6/UDP/IPV4/TCP: Outer UDP checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_69: IPV6/UDP/IPV4/UDP: Outer UDP and inner IPV4 and inner UDP checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Outer UDP and inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_70: IPV6/UDP/IPV4/UDP: Outer UDP and inner UDP checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Outer UDP and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_71: IPV6/UDP/IPV4/UDP: Outer UDP and inner IPV4 checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Outer UDP and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_72: IPV6/UDP/IPV4/UDP: Outer UDP checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_73: IPV6/UDP/IPV6: Outer UDP checksum error */
		{.description = "IPV6/UDP/IPV6: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_74: IPV6/UDP/IPV6/TCP: Outer UDP and inner TCP checksum error */
		{.description = "IPV6/UDP/IPV6/TCP: Outer UDP and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_75: IPV6/UDP/IPV6/TCP: Outer UDP checksum error */
		{.description = "IPV6/UDP/IPV6/TCP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_76: IPV6/UDP/IPV6/UDP: Outer UDP and inner UDP checksum error */
		{.description = "IPV6/UDP/IPV6/UDP: Outer UDP and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_77: IPV6/UDP/IPV6/UDP: Outer UDP checksum error */
		{.description = "IPV6/UDP/IPV6/UDP: Outer UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_78: IPV6/UDP/IPV4: Inner IPV4 checksum error */
		{.description = "IPV6/UDP/IPV4: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_79: IPV6/UDP/IPV4/TCP: Inner IPV4 and inner TCP checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_80: IPV6/UDP/IPV4/TCP: Inner TCP checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_81: IPV6/UDP/IPV4/TCP: Inner IPV4 checksum error */
		{.description = "IPV6/UDP/IPV4/TCP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_82: IPV6/UDP/IPV4/UDP: Inner IPV4 and inner UDP checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_83: IPV6/UDP/IPV4/UDP: Inner UDP checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_84: IPV6/UDP/IPV4/UDP: Inner IPV4 checksum error */
		{.description = "IPV6/UDP/IPV4/UDP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_85: IPV6/UDP/IPV6/TCP: Inner TCP checksum error */
		{.description = "IPV6/UDP/IPV6/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_86: IPV6/UDP/IPV6/UDP: Inner UDP checksum error */
		{.description = "IPV6/UDP/IPV6/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_87: IPV4/IPV4: Outer IPV4 and inner IPV4 checksum error */
		{.description = "IPV4/IPV4: Outer IPV4 and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_88: IPV4/IPV4: Outer IPV4 checksum error */
		{.description = "IPV4/IPV4: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_89: IPV4/IPV4/TCP: Outer IPV4 and inner IPV4 and inner TCP checksum error */
		{.description = "IPV4/IPV4/TCP: Outer IPV4 and inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_90: IPV4/IPV4/TCP: Outer IPV4 and inner TCP checksum error */
		{.description = "IPV4/IPV4/TCP: Outer IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_91: IPV4/IPV4/TCP: Outer IPV4 and inner IPV4 checksum error */
		{.description = "IPV4/IPV4/TCP: Outer IPV4 and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_92: IPV4/IPV4/TCP: Outer IPV4 checksum error */
		{.description = "IPV4/IPV4/TCP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_93: IPV4/IPV4/UDP: Outer IPV4 and inner IPV4 and inner UDP checksum error */
		{.description = "IPV4/IPV4/UDP: Outer IPV4 and inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_94: IPV4/IPV4/UDP: Outer IPV4 and inner UDP checksum error */
		{.description = "IPV4/IPV4/UDP: Outer IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_95: IPV4/IPV4/UDP: Outer IPV4 and inner IPV4 checksum error */
		{.description = "IPV4/IPV4/UDP: Outer IPV4 and inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_96: IPV4/IPV4/UDP: Outer IPV4 checksum error */
		{.description = "IPV4/IPV4/UDP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_97: IPV4/IPV6: Outer IPV4 checksum error */
		{.description = "IPV4/IPV6: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_98: IPV4/IPV6/TCP: Outer IPV4 and inner TCP checksum error */
		{.description = "IPV4/IPV6/TCP: Outer IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_99: IPV4/IPV6/TCP: Outer IPV4 checksum error */
		{.description = "IPV4/IPV6/TCP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_100: IPV4/IPV6/UDP: Outer IPV4 and inner UDP checksum error */
		{.description = "IPV4/IPV6/UDP: Outer IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_101: IPV4/IPV6/UDP: Outer IPV4 checksum error */
		{.description = "IPV4/IPV6/UDP: Outer IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_102: IPV4/IPV4: Inner IPV4 checksum error */
		{.description = "IPV4/IPV4: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_103: IPV4/IPV4/TCP: Inner IPV4 and inner TCP checksum error */
		{.description = "IPV4/IPV4/TCP: Inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_104: IPV4/IPV4/TCP: Inner TCP checksum error */
		{.description = "IPV4/IPV4/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_105: IPV4/IPV4/TCP: Inner IPV4 checksum error */
		{.description = "IPV4/IPV4/TCP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_106: IPV4/IPV4/UDP: Inner IPV4 and inner UDP checksum error */
		{.description = "IPV4/IPV4/UDP: Inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_107: IPV4/IPV4/UDP: Inner UDP checksum error */
		{.description = "IPV4/IPV4/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_108: IPV4/IPV4/UDP: Inner IPV4 checksum error */
		{.description = "IPV4/IPV4/UDP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_109: IPV4/IPV6/TCP: Inner TCP checksum error */
		{.description = "IPV4/IPV6/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_110: IPV4/IPV6/UDP: Inner UDP checksum error */
		{.description = "IPV4/IPV6/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_111: IPV6/IPV4: Inner IPV4 checksum error */
		{.description = "IPV6/IPV4: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_112: IPV6/IPV4/TCP: Inner IPV4 and inner TCP checksum error */
		{.description = "IPV6/IPV4/TCP: Inner IPV4 and inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_113: IPV6/IPV4/TCP: Inner TCP checksum error */
		{.description = "IPV6/IPV4/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_114: IPV6/IPV4/TCP: Inner IPV4 checksum error */
		{.description = "IPV6/IPV4/TCP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_115: IPV6/IPV4/UDP: Inner IPV4 and inner UDP checksum error */
		{.description = "IPV6/IPV4/UDP: Inner IPV4 and inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_116: IPV6/IPV4/UDP: Inner UDP checksum error */
		{.description = "IPV6/IPV4/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 1,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_117: IPV6/IPV4/UDP: Inner IPV4 checksum error */
		{.description = "IPV6/IPV4/UDP: Inner IPV4 checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 1},
		 .has_inner = true},
		/* invalid_flow_118: IPV6/IPV6/TCP: Inner TCP checksum error */
		{.description = "IPV6/IPV6/TCP: Inner TCP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_TCP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true},
		/* invalid_flow_119: IPV6/IPV6/UDP: Inner UDP checksum error */
		{.description = "IPV6/IPV6/UDP: Inner UDP checksum error",
		 .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_NONE,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 0,
			   .l4_checksum_ok = 0},
		 .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
			   .l4_type = DOCA_FLOW_L4_META_UDP,
			   .ip_fragmented = 0,
			   .l3_ok = 1,
			   .ip4_checksum_ok = 0,
			   .l4_ok = 1,
			   .l4_checksum_ok = 0},
		 .has_inner = true}};

	int num_entries = sizeof(invalid_entries) / sizeof(invalid_entries[0]);

	for (int i = 0; i < num_entries; i++) {
		memset(&match, 0, sizeof(match));

		/* Get current entry */
		const struct flow_entry *entry = &invalid_entries[i];

		memset(&match_mask, 0, sizeof(match_mask));

		/* Set outer match mask fields */
		match_mask.parser_meta = (struct doca_flow_parser_meta){
			.outer_l3_type = UINT32_MAX,
			.outer_l4_type = UINT32_MAX,
			.outer_ip_fragmented = UINT8_MAX,
			.outer_l3_ok = UINT8_MAX,
			.outer_ip4_checksum_ok = UINT8_MAX,
			.outer_l4_ok = UINT8_MAX,
			.outer_l4_checksum_ok = UINT8_MAX,
			.inner_l3_type = UINT32_MAX,
			.inner_l4_type = UINT32_MAX,
			.inner_ip_fragmented = UINT8_MAX,
			.inner_l3_ok = UINT8_MAX,
			.inner_ip4_checksum_ok = UINT8_MAX,
			.inner_l4_ok = UINT8_MAX,
			.inner_l4_checksum_ok = UINT8_MAX,
		};

		memset(&match, 0, sizeof(match));

		/* Set outer match fields */
		match.parser_meta = (struct doca_flow_parser_meta){
			.outer_l3_type = entry->outer.l3_type,
			.outer_l4_type = entry->outer.l4_type,
			.outer_ip_fragmented = entry->outer.ip_fragmented,
			.outer_l3_ok = entry->outer.l3_ok,
			.outer_ip4_checksum_ok = entry->outer.ip4_checksum_ok,
			.outer_l4_ok = entry->outer.l4_ok,
			.outer_l4_checksum_ok = entry->outer.l4_checksum_ok,
			.inner_l3_type = entry->has_inner ? entry->inner.l3_type : DOCA_FLOW_L3_META_NONE,
			.inner_l4_type = entry->has_inner ? entry->inner.l4_type : DOCA_FLOW_L4_META_NONE,
			.inner_ip_fragmented = entry->has_inner ? entry->inner.ip_fragmented : 0,
			.inner_l3_ok = entry->has_inner ? entry->inner.l3_ok : 0,
			.inner_ip4_checksum_ok = entry->has_inner ? entry->inner.ip4_checksum_ok : 0,
			.inner_l4_ok = entry->has_inner ? entry->inner.l4_ok : 0,
			.inner_l4_checksum_ok = entry->has_inner ? entry->inner.l4_checksum_ok : 0,
		};

		memset(&fwd, 0, sizeof(fwd));
		fwd.type = DOCA_FLOW_FWD_DROP;
		memset(&monitor, 0, sizeof(monitor));
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

		result = doca_flow_pipe_control_add_entry(0,
							  priority,
							  pipe,
							  &match,
							  &match_mask,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &monitor,
							  &fwd,
							  status,
							  &((*entries)[g_flow_db[port_id].count]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add invalid entry for %s: %s",
				     invalid_entries[i].description,
				     doca_error_get_descr(result));
			return result;
		}
		result = flow_database_add(&g_flow_db[port_id], &invalid_entries[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add invalid flow %s to database", invalid_entries[i].description);
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * New function: add_not_ok_flows_table_entries
 * Adds DOCA Flow pipe entries for flows where the parser stopped parsing due to errors,
 * such as invalid length (mainly due to length check errors) or other reasons that cause
 * the parser to stop processing the input packet.
 * Uses the existing packet_attrs and flow_entry structures.
 * Priority is 4 and fwd action is DROP.
 */
static doca_error_t add_not_ok_flows_table_entries(struct doca_flow_pipe *pipe,
						   int port_id,
						   struct entries_status *status,
						   struct doca_flow_pipe_entry *(*entries)[MAX_ENTRIES])
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	uint8_t priority = FLOW_PRIORITY_NOT_OK;
	doca_error_t result;

	/* Define an array of invalid flow entries for length errors */
	static struct flow_entry not_ok_entries[] = {/* invalid_flow_0: IPV4: IPV4 len error */
						     {.description = "IPV4: IPV4 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = false},
						     /* invalid_flow_1: IPV6: IPV6 len error */
						     {.description = "IPV6: IPV6 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = false},
						     /* invalid_flow_2: IPV4/TCP: TCP len error */
						     {.description = "IPV4/TCP: TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = false},
						     /* invalid_flow_3: IPV4/UDP: UDP len error */
						     {.description = "IPV4/UDP: UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = false},
						     /* invalid_flow_4: IPV6/TCP: TCP len error */
						     {.description = "IPV6/TCP: TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = false},
						     /* invalid_flow_5: IPV6/UDP: UDP len error */
						     {.description = "IPV6/UDP: UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = false},
						     /* invalid_flow_6: IPV4/UDP/IPV4: Inner IPV4 len error */
						     {.description = "IPV4/UDP/IPV4: Inner IPV4 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_7: IPV4/UDP/IPV6: Inner IPV6 len error */
						     {.description = "IPV4/UDP/IPV6: Inner IPV6 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_8: IPV4/UDP/IPV4/TCP: Inner TCP len error */
						     {.description = "IPV4/UDP/IPV4/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_9: IPV4/UDP/IPV4/UDP: Inner UDP len error */
						     {.description = "IPV4/UDP/IPV4/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_10: IPV4/UDP/IPV6/TCP: Inner TCP len error */
						     {.description = "IPV4/UDP/IPV6/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_11: IPV4/UDP/IPV6/UDP: Inner UDP len error */
						     {.description = "IPV4/UDP/IPV6/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_12: IPV6/UDP/IPV4: Inner IPV4 len error */
						     {.description = "IPV6/UDP/IPV4: Inner IPV4 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_13: IPV6/UDP/IPV6: Inner IPV6 len error */
						     {.description = "IPV6/UDP/IPV6: Inner IPV6 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_14: IPV6/UDP/IPV4/TCP: Inner TCP len error */
						     {.description = "IPV6/UDP/IPV4/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_15: IPV6/UDP/IPV4/UDP: Inner UDP len error */
						     {.description = "IPV6/UDP/IPV4/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_16: IPV6/UDP/IPV6/TCP: Inner TCP len error */
						     {.description = "IPV6/UDP/IPV6/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_17: IPV6/UDP/IPV6/UDP: Inner UDP len error */
						     {.description = "IPV6/UDP/IPV6/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 1},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_18: IPV4/IPV4: Inner IPV4 len error */
						     {.description = "IPV4/IPV4: Inner IPV4 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_19: IPV4/IPV6: Inner IPV6 len error */
						     {.description = "IPV4/IPV6: Inner IPV6 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_20: IPV4/IPV4/TCP: Inner TCP len error */
						     {.description = "IPV4/IPV4/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_21: IPV4/IPV4/UDP: Inner UDP len error */
						     {.description = "IPV4/IPV4/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_22: IPV4/IPV6/TCP: Inner TCP len error */
						     {.description = "IPV4/IPV6/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_23: IPV4/IPV6/UDP: Inner UDP len error */
						     {.description = "IPV4/IPV6/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_24: IPV6/IPV4: Inner IPV4 len error */
						     {.description = "IPV6/IPV4: Inner IPV4 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_25: IPV6/IPV6: Inner IPV6 len error */
						     {.description = "IPV6/IPV6: Inner IPV6 len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 0,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_26: IPV6/IPV4/TCP: Inner TCP len error */
						     {.description = "IPV6/IPV4/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_27: IPV6/IPV4/UDP: Inner UDP len error */
						     {.description = "IPV6/IPV4/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV4,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_28: IPV6/IPV6/TCP: Inner TCP len error */
						     {.description = "IPV6/IPV6/TCP: Inner TCP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_TCP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true},
						     /* invalid_flow_29: IPV6/IPV6/UDP: Inner UDP len error */
						     {.description = "IPV6/IPV6/UDP: Inner UDP len error",
						      .outer = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_NONE,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .inner = {.l3_type = DOCA_FLOW_L3_META_IPV6,
								.l4_type = DOCA_FLOW_L4_META_UDP,
								.ip_fragmented = 0,
								.l3_ok = 1,
								.l4_ok = 0},
						      .has_inner = true}};

	int num_entries = sizeof(not_ok_entries) / sizeof(not_ok_entries[0]);

	for (int i = 0; i < num_entries; i++) {
		memset(&match, 0, sizeof(match));

		/* Get current entry */
		const struct flow_entry *entry = &not_ok_entries[i];

		memset(&match_mask, 0, sizeof(match_mask));

		/* Set match mask fields */
		match_mask.parser_meta = (struct doca_flow_parser_meta){
			.outer_l3_type = UINT32_MAX,
			.outer_l4_type = UINT32_MAX,
			.outer_ip_fragmented = UINT8_MAX,
			.outer_l3_ok = UINT8_MAX,
			.outer_l4_ok = UINT8_MAX,
			.inner_l3_type = UINT32_MAX,
			.inner_l4_type = UINT32_MAX,
			.inner_ip_fragmented = UINT8_MAX,
			.inner_l3_ok = UINT8_MAX,
			.inner_l4_ok = UINT8_MAX,
		};

		memset(&match, 0, sizeof(match));

		/* Set match fields */
		match.parser_meta = (struct doca_flow_parser_meta){
			.outer_l3_type = entry->outer.l3_type,
			.outer_l4_type = entry->outer.l4_type,
			.outer_ip_fragmented = entry->outer.ip_fragmented,
			.outer_l3_ok = entry->outer.l3_ok,
			.outer_l4_ok = entry->outer.l4_ok,
			.inner_l3_type = entry->has_inner ? entry->inner.l3_type : DOCA_FLOW_L3_META_NONE,
			.inner_l4_type = entry->has_inner ? entry->inner.l4_type : DOCA_FLOW_L4_META_NONE,
			.inner_ip_fragmented = entry->has_inner ? entry->inner.ip_fragmented : 0,
			.inner_l3_ok = entry->has_inner ? entry->inner.l3_ok : 0,
			.inner_l4_ok = entry->has_inner ? entry->inner.l4_ok : 0,
		};

		memset(&fwd, 0, sizeof(fwd));
		fwd.type = DOCA_FLOW_FWD_DROP;
		memset(&monitor, 0, sizeof(monitor));
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

		result = doca_flow_pipe_control_add_entry(0,
							  priority,
							  pipe,
							  &match,
							  &match_mask,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &monitor,
							  &fwd,
							  status,
							  &((*entries)[g_flow_db[port_id].count]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add invalid length entry for %s: %s",
				     not_ok_entries[i].description,
				     doca_error_get_descr(result));
			return result;
		}
		result = flow_database_add(&g_flow_db[port_id], &not_ok_entries[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add invalid length flow %s to database", not_ok_entries[i].description);
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/* Context structure for statistics printing */
struct stats_context {
	int nb_ports;
	struct doca_flow_pipe_entry *(*entries)[MAX_ENTRIES];
};

static void print_port_stats(int nb_ports, struct doca_flow_pipe_entry *entries[][MAX_ENTRIES])
{
	for (int port_id = 0; port_id < nb_ports; port_id++) {
		struct doca_flow_resource_query query_stats;
		DOCA_LOG_INFO("Port %d Statistics:", port_id);
		DOCA_LOG_INFO("===================================================");
		for (size_t i = 0; i < g_flow_db[port_id].count; i++) {
			struct flow_entry *flow = flow_database_get_by_index(&g_flow_db[port_id], i);
			if (flow == NULL)
				continue;
			if (doca_flow_resource_query_entry(entries[port_id][i], &query_stats) != DOCA_SUCCESS)
				continue;
			DOCA_LOG_INFO("Port %d Entry %zu (%s): Total packets: %ld",
				      port_id,
				      i,
				      flow->description,
				      query_stats.counter.total_pkts);
		}
		DOCA_LOG_INFO("===================================================");
	}
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: stats_context structure
 */
static void print_port_stats_wrapper(void *context)
{
	struct stats_context *ctx = (struct stats_context *)context;
	print_port_stats(ctx->nb_ports, ctx->entries);
}

doca_error_t flow_parser_meta(int nb_queues, int stats_interval)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[NUMBER_OF_PORTS];
	uint32_t actions_mem_size[NUMBER_OF_PORTS];
	struct doca_flow_pipe *parser_meta_pipe;
	struct doca_flow_pipe_entry *entries[NUMBER_OF_PORTS][MAX_ENTRIES];
	struct entries_status status;
	struct stats_context ctx = {.nb_ports = NUMBER_OF_PORTS, .entries = entries};
	doca_error_t result;
	int port_id;
	int wait_time = (stats_interval > 0) ? 20 : 10;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = MAX_ENTRIES * NUMBER_OF_PORTS;
	resource.nr_rss = 2;

	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(MAX_ENTRIES));
	result = init_doca_flow_vnf_ports(NUMBER_OF_PORTS, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto cleanup_flow;
	}

	result = init_flow_database(NUMBER_OF_PORTS, MAX_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize flow database: %s", doca_error_get_descr(result));
		goto cleanup_ports;
	}

	for (port_id = 0; port_id < NUMBER_OF_PORTS; port_id++) {
		memset(&status, 0, sizeof(status));

		result = create_parser_meta_pipe(ports[port_id], &parser_meta_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create parser_meta pipe: %s", doca_error_get_descr(result));
			goto cleanup_all;
		}

		result = add_valid_flows_table_entries(parser_meta_pipe, port_id, &status, &(entries[port_id]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add parser_meta entries: %s", doca_error_get_descr(result));
			goto cleanup_all;
		}

		result = add_fragmented_flows_table_entries(parser_meta_pipe, port_id, &status, &(entries[port_id]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add fragmented entries: %s", doca_error_get_descr(result));
			goto cleanup_all;
		}

		result = add_invalid_csum_flows_table_entries(parser_meta_pipe, port_id, &status, &(entries[port_id]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add invalid entries: %s", doca_error_get_descr(result));
			goto cleanup_all;
		}

		result = add_not_ok_flows_table_entries(parser_meta_pipe, port_id, &status, &(entries[port_id]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add invalid length entries: %s", doca_error_get_descr(result));
			goto cleanup_all;
		}

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, MAX_ENTRIES);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			goto cleanup_all;
		}

		if (status.failure || !status.nb_processed) {
			DOCA_LOG_ERR("Failed to process entries");
			result = DOCA_ERROR_BAD_STATE;
			goto cleanup_all;
		}
	}

	flow_wait_for_packets(wait_time, print_port_stats_wrapper, &ctx);

	result = stop_doca_flow_ports(NUMBER_OF_PORTS, ports);
	doca_flow_destroy();
	flow_database_destroy_all();
	return result;

cleanup_all : {
	flow_database_destroy_all();
}
cleanup_ports : {
	stop_doca_flow_ports(NUMBER_OF_PORTS, ports);
}
cleanup_flow : {
	doca_flow_destroy();
}
	return result;
}
