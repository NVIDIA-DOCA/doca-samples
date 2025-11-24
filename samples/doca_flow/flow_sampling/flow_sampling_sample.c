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

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_bitfield.h>

#include "doca_error.h"
#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_SAMPLING);

/* The number of bits in random field */
#define RANDOM_WIDTH 16

/* The number of numbers in random field range */
#define RANDOM_TOTAL_RANGE (1 << RANDOM_WIDTH)

/* Get the percentage according to part and total */
#define GET_PERCENTAGE(part, total) (((double)part / (double)total) * 100)

/* The number of seconds app waits for traffic to come */
#define WAITING_TIME 15

/*
 * Create DOCA Flow pipe with changeable 5 tuple match as root
 *
 * @port [in]: port of the pipe
 * @next_pipe [in]: next pipe pointer
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port,
				     struct doca_flow_pipe *next_pipe,
				     struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	/* 5 tuple match */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;
	match.outer.tcp.l4_port.src_port = 0xffff;
	match.outer.tcp.l4_port.dst_port = 0xffff;

	/* Add counter to see how many packet arrived before sampling */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

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
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
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

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the root pipe that forwards the traffic to specific random pipe.
 *
 * @pipe [in]: pipe of the entries.
 * @status [in]: user context for adding entry.
 * @entry [out]: created entry pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_root_pipe_entry(struct doca_flow_pipe *pipe,
					struct entries_status *status,
					struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;

	memset(&match, 0, sizeof(match));

	match.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 1, 1, 1);
	match.outer.tcp.l4_port.dst_port = DOCA_HTOBE16(80);
	match.outer.tcp.l4_port.src_port = DOCA_HTOBE16(1234);

	DOCA_LOG_DBG("Adding root pipe entry matching of: "
		     "IPv4(src='1.1.1.1',dst='8.8.8.8') and TCP(src_port=1234,dst_port=80)");

	return doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, status, entry);
}

/*
 * Calculate the random value used to achieve given certain percentage.
 *
 * @percentage [in]: the certain percentage user wish to get in sampling.
 * @return: value to use in match.parser_meta.random field for getting this percentage along with
 *          "parser_meta.random.value" string in condition argument.
 */
static uint16_t get_random_value(double percentage)
{
	uint16_t random_value;
	double temp;

	temp = (percentage / 100) * RANDOM_TOTAL_RANGE;
	random_value = (uint16_t)temp;
	temp = GET_PERCENTAGE(random_value, RANDOM_TOTAL_RANGE);

	DOCA_LOG_DBG("Using random value 0x%04x for sampling %g%% of traffic (%g%% requested)",
		     random_value,
		     temp,
		     percentage);

	return random_value;
}

/*
 * Add DOCA Flow pipe for sampling according to random value
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_random_sampling_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SAMPLING_PIPE", DOCA_FLOW_PIPE_CONTROL, false);
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
 * Add DOCA Flow pipe entry to the random sampling pipe.
 *
 * @pipe [in]: pipe of the entries
 * @status [in]: user context for adding entry
 * @percentage [in]: the certain percentage user wish to get in sampling
 * @entry [out]: created entry pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_random_sampling_pipe_entry(struct doca_flow_pipe *pipe,
						   struct entries_status *status,
						   double percentage,
						   struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_match_condition condition;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&condition, 0, sizeof(condition));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	condition.operation = DOCA_FLOW_COMPARE_LT;
	/* Argument field is random */
	condition.field_op.a.field_string = "parser_meta.random.value";
	condition.field_op.a.bit_offset = 0;
	/* Base is immediate value, so the string should be NULL and value is taken from match structure */
	condition.field_op.b.field_string = NULL;
	condition.field_op.b.bit_offset = 0;
	condition.field_op.width = RANDOM_WIDTH;

	/*
	 * The immediate value to compare with random field is provided in the match structure.
	 * Match mask structure is not relevant here since it is masked in argument using offset + width.
	 */
	match.parser_meta.random = DOCA_HTOBE16(get_random_value(percentage));

	/* Add counter to see how many packet are sampled */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 1;

	return doca_flow_pipe_control_add_entry(0 /* queue */,
						0 /* priority */,
						pipe,
						&match,
						NULL /* match_mask */,
						&condition,
						NULL /* actions */,
						NULL /* actions_mask */,
						NULL /* action_descs */,
						&monitor,
						&fwd,
						status,
						entry);
}

/*
 * Get results about certain percentage sampling.
 *
 * @root_entry [in]: entry sent packets to sampling.
 * @random_entry [in]: entry samples certain percentage of traffic.
 * @requested_percentage [in]: the certain percentage user wished to get in sampling.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t random_sampling_results(struct doca_flow_pipe_entry *root_entry,
					    struct doca_flow_pipe_entry *random_entry,
					    double requested_percentage)
{
	struct doca_flow_resource_query root_query_stats;
	struct doca_flow_resource_query random_query_stats;
	double actuall_percentage;
	uint32_t total_packets;
	uint32_t nb_sampled_packets;
	doca_error_t result;

	result = doca_flow_resource_query_entry(root_entry, &root_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query root entry: %s", doca_error_get_descr(result));
		return result;
	}

	total_packets = root_query_stats.counter.total_pkts;
	if (total_packets == 0)
		return DOCA_SUCCESS;

	result = doca_flow_resource_query_entry(random_entry, &random_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query random entry: %s", doca_error_get_descr(result));
		return result;
	}

	nb_sampled_packets = random_query_stats.counter.total_pkts;
	actuall_percentage = GET_PERCENTAGE(nb_sampled_packets, total_packets);

	DOCA_LOG_INFO("Sampling result information (%g%% is requested):", requested_percentage);
	DOCA_LOG_INFO("This pipeline samples %u packets which is %g%% of the traffic (%u/%u)",
		      nb_sampled_packets,
		      actuall_percentage,
		      nb_sampled_packets,
		      total_packets);

	return DOCA_SUCCESS;
}

/* Context structure for statistics printing */
struct sampling_stats_context {
	struct doca_flow_pipe_entry *root_entry;
	struct doca_flow_pipe_entry *random_entry;
	double requested_percentage;
};

/*
 * Print sampling statistics
 *
 * @root_entry [in]: root entry
 * @random_entry [in]: random entry
 * @requested_percentage [in]: requested sampling percentage
 */
static void print_sampling_stats(struct doca_flow_pipe_entry *root_entry,
				 struct doca_flow_pipe_entry *random_entry,
				 double requested_percentage)
{
	doca_error_t result;

	/* Show the results for sampling */
	result = random_sampling_results(root_entry, random_entry, requested_percentage);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to show sampling results: %s", doca_error_get_descr(result));
	}
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: sampling_stats_context structure
 */
static void print_sampling_stats_wrapper(void *context)
{
	struct sampling_stats_context *ctx = (struct sampling_stats_context *)context;
	print_sampling_stats(ctx->root_entry, ctx->random_entry, ctx->requested_percentage);
}

/*
 * Run flow_sampling sample.
 *
 * This sample tests the sampling certain percentage of traffic.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_sampling(int nb_queues, struct flow_switch_ctx *ctx)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	struct doca_flow_port *switch_port;
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *root_pipe;
	struct doca_flow_pipe *sampling_pipe;
	struct doca_flow_pipe_entry *root_entry;
	struct doca_flow_pipe_entry *random_entry;
	struct entries_status status;
	double requested_percentage = 35;
	uint32_t num_of_entries = 2;
	doca_error_t result;

	memset(&status, 0, sizeof(status));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = num_of_entries;

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

	result = create_random_sampling_pipe(switch_port, &sampling_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create random sampling pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_random_sampling_pipe_entry(sampling_pipe, &status, requested_percentage, &random_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add random sampling pipe entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_root_pipe(switch_port, sampling_pipe, &root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_root_pipe_entry(root_pipe, &status, &root_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add root pipe entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = flow_process_entries(switch_port, &status, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	DOCA_LOG_INFO("Wait %u seconds for packets to arrive", WAITING_TIME);

	/* Setup statistics context and wait for packets */
	struct sampling_stats_context stats_ctx = {.root_entry = root_entry,
						   .random_entry = random_entry,
						   .requested_percentage = requested_percentage};

	flow_wait_for_packets(WAITING_TIME, print_sampling_stats_wrapper, &stats_ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
