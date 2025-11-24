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

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_ACL);

#define ACL_MEM_REQ_PER_ENTRY (32)

#define ACL_ACTIONS_MEM_SIZE(entries) \
	rte_align32pow2((uint32_t)(entries * ACL_MEM_REQ_PER_ENTRY * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE))

/* for egress use domain = DOCA_FLOW_PIPE_DOMAIN_EGRESS*/
static enum doca_flow_pipe_domain domain = DOCA_FLOW_PIPE_DOMAIN_DEFAULT;

/*
 * Create DOCA Flow control pipe
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t create_rx_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	doca_error_t result;

	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

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
	if (result != DOCA_SUCCESS)
		goto destroy_pipe_cfg;
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

	/* forwarding traffic to other port */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	return doca_flow_pipe_control_add_entry(0,
						0,
						*pipe,
						&match,
						NULL,
						NULL,
						NULL,
						NULL,
						NULL,
						NULL,
						&fwd,
						NULL,
						NULL);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow main pipe
 *
 * @port [in]: port of the pipe
 * @next_pipe [in]: acl pipe to forward the matched traffic
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_main_pipe(struct doca_flow_port *port,
				     struct doca_flow_pipe *next_pipe,
				     struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor_counter;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor_counter, 0, sizeof(monitor_counter));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

	monitor_counter.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MAIN_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_counter);
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
 * Add DOCA Flow pipe entry to the main pipe that forwards ipv4 traffic to acl pipe
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @entry [out]: result of entry addition
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_main_pipe_entry(struct doca_flow_pipe *pipe,
					struct entries_status *status,
					struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;

	memset(&match, 0, sizeof(match));

	return doca_flow_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, DOCA_FLOW_NO_WAIT, status, entry);
}

/*
 * Add DOCA Flow ACL pipe that matched IPV4 addresses
 *
 * @port [in]: port of the pipe
 * @is_root [in]: pipeline is root or not.
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t create_acl_pipe(struct doca_flow_port *port, bool is_root, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_monitor monitor_counter;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&monitor_counter, 0, sizeof(monitor_counter));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;

	match.outer.tcp.l4_port.src_port = 0xffff;
	match.outer.tcp.l4_port.dst_port = 0xffff;

	actions_arr[0] = &actions;

	monitor_counter.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "ACL_PIPE", DOCA_FLOW_PIPE_ACL, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 10);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, domain);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
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

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_counter);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the ACL pipe.
 *
 * @pipe [in]: pipe of the entry
 * @port_id [in]: port ID of the entry
 * @status [in]: the entries status struct that monitors the entries in this specific port
 * @src_ip_addr [in]: src ip address
 * @dst_ip_addr [in]: dst ip address
 * @src_port [in]: src port
 * @dst_port [in]: dst port
 * @l4_type [in]: l4 protocol
 * @src_ip_addr_mask [in]: src ip mask
 * @dst_ip_addr_mask [in]: dst ip mask
 * @src_port_mask [in]: src port mask.
 *	if src_port_mask is equal to src_port, ACL adds rule with exact src port : src_port with mask 0xffff
 *	if src_port_mask is 0, ACL adds rule with any src port : src_port with mask 0x0
 *	if src_port_mask > src_port, ACL adds rule with port range : src_port_from = src_port, src_port_to =
 *src_port_mask  with mask 0xffff if src_port_mask < src_port, ACL will return with the error
 * @dst_port_mask [in]: dst port mask
 *	if dst_port_mask is equal to dst_port, ACL adds rule with exact dst port : dst_port with mask 0xffff
 *	if dst_port_mask is 0, ACL adds rule with any dst port : dst_port with mask 0x0
 *	if dst_port_mask > dst_port, ACL adds rule with port range : dst_port_from = dst_port, dst_port_to =
 *dst_port_mask  with mask 0xffff if dst_port_mask < dst_port, ACL will return with the error
 * @priority [in]: priority of the entry. 0 <= priority <= 1024. the lowest parameter value is used as the highest
 *priority
 * @is_allow [in]: allow or deny the entry
 * @flag [in]: Flow entry will be pushed to hw immediately or not. enum doca_flow_flags_type.
 *	flag DOCA_FLOW_WAIT_FOR_BATCH is using for collecting entries by ACL module
 *	flag DOCA_FLOW_NO_WAIT is using for adding the entry and starting building and offloading
 * @param[out] entry The entry inserted.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t add_acl_specific_entry(struct doca_flow_pipe *pipe,
				    int port_id,
				    struct entries_status *status,
				    doca_be32_t src_ip_addr,
				    doca_be32_t dst_ip_addr,
				    doca_be16_t src_port,
				    doca_be16_t dst_port,
				    uint8_t l4_type,
				    doca_be32_t src_ip_addr_mask,
				    doca_be32_t dst_ip_addr_mask,
				    doca_be16_t src_port_mask,
				    doca_be16_t dst_port_mask,
				    uint16_t priority,
				    bool is_allow,
				    enum doca_flow_flags_type flag,
				    struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&fwd, 0, sizeof(fwd));

	match_mask.outer.ip4.src_ip = src_ip_addr_mask;
	match_mask.outer.ip4.dst_ip = dst_ip_addr_mask;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = src_ip_addr;
	match.outer.ip4.dst_ip = dst_ip_addr;

	if (l4_type == DOCA_FLOW_L4_TYPE_EXT_TCP) {
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
		match_mask.parser_meta.outer_l4_type = UINT32_MAX;
		match.outer.tcp.l4_port.src_port = src_port;
		match.outer.tcp.l4_port.dst_port = dst_port;
		match_mask.outer.tcp.l4_port.src_port = src_port_mask;
		match_mask.outer.tcp.l4_port.dst_port = dst_port_mask;
	} else {
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
		match_mask.parser_meta.outer_l4_type = UINT32_MAX;
		match.outer.udp.l4_port.src_port = src_port;
		match.outer.udp.l4_port.dst_port = dst_port;
		match_mask.outer.udp.l4_port.src_port = src_port_mask;
		match_mask.outer.udp.l4_port.dst_port = dst_port_mask;
	}
	match.outer.l4_type_ext = l4_type;

	if (is_allow) {
		if (domain == DOCA_FLOW_PIPE_DOMAIN_DEFAULT) {
			fwd.type = DOCA_FLOW_FWD_PORT;
			fwd.port_id = port_id ^ 1;
		} else { // domain == DOCA_FLOW_PIPE_DOMAIN_EGRESS
			fwd.type = DOCA_FLOW_FWD_PORT;
			fwd.port_id = port_id;
		}
	} else
		fwd.type = DOCA_FLOW_FWD_DROP;

	result =
		doca_flow_pipe_acl_add_entry(0, pipe, &match, &match_mask, 0, NULL, priority, &fwd, flag, status, entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add acl pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow pipe entries to the ACL pipe.
 *
 * @pipe [in]: pipe of the entry
 * @port_id [in]: port ID of the entry
 * @status [in]: user context for adding entry
 * @entries [out]: array of pointers to created entries
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t add_acl_pipe_entries(struct doca_flow_pipe *pipe,
				  int port_id,
				  struct entries_status *status,
				  struct doca_flow_pipe_entry **entries)
{
	doca_error_t result;
	int i_entry = 0;

	result = add_acl_specific_entry(pipe,
					port_id,
					status,
					BE_IPV4_ADDR(1, 2, 3, 4),
					BE_IPV4_ADDR(8, 8, 8, 8),
					DOCA_HTOBE16(1234),
					DOCA_HTOBE16(80),
					DOCA_FLOW_L4_TYPE_EXT_TCP,
					DOCA_HTOBE32(0xffffffff),
					DOCA_HTOBE32(0xffffffff),
					DOCA_HTOBE16(0x00),
					DOCA_HTOBE16(0x0),
					10,
					false,
					DOCA_FLOW_WAIT_FOR_BATCH,
					&entries[i_entry++]);
	if (result != DOCA_SUCCESS)
		return result;

	result = add_acl_specific_entry(pipe,
					port_id,
					status,
					BE_IPV4_ADDR(172, 20, 1, 4),
					BE_IPV4_ADDR(192, 168, 3, 4),
					DOCA_HTOBE16(1234),
					DOCA_HTOBE16(80),
					DOCA_FLOW_L4_TYPE_EXT_UDP,
					DOCA_HTOBE32(0xffffffff),
					DOCA_HTOBE32(0xffffffff),
					DOCA_HTOBE16(0x0),
					DOCA_HTOBE16(3000),
					50,
					true,
					DOCA_FLOW_WAIT_FOR_BATCH,
					&entries[i_entry++]);

	if (result != DOCA_SUCCESS)
		return result;

	result = add_acl_specific_entry(pipe,
					port_id,
					status,
					BE_IPV4_ADDR(172, 20, 1, 4),
					BE_IPV4_ADDR(192, 168, 3, 4),
					DOCA_HTOBE16(1234),
					DOCA_HTOBE16(80),
					DOCA_FLOW_L4_TYPE_EXT_TCP,
					DOCA_HTOBE32(0xffffffff),
					DOCA_HTOBE32(0xffffffff),
					DOCA_HTOBE16(1234),
					DOCA_HTOBE16(0x0),
					40,
					true,
					DOCA_FLOW_WAIT_FOR_BATCH,
					&entries[i_entry++]);

	if (result != DOCA_SUCCESS)
		return result;

	result = add_acl_specific_entry(pipe,
					port_id,
					status,
					BE_IPV4_ADDR(1, 2, 3, 5),
					BE_IPV4_ADDR(8, 8, 8, 6),
					DOCA_HTOBE16(1234),
					DOCA_HTOBE16(80),
					DOCA_FLOW_L4_TYPE_EXT_TCP,
					DOCA_HTOBE32(0xffffff00),
					DOCA_HTOBE32(0xffffff00),
					DOCA_HTOBE16(0xffff),
					DOCA_HTOBE16(80),
					20,
					true,
					DOCA_FLOW_NO_WAIT,
					&entries[i_entry++]);

	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Print ACL statistics
 *
 * @nb_ports [in]: number of ports
 * @num_of_entries [in]: number of entries per port
 * @entries [in]: array of flow entries
 */
static void print_acl_stats(int nb_ports, int num_of_entries, struct doca_flow_pipe_entry *entries[][2])
{
	doca_error_t result;
	struct doca_flow_resource_query stats;
	int port_id;

	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = doca_flow_resource_query_entry(entries[port_id][0], &stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %d failed to query main pipe entry: %s",
				     port_id,
				     doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Port %d, main pipe entry received %lu packets", port_id, stats.counter.total_pkts);

		for (int acl_entry_id = 1; acl_entry_id < num_of_entries; acl_entry_id++) {
			result = doca_flow_resource_query_entry(entries[port_id][acl_entry_id], &stats);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Port %d failed to query ACL pipe entry %d: %s",
					     port_id,
					     acl_entry_id,
					     doca_error_get_descr(result));
				return;
			}
			DOCA_LOG_INFO("Port %d, ACL pipe entry %d received %lu packets",
				      port_id,
				      acl_entry_id,
				      stats.counter.total_pkts);
		}
	}
}

/* Context structure for statistics printing */
struct acl_stats_context {
	int nb_ports;
	int num_of_entries;
	struct doca_flow_pipe_entry *(*entries)[2];
};

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: acl_stats_context structure
 */
static void print_acl_stats_wrapper(void *context)
{
	struct acl_stats_context *ctx = (struct acl_stats_context *)context;
	print_acl_stats(ctx->nb_ports, ctx->num_of_entries, ctx->entries);
}

doca_error_t flow_acl(int nb_queues)
{
	const int nb_ports = 2;
	/* 1 entry for main pipe and 4 entries for ACL pipe */
	const int num_of_entries = 5;
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT, .nr_counters = num_of_entries};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *acl_pipe;
	struct doca_flow_pipe *main_pipe;
	struct doca_flow_pipe *rx_pipe;
	struct entries_status status;
	struct doca_flow_pipe_entry *entries[nb_ports][num_of_entries];
	struct acl_stats_context ctx = {.nb_ports = nb_ports, .num_of_entries = num_of_entries, .entries = entries};
	doca_error_t result;
	int port_id, port_acl;

	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACL_ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));

		if (domain == DOCA_FLOW_PIPE_DOMAIN_DEFAULT)
			port_acl = port_id;
		else // domain == DOCA_FLOW_PIPE_DOMAIN_EGRESS
			port_acl = port_id ^ 1;

		result = create_acl_pipe(ports[port_acl], false, &acl_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create acl pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_acl_pipe_entries(acl_pipe, port_acl, &status, &entries[port_id][1]);
		if (result != DOCA_SUCCESS) {
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_main_pipe(ports[port_id], acl_pipe, &main_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create main pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_main_pipe_entry(main_pipe, &status, &entries[port_id][0]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		if (domain == DOCA_FLOW_PIPE_DOMAIN_EGRESS) {
			result = create_rx_pipe(ports[port_id], port_id, &rx_pipe);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create main pipe: %s", doca_error_get_descr(result));
				stop_doca_flow_ports(nb_ports, ports);
				doca_flow_destroy();
				return result;
			}
		}

		result = doca_flow_entries_process(ports[port_acl], 0, DEFAULT_TIMEOUT_US, num_of_entries);
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

	flow_wait_for_packets(20, print_acl_stats_wrapper, &ctx);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
