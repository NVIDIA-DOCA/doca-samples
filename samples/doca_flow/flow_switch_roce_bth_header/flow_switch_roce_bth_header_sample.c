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

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_net.h>

#include <flow_common.h>
#include <flow_switch_common.h>

DOCA_LOG_REGISTER(FLOW_SWITCH);

#define NB_ENTRIES 2
#define BTH_OPCODE_CNP 0x81
#define BTH_OPCODE_EXTENDED 0x1a
#define BTH_OPCODE_UD_SEND 0x64
#define ROCE_V2_DEFAULT_SRC_PORT 4789

static struct doca_flow_pipe_entry *entries[NB_ENTRIES]; /* array for storing created entries */

/*
 * Create DOCA Flow pipe with RoCE v2 BTH header match on the switch port.
 * Pipe matches on: UDP source port, BTH opcode, destination QP and flags1 (ACK request).
 * Matched traffic will be forwarded to port 1.
 * Unmatched traffic will be dropped.
 *
 * @sw_port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_roce_pipe(struct doca_flow_port *sw_port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	uint8_t dest_qp_changeable[DOCA_FLOW_IB_BTH_DST_QP_LEN] = {0xff, 0xff, 0xff};

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match.outer.roce_v2.udp.l4_port.src_port = 0xffff;
	match.outer.roce_v2.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_ROCEV2_DEFAULT_PORT);
	match.outer.roce_v2.bth.opcode = 0xff;
	match.outer.roce_v2.bth.dest_qp[0] = dest_qp_changeable[0];
	match.outer.roce_v2.bth.dest_qp[1] = dest_qp_changeable[1];
	match.outer.roce_v2.bth.dest_qp[2] = dest_qp_changeable[2];
	match.outer.roce_v2.bth.flags1 = 0xff;

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 1;

	/* Unmatched packets will be dropped */
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, sw_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = set_flow_pipe_cfg(pipe_cfg, "ROCE_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the RoCE pipe with specific BTH header matches.
 * Creates 2 entries matching different BTH opcodes (UD_SEND and EXTENDED)
 * with specific destination QP (0x102030) and ACK request flag set.
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_roce_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	uint32_t flags = DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH;
	doca_error_t result;
	int entry_index = 0;
	uint8_t dest_qp_spec[DOCA_FLOW_IB_BTH_DST_QP_LEN] = {0x10, 0x20, 0x30};
	uint8_t flags1_ackreq_set = 0x80;
	uint8_t opcodes[NB_ENTRIES] = {BTH_OPCODE_UD_SEND, BTH_OPCODE_EXTENDED};

	memset(&match, 0, sizeof(match));

	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match.outer.roce_v2.udp.l4_port.src_port = DOCA_HTOBE16(ROCE_V2_DEFAULT_SRC_PORT);
	match.outer.roce_v2.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_ROCEV2_DEFAULT_PORT);
	match.outer.roce_v2.bth.dest_qp[0] = dest_qp_spec[0];
	match.outer.roce_v2.bth.dest_qp[1] = dest_qp_spec[1];
	match.outer.roce_v2.bth.dest_qp[2] = dest_qp_spec[2];
	match.outer.roce_v2.bth.flags1 = flags1_ackreq_set;

	for (entry_index = 0; entry_index < NB_ENTRIES; entry_index++) {
		match.outer.roce_v2.bth.opcode = opcodes[entry_index];

		/* last entry should be inserted with DOCA_FLOW_ENTRY_FLAGS_NO_WAIT flag */
		if (entry_index == NB_ENTRIES - 1)
			flags = DOCA_FLOW_ENTRY_FLAGS_NO_WAIT;

		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match,
							0,
							NULL,
							NULL,
							NULL,
							flags,
							status,
							&entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/* Context structure for statistics printing */
struct switch_stats_context {
	struct doca_flow_pipe_entry **entries;
};

/*
 * Print RoCE flow statistics for matched BTH entries
 * Displays packet and byte counters for each flow entry
 *
 * @entries [in]: array of flow entries
 */
static void print_switch_stats(struct doca_flow_pipe_entry *entries[])
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
 * @context [in]: switch_stats_context structure
 */
static void print_switch_stats_wrapper(void *context)
{
	struct switch_stats_context *ctx = (struct switch_stats_context *)context;
	print_switch_stats(ctx->entries);
}

/*
 * Run flow_roce_bth_header sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @devs_manager [in]: Array of DOCA devices for the switch ports
 * @nb_devs [in]: Amount of eswtich manager dev bundles in the switch_manager_devs array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_roce_bth_header(int nb_queues, int nb_ports, struct flow_devs_manager devs_manager[], uint16_t nb_devs)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipe1;
	struct entries_status status;
	doca_error_t result;

	memset(&status, 0, sizeof(status));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = NB_ENTRIES; /* counter per entry */

	result = init_doca_flow(nb_queues, "switch,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NB_ENTRIES));
	result = init_doca_flow_switch_ports(devs_manager, nb_devs, ports, nb_ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	result = create_roce_pipe(doca_flow_port_switch_get(ports[0]), &pipe1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_roce_pipe_entries(pipe1, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to the pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_entries_process(doca_flow_port_switch_get(ports[0]), 0, DEFAULT_TIMEOUT_US, NB_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	if (status.nb_processed != (NB_ENTRIES) || status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_BAD_STATE;
	}

	/* Setup statistics context and wait for packets */
	struct switch_stats_context stats_ctx = {.entries = entries};

	flow_wait_for_packets(15, print_switch_stats_wrapper, &stats_ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
