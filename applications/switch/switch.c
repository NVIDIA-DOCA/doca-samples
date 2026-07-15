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

#include <doca_argp.h>
#include <doca_log.h>

#include "dpdk_utils.h"
#include "flow_common.h"
#include "flow_switch_common.h"

#include "switch_core.h"

DOCA_LOG_REGISTER(SWITCH);

/*
 * Switch application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	int exit_status = EXIT_FAILURE;
	struct doca_log_backend *sdk_log;
	struct flow_switch_ctx ctx = {0};
	struct application_dpdk_config dpdk_config = {0};

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return exit_status;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return exit_status;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return exit_status;

	/* Parse cmdline/json arguments */
	result = doca_argp_init(NULL, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return exit_status;
	}

	result = register_doca_flow_switch_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application param: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&ctx.devs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init application param: %s", doca_error_get_descr(result));
		goto dpdk_destroy;
	}

	dpdk_config.port_config.nb_ports = get_dpdk_nb_ports();
	dpdk_config.port_config.switch_mode = 1;

	/* Update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update application ports and queues: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto dpdk_destroy;
	}

	/* Init switch */
	result = switch_init(&dpdk_config, &ctx);
	if (result != DOCA_SUCCESS) {
		exit_status = EXIT_FAILURE;
		goto dpdk_cleanup;
	}

	/* Initiate Flow Parser */
	result = flow_parser_init("SWITCH>> ", false);
	if (result != DOCA_SUCCESS) {
		exit_status = EXIT_FAILURE;
		goto switch_cleanup;
	}

	exit_status = EXIT_SUCCESS;

	/* Clean Flow Parser structures */
	flow_parser_cleanup();

switch_cleanup:
	/* Closing and releasing switch resources */
	switch_destroy();
dpdk_cleanup:
	/* DPDK ports cleanup */
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_destroy:
	/* DPDK cleanup + device closure */
	dpdk_fini();
	destroy_doca_flow_devs(&ctx.devs_ctx);
argp_cleanup:
	doca_argp_destroy();

	return exit_status;
}
