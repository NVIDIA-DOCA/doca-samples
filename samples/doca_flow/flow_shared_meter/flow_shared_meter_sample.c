/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_SHARED_METER);

/* Meter colors green, yellow, red use 2 bits mask */
#define METER_COLOR_MASK 3

/* Set match l4 port */
#define SET_L4_PORT(layer, port, value) \
	do { \
		if (match.layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_TCP) \
			match.layer.tcp.l4_port.port = (value); \
		else if (match.layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_UDP) \
			match.layer.udp.l4_port.port = (value); \
	} while (0)

/*
 * Create DOCA Flow control pipe with match on greed/red colors and their respective forwarding.
 *
 * @port [in]: port of the pipe
 * @fwd_on_green [in]: the forwarding in case there is a green color match
 * @fwd_on_red [in]: the forwarding in case there is a red color match
 * @color_pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_color_matched_pipe(struct doca_flow_port *port,
					      const struct doca_flow_fwd *fwd_on_green,
					      const struct doca_flow_fwd *fwd_on_red,
					      struct doca_flow_pipe **color_pipe)
{
	doca_error_t result;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_match red_match, red_match_mask;
	struct doca_flow_match green_match, green_match_mask;

	memset(&red_match, 0, sizeof(red_match));
	memset(&red_match_mask, 0, sizeof(red_match_mask));
	memset(&green_match, 0, sizeof(green_match));
	memset(&green_match_mask, 0, sizeof(green_match_mask));

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MTR_CTRL_PIPE", DOCA_FLOW_PIPE_CONTROL, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, color_pipe);
	if (result != DOCA_SUCCESS)
		goto destroy_pipe_cfg;
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	red_match.parser_meta.meter_color = DOCA_FLOW_METER_COLOR_RED;
	*(uint8_t *)&red_match_mask.parser_meta.meter_color = METER_COLOR_MASK;
	green_match.parser_meta.meter_color = DOCA_FLOW_METER_COLOR_GREEN;
	*(uint8_t *)&green_match_mask.parser_meta.meter_color = METER_COLOR_MASK;

	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  *color_pipe,
						  &red_match,
						  &red_match_mask,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  fwd_on_red,
						  NULL,
						  NULL);
	if (result)
		return result;
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  *color_pipe,
						  &green_match,
						  &green_match_mask,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  fwd_on_green,
						  NULL,
						  NULL);
	if (result)
		return result;

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe with 5 tuple match and monitor on shared meter ID
 *
 * @port [in]: port of the pipe
 * @out_l4_meta_type [in]: l4 meta type to match: UDP/TCP
 * @out_l4_type [in]: l4 type to match: UDP/TCP
 * @color_pipe [in]: next forwarded pipe to match on color
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_shared_meter_mark_pipe(struct doca_flow_port *port,
						  enum doca_flow_l4_meta out_l4_meta_type,
						  enum doca_flow_l4_type_ext out_l4_type,
						  struct doca_flow_pipe *color_pipe,
						  struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* 5 tuple match */
	match.parser_meta.outer_l4_type = out_l4_meta_type;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = out_l4_type;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;
	SET_L4_PORT(outer, src_port, 0xffff);
	SET_L4_PORT(outer, dst_port, 0xffff);

	actions_arr[0] = &actions;

	/* monitor with changeable shared meter ID */
	monitor.meter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	monitor.shared_meter.shared_meter_id = 0xffffffff;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SHARED_METER_PIPE", DOCA_FLOW_PIPE_BASIC, false);
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

	/* Forward to next pipe to match on marked meter color */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = color_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the shared meter pipe
 *
 * @pipe [in]: pipe of the entry
 * @out_l4_type [in]: l4 type to match: UDP/TCP
 * @shared_meter_id [in]: ID of the shared meter
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_shared_meter_pipe_entry(struct doca_flow_pipe *pipe,
						enum doca_flow_l4_type_ext out_l4_type,
						uint32_t shared_meter_id,
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

	/* set shared meter ID */
	monitor.meter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	monitor.shared_meter.shared_meter_id = shared_meter_id;

	match.outer.ip4.dst_ip = dst_ip_addr;
	match.outer.ip4.src_ip = src_ip_addr;
	match.outer.l4_type_ext = out_l4_type;
	SET_L4_PORT(outer, dst_port, dst_port);
	SET_L4_PORT(outer, src_port, src_port);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, &monitor, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		return result;
	}

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
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	uint8_t priority = 0;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = udp_pipe;

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

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = tcp_pipe;

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
	return DOCA_SUCCESS;
}

/*
 * Run flow_shared_meter sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_shared_meter(int nb_queues)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *tcp_pipe, *udp_pipe, *pipe, *color_pipe;
	int port_id;
	uint32_t shared_meter_ids[] = {UINT32_MAX, UINT32_MAX}; /* Invalid meter id */
	struct doca_flow_shared_resource_cfg cfg = {0};
	struct doca_flow_resource_meter_cfg meter_cfg = {0};
	struct doca_flow_fwd fwd_on_green, fwd_on_red;
	struct entries_status status;
	int num_of_entries = 4;
	doca_error_t result;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_meters = 2;

	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	meter_cfg.limit_type = DOCA_FLOW_METER_LIMIT_TYPE_BYTES;
	meter_cfg.color_mode = DOCA_FLOW_METER_COLOR_MODE_BLIND;
	meter_cfg.alg = DOCA_FLOW_METER_ALGORITHM_TYPE_RFC2697;
	meter_cfg.cir = 1024;
	meter_cfg.cbs = 1024;
	meter_cfg.rfc2697.ebs = 0;
	cfg.meter_cfg = meter_cfg;
	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));
		/* get shared meter id from port and configure it (RFC 2697) */
		result = doca_flow_port_shared_resource_get(ports[port_id],
							    DOCA_FLOW_SHARED_RESOURCE_METER,
							    &shared_meter_ids[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get shared meter id from port %d", port_id);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		result = doca_flow_port_shared_resource_set_cfg(ports[port_id],
								DOCA_FLOW_SHARED_RESOURCE_METER,
								shared_meter_ids[port_id],
								&cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure shared meter to port %d", port_id);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		/* forwarding traffic to other port for passed (green) traffic */
		memset(&fwd_on_green, 0, sizeof(fwd_on_green));
		fwd_on_green.type = DOCA_FLOW_FWD_PORT;
		fwd_on_green.port_id = port_id ^ 1;
		/* drop on failed (red) traffic */
		memset(&fwd_on_red, 0, sizeof(fwd_on_red));
		fwd_on_red.type = DOCA_FLOW_FWD_DROP;

		result = create_color_matched_pipe(ports[port_id], &fwd_on_green, &fwd_on_red, &color_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create color pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_shared_meter_mark_pipe(ports[port_id],
						       DOCA_FLOW_L4_META_TCP,
						       DOCA_FLOW_L4_TYPE_EXT_TCP,
						       color_pipe,
						       &tcp_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_shared_meter_pipe_entry(tcp_pipe,
						     DOCA_FLOW_L4_TYPE_EXT_TCP,
						     shared_meter_ids[port_id],
						     &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_color_matched_pipe(ports[port_id], &fwd_on_green, &fwd_on_red, &color_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create color pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_shared_meter_mark_pipe(ports[port_id],
						       DOCA_FLOW_L4_META_UDP,
						       DOCA_FLOW_L4_TYPE_EXT_UDP,
						       color_pipe,
						       &udp_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_shared_meter_pipe_entry(udp_pipe,
						     DOCA_FLOW_L4_TYPE_EXT_UDP,
						     shared_meter_ids[port_id],
						     &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		result = create_control_pipe(ports[port_id], &pipe);
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

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries);
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
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(5);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
