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
#include <string.h>

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>
#include <flow_switch_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_TRIM_MAIN);

#define FLOW_TRIM_NB_PORTS 2
#define FLOW_TRIM_NB_QUEUES 1
#define FLOW_TRIM_MAX_MBUF_SIZE (9200)
#define STR(s) #s
#define FLOW_TRIM_DEFAULT_TRIM_SIZE 128
#define FLOW_TRIM_DEFAULT_TRIM_SIZE_STR STR(FLOW_TRIM_DEFAULT_TRIM_SIZE)

doca_error_t flow_trim(int nb_queues,
		       bool is_switch_mode,
		       uint32_t trim_size,
		       struct flow_devs_manager devs_manager[],
		       uint16_t nb_devs);

struct flow_trim_config {
	struct flow_switch_ctx switch_ctx; /* Switch context, must be first member. */
	bool is_switch_mode;		   /* True if running in switch mode, false otherwise. */
	uint32_t trim_size;		   /* Number of bytes to trim from each packet. */
};

/* Pre-argp argument mode check */
static bool flow_trim_check_switch_mode(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--switch-mode") == 0) {
			return true;
		}
	}
	return false;
}

static doca_error_t switch_mode_callback(void *param, void *config)
{
	(void)param;
	struct flow_trim_config *app_config = (struct flow_trim_config *)config;

	app_config->is_switch_mode = true;

	return DOCA_SUCCESS;
}

static doca_error_t trim_size_callback(void *param, void *config)
{
	struct flow_trim_config *app_config = (struct flow_trim_config *)config;
	int trim_size = *(int *)param;

	if (trim_size < 0) {
		DOCA_LOG_ERR("Trim size must be non-negative, got %d", trim_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->trim_size = (uint32_t)trim_size;

	return DOCA_SUCCESS;
}

static doca_error_t register_trim_params(void)
{
	struct doca_argp_param *mode_param;
	struct doca_argp_param *trim_size_param;
	doca_error_t result;

	result = register_doca_flow_switch_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mode ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(mode_param, "switch-mode");
	doca_argp_param_set_description(mode_param, "Run in switch mode (default: vnf mode)");
	doca_argp_param_set_callback(mode_param, switch_mode_callback);
	doca_argp_param_set_type(mode_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register mode param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&trim_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create trim-size ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(trim_size_param, "trim-size");
	doca_argp_param_set_description(
		trim_size_param,
		"Target packet size after trimming in bytes (default: " FLOW_TRIM_DEFAULT_TRIM_SIZE_STR ")");
	doca_argp_param_set_callback(trim_size_param, trim_size_callback);
	doca_argp_param_set_type(trim_size_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(trim_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register trim-size param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct flow_trim_config app_config = {.trim_size = FLOW_TRIM_DEFAULT_TRIM_SIZE};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = FLOW_TRIM_NB_PORTS,
		.port_config.nb_queues = FLOW_TRIM_NB_QUEUES,
		.port_config.mbuf_size = FLOW_TRIM_MAX_MBUF_SIZE,
	};

	if (flow_trim_check_switch_mode(argc, argv)) {
		app_config.is_switch_mode = true;
		dpdk_config.port_config.switch_mode = 1;
		app_config.switch_ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;
	}

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create standard log backend: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create SDK log backend: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SDK log level: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init(NULL, &app_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_trim_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&app_config.switch_ctx.devs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	result = flow_trim(dpdk_config.port_config.nb_queues,
			   app_config.is_switch_mode,
			   app_config.trim_size,
			   app_config.switch_ctx.devs_ctx.devs_manager,
			   app_config.switch_ctx.devs_ctx.nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_trim() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	if (app_config.is_switch_mode)
		dpdk_fini();
	else
		dpdk_fini_with_devs(dpdk_config.port_config.nb_ports);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (app_config.is_switch_mode)
		destroy_doca_flow_devs(&app_config.switch_ctx.devs_ctx);
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
