/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <rte_random.h>

#include <doca_flow.h>
#include <doca_dpdk.h>
#include <doca_log.h>
#include <doca_bitfield.h>

#include "app_vnf.h"
#include "simple_fwd.h"
#include "simple_fwd_ft.h"
#include "utils.h"

DOCA_LOG_REGISTER(SIMPLE_FWD);

/* Convert IPv4 address to big endian */
#define BE_IPV4_ADDR(a, b, c, d) (RTE_BE32(((uint32_t)a << 24) + (b << 16) + (c << 8) + d))

/* Set the MAC address with respect to the given 6 bytes */
#define SET_MAC_ADDR(addr, a, b, c, d, e, f) \
	do { \
		addr[0] = a & 0xff; \
		addr[1] = b & 0xff; \
		addr[2] = c & 0xff; \
		addr[3] = d & 0xff; \
		addr[4] = e & 0xff; \
		addr[5] = f & 0xff; \
	} while (0)

/* Set match l4 port */
#define SET_L4_PORT(layer, port, value) \
	do { \
		if (match->layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_TCP) \
			match->layer.tcp.l4_port.port = (value); \
		else if (match->layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_UDP) \
			match->layer.udp.l4_port.port = (value); \
	} while (0)

/*
 * Currently we only can get the ft_entry ctx, but for the aging,
 * we need get the ft_entry pointer, add destroy the ft entry.
 */
#define GET_FT_ENTRY(ctx) container_of(ctx, struct simple_fwd_ft_entry, user_ctx)

#define PULL_TIME_OUT 10000 /* Maximum timeout for pulling */
#define NB_ACTION_ARRAY (1) /* Used as the size of muti-actions array for DOCA Flow API */
#define NB_ACTION_DESC (1)  /* Used as the size of muti-action descs array for DOCA Flow API */

static struct simple_fwd_app *simple_fwd_ins; /* Instance holding all allocated resources needed for a proper run */

/* user context struct that will be used in entries process callback */
struct entries_status {
	bool failure;		     /* will be set to true if some entry status will not be success */
	int nb_processed;	     /* will hold the number of entries that was already processed */
	void *ft_entry;		     /* pointer to struct simple_fwd_ft_entry */
	struct doca_flow_port *port; /* pointer to the port that pipe belongs to */
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
static void simple_fwd_check_for_valid_entry(struct doca_flow_pipe_entry *entry,
					     uint16_t pipe_queue,
					     enum doca_flow_entry_status status,
					     enum doca_flow_entry_op op,
					     void *user_ctx)
{
	doca_error_t result;
	(void)entry;
	(void)op;
	(void)pipe_queue;

	struct simple_fwd_ft_entry *ft_entry;
	struct entries_status *entry_status = (struct entries_status *)user_ctx;

	if (entry_status == NULL)
		return;
	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->failure = true; /* set failure to true if processing failed */
	if (op == DOCA_FLOW_ENTRY_OP_AGED) {
		ft_entry = GET_FT_ENTRY((void *)(entry_status->ft_entry));
		simple_fwd_ft_destroy_entry(simple_fwd_ins->ft, ft_entry);
		result = doca_flow_entries_process(entry_status->port, pipe_queue, PULL_TIME_OUT, 1);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
	} else if (op == DOCA_FLOW_ENTRY_OP_ADD)
		entry_status->nb_processed++;
	else if (op == DOCA_FLOW_ENTRY_OP_DEL) {
		entry_status->nb_processed--;
		if (entry_status->nb_processed == 0)
			free(entry_status);
	}
}

static doca_error_t configure_tune_server(struct doca_flow_cfg *flow_cfg, enum doca_flow_tune_profile profile)
{
	doca_error_t result = DOCA_SUCCESS;
	struct doca_flow_tune_cfg *tune_cfg;

	result = doca_flow_tune_cfg_create(&tune_cfg);
	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_INFO("DOCA Flow Tune Server isn't supported in this runtime version");
			DOCA_LOG_INFO("Program will continue execution without activating the DOCA Flow Tune Server");
			return DOCA_SUCCESS;
		}

		DOCA_LOG_ERR("Failed to create doca_flow_tune_cfg: %s", doca_error_get_descr(result));
		goto out;
	}

	result = doca_flow_tune_cfg_set_profile(tune_cfg, profile);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_tune_cfg profile: %s", doca_error_get_descr(result));
		goto cfg_destroy;
	}

	result = doca_flow_cfg_set_tune_cfg(flow_cfg, tune_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg tune_cfg: %s", doca_error_get_descr(result));
		goto cfg_destroy;
	}

cfg_destroy:
	doca_flow_tune_cfg_destroy(tune_cfg);
out:
	return result;
}

/*
 * Initialize DOCA Flow library
 *
 * @nb_queues [in]: number of queues the sample will use
 * @mode [in]: doca flow architecture mode
 * @nr_counters [in]: number of counters to configure
 * @nr_meters [in]: number of meters to configure
 * @return: 0 on success, negative errno value otherwise and error is set.
 */
static int simple_fwd_init_doca_flow(int nb_queues, const char *mode, uint32_t nr_counters, uint32_t nr_meters)
{
	struct doca_flow_cfg *flow_cfg;
	uint16_t rss_queues[nb_queues];
	struct doca_flow_resource_rss_cfg rss = {0};
	doca_error_t result, tmp_result;

	result = doca_flow_cfg_create(&flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		return -1;
	}

	result = doca_flow_cfg_set_pipe_queues(flow_cfg, nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_mode_args(flow_cfg, mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_nr_counters(flow_cfg, nr_counters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_nr_meters(flow_cfg, nr_meters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_meters: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_cb_entry_process(flow_cfg, simple_fwd_check_for_valid_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg cb_entry_process: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	linear_array_init_u16(rss_queues, nb_queues);
	rss.nr_queues = nb_queues;
	rss.queues_array = rss_queues;
	result = doca_flow_cfg_set_default_rss(flow_cfg, &rss);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg rss: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = configure_tune_server(flow_cfg, DOCA_FLOW_TUNE_PROFILE_FULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure tune server: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_init(flow_cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to initialize doca flow: %s", doca_error_get_descr(result));

destroy_cfg:
	tmp_result = doca_flow_cfg_destroy(flow_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow_cfg: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return (result == DOCA_SUCCESS) ? 0 : -1;
}

/*
 * Create DOCA Flow port by port id
 *
 * @port_id [in]: port ID
 * @return: port handler on success, NULL otherwise and error is set.
 */
static struct doca_flow_port *simple_fwd_create_doca_flow_port(int port_id)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result, tmp_result;
	struct doca_flow_port *port;
	struct doca_dev *dev;

	result = doca_dpdk_port_as_dev(port_id, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DOCA device: %s", doca_error_get_descr(result));
		return NULL;
	}

	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return NULL;
	}

	result = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port_id: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg doca device: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_cfg_set_priv_data_size(port_cfg, sizeof(struct simple_fwd_port_cfg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg priv_data_size: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_cfg_set_actions_mem_size(
		port_cfg,
		rte_align32pow2(SIMPLE_FWD_MAX_FLOWS * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg actions mem size: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_start(port_cfg, &port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca_flow port: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

destroy_port_cfg:
	tmp_result = doca_flow_port_cfg_destroy(port_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow port: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result == DOCA_SUCCESS ? port : NULL;
}

/*
 * Stop DOCA Flow ports
 *
 * @nb_ports [in]: number of ports to stop
 * @ports [in]: array of DOCA flow ports to stop
 */
static void simple_fwd_stop_doca_flow_ports(int nb_ports, struct doca_flow_port *ports[])
{
	int portid;

	for (portid = 0; portid < nb_ports; portid++) {
		if (ports[portid] != NULL)
			doca_flow_port_stop(ports[portid]);
	}
}

/*
 * Initialize DOCA Flow ports
 *
 * @nb_ports [in]: number of ports to create
 * @ports [in]: array of ports to create
 * @is_hairpin [in]: port pair should run if is_hairpin = true
 * @return: 0 on success and negative value otherwise.
 */
static int simple_fwd_init_doca_flow_ports(int nb_ports, struct doca_flow_port *ports[], bool is_hairpin)
{
	int portid;

	for (portid = 0; portid < nb_ports; portid++) {
		/* Create doca flow port */
		ports[portid] = simple_fwd_create_doca_flow_port(portid);
		if (ports[portid] == NULL) {
			simple_fwd_stop_doca_flow_ports(portid + 1, ports);
			return -1;
		}
	}

	if (!is_hairpin)
		return 0;

	for (portid = 0; portid < nb_ports; portid++) {
		/* Pair ports should be done in the following order: port0 with port1, port2 with port3 etc */
		if (!portid || !(portid % 2))
			continue;

		/* pair odd port with previous port */
		if (doca_flow_port_pair(ports[portid], ports[portid ^ 1]) != DOCA_SUCCESS) {
			simple_fwd_stop_doca_flow_ports(portid + 1, ports);
			return -1;
		}

		if (doca_flow_port_pair(ports[portid ^ 1], ports[portid]) != DOCA_SUCCESS) {
			simple_fwd_stop_doca_flow_ports(portid + 1, ports);
			return -1;
		}
	}

	return 0;
}

/*
 * Callback function for removing aged flow
 *
 * @ctx [in]: the context of the aged flow to remove
 */
static void simple_fwd_aged_flow_cb(struct simple_fwd_ft_user_ctx *ctx)
{
	struct simple_fwd_pipe_entry *entry = (struct simple_fwd_pipe_entry *)&ctx->data[0];

	if (entry->is_hw) {
		doca_flow_pipe_remove_entry(entry->pipe_queue, DOCA_FLOW_NO_WAIT, entry->hw_entry);
		entry->hw_entry = NULL;
	}
}

/*
 * Destroy flow table used by the application
 *
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_destroy_ins(void)
{
	uint16_t idx;

	if (simple_fwd_ins == NULL)
		return 0;

	simple_fwd_ft_destroy(simple_fwd_ins->ft);

	for (idx = 0; idx < SIMPLE_FWD_PORTS; idx++) {
		if (simple_fwd_ins->ports[idx])
			doca_flow_port_stop(simple_fwd_ins->ports[idx]);
	}
	free(simple_fwd_ins);
	simple_fwd_ins = NULL;
	return 0;
}

/*
 * Destroy application allocated resources
 *
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_destroy(void)
{
	simple_fwd_destroy_ins();
	doca_flow_destroy();
	return 0;
}

/*
 * Initializes flow tables used by the application for a given port
 *
 * @port_cfg [in]: the port configuration to allocate the resources
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_create_ins(struct simple_fwd_port_cfg *port_cfg)
{
	uint16_t index;

	simple_fwd_ins = (struct simple_fwd_app *)
		calloc(1, sizeof(struct simple_fwd_app) + sizeof(struct doca_flow_aged_query *) * port_cfg->nb_queues);
	if (simple_fwd_ins == NULL) {
		DOCA_LOG_ERR("Failed to allocate SF");
		goto fail_init;
	}

	simple_fwd_ins->ft = simple_fwd_ft_create(SIMPLE_FWD_MAX_FLOWS,
						  sizeof(struct simple_fwd_pipe_entry),
						  &simple_fwd_aged_flow_cb,
						  NULL,
						  port_cfg->age_thread);
	if (simple_fwd_ins->ft == NULL) {
		DOCA_LOG_ERR("Failed to allocate FT");
		goto fail_init;
	}
	simple_fwd_ins->nb_queues = port_cfg->nb_queues;
	for (index = 0; index < SIMPLE_FWD_PORTS; index++)
		simple_fwd_ins->hairpin_peer[index] = index ^ 1;
	return 0;
fail_init:
	simple_fwd_destroy_ins();
	return -1;
}

/*
 * Create DOCA Flow "RSS pipe" and adds entry that match every packet, and forwards to SW, for a given port
 *
 * @port_id [in]: port identifier
 * @return: 0 on success, negative value otherwise and error is set.
 */
static int simple_fwd_build_rss_flow(uint16_t port_id)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	struct simple_fwd_port_cfg *port_cfg =
		((struct simple_fwd_port_cfg *)doca_flow_port_priv_data(simple_fwd_ins->ports[port_id]));
	struct entries_status *status;
	uint16_t rss_queues[port_cfg->nb_queues];
	int num_of_entries = 1;
	int i;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, simple_fwd_ins->ports[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "RSS_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTION_ARRAY);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	for (i = 0; i < port_cfg->nb_queues; i++)
		rss_queues[i] = i;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = port_cfg->nb_queues;
	fwd.rss.queues_array = rss_queues;

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));
	if (status == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for pipe");
		goto destroy_pipe_cfg;
	}

	status->port = simple_fwd_ins->ports[port_cfg->port_id];

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &simple_fwd_ins->pipe_rss[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		free(status);
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_add_entry(0,
					  simple_fwd_ins->pipe_rss[port_cfg->port_id],
					  &match,
					  &actions,
					  NULL,
					  &fwd,
					  0,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}
	result = doca_flow_entries_process(simple_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, num_of_entries);
	if (result != DOCA_SUCCESS)
		return -1;

	if (status->nb_processed != num_of_entries || status->failure)
		return -1;

	return 0;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return -1;
}

/*
 * Create DOCA Flow hairpin pipe and adds entry that match every packet for a given port
 *
 * @port_id [in]: port identifier
 * @return: 0 on success, negative value otherwise and error is set.
 */
static int simple_fwd_build_hairpin_flow(uint16_t port_id)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	struct simple_fwd_port_cfg *port_cfg =
		((struct simple_fwd_port_cfg *)doca_flow_port_priv_data(simple_fwd_ins->ports[port_id]));
	struct entries_status *status;
	int num_of_entries = 1;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, simple_fwd_ins->ports[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "HAIRPIN_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTION_ARRAY);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = simple_fwd_ins->vxlan_encap_pipe[port_cfg->port_id ^ 1];

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));
	if (status == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for entries status");
		goto destroy_pipe_cfg;
	}

	status->port = simple_fwd_ins->ports[port_cfg->port_id];

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &simple_fwd_ins->pipe_hairpin[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		free(status);
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_add_entry(0,
					  simple_fwd_ins->pipe_hairpin[port_cfg->port_id],
					  &match,
					  &actions,
					  NULL,
					  &fwd,
					  0,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	result = doca_flow_entries_process(simple_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, num_of_entries);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	if (status->nb_processed != num_of_entries || status->failure) {
		free(status);
		return -1;
	}

	return 0;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return -1;
}

/*
 * Build DOCA Flow FWD component based on the port configuration provided by the user
 *
 * @port_cfg [in]: port configuration as provided by the user
 * @fwd [out]: DOCA Flow FWD component to fill its fields
 */
static void simple_fwd_build_fwd(struct simple_fwd_port_cfg *port_cfg, struct doca_flow_fwd *fwd)
{
	if (port_cfg->is_hairpin > 0) {
		fwd->type = DOCA_FLOW_FWD_PIPE;
		fwd->next_pipe = simple_fwd_ins->vxlan_encap_pipe[port_cfg->port_id ^ 1];
	}

	else {
		fwd->type = DOCA_FLOW_FWD_PIPE;
		fwd->next_pipe = simple_fwd_ins->pipe_rss[port_cfg->port_id];
	}
}

/*
 * Build common fields in the DOCA Flow match for layers in DOCA Flow match for VxLAN, GRE and GTP pipes creation
 *
 * @match [out]: DOCA Flow match to fill its inner layers
 */
static void simple_fwd_build_pipe_common_match_fields(struct doca_flow_match *match)
{
	if (match->tun.type != DOCA_FLOW_TUN_GRE) {
		match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		match->parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV4;
		match->parser_meta.inner_l4_type = DOCA_FLOW_L4_META_TCP;
	}

	match->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match->outer.ip4.src_ip = UINT32_MAX;
	match->outer.ip4.dst_ip = UINT32_MAX;
	match->inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match->inner.ip4.src_ip = UINT32_MAX;
	match->inner.ip4.dst_ip = UINT32_MAX;
	match->inner.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	match->inner.tcp.l4_port.src_port = UINT16_MAX;
	match->inner.tcp.l4_port.dst_port = UINT16_MAX;
}

/*
 * Create DOCA Flow pipe that match VXLAN traffic with changeable VXLAN tunnel ID and decap action
 *
 * @port_cfg [in]: port configuration as provided by the user
 * @type [in]: DOCA Flow tunnel type to determine what pipe to create
 * @return: 0 on success, negative value otherwise and error is set
 */
static int simple_fwd_create_match_pipe(struct simple_fwd_port_cfg *port_cfg, enum doca_flow_tun_type type)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_action_descs descs;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe **pipe;
	const char *pipe_name;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&descs, 0, sizeof(descs));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	actions_arr[0] = &actions;

	match.tun.type = type;
	simple_fwd_build_pipe_common_match_fields(&match);

	switch (type) {
	case DOCA_FLOW_TUN_VXLAN:
		pipe_name = "VXLAN_PIPE";
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
		match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
		match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
		match.tun.vxlan_tun_id = UINT32_MAX;
		actions.meta.pkt_meta = DOCA_HTOBE32(1);
		actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		actions.decap_cfg.is_l2 = true;
		pipe = &simple_fwd_ins->pipe_vxlan[port_cfg->port_id];
		break;
	case DOCA_FLOW_TUN_GTPU:
		pipe_name = "GTP_FWD";
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
		match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
		match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(DOCA_FLOW_GTPU_DEFAULT_PORT);
		match.tun.gtp_teid = UINT32_MAX;

		actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		actions.decap_cfg.is_l2 = false;
		SET_MAC_ADDR(actions.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
		SET_MAC_ADDR(actions.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
		actions.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
		pipe = &simple_fwd_ins->pipe_gtp[port_cfg->port_id];
		break;
	case DOCA_FLOW_TUN_GRE:
		pipe_name = "GRE_PIPE";
		match.tun.gre_key = UINT32_MAX;
		match.tun.key_present = true;
		actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		actions.decap_cfg.is_l2 = false;
		SET_MAC_ADDR(actions.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
		SET_MAC_ADDR(actions.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
		actions.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
		actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		actions.meta.pkt_meta = DOCA_HTOBE32(1);
		pipe = &simple_fwd_ins->pipe_gre[port_cfg->port_id];
		break;
	default:
		return -1;
	}

	/* build monitor part */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	monitor.aging_sec = 0xffffffff;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, simple_fwd_ins->ports[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTION_ARRAY);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	simple_fwd_build_fwd(port_cfg, &fwd);

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = simple_fwd_ins->pipe_rss[port_cfg->port_id];

	if (doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe) != DOCA_SUCCESS)
		goto destroy_pipe_cfg;

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	return 0;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return -1;
}

/*
 * Create DOCA Flow control pipe
 *
 * @port_cfg [in]: port configuration as provided by the user
 * @return: 0 on success, negative value otherwise and error is set.
 */
static int simple_fwd_create_control_pipe(struct simple_fwd_port_cfg *port_cfg)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, simple_fwd_ins->ports[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "CONTROL_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (doca_flow_pipe_create(pipe_cfg, NULL, NULL, &simple_fwd_ins->pipe_control[port_cfg->port_id]) !=
	    DOCA_SUCCESS)
		goto destroy_pipe_cfg;
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return 0;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return -1;
}

/*
 * Add DOCA Flow pipe entries to the control pipe:
 * - entry with VXLAN match that forward the matched packet to VxLAN pipe
 * - entry with GRE match that forward the matched packet to GRE pipe
 * - entry with GTP match that forward the matched packet to GTP pipe
 *
 * @port_cfg [in]: port configuration as provided by the user
 * @return: 0 on success, negative value otherwise and error is set.
 */
static int simple_fwd_add_control_pipe_entries(struct simple_fwd_port_cfg *port_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_entry *entry;
	struct entries_status *status;
	doca_error_t result;
	uint8_t priority = 0;
	int nb_entries = 4;

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));
	if (unlikely(status == NULL))
		return -1;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(DOCA_FLOW_VXLAN_DEFAULT_PORT);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = simple_fwd_ins->pipe_vxlan[port_cfg->port_id];

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  simple_fwd_ins->pipe_control[port_cfg->port_id],
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &fwd,
						  status,
						  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.next_proto = DOCA_FLOW_PROTO_GRE;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = simple_fwd_ins->pipe_gre[port_cfg->port_id];

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  simple_fwd_ins->pipe_control[port_cfg->port_id],
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &fwd,
						  status,
						  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(DOCA_FLOW_GTPU_DEFAULT_PORT);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = simple_fwd_ins->pipe_gtp[port_cfg->port_id];

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  simple_fwd_ins->pipe_control[port_cfg->port_id],
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &fwd,
						  status,
						  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	priority = 1;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = simple_fwd_ins->pipe_hairpin[port_cfg->port_id];

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  simple_fwd_ins->pipe_control[port_cfg->port_id],
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &fwd,
						  status,
						  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	result = doca_flow_entries_process(simple_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, nb_entries);
	if (result != DOCA_SUCCESS)
		return result;

	if (status->nb_processed != nb_entries || status->failure)
		return DOCA_ERROR_BAD_STATE;

	return 0;
}

/*
 * Create DOCA Flow pipe on EGRESS domain with match on the packet meta and encap action with changeable values
 *
 * @port_cfg [in]: port configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t simple_fwd_create_vxlan_encap_pipe(struct simple_fwd_port_cfg *port_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* match on pkt meta */
	match_mask.meta.pkt_meta = UINT32_MAX;

	/* build basic outer VXLAN encap data*/
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions.encap_cfg.encap.outer.udp.l4_port.dst_port = RTE_BE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	actions.encap_cfg.encap.tun.vxlan_tun_id = 0xffffffff;
	actions.encap_cfg.is_l2 = true;
	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, simple_fwd_ins->ports[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "VXLAN_ENCAP_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTION_ARRAY);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* forwarding traffic to the wire */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_cfg->port_id;

	if (doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &simple_fwd_ins->vxlan_encap_pipe[port_cfg->port_id]) !=
	    DOCA_SUCCESS)
		goto destroy_pipe_cfg;

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	return 0;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return -1;
}

/*
 * Add DOCA Flow pipe entry with example encap values
 *
 * @port_cfg [in]: port configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t simple_fwd_add_vxlan_encap_pipe_entry(struct simple_fwd_port_cfg *port_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	struct entries_status *status;
	doca_error_t result;
	int num_of_entries = 1;

	doca_be32_t encap_dst_ip_addr = BE_IPV4_ADDR(81, 81, 81, 81);
	doca_be32_t encap_src_ip_addr = BE_IPV4_ADDR(11, 21, 31, 41);
	uint8_t encap_ttl = 17;
	doca_be32_t encap_vxlan_tun_id = DOCA_HTOBE32(0xadadad);
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));
	if (status == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for entries status");
		return DOCA_ERROR_NO_MEMORY;
	}

	status->port = simple_fwd_ins->ports[port_cfg->port_id];

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	match.meta.pkt_meta = DOCA_HTOBE32(1);

	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);
	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.encap_cfg.encap.outer.ip4.src_ip = encap_src_ip_addr;
	actions.encap_cfg.encap.outer.ip4.dst_ip = encap_dst_ip_addr;
	actions.encap_cfg.encap.outer.ip4.ttl = encap_ttl;
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	actions.encap_cfg.encap.tun.vxlan_tun_id = encap_vxlan_tun_id;
	actions.action_idx = 0;

	result = doca_flow_pipe_add_entry(0,
					  simple_fwd_ins->vxlan_encap_pipe[port_cfg->port_id],
					  &match,
					  &actions,
					  NULL,
					  NULL,
					  0,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return result;
	}

	result = doca_flow_entries_process(simple_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, num_of_entries);
	if (result != DOCA_SUCCESS)
		return result;

	if (status->nb_processed != num_of_entries || status->failure)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

/*
 * Initialize simple FWD application DOCA Flow ports and pipes
 *
 * @port_cfg [in]: a pointer to the port configuration to initialize
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_init_ports_and_pipes(struct simple_fwd_port_cfg *port_cfg)
{
	int nb_ports = SIMPLE_FWD_PORTS;
	struct simple_fwd_port_cfg *curr_port_cfg;
	int port_id;
	int result;

	if (simple_fwd_init_doca_flow(port_cfg->nb_queues, "vnf,hws", port_cfg->nb_counters, port_cfg->nb_meters) < 0) {
		DOCA_LOG_ERR("Failed to init DOCA Flow");
		simple_fwd_destroy_ins();
		return -1;
	}

	if (simple_fwd_init_doca_flow_ports(nb_ports, simple_fwd_ins->ports, true) < 0) {
		DOCA_LOG_ERR("Failed to init DOCA ports");
		return -1;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		curr_port_cfg =
			((struct simple_fwd_port_cfg *)doca_flow_port_priv_data(simple_fwd_ins->ports[port_id]));
		curr_port_cfg->port_id = port_id;
		curr_port_cfg->nb_queues = port_cfg->nb_queues;
		curr_port_cfg->is_hairpin = port_cfg->is_hairpin;
		curr_port_cfg->nb_meters = port_cfg->nb_meters;
		curr_port_cfg->nb_counters = port_cfg->nb_counters;
		curr_port_cfg->age_thread = port_cfg->age_thread;

		curr_port_cfg->port_id = port_id ^ 1;
		result = simple_fwd_create_vxlan_encap_pipe(curr_port_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create vxlan encap pipe: %s", doca_error_get_descr(result));
			return -1;
		}

		result = simple_fwd_add_vxlan_encap_pipe_entry(curr_port_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry to vxlan encap pipe: %s", doca_error_get_descr(result));
			return -1;
		}

		curr_port_cfg->port_id = port_id;
		result = simple_fwd_build_hairpin_flow(curr_port_cfg->port_id);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building hairpin flow");
			return -1;
		}

		result = simple_fwd_build_rss_flow(curr_port_cfg->port_id);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building RSS flow");
			return -1;
		}

		result = simple_fwd_create_match_pipe(curr_port_cfg, DOCA_FLOW_TUN_VXLAN);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building VXLAN pipe");
			return -1;
		}

		result = simple_fwd_create_match_pipe(curr_port_cfg, DOCA_FLOW_TUN_GTPU);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building GTPU pipe");
			return -1;
		}

		result = simple_fwd_create_match_pipe(curr_port_cfg, DOCA_FLOW_TUN_GRE);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building GRE pipe");
			return -1;
		}

		result = simple_fwd_create_control_pipe(curr_port_cfg);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building control pipe");
			return -1;
		}

		result = simple_fwd_add_control_pipe_entries(curr_port_cfg);
		if (result < 0) {
			DOCA_LOG_ERR("Failed adding entries to the control pipe");
			return -1;
		}
	}

	return 0;
}

/*
 * Initialize simple FWD application resources
 *
 * @p [in]: a pointer to the port configuration
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_init(void *p)
{
	struct simple_fwd_port_cfg *port_cfg;
	int ret = 0;

	port_cfg = (struct simple_fwd_port_cfg *)p;
	ret = simple_fwd_create_ins(port_cfg);
	if (ret)
		return ret;
	return simple_fwd_init_ports_and_pipes(port_cfg);
}

/*
 * Setting tunneling type in the match component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @match [out]: match component to set the tunneling type in based on the packet info provided
 */
static inline void simple_fwd_match_set_tun(struct simple_fwd_pkt_info *pinfo, struct doca_flow_match *match)
{
	if (!pinfo->tun_type)
		return;
	match->tun.type = pinfo->tun_type;
	switch (match->tun.type) {
	case DOCA_FLOW_TUN_VXLAN:
		match->tun.vxlan_tun_id = DOCA_HTOBE32(DOCA_BETOH32(pinfo->tun.vni) >> 8);
		break;
	case DOCA_FLOW_TUN_GRE:
		match->tun.gre_key = pinfo->tun.gre_key;
		break;
	case DOCA_FLOW_TUN_GTPU:
		match->tun.gtp_teid = pinfo->tun.teid;
		break;
	default:
		DOCA_LOG_WARN("Unsupported tunnel type:%u", match->tun.type);
		break;
	}
}

/*
 * Transfer packet l4 type to doca l4 type
 *
 * @pkt_l4_type [in]: the l4 type of eth packet
 * @return: the doca type of the packet l4 type
 */
static enum doca_flow_l4_type_ext simple_fwd_l3_type_transfer(uint8_t pkt_l4_type)
{
	switch (pkt_l4_type) {
	case DOCA_FLOW_PROTO_TCP:
		return DOCA_FLOW_L4_TYPE_EXT_TCP;
	case DOCA_FLOW_PROTO_UDP:
		return DOCA_FLOW_L4_TYPE_EXT_UDP;
	case DOCA_FLOW_PROTO_GRE:
		/* set gre type in doca_flow_tun type */
		return DOCA_FLOW_L4_TYPE_EXT_NONE;
	default:
		DOCA_LOG_WARN("The L4 type %u is not supported", pkt_l4_type);
		return DOCA_FLOW_L4_TYPE_EXT_NONE;
	}
}

/*
 * Build action component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @match [out]: the actions component to build
 */
static void simple_fwd_build_entry_actions(struct doca_flow_actions *actions)
{
	SET_MAC_ADDR(actions->outer.eth.src_mac, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
	SET_MAC_ADDR(actions->outer.eth.dst_mac, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
}

/*
 * Build match component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @match [out]: the match component to build
 */
static void simple_fwd_build_entry_match(struct simple_fwd_pkt_info *pinfo, struct doca_flow_match *match)
{
	memset(match, 0x0, sizeof(*match));
	/* set match all fields, pipe will select which field to match */
	memcpy(match->outer.eth.dst_mac, simple_fwd_pinfo_outer_mac_dst(pinfo), DOCA_FLOW_ETHER_ADDR_LEN);
	memcpy(match->outer.eth.src_mac, simple_fwd_pinfo_outer_mac_src(pinfo), DOCA_FLOW_ETHER_ADDR_LEN);
	match->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match->outer.ip4.dst_ip = simple_fwd_pinfo_outer_ipv4_dst(pinfo);
	match->outer.ip4.src_ip = simple_fwd_pinfo_outer_ipv4_src(pinfo);
	match->outer.l4_type_ext = simple_fwd_l3_type_transfer(pinfo->outer.l4_type);
	SET_L4_PORT(outer, src_port, simple_fwd_pinfo_outer_src_port(pinfo));
	SET_L4_PORT(outer, dst_port, simple_fwd_pinfo_outer_dst_port(pinfo));
	if (!pinfo->tun_type)
		return;
	simple_fwd_match_set_tun(pinfo, match);
	match->inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match->inner.ip4.dst_ip = simple_fwd_pinfo_inner_ipv4_dst(pinfo);
	match->inner.ip4.src_ip = simple_fwd_pinfo_inner_ipv4_src(pinfo);
	match->inner.l4_type_ext = simple_fwd_l3_type_transfer(pinfo->inner.l4_type);
	SET_L4_PORT(inner, src_port, simple_fwd_pinfo_inner_src_port(pinfo));
	SET_L4_PORT(inner, dst_port, simple_fwd_pinfo_inner_dst_port(pinfo));
}

/*
 * Build monitor component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @monitor [out]: the monitor component to build
 */
static void simple_fwd_build_entry_monitor(struct simple_fwd_pkt_info *pinfo, struct doca_flow_monitor *monitor)
{
	(void)pinfo;

	monitor->counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	/* flows will be aged out in 5 - 60s */
	monitor->aging_sec = (uint32_t)rte_rand() % 55 + 5;
}

/*
 * Selects the pipe based on the tunneling type
 *
 * @pinfo [in]: the packet info as represented in the application
 * @return: a pointer for the selected pipe on success and NULL otherwise
 */
static struct doca_flow_pipe *simple_fwd_select_pipe(struct simple_fwd_pkt_info *pinfo)
{
	if (pinfo->tun_type == DOCA_FLOW_TUN_GRE)
		return simple_fwd_ins->pipe_gre[pinfo->orig_port_id];
	if (pinfo->tun_type == DOCA_FLOW_TUN_VXLAN)
		return simple_fwd_ins->pipe_vxlan[pinfo->orig_port_id];
	if (pinfo->tun_type == DOCA_FLOW_TUN_GTPU)
		return simple_fwd_ins->pipe_gtp[pinfo->orig_port_id];
	return NULL;
}

/*
 * Adds new entry, with respect to the packet info, to the flow table
 *
 * @pinfo [in]: the packet info as represented in the application
 * @user_ctx [in]: user context
 * @age_sec [out]: Aging time for the created entry in seconds
 * @return: created entry pointer on success and NULL otherwise
 */
static struct doca_flow_pipe_entry *simple_fwd_pipe_add_entry(struct simple_fwd_pkt_info *pinfo,
							      void *user_ctx,
							      uint32_t *age_sec)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor = {};
	struct doca_flow_actions actions = {0};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entries_status *status;
	int num_of_entries = 1;
	doca_error_t result;

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));
	if (status == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for entries status");
		return NULL;
	}

	status->port = simple_fwd_ins->ports[pinfo->orig_port_id];

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	pipe = simple_fwd_select_pipe(pinfo);
	if (pipe == NULL) {
		DOCA_LOG_WARN("Failed to select pipe on this packet");
		free(status);
		return NULL;
	}

	actions.meta.pkt_meta = DOCA_HTOBE32(1);
	actions.action_idx = 0;

	status->ft_entry = user_ctx;

	if (pinfo->tun_type != DOCA_FLOW_TUN_VXLAN) {
		simple_fwd_build_entry_actions(&actions);
	}

	simple_fwd_build_entry_match(pinfo, &match);
	simple_fwd_build_entry_monitor(pinfo, &monitor);
	result = doca_flow_pipe_add_entry(pinfo->pipe_queue,
					  pipe,
					  &match,
					  &actions,
					  &monitor,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed adding entry to pipe");
		free(status);
		return NULL;
	}

	result = doca_flow_entries_process(simple_fwd_ins->ports[pinfo->orig_port_id],
					   pinfo->pipe_queue,
					   PULL_TIME_OUT,
					   num_of_entries);

	if (result != DOCA_SUCCESS)
		goto error;

	if (status->nb_processed != num_of_entries || status->failure)
		goto error;

	*age_sec = monitor.aging_sec;
	return entry;

error:
	doca_flow_pipe_remove_entry(pinfo->pipe_queue, DOCA_FLOW_NO_WAIT, entry);
	return NULL;
}

/*
 * Adds new flow, with respect to the packet info, to the flow table
 *
 * @pinfo [in]: the packet info as represented in the application
 * @ctx [in]: user context
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_handle_new_flow(struct simple_fwd_pkt_info *pinfo, struct simple_fwd_ft_user_ctx **ctx)
{
	doca_error_t result;
	struct simple_fwd_pipe_entry *entry = NULL;
	struct simple_fwd_ft_entry *ft_entry;
	uint32_t age_sec;

	result = simple_fwd_ft_add_new(simple_fwd_ins->ft, pinfo, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_DBG("Failed create new entry");
		return -1;
	}
	ft_entry = GET_FT_ENTRY(*ctx);
	entry = (struct simple_fwd_pipe_entry *)&(*ctx)->data[0];
	entry->pipe_queue = pinfo->pipe_queue;
	entry->hw_entry = simple_fwd_pipe_add_entry(pinfo, (void *)(*ctx), &age_sec);
	if (entry->hw_entry == NULL) {
		simple_fwd_ft_destroy_entry(simple_fwd_ins->ft, ft_entry);
		return -1;
	}
	simple_fwd_ft_update_age_sec(ft_entry, age_sec);
	simple_fwd_ft_update_expiration(ft_entry);
	entry->is_hw = true;

	return 0;
}

/*
 * Checks whether or not the received packet info is new.
 *
 * @pinfo [in]: the packet info as represented in the application
 * @return: true on success and false otherwise
 */
static bool simple_fwd_need_new_ft(struct simple_fwd_pkt_info *pinfo)
{
	if (pinfo->outer.l3_type != IPV4) {
		DOCA_LOG_WARN("The outer L3 type %u is not supported", pinfo->outer.l3_type);
		return false;
	}
	if ((pinfo->outer.l4_type != DOCA_FLOW_PROTO_TCP) && (pinfo->outer.l4_type != DOCA_FLOW_PROTO_UDP) &&
	    (pinfo->outer.l4_type != DOCA_FLOW_PROTO_GRE)) {
		DOCA_LOG_WARN("The outer L4 type %u is not supported", pinfo->outer.l4_type);
		return false;
	}
	return true;
}

/*
 * Adjust the mbuf pointer, to point on the packet's raw data
 *
 * @pinfo [in]: packet info representation  in the application
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_handle_packet(struct simple_fwd_pkt_info *pinfo)
{
	struct simple_fwd_ft_user_ctx *ctx = NULL;
	struct simple_fwd_pipe_entry *entry = NULL;

	if (!simple_fwd_need_new_ft(pinfo))
		return -1;
	if (simple_fwd_ft_find(simple_fwd_ins->ft, pinfo, &ctx) != DOCA_SUCCESS) {
		if (simple_fwd_handle_new_flow(pinfo, &ctx))
			return -1;
	}
	entry = (struct simple_fwd_pipe_entry *)&ctx->data[0];
	entry->total_pkts++;

	return 0;
}

/*
 * Handles aged flows
 *
 * @port_id [in]: port identifier of the port to handle its aged flows
 * @queue [in]: queue index of the queue to handle its aged flows
 */
static void simple_fwd_handle_aging(uint32_t port_id, uint16_t queue)
{
#define MAX_HANDLING_TIME_MS 10 /*ms*/

	if (queue > simple_fwd_ins->nb_queues)
		return;
	doca_flow_aging_handle(simple_fwd_ins->ports[port_id], queue, MAX_HANDLING_TIME_MS, 0);
}

/*
 * Dump stats of the given port identifier
 *
 * @port_id [in]: port identifier to dump its stats
 * @return: 0 on success and non-zero value on failure
 */
static int simple_fwd_dump_stats(uint32_t port_id)
{
	return simple_fwd_dump_port_stats(port_id);
}

/* Stores all functions pointers used by the application */
static struct app_vnf simple_fwd_vnf = {
	.vnf_init = &simple_fwd_init,		      /* Simple Forward initialization resources function pointer */
	.vnf_process_pkt = &simple_fwd_handle_packet, /* Simple Forward packet processing function pointer */
	.vnf_flow_age = &simple_fwd_handle_aging,     /* Simple Forward aging handling function pointer */
	.vnf_dump_stats = &simple_fwd_dump_stats,     /* Simple Forward dumping stats function pointer */
	.vnf_destroy = &simple_fwd_destroy,	      /* Simple Forward destroy allocated resources function pointer */
};

/*
 * Sets and stores all function pointers, in order to  call them later in the application
 */
struct app_vnf *simple_fwd_get_vnf(void)
{
	return &simple_fwd_vnf;
}
