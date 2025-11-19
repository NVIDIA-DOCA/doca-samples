/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdio.h>
#include <unistd.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_dev.h>

#include <flow_common.h>
#include <flow_switch_common.h>

#include "bifurcated_driver_model_core.h"

DOCA_LOG_REGISTER(BIFURCATED_DRIVER_MODEL::Core);

/* Constants */
#define INGRESS_ENTRIES 9
#define EGRESS_ENTRIES 3
#define MAX_PKTS 16
#define PROCESSING_TIMEOUT_SECS 10
#define SWITCH_PORT 0

/* Port indices for the application */
enum port_indices {
	PORT_P0 = 0,
	PORT_LINUX = 1,
	PORT_P1 = 2
};

/* Global pipe and entry handles */
static struct doca_flow_pipe *pipe_ingress;
static struct doca_flow_pipe *pipe_egress;
static struct doca_flow_pipe_entry *ingress_entries[INGRESS_ENTRIES];
static struct doca_flow_pipe_entry *egress_entries[EGRESS_ENTRIES];

/* Handle RX/TX packets on the SWITCH_PORT for testing.
 * As the application uses a multi-port eSwitch configuration, although the hardware exposes multiple PFs,
 * from the RSS and RX/TX perspective there is only one port (index 0) used.
 * Thus, for RX/TX and RSS, only port 0 is relevant even though there are actually two PFs.
 */
static void handle_rx_tx_pkts(void)
{
	int rc;
	uint32_t secs = PROCESSING_TIMEOUT_SECS, nb_rx, i;
	struct rte_mbuf *mbufs[MAX_PKTS];

	rc = rte_flow_dynf_metadata_register();
	if (unlikely(rc)) {
		DOCA_LOG_ERR("Enable metadata failed");
		return;
	}

	while (secs--) {
		sleep(1);
		nb_rx = rte_eth_rx_burst(SWITCH_PORT, 0, mbufs, MAX_PKTS);
		for (i = 0; i < nb_rx; i++) {
			/* Set metadata to trigger egress pipe processing */
			rte_flow_dynf_metadata_set(mbufs[i], PORT_LINUX);

			/* Set the TX metadata flag to enable metadata processing */
			mbufs[i]->ol_flags |= RTE_MBUF_DYNFLAG_TX_METADATA;

			DOCA_LOG_INFO("Packet %u received => set meta to %d (PORT_LINUX)", i, PORT_LINUX);

			rte_eth_tx_burst(SWITCH_PORT, 0, &mbufs[i], 1);
			rte_pktmbuf_free(mbufs[i]);
		}
	}
}

/*
 * Create ingress classifier pipe that matches on port_id, sets meta.oport,
 * and forwards to correct port or RSS
 *
 * @port [in]: DOCA flow port to create the pipe on
 * @pipe [out]: Created pipe handle
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t create_ingress_classifier_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions *actions_arr[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain (ingress_classifier): %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Enable miss counter for tracking packets that miss explicit rules and go to kernel */
	result = doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg miss_counter (ingress_classifier): %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "ingress_classifier", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg (ingress_classifier): %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Monitor configuration */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor (ingress_classifier): %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Match on parser_meta.port_id and outer_l4_type (changeable fields) */
	match.parser_meta.port_id = 0xffff;
	match_mask.parser_meta.port_id = 0xffff;
	match.parser_meta.outer_l4_type = 0xffffffff;
	match_mask.parser_meta.outer_l4_type = 0xffffffff;
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match (ingress_classifier): %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Set actions array for the pipe */
	actions.meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = &actions;
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions (ingress_classifier): %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Create pipe with changeable forward */
	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe (ingress_classifier): %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add entries to the ingress classifier pipe to match on the source port and L4 type.
 *  - For P0/P1 with TCP/UDP: forward to egress pipe with appropriate metadata
 *  - For PORT_LINUX with any L4 type: forward to RSS
 *
 * @pipe [in]: Ingress classifier pipe
 * @status [in]: Entry status structure
 * @nb_queues [in]: Number of queues
 * @egress_pipe [in]: Egress pipe for non-Linux ports
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t add_ingress_classifier_entries(struct doca_flow_pipe *pipe,
						   struct entries_status *status,
						   int nb_queues,
						   struct doca_flow_pipe *egress_pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	doca_error_t result;
	uint16_t rss_queues[nb_queues];
	int entry_idx = 0;

	/* Configure RSS to queue 0 for Linux port entries */
	rss_queues[0] = 0;

	/* PORT_P0 entries - TCP and UDP */
	/* Entry 0: PORT_P0 + TCP -> forward to egress pipe */
	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	match.parser_meta.port_id = PORT_P0;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	actions.meta.pkt_meta = DOCA_HTOBE32(PORT_P1);
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_pipe;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_P0 TCP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Entry 1: PORT_P0 + UDP -> forward to egress pipe */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_P0 UDP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* PORT_P1 entries - TCP and UDP */
	/* Entry 2: PORT_P1 + TCP -> forward to egress pipe */
	match.parser_meta.port_id = PORT_P1;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	actions.meta.pkt_meta = DOCA_HTOBE32(PORT_P0);

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_P1 TCP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Entry 3: PORT_P1 + UDP -> forward to egress pipe */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_P1 UDP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* PORT_LINUX entries - all L4 types forward to RSS */
	/* Entry 4: PORT_LINUX + NONE -> forward to RSS */
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.port_id = PORT_LINUX;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_NONE;
	actions.meta.pkt_meta = DOCA_HTOBE32(PORT_LINUX);

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_LINUX NONE: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Entry 5: PORT_LINUX + TCP -> forward to RSS */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_LINUX TCP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Entry 6: PORT_LINUX + UDP -> forward to RSS */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_LINUX UDP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Entry 7: PORT_LINUX + ICMP -> forward to RSS */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ICMP;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_LINUX ICMP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Entry 8: PORT_LINUX + ESP -> forward to RSS */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ESP;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_ESP;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  0,
					  &actions,
					  &monitor,
					  &fwd,
					  0,
					  status,
					  &ingress_entries[entry_idx++]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entry for PORT_LINUX ESP: %s",
			     doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create egress basic pipe that matches on meta.pkt_meta value and forwards traffic to the port specified in
 * meta.pkt_meta
 *
 * @port [in]: DOCA flow port to create the pipe on
 * @pipe [out]: Created pipe handle
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t create_egress_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain (egress_pipe): %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "egress_pipe", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg (egress_pipe): %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Match on metadata with full mask */
	match.meta.pkt_meta = UINT32_MAX;
	match_mask.meta.pkt_meta = UINT32_MAX;
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match (egress_pipe): %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Configure monitor */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor (egress_pipe): %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Forward to changeable port */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 0xffff;
	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe (egress_pipe): %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add 3 egress entries: match meta.pkt_meta (PORT_P0, PORT_LINUX, PORT_P1) and forward to the respective port.
 *
 * @pipe [in]: Egress pipe
 * @status [in]: Entry status structure
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t add_egress_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	/* Entry 1: meta.pkt_meta = PORT_P0 -> forward to port PORT_P0 */
	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.meta.pkt_meta = DOCA_HTOBE32(PORT_P0);
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = PORT_P0;

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, &monitor, &fwd, 0, status, &egress_entries[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress pipe entry for PORT_P0: %s", doca_error_get_descr(result));
		return result;
	}

	/* Entry 2: meta.pkt_meta = PORT_LINUX -> forward to PORT_LINUX */
	match.meta.pkt_meta = DOCA_HTOBE32(PORT_LINUX);
	fwd.port_id = PORT_LINUX;

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, &monitor, &fwd, 0, status, &egress_entries[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress pipe entry for PORT_LINUX: %s", doca_error_get_descr(result));
		return result;
	}

	/* Entry 3: meta.pkt_meta = PORT_P1 -> forward to port PORT_P1 */
	match.meta.pkt_meta = DOCA_HTOBE32(PORT_P1);
	fwd.port_id = PORT_P1;

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, &monitor, &fwd, 0, status, &egress_entries[2]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress pipe entry for PORT_P1: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Print statistics for both ingress and egress entries with descriptive information
 */
static void print_flow_statistics(void)
{
	struct doca_flow_resource_query query_stats;
	doca_error_t result;
	int entry_idx;
	const char *ingress_descriptions[] = {
		"Physical Port 0 + TCP (PORT_P0) -> Set metadata to PORT_P1, forward to egress pipe",
		"Physical Port 0 + UDP (PORT_P0) -> Set metadata to PORT_P1, forward to egress pipe",
		"Physical Port 1 + TCP (PORT_P1) -> Set metadata to PORT_P0, forward to egress pipe",
		"Physical Port 1 + UDP (PORT_P1) -> Set metadata to PORT_P0, forward to egress pipe",
		"Linux Port + NONE (PORT_LINUX) -> RSS forwarding",
		"Linux Port + TCP (PORT_LINUX) -> RSS forwarding",
		"Linux Port + UDP (PORT_LINUX) -> RSS forwarding",
		"Linux Port + ICMP (PORT_LINUX) -> RSS forwarding",
		"Linux Port + ESP (PORT_LINUX) -> RSS forwarding"};
	const char *egress_descriptions[] = {"Metadata PORT_P0 -> Forward to Physical Port 0",
					     "Metadata PORT_LINUX -> Forward to Linux Port",
					     "Metadata PORT_P1 -> Forward to Physical Port 1"};

	DOCA_LOG_INFO("=== Bifurcated Driver Model Application Statistics ===");

	/* Print ingress statistics */
	DOCA_LOG_INFO("--- Ingress Classifier Pipe Statistics ---");
	for (entry_idx = 0; entry_idx < INGRESS_ENTRIES; entry_idx++) {
		if (ingress_entries[entry_idx] == NULL) {
			DOCA_LOG_ERR("Ingress entry %d is NULL, cannot query statistics", entry_idx);
			continue;
		}

		result = doca_flow_resource_query_entry(ingress_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query ingress entry %d: %s", entry_idx, doca_error_get_descr(result));
			continue;
		}

		DOCA_LOG_INFO("Ingress Entry %d: %s - Total packets: %ld",
			      entry_idx,
			      ingress_descriptions[entry_idx],
			      query_stats.counter.total_pkts);
	}

	/* Print ingress pipe miss statistics */
	result = doca_flow_resource_query_pipe_miss(pipe_ingress, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query ingress pipe miss: %s", doca_error_get_descr(result));
	} else {
		DOCA_LOG_INFO("Ingress Pipe Miss (non-TCP/UDP from P0/P1 to kernel): %ld packets",
			      query_stats.counter.total_pkts);
	}

	/* Print egress statistics */
	DOCA_LOG_INFO("--- Egress Pipe Statistics ---");
	for (entry_idx = 0; entry_idx < EGRESS_ENTRIES; entry_idx++) {
		if (egress_entries[entry_idx] == NULL) {
			DOCA_LOG_ERR("Egress entry %d is NULL, cannot query statistics", entry_idx);
			continue;
		}

		result = doca_flow_resource_query_entry(egress_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query egress entry %d: %s", entry_idx, doca_error_get_descr(result));
			continue;
		}

		DOCA_LOG_INFO("Egress Entry %d: %s - Total packets: %ld",
			      entry_idx,
			      egress_descriptions[entry_idx],
			      query_stats.counter.total_pkts);
	}
}

doca_error_t bifurcated_driver_model_init(struct application_dpdk_config *app_dpdk_config, struct flow_switch_ctx *ctx)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[app_dpdk_config->port_config.nb_ports];
	uint32_t actions_mem_size[app_dpdk_config->port_config.nb_ports];
	struct entries_status status;
	doca_error_t result;
	int num_entries = INGRESS_ENTRIES + EGRESS_ENTRIES;

	memset(&status, 0, sizeof(status));
	memset(ingress_entries, 0, sizeof(ingress_entries));
	memset(egress_entries, 0, sizeof(egress_entries));

	resource.nr_counters = num_entries + 1; /* +1 for ingress pipe miss counter */

	/* Initialize DOCA Flow */
	result = init_doca_flow(app_dpdk_config->port_config.nb_queues,
				"switch,hws,expert",
				&resource,
				nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	/* Initialize DOCA Flow switch ports */
	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_entries));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     app_dpdk_config->port_config.nb_ports,
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/*
	 * Set the default miss behavior for miss to KERNEL.
	 * This ensures that any non-TCP/UDP traffic from PORT_P0/P1, which does not match the explicit rules,
	 * will be handled by the kernel. This is done on purpose to allow the kernel to process such traffic.
	 */
	result = doca_flow_port_update_default_miss(doca_flow_port_switch_get(ports[0]),
						    DOCA_FLOW_PORT_DEFAULT_MISS_KERNEL,
						    false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update port default miss to kernel: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);
		goto cleanup;
	}

	/* Create egress pipe first - on the switch port which handles all traffic */
	result = create_egress_pipe(doca_flow_port_switch_get(ports[0]), &pipe_egress);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create egress pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);
		goto cleanup;
	}

	/* Add entries to egress pipe */
	result = add_egress_pipe_entries(pipe_egress, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress pipe entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);
		goto cleanup;
	}

	/* Create ingress classifier pipe */
	result = create_ingress_classifier_pipe(doca_flow_port_switch_get(ports[0]), &pipe_ingress);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress classifier pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);
		goto cleanup;
	}

	/* Add entries to ingress classifier pipe */
	result = add_ingress_classifier_entries(pipe_ingress,
						&status,
						app_dpdk_config->port_config.nb_queues,
						pipe_egress);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress classifier entries: %s", doca_error_get_descr(result));
		return result;
	}

	/* Process entries */
	result = doca_flow_entries_process(ports[SWITCH_PORT], 0, DEFAULT_TIMEOUT_US, num_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries for port %d: %s", PORT_P0, doca_error_get_descr(result));
		stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);
		goto cleanup;
	}

	/* Verify all entries were processed successfully */
	if (status.nb_processed != num_entries || status.failure) {
		DOCA_LOG_ERR("Failed to process all entries - processed: %d/%d, failure: %s",
			     status.nb_processed,
			     num_entries,
			     status.failure ? "true" : "false");
		stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);
		goto cleanup;
	}

	/* Handle packet processing */
	DOCA_LOG_INFO("Waiting a few seconds for packets to arrive...");
	handle_rx_tx_pkts();

	/* Print statistics */
	print_flow_statistics();

	result = stop_doca_flow_ports(app_dpdk_config->port_config.nb_ports, ports);

cleanup:
	doca_flow_destroy();
	return result;
}
