/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <stdlib.h>

#include <doca_argp.h>
#include <doca_log.h>

#include "time_sync_dpu_core.h"

DOCA_LOG_REGISTER(TIME_SYNC::TIME_SYNC_DPU);

/*
 * Time sync dpu application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;
	struct time_sync_cfg ts_cfg = {0};
	int exit_status = EXIT_FAILURE;
	doca_error_t result;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Initialise arg parser to populate time_sync_conf fields */
	result = doca_argp_init(NULL, &ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialise doca arg parser: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	/* Register the common app parameters */
	result = time_sync_common_reg_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register arg parser parameters: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	/* Start the arg parser */
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start arg parser: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	/* Open devices for use in DOCA contexts */
	result = time_sync_dpu_open_devs(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open dpu doca devs: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	/* Create a clock context for cross timestamping */
	result = time_sync_common_create_clock(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create clock: %s", doca_error_get_descr(result));
		goto close_devs;
	}

	/* Set up context for loading and interfacing with DPA app */
	result = time_sync_dpu_load_dpa_app(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca dpa app: %s", doca_error_get_descr(result));
		goto destroy_clock;
	}

	/* Configure a doca comch server to receive messages from host app */
	result = time_sync_dpu_init_comch_server(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init comch server: %s", doca_error_get_descr(result));
		goto unload_dpa;
	}

	/* Run the main loop of the app */
	result = time_sync_dpu_main_loop(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed in application main loop: %s", doca_error_get_descr(result));
		goto close_comch;
	}

	exit_status = EXIT_SUCCESS;

close_comch:
	(void)time_sync_dpu_close_comch_server(&ts_cfg);
unload_dpa:
	(void)time_sync_dpu_unload_dpa_app(&ts_cfg);
destroy_clock:
	time_sync_common_destroy_clock(&ts_cfg);
close_devs:
	(void)time_sync_common_close_devs(&ts_cfg);
destroy_argp:
	(void)doca_argp_destroy();

	return exit_status;
}
