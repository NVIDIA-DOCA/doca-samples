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
#include <doca_dpdk.h>
#include <doca_argp.h>

#include <rte_eal.h>

#include "flow_common.h"

DOCA_LOG_REGISTER(flow_common);

/* Hold the converter function for the flow_param_dev_callback (if exists) */
static flow_dev_ctx_from_user_ctx_t global_user_ctx_converter = NULL;

static doca_error_t set_system_level_resources(struct doca_flow_cfg *cfg,
					       struct flow_resources *resource,
					       uint32_t nr_shared_resources[])
{
	doca_error_t result;

	if (resource == NULL)
		return DOCA_SUCCESS;

	result = doca_flow_cfg_set_nr_counters(cfg, resource->nr_counters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_cfg_set_nr_meters(cfg, resource->nr_meters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_meters: %s", doca_error_get_descr(result));
		return result;
	}

	for (int i = 0; i < SHARED_RESOURCE_NUM_VALUES; i++) {
		result = doca_flow_cfg_set_nr_shared_resource(cfg, nr_shared_resources[i], i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_shared_resources: %s",
				     doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

static doca_error_t set_port_level_resources(struct doca_flow_port_cfg *cfg, struct flow_resources *resource)
{
	doca_error_t result;

	if (resource == NULL)
		return DOCA_SUCCESS;

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_COUNTER, resource->nr_counters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level counter resource to %d: %s",
			     resource->nr_counters,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_METER, resource->nr_meters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level meter resource to %d: %s",
			     resource->nr_meters,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_RSS, resource->nr_rss);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level rss resource to %d: %s",
			     resource->nr_rss,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_PSP, resource->nr_psp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level psp resource to %d: %s",
			     resource->nr_psp,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_ENCAP, resource->nr_encap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level encap resource to %d: %s",
			     resource->nr_encap,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_DECAP, resource->nr_decap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level decap resource to %d: %s",
			     resource->nr_decap,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_IPSEC_SA, resource->nr_ipsec_sa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port level ipsec_sa resource to %d: %s",
			     resource->nr_ipsec_sa,
			     doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/* Global statistics interval variable */
static int g_flow_stats_interval = 0;

/*
 * Entry processing callback
 *
 * @entry [in]: DOCA Flow entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
void check_for_valid_entry(struct doca_flow_pipe_entry *entry,
			   uint16_t pipe_queue,
			   enum doca_flow_entry_status status,
			   enum doca_flow_entry_op op,
			   void *user_ctx)
{
	(void)entry;
	(void)op;
	(void)pipe_queue;
	struct entries_status *entry_status = (struct entries_status *)user_ctx;

	if (entry_status == NULL)
		return;
	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->failure = true; /* set failure to true if processing failed */
	entry_status->nb_processed++;
}

doca_error_t init_doca_flow(int nb_queues,
			    const char *mode,
			    struct flow_resources *resource,
			    uint32_t nr_shared_resources[])
{
	return init_doca_flow_cb(nb_queues, mode, resource, nr_shared_resources, check_for_valid_entry, NULL, NULL);
}

doca_error_t init_doca_flow_with_defs(int nb_queues,
				      const char *mode,
				      struct flow_resources *resource,
				      uint32_t nr_shared_resources[],
				      struct doca_flow_definitions *defs)
{
	return init_doca_flow_cb(nb_queues, mode, resource, nr_shared_resources, check_for_valid_entry, NULL, defs);
}

doca_error_t init_doca_flow_cb(int nb_queues,
			       const char *mode,
			       struct flow_resources *resource,
			       uint32_t nr_shared_resources[],
			       doca_flow_entry_process_cb cb,
			       doca_flow_pipe_process_cb pipe_process_cb,
			       struct doca_flow_definitions *defs)
{
	struct doca_flow_cfg *flow_cfg;
	doca_error_t result, tmp_result;

	result = doca_flow_cfg_create(&flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		return result;
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

	result = doca_flow_cfg_set_cb_entry_process(flow_cfg, cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg doca_flow_entry_process_cb: %s",
			     doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_cb_pipe_process(flow_cfg, pipe_process_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_process_cb: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	if (resource && resource->mode == DOCA_FLOW_RESOURCE_MODE_PORT) {
		result = doca_flow_cfg_set_resource_mode(flow_cfg, resource->mode);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_cfg resource_mode: %s", doca_error_get_descr(result));
			goto destroy_cfg;
		}
	} else {
		result = set_system_level_resources(flow_cfg, resource, nr_shared_resources);
		if (result != DOCA_SUCCESS) {
			goto destroy_cfg;
		}
	}

	if (defs) {
		result = doca_flow_cfg_set_definitions(flow_cfg, defs);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_cfg defs: %s", doca_error_get_descr(result));
			goto destroy_cfg;
		}
	}

	/* creating doca flow with defs */
	result = doca_flow_init(flow_cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to initialize DOCA Flow: %s", doca_error_get_descr(result));
destroy_cfg:
	tmp_result = doca_flow_cfg_destroy(flow_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow_cfg: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*
 * Create DOCA Flow port cfg and fill the fields common between master and rpresentors.
 *
 * @port_id [in]: port ID
 * @actions_mem_size [in]: action memory size
 * @cfg [out]: the port configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_flow_port_cfg(int port_id,
					      uint32_t actions_mem_size,
					      struct flow_resources *resources,
					      struct doca_flow_port_cfg **cfg)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result;

	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port_id: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	result = doca_flow_port_cfg_set_actions_mem_size(port_cfg, actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set actions memory size: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	if (resources && resources->mode == DOCA_FLOW_RESOURCE_MODE_PORT) {
		result = set_port_level_resources(port_cfg, resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set port levle resources: %s", doca_error_get_descr(result));
			doca_flow_port_cfg_destroy(port_cfg);
			return result;
		}
	}

	*cfg = port_cfg;
	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow port by port id
 *
 * @port_id [in]: port ID
 * @actions_mem_size [in]: action memory size
 * @dev [in]: doca device to attach
 * @state [in]: port operation initial state
 * @port [out]: port handler on success
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_flow_port(int port_id,
					  uint32_t actions_mem_size,
					  struct doca_dev *dev,
					  port_config_cb port_config_cb,
					  void *config_cb_ctx,
					  struct flow_resources *resources,
					  struct doca_flow_port **port)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result, tmp_result;

	result = create_doca_flow_port_cfg(port_id, actions_mem_size, resources, &port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg dev: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	if (port_config_cb) {
		result = port_config_cb(port_cfg, port_id, config_cb_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_port_cfg operation state: %s",
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

/*
 * Create DOCA Flow port representor by port id
 *
 * @port_id [in]: port ID
 * @actions_mem_size [in]: action memory size
 * @dev_rep [in]: doca reprtesentor device to attach
 * @port [out]: port handler on success
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_flow_port_representor(int port_id,
						      uint32_t actions_mem_size,
						      struct doca_dev_rep *dev_rep,
						      port_config_cb port_rep_config_cb,
						      void *config_cb_ctx,
						      struct flow_resources *resources,
						      struct doca_flow_port **port)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result, tmp_result;

	result = create_doca_flow_port_cfg(port_id, actions_mem_size, resources, &port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_dev_rep(port_cfg, dev_rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg dev: %s", doca_error_get_descr(result));
		goto destroy_port_cfg;
	}

	if (port_rep_config_cb) {
		result = port_rep_config_cb(port_cfg, port_id, config_cb_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_port_cfg operation state: %s",
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

doca_error_t stop_doca_flow_ports(int nb_ports, struct doca_flow_port *ports[])
{
	int portid;
	doca_error_t ret, doca_error = DOCA_SUCCESS;

	for (portid = nb_ports - 1; portid >= 0; portid--)
		doca_flow_port_pipes_flush(ports[portid]);

	/*
	 * Stop the ports in reverse order, since in switch mode port 0
	 * is proxy port, and proxy port should stop as last.
	 */
	for (portid = nb_ports - 1; portid >= 0; portid--) {
		if (ports[portid] != NULL) {
			ret = doca_flow_port_stop(ports[portid]);
			/* record first error */
			if (ret != DOCA_SUCCESS && doca_error == DOCA_SUCCESS)
				doca_error = ret;
		}
	}
	return doca_error;
}

doca_error_t init_doca_flow_ports_with_custom_config(int nb_ports,
						     struct doca_flow_port *ports[],
						     bool is_port_fwd,
						     struct doca_dev *dev_arr[],
						     struct doca_dev_rep *dev_rep_arr[],
						     port_config_cb port_cb,
						     port_config_cb port_rep_cb,
						     void *config_cb_ctx,
						     uint32_t actions_mem_size[],
						     struct flow_resources *resources)
{
	int portid;
	doca_error_t result;

	for (portid = 0; portid < nb_ports; portid++) {
		if (dev_rep_arr && dev_rep_arr[portid]) {
			/* Create doca flow port */
			result = create_doca_flow_port_representor(portid,
								   actions_mem_size[portid],
								   dev_rep_arr[portid],
								   port_rep_cb,
								   config_cb_ctx,
								   resources,
								   &ports[portid]);
		} else {
			/* Create doca flow port */
			result = create_doca_flow_port(portid,
						       actions_mem_size[portid],
						       dev_arr[portid],
						       port_cb,
						       config_cb_ctx,
						       resources,
						       &ports[portid]);
		}
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start port: %s", doca_error_get_descr(result));
			if (portid != 0)
				stop_doca_flow_ports(portid, ports);
			return result;
		}
	}

	/* Pair ports should be done in the following order: port0 with port1, port2 with port3 etc */
	if (!is_port_fwd)
		return DOCA_SUCCESS;

	for (portid = 0; portid < nb_ports; portid++) {
		if (!portid || !(portid % 2))
			continue;
		/* pair odd port with previous port */
		result = doca_flow_port_pair(ports[portid], ports[portid ^ 1]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to pair ports %u - %u", portid, portid ^ 1);
			stop_doca_flow_ports(portid + 1, ports);
			return result;
		}
		result = doca_flow_port_pair(ports[portid ^ 1], ports[portid]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to pair ports %u - %u", portid, portid ^ 1);
			stop_doca_flow_ports(portid + 1, ports);
			return result;
		}
	}
	return DOCA_SUCCESS;
}

doca_error_t init_doca_flow_ports(int nb_ports,
				  struct doca_flow_port *ports[],
				  bool is_port_fwd,
				  struct doca_dev *dev_arr[],
				  uint32_t actions_mem_size[],
				  struct flow_resources *resources)
{
	return init_doca_flow_ports_with_custom_config(nb_ports,
						       ports,
						       is_port_fwd,
						       dev_arr,
						       NULL /* dev_rep_arr */,
						       NULL /* port_cb */,
						       NULL /* port_rep_cb */,
						       NULL /* config_cb_ctx */,
						       actions_mem_size,
						       resources);
}

doca_error_t init_doca_flow_vnf_ports(int nb_ports,
				      struct doca_flow_port *ports[],
				      uint32_t actions_mem_size[],
				      struct flow_resources *resources)
{
	struct doca_dev *dev_arr[nb_ports];
	doca_error_t result;
	int i;

	memset(dev_arr, 0, sizeof(struct doca_dev *) * nb_ports);
	for (i = 0; i < nb_ports; i++) {
		result = doca_dpdk_port_as_dev(i, &dev_arr[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get device for port %d: %s", i, doca_error_get_descr(result));
			return result;
		}
	}

	return init_doca_flow_ports(nb_ports, ports, true, dev_arr, actions_mem_size, resources);
}

doca_error_t set_flow_pipe_cfg(struct doca_flow_pipe_cfg *cfg,
			       const char *name,
			       enum doca_flow_pipe_type type,
			       bool is_root)
{
	doca_error_t result;

	if (cfg == NULL) {
		DOCA_LOG_ERR("Failed to set DOCA Flow pipe configurations, cfg=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_flow_pipe_cfg_set_name(cfg, name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_type(cfg, type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_is_root(cfg, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t flow_process_entries(struct doca_flow_port *port, struct entries_status *status, uint32_t nr_entries)
{
	doca_error_t result;

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, nr_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process %u entries: %s", nr_entries, doca_error_get_descr(result));
		return result;
	}

	if (status->nb_processed != (int)nr_entries) {
		DOCA_LOG_ERR("Failed to process %u entries, nb_processed=%d", nr_entries, status->nb_processed);
		return DOCA_ERROR_BAD_STATE;
	}

	if (status->failure) {
		DOCA_LOG_ERR("Failed to process %u entries, status is failure", nr_entries);
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

doca_error_t flow_param_dev_callback(void *param, void *config)
{
	struct flow_dev_ctx *ctx = (struct flow_dev_ctx *)config;
	struct doca_argp_device_ctx *dev_ctx = (struct doca_argp_device_ctx *)param;
	doca_error_t result;

	if (FLOW_COMMON_PORTS_MAX <= ctx->nb_ports) {
		DOCA_LOG_ERR("Encountered too many ports, maximal number of ports is: %d", FLOW_COMMON_PORTS_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (ctx->nb_devs == FLOW_COMMON_DEV_MAX) {
		DOCA_LOG_ERR("Encountered too many devices, maximal number of devices is: %d", FLOW_COMMON_DEV_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Possible that the same device was passed twice, warn about such cases */
	for (int i = 0; i < ctx->nb_devs; i++) {
		if (ctx->devs_manager[i].doca_dev == dev_ctx->dev) {
			DOCA_LOG_WARN("Device %p already exists", dev_ctx->dev);
			result = doca_dev_close(dev_ctx->dev);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to close doca dev: %p: %s",
					     dev_ctx->dev,
					     doca_error_get_descr(result));
				return result;
			}
			/* Nothing to be done */
			return DOCA_SUCCESS;
		}
	}

	ctx->devs_manager[ctx->nb_devs].doca_dev = dev_ctx->dev;
	ctx->devs_manager[ctx->nb_devs].device_opened_without_reps = true;
	if (dev_ctx->devargs != NULL)
		ctx->devs_manager[ctx->nb_devs].dev_arg = dev_ctx->devargs;
	else
		ctx->devs_manager[ctx->nb_devs].dev_arg = ctx->default_dev_args;

	ctx->nb_devs++;
	ctx->nb_ports++;

	return DOCA_SUCCESS;
}

/*
 * Wrapper for the flow_param_dev_callback function
 *
 * @param [in]: input parameter
 * @config [out]: configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t wrapped_flow_param_dev_callback(void *param, void *config)
{
	return flow_param_dev_callback(param, global_user_ctx_converter(config));
}

doca_error_t flow_param_rep_callback(void *param, void *config)
{
	struct flow_dev_ctx *ctx = (struct flow_dev_ctx *)config;
	struct doca_argp_device_rep_ctx *rep_ctx = (struct doca_argp_device_rep_ctx *)param;
	uint32_t dev_idx = 0;
	bool found_dev = false;

	if (FLOW_COMMON_PORTS_MAX <= ctx->nb_ports) {
		DOCA_LOG_ERR("Encountered too many ports, maximal number of ports is: %d", FLOW_COMMON_PORTS_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Store the information under the correct device index */
	for (int i = 0; i < ctx->nb_devs && !found_dev; i++) {
		if (ctx->devs_manager[i].doca_dev == rep_ctx->dev_ctx.dev) {
			found_dev = true;
			dev_idx = i;
			break;
		}
	}

	/* If this is the first representor for this device, we need to count the device */
	if (!found_dev) {
		if (ctx->nb_devs == FLOW_COMMON_DEV_MAX) {
			DOCA_LOG_ERR("Encountered too many devices, maximal number of devices is: %d",
				     FLOW_COMMON_DEV_MAX);
			return DOCA_ERROR_INVALID_VALUE;
		}
		dev_idx = ctx->nb_devs++;
		ctx->devs_manager[dev_idx].doca_dev = rep_ctx->dev_ctx.dev;
		if (rep_ctx->dev_ctx.devargs != NULL)
			ctx->devs_manager[dev_idx].dev_arg = rep_ctx->dev_ctx.devargs;
		else
			ctx->devs_manager[dev_idx].dev_arg = ctx->default_dev_args;
	} else {
		if (ctx->devs_manager[dev_idx].nb_reps == FLOW_COMMON_REPS_MAX) {
			DOCA_LOG_ERR("Encountered too many representors for device %p, maximal number of reps is: %d",
				     ctx->devs_manager[dev_idx].doca_dev,
				     FLOW_COMMON_REPS_MAX);
			return DOCA_ERROR_INVALID_VALUE;
		}

		/* We can't catch passing the same representor twice, as they will look like different representors */
	}

	ctx->devs_manager[dev_idx].doca_dev_rep[ctx->devs_manager[dev_idx].nb_reps++] = rep_ctx->dev_rep;
	ctx->nb_ports++;

	return DOCA_SUCCESS;
}

/*
 * Wrapper for the flow_param_rep_callback function
 *
 * @param [in]: input parameter
 * @config [out]: configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t wrapped_flow_param_rep_callback(void *param, void *config)
{
	return flow_param_rep_callback(param, global_user_ctx_converter(config));
}

doca_error_t register_flow_switch_device_params(flow_dev_ctx_from_user_ctx_t converter)
{
	doca_error_t result;
	struct doca_argp_param *rep_param;

	/* Start by setting the global user context converter */
	global_user_ctx_converter = converter;

	/* Create the representor parameter */
	result = doca_argp_param_create(&rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rep_param, "r");
	doca_argp_param_set_long_name(rep_param, "rep");
	doca_argp_param_set_description(rep_param, "device representor");
	doca_argp_param_set_callback(rep_param,
				     converter == NULL ? flow_param_rep_callback : wrapped_flow_param_rep_callback);
	doca_argp_param_set_type(rep_param, DOCA_ARGP_TYPE_DEVICE_REP);
	doca_argp_param_set_multiplicity(rep_param);
	result = doca_argp_register_param(rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t register_flow_device_params(flow_dev_ctx_from_user_ctx_t converter)
{
	doca_error_t result;
	struct doca_argp_param *dev_param;

	/* Start by setting the global user context converter */
	global_user_ctx_converter = converter;

	/* Create the device parameter */
	result = doca_argp_param_create(&dev_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dev_param, "a");
	doca_argp_param_set_long_name(dev_param, "device");
	doca_argp_param_set_description(dev_param, "device");
	doca_argp_param_set_callback(dev_param,
				     converter == NULL ? flow_param_dev_callback : wrapped_flow_param_dev_callback);
	doca_argp_param_set_type(dev_param, DOCA_ARGP_TYPE_DEVICE);
	doca_argp_param_set_multiplicity(dev_param);
	result = doca_argp_register_param(dev_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return register_flow_switch_device_params(converter);
}

doca_error_t flow_init_dpdk(int argc, char **dpdk_argv)
{
	char *argv[argc + 2];
	doca_error_t result;

	memcpy(argv, dpdk_argv, sizeof(argv[0]) * argc);
	argv[argc++] = "-a";
	argv[argc++] = "pci:00:00.0";

	result = rte_eal_init(argc, argv);
	if (result < 0) {
		DOCA_LOG_ERR("EAL initialization failed");
		return DOCA_ERROR_DRIVER;
	}
	return DOCA_SUCCESS;
}

doca_error_t init_doca_flow_devs(struct flow_dev_ctx *ctx)
{
	doca_error_t result;
	int i;

	for (i = 0; i < ctx->nb_devs; i++) {
		/* If we have a required capability callback, we need to check if the device supports it */
		if (ctx->port_cap != NULL) {
			result = ctx->port_cap(doca_dev_as_devinfo(ctx->devs_manager[i].doca_dev));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Device %p does not support the required capability: %s",
					     ctx->devs_manager[i].doca_dev,
					     doca_error_get_descr(result));
				goto quit;
			}
		}

		/* We need a different probing based on the existence of representors */
		if (ctx->devs_manager[i].nb_reps > 0) {
			result = doca_dpdk_port_probe_with_representors(ctx->devs_manager[i].doca_dev,
									ctx->devs_manager[i].dev_arg,
									ctx->devs_manager[i].doca_dev_rep,
									ctx->devs_manager[i].nb_reps);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to probe DOCA device and representors: %s",
					     doca_error_get_descr(result));
				goto quit;
			}
			continue;
		} else if (ctx->devs_manager[i].dev_arg != NULL) {
			result = doca_dpdk_port_probe(ctx->devs_manager[i].doca_dev, ctx->devs_manager[i].dev_arg);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to probe DOCA device: %s", doca_error_get_descr(result));
				goto quit;
			}
			/* Fallthrough*/
		} else {
			result = doca_dpdk_port_probe(ctx->devs_manager[i].doca_dev, "");
			/* Fallthrough*/
		}

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to probe DOCA device: %s", doca_error_get_descr(result));
			goto quit;
		}
	}

quit:
	if (result != DOCA_SUCCESS)
		destroy_doca_flow_devs(ctx);
	return result;
}

doca_error_t close_doca_dev_reps(struct doca_dev_rep *dev_reps[], uint16_t nb_reps)
{
	doca_error_t result, retval = DOCA_SUCCESS;

	for (int i = 0; i < nb_reps; i++) {
		result = doca_dev_rep_close(dev_reps[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca dev rep: %p: %s", dev_reps[i], doca_error_get_descr(result));
			DOCA_ERROR_PROPAGATE(retval, result);
		}
	}

	return retval;
}

void destroy_doca_flow_devs(struct flow_dev_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->nb_devs; i++) {
		if (ctx->devs_manager[i].doca_dev) {
			close_doca_dev_reps(ctx->devs_manager[i].doca_dev_rep, ctx->devs_manager[i].nb_reps);
			ctx->devs_manager[i].nb_reps = 0;
			if (ctx->devs_manager[i].device_opened_without_reps)
				doca_dev_close(ctx->devs_manager[i].doca_dev);
			ctx->devs_manager[i].doca_dev = NULL;
		}
	}
}

/*
 * Callback function for stats interval parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context (unused)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t flow_stats_interval_callback(void *param, void *config)
{
	(void)config;
	int *value = (int *)param;

	if (*value < 0) {
		DOCA_LOG_ERR("Stats interval must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	g_flow_stats_interval = *value;
	return DOCA_SUCCESS;
}

doca_error_t register_flow_stats_params(void)
{
	doca_error_t result;
	struct doca_argp_param *stats_interval_param;

	result = doca_argp_param_create(&stats_interval_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(stats_interval_param, "i");
	doca_argp_param_set_long_name(stats_interval_param, "stats-interval");
	doca_argp_param_set_description(stats_interval_param,
					"Statistics printing interval in microseconds (0 = print only at end)");
	doca_argp_param_set_callback(stats_interval_param, flow_stats_interval_callback);
	doca_argp_param_set_type(stats_interval_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(stats_interval_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register stats_interval argument: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

int get_flow_stats_interval(void)
{
	return g_flow_stats_interval;
}

void flow_wait_for_packets(int total_wait_sec, void (*stats_print_func)(void *), void *stats_context)
{
	DOCA_LOG_INFO("Wait %d seconds for packets to arrive", total_wait_sec);

	if (g_flow_stats_interval > 0 && stats_print_func != NULL) {
		uint64_t waited = 0;
		const uint64_t total_wait = total_wait_sec * 1000000ULL; /* Convert seconds to microseconds */
		while (waited < total_wait) {
			usleep(g_flow_stats_interval);
			waited += g_flow_stats_interval;
			stats_print_func(stats_context);
		}
	} else {
		sleep(total_wait_sec);
	}

	/* Print final stats if function is provided */
	if (stats_print_func != NULL) {
		stats_print_func(stats_context);
	}
}
