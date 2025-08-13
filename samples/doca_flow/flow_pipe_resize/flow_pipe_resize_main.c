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

#include <flow_common.h>
#include <flow_switch_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_PIPE_RESIZE::MAIN);

/*
 * Config for pipe resize params
 */
struct flow_resize_ctx {
	struct flow_switch_ctx ctx; /* switch context */
	bool is_basic_pipe;	    /* determine pipe type under resize */
};

/* Sample's Logic */
doca_error_t flow_pipe_resize(uint16_t nb_queues, struct flow_switch_ctx *ctx, bool is_basic_pipe);

/*
 * Callback for arg pipe-type
 *
 * @param [in]: parameter
 * @config [in]: app configuration
 * @return: DOCA_SUCCESS on success and negative number otherwise
 */
static doca_error_t pipe_type_callback(void *param, void *config)
{
	struct flow_resize_ctx *app_config = (struct flow_resize_ctx *)config;
	const char *str = (const char *)param;

	if (strcmp(str, "basic") == 0)
		app_config->is_basic_pipe = true;
	else if (strcmp(str, "control") == 0)
		app_config->is_basic_pipe = false;
	else {
		DOCA_LOG_ERR("Unsupported resize pipe_type '%s' was specified", str);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Register for which pipe type (BASIC/CONTROL) to resize.
 *
 * @return: DOCA_SUCCESS on success, negative number otherwise.
 *
 */
static int register_pipe_type_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pipe_type_param;

	/* Create and register pipe-type para */
	result = doca_argp_param_create(&pipe_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe-type ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(pipe_type_param, "ptype");
	doca_argp_param_set_long_name(pipe_type_param, "pipe-type");
	doca_argp_param_set_arguments(pipe_type_param, "<pipe-type>");
	doca_argp_param_set_description(pipe_type_param, "Set pipe-type (\"basic\", \"control\") to resize");
	doca_argp_param_set_callback(pipe_type_param, pipe_type_callback);
	doca_argp_param_set_type(pipe_type_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(pipe_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return 0;
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
		.port_config.nb_ports = 3,
		.port_config.switch_mode = 1,
	};
	struct flow_resize_ctx resize_ctx = {0};

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

	result = doca_argp_init(NULL, &resize_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_doca_flow_switch_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow param: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = register_pipe_type_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	resize_ctx.ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&resize_ctx.ctx.devs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow switch common: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues");
		goto dpdk_cleanup;
	}

	/* run sample */
	result = flow_pipe_resize(dpdk_config.port_config.nb_queues, &resize_ctx.ctx, resize_ctx.is_basic_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_pipe_resize() encountered an error: %s", doca_error_get_descr(result));
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
	destroy_doca_flow_devs(&resize_ctx.ctx.devs_ctx);
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
