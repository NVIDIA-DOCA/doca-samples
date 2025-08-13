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

#include <stdlib.h>

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_dpdk.h>

#include <flow_common.h>
#include <flow_switch_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_SWITCH_HOT_UPGRADE::MAIN);

/* Sample's Logic */
doca_error_t flow_switch_hot_upgrade(int nb_queues,
				     int nb_ports,
				     struct flow_devs_manager devs_manager[],
				     uint16_t nb_devs,
				     enum doca_flow_port_operation_state state);

/* doca flow hot upgrade context */
struct flow_hot_upgrade_ctx {
	struct flow_switch_ctx switch_ctx;	   /* common switch context */
	enum doca_flow_port_operation_state state; /* operation state to use after port configuration */
};

/*
 * Get DOCA Flow Hot Upgrade directory path.
 *
 * @param [in]: input parameter
 * @config [out]: configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t param_flow_hot_upgrade_operation_state_callback(void *param, void *config)
{
	struct flow_hot_upgrade_ctx *ctx = (struct flow_hot_upgrade_ctx *)config;
	int state = *(int *)param;

	ctx->state = state;
	DOCA_LOG_DBG("Operation state %d is configured for current instance", state);

	return DOCA_SUCCESS;
}

/*
 * Register DOCA Flow extra parameters.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t register_extra_params(void)
{
	doca_error_t result;
	struct doca_argp_param *state_param;

	/* Register common switch sample parameters */
	result = register_doca_flow_switch_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow switch parameters: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register extra parameter for this sample */
	result = doca_argp_param_create(&state_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow hot upgrade operation state ARGP param: %s",
			     doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(state_param, "s");
	doca_argp_param_set_long_name(state_param, "state");
	doca_argp_param_set_description(
		state_param,
		"Set the (numeric) operation state for the ports <0=ACTIVE, 1=ACTIVE_READY_TO_SWAP, 2=STANDBY, 3=UNCONNECTED>");
	doca_argp_param_set_callback(state_param, param_flow_hot_upgrade_operation_state_callback);
	doca_argp_param_set_type(state_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(state_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow hot upgrade operation state ARGP param: %s",
			     doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 6,
		.port_config.nb_queues = 1,
		.port_config.switch_mode = 1,
	};
	struct flow_hot_upgrade_ctx ctx = {0};

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init(NULL, &ctx.switch_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_extra_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register extra parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	ctx.switch_ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&ctx.switch_ctx.devs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow switch common: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	/* run sample */
	result = flow_switch_hot_upgrade(dpdk_config.port_config.nb_queues,
					 dpdk_config.port_config.nb_ports,
					 ctx.switch_ctx.devs_ctx.devs_manager,
					 ctx.switch_ctx.devs_ctx.nb_devs,
					 ctx.state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_switch_hot_upgrade() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini();
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	destroy_doca_flow_devs(&ctx.switch_ctx.devs_ctx);
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
