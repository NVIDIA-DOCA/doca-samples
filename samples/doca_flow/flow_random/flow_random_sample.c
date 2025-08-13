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

DOCA_LOG_REGISTER(FLOW_RANDOM);

/* The number of packets in the rx queue */
#define PACKET_BURST 256

/* The number of bits in random field */
#define RANDOM_WIDTH 16

/* Get the percentage according to part and total */
#define GET_PERCENTAGE(part, total) (((double)part / (double)total) * 100)

/*
 * Create DOCA Flow pipe with changeable 5 tuple match as root
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
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

	/* Add counter to see how many packet arrived before sampling/distribution */
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
	fwd.next_pipe = NULL;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the root pipe that forwards the traffic to specific random pipe.
 *
 * @pipe [in]: pipe of the entries.
 * @next_pipe [in]: random pipe to forward the matched traffic.
 * @src_ip_addr [in]: the source IP address to match on in this entry.
 * @status [in]: user context for adding entry.
 * @entry [out]: created entry pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_root_pipe_entry(struct doca_flow_pipe *pipe,
					struct doca_flow_pipe *next_pipe,
					doca_be32_t src_ip_addr,
					struct entries_status *status,
					struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	match.outer.ip4.src_ip = src_ip_addr;
	match.outer.tcp.l4_port.dst_port = DOCA_HTOBE16(80);
	match.outer.tcp.l4_port.src_port = DOCA_HTOBE16(1234);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	return doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, &fwd, 0, status, entry);
}

/*
 * Add DOCA Flow pipe entries to the root pipe that forwards the traffic to random pipes.
 *
 * @pipe [in]: pipe of the entries.
 * @status [in]: user context for adding entry
 * @distribution_pipe [in]: distribution random pipe to forward the matched traffic.
 * @sampling_pipe [in]: sampling random pipe to forward the matched traffic.
 * @distribution_entry [out]: created distribution entry pointer.
 * @sampling_entry [out]: created sampling entry pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_root_pipe_entries(struct doca_flow_pipe *pipe,
					  struct entries_status *status,
					  struct doca_flow_pipe *distribution_pipe,
					  struct doca_flow_pipe *sampling_pipe,
					  struct doca_flow_pipe_entry **distribution_entry,
					  struct doca_flow_pipe_entry **sampling_entry)
{
	doca_be32_t src_ip_addr;
	doca_error_t result;

	src_ip_addr = BE_IPV4_ADDR(1, 1, 1, 1);
	result = add_root_pipe_entry(pipe, sampling_pipe, src_ip_addr, status, sampling_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry - go to sampling pipe: %s", doca_error_get_descr(result));
		return result;
	}

	src_ip_addr = BE_IPV4_ADDR(2, 2, 2, 2);
	result = add_root_pipe_entry(pipe, distribution_pipe, src_ip_addr, status, distribution_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry - go to distribution pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Calculate the mask used to achieve given certain percentage.
 *
 * The function finds the nearest supported percentage which is not bigger than requested
 * percentage and calculates mask for it.
 * The supported percentages are: [50, 25, 12.5, ... , 0.0015258789].
 *
 * @percentage [in]: the certain percentage user wish to get in sampling.
 * @return: value to use in match_mask.parser_meta.random field for getting this percentage.
 */
static uint16_t get_random_mask(double percentage)
{
	double next_highest_supported_percentage = 50;
	uint16_t mask;
	uint8_t i;

	for (i = 1; i <= RANDOM_WIDTH; ++i) {
		if (percentage >= next_highest_supported_percentage)
			break;

		next_highest_supported_percentage /= 2;
	}

	if (percentage > next_highest_supported_percentage)
		DOCA_LOG_WARN("Requested %g%% is not supported, converted to %g%% instead",
			      percentage,
			      next_highest_supported_percentage);

	mask = (1 << i) - 1;
	DOCA_LOG_DBG("Get random mask 0x%04x for sampling %g%% of traffic", mask, next_highest_supported_percentage);

	return mask;
}

/*
 * Add DOCA Flow pipe for sampling according to random value
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @percentage [in]: the certain percentage user wish to get in sampling
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_random_sampling_pipe(struct doca_flow_port *port,
						int port_id,
						double percentage,
						struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	/* Calculate the mask according to requested percentage */
	match_mask.parser_meta.random = DOCA_HTOBE16(get_random_mask(percentage));
	/*
	 * Specific value 0, 0 is valid value for any supported percentage.
	 */
	match.parser_meta.random = 0;

	/* Add counter to see how many packet are sampled */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SAMPLING_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the random sampling pipe.
 *
 * @pipe [in]: pipe of the entries
 * @status [in]: user context for adding entry
 * @entry [out]: created entry pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_random_sampling_pipe_entry(struct doca_flow_pipe *pipe,
						   struct entries_status *status,
						   struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;

	memset(&match, 0, sizeof(match));

	/*
	 * The values for both fwd and match structures was provided as specific in pipe creation,
	 * no need to provide fresh information here again.
	 */

	return doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, 0, status, entry);
}

/*
 * Add DOCA Flow hash pipe for distribution according to random value.
 *
 * @port [in]: port of the pipe
 * @nb_flows [in]: number of entries for this pipe.
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_random_distribution_pipe(struct doca_flow_port *port,
						    int nb_flows,
						    struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	uint16_t rss_queues = 0;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match_mask, 0, sizeof(match_mask));
	memset(&fwd, 0, sizeof(fwd));

	/* The distribution is determined by number of entries, we can use full mask */
	match_mask.parser_meta.random = DOCA_HTOBE16(UINT16_MAX);

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "DISTRIBUTION_PIPE", DOCA_FLOW_PIPE_HASH, false);
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

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = &rss_queues;
	fwd.rss.nr_queues = UINT32_MAX;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the random distribution pipe.
 *
 * @pipe [in]: pipe of the entries.
 * @nb_entries [in]: number of entries to add.
 * @status [in]: user context for adding entry.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_random_distribution_pipe_entries(struct doca_flow_pipe *pipe,
							 int nb_entries,
							 struct entries_status *status)
{
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	struct doca_flow_pipe_entry *entry;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	uint16_t queue;
	uint16_t i;

	memset(&fwd, 0, sizeof(fwd));

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = &queue;
	fwd.rss.nr_queues = 1;

	for (i = 0; i < nb_entries; i++) {
		queue = i;

		if (i == nb_entries - 1)
			flags = DOCA_FLOW_NO_WAIT;

		result = doca_flow_pipe_hash_add_entry(0, pipe, i, NULL, NULL, &fwd, flags, status, &entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add hash pipe entry %u: %s", i, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Get results about certain percentage sampling.
 *
 * @port_id [in]: port id.
 * @root_entry [in]: entry sent packets to sampling.
 * @random_entry [in]: entry samples certain percentage of traffic.
 * @requested_percentage [in]: the certain percentage user wished to get in sampling.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t random_sampling_results(uint16_t port_id,
					    struct doca_flow_pipe_entry *root_entry,
					    struct doca_flow_pipe_entry *random_entry,
					    double requested_percentage)
{
	struct doca_flow_resource_query root_query_stats;
	struct doca_flow_resource_query random_query_stats;
	double actuall_percentage;
	uint32_t total_packets;
	uint32_t nb_packets;
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

	nb_packets = random_query_stats.counter.total_pkts;
	actuall_percentage = GET_PERCENTAGE(nb_packets, total_packets);

	DOCA_LOG_INFO("Port %d sampling information (%g%% is requested):", port_id, requested_percentage);
	DOCA_LOG_INFO("This pipeline samples %u packets which is %g%% of the traffic (%u/%u)",
		      nb_packets,
		      actuall_percentage,
		      nb_packets,
		      total_packets);

	return DOCA_SUCCESS;
}

/*
 * Dequeue packets from DPDK queues and calculates the distribution.
 *
 * @port_id [in]: port id for dequeue packets.
 * @nb_queues [in]: number of Rx queues.
 * @root_entry [in]: entry sent packets before distribution.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t random_distribution_results(uint16_t port_id,
						uint16_t nb_queues,
						struct doca_flow_pipe_entry *root_entry)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_resource_query root_query_stats;
	double actuall_percentage, total_percentage = 0;
	uint32_t total_packets, total_dist_packets = 0;
	uint16_t nb_packets, nb_hit_queues = 0;
	doca_error_t result;
	int i;

	result = doca_flow_resource_query_entry(root_entry, &root_query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query root entry in port %u: %s", port_id, doca_error_get_descr(result));
		return result;
	}

	total_packets = root_query_stats.counter.total_pkts;
	if (total_packets == 0)
		return DOCA_SUCCESS;

	DOCA_LOG_INFO("Port %d distribution information:", port_id);

	for (i = 0; i < nb_queues; i++) {
		nb_packets = rte_eth_rx_burst(port_id, i, packets, PACKET_BURST);
		actuall_percentage = GET_PERCENTAGE(nb_packets, total_packets);
		total_dist_packets += nb_packets;
		total_percentage += actuall_percentage;
		nb_hit_queues += nb_packets ? 1 : 0;

		DOCA_LOG_INFO("Queue %u received %u packets which is %g%% of the traffic (%u/%u)",
			      i,
			      nb_packets,
			      actuall_percentage,
			      nb_packets,
			      total_packets);
	}

	DOCA_LOG_INFO("The distribution pipe received %u packets which is %g%% of the traffic (%u/%u)",
		      total_dist_packets,
		      total_percentage,
		      total_dist_packets,
		      total_packets);

	if (nb_hit_queues > 1) {
		DOCA_LOG_INFO("Traffic is distributed on multiple queues");
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_random sample.
 *
 * This sample tests the 2 common use-cases of random matching:
 *  1. Sampling certain percentage of traffic.
 *  2. Random distribution over port/queues.
 *
 * @nb_steering_queues [in]: number of steering queues the sample will use
 * @nb_rss_queues [in]: number of rss queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_random(int nb_steering_queues, int nb_rss_queues)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *root_pipe;
	struct doca_flow_pipe *sampling_pipe;
	struct doca_flow_pipe *distribution_pipe;
	struct doca_flow_pipe_entry *root2sampling_entry[nb_ports];
	struct doca_flow_pipe_entry *root2distribution_entry[nb_ports];
	struct doca_flow_pipe_entry *random_entry[nb_ports];
	struct entries_status status;
	double requested_percentage = 12.5;
	uint32_t num_of_entries = 3 + nb_rss_queues;
	doca_error_t result;
	int port_id;

	memset(&status, 0, sizeof(status));
	resource.nr_counters = num_of_entries;

	result = init_doca_flow(nb_steering_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));

		result = create_random_distribution_pipe(ports[port_id], nb_rss_queues, &distribution_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create random distribution pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_random_distribution_pipe_entries(distribution_pipe, nb_rss_queues, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add random distribution pipe entries: %s",
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_random_sampling_pipe(ports[port_id], port_id, requested_percentage, &sampling_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create random sampling pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_random_sampling_pipe_entry(sampling_pipe, &status, &random_entry[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add random sampling pipe entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_root_pipe(ports[port_id], &root_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_root_pipe_entries(root_pipe,
					       &status,
					       distribution_pipe,
					       sampling_pipe,
					       &root2distribution_entry[port_id],
					       &root2sampling_entry[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add root pipe entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = flow_process_entries(ports[port_id], &status, num_of_entries);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(15);

	for (port_id = 0; port_id < nb_ports; port_id++) {
		/* Show the results for sampling */
		result = random_sampling_results(port_id,
						 root2sampling_entry[port_id],
						 random_entry[port_id],
						 requested_percentage);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to show sampling results in port %u: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		/* Show the results for distribution */
		result = random_distribution_results(port_id, nb_rss_queues, root2distribution_entry[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to show distribution results in port %u: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
