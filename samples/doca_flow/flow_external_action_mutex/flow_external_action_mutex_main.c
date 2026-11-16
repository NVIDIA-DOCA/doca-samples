/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <flow_common.h>

#include <dpdk_utils.h>

#include "flow_external_action_mutex_sample.h"

DOCA_LOG_REGISTER(FLOW_EXTERNAL_ACTION_MUTEX::MAIN);

/*
 * Converter used by register_flow_device_params() to recover the
 * flow_dev_ctx embedded in our argp user-context.
 */
static struct flow_dev_ctx *flow_dev_ctx_from_user_ctx(void *user_ctx)
{
	return &((struct flow_external_action_mutex_cfg *)user_ctx)->flow_devs;
}

/*
 * Callback for --no-mutex: clear the default-true use_mutex bit.
 */
static doca_error_t no_mutex_callback(void *param, void *config)
{
	struct flow_external_action_mutex_cfg *cfg = (struct flow_external_action_mutex_cfg *)config;

	(void)param;
	cfg->use_mutex = false;
	return DOCA_SUCCESS;
}

/*
 * Callback for --duration <seconds> flag.
 */
static doca_error_t duration_callback(void *param, void *config)
{
	struct flow_external_action_mutex_cfg *cfg = (struct flow_external_action_mutex_cfg *)config;
	int value = *(int *)param;

	if (value <= 0) {
		DOCA_LOG_ERR("--duration must be a positive number of seconds");
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->duration_sec = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t register_sample_params(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create argp param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "no-mutex");
	doca_argp_param_set_description(param,
					"Disable the LOCK/UNLOCK mutex around the ASO memory RMW (default: enabled)");
	doca_argp_param_set_callback(param, no_mutex_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register no-mutex param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create argp param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "duration");
	doca_argp_param_set_description(param, "RX collection duration in seconds (default: 10)");
	doca_argp_param_set_callback(param, duration_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register duration param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct flow_external_action_mutex_cfg sample_cfg = {
		.use_mutex = true,
		.duration_sec = 10,
	};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 2,
		.port_config.nb_queues = 2,
		.port_config.enable_mbuf_metadata = 1,
		.port_config.rss_support = 1,
	};

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init(NULL, &sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_flow_device_params(flow_dev_ctx_from_user_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = register_sample_params();
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	/* Register tune profile parameters */
	result = register_flow_tune_profile_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register tune parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&sample_cfg.flow_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	result = flow_external_action_mutex(dpdk_config.port_config.nb_queues, &sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_external_action_mutex() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini_with_devs(dpdk_config.port_config.nb_ports);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
