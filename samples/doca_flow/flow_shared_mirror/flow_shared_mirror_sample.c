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

#include <rte_ethdev.h>
#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_SHARED_MIRROR);

/* Set match l4 port */
#define SET_L4_PORT(layer, port, value) \
	do { \
		if (match.layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_TCP) \
			match.layer.tcp.l4_port.port = (value); \
		else if (match.layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_UDP) \
			match.layer.udp.l4_port.port = (value); \
	} while (0)

#define SAMPLE_MIRROR_CONTROL_ENTRY_MAX 2
#define SAMPLE_MIRROR_ENTRY_MAX 4
#define SAMPLE_ENTRY_TOTAL (SAMPLE_MIRROR_CONTROL_ENTRY_MAX + SAMPLE_MIRROR_ENTRY_MAX)

static struct doca_flow_pipe_entry *mirror_entries[SAMPLE_MIRROR_ENTRY_MAX];
static uint32_t mirror_idx = 0;

/*
 * Create DOCA Flow pipe with 5 tuple match and monitor with shared mirror ID
 *
 * @port [in]: port of the pipe
 * @out_l4_type [in]: l4 type to match: UDP/TCP
 * @fwd [in]: pipe fwd
 * @match_src [in]: pipe match with src
 * @mirror_id [in]: pipe mirror_id
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_shared_mirror_pipe(struct doca_flow_port *port,
					      enum doca_flow_l4_type_ext out_l4_type,
					      struct doca_flow_fwd *fwd,
					      bool match_src,
					      uint32_t mirror_id,
					      struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));

	/* 5 tuple match */
	match.outer.l4_type_ext = out_l4_type;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	if (match_src)
		match.outer.ip4.src_ip = UINT32_MAX;
	else
		match.outer.ip4.dst_ip = UINT32_MAX;
	SET_L4_PORT(outer, src_port, UINT16_MAX);
	SET_L4_PORT(outer, dst_port, UINT16_MAX);

	actions_arr[0] = &actions;

	monitor.shared_mirror_id = mirror_id;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SHARED_MIRROR_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the shared mirror pipe
 *
 * @pipe [in]: pipe of the entry
 * @out_l4_type [in]: l4 type to match: UDP/TCP
 * @fwd [in]: entry fwd
 * @match_src [in]: match with src
 * @shared_mirror_id [in]: ID of the shared mirror
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_shared_mirror_pipe_entry(struct doca_flow_pipe *pipe,
						 enum doca_flow_l4_type_ext out_l4_type,
						 struct doca_flow_fwd *fwd,
						 bool match_src,
						 uint32_t shared_mirror_id,
						 struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_monitor monitor;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	/* example 5-tuple to match */
	doca_be32_t dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8);
	doca_be32_t src_ip_addr = BE_IPV4_ADDR(1, 2, 3, 4);
	doca_be16_t dst_port = DOCA_HTOBE16(80);
	doca_be16_t src_port = DOCA_HTOBE16(1234);

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&monitor, 0, sizeof(monitor));

	/* set shared mirror ID */
	monitor.shared_mirror_id = shared_mirror_id;

	if (match_src)
		match.outer.ip4.src_ip = src_ip_addr;
	else
		match.outer.ip4.dst_ip = dst_ip_addr;
	match.outer.l4_type_ext = out_l4_type;
	SET_L4_PORT(outer, dst_port, dst_port);
	SET_L4_PORT(outer, src_port, src_port);

	result = doca_flow_pipe_add_entry(0, pipe, &match, &actions, &monitor, fwd, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		return result;
	}
	mirror_entries[mirror_idx++] = entry;

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow control pipe
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
 * Add DOCA Flow pipe entries to the control pipe. First entry forwards UDP packets to udp_pipe and the second
 * forwards TCP packets to tcp_pipe
 *
 * @control_pipe [in]: pipe of the entries
 * @tcp_pipe [in]: pointer to the TCP pipe to forward packets to
 * @udp_pipe [in]: pointer to the UDP pipe to forward packets to
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_control_pipe_entries(struct doca_flow_pipe *control_pipe,
					     struct doca_flow_pipe *tcp_pipe,
					     struct doca_flow_pipe *udp_pipe,
					     struct entries_status *status)
{
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	enum doca_flow_l4_meta l4_meta_type[SAMPLE_MIRROR_CONTROL_ENTRY_MAX] = {
		DOCA_FLOW_L4_META_UDP,
		DOCA_FLOW_L4_META_TCP,
	};
	struct doca_flow_pipe *next_pipe[SAMPLE_MIRROR_CONTROL_ENTRY_MAX] = {
		udp_pipe,
		tcp_pipe,
	};
	uint8_t priority = 0;
	uint32_t i;
	doca_error_t result;

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	for (i = 0; i < SAMPLE_MIRROR_CONTROL_ENTRY_MAX; i++) {
		match.parser_meta.outer_l4_type = l4_meta_type[i];
		fwd.next_pipe = next_pipe[i];

		result = doca_flow_pipe_control_add_entry(0,
							  priority,
							  control_pipe,
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &fwd,
							  status,
							  NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_shared_mirror sample
 *
 * @nb_ports [in]: number of ports the sample will use
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_shared_mirror(int nb_ports, int nb_queues, struct flow_switch_ctx *ctx)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *tcp_pipe, *udp_pipe, *pipe;
	uint32_t shared_mirror_ids[] = {1, 2};
	struct doca_flow_mirror_target target = {0};
	struct doca_flow_shared_resource_cfg cfg = {0};
	struct doca_flow_resource_mirror_cfg mirror_cfg = {0};
	struct entries_status status;
	struct doca_flow_fwd fwd = {0};
	int num_of_entries = SAMPLE_ENTRY_TOTAL;
	struct doca_flow_resource_query query_stats;
	int i;
	doca_error_t result;

	nr_shared_resources[DOCA_FLOW_SHARED_RESOURCE_MIRROR] = SAMPLE_MIRROR_ENTRY_MAX;
	resource.nr_counters = SAMPLE_MIRROR_ENTRY_MAX;

	result = init_doca_flow(nb_queues, "switch,hws,isolated", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
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

	mirror_cfg.nr_targets = 1;
	target.fwd.type = DOCA_FLOW_FWD_PORT;
	target.fwd.port_id = 1;
	mirror_cfg.target = &target;
	cfg.mirror_cfg = mirror_cfg;

	memset(&status, 0, sizeof(status));

	/* config shared mirror without FWD */
	result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, shared_mirror_ids[0], &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cfg shared mirror");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* config shared mirror with FWD */
	target.fwd.port_id = 0;
	cfg.mirror_cfg.fwd.type = DOCA_FLOW_FWD_PORT;
	cfg.mirror_cfg.fwd.port_id = 0;
	result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, shared_mirror_ids[1], &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cfg shared mirror");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* bind shared mirror to port */
	result = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR, &shared_mirror_ids[0], 2, ports[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind shared mirror to port");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* 1. Pipe with constant has_fwd mirror and NULL FWD */
	result = create_shared_mirror_pipe(ports[0],
					   DOCA_FLOW_L4_TYPE_EXT_TCP,
					   NULL,
					   false,
					   shared_mirror_ids[1],
					   &tcp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_shared_mirror_pipe_entry(tcp_pipe, DOCA_FLOW_L4_TYPE_EXT_TCP, NULL, false, 0, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* 2. Pipe with dynamic has_fwd mirror and NULL FWD */
	result = create_shared_mirror_pipe(ports[0], DOCA_FLOW_L4_TYPE_EXT_UDP, NULL, false, UINT32_MAX, &udp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_shared_mirror_pipe_entry(udp_pipe,
					      DOCA_FLOW_L4_TYPE_EXT_UDP,
					      NULL,
					      false,
					      shared_mirror_ids[1],
					      &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* config fwd */
	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_PIPE;

	/* 3. Pipe with constant mirror and constant FWD */
	fwd.next_pipe = tcp_pipe;
	result = create_shared_mirror_pipe(ports[0],
					   DOCA_FLOW_L4_TYPE_EXT_TCP,
					   &fwd,
					   true,
					   shared_mirror_ids[0],
					   &tcp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_shared_mirror_pipe_entry(tcp_pipe, DOCA_FLOW_L4_TYPE_EXT_TCP, NULL, true, 0, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* 4. Pipe with dynamic mirror and constant FWD */
	fwd.next_pipe = udp_pipe;
	result = create_shared_mirror_pipe(ports[0], DOCA_FLOW_L4_TYPE_EXT_UDP, &fwd, true, UINT32_MAX, &udp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_shared_mirror_pipe_entry(udp_pipe,
					      DOCA_FLOW_L4_TYPE_EXT_UDP,
					      NULL,
					      true,
					      shared_mirror_ids[0],
					      &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = create_control_pipe(ports[0], &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create control pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_control_pipe_entries(pipe, tcp_pipe, udp_pipe, &status);
	if (result != DOCA_SUCCESS) {
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = doca_flow_entries_process(ports[0], 0, DEFAULT_TIMEOUT_US, num_of_entries);
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

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(15);

	for (i = 0; i < SAMPLE_MIRROR_ENTRY_MAX; i++) {
		result = doca_flow_resource_query_entry(mirror_entries[i], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Mirror Entry in index: %d", i);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
