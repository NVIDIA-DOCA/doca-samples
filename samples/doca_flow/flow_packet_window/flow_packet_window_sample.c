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
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <doca_bitfield.h>
#include <doca_flow.h>
#include <doca_flow_definitions.h>
#include <doca_flow_net.h>
#include <doca_log.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_PACKET_WINDOW);

#define NB_PORTS 2
#define ENTRIES_PER_PORT 2
#define COUNTERS_PER_PORT 2
#define NB_ACTION_DESCS 2
#define WAIT_TIME 15

#define PACKET_WINDOW_SIZE 8
#define UDP_HEADER_SIZE 8
#define SAMPLE_UDP_DST_PORT 1234
#define PAYLOAD_TAG_OFFSET 0
#define PAYLOAD_TAG_SIZE 4
#define PAYLOAD_CODE_OFFSET 4
#define PAYLOAD_CODE_SIZE 1
#define PAYLOAD_COPY_OFFSET 5
#define PAYLOAD_COPY_SIZE 1
#define PAYLOAD_ADD_OFFSET 6
#define PAYLOAD_ADD_SIZE 1
#define PAYLOAD_CODE_MATCH 0x01
#define PAYLOAD_ADD_VALUE 0x01

#define PACKET_WINDOW_MATCH_PREFIX "match.packet.programmable.packet_window"
#define PACKET_WINDOW_ACTION_PREFIX "actions.packet.programmable.packet_window"
#define PAYLOAD_CODE_FIELD "programmable.packet_window.payload_code"
#define PAYLOAD_COPY_FIELD "programmable.packet_window.payload_copy"
#define PAYLOAD_ADD_FIELD "programmable.packet_window.payload_add"

static_assert(PAYLOAD_CODE_SIZE == 1 && PAYLOAD_COPY_SIZE == 1 && PAYLOAD_ADD_SIZE == 1,
	      "single-byte Packet Window writes assume 1-byte code/copy/add fields");

static const uint8_t payload_match_tag[PAYLOAD_TAG_SIZE] = {'P', 'W', 'O', 'K'};
static const uint8_t payload_set_tag[PAYLOAD_TAG_SIZE] = {'D', 'O', 'N', 'E'};

struct packet_window_match {
	struct doca_flow_match base;
	uint8_t packet_window[PACKET_WINDOW_SIZE];
};
static_assert(offsetof(struct packet_window_match, base) == 0,
	      "packet_window_match: 'base' must be first to stay compatible with doca_flow_match");
static_assert(offsetof(struct packet_window_match, packet_window) == sizeof(struct doca_flow_match),
	      "packet_window_match: packet_window must immediately follow doca_flow_match");

struct packet_window_actions {
	struct doca_flow_actions base;
	uint8_t packet_window[PACKET_WINDOW_SIZE];
};
static_assert(offsetof(struct packet_window_actions, base) == 0,
	      "packet_window_actions: 'base' must be first to stay compatible with doca_flow_actions");
static_assert(offsetof(struct packet_window_actions, packet_window) == sizeof(struct doca_flow_actions),
	      "packet_window_actions: packet_window must immediately follow doca_flow_actions");

struct packet_window_port_ctx {
	struct doca_flow_pipe *user_pipe;
	struct doca_flow_pipe *root_pipe;
	struct doca_flow_pipe_entry *user_entry;
	struct doca_flow_pipe_entry *root_entry;
};

/*
 * Create matching and action definitions for the eight-byte Packet Window.
 *
 * @definitions [out]: created definitions object
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_packet_window_definitions(struct doca_flow_definitions **definitions)
{
	struct doca_flow_definitions_programmable_struct *programmable_struct = NULL;
	struct doca_flow_definitions_cfg *definitions_cfg = NULL;
	doca_error_t cleanup_result;
	doca_error_t result;

	*definitions = NULL;

	result = doca_flow_definitions_cfg_create(&definitions_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_create(definitions_cfg, definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions object: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_cfg_destroy(definitions_cfg);
	definitions_cfg = NULL;
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy definitions configuration: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_programmable_struct_create(PACKET_WINDOW_SIZE, &programmable_struct);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Packet Window programmable struct: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_programmable_struct_add_field(programmable_struct,
								     "payload_tag",
								     PAYLOAD_TAG_OFFSET,
								     PAYLOAD_TAG_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add payload tag field: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_programmable_struct_add_field(programmable_struct,
								     "payload_code",
								     PAYLOAD_CODE_OFFSET,
								     PAYLOAD_CODE_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add payload code field: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_programmable_struct_add_field(programmable_struct,
								     "payload_copy",
								     PAYLOAD_COPY_OFFSET,
								     PAYLOAD_COPY_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add payload copy field: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_programmable_struct_add_field(programmable_struct,
								     "payload_add",
								     PAYLOAD_ADD_OFFSET,
								     PAYLOAD_ADD_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add payload add field: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_add_programmable_struct(*definitions,
							       PACKET_WINDOW_MATCH_PREFIX,
							       offsetof(struct packet_window_match, packet_window),
							       programmable_struct);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register Packet Window programmable struct: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_add_programmable_struct(*definitions,
							       PACKET_WINDOW_ACTION_PREFIX,
							       offsetof(struct packet_window_actions, packet_window),
							       programmable_struct);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register Packet Window action programmable struct: %s",
			     doca_error_get_descr(result));
		goto cleanup;
	}

	doca_flow_definitions_programmable_struct_destroy(programmable_struct);
	return DOCA_SUCCESS;

cleanup:
	if (programmable_struct != NULL)
		doca_flow_definitions_programmable_struct_destroy(programmable_struct);
	if (definitions_cfg != NULL) {
		cleanup_result = doca_flow_definitions_cfg_destroy(definitions_cfg);
		if (cleanup_result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy definitions configuration: %s",
				     doca_error_get_descr(cleanup_result));
	}
	if (*definitions != NULL) {
		doca_flow_definitions_destroy(*definitions);
		*definitions = NULL;
	}
	return result;
}

/*
 * Create a non-root BASIC pipe that matches and modifies IPv4/UDP payload fields through Packet Window.
 *
 * @port [in]: pipe port
 * @port_id [in]: pipe port ID
 * @pipe [out]: created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_packet_window_user_pipe(struct doca_flow_port *port,
						   int port_id,
						   struct doca_flow_pipe **pipe)
{
	struct packet_window_match match = {0};
	struct packet_window_match match_mask = {0};
	struct packet_window_actions actions = {0};
	struct packet_window_actions actions_mask = {0};
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR] = {&actions.base};
	struct doca_flow_actions *actions_mask_arr[NB_ACTIONS_ARR] = {&actions_mask.base};
	struct doca_flow_action_desc action_desc_array[NB_ACTION_DESCS] = {0};
	struct doca_flow_action_descs action_descs = {
		.nb_action_desc = NB_ACTION_DESCS,
		.desc_array = action_desc_array,
	};
	struct doca_flow_action_descs *action_descs_arr[NB_ACTIONS_ARR] = {&action_descs};
	struct doca_flow_packet_window_cfg packet_window_cfg = {
		.position = DOCA_FLOW_HEADER_POSITION_OUTER_TCP_UDP_START,
		.byte_offset = UDP_HEADER_SIZE,
	};
	struct doca_flow_monitor monitor = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd fwd_miss = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t cleanup_result;
	doca_error_t result;

	memset(&match.packet_window[PAYLOAD_TAG_OFFSET], UINT8_MAX, PAYLOAD_TAG_SIZE);
	memset(&match_mask.packet_window[PAYLOAD_TAG_OFFSET], UINT8_MAX, PAYLOAD_TAG_SIZE);
	match.packet_window[PAYLOAD_CODE_OFFSET] = UINT8_MAX;
	match_mask.packet_window[PAYLOAD_CODE_OFFSET] = UINT8_MAX;

	/* Tag SET uses actions/mask (implicit AUTO). COPY dst stays unmasked; only COPY/ADD need descriptors. */
	memcpy(&actions.packet_window[PAYLOAD_TAG_OFFSET], payload_set_tag, sizeof(payload_set_tag));
	/* The descriptor reads its fixed ADD immediate from the destination field in actions. */
	actions.packet_window[PAYLOAD_ADD_OFFSET] = PAYLOAD_ADD_VALUE;
	memset(&actions_mask.packet_window[PAYLOAD_TAG_OFFSET], UINT8_MAX, PAYLOAD_TAG_SIZE);
	actions_mask.packet_window[PAYLOAD_ADD_OFFSET] = UINT8_MAX;

	action_desc_array[0].type = DOCA_FLOW_ACTION_COPY;
	action_desc_array[0].field_op.src.field_string = PAYLOAD_CODE_FIELD;
	action_desc_array[0].field_op.src.bit_offset = 0;
	action_desc_array[0].field_op.dst.field_string = PAYLOAD_COPY_FIELD;
	action_desc_array[0].field_op.dst.bit_offset = 0;
	action_desc_array[0].field_op.width = PAYLOAD_COPY_SIZE * 8;

	action_desc_array[1].type = DOCA_FLOW_ACTION_ADD;
	action_desc_array[1].field_op.dst.field_string = PAYLOAD_ADD_FIELD;
	action_desc_array[1].field_op.dst.bit_offset = 0;
	action_desc_array[1].field_op.width = PAYLOAD_ADD_SIZE * 8;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Packet Window pipe configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "PACKET_WINDOW_USER_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure Packet Window user pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Packet Window user pipe size: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, &match_mask.base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Packet Window user pipe match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_actions(pipe_cfg,
						actions_arr,
						actions_mask_arr,
						action_descs_arr,
						NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Packet Window user pipe actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Packet Window user pipe monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_packet_window(pipe_cfg, &packet_window_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Packet Window pipe configuration: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Packet Window user pipe: %s", doca_error_get_descr(result));
		if (result == DOCA_ERROR_NOT_SUPPORTED)
			DOCA_LOG_ERR("This sample requires a device that can place a Packet Window at an arbitrary "
				     "parsed-header offset; earlier devices return DOCA_ERROR_NOT_SUPPORTED");
	}

destroy_pipe_cfg:
	cleanup_result = doca_flow_pipe_cfg_destroy(pipe_cfg);
	if (cleanup_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Packet Window pipe configuration: %s",
			     doca_error_get_descr(cleanup_result));
		DOCA_ERROR_PROPAGATE(result, cleanup_result);
	}
	return result;
}

/*
 * Fill the IPv4/UDP dispatcher match used by both the root pipe template and its entry.
 *
 * @match [out]: match to populate
 */
static void set_ipv4_udp_match(struct packet_window_match *match)
{
	match->base.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match->base.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match->base.outer.udp.l4_port.dst_port = DOCA_HTOBE16(SAMPLE_UDP_DST_PORT);
}

/*
 * Create a root BASIC pipe that dispatches IPv4/UDP traffic to the Packet Window user pipe.
 *
 * @port [in]: pipe port
 * @user_pipe [in]: destination user pipe
 * @pipe [out]: created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipv4_udp_root_pipe(struct doca_flow_port *port,
					      struct doca_flow_pipe *user_pipe,
					      struct doca_flow_pipe **pipe)
{
	/* Definitions register Packet Window past sizeof(doca_flow_match), so every pipe uses the extended match. */
	struct packet_window_match match = {0};
	struct doca_flow_monitor monitor = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd fwd_miss = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t cleanup_result;
	doca_error_t result;

	set_ipv4_udp_match(&match);

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = user_pipe;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IPv4/UDP root pipe configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "IPV4_UDP_ROOT_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure IPv4/UDP root pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set IPv4/UDP root pipe size: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set IPv4/UDP root pipe match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set IPv4/UDP root pipe monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create IPv4/UDP root pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	cleanup_result = doca_flow_pipe_cfg_destroy(pipe_cfg);
	if (cleanup_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy IPv4/UDP root pipe configuration: %s",
			     doca_error_get_descr(cleanup_result));
		DOCA_ERROR_PROPAGATE(result, cleanup_result);
	}
	return result;
}

/*
 * Add the payload tag and code match entry to the Packet Window user pipe.
 *
 * @pipe [in]: Packet Window user pipe
 * @status [in/out]: entry processing status
 * @entry [out]: created entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_packet_window_user_entry(struct doca_flow_pipe *pipe,
						 struct entries_status *status,
						 struct doca_flow_pipe_entry **entry)
{
	struct packet_window_match match = {0};
	doca_error_t result;

	memcpy(&match.packet_window[PAYLOAD_TAG_OFFSET], payload_match_tag, sizeof(payload_match_tag));
	match.packet_window[PAYLOAD_CODE_OFFSET] = PAYLOAD_CODE_MATCH;

	result = doca_flow_pipe_basic_add_entry(0,
						pipe,
						&match.base,
						0,
						NULL,
						NULL,
						NULL,
						DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH,
						status,
						entry);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add Packet Window user entry: %s", doca_error_get_descr(result));
	return result;
}

/*
 * Add the IPv4/UDP dispatcher entry to the root pipe.
 *
 * @pipe [in]: IPv4/UDP root pipe
 * @status [in/out]: entry processing status
 * @entry [out]: created entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_ipv4_udp_root_entry(struct doca_flow_pipe *pipe,
					    struct entries_status *status,
					    struct doca_flow_pipe_entry **entry)
{
	struct packet_window_match match = {0};
	doca_error_t result;

	set_ipv4_udp_match(&match);

	result = doca_flow_pipe_basic_add_entry(0,
						pipe,
						&match.base,
						0,
						NULL,
						NULL,
						NULL,
						DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						status,
						entry);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add IPv4/UDP root entry: %s", doca_error_get_descr(result));
	return result;
}

/*
 * Print the IPv4/UDP root and Packet Window user entry counters for every port.
 *
 * @port_ctx [in]: per-port sample context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t print_packet_window_stats(const struct packet_window_port_ctx port_ctx[NB_PORTS])
{
	struct doca_flow_resource_query query_stats;
	doca_error_t result;
	int port_id;

	for (port_id = 0; port_id < NB_PORTS; port_id++) {
		memset(&query_stats, 0, sizeof(query_stats));
		result = doca_flow_resource_query_entry(port_ctx[port_id].root_entry, &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query IPv4/UDP root entry on port %d: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_INFO("Port %d IPv4/UDP root entry: packets=%" PRIu64 ", bytes=%" PRIu64,
			      port_id,
			      query_stats.counter.total_pkts,
			      query_stats.counter.total_bytes);

		memset(&query_stats, 0, sizeof(query_stats));
		result = doca_flow_resource_query_entry(port_ctx[port_id].user_entry, &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query Packet Window entry on port %d: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_INFO("Port %d Packet Window entry: packets=%" PRIu64 ", bytes=%" PRIu64,
			      port_id,
			      query_stats.counter.total_pkts,
			      query_stats.counter.total_bytes);
	}
	return DOCA_SUCCESS;
}

/*
 * Run flow_packet_window sample.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_packet_window(int nb_queues)
{
	struct doca_flow_definitions *definitions = NULL;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[NB_PORTS];
	uint32_t actions_mem_size[NB_PORTS];
	struct packet_window_port_ctx port_ctx[NB_PORTS] = {0};
	struct entries_status status;
	doca_error_t cleanup_result;
	doca_error_t result;
	int port_id;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = COUNTERS_PER_PORT;

	result = create_packet_window_definitions(&definitions);
	if (result != DOCA_SUCCESS)
		return result;

	result = init_doca_flow_with_defs(nb_queues, "vnf", &resource, nr_shared_resources, definitions);
	doca_flow_definitions_destroy(definitions);
	definitions = NULL;
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA Flow with Packet Window definitions: %s",
			     doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(ENTRIES_PER_PORT));
	result = init_doca_flow_vnf_ports(NB_PORTS, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA Flow ports: %s", doca_error_get_descr(result));
		goto destroy_flow;
	}

	for (port_id = 0; port_id < NB_PORTS; port_id++) {
		memset(&status, 0, sizeof(status));

		result = create_packet_window_user_pipe(ports[port_id], port_id, &port_ctx[port_id].user_pipe);
		if (result != DOCA_SUCCESS)
			goto stop_ports;

		result = create_ipv4_udp_root_pipe(ports[port_id],
						   port_ctx[port_id].user_pipe,
						   &port_ctx[port_id].root_pipe);
		if (result != DOCA_SUCCESS)
			goto stop_ports;

		result = add_packet_window_user_entry(port_ctx[port_id].user_pipe,
						      &status,
						      &port_ctx[port_id].user_entry);
		if (result != DOCA_SUCCESS)
			goto stop_ports;

		result = add_ipv4_udp_root_entry(port_ctx[port_id].root_pipe, &status, &port_ctx[port_id].root_entry);
		if (result != DOCA_SUCCESS)
			goto stop_ports;

		result = flow_process_entries(ports[port_id], &status, ENTRIES_PER_PORT);
		if (result != DOCA_SUCCESS)
			goto stop_ports;
	}

	DOCA_LOG_INFO("Packet Window actions: SET payload_tag to DONE, COPY payload_code to payload_copy, "
		      "ADD 1 to payload_add");
	flow_wait_for_packets(WAIT_TIME, NULL, NULL);
	result = print_packet_window_stats(port_ctx);

stop_ports:
	cleanup_result = stop_doca_flow_ports(NB_PORTS, ports);
	if (cleanup_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA Flow ports: %s", doca_error_get_descr(cleanup_result));
		DOCA_ERROR_PROPAGATE(result, cleanup_result);
	}
destroy_flow:
	doca_flow_destroy();
	return result;
}
