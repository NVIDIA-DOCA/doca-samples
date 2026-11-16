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

/* CT Firewall hardware pipeline, built leaf-first. */

#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>
#include <doca_flow_net.h>

#include <flow_common.h>
#include <flow_ct_common.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::PIPELINE);

/* Build-time entries submitted and completed. */
struct build_status {
	struct entries_status status;
	int nb_added;
};

/* Control-pipe add that counts the entry. */
static doca_error_t ctrl_add(struct doca_flow_pipe *pipe,
			     const struct doca_flow_match *match,
			     const struct doca_flow_match *match_mask,
			     const struct doca_flow_monitor *monitor,
			     uint32_t priority,
			     const struct doca_flow_fwd *fwd,
			     struct build_status *bs,
			     struct doca_flow_pipe_entry **entry)
{
	doca_error_t result;

	result = doca_flow_pipe_control_add_entry(0,
						  pipe,
						  match,
						  match_mask,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor,
						  priority,
						  fwd,
						  &bs->status,
						  entry);
	if (result == DOCA_SUCCESS)
		bs->nb_added++;
	return result;
}

/* Basic-pipe add that counts the entry. */
static doca_error_t basic_add(struct doca_flow_pipe *pipe,
			      const struct doca_flow_match *match,
			      const struct doca_flow_actions *actions,
			      const struct doca_flow_monitor *monitor,
			      const struct doca_flow_fwd *fwd,
			      struct build_status *bs,
			      struct doca_flow_pipe_entry **entry)
{
	doca_error_t result;

	result = doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, monitor, fwd, 0, &bs->status, entry);
	if (result == DOCA_SUCCESS)
		bs->nb_added++;
	return result;
}

/* Lower value, higher priority. */
#define PRIO_HIGHEST 0
#define PRIO_HIGH 1
#define PRIO_MID 2
#define PRIO_LOW 3
#define PRIO_LOWEST 4
#define PRIO_MISS 5

/* Forward port set per entry. */
#define CT_FW_FWD_PORT_CHANGEABLE UINT16_MAX

static uint32_t meta_be(uint32_t host)
{
	return rte_cpu_to_be_32(host);
}

/* Every submitted entry must be processed. */
static doca_error_t process_and_check(struct doca_flow_port *port, struct build_status *bs)
{
	doca_error_t result;
	int tries;

	/* A loaded host may need several windows. */
	for (tries = 0; tries < CT_FW_BUILD_DRAIN_TRIES && bs->status.nb_processed < bs->nb_added; tries++) {
		result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, bs->nb_added - bs->status.nb_processed);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process pipeline entries: %s", doca_error_get_descr(result));
			return result;
		}
		if (bs->status.failure)
			break;
	}
	if (bs->status.failure) {
		DOCA_LOG_ERR("Pipeline entry processing reported failure");
		return DOCA_ERROR_BAD_STATE;
	}
	if (bs->status.nb_processed != bs->nb_added) {
		DOCA_LOG_ERR("Pipeline drain processed %d of %d entries", bs->status.nb_processed, bs->nb_added);
		return DOCA_ERROR_BAD_STATE;
	}
	return DOCA_SUCCESS;
}

/* One RSS pipe per L3 family; each uplink's entry stamps its port code. */
static doca_error_t create_rss_pipe(struct ct_fw_ctx *app,
				    struct doca_flow_port *port,
				    const char *name,
				    uint32_t rss_l3_flag,
				    struct build_status *bs,
				    struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match, match_mask;
	struct doca_flow_actions actions, *actions_arr[1];
	struct doca_flow_actions actions_mask, *actions_mask_arr[1];
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd, miss;
	uint16_t rss_queues[CT_FW_MAX_WORKERS];
	doca_error_t result;
	uint16_t i;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&actions_mask, 0, sizeof(actions_mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&miss, 0, sizeof(miss));
	for (i = 0; i < app->nb_workers; i++)
		rss_queues[i] = app->workers[i].rss_queue;

	match.parser_meta.port_id = CT_FW_MATCH_FIELD_CHANGEABLE;
	match_mask.parser_meta.port_id = CT_FW_MATCH_FIELD_CHANGEABLE;
	/* Only the port bits change. */
	actions.meta.pkt_meta = CT_FW_ACT_META_CHANGEABLE;
	actions_mask.meta.pkt_meta = meta_be(CT_FW_META_PORT_MASK);
	actions_arr[0] = &actions;
	actions_mask_arr[0] = &actions_mask;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, name, DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, actions_mask_arr, NULL, 1);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_miss_counter(cfg, true);
	if (result != DOCA_SUCCESS)
		goto destroy;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.nr_queues = app->nb_workers;
	/* TCP and UDP both punt, so hash on L3 only. */
	fwd.rss.outer_flags = rss_l3_flag;
	fwd.rss.rss_hash_func = DOCA_FLOW_RSS_HASH_FUNCTION_SYMMETRIC_TOEPLITZ;
	miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(cfg, &fwd, &miss, pipe);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create %s pipe: %s", name, doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < app->nb_ports; i++) {
		memset(&match, 0, sizeof(match));
		memset(&actions, 0, sizeof(actions));
		match.parser_meta.port_id = i;
		actions.meta.pkt_meta = meta_be(CT_FW_PORT_CODE(i));
		result = basic_add(*pipe, &match, &actions, NULL, &fwd, bs, NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add %s entry for port %u: %s", name, i, doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* Target of every software punt. */
static doca_error_t create_rss_to_cpu(struct ct_fw_ctx *app,
				      struct doca_flow_port *port,
				      struct build_status *bs,
				      struct doca_flow_pipe **pipe)
{
	struct ct_fw_pipes *pipes = &app->pipes;
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd, miss;
	doca_error_t result;

	result = create_rss_pipe(app, port, "RSS_TO_CPU_V4", DOCA_FLOW_RSS_IPV4, bs, &pipes->rss_v4);
	if (result != DOCA_SUCCESS)
		return result;

	if (!app->has_ipv6_ct) {
		*pipe = pipes->rss_v4;
		return DOCA_SUCCESS;
	}

	result = create_rss_pipe(app, port, "RSS_TO_CPU_V6", DOCA_FLOW_RSS_IPV6, bs, &pipes->rss_v6);
	if (result != DOCA_SUCCESS)
		return result;

	/* IPv6 to the IPv6 pipe; the rest misses to IPv4. */
	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&miss, 0, sizeof(miss));
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->rss_v6;
	miss.type = DOCA_FLOW_FWD_PIPE;
	miss.next_pipe = pipes->rss_v4;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "RSS_L3_SPLIT", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_create(cfg, &fwd, &miss, &pipes->rss_l3_split);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up traffic steering to software: %s", doca_error_get_descr(result));
		return result;
	}

	result = basic_add(pipes->rss_l3_split, &match, NULL, NULL, NULL, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up traffic steering to software (IPv6): %s", doca_error_get_descr(result));
		return result;
	}
	*pipe = pipes->rss_l3_split;
	return DOCA_SUCCESS;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* A replay clone that missed CT is dropped, not punted again. */
static doca_error_t create_miss_handler(struct doca_flow_port *port, struct ct_fw_pipes *pipes, struct build_status *bs)
{
	struct doca_flow_pipe **pipe = &pipes->miss_handler;
	struct doca_flow_match match, mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	match.meta.pkt_meta = CT_FW_MATCH_META_CHANGEABLE;
	mask.meta.pkt_meta = meta_be(CT_FW_META_REINJECT_MASK);
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "MISS_HANDLER", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_match(cfg, &match, &mask);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_create(cfg, &fwd, NULL, pipe);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Miss Handler pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* Marked clone: drop. */
	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	match.meta.pkt_meta = meta_be(CT_FW_META_REINJECT_MASK);
	fwd.type = DOCA_FLOW_FWD_DROP;
	result = basic_add(*pipe, &match, NULL, &monitor, &fwd, bs, &pipes->miss_mark_drop_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add Miss Handler drop entry: %s", doca_error_get_descr(result));
		return result;
	}

	/* Unmarked: punt to software. */
	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->rss_to_cpu;
	result = basic_add(*pipe, &match, NULL, &monitor, &fwd, bs, &pipes->ct_miss_entry);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add Miss Handler new-connection entry: %s", doca_error_get_descr(result));
	return result;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* Validated traffic leaves by the other uplink. */
static doca_error_t create_port_forward(struct ct_fw_ctx *app,
					struct doca_flow_port *port,
					struct build_status *bs,
					struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match, mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd, miss;
	doca_error_t result;
	uint16_t i;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&miss, 0, sizeof(miss));
	match.parser_meta.port_id = CT_FW_MATCH_FIELD_CHANGEABLE;
	mask.parser_meta.port_id = CT_FW_MATCH_FIELD_CHANGEABLE;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = CT_FW_FWD_PORT_CHANGEABLE;
	miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "PORT_FORWARD", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_match(cfg, &match, &mask);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_miss_counter(cfg, true);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_create(cfg, &fwd, &miss, pipe);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Port Forward pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* The counters are indexed by output port. */
	for (i = 0; i < app->nb_ports; i++) {
		uint16_t out = (i == CT_FW_FIRST_UPLINK) ? CT_FW_SECOND_UPLINK : CT_FW_FIRST_UPLINK;
		struct doca_flow_pipe_entry *entry = NULL;

		memset(&match, 0, sizeof(match));
		memset(&fwd, 0, sizeof(fwd));
		match.parser_meta.port_id = i;
		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = out;
		result = basic_add(*pipe, &match, NULL, &monitor, &fwd, bs, &entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add Port Forward entry %u: %s", i, doca_error_get_descr(result));
			return result;
		}
		app->pipes.fwd_entries[out] = entry;
	}
	app->pipes.nb_fwd_entries = app->nb_ports;
	return DOCA_SUCCESS;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

static doca_error_t add_syndrome_entry(struct ct_fw_pipes *pipes,
				       uint8_t syndrome_value,
				       uint8_t syndrome_mask,
				       uint32_t priority,
				       const struct doca_flow_fwd *fwd,
				       struct build_status *bs)
{
	struct doca_flow_match match, mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_pipe_entry *entry = NULL;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&monitor, 0, sizeof(monitor));
	match.parser_meta.tcp_window_validation_syndrome = syndrome_value;
	mask.parser_meta.tcp_window_validation_syndrome = syndrome_mask;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = ctrl_add(pipes->syndrome_check, &match, &mask, &monitor, priority, fwd, bs, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up window-validation rule: %s", doca_error_get_descr(result));
		return result;
	}
	if (pipes->nb_syn_entries < ARRAY_DIM(pipes->syn_entries))
		pipes->syn_entries[pipes->nb_syn_entries++] = entry;
	return DOCA_SUCCESS;
}

/* Default-deny; only VALID and UDP forward in hardware. */
static doca_error_t create_syndrome_check(struct doca_flow_port *port,
					  struct ct_fw_pipes *pipes,
					  struct build_status *bs)
{
	struct doca_flow_match match, mask;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd_fwd, fwd_cpu, fwd_drop;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "SYNDROME_CHECK", DOCA_FLOW_PIPE_CONTROL, false);
	if (result != DOCA_SUCCESS) {
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}
	result = doca_flow_pipe_create(cfg, NULL, NULL, &pipes->syndrome_check);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up window-validation stage: %s", doca_error_get_descr(result));
		return result;
	}

	memset(&fwd_fwd, 0, sizeof(fwd_fwd));
	memset(&fwd_cpu, 0, sizeof(fwd_cpu));
	memset(&fwd_drop, 0, sizeof(fwd_drop));
	fwd_fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd_fwd.next_pipe = pipes->port_forward;
	fwd_cpu.type = DOCA_FLOW_FWD_PIPE;
	fwd_cpu.next_pipe = pipes->rss_to_cpu;
	fwd_drop.type = DOCA_FLOW_FWD_DROP;

	/* 0: a marked clone has done its job. */
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	match.meta.pkt_meta = meta_be(CT_FW_META_REINJECT_MASK);
	mask.meta.pkt_meta = meta_be(CT_FW_META_REINJECT_MASK);
	{
		struct doca_flow_monitor mon;

		memset(&mon, 0, sizeof(mon));
		mon.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = ctrl_add(pipes->syndrome_check,
				  &match,
				  &mask,
				  &mon,
				  PRIO_HIGHEST,
				  &fwd_drop,
				  bs,
				  &pipes->reinject_drop_entry);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add reinject drop: %s", doca_error_get_descr(result));
		return result;
	}

	/* 1: UDP has no syndrome. */
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	mask.parser_meta.outer_l4_type = (enum doca_flow_l4_meta)UINT8_MAX;
	result = ctrl_add(pipes->syndrome_check, &match, &mask, NULL, PRIO_HIGH, &fwd_fwd, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up UDP bypass rule: %s", doca_error_get_descr(result));
		return result;
	}

	/* 2: VALID -> forward. */
	result = add_syndrome_entry(pipes,
				    DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_VALID,
				    CT_FW_SYN_STATE_FLAGS_MASK,
				    PRIO_MID,
				    &fwd_fwd,
				    bs);
	if (result != DOCA_SUCCESS)
		return result;
	/* 3: STATE_CHANGE -> CPU, software forwards. */
	result = add_syndrome_entry(pipes,
				    DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_STATE_CHANGE,
				    CT_FW_SYN_STATE_FLAGS_MASK,
				    PRIO_MID,
				    &fwd_cpu,
				    bs);
	if (result != DOCA_SUCCESS)
		return result;
	/* 4: INVALID -> drop. */
	result = add_syndrome_entry(pipes,
				    DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_INVALID,
				    CT_FW_SYN_STATE_BAD_MASK,
				    PRIO_LOW,
				    &fwd_drop,
				    bs);
	if (result != DOCA_SUCCESS)
		return result;
	/* 5: DISABLED -> CPU, re-enable or remove. */
	result = add_syndrome_entry(pipes,
				    DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_DISABLED,
				    CT_FW_SYN_STATE_BAD_MASK,
				    PRIO_LOWEST,
				    &fwd_cpu,
				    bs);
	if (result != DOCA_SUCCESS)
		return result;

	/* miss: any other encoding -> drop. */
	memset(&match, 0, sizeof(match));
	result = ctrl_add(pipes->syndrome_check, &match, NULL, NULL, PRIO_MISS, &fwd_drop, bs, NULL);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to set up default-deny rule: %s", doca_error_get_descr(result));
	return result;
}

/* Typed by L4 so the checksum is fixed up. */
static void fill_nat_template(struct doca_flow_actions *act, enum ct_fw_family family, uint8_t l4_type)
{
	uint32_t i;

	if (family == CT_FW_FAMILY_IPV6) {
		act->outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		for (i = 0; i < RTE_DIM(act->outer.ip6.src_ip); i++) {
			act->outer.ip6.src_ip[i] = CT_FW_ACT_ADDR_CHANGEABLE;
			act->outer.ip6.dst_ip[i] = CT_FW_ACT_ADDR_CHANGEABLE;
		}
	} else {
		act->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		act->outer.ip4.src_ip = CT_FW_ACT_ADDR_CHANGEABLE;
		act->outer.ip4.dst_ip = CT_FW_ACT_ADDR_CHANGEABLE;
	}
	act->outer.l4_type_ext = l4_type;
	act->outer.transport.src_port = CT_FW_ACT_PORT_CHANGEABLE;
	act->outer.transport.dst_port = CT_FW_ACT_PORT_CHANGEABLE;
}

/* Hits go to Syndrome Check, misses to Miss Handler. */
static doca_error_t create_ct_pipe(struct ct_fw_ctx *app, struct doca_flow_port *port, struct ct_fw_pipes *pipes)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions[CT_FW_ACT_NB];
	struct doca_flow_actions *actions_arr[CT_FW_ACT_NB];
	struct doca_flow_actions *actions_mask_arr[CT_FW_ACT_NB];
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd, fwd_miss;
	uint32_t nb_ipv6 = app->has_ipv6_ct ? app->config.nb_sessions : 0;
	enum ct_fw_family nat_family;
	bool nat_templates;
	uint32_t i;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));
	/* Templates only for the translated family. */
	nat_templates = ct_fw_nat_family(&app->config, &nat_family) &&
			(nat_family == CT_FW_FAMILY_IPV4 || app->has_ipv6_ct);
	if (nat_templates) {
		fill_nat_template(&actions[CT_FW_ACT_IDX_TCP], nat_family, DOCA_FLOW_L4_TYPE_EXT_TCP);
		fill_nat_template(&actions[CT_FW_ACT_IDX_UDP], nat_family, DOCA_FLOW_L4_TYPE_EXT_UDP);
		for (i = 0; i < CT_FW_ACT_NB; i++) {
			actions_arr[i] = &actions[i];
			actions_mask_arr[i] = &actions[i];
		}
	}

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "CT_PIPE", DOCA_FLOW_PIPE_CT, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	/* Totals, shared across the CT queues. */
	result = doca_flow_pipe_cfg_set_ct_connections(cfg, app->config.nb_sessions, nb_ipv6, 0);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_ct_max_connections_per_zone(cfg, CT_DEFAULT_MAX_ZONE_SESSIONS);
	if (result != DOCA_SUCCESS)
		goto destroy;
	/* Private data holds the connection key. */
	result = doca_flow_pipe_cfg_set_ct_entry_private_data_size(cfg, sizeof(struct ct_fw_key));
	if (result != DOCA_SUCCESS)
		goto destroy;
	if (nat_templates) {
		result = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, actions_mask_arr, NULL, CT_FW_ACT_NB);
		if (result != DOCA_SUCCESS)
			goto destroy;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS)
		goto destroy;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->syndrome_check;
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = pipes->miss_handler;
	result = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipes->ct);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create CT pipe: %s", doca_error_get_descr(result));
	return result;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* Inner VLAN ID 1..4095; inner 0 keeps the outer zone. */
static doca_error_t create_zone_inner_pipe(struct doca_flow_port *port,
					   struct ct_fw_pipes *pipes,
					   struct build_status *bs)
{
	struct doca_flow_match match, match_mask;
	struct doca_flow_actions actions, actions_mask, *actions_arr[1], *actions_mask_arr[1];
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	uint32_t vid;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&actions_mask, 0, sizeof(actions_mask));
	memset(&fwd, 0, sizeof(fwd));
	match.outer.l2_valid_headers = DOCA_FLOW_L2_VALID_HEADER_VLAN_0 | DOCA_FLOW_L2_VALID_HEADER_VLAN_1;
	match_mask.outer.l2_valid_headers = UINT16_MAX;
	match.outer.eth_vlan[1].tci = CT_FW_MATCH_FIELD_CHANGEABLE;
	match_mask.outer.eth_vlan[1].tci = rte_cpu_to_be_16(CT_FW_VLAN_VID_MASK);
	actions.meta.pkt_meta = CT_FW_ACT_META_CHANGEABLE;
	actions_mask.meta.pkt_meta = meta_be(CT_FW_META_INNER_VID_MASK);
	actions_arr[0] = &actions;
	actions_mask_arr[0] = &actions_mask;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->ct;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "ZONE_INNER", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_nr_entries(cfg, CT_FW_VLAN_VID_MAX);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, actions_mask_arr, NULL, 1);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_create(cfg, &fwd, &fwd, &pipes->zone_inner);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create inner Zone pipe: %s", doca_error_get_descr(result));
		return result;
	}

	for (vid = 1; vid <= CT_FW_VLAN_VID_MAX; vid++) {
		match.outer.eth_vlan[1].tci = rte_cpu_to_be_16((uint16_t)vid);
		actions.meta.pkt_meta = meta_be(vid << CT_FW_META_INNER_VID_SHIFT);
		result = basic_add(pipes->zone_inner, &match, &actions, NULL, &fwd, bs, NULL);
		if (result == DOCA_SUCCESS && vid % CT_FW_BUILD_BATCH == 0)
			result = process_and_check(port, bs);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add inner Zone entry %u: %s", vid, doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* Zone from the outer (and inner) 802.1Q VLAN ID; else 0. */
static doca_error_t create_zone_pipe(struct doca_flow_port *port, struct ct_fw_pipes *pipes, struct build_status *bs)
{
	struct doca_flow_match match, match_mask;
	struct doca_flow_action_desc desc;
	struct doca_flow_action_descs descs, *descs_arr[1] = {&descs};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd, fwd_miss;
	doca_error_t result;

	result = create_zone_inner_pipe(port, pipes, bs);
	if (result != DOCA_SUCCESS)
		return result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&desc, 0, sizeof(desc));
	memset(&descs, 0, sizeof(descs));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));
	match.parser_meta.outer_l2_type = DOCA_FLOW_L2_META_SINGLE_VLAN;
	match_mask.parser_meta.outer_l2_type = (enum doca_flow_l2_meta)UINT8_MAX;
	desc.type = DOCA_FLOW_ACTION_COPY;
	desc.field_op.src.field_string = "outer.eth_vlan0.tci";
	desc.field_op.src.bit_offset = 0;
	desc.field_op.dst.field_string = "meta.data";
	desc.field_op.dst.bit_offset = CT_FW_META_ZONE_SHIFT;
	desc.field_op.width = CT_FW_VLAN_VID_BITS;
	descs.nb_action_desc = 1;
	descs.desc_array = &desc;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->zone_inner;
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = pipes->ct;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "ZONE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_flow_pipe_cfg_set_actions(cfg, NULL, NULL, descs_arr, 1);
	if (result != DOCA_SUCCESS)
		goto destroy;

	result = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipes->zone);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Zone pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = basic_add(pipes->zone, &match, NULL, NULL, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add Zone entry: %s", doca_error_get_descr(result));
	return result;

destroy:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* Drops all but TCP, UDP, fragments, ARP and ICMP (IPv4 ICMP only under IPv6 NAT). */
static doca_error_t create_ingress_root(struct ct_fw_ctx *app,
					struct doca_flow_port *port,
					struct ct_fw_pipes *pipes,
					struct build_status *bs)
{
	struct doca_flow_match match, mask;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "INGRESS_ROOT", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}
	result = doca_flow_pipe_create(cfg, NULL, NULL, &pipes->ingress_root);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up ingress stage: %s", doca_error_get_descr(result));
		return result;
	}

	/* No IPv6 CT: drop and count IPv6. */
	if (!app->has_ipv6_ct) {
		struct doca_flow_monitor monitor;

		memset(&match, 0, sizeof(match));
		memset(&mask, 0, sizeof(mask));
		memset(&fwd, 0, sizeof(fwd));
		memset(&monitor, 0, sizeof(monitor));
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT8_MAX;
		fwd.type = DOCA_FLOW_FWD_DROP;
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = ctrl_add(pipes->ingress_root,
				  &match,
				  &mask,
				  &monitor,
				  PRIO_HIGHEST,
				  &fwd,
				  bs,
				  &pipes->ipv6_drop_entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add IPv6 drop: %s", doca_error_get_descr(result));
			return result;
		}
	}

	/* Fragments go to software. */
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&fwd, 0, sizeof(fwd));
	match.parser_meta.outer_ip_fragmented = 1;
	mask.parser_meta.outer_ip_fragmented = UINT8_MAX;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->rss_to_cpu;
	result = ctrl_add(pipes->ingress_root, &match, &mask, NULL, PRIO_HIGH, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send fragments to software: %s", doca_error_get_descr(result));
		return result;
	}

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->zone;
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	mask.parser_meta.outer_l4_type = (enum doca_flow_l4_meta)UINT8_MAX;
	{
		struct doca_flow_monitor monitor;

		memset(&monitor, 0, sizeof(monitor));
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = ctrl_add(pipes->ingress_root,
				  &match,
				  &mask,
				  &monitor,
				  PRIO_MID,
				  &fwd,
				  bs,
				  &pipes->ingress_tcp_entry);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP continue: %s", doca_error_get_descr(result));
		return result;
	}
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	mask.parser_meta.outer_l4_type = (enum doca_flow_l4_meta)UINT8_MAX;
	result = ctrl_add(pipes->ingress_root, &match, &mask, NULL, PRIO_MID, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP continue: %s", doca_error_get_descr(result));
		return result;
	}

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->rss_to_cpu;
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	match.outer.eth.type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);
	mask.outer.eth.type = UINT16_MAX;
	result = ctrl_add(pipes->ingress_root, &match, &mask, NULL, PRIO_MID, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send ARP to software: %s", doca_error_get_descr(result));
		return result;
	}
	/* ICMP and ICMPv6 (L4 meta ICMP matches both); IPv4 ICMP only under IPv6 NAT, which does not translate ICMPv6.
	 * Without IPv6 CT, ICMPv6 is dropped first. */
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ICMP;
	mask.parser_meta.outer_l4_type = (enum doca_flow_l4_meta)UINT8_MAX;
	if (app->config.nat[CT_FW_FAMILY_IPV6].enabled) {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT8_MAX;
	}
	result = ctrl_add(pipes->ingress_root, &match, &mask, NULL, PRIO_MID, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send ICMP to software: %s", doca_error_get_descr(result));
		return result;
	}

	/* Everything else is dropped. */
	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_DROP;
	result = ctrl_add(pipes->ingress_root, &match, NULL, NULL, PRIO_MISS, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add ingress default drop: %s", doca_error_get_descr(result));
	return result;
}

/* Software transmit: the port code picks the destination. */
static doca_error_t create_egress_root(struct ct_fw_ctx *app,
				       struct doca_flow_port *port,
				       struct ct_fw_pipes *pipes,
				       struct build_status *bs)
{
	struct doca_flow_match match, mask;
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	uint16_t i;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS)
		return result;
	result = set_flow_pipe_cfg(cfg, "EGRESS_ROOT", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}
	result = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}
	result = doca_flow_pipe_create(cfg, NULL, NULL, &pipes->egress_root);
	doca_flow_pipe_cfg_destroy(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set up egress stage: %s", doca_error_get_descr(result));
		return result;
	}

	/* A marked clone re-enters CT to advance its state. */
	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&fwd, 0, sizeof(fwd));
	match.meta.pkt_meta = meta_be(CT_FW_META_REINJECT_MASK);
	mask.meta.pkt_meta = meta_be(CT_FW_META_REINJECT_MASK);
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->ct;
	result = ctrl_add(pipes->egress_root, &match, &mask, NULL, PRIO_HIGHEST, &fwd, bs, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress reinject entry: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < app->nb_ports; i++) {
		struct doca_flow_monitor monitor;
		struct doca_flow_pipe_entry *entry = NULL;

		memset(&match, 0, sizeof(match));
		memset(&mask, 0, sizeof(mask));
		memset(&fwd, 0, sizeof(fwd));
		memset(&monitor, 0, sizeof(monitor));
		match.meta.pkt_meta = meta_be(CT_FW_PORT_CODE(i));
		mask.meta.pkt_meta = meta_be(CT_FW_META_PORT_MASK);
		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = i;
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = ctrl_add(pipes->egress_root, &match, &mask, &monitor, PRIO_HIGH, &fwd, bs, &entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add egress port entry %u: %s", i, doca_error_get_descr(result));
			return result;
		}
		if (pipes->nb_egress_entries < ARRAY_DIM(pipes->egress_entries))
			pipes->egress_entries[pipes->nb_egress_entries++] = entry;
	}

	/* A transmit with no destination is dropped and counted. */
	{
		struct doca_flow_monitor monitor;

		memset(&match, 0, sizeof(match));
		memset(&mask, 0, sizeof(mask));
		memset(&fwd, 0, sizeof(fwd));
		memset(&monitor, 0, sizeof(monitor));
		fwd.type = DOCA_FLOW_FWD_DROP;
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = ctrl_add(pipes->egress_root,
				  &match,
				  NULL,
				  &monitor,
				  PRIO_MISS,
				  &fwd,
				  bs,
				  &pipes->egress_drop_entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add egress default drop: %s", doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

/* Names the stage that failed; the pipe calls log only their own errors. */
static doca_error_t stage_failed(const char *stage, doca_error_t result)
{
	DOCA_LOG_ERR("Failed to create the %s stage: %s", stage, doca_error_get_descr(result));
	return result;
}

doca_error_t ct_fw_pipeline_build(struct ct_fw_ctx *app)
{
	struct doca_flow_port *port = app->ports[0];
	struct ct_fw_pipes *pipes = &app->pipes;
	/* Entry callbacks also fire on pipe destroy. */
	static struct build_status bs;
	doca_error_t result;

	memset(pipes, 0, sizeof(*pipes));
	memset(&bs, 0, sizeof(bs));

	result = create_rss_to_cpu(app, port, &bs, &pipes->rss_to_cpu);
	if (result != DOCA_SUCCESS)
		return stage_failed("RSS to CPU", result);
	result = create_miss_handler(port, pipes, &bs);
	if (result != DOCA_SUCCESS)
		return stage_failed("Miss Handler", result);
	result = create_port_forward(app, port, &bs, &pipes->port_forward);
	if (result != DOCA_SUCCESS)
		return stage_failed("Port Forward", result);
	result = create_syndrome_check(port, pipes, &bs);
	if (result != DOCA_SUCCESS)
		return stage_failed("Syndrome Check", result);
	result = create_ct_pipe(app, port, pipes);
	if (result != DOCA_SUCCESS)
		return stage_failed("CT", result);
	result = create_zone_pipe(port, pipes, &bs);
	if (result != DOCA_SUCCESS)
		return stage_failed("Zone", result);
	result = create_ingress_root(app, port, pipes, &bs);
	if (result != DOCA_SUCCESS)
		return stage_failed("Ingress Root", result);
	result = create_egress_root(app, port, pipes, &bs);
	if (result != DOCA_SUCCESS)
		return stage_failed("Egress Root", result);

	result = process_and_check(port, &bs);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("CT Firewall pipeline built (%d entries)", bs.status.nb_processed);
	return DOCA_SUCCESS;
}

void ct_fw_pipeline_destroy(struct ct_fw_ctx *app)
{
	struct ct_fw_pipes *pipes = &app->pipes;
	struct doca_flow_pipe *const list[] = {
		/* Roots first: they reference the downstream pipes. */
		pipes->ingress_root,
		pipes->egress_root,
		pipes->zone,
		pipes->zone_inner,
		pipes->ct,
		pipes->syndrome_check,
		pipes->miss_handler,
		pipes->port_forward,
		/* rss_to_cpu aliases one of these. */
		pipes->rss_l3_split,
		pipes->rss_v4,
		pipes->rss_v6,
	};
	size_t i;

	for (i = 0; i < ARRAY_DIM(list); i++) {
		if (list[i] != NULL)
			doca_flow_pipe_destroy(list[i]);
	}
	memset(pipes, 0, sizeof(*pipes));
}
