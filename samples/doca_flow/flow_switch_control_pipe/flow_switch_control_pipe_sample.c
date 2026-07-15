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

#include <string.h>
#include <unistd.h>

#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_SWITCH_CONTROL_PIPE);

#define NB_ENTRIES 2 /* number of entries in the created control pipe */

static struct doca_flow_pipe_entry *entries[NB_ENTRIES]; /* array for storing created entries */

/*
 * Create DOCA Flow control pipe
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_control_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "CONTROL_PIPE", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the control pipe
 *
 * @control_pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_control_pipe_entries(struct doca_flow_pipe *control_pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	uint8_t priority = 0;
	int entry_index = 0;
	doca_error_t result;

	for (entry_index = 0; entry_index < NB_ENTRIES; entry_index++) {
		memset(&match, 0, sizeof(match));
		memset(&monitor, 0, sizeof(monitor));
		memset(&fwd, 0, sizeof(fwd));

		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TRANSPORT;
		match.outer.transport.src_port = DOCA_HTOBE16(1234 + entry_index);
		match.outer.transport.dst_port = DOCA_HTOBE16(80);

		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = entry_index + 1; /* The port to forward to is defined based on the entry index */

		result = doca_flow_pipe_control_add_entry(0,
							  priority,
							  control_pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &monitor,
							  &fwd,
							  status,
							  &entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_switch_control_pipe sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */

/* Context structure for statistics printing */
struct switch_control_pipe_stats_context {
	struct doca_flow_pipe_entry **entries;
};

/*
 * Print switch control pipe statistics
 *
 * @entries [in]: array of flow entries
 */
static void print_switch_control_pipe_stats(struct doca_flow_pipe_entry *entries[])
{
	doca_error_t result;
	struct doca_flow_resource_query query_stats;
	int entry_idx;

	/* dump entries counters */
	for (entry_idx = 0; entry_idx < NB_ENTRIES; entry_idx++) {
		result = doca_flow_resource_query_entry(entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: switch_control_pipe_stats_context structure
 */
static void print_switch_control_pipe_stats_wrapper(void *context)
{
	struct switch_control_pipe_stats_context *ctx = (struct switch_control_pipe_stats_context *)context;
	print_switch_control_pipe_stats(ctx->entries);
}

doca_error_t flow_switch_control_pipe(int nb_queues, struct flow_switch_ctx *ctx)
{
	int nb_ports = 3;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_port *switch_port;
	struct doca_flow_pipe *control_pipe;
	struct entries_status status;
	int num_of_entries = 2;
	doca_error_t result;

	memset(&status, 0, sizeof(status));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = NB_ENTRIES; /* counter per entry */

	result = init_doca_flow(nb_queues, "switch,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
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

	switch_port = doca_flow_port_switch_get(NULL);

	result = create_control_pipe(switch_port, &control_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create control pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_control_pipe_entries(control_pipe, &status);
	if (result != DOCA_SUCCESS) {
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_entries_process(switch_port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	if (status.nb_processed != num_of_entries || status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_BAD_STATE;
	}

	/* Setup statistics context and wait for packets */
	struct switch_control_pipe_stats_context stats_ctx = {.entries = entries};

	flow_wait_for_packets(15, print_switch_control_pipe_stats_wrapper, &stats_ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
