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
#include <stdlib.h>

#include <rte_random.h>

#include <doca_flow.h>
#include <doca_flow_definitions.h>
#include <doca_log.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_AGING);

/* user context struct for aging entry */
struct aging_user_data {
	struct entries_status *status; /* status pointer */
	int entry_num;		       /* entry number */
	int port_id;		       /* port ID of the entry */
};

/*
 * Entry processing callback
 *
 * @entry [in]: DOCA Flow entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
static void check_for_valid_entry_aging(struct doca_flow_pipe_entry *entry,
					uint16_t pipe_queue,
					enum doca_flow_entry_status status,
					enum doca_flow_entry_op op,
					void *user_ctx)
{
	struct aging_user_data *entry_status = (struct aging_user_data *)user_ctx;
	doca_error_t result;

	if (entry_status == NULL)
		return;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->status->failure = true; /* set failure to true if processing failed */
	if (op == DOCA_FLOW_ENTRY_OP_AGED) {
		result = doca_flow_pipe_remove_entry(pipe_queue, DOCA_FLOW_NO_WAIT, entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to remove entry number %d from port %d: %s",
				     entry_status->entry_num,
				     entry_status->port_id,
				     doca_error_get_descr(result));
		} else {
			DOCA_LOG_INFO("Entry number %d from port %d aged out and removed",
				      entry_status->entry_num,
				      entry_status->port_id);
		}
	} else
		entry_status->status->nb_processed++;
}

/*
 * Create DOCA Flow pipe with five tuple match and monitor with aging flag
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @n_entries [in]: pipe size
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_aging_pipe(struct doca_flow_port *port,
				      int port_id,
				      uint32_t n_entries,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions;
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd, fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));
	memset(&pipe_cfg, 0, sizeof(pipe_cfg));

	/* set monitor with aging */
	monitor.aging_sec = 0xffffffff;

	/* 5 tuple match */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;
	match.outer.tcp.l4_port.src_port = 0xffff;
	match.outer.tcp.l4_port.dst_port = 0xffff;

	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "AGING_PIPE", DOCA_FLOW_PIPE_BASIC, true);
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
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg miss counter: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, n_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow entries to the aging pipe, each entry with different aging time
 *
 * @pipe [in]: pipe of the entries
 * @user_data [in]: user data array
 * @port [in]: port of the entries
 * @port_id [in]: port ID of the entries
 * @num_of_aging_entries [in]: number of entries to add
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_aging_pipe_entries(struct doca_flow_pipe *pipe,
					   struct aging_user_data *user_data,
					   struct doca_flow_port *port,
					   int port_id,
					   int num_of_aging_entries,
					   struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_monitor monitor;
	int i;
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	doca_be32_t dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8);
	doca_be16_t dst_port = DOCA_HTOBE16(80);
	doca_be16_t src_port = DOCA_HTOBE16(1234);
	doca_be32_t src_ip_addr; /* set different src ip per entry */
	doca_error_t result;

	for (i = 0; i < num_of_aging_entries; i++) {
		src_ip_addr = BE_IPV4_ADDR((i + 1), 2, 3, 4);

		memset(&match, 0, sizeof(match));
		memset(&actions, 0, sizeof(actions));
		memset(&monitor, 0, sizeof(monitor));

		/* flows will be aged out in 5s */
		monitor.aging_sec = 5;

		match.outer.ip4.dst_ip = dst_ip_addr;
		match.outer.ip4.src_ip = src_ip_addr;
		match.outer.tcp.l4_port.dst_port = dst_port;
		match.outer.tcp.l4_port.src_port = src_port;
		/* fill user data with entry number and entry pointer */
		user_data[i].entry_num = i;
		user_data[i].port_id = port_id;
		user_data[i].status = status;

		if (i == num_of_aging_entries - 1)
			flags = DOCA_FLOW_NO_WAIT; /* send the last entry with DOCA_FLOW_NO_WAIT flag for pushing all
						      the entries */

		result = doca_flow_pipe_add_entry(0,
						  pipe,
						  &match,
						  0,
						  &actions,
						  &monitor,
						  NULL,
						  flags,
						  &user_data[i],
						  NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
			return result;
		}
	}
	do {
		result = doca_flow_entries_process(port,
						   0,
						   DEFAULT_TIMEOUT_US,
						   num_of_aging_entries - status->nb_processed);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			return result;
		}
		if (status->failure) {
			DOCA_LOG_ERR("Failed to process entries, status is not success");
			return DOCA_ERROR_BAD_STATE;
		}
	} while (status->nb_processed < num_of_aging_entries);
	DOCA_LOG_INFO("Added %d entries to port %d", status->nb_processed, port_id);
	status->nb_processed = 0;

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
static doca_error_t miss_counter_query(struct doca_flow_pipe *pipe, int port_id)
{
	struct doca_flow_resource_query query_stats;
	doca_error_t result;

	result = doca_flow_resource_query_pipe_miss(pipe, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Port %u failed to query pipe miss: %s", port_id, doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Port %d pipe miss %ld packets", port_id, query_stats.counter.total_pkts);

	return DOCA_SUCCESS;
}

/*
 * Handle all aged flow in a port
 *
 * @port [in]: port to remove the aged flow from
 * @total_counter [in/out]: counter for all aged flows in both ports
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t handle_aged_flow(struct doca_flow_port *port, int *total_counter)
{
	uint64_t quota_time = 20; /* max handling aging time in ms */
	int num_of_aged_entries = 0;

	num_of_aged_entries = doca_flow_aging_handle(port, 0, quota_time, 0);
	/* call handle aging until full cycle complete */
	while (num_of_aged_entries != -1) {
		if (num_of_aged_entries > 0) {
			*total_counter += num_of_aged_entries;
			DOCA_LOG_INFO("Num of aged entries: %d, total: %d", num_of_aged_entries, *total_counter);
		}

		num_of_aged_entries = doca_flow_aging_handle(port, 0, quota_time, 0);
	}
	return DOCA_SUCCESS;
}

/*
 * Run flow_aging sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_aging(int nb_queues)
{
	const int nb_ports = 2;
	/* the counters will divide by all queues per port */
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipe;
	struct entries_status status[nb_ports];
	struct aging_user_data *user_data[nb_ports];
	int num_of_aging_entries = 10;
	int aged_entry_counter = 0;
	doca_error_t result, doca_error = DOCA_SUCCESS;
	int port_id;

	resource.nr_counters = 80 + num_of_aging_entries * nb_ports;

	result = init_doca_flow_cb(nb_queues,
				   "vnf,hws",
				   &resource,
				   nr_shared_resources,
				   check_for_valid_entry_aging,
				   NULL,
				   NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_aging_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	memset(user_data, 0, sizeof(struct aging_user_data *) * nb_ports);

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status[port_id], 0, sizeof(status[port_id]));

		result = create_aging_pipe(ports[port_id], port_id, num_of_aging_entries, &pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
			goto entries_cleanup;
		}

		user_data[port_id] =
			(struct aging_user_data *)malloc(num_of_aging_entries * sizeof(struct aging_user_data));
		if (user_data[port_id] == NULL) {
			DOCA_LOG_ERR("Failed to allocate user data");
			result = DOCA_ERROR_NO_MEMORY;
			goto entries_cleanup;
		}

		result = add_aging_pipe_entries(pipe,
						user_data[port_id],
						ports[port_id],
						port_id,
						num_of_aging_entries,
						&status[port_id]);
		if (result != DOCA_SUCCESS) {
			stop_doca_flow_ports(nb_ports, ports);
			goto entries_cleanup;
		}
	}

	/* wait few seconds for packets to arrive so query will not return zero */
	DOCA_LOG_INFO("Wait %d seconds for first batch of packets to arrive", 5);
	sleep(5);

	DOCA_LOG_INFO("Show miss counter results after first iteration of sending packets");
	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = miss_counter_query(pipe, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to query miss counter: %s", port_id, doca_error_get_descr(result));
			goto entries_cleanup;
		}
	}

	DOCA_LOG_INFO("Wait few seconds for all entries to age out");
	memset(status, 0, sizeof(status));
	/* handle aging in loop until all entries aged out */
	while (aged_entry_counter < num_of_aging_entries * nb_ports) {
		sleep(1);
		for (port_id = 0; port_id < nb_ports; port_id++) {
			result = handle_aged_flow(ports[port_id], &aged_entry_counter);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to handle aged flow: %s", doca_error_get_descr(result));
				goto entries_cleanup;
			}
		}
	}

	/* most of entries are processed automatically before removing new entries, poll results of remaining entries */
	for (port_id = 0; port_id < nb_ports; port_id++) {
		while (status[port_id].nb_processed < num_of_aging_entries) {
			result = doca_flow_entries_process(ports[port_id],
							   0,
							   DEFAULT_TIMEOUT_US,
							   num_of_aging_entries - status[port_id].nb_processed);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
				goto entries_cleanup;
			}
			if (status[port_id].failure) {
				DOCA_LOG_ERR("Failed to process entries, status is not success");
				goto entries_cleanup;
			}
		}
		DOCA_LOG_INFO("Completed processing port %d", port_id);
	}

	DOCA_LOG_INFO("Wait %d seconds for second batch of packets to arrive", 5);
	sleep(5);

	DOCA_LOG_INFO("Show miss counter results after second batch of packets");
	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = miss_counter_query(pipe, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Port %u failed to query miss counter: %s", port_id, doca_error_get_descr(result));
			goto entries_cleanup;
		}
	}

entries_cleanup:
	for (port_id = 0; port_id < nb_ports; port_id++) {
		if (user_data[port_id])
			free(user_data[port_id]);
	}
	doca_error = result;
	result = stop_doca_flow_ports(nb_ports, ports);
	if (result != DOCA_SUCCESS && doca_error == DOCA_SUCCESS)
		doca_error = result;
	doca_flow_destroy();
	return doca_error;
}
