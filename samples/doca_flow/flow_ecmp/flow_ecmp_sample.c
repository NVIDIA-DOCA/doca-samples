/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>
#include <flow_switch_common.h>

DOCA_LOG_REGISTER(FLOW_ECMP);

/* Get the percentage according to part and total */
#define GET_PERCENTAGE(part, total) (((double)part / (double)total) * 100)

/* The number of seconds app waits for traffic to come */
#define WAITING_TIME 15

#define MAX_ECMP_PORTS (8)
#define MAX_TOTAL_PORTS ((MAX_ECMP_PORTS) + 1)

/*
 * Create DOCA Flow root pipe on the switch port and add its entries.
 *
 * This pipe matches on outer L3 and L4 type,
 * only IPv6 packet and either UDP or TCP packets are forwarded to hash pipe.
 *
 * @port [in]: port of the pipe.
 * @shared_counter_id [in]: shared counter ID to use in monitor for all entries.
 * @next_pipe [in]: pointer to the hash pipe for forwarding to.
 * @status [in]: user context for adding entry.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port,
				     uint32_t shared_counter_id,
				     struct doca_flow_pipe *next_pipe,
				     struct entries_status *status)
{
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6; /* Specific */
	match.parser_meta.outer_l4_type = UINT32_MAX;		  /* Changeable per entry */

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	monitor.shared_counter.shared_counter_id = shared_counter_id; /* Specific */

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
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2);
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	result = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, DOCA_FLOW_WAIT_FOR_BATCH, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP entry into root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	result = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, DOCA_FLOW_NO_WAIT, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP entry into root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Check for given number whether it is power of 2.
 *
 * @x [in]: number to check.
 * @return: true if given number is power of 2, false otherwise.
 */
static inline bool is_power_of_two(uint8_t x)
{
	return (x != 0) && ((x & (x - 1)) == 0);
}

/*
 * Create DOCA Flow hash pipe on the switch port.
 *
 * The hash pipe calculates the entry index based on IPv6 flow label.
 *
 * @port [in]: port of the pipe.
 * @nb_flows [in]: number entries of the pipe.
 * @pipe [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_hash_pipe(struct doca_flow_port *port, uint8_t nb_flows, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	if (!is_power_of_two(nb_flows))
		DOCA_LOG_WARN("Hash pipe nb_flows %u is not power of 2, part of traffic will be lost", nb_flows);

	/* match mask defines which header fields to use in order to calculate the entry index */
	match_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match_mask.outer.ip6.flow_label = DOCA_HTOBE32(0x000fffff);

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "HASH_PIPE", DOCA_FLOW_PIPE_HASH, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nb_flows);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, NULL, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* FWD component is defined per entry */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 0xffff;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the hash pipe.
 *
 * Each entry forwards to different port.
 *
 * @pipe [in]: pipe of the entry.
 * @nb_ecmp_ports [in]: number of ECMP target ports which it number of requested entries.
 * @entries [in]: array of entry pointers to use for adding entry function.
 * @status [in]: user context for adding entry.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_hash_pipe_entries(struct doca_flow_pipe *pipe,
					  uint8_t nb_ecmp_ports,
					  struct doca_flow_pipe_entry **entries,
					  struct entries_status *status)
{
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT};
	doca_error_t result;
	uint16_t target_port;
	uint8_t i;

	for (i = 0; i < nb_ecmp_ports; i++) {
		target_port = i + 1;
		fwd.port_id = target_port;

		/* Last entry should be inserted with DOCA_FLOW_NO_WAIT flag */
		if (i == nb_ecmp_ports - 1)
			flags = DOCA_FLOW_NO_WAIT;

		result = doca_flow_pipe_hash_add_entry(0, pipe, i, NULL, NULL, &fwd, flags, status, &entries[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add hash pipe entry index %u: %s", i, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Show ECMP results.
 *
 * @counter_id [in]: shared counter ID used in root pipe - used to query total traffic.
 * @entries [in]: array of entry pointers from hash pipe - used to query their counters.
 * @nb_ecmp_ports [in]: number of ECMP target ports - it is equal to entries array size.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t show_ecmp_results(uint32_t counter_id, struct doca_flow_pipe_entry **entries, uint8_t nb_ecmp_ports)
{
	struct doca_flow_resource_query root_query_stats;
	struct doca_flow_resource_query hash_query_stats;
	uint32_t total_packets;
	uint32_t nb_packets;
	double percentage;
	doca_error_t result;
	uint8_t i;

	result = doca_flow_shared_resources_query(DOCA_FLOW_SHARED_RESOURCE_COUNTER, &counter_id, &root_query_stats, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query root pipe shared counter: %s", doca_error_get_descr(result));
		return result;
	}

	total_packets = root_query_stats.counter.total_pkts;
	if (total_packets == 0) {
		DOCA_LOG_DBG("No traffic is arrived, no results");
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("Show ECMP results, %u packets are distributed into %u ports:", total_packets, nb_ecmp_ports);

	for (i = 0; i < nb_ecmp_ports; i++) {
		result = doca_flow_resource_query_entry(entries[i], &hash_query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query hash pipe entry %u: %s", i, doca_error_get_descr(result));
			return result;
		}

		nb_packets = hash_query_stats.counter.total_pkts;
		percentage = GET_PERCENTAGE(nb_packets, total_packets);

		DOCA_LOG_INFO("Port %u received %u packets which is %g%% of the traffic (%u/%u)",
			      i + 1,
			      nb_packets,
			      percentage,
			      nb_packets,
			      total_packets);
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_ecmp sample.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_ecmp(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	struct doca_flow_shared_resource_cfg cfg = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct flow_resources resource = {0};
	uint32_t shared_counter_id = 0;
	struct doca_flow_port *switch_port;
	struct doca_flow_port *ports[MAX_TOTAL_PORTS];
	struct doca_flow_pipe *hash_pipe;
	uint32_t actions_mem_size[MAX_TOTAL_PORTS];
	uint8_t nb_ecmp_ports = nb_ports - 1;
	uint8_t nb_entries = nb_ecmp_ports + 2;
	struct doca_flow_pipe_entry *entries[MAX_ECMP_PORTS];
	struct entries_status status;
	doca_error_t result;

	nr_shared_resources[DOCA_FLOW_SHARED_RESOURCE_COUNTER] = 1;
	resource.nr_counters = nb_entries;

	if (nb_ports > MAX_TOTAL_PORTS) {
		DOCA_LOG_ERR("Number provided ports %d is too big (maximal supported is %d)",
			     nb_ports,
			     MAX_TOTAL_PORTS);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = init_doca_flow(nb_queues, "switch,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(nb_entries));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	switch_port = doca_flow_port_switch_get(NULL);

	result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_COUNTER, shared_counter_id, &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure shared counter for root pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_COUNTER, &shared_counter_id, 1, switch_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind shared counter to port: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_hash_pipe(switch_port, nb_ecmp_ports, &hash_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create hash pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	memset(&status, 0, sizeof(status));

	result = add_hash_pipe_entries(hash_pipe, nb_ecmp_ports, entries, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to hash pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_root_pipe(switch_port, shared_counter_id, hash_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe with entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_entries_process(switch_port, 0, DEFAULT_TIMEOUT_US, nb_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	if (status.nb_processed != nb_entries || status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Wait %u seconds for packets to arrive", WAITING_TIME);
	sleep(WAITING_TIME);

	result = show_ecmp_results(shared_counter_id, entries, nb_ecmp_ports);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to show results: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
