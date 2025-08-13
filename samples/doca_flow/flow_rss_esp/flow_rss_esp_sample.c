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

#include <string.h>
#include <unistd.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_RSS_ESP);

/* The number of packets in the rx queue */
#define PACKET_BURST 256

/* Get the percentage according to part and total */
#define GET_PERCENTAGE(part, total) (((double)part / (double)total) * 100)

/* The number of seconds app waits for traffic to come */
#define WAITING_TIME 15

/*
 * Create DOCA Flow pipe with header existence match, copy action, counter and forward RSS.
 *
 * @port [in]: port of the pipe.
 * @nb_rss_queues [in]: number of RSS queues used by pipe FWD action.
 * @pipe [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_rss_esp_pipe(struct doca_flow_port *port,
					uint32_t nb_rss_queues,
					struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_action_desc desc;
	struct doca_flow_action_descs descs = {.nb_action_desc = 1, .desc_array = &desc};
	struct doca_flow_action_descs *desc_list[] = {&descs};
	struct doca_flow_monitor counter;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint16_t *rss_queues;
	doca_error_t result;
	uint32_t i;

	memset(&match, 0, sizeof(match));
	memset(&desc, 0, sizeof(desc));
	memset(&counter, 0, sizeof(counter));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	rss_queues = calloc(nb_rss_queues, sizeof(*rss_queues));
	if (rss_queues == NULL) {
		DOCA_LOG_ERR("Failed to allocate RSS queues array memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		free(rss_queues);
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RSS_ESP_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* IPv4 and ESP existing match */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ESP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* prepare copy action */
	desc.type = DOCA_FLOW_ACTION_COPY;
	desc.field_op.src.field_string = "tunnel.esp.spi";
	desc.field_op.dst.field_string = "meta.data";
	desc.field_op.width = 32;
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, NULL, NULL, desc_list, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg descs: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	counter.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &counter);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg counter: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* RSS queue - distribute matched traffic between queues */
	for (i = 0; i < nb_rss_queues; ++i)
		rss_queues[i] = i;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_ESP;
	fwd.rss.nr_queues = nb_rss_queues;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	free(rss_queues);
	return result;
}

/*
 * Add DOCA Flow pipe entry.
 *
 * @pipe [in]: pipe of the entry.
 * @port [in]: port of the entry.
 * @status [in]: user context for adding entry
 * @entry [out]: created entry pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_rss_esp_pipe_entry(struct doca_flow_pipe *pipe,
					   struct doca_flow_port *port,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(status, 0, sizeof(*status));

	result = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, 0, status, entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to insert ESP RSS entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = flow_process_entries(port, status, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process ESP RSS entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Dequeue packets from DPDK queues and calculates the distribution.
 *
 * @port_id [in]: port id for dequeue packets.
 * @nb_queues [in]: number of RSS queues.
 * @entry [in]: entry sent packets before distribution.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t rss_distribution_results(uint16_t port_id, uint16_t nb_queues, struct doca_flow_pipe_entry *entry)
{
	struct rte_mbuf *packets[PACKET_BURST];
	struct doca_flow_resource_query stats;
	double actuall_percentage;
	uint32_t total_packets;
	uint16_t nb_packets;
	uint16_t queue_index;
	doca_error_t result;
	int i, j;

	result = doca_flow_resource_query_entry(entry, &stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Port %u failed to query RSS ESP entry: %s", port_id, doca_error_get_descr(result));
		return result;
	}

	total_packets = stats.counter.total_pkts;
	if (total_packets == 0) {
		DOCA_LOG_DBG("Port %d doesn't receive any packet", port_id);
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("Port %d RSS distribution information:", port_id);

	for (i = 0; i < nb_queues; i++) {
		queue_index = i;
		nb_packets = rte_eth_rx_burst(port_id, queue_index, packets, PACKET_BURST);
		actuall_percentage = GET_PERCENTAGE(nb_packets, total_packets);

		DOCA_LOG_INFO("Queue %u received %u packets which is %g%% of the traffic (%u/%u)",
			      queue_index,
			      nb_packets,
			      actuall_percentage,
			      nb_packets,
			      total_packets);

		for (j = 0; j < nb_packets; j++) {
			uint32_t metadata = 0;

			if (rte_flow_dynf_metadata_avail())
				metadata = *RTE_FLOW_DYNF_METADATA(packets[j]);

			DOCA_LOG_DBG("Queue %u packet %d ESP SPI is 0x%08x", queue_index, j, metadata);
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_rss_esp sample
 *
 * @nb_steering_queues [in]: number of steering queues the sample will use
 * @nb_rss_queues [in]: number of rss queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_rss_esp(int nb_steering_queues, int nb_rss_queues)
{
	const int nb_ports = 2;
	struct flow_resources resource = {.nr_counters = 1};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe_entry *entries[nb_ports];
	struct doca_flow_pipe *pipe;
	struct entries_status status;
	doca_error_t result;
	int port_id;

	result = init_doca_flow(nb_steering_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(1));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = create_rss_esp_pipe(ports[port_id], nb_rss_queues, &pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %d failed to create pipe: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_rss_esp_pipe_entry(pipe, ports[port_id], &status, &entries[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %d failed to add entry: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	DOCA_LOG_INFO("Wait %d seconds for packets to arrive", WAITING_TIME);
	sleep(WAITING_TIME);

	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = rss_distribution_results(port_id, nb_rss_queues, entries[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %d failed to query entry: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
