/*
 * Copyright (c) 2025-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "flow_common.h"
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_HASH_FLOODING_PIPE);

#define NB_INGRESS_ROOT 2
#define NB_INGRESS_VPORT 1
#define NB_INGRESS_FWD_PIPE 1
#define NB_EGRESS_ROOT 2
#define NB_EGRESS_HASH_TO_VPORT_1 2
#define NB_EGRESS_HASH_TO_VPORT_2 4
#define NB_EGRESS_HASH_TO_VPORT_MAX NB_EGRESS_HASH_TO_VPORT_2

#define NB_TOTAL \
	(NB_INGRESS_ROOT + NB_INGRESS_VPORT * 2 + NB_INGRESS_FWD_PIPE + NB_EGRESS_ROOT + NB_EGRESS_HASH_TO_VPORT_1 + \
	 NB_EGRESS_HASH_TO_VPORT_2)

enum egress_hash_pipe {
	EGRESS_HASH_PIPE_TO_VPORT_1,
	EGRESS_HASH_PIPE_TO_VPORT_2,
	EGRESS_HASH_PIPE_MAX,
};

static struct doca_flow_pipe *ingress_vport_pipe[EGRESS_HASH_PIPE_MAX];
static struct doca_flow_pipe *ingress_fwd_pipe;
static struct doca_flow_pipe *egress_root_pipe;
static struct doca_flow_pipe *egress_hash_pipes[EGRESS_HASH_PIPE_MAX];

static struct doca_flow_pipe_entry *ingress_root_entries[NB_INGRESS_ROOT];
static struct doca_flow_pipe_entry *ingress_vport_entries[EGRESS_HASH_PIPE_MAX];
static struct doca_flow_pipe_entry *ingress_fwd_entry;
static struct doca_flow_pipe_entry *egress_root_entries[NB_EGRESS_ROOT];
static struct doca_flow_pipe_entry *egress_hash_vport_entries[EGRESS_HASH_PIPE_MAX][NB_EGRESS_HASH_TO_VPORT_MAX];

/*
 * Create DOCA Flow forward pipe on the switch port.
 * The forward pipe forwads packet from ingress to egress root.
 *
 * @port [in]: port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_fwd_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_root_pipe;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SWITCH_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_FWD_PIPE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow ingress vport pipe on the switch port.
 * The traffic will be forwad to the dest VF port. That ingress vport
 * pipe will be used as flooding dest pipe.
 *
 * @port [in]: switch port
 * @pipe_idx [in]: pipe index
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_vport_pipe(struct doca_flow_port *port,
					      uint32_t pipe_idx,
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
	memset(&pipe_cfg, 0, sizeof(pipe_cfg));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

	/* Port ID to forward to is defined per entry */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = pipe_idx + 1;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SWITCH_VPORT_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_VPORT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow hash pipe on the switch port.
 * The flooding dest will be ingress forward pipe in order to verify flooding
 * to ingress pipe.
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ingress_hash_root_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "HASH_PIPE", DOCA_FLOW_PIPE_HASH, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_INGRESS_ROOT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_hash_map_algorithm(pipe_cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg algorithm: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = ingress_fwd_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the const fwd pipe.
 *
 * @pipe [in]: pipe of the entry
 * @entries [in]: pipe of the entries
 * @nb_entries [in]: pipe of the entries
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_const_fwd_pipe_entries(struct doca_flow_pipe *pipe,
					       struct doca_flow_pipe_entry **entries,
					       uint32_t nb_entries,
					       bool is_hash_pipe,
					       struct entries_status *status)
{
	doca_error_t result;
	struct doca_flow_match match;
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	uint32_t entry_index = 0;

	memset(&match, 0, sizeof(match));
	for (entry_index = 0; entry_index < nb_entries; entry_index++) {
		/* last entry should be inserted with DOCA_FLOW_NO_WAIT flag */
		if (entry_index == nb_entries - 1)
			flags = DOCA_FLOW_NO_WAIT;

		if (is_hash_pipe)
			result = doca_flow_pipe_hash_add_entry(0,
							       pipe,
							       entry_index,
							       NULL,
							       NULL,
							       NULL,
							       flags,
							       status,
							       &entries[entry_index]);
		else
			result = doca_flow_pipe_add_entry(0,
							  pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  flags,
							  status,
							  &entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add hash pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow hash pipe on the switch port.
 * The flooding dest will be ingress vport pipe in order to verify flooding
 * from egress to ingress pipe.
 *
 * @port [in]: port of the pipe
 * @nb_entries [in]: number of entries
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_egress_hash_pipe(struct doca_flow_port *port,
					    uint32_t nb_entries,
					    struct doca_flow_pipe **pipe)
{
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

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
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nb_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_hash_map_algorithm(pipe_cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg algorithm: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* FWD component is defined per entry */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = NULL;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to the egress hash pipe.
 *
 * @pipe [in]: pipe of the entry
 * @dst_pipe [in]: dest pipe
 * @nb_entries [in]: number of entries
 * @entries [in]: pointer of entries
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_egress_hash_pipe_entries(struct doca_flow_pipe *pipe,
						 struct doca_flow_pipe *dst_pipe,
						 uint32_t nb_entries,
						 struct doca_flow_pipe_entry **entries,
						 struct entries_status *status)
{
	struct doca_flow_fwd fwd;
	doca_error_t result;
	uint32_t entry_index = 0;

	memset(&fwd, 0, sizeof(fwd));

	for (entry_index = 0; entry_index < nb_entries; entry_index++) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = dst_pipe;

		result = doca_flow_pipe_hash_add_entry(0,
						       pipe,
						       entry_index,
						       NULL,
						       NULL,
						       &fwd,
						       DOCA_FLOW_WAIT_FOR_BATCH,
						       status,
						       &entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add hash pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow egress root pipe with source ip match on the switch port.
 * The foward will be either of the egress hash flooding pipe.
 *
 * @sw_port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_egress_root_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = NULL;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SWITCH_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_EGRESS_ROOT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the egress hash flooding pipe.
 * 1.2.3.4 to egress hash flooding pipe index 0.
 * 1.2.3.5 to egress hash flooding pipe index 1.
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_egress_root_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	int entry_index = 0;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));

	for (entry_index = 0; entry_index < NB_EGRESS_ROOT; entry_index++) {
		match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 2, 3, 4 + entry_index);

		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = egress_hash_pipes[entry_index];

		result = doca_flow_pipe_add_entry(0,
						  pipe,
						  &match,
						  NULL,
						  NULL,
						  &fwd,
						  DOCA_FLOW_WAIT_FOR_BATCH,
						  status,
						  &egress_root_entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_hash_flooding_pipe sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_hash_flooding_pipe(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *hash_pipe;
	struct doca_flow_resource_query query_stats;
	struct entries_status status;
	doca_error_t result;
	uint32_t nb_entries;
	int entry_idx;
	int i;

	memset(&status, 0, sizeof(status));
	resource.nr_counters = NB_TOTAL; /* counter per entry */

	result = init_doca_flow(nb_queues, "switch,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NB_TOTAL));
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

	for (i = 0; i < EGRESS_HASH_PIPE_MAX; i++) {
		result = create_ingress_vport_pipe(doca_flow_port_switch_get(NULL), i, &ingress_vport_pipe[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create rss pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_const_fwd_pipe_entries(ingress_vport_pipe[i],
						    &ingress_vport_entries[i],
						    NB_INGRESS_VPORT,
						    false,
						    &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entries to hash pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	for (i = 0; i < EGRESS_HASH_PIPE_MAX; i++) {
		if (i == EGRESS_HASH_PIPE_TO_VPORT_1)
			nb_entries = NB_EGRESS_HASH_TO_VPORT_1;
		else
			nb_entries = NB_EGRESS_HASH_TO_VPORT_2;
		result = create_egress_hash_pipe(doca_flow_port_switch_get(NULL), nb_entries, &egress_hash_pipes[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create rss pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_egress_hash_pipe_entries(egress_hash_pipes[i],
						      ingress_vport_pipe[i],
						      nb_entries,
						      &egress_hash_vport_entries[i][0],
						      &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entries to hash pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	result = create_egress_root_pipe(doca_flow_port_switch_get(NULL), &egress_root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rss pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_egress_root_pipe_entries(egress_root_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to egress root pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_ingress_fwd_pipe(doca_flow_port_switch_get(NULL), &ingress_fwd_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress fwd pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_const_fwd_pipe_entries(ingress_fwd_pipe, &ingress_fwd_entry, NB_INGRESS_FWD_PIPE, false, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to ingress fwd pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_ingress_hash_root_pipe(doca_flow_port_switch_get(NULL), &hash_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_const_fwd_pipe_entries(hash_pipe, &ingress_root_entries[0], NB_INGRESS_ROOT, true, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entries to hash pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_entries_process(doca_flow_port_switch_get(NULL), 0, DEFAULT_TIMEOUT_US, NB_TOTAL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	if (status.nb_processed != NB_TOTAL || status.failure) {
		DOCA_LOG_ERR("Failed to process entries");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(15);

	/* dump entries counters */
	DOCA_LOG_INFO("Egress root statistics");
	for (entry_idx = 0; entry_idx < NB_EGRESS_ROOT; entry_idx++) {
		result = doca_flow_resource_query_entry(egress_root_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
