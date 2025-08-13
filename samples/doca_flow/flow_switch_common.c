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

#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_dpdk.h>

#include <dpdk_utils.h>

#include "flow_common.h"
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(flow_switch_common);

/*
 * Get DOCA Flow switch mode
 *
 * @param [in]: input parameter
 * @config [out]: configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t param_flow_switch_exp_callback(void *param, void *config)
{
	struct flow_switch_ctx *ctx = (struct flow_switch_ctx *)config;

	ctx->is_expert = *(bool *)param;

	return DOCA_SUCCESS;
}

doca_error_t register_doca_flow_switch_params(void)
{
	doca_error_t result;
	struct doca_argp_param *exp_param;

	result = register_flow_device_params(NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow ARGP device params: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&exp_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow switch ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(exp_param, "exp");
	doca_argp_param_set_long_name(exp_param, "expert-mode");
	doca_argp_param_set_description(exp_param, "set expert mode");
	doca_argp_param_set_callback(exp_param, param_flow_switch_exp_callback);
	doca_argp_param_set_type(exp_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(exp_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow switch ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t init_doca_flow_switch_ports(struct flow_devs_manager devs_manager[],
					 int nb_managers,
					 struct doca_flow_port *ports[],
					 int nb_ports,
					 uint32_t actions_mem_size[])
{
	struct doca_dev *dev_arr[FLOW_COMMON_DEV_MAX];
	struct doca_dev_rep *dev_rep_arr[FLOW_COMMON_REPS_MAX];
	int nb_devs = 0;

	if (nb_managers <= 0) {
		DOCA_LOG_ERR("Failed to init doca flow switch ports: Invalid number of switch managers: %d",
			     nb_managers);
		return DOCA_ERROR_INVALID_VALUE;
	}

	memset(dev_arr, 0, sizeof(struct doca_dev *) * FLOW_COMMON_DEV_MAX);
	memset(dev_rep_arr, 0, sizeof(struct doca_dev_rep *) * FLOW_COMMON_REPS_MAX);
	for (int i = 0; i < nb_managers; i++) {
		dev_arr[nb_devs++] = devs_manager[i].doca_dev;
		for (int j = 0; j < devs_manager[i].nb_reps; j++)
			dev_rep_arr[nb_devs++] = devs_manager[i].doca_dev_rep[j];
	}

	if (nb_devs < nb_ports) {
		DOCA_LOG_ERR("Insufficient DOCA devices: required %d for %d ports, but only %d provided",
			     nb_ports,
			     nb_ports,
			     nb_devs);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return init_doca_flow_ports_with_op_state(nb_ports,
						  ports,
						  false /* is_port_fwd */,
						  dev_arr,
						  dev_rep_arr,
						  NULL,
						  actions_mem_size);
}

uint8_t get_dpdk_nb_ports(void)
{
	uint8_t nb_ports = 0;
	uint16_t port_id;

	for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
		if (!rte_eth_dev_is_valid_port(port_id))
			continue;

		nb_ports++;
		DOCA_LOG_INFO("Port ID %u is valid DPDK port", port_id);
	}

	return nb_ports;
}
