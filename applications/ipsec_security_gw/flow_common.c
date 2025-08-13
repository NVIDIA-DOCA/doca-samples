/*
 * Copyright (c) 2023-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <stdlib.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_tune_server.h>
#include <doca_bitfield.h>

#include "utils.h"
#include "flow_common.h"
#include "flow_decrypt.h"
#include "flow_encrypt.h"

DOCA_LOG_REGISTER(IPSEC_SECURITY_GW::flow_common);

/*
 * Entry processing callback
 *
 * @entry [in]: entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
static void check_for_valid_entry(struct doca_flow_pipe_entry *entry,
				  uint16_t pipe_queue,
				  enum doca_flow_entry_status status,
				  enum doca_flow_entry_op op,
				  void *user_ctx)
{
	(void)entry;
	(void)op;
	(void)pipe_queue;

	struct entries_status *entry_status = (struct entries_status *)user_ctx;

	if (entry_status == NULL || op != DOCA_FLOW_ENTRY_OP_ADD)
		return;
	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->failure = true; /* set failure to true if processing failed */
	entry_status->nb_processed++;
	entry_status->entries_in_queue--;
}

/*
 * Process entries and check the returned status
 *
 * @port [in]: the port we want to process in
 * @status [in]: the entries status that was sent to the pipe
 * @timeout [in]: timeout for the entries process function
 * @pipe_queue [in]: queue identifier
 */
doca_error_t process_entries(struct doca_flow_port *port,
			     struct entries_status *status,
			     int timeout,
			     uint16_t pipe_queue)
{
	doca_error_t result;

	result = doca_flow_entries_process(port, pipe_queue, timeout, status->entries_in_queue);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		return result;
	}
	if (status->failure || status->entries_in_queue == QUEUE_DEPTH) {
		DOCA_LOG_ERR("Failed to process entries");
		return DOCA_ERROR_BAD_STATE;
	}
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow port by port id
 *
 * @port_id [in]: port ID
 * @dev [in]: DOCA device pointer
 * @dev_rep [in]: DOCA device representor pointer
 * @sn_offload_disable [in]: disable SN offload
 * @port [out]: pointer to port handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_flow_port(int port_id,
					  struct doca_dev *dev,
					  struct doca_dev_rep *dev_rep,
					  bool sn_offload_disable,
					  struct doca_flow_port **port)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result, tmp_result;

	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port_id: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg device: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_cfg_set_dev_rep(port_cfg, dev_rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg device representor: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	result = doca_flow_port_cfg_set_actions_mem_size(port_cfg, rte_align32pow2(MAX_ACTIONS_MEM_SIZE));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg actions memory size: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	if (sn_offload_disable) {
		result = doca_flow_port_cfg_set_ipsec_sn_offload_disable(port_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_port_cfg sn offload disable: %s",
				     doca_error_get_descr(result));
			goto destroy_port_cfg;
		}
	}

	result = doca_flow_port_start(port_cfg, port);
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

	return result;
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

doca_error_t ipsec_security_gw_init_doca_flow(const struct ipsec_security_gw_config *app_cfg,
					      int nb_queues,
					      struct ipsec_security_gw_ports_map *ports[])
{
	int port_id;
	int port_idx = 0;
	int nb_ports = 0;
	struct doca_dev *dev;
	struct doca_dev_rep *rep;
	struct doca_flow_cfg *flow_cfg;
	struct doca_flow_tune_server_cfg *server_cfg;
	struct doca_flow_resource_rss_cfg rss = {0};
	uint16_t rss_queues[nb_queues];
	char *mode_args;
	doca_error_t result;
	bool sn_offload_disable;

	memset(&flow_cfg, 0, sizeof(flow_cfg));

	/* init doca flow with crypto shared resources */
	result = doca_flow_cfg_create(&flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_flow_cfg_set_pipe_queues(flow_cfg, nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}
	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF)
		mode_args = "vnf,hws,isolated";
	else
		mode_args = "switch,hws,isolated,expert";
	result = doca_flow_cfg_set_mode_args(flow_cfg, mode_args);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}
	result = doca_flow_cfg_set_queue_depth(flow_cfg, QUEUE_DEPTH);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg queue_depth: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}
	result = doca_flow_cfg_set_cb_entry_process(flow_cfg, check_for_valid_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg cb_entry_process: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}

	result = doca_flow_cfg_set_nr_counters(flow_cfg, MAX_NB_RULES * NUM_OF_SYNDROMES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}

	result = configure_tune_server(flow_cfg, DOCA_FLOW_TUNE_PROFILE_FULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure tune server: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}

	uint32_t nb_of_rules = app_cfg->app_rules.nb_decrypt_rules + app_cfg->app_rules.nb_encrypt_rules;
	uint32_t nr_shared_resources = app_cfg->socket_ctx.socket_conf ? MAX_NB_RULES * 2 : nb_of_rules; /* for both
													    encrypt and
													    decrypt */
	result =
		doca_flow_cfg_set_nr_shared_resource(flow_cfg, nr_shared_resources, DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_shared_resources: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}

	linear_array_init_u16(rss_queues, nb_queues);
	rss.nr_queues = nb_queues;
	rss.queues_array = rss_queues;
	result = doca_flow_cfg_set_default_rss(flow_cfg, &rss);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg rss: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}
	result = doca_flow_init(flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}
	doca_flow_cfg_destroy(flow_cfg);

	sn_offload_disable = app_cfg->sw_sn_inc_enable && app_cfg->sw_antireplay;
	for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
		/* search for the probed devices */
		if (!rte_eth_dev_is_valid_port(port_id))
			continue;
		/* get device idx for ports array - secured or unsecured */
		if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
			rep = NULL;
			result = find_port_action_type_vnf(app_cfg, port_id, &dev, &port_idx);
			if (result != DOCA_SUCCESS)
				return result;
		} else {
			result = find_port_action_type_switch(port_id, &port_idx);
			if (result != DOCA_SUCCESS)
				return result;
			if (port_idx == SECURED_IDX) {
				dev = app_cfg->objects.secured_dev.doca_dev;
				rep = NULL;
			} else {
				dev = NULL;
				rep = app_cfg->objects.unsecured_dev.doca_dev_rep;
			}
		}

		ports[port_idx] = malloc(sizeof(struct ipsec_security_gw_ports_map));
		if (ports[port_idx] == NULL) {
			DOCA_LOG_ERR("malloc() failed");
			doca_flow_cleanup(nb_ports, ports);
			return DOCA_ERROR_NO_MEMORY;
		}
		result = create_doca_flow_port(port_id, dev, rep, sn_offload_disable, &ports[port_idx]->port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init DOCA Flow port: %s", doca_error_get_descr(result));
			free(ports[port_idx]);
			doca_flow_cleanup(nb_ports, ports);
			return result;
		}
		nb_ports++;
		ports[port_idx]->port_id = port_id;
	}
	if (ports[SECURED_IDX]->port == NULL || ports[UNSECURED_IDX]->port == NULL) {
		DOCA_LOG_ERR("Failed to init two DOCA Flow ports");
		doca_flow_cleanup(nb_ports, ports);
		return DOCA_ERROR_INITIALIZATION;
	}
	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		result = doca_flow_port_pair(ports[SECURED_IDX]->port, ports[UNSECURED_IDX]->port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to pair ports");
			doca_flow_cleanup(nb_ports, ports);
			return DOCA_ERROR_INITIALIZATION;
		}

		result = doca_flow_port_pair(ports[UNSECURED_IDX]->port, ports[SECURED_IDX]->port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to pair ports");
			doca_flow_cleanup(nb_ports, ports);
			return DOCA_ERROR_INITIALIZATION;
		}
	}
	/* Init DOCA Flow Tune Server */
	result = doca_flow_tune_server_cfg_create(&server_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow tune server configuration");
		doca_flow_cleanup(nb_ports, ports);
		return result;
	}
	result = doca_flow_tune_server_init(server_cfg);
	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_DBG("DOCA Flow Tune Server isn't supported in this runtime version");
		} else {
			DOCA_LOG_ERR("Failed to initialize the flow tune server");
			doca_flow_tune_server_cfg_destroy(server_cfg);
			doca_flow_cleanup(nb_ports, ports);
			return result;
		}
	}
	doca_flow_tune_server_cfg_destroy(server_cfg);
	return DOCA_SUCCESS;
}

doca_error_t ipsec_security_gw_init_status(struct ipsec_security_gw_config *app_cfg, int nb_queues)
{
	app_cfg->secured_status = (struct entries_status *)malloc(sizeof(struct entries_status) * nb_queues);
	if (app_cfg->secured_status == NULL) {
		DOCA_LOG_ERR("malloc() status array failed");
		return DOCA_ERROR_NO_MEMORY;
	}

	app_cfg->unsecured_status = (struct entries_status *)malloc(sizeof(struct entries_status) * nb_queues);
	if (app_cfg->unsecured_status == NULL) {
		DOCA_LOG_ERR("malloc() status array failed");
		free(app_cfg->secured_status);
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

doca_error_t ipsec_security_gw_bind(struct ipsec_security_gw_ports_map *ports[],
				    struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_port *secured_port;
	doca_error_t result;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		secured_port = ports[SECURED_IDX]->port;
	} else {
		secured_port = doca_flow_port_switch_get(NULL);
	}
	result = bind_encrypt_ids(app_cfg->app_rules.nb_encrypt_rules, secured_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind IDs: %s", doca_error_get_descr(result));
		return result;
	}

	result = bind_decrypt_ids(app_cfg->app_rules.nb_decrypt_rules,
				  app_cfg->app_rules.nb_encrypt_rules,
				  secured_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind IDs: %s", doca_error_get_descr(result));
		return result;
	}
	return result;
}

void doca_flow_cleanup(int nb_ports, struct ipsec_security_gw_ports_map *ports[])
{
	int port_id;

	for (port_id = nb_ports - 1; port_id >= 0; port_id--) {
		if (ports[port_id] != NULL) {
			doca_flow_port_stop(ports[port_id]->port);
			free(ports[port_id]);
		}
	}

	doca_flow_destroy();
}

uint32_t get_icv_len_int(enum doca_flow_crypto_icv_len icv_len)
{
	if (icv_len == DOCA_FLOW_CRYPTO_ICV_LENGTH_8)
		return 8;
	else if (icv_len == DOCA_FLOW_CRYPTO_ICV_LENGTH_12)
		return 12;
	else
		return 16;
}

doca_error_t create_rss_pipe(struct ipsec_security_gw_config *app_cfg,
			     struct doca_flow_port *port,
			     uint16_t nb_queues,
			     struct doca_flow_pipe **rss_pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	int num_of_entries = 4;
	uint16_t rss_queues[nb_queues - 1];
	int i;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	bool is_root = (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF);

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	meta.encrypt = 1;
	meta.decrypt = 1;
	match_mask.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.nr_queues = -1; /* changeable RSS */

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "RSS_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, rss_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RSS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	for (i = 0; i < nb_queues - 1; i++)
		rss_queues[i] = i + 1;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.nr_queues = nb_queues - 1;

	/* encrypt IPv4 */
	meta.encrypt = 1;
	meta.decrypt = 0;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	result = doca_flow_pipe_add_entry(0,
					  *rss_pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to RSS pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* encrypt IPv6 */
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV6;
	result = doca_flow_pipe_add_entry(0,
					  *rss_pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to RSS pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* decrypt IPv4 */
	meta.encrypt = 0;
	meta.decrypt = 1;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	result = doca_flow_pipe_add_entry(0,
					  *rss_pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to RSS pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* decrypt IPv6 */
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV6;
	result = doca_flow_pipe_add_entry(0,
					  *rss_pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to RSS pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entry: %s", doca_error_get_descr(result));
		return result;
	}
	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure) {
		DOCA_LOG_ERR("Failed to process entry");
		return DOCA_ERROR_BAD_STATE;
	}
	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

void create_hairpin_pipe_fwd(struct ipsec_security_gw_config *app_cfg,
			     int port_id,
			     bool encrypt,
			     uint16_t *rss_queues,
			     uint32_t rss_flags,
			     struct doca_flow_fwd *fwd)
{
	uint32_t nb_queues = app_cfg->dpdk_config->port_config.nb_queues;
	uint32_t i;

	memset(fwd, 0, sizeof(*fwd));

	if ((app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_NONE) ||
	    (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP && !encrypt) ||
	    (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP && encrypt)) {
		/* for software handling the packets will be sent to the application by RSS queues */
		if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
			fwd->type = DOCA_FLOW_FWD_PIPE;
			fwd->next_pipe = app_cfg->switch_pipes.rss_pipe.pipe;
		} else {
			fwd->type = DOCA_FLOW_FWD_RSS;
			fwd->rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
			if (rss_queues == NULL) {
				fwd->rss.nr_queues = -1; /* changeable RSS */
				return;
			}

			fwd->rss.nr_queues = nb_queues - 1;
			if (!encrypt && app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL)
				fwd->rss.inner_flags = rss_flags;
			else
				fwd->rss.outer_flags = rss_flags;

			fwd->rss.queues_array = rss_queues;
			for (i = 0; i < nb_queues - 1; i++)
				rss_queues[i] = i + 1;
		}
	} else {
		if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
			fwd->type = DOCA_FLOW_FWD_PIPE;
			fwd->next_pipe = app_cfg->switch_pipes.pkt_meta_pipe.pipe;
		} else if (encrypt) {
			fwd->type = DOCA_FLOW_FWD_PIPE;
			fwd->next_pipe = app_cfg->encrypt_pipes.egress_ip_classifier.pipe;
		} else {
			fwd->type = DOCA_FLOW_FWD_PORT;
			fwd->port_id = port_id ^ 1;
		}
	}
}

/*
 * Create DOCA Flow pipe that match on port meta field.
 *
 * @pipe [out]: the created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_switch_port_meta_pipe(struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};

	memset(&match, 0, sizeof(match));

	match.parser_meta.port_id = UINT16_MAX;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, doca_flow_port_switch_get(NULL));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "SWITCH_PORT_META_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create switch port meta pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add entries to port meta pipe
 * Send packets to decrypt / encrypt path based on the port
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @encrypt_root [in]: pipe to send the packets that comes from unsecured port
 * @decrypt_root [in]: pipe to send the packets that comes from secured port
 * @pipe [in]: the pipe to add entries to
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_switch_port_meta_entries(struct ipsec_security_gw_ports_map *ports[],
						 struct doca_flow_pipe *encrypt_root,
						 struct doca_flow_pipe *decrypt_root,
						 struct doca_flow_pipe *pipe,
						 struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	int num_of_entries = 2;
	doca_error_t result;

	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));
	memset(&match, 0, sizeof(match));

	app_cfg->secured_status[0].entries_in_queue = num_of_entries;

	/* forward the packets from the unsecured port to encryption */
	match.parser_meta.port_id = ports[UNSECURED_IDX]->port_id;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = encrypt_root;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to port meta pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* forward the packets from the secured port to decryption */
	match.parser_meta.port_id = ports[SECURED_IDX]->port_id;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = decrypt_root;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to port meta pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(doca_flow_port_switch_get(NULL), 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS)
		return result;
	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

/*
 * Create the switch root pipe, which match the first 2 MSB in pkt meta
 *
 * @pipe [out]: the created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_switch_pkt_meta_pipe(struct ipsec_security_gw_config *app_cfg, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	doca_error_t result;
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
	union security_gateway_pkt_meta meta = {0};

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));

	meta.decrypt = 1;
	meta.encrypt = 1;
	match_mask.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	result = doca_flow_pipe_cfg_create(&pipe_cfg, doca_flow_port_switch_get(NULL));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "PKT_META_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
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
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}
	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pkt meta pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		app_cfg->switch_pipes.pkt_meta_pipe.entries_info =
			(struct security_gateway_entry_info *)calloc(2, sizeof(struct security_gateway_entry_info));
		if (app_cfg->switch_pipes.pkt_meta_pipe.entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
			goto destroy_pipe_cfg;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add entries to pkt meta pipe
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @encrypt_pipe [in]: pipe to forward the packets for encryption if pkt meta second bit is one
 * @pipe [in]: pipe to add the entries
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_switch_pkt_meta_entries(struct ipsec_security_gw_ports_map *ports[],
						struct doca_flow_pipe *encrypt_pipe,
						struct doca_flow_pipe *pipe,
						struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	int num_of_entries = 2;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	struct doca_flow_pipe_entry **entry = NULL;
	struct security_gateway_pipe_info *pipe_info = &app_cfg->switch_pipes.pkt_meta_pipe;

	memset(&match, 0, sizeof(match));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	meta.decrypt = 0;
	meta.encrypt = 1;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = encrypt_pipe;

	if (app_cfg->debug_mode) {
		snprintf(pipe_info->entries_info[pipe_info->nb_entries].name, MAX_NAME_LEN, "encrypt");
		entry = &(pipe_info->entries_info[pipe_info->nb_entries++].entry);
	}

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to pkt meta pipe: %s", doca_error_get_descr(result));
		return result;
	}

	meta.encrypt = 0;
	meta.decrypt = 1;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = ports[UNSECURED_IDX]->port_id;

	if (app_cfg->debug_mode) {
		snprintf(pipe_info->entries_info[pipe_info->nb_entries].name, MAX_NAME_LEN, "decrypt");
		entry = &pipe_info->entries_info[pipe_info->nb_entries++].entry;
	}

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry to pkt meta pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(doca_flow_port_switch_get(NULL), 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS)
		return result;
	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

doca_error_t create_switch_ingress_root_pipes(struct ipsec_security_gw_ports_map *ports[],
					      struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_pipe *match_port_pipe;
	doca_error_t result;

	result = create_switch_port_meta_pipe(&match_port_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create port meta pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = add_switch_port_meta_entries(ports,
					      app_cfg->encrypt_pipes.encrypt_root.pipe,
					      app_cfg->decrypt_pipes.decrypt_root.pipe,
					      match_port_pipe,
					      app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add port meta pipe entries: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t create_switch_egress_root_pipes(struct ipsec_security_gw_ports_map *ports[],
					     struct ipsec_security_gw_config *app_cfg)
{
	doca_error_t result;

	snprintf(app_cfg->switch_pipes.pkt_meta_pipe.name, MAX_NAME_LEN, "egress_root");
	result = create_switch_pkt_meta_pipe(app_cfg, &app_cfg->switch_pipes.pkt_meta_pipe.pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pkt meta pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = add_switch_pkt_meta_entries(ports,
					     app_cfg->encrypt_pipes.egress_ip_classifier.pipe,
					     app_cfg->switch_pipes.pkt_meta_pipe.pipe,
					     app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pkt meta pipe entries: %s", doca_error_get_descr(result));
		return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Remove the ethernet padding from the packet
 * Ethernet padding is added to the packet to make sure the packet is at least 64 bytes long
 * This is required by the Ethernet standard
 * The padding is added after the payload and before the FCS
 *
 * @m [in]: the packet to remove the padding from
 */
void remove_ethernet_padding(struct rte_mbuf **m)
{
	struct rte_ether_hdr *oh;
	struct rte_ipv4_hdr *ipv4;
	struct rte_ipv6_hdr *ipv6;
	uint32_t payload_len, payload_len_l3, l2_l3_len;

	oh = rte_pktmbuf_mtod(*m, struct rte_ether_hdr *);

	if (RTE_ETH_IS_IPV4_HDR((*m)->packet_type)) {
		ipv4 = (void *)(oh + 1);
		l2_l3_len = rte_ipv4_hdr_len(ipv4) + sizeof(struct rte_ether_hdr);
		payload_len_l3 = rte_be_to_cpu_16(ipv4->total_length) - rte_ipv4_hdr_len(ipv4);
	} else {
		ipv6 = (void *)(oh + 1);
		l2_l3_len = sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_ether_hdr);
		payload_len_l3 = rte_be_to_cpu_16(ipv6->payload_len);
	}

	payload_len = (*m)->pkt_len - l2_l3_len;

	/* check if need to remove trailing l2 zeros - occurs when packet_len < eth_minimum_len=64 */
	if (payload_len > payload_len_l3) {
		/* need to remove the extra zeros */
		rte_pktmbuf_trim(*m, payload_len - payload_len_l3);
	}
}

/*
 * Free encrypt pipes resources
 *
 * @encrypt_pipes [in]: encrypt pipes struct
 */
static void security_gateway_free_encrypt_resources(struct encrypt_pipes *encrypt_pipes)
{
	if (encrypt_pipes->marker_insert_pipe.entries_info)
		free(encrypt_pipes->marker_insert_pipe.entries_info);
	if (encrypt_pipes->encrypt_root.entries_info)
		free(encrypt_pipes->encrypt_root.entries_info);
	if (encrypt_pipes->egress_ip_classifier.entries_info)
		free(encrypt_pipes->egress_ip_classifier.entries_info);
	if (encrypt_pipes->ipv4_encrypt_pipe.entries_info)
		free(encrypt_pipes->ipv4_encrypt_pipe.entries_info);
	if (encrypt_pipes->ipv6_encrypt_pipe.entries_info)
		free(encrypt_pipes->ipv6_encrypt_pipe.entries_info);
	if (encrypt_pipes->ipv4_tcp_pipe.entries_info)
		free(encrypt_pipes->ipv4_tcp_pipe.entries_info);
	if (encrypt_pipes->ipv4_udp_pipe.entries_info)
		free(encrypt_pipes->ipv4_udp_pipe.entries_info);
	if (encrypt_pipes->ipv6_tcp_pipe.entries_info)
		free(encrypt_pipes->ipv6_tcp_pipe.entries_info);
	if (encrypt_pipes->ipv6_udp_pipe.entries_info)
		free(encrypt_pipes->ipv6_udp_pipe.entries_info);
	if (encrypt_pipes->ipv6_src_tcp_pipe.entries_info)
		free(encrypt_pipes->ipv6_src_tcp_pipe.entries_info);
	if (encrypt_pipes->ipv6_src_udp_pipe.entries_info)
		free(encrypt_pipes->ipv6_src_udp_pipe.entries_info);
	if (encrypt_pipes->vxlan_encap_pipe.entries_info)
		free(encrypt_pipes->vxlan_encap_pipe.entries_info);
}

/*
 * Free decrypt pipes resources
 *
 * @decrypt_pipes [in]: decrypt pipes struct
 */
static void security_gateway_free_decrypt_resources(struct decrypt_pipes *decrypt_pipes)
{
	if (decrypt_pipes->marker_remove_pipe.entries_info)
		free(decrypt_pipes->marker_remove_pipe.entries_info);
	if (decrypt_pipes->decrypt_root.entries_info)
		free(decrypt_pipes->decrypt_root.entries_info);
	if (decrypt_pipes->decrypt_ipv4_pipe.entries_info)
		free(decrypt_pipes->decrypt_ipv4_pipe.entries_info);
	if (decrypt_pipes->decrypt_ipv6_pipe.entries_info)
		free(decrypt_pipes->decrypt_ipv6_pipe.entries_info);
	if (decrypt_pipes->decap_pipe.entries_info)
		free(decrypt_pipes->decap_pipe.entries_info);
	if (decrypt_pipes->vxlan_decap_ipv4_pipe.entries_info)
		free(decrypt_pipes->vxlan_decap_ipv4_pipe.entries_info);
	if (decrypt_pipes->vxlan_decap_ipv6_pipe.entries_info)
		free(decrypt_pipes->vxlan_decap_ipv6_pipe.entries_info);
}

void security_gateway_free_status_entries(struct ipsec_security_gw_config *app_cfg)
{
	free(app_cfg->secured_status);
	free(app_cfg->unsecured_status);
}

void security_gateway_free_resources(struct ipsec_security_gw_config *app_cfg)
{
	security_gateway_free_encrypt_resources(&app_cfg->encrypt_pipes);
	security_gateway_free_decrypt_resources(&app_cfg->decrypt_pipes);
}
