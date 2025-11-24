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

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#include <doca_dev.h>
#include <doca_log.h>
#include <doca_flow.h>
#include "doca_error.h"

#include <flow_common.h>
#include <flow_switch_common.h>

DOCA_LOG_REGISTER(FLOW_SWITCH_HOT_UPGRADE);

#define FLOW_SWITCH_PROXY_PORT_NB 2

#define INTERVAL_QUERY_TIME 1

#define SEC2USEC(sec) ((sec) * (1000000L))

#define DEFAULT_CTRL_PIPE_SIZE (8192)

/* Structure to control all port logic */
struct port_control {
	struct doca_flow_port *port;		       /* DOCA Flow switch port */
	struct doca_flow_pipe *pipe;		       /* Root pipe of the port */
	struct doca_flow_pipe_entry *tcp_entry;	       /* Entry matching TCP packets */
	struct doca_flow_pipe_entry *udp_entry;	       /* Entry matching UDP packets */
	char pci_dev[DOCA_DEVINFO_PCI_ADDR_SIZE];      /* PCI address as string */
	char iface_name[DOCA_DEVINFO_IFACE_NAME_SIZE]; /* Port interface name */
	uint64_t last_hit_tcp;			       /* Number TCP packets received so far */
	uint64_t last_hit_udp;			       /* Number UDP packets received so far */
	uint64_t last_miss;			       /* Number miss packets received so far */
	uint64_t last_total;			       /* Total packets received so far */
};

static struct doca_flow_port *switch_ports[FLOW_SWITCH_PROXY_PORT_NB];
static uint8_t waiting_for_traffic;

/* The current operation state of the switch ports in this instance */
static enum doca_flow_port_operation_state current_state;

/*
 * Create DOCA Flow pipe with L3/L4 match on the switch port.
 * Matched traffic will be forwarded to the port defined per entry.
 * Unmatched traffic will be dropped.
 *
 * @port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_switch_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

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

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4; /* specific */
	match.parser_meta.outer_l4_type = UINT32_MAX;		  /* changeable */
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 0xffff; /* changeable */
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the pipe.
 *
 * @switch_port_idx [in]: switch port index.
 * @control [in]: port control with all needed information.
 * @status [in]: user context for adding entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_switch_pipe_entries(uint8_t switch_port_idx,
					    struct port_control *control,
					    struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));
	memset(status, 0, sizeof(*status));

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = (switch_port_idx * 3) + 1; /* first representor of this port */

	result = doca_flow_pipe_add_entry(0,
					  control->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  status,
					  &control->tcp_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	fwd.port_id = (switch_port_idx * 3) + 2; /* second representor of this port */

	result = doca_flow_pipe_add_entry(0,
					  control->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  status,
					  &control->udp_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = flow_process_entries(control->port, status, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Initializes control structure including pipe and entries creation for single switch port.
 *
 * @port [in]: switch port.
 * @dev [in]: DOCA device connected to this port.
 * @switch_port_idx [in]: switch port index.
 * @state [in]: the operation state of this instance.
 * @status [in]: user context for adding entries.
 * @control [out]: pointer to control port structure.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t port_control_init(struct doca_flow_port *port,
				      struct doca_dev *dev,
				      uint8_t switch_port_idx,
				      enum doca_flow_port_operation_state state,
				      struct entries_status *status,
				      struct port_control *control)
{
	struct doca_devinfo *devinfo;
	doca_error_t result;

	devinfo = doca_dev_as_devinfo(dev);
	if (devinfo == NULL) {
		DOCA_LOG_ERR("Failed to convert DOCA device to devinfo for port %u", switch_port_idx);
		return DOCA_ERROR_NOT_FOUND;
	}

	result = doca_devinfo_get_iface_name(devinfo, control->iface_name, DOCA_DEVINFO_IFACE_NAME_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get interface name for port %u: %s",
			     switch_port_idx,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_devinfo_get_pci_addr_str(devinfo, control->pci_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PCI string for port %u: %s", switch_port_idx, doca_error_get_descr(result));
		return result;
	}

	control->port = doca_flow_port_switch_get(port);
	result = create_switch_pipe(control->port, &control->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe for port %u", switch_port_idx);
		return result;
	}

	result = add_switch_pipe_entries(switch_port_idx, control, status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pipe entries for port %u", switch_port_idx);
		return result;
	}

	result = doca_flow_port_operation_state_modify(control->port, state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify operation state for port %u: %s",
			     switch_port_idx,
			     doca_error_get_descr(result));
		return result;
	}

	control->last_hit_tcp = 0;
	control->last_hit_udp = 0;
	control->last_miss = 0;
	control->last_total = 0;

	switch_ports[switch_port_idx] = control->port;

	return DOCA_SUCCESS;
}

/*
 * Modify operation state for all proxy ports.
 *
 * @state [in]: next operation state.
 */
static void ports_operation_state_modify(enum doca_flow_port_operation_state state)
{
	struct doca_flow_port *port;
	doca_error_t result;
	uint8_t i;

	for (i = 0; i < FLOW_SWITCH_PROXY_PORT_NB; ++i) {
		port = switch_ports[i];

		result = doca_flow_port_operation_state_modify(port, state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify operation state from %u to %u for port %u: %s",
				     current_state,
				     state,
				     i,
				     doca_error_get_descr(result));
			return;
		}

		DOCA_LOG_DBG("Port %u operation state was successfully modified from %u to %u", i, current_state, state);
	}

	current_state = state;
}

/*
 * Signal handler triggering switch port activation for hot upgrade.
 *
 * @signum [in]: signal number.
 */
static void activate_signal_handler(int signum)
{
	enum doca_flow_port_operation_state next;

	printf("\n");
	DOCA_LOG_INFO("CTRL-C is pressed, running over the ports and activating them");

	switch (current_state) {
	case DOCA_FLOW_PORT_OPERATION_STATE_ACTIVE:
		next = DOCA_FLOW_PORT_OPERATION_STATE_ACTIVE_READY_TO_SWAP;
		break;
	case DOCA_FLOW_PORT_OPERATION_STATE_ACTIVE_READY_TO_SWAP:
		next = DOCA_FLOW_PORT_OPERATION_STATE_STANDBY;
		break;
	case DOCA_FLOW_PORT_OPERATION_STATE_STANDBY:
	case DOCA_FLOW_PORT_OPERATION_STATE_UNCONNECTED:
	default:
		next = DOCA_FLOW_PORT_OPERATION_STATE_ACTIVE;
		break;
	}

	ports_operation_state_modify(next);
	(void)signum;
}

/*
 * Signal handler for quit.
 *
 * @signum [in]: signal number.
 */
static void quit_signal_handler(int signum)
{
	printf("\n");
	DOCA_LOG_INFO("Signal %d is triggered, quit the sample", signum);

	ports_operation_state_modify(DOCA_FLOW_PORT_OPERATION_STATE_UNCONNECTED);

	/*
	 * Pause for one second before setting the 'waiting_for_traffic' flag to false.
	 * This delay ensures that the query counters loop is stopped at the right time.
	 * The hardware updates the counters every second, so we wait to ensure all
	 * counters are updated before performing the last query.
	 */
	sleep(1);
	__atomic_store_n(&waiting_for_traffic, 0, __ATOMIC_RELAXED);
}

/*
 * Register the signal handler functions.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static int signal_handler_register(void)
{
	struct sigaction action = {0};

	action.sa_handler = activate_signal_handler;
	if (sigaction(SIGINT, &action, NULL) == -1) {
		DOCA_LOG_ERR("Failed to take SIGINT signal for hot upgrade, errno=%d", errno);
		return -errno;
	}

	action.sa_handler = quit_signal_handler;
	if (sigaction(SIGQUIT, &action, NULL) == -1) {
		DOCA_LOG_ERR("Failed to take SIGQUIT signal for quit program, errno=%d", errno);
		return -errno;
	}

	return 0;
}

/*
 * Query all counters in port.
 *
 * Query both UDP and TCP entries as same as miss counter.
 * When at least one of them is increased, all counter results are printed in this format:
 * 	| <type> new_value (old_value)
 *
 * @control [in]: pointer to port control structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t port_control_query(struct port_control *control)
{
	struct doca_flow_resource_query miss_stats = {0};
	struct doca_flow_resource_query udp_stats = {0};
	struct doca_flow_resource_query tcp_stats = {0};
	uint64_t total;
	doca_error_t result;

	result = doca_flow_resource_query_pipe_miss(control->pipe, &miss_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query pipe miss: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_resource_query_entry(control->tcp_entry, &tcp_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query TCP entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_resource_query_entry(control->udp_entry, &udp_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query UDP entry: %s", doca_error_get_descr(result));
		return result;
	}

	total = miss_stats.counter.total_pkts + udp_stats.counter.total_pkts + tcp_stats.counter.total_pkts;
	if (total == control->last_total)
		return DOCA_SUCCESS;

	DOCA_LOG_INFO("Device %s with PCI address %s receive new traffic:", control->iface_name, control->pci_dev);
	DOCA_LOG_INFO("Total traffic %lu (%lu) | TCP %lu (%lu) | UDP %lu (%lu) | miss %lu (%lu)",
		      total,
		      control->last_total,
		      tcp_stats.counter.total_pkts,
		      control->last_hit_tcp,
		      udp_stats.counter.total_pkts,
		      control->last_hit_udp,
		      miss_stats.counter.total_pkts,
		      control->last_miss);

	control->last_hit_tcp = tcp_stats.counter.total_pkts;
	control->last_hit_udp = udp_stats.counter.total_pkts;
	control->last_miss = miss_stats.counter.total_pkts;
	control->last_total = total;

	return DOCA_SUCCESS;
}

static doca_error_t set_doca_flow_port_operation_state(struct doca_flow_port_cfg *port_cfg,
						       int port_id,
						       void *config_cb_ctx)
{
	enum doca_flow_port_operation_state state = *(enum doca_flow_port_operation_state *)config_cb_ctx;
	doca_error_t result;
	(void)port_id;

	result = doca_flow_port_cfg_set_operation_state(port_cfg, state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg operation state: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize DOCA Flow ports
 *
 * @devs_manager [in]: array of switch manager device bundles (doca device and representor devices)
 *			      for each switch manager
 * @nb_devs [in]: number of managers to create ports for
 * @ports [in]: array of ports to create
 * @nb_ports [in]: number of ports the sample will use.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t init_hot_upgrade_ports(struct flow_devs_manager devs_manager[],
					   int nb_devs,
					   struct doca_flow_port *ports[],
					   int nb_ports,
					   struct flow_resources *resources)
{
	struct doca_dev *dev_arr[nb_ports];
	struct doca_dev_rep *rep_arr[nb_ports];
	enum doca_flow_port_operation_state state = DOCA_FLOW_PORT_OPERATION_STATE_UNCONNECTED;
	uint32_t actions_mem_size[nb_ports];
	int nb_mapped_devs = 0;

	memset(dev_arr, 0, sizeof(struct doca_dev *) * nb_ports);
	memset(rep_arr, 0, sizeof(struct doca_dev_rep *) * nb_ports);

	for (int i = 0; i < nb_devs; i++) {
		dev_arr[nb_mapped_devs++] = devs_manager[i].doca_dev;

		for (int j = 0; j < devs_manager[i].nb_reps; j++)
			rep_arr[nb_mapped_devs++] = devs_manager[i].doca_dev_rep[j];
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(DEFAULT_CTRL_PIPE_SIZE));

	return init_doca_flow_ports_with_custom_config(nb_mapped_devs,
						       ports,
						       false,
						       dev_arr,
						       rep_arr,
						       set_doca_flow_port_operation_state,
						       NULL /* port_rep_cb */,
						       &state,
						       actions_mem_size,
						       resources);
}

/*
 * Run flow_switch_hot_upgrade sample
 *
 * @nb_queues [in]: number of queues the sample will use.
 * @nb_ports [in]: number of ports the sample will use.
 * @devs_manager [in]: Array of DOCA devices for the switch ports
 * @nb_devs [in]: Amount of eswtich manager dev bundles in the switch_manager_devs array
 * @state [in]: the operation state of this instance.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_switch_hot_upgrade(int nb_queues,
				     int nb_ports,
				     struct flow_devs_manager devs_manager[],
				     uint16_t nb_devs,
				     enum doca_flow_port_operation_state state)
{
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT, .nr_counters = 6};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	struct port_control port_control_list[FLOW_SWITCH_PROXY_PORT_NB];
	struct entries_status status;
	struct timeval start, end;
	long query_time, sleep_time;
	uint8_t switch_port_idx;
	doca_error_t result;

	result = init_doca_flow(nb_queues, "switch,isolated", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	result = init_hot_upgrade_ports(devs_manager, nb_devs, ports, nb_ports, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	for (switch_port_idx = 0; switch_port_idx < nb_devs; ++switch_port_idx) {
		uint8_t port_id = switch_port_idx * 3;

		result = port_control_init(ports[port_id],
					   devs_manager[switch_port_idx].doca_dev,
					   switch_port_idx,
					   state,
					   &status,
					   &port_control_list[switch_port_idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init port control %u", switch_port_idx);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	current_state = state;
	gettimeofday(&start, NULL);

	if (signal_handler_register() < 0) {
		DOCA_LOG_ERR("Failed to register signal handlers");
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	DOCA_LOG_INFO("Waiting for traffic to arrived, press CTRL+C to make process active or CTRL+\\ for quit");
	__atomic_store_n(&waiting_for_traffic, 1, __ATOMIC_RELAXED);

	while (__atomic_load_n(&waiting_for_traffic, __ATOMIC_RELAXED)) {
		gettimeofday(&end, NULL);
		query_time = SEC2USEC(end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec);
		sleep_time = SEC2USEC(INTERVAL_QUERY_TIME) - query_time;
		if (sleep_time > 0)
			usleep(sleep_time);

		gettimeofday(&start, NULL);

		for (switch_port_idx = 0; switch_port_idx < nb_devs; ++switch_port_idx) {
			result = port_control_query(&port_control_list[switch_port_idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query port control %u", switch_port_idx);
				stop_doca_flow_ports(nb_ports, ports);
				doca_flow_destroy();
				return result;
			}
		}
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
