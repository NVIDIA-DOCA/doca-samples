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

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_FWD_MISS);

/* The number of seconds app waits for traffic to come */
#define WAITING_TIME 5

/*
 * Create pipe configure structure.
 *
 * @port [in]: port of the pipe.
 * @name [in]: name of the pipe.
 * @match [in]: match structure for this pipe.
 * @actions [in]: actions array for this pipe.
 * @descs [in]: action descriptor array for this pipe.
 * @nb_actions [in]: nb_actions of the pipe.
 * @nb_flows [in]: nb_flows of the pipe.
 * @is_root [in]: indicator whether this pipe is root.
 * @miss_counter [in]: pipe has miss counter.
 * @pipe_cfg [out]: created pipe configuration pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_pipe_cfg(struct doca_flow_port *port,
				    const char *name,
				    struct doca_flow_match *match,
				    struct doca_flow_actions **actions,
				    struct doca_flow_action_descs **descs,
				    uint32_t nb_actions,
				    uint32_t nb_flows,
				    bool is_root,
				    bool miss_counter,
				    struct doca_flow_pipe_cfg **pipe_cfg)
{
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, name, DOCA_FLOW_PIPE_BASIC, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(cfg, nb_flows);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg number entries: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}

	result = doca_flow_pipe_cfg_set_miss_counter(cfg, miss_counter);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg miss counter: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}

	result = doca_flow_pipe_cfg_set_match(cfg, match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(cfg);
		return result;
	}

	if (actions || descs) {
		result = doca_flow_pipe_cfg_set_actions(cfg, actions, NULL, descs, nb_actions);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
			doca_flow_pipe_cfg_destroy(cfg);
			return result;
		}
	}

	*pipe_cfg = cfg;
	return DOCA_SUCCESS;
}

/*
 * Destroy pipe configure structure.
 *
 * @cfg [in]: pipe cfg structure to destroy.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t destroy_pipe_cfg(struct doca_flow_pipe_cfg *cfg)
{
	return doca_flow_pipe_cfg_destroy(cfg);
}

/*
 * Create basic pipe and its configure structure.
 *
 * @port [in]: port of the pipe.
 * @name [in]: name of the pipe.
 * @match [in]: match structure for this pipe.
 * @actions [in]: actions array for this pipe.
 * @descs [in]: action descriptor array for this pipe.
 * @fwd [in]: action descriptor array for this pipe.
 * @fwd_miss [in]: action descriptor array for this pipe.
 * @nb_actions [in]: nb_actions of the pipe.
 * @nb_flows [in]: nb_flows of the pipe.
 * @is_root [in]: indicator whether this pipe is root.
 * @miss_counter [in]: pipe has miss counter.
 * @pipe [out]: created pipe configuration pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_basic_pipe(struct doca_flow_port *port,
				      const char *name,
				      struct doca_flow_match *match,
				      struct doca_flow_actions **actions,
				      struct doca_flow_action_descs **descs,
				      struct doca_flow_fwd *fwd,
				      struct doca_flow_fwd *fwd_miss,
				      uint32_t nb_actions,
				      uint32_t nb_flows,
				      bool is_root,
				      bool miss_counter,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	result = create_pipe_cfg(port, name, match, actions, descs, nb_actions, nb_flows, is_root, miss_counter, &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe, configuration failed");
		return result;
	}

	result = doca_flow_pipe_create(cfg, fwd, fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe, pipe creation failed");
		destroy_pipe_cfg(cfg);
		return result;
	}

	result = destroy_pipe_cfg(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe, configuration destruction failed");
		doca_flow_pipe_destroy(*pipe);
		return result;
	}

	DOCA_LOG_DBG("Basic pipe %s is created successfully", name);
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe that modify IP field and goes to the pair port.
 *
 * @port [in]: port of the pipe.
 * @fwd [in]: forward the traffic that hit the pipe rule.
 * @status [in]: user context for adding entry.
 * @pipe_ptr [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_modify_pipe(struct doca_flow_port *port,
				       struct doca_flow_fwd *fwd,
				       struct entries_status *status,
				       struct doca_flow_pipe **pipe_ptr)
{
	struct doca_flow_match match = {.parser_meta = {.outer_l3_type = UINT32_MAX}};
	struct doca_flow_actions actions = {0};
	struct doca_flow_action_desc ip4_desc = {0};
	struct doca_flow_action_desc ip6_desc = {0};
	struct doca_flow_action_descs ip4_descs = {.nb_action_desc = 1, .desc_array = &ip4_desc};
	struct doca_flow_action_descs ip6_descs = {.nb_action_desc = 1, .desc_array = &ip6_desc};
	struct doca_flow_action_descs *descs_arr[] = {&ip4_descs, &ip6_descs};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	ip4_desc.type = DOCA_FLOW_ACTION_COPY;
	ip4_desc.field_op.src.field_string = "outer.ipv4.version_ihl";
	ip4_desc.field_op.dst.field_string = "outer.ipv4.dscp_ecn";
	ip4_desc.field_op.width = 4;

	ip6_desc.type = DOCA_FLOW_ACTION_COPY;
	ip6_desc.field_op.src.field_string = "outer.ipv6.payload_len";
	ip6_desc.field_op.dst.field_string = "outer.ipv6.traffic_class";
	ip6_desc.field_op.width = 8;

	result = create_basic_pipe(port, "MODIFY_PIPE", &match, NULL, descs_arr, fwd, NULL, 2, 2, false, false, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create modify pipe, pipe creation failed");
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create modify pipe, IPv4 entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 1, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create modify pipe, IPv6 entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	*pipe_ptr = pipe;
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe that push VLAN header and goes to the pair port.
 *
 * @port [in]: port of the pipe.
 * @fwd [in]: forward the traffic that hit the pipe rule.
 * @status [in]: user context for adding entry.
 * @pipe_ptr [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_push_pipe(struct doca_flow_port *port,
				     struct doca_flow_fwd *fwd,
				     struct entries_status *status,
				     struct doca_flow_pipe **pipe_ptr)
{
	struct doca_flow_match match = {0};
	struct doca_flow_actions actions = {0};
	struct doca_flow_actions *actions_arr[] = {&actions};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	actions.has_push = true;
	actions.push.type = DOCA_FLOW_PUSH_ACTION_VLAN;
	actions.push.vlan.eth_type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_VLAN);
	actions.push.vlan.vlan_hdr.tci = DOCA_HTOBE16(0x1234);

	result = create_basic_pipe(port, "PUSH_PIPE", &match, actions_arr, NULL, fwd, NULL, 1, 1, false, false, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create push pipe, pipe creation failed");
		return result;
	}

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create push pipe, entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	*pipe_ptr = pipe;
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe matching on IP addresses.
 *
 * @port [in]: port of the pipe.
 * @fwd [in]: forward the traffic that hit the pipe rule.
 * @modify_pipe [in]: pipe to forward the traffic that didn't hit the pipe rule.
 * @status [in]: user context for adding entry.
 * @pipe_ptr [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ipv4_pipe(struct doca_flow_port *port,
				     struct doca_flow_fwd *fwd,
				     struct doca_flow_pipe *modify_pipe,
				     struct entries_status *status,
				     struct doca_flow_pipe **pipe_ptr)
{
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = modify_pipe};
	struct doca_flow_match match = {0};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 2, 3, 4);
	match.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);

	result = create_basic_pipe(port, "IPV4_PIPE", &match, NULL, NULL, fwd, &fwd_miss, 0, 1, false, false, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IPv4 pipe, pipe creation failed");
		return result;
	}

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IPv4 pipe, entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	*pipe_ptr = pipe;
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe that match on outer IP type and send IPv4 to next pipe and IPv6 to miss pipe.
 *
 * @port [in]: port of the pipe.
 * @ipv4_pipe [in]: pipe to forward the traffic that hit the pipe rule.
 * @modify_pipe [in]: pipe to forward the traffic that didn't hit the pipe rule.
 * @status [in]: user context for adding entry.
 * @pipe_ptr [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_ip_selector_pipe(struct doca_flow_port *port,
					    struct doca_flow_pipe *ipv4_pipe,
					    struct doca_flow_pipe *modify_pipe,
					    struct entries_status *status,
					    struct doca_flow_pipe **pipe_ptr)
{
	struct doca_flow_match match = {.parser_meta = {.outer_l3_type = DOCA_FLOW_L3_META_IPV4}};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = ipv4_pipe};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = modify_pipe};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	result = create_basic_pipe(port,
				   "IP_SELECTOR_PIPE",
				   &match,
				   NULL /* actions */,
				   NULL /* descs */,
				   &fwd,
				   &fwd_miss,
				   0 /* nb_actions */,
				   1 /* nb_flows */,
				   false /* is_root */,
				   true /* miss_counter */,
				   &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ip selector pipe, pipe creation failed");
		return result;
	}

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ip selector pipe, entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	*pipe_ptr = pipe;
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow root pipe that modify IP field and directs packets to the next_pipe (IP selector pipe).
 *
 * @port [in]: port of the pipe.
 * @next_pipe [in]: IP selector pipe to forward to.
 * @status [in]: user context for adding entry.
 * @pipe_ptr [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port,
				     struct doca_flow_pipe *next_pipe,
				     struct entries_status *status,
				     struct doca_flow_pipe **pipe_ptr)
{
	struct doca_flow_match match = {.parser_meta = {.outer_l3_type = UINT32_MAX}};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = next_pipe};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	result = create_basic_pipe(port, "ROOT_PIPE", &match, NULL, NULL, &fwd, &fwd_miss, 0, 2, true, true, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe, pipe creation failed");
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe, IPv4 entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe, IPv6 entry adding failed");
		doca_flow_pipe_destroy(pipe);
		return result;
	}

	*pipe_ptr = pipe;
	return DOCA_SUCCESS;
}

/*
 * Query the miss counters and show the results.
 *
 * @root_pipe [in]: root pipe containing miss counter for drop action.
 * @ip_selector_pipe [in]: IP selector pipe containing miss counter for group action.
 * @miss_is_updated [in]: indicator whether miss updating is done.
 * @port_id [in]: port ID of the pipes.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t miss_counters_query(struct doca_flow_pipe *root_pipe,
					struct doca_flow_pipe *ip_selector_pipe,
					bool miss_is_updated,
					int port_id)
{
	struct doca_flow_resource_query query_stats;
	doca_error_t result;

	result = doca_flow_resource_query_pipe_miss(root_pipe, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Port %u failed to query root pipe miss: %s", port_id, doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Port %d root pipe miss %ld packets %s updating",
		      port_id,
		      query_stats.counter.total_pkts,
		      miss_is_updated ? "after" : "before");

	result = doca_flow_resource_query_pipe_miss(ip_selector_pipe, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Port %u failed to query IP selector pipe miss: %s",
			     port_id,
			     doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Port %d IP selector pipe miss %ld packets %s updating",
		      port_id,
		      query_stats.counter.total_pkts,
		      miss_is_updated ? "after" : "before");

	return DOCA_SUCCESS;
}

/*
 * Update the the pipe target of FWD miss action.
 *
 * @pipe [in]: pipe to update its miss action.
 * @next_pipe [in]: a new pipe to forward the traffic that didn't hit the pipe rule.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t update_miss_fwd_next_pipe(struct doca_flow_pipe *pipe, struct doca_flow_pipe *next_pipe)
{
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = next_pipe};

	return doca_flow_pipe_update_miss(pipe, &fwd_miss);
}

enum {
	ROOT,
	IP_SELECTOR,
	IPV4,
	MODIFY,
	PUSH,
	NUMBER_OF_PIPES,
};

/*
 * Run flow_fwd_miss sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_fwd_miss(int nb_queues)
{
	const int nb_ports = 2;
	struct flow_resources resource = {.nr_counters = 4};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *port, *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe **pipes, *pipes_array[nb_ports][NUMBER_OF_PIPES];
	struct doca_flow_fwd fwd_port = {.type = DOCA_FLOW_FWD_PORT};
	struct entries_status status;
	uint32_t num_of_entries = 6;
	doca_error_t result;
	int port_id;

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

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));
		pipes = pipes_array[port_id];
		port = ports[port_id];
		fwd_port.port_id = port_id ^ 1;

		DOCA_LOG_DBG("Port %u starts preparing the first pipeline", port_id);

		result = create_modify_pipe(port, &fwd_port, &status, &pipes[MODIFY]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to create modify pipe: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_ipv4_pipe(port, &fwd_port, pipes[MODIFY], &status, &pipes[IPV4]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to create IPv4 pipe: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_ip_selector_pipe(port, pipes[IPV4], pipes[MODIFY], &status, &pipes[IP_SELECTOR]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to create IP selector pipe: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_root_pipe(port, pipes[IP_SELECTOR], &status, &pipes[ROOT]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to create root pipe: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = flow_process_entries(port, &status, num_of_entries);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to process %u entries: %s",
				     port_id,
				     num_of_entries,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	/* wait few seconds for packets to arrive so query will not return zero */
	DOCA_LOG_INFO("Wait %d seconds for first batch of packets to arrive", WAITING_TIME);
	sleep(WAITING_TIME);

	DOCA_LOG_DBG("Show miss counter results after first batch of packets");

	for (port_id = 0; port_id < nb_ports; port_id++) {
		pipes = pipes_array[port_id];

		result = miss_counters_query(pipes[ROOT], pipes[IP_SELECTOR], false, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to query miss counters before updating: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));
		pipes = pipes_array[port_id];
		port = ports[port_id];
		fwd_port.port_id = port_id ^ 1;

		DOCA_LOG_DBG("Port %u starts preparing the second pipeline", port_id);

		result = create_push_pipe(port, &fwd_port, &status, &pipes[PUSH]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to create push pipe: %s", port_id, doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = flow_process_entries(port, &status, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to process pipe push entry: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		DOCA_LOG_DBG("Port %u updates the FWD miss actions", port_id);

		result = update_miss_fwd_next_pipe(pipes[IPV4], pipes[PUSH]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to update fwd miss for IPv4 pipe: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = update_miss_fwd_next_pipe(pipes[IP_SELECTOR], pipes[PUSH]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to update fwd miss for IP selector pipe: %s",
				     port_id,
				     doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	/* wait few seconds for packets to arrive so query will not return zero */
	DOCA_LOG_INFO("Wait %d seconds for second batch of packets to arrive", WAITING_TIME);
	sleep(WAITING_TIME);

	DOCA_LOG_DBG("Show miss counter results after second batch of packets");

	for (port_id = 0; port_id < nb_ports; port_id++) {
		pipes = pipes_array[port_id];

		result = miss_counters_query(pipes[ROOT], pipes[IP_SELECTOR], true, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to query miss counters after updating: %s",
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
