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

/**
 * @file flow_event_rss_sample.c
 * @brief Delivers VLAN TCI to software via shared EVENT_RSS while forwarding popped packets to a
 *        representor.
 */

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>
#include <flow_switch_common.h>

DOCA_LOG_REGISTER(FLOW_EVENT_RSS);

/* VLAN TCI stored in meta.u32[1]; index passed to doca_flow_event_get_meta_value(). */
#define META_VLAN_TCI_DW_OFFSET 1
#define META_U32_BIT_OFFSET(idx) (offsetof(struct doca_flow_meta, u32[(idx)]) << 3)

#define REPRESENTOR_PORT_ID 1
#define NB_EVENT_QUEUES 8
#define EVENT_QUEUE_DEPTH 128
#define WAIT_SECS 5

static struct doca_flow_pipe *pipe_event_rss;
static struct doca_flow_pipe *pipe_pop_vlan;
static struct doca_flow_pipe_entry *entry_event_rss;
static struct doca_flow_pipe_entry *entry_pop_ok;

struct event_rss_cqe_ctx {
	struct doca_flow_port *port;
	uint32_t cqe_received; /* CQE events delivered to the callback - zero means no traffic was processed */
	bool cqe_cb_failure;
};

static struct event_rss_cqe_ctx event_rss_cqe_ctx;

static void cqe_callback(const struct doca_flow_event *event, uint16_t event_queue_id, void *user_ctx)
{
	struct event_rss_cqe_ctx *ctx = (struct event_rss_cqe_ctx *)user_ctx;
	struct doca_flow_port *port;
	doca_error_t result;
	uint32_t vlan_tci;

	if (ctx == NULL) {
		DOCA_LOG_ERR("CQE callback received NULL user_ctx");
		return;
	}

	ctx->cqe_received++;

	if (ctx->port == NULL) {
		ctx->cqe_cb_failure = true;
		return;
	}

	port = ctx->port;
	result = doca_flow_event_get_meta_value(port, event, META_VLAN_TCI_DW_OFFSET, &vlan_tci);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get meta value: %s", doca_error_get_descr(result));
		ctx->cqe_cb_failure = true;
		return;
	}

	DOCA_LOG_INFO("cqe_callback: vlan_tci = %u, event_queue = %d", vlan_tci, event_queue_id);
}

static doca_error_t event_rss_port_config_cb(struct doca_flow_port_cfg *port_cfg, int port_id, void *config_cb_ctx)
{
	doca_error_t result;

	(void)config_cb_ctx;
	if (port_id != 0)
		return DOCA_SUCCESS;

	result =
		doca_flow_port_cfg_set_nr_event_queues(port_cfg, DOCA_FLOW_EVENT_TYPE_CUSTOM_METADATA, NB_EVENT_QUEUES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr_event_queues: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_flow_port_cfg_set_event_queue_depth(port_cfg,
							  DOCA_FLOW_EVENT_TYPE_CUSTOM_METADATA,
							  EVENT_QUEUE_DEPTH);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set event_queue_depth: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_flow_port_cfg_set_cb_event_process(port_cfg,
							 DOCA_FLOW_EVENT_TYPE_CUSTOM_METADATA,
							 cqe_callback,
							 &event_rss_cqe_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cb_event_process: %s", doca_error_get_descr(result));
		return result;
	}
	return DOCA_SUCCESS;
}

static doca_error_t create_pipe_pop_vlan(struct doca_flow_port *sw_port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_actions actions, *actions_arr[1];
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions.pop_vlan = true;
	actions_arr[0] = &actions;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = REPRESENTOR_PORT_ID;

	memset(&monitor, 0, sizeof(monitor));
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, sw_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = set_flow_pipe_cfg(pipe_cfg, "POP_VLAN_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, 1);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;
	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);

destroy_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t create_pipe_event_rss(struct doca_flow_port *sw_port,
					  uint32_t shared_event_rss_id,
					  struct doca_flow_pipe *next_pipe,
					  struct doca_flow_pipe **pipe)
{
	struct doca_flow_action_descs action_descs;
	struct doca_flow_action_descs *action_descs_arr[1] = {&action_descs};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_match match_mask;
	struct doca_flow_action_desc desc;
	struct doca_flow_monitor monitor;
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&desc, 0, sizeof(desc));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.port_id = 0;
	match.parser_meta.outer_l2_type = DOCA_FLOW_L2_META_SINGLE_VLAN;
	match_mask.parser_meta.port_id = UINT16_MAX;
	match_mask.parser_meta.outer_l2_type = UINT32_MAX;

	desc.type = DOCA_FLOW_ACTION_COPY;
	desc.field_op.src.field_string = "outer.eth_vlan0.tci";
	desc.field_op.src.bit_offset = 0;
	desc.field_op.dst.field_string = "meta.data";
	desc.field_op.dst.bit_offset = META_U32_BIT_OFFSET(META_VLAN_TCI_DW_OFFSET);
	desc.field_op.width = 16;

	action_descs.nb_action_desc = 1;
	action_descs.desc_array = &desc;

	monitor.event_rss_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	monitor.shared_event_rss.shared_event_rss_id = shared_event_rss_id;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, sw_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = set_flow_pipe_cfg(pipe_cfg, "EVENT_RSS_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, NULL, NULL, action_descs_arr, 1);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;
	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);

destroy_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t add_pop_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_actions actions;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions.pop_vlan = true;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = REPRESENTOR_PORT_ID;

	result = doca_flow_pipe_basic_add_entry(0, pipe, NULL, 0, &actions, NULL, &fwd, 0, status, &entry_pop_ok);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

static doca_error_t add_event_rss_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;

	memset(&match, 0, sizeof(match));

	match.parser_meta.port_id = 0;
	match.parser_meta.outer_l2_type = DOCA_FLOW_L2_META_SINGLE_VLAN;

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, status, &entry_event_rss);
}

struct event_rss_stats_context {
	struct doca_flow_pipe_entry *entry_port0;
	struct doca_flow_pipe_entry *entry_port1;
};

static doca_error_t event_rss_entry_port0_stats_get(struct event_rss_stats_context *ctx, uint64_t *nb_packets)
{
	struct doca_flow_resource_query query_stats;
	doca_error_t result;

	result = doca_flow_resource_query_entry(ctx->entry_port0, &query_stats);
	if (result != DOCA_SUCCESS)
		return result;

	*nb_packets = query_stats.counter.total_pkts;
	return DOCA_SUCCESS;
}

static void print_event_rss_stats(struct doca_flow_pipe_entry *entry_port0, struct doca_flow_pipe_entry *entry_port1)
{
	struct doca_flow_resource_query query_stats;
	doca_error_t result;

	result = doca_flow_resource_query_entry(entry_port0, &query_stats);
	if (result == DOCA_SUCCESS) {
		DOCA_LOG_INFO("Port 0 (wire ingress): packets=%" PRIu64 " bytes=%" PRIu64,
			      query_stats.counter.total_pkts,
			      query_stats.counter.total_bytes);
	} else {
		DOCA_LOG_ERR("Failed to query port 0 entry: %s", doca_error_get_descr(result));
	}

	result = doca_flow_resource_query_entry(entry_port1, &query_stats);
	if (result == DOCA_SUCCESS) {
		DOCA_LOG_INFO("Port 1 (representor egress): packets=%" PRIu64 " bytes=%" PRIu64,
			      query_stats.counter.total_pkts,
			      query_stats.counter.total_bytes);
	} else {
		DOCA_LOG_ERR("Failed to query port 1 entry: %s", doca_error_get_descr(result));
	}
}

static void print_event_rss_stats_wrapper(void *context)
{
	struct event_rss_stats_context *ctx = (struct event_rss_stats_context *)context;

	print_event_rss_stats(ctx->entry_port0, ctx->entry_port1);
}

doca_error_t flow_event_rss(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	uint16_t event_queues[NB_EVENT_QUEUES] = {0, 1, 2, 3, 4, 5, 6, 7};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_shared_resource_cfg shared_resource_cfg = {0};
	struct doca_dev_rep *dev_rep_arr[FLOW_COMMON_REPS_MAX];
	struct event_rss_stats_context stats_ctx = {0};
	struct doca_dev *dev_arr[FLOW_COMMON_DEV_MAX];
	struct doca_flow_match event_mask = {0};
	struct doca_flow_port *ports[nb_ports];
	struct flow_resources resource = {0};
	uint32_t actions_mem_size[nb_ports];
	uint64_t nb_packets_counter_p0 = 0;
	uint32_t shared_event_rss_id = 0;
	bool event_rss_acquired = false;
	struct entries_status status;
	doca_error_t result;
	int nb_devs;
	int i, j;

	memset(&status, 0, sizeof(status));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = 2;
	resource.nr_event_rss = 1;

	result = init_doca_flow(nb_queues, "switch", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(4));

	memset(dev_arr, 0, sizeof(dev_arr));
	memset(dev_rep_arr, 0, sizeof(dev_rep_arr));
	nb_devs = 0;
	for (i = 0; i < ctx->devs_ctx.nb_devs; i++) {
		dev_arr[nb_devs++] = ctx->devs_ctx.devs_manager[i].doca_dev;
		for (j = 0; j < ctx->devs_ctx.devs_manager[i].nb_reps; j++)
			dev_rep_arr[nb_devs++] = ctx->devs_ctx.devs_manager[i].doca_dev_rep[j];
	}
	if (nb_devs < nb_ports) {
		DOCA_LOG_ERR("Insufficient DOCA devices: need %d ports, have %d devices", nb_ports, nb_devs);
		doca_flow_destroy();
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = init_doca_flow_ports_with_custom_config(nb_ports,
							 ports,
							 false,
							 dev_arr,
							 dev_rep_arr,
							 event_rss_port_config_cb,
							 NULL,
							 NULL,
							 actions_mem_size,
							 &resource,
							 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA switch ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}
	event_rss_cqe_ctx.port = ports[0];

	event_mask.parser_meta.random = DOCA_HTOBE16(UINT16_MAX);
	shared_resource_cfg.event_rss_cfg.mask = &event_mask;
	shared_resource_cfg.event_rss_cfg.nr_queues = NB_EVENT_QUEUES;
	shared_resource_cfg.event_rss_cfg.event_queues = event_queues;

	result = doca_flow_port_shared_resource_get(doca_flow_port_switch_get(ports[0]),
						    DOCA_FLOW_SHARED_RESOURCE_EVENT_RSS,
						    &shared_event_rss_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get EVENT_RSS shared resource: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	event_rss_acquired = true;

	result = doca_flow_port_shared_resource_set_cfg(doca_flow_port_switch_get(ports[0]),
							DOCA_FLOW_SHARED_RESOURCE_EVENT_RSS,
							shared_event_rss_id,
							&shared_resource_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set EVENT_RSS cfg: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = create_pipe_pop_vlan(doca_flow_port_switch_get(ports[0]), &pipe_pop_vlan);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pop VLAN pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = add_pop_pipe_entry(pipe_pop_vlan, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pop pipe entry: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = create_pipe_event_rss(doca_flow_port_switch_get(ports[0]),
				       shared_event_rss_id,
				       pipe_pop_vlan,
				       &pipe_event_rss);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create EVENT_RSS pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = add_event_rss_pipe_entry(pipe_event_rss, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add EVENT_RSS entry: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = flow_process_entries(doca_flow_port_switch_get(ports[0]), &status, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	stats_ctx.entry_port0 = entry_event_rss;
	stats_ctx.entry_port1 = entry_pop_ok;
	flow_wait_for_packets(WAIT_SECS, print_event_rss_stats_wrapper, &stats_ctx);

	for (i = 0; i < NB_EVENT_QUEUES; i++) {
		result = doca_flow_event_process(ports[0], DOCA_FLOW_EVENT_TYPE_CUSTOM_METADATA, event_queues[i], 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process event: %s, event_queue_id = %d",
				     doca_error_get_descr(result),
				     event_queues[i]);
			goto cleanup;
		}
	}

	result = event_rss_entry_port0_stats_get(&stats_ctx, &nb_packets_counter_p0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get port 0 entry stats");
		goto cleanup;
	}

	if (nb_packets_counter_p0 != 0 && event_rss_cqe_ctx.cqe_received == 0) {
		DOCA_LOG_ERR("Packets were received but no CQE events were processed");
		result = DOCA_ERROR_UNEXPECTED;
		goto cleanup;
	}

	if (event_rss_cqe_ctx.cqe_cb_failure) {
		DOCA_LOG_WARN("CQE callback failure");
		result = DOCA_ERROR_UNEXPECTED;
		goto cleanup;
	}

	DOCA_LOG_INFO("EVENT_RSS: %u CQE event(s) processed successfully", event_rss_cqe_ctx.cqe_received);

cleanup:
	if (pipe_event_rss != NULL)
		doca_flow_pipe_destroy(pipe_event_rss);
	if (event_rss_acquired)
		doca_flow_port_shared_resource_put(doca_flow_port_switch_get(ports[0]),
						   DOCA_FLOW_SHARED_RESOURCE_EVENT_RSS,
						   shared_event_rss_id);
	if (pipe_pop_vlan != NULL)
		doca_flow_pipe_destroy(pipe_pop_vlan);

	stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
