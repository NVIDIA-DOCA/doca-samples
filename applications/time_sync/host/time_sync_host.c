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
#include <doca_comch.h>
#include <doca_log.h>

#include "time_sync_host_core.h"

DOCA_LOG_REGISTER(TIME_SYNC::TIME_SYNC_HOST);

/* Default delay of 1 sec (unit is msecs) */
#define DEFAULT_DELAY 1000

/*
 * Helper function to check capabilities of selected device for use in app
 *
 * @devinfo [in]: device to check caps for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t time_sync_dpu_cap_check(struct doca_devinfo *devinfo)
{
	doca_error_t result;

	/* Host device must have comch client capabilities */
	result = doca_comch_cap_client_is_supported(devinfo);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Comch client is not supported on device: %s", doca_error_get_descr(result));
		return result;
	}

	/* Host requires associated NIC to have real time clock enabled */
	result = doca_clock_cap_nic_real_time_is_supported(devinfo);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Real time clock is not supported on device: %s", doca_error_get_descr(result));
		return result;
	}

	/* Host requires support for DPA timer clock */
	result = doca_clock_cap_nic_dpa_timer_is_supported(devinfo);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("DPA timer is not supported on device: %s", doca_error_get_descr(result));

	return result;
}

/*
 * Time sync host application main function
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

	/* Initialise the delay value before reading user input */
	ts_cfg.delay = DEFAULT_DELAY;

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

	/* Open device for use in DOCA comch context */
	result = time_sync_common_open_dev_with_caps(&ts_cfg, time_sync_dpu_cap_check);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open host doca dev: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	/* Create a clock context for cross timestamping */
	result = time_sync_common_create_clock(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create clock: %s", doca_error_get_descr(result));
		goto close_devs;
	}

	/* Configure a doca comch client to send/receive messages to/from the DPU app */
	result = time_sync_host_init_comch_client(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init comch client: %s", doca_error_get_descr(result));
		goto destroy_clock;
	}

	/* Run the main loop of the app */
	result = time_sync_host_main_loop(&ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed in application main loop: %s", doca_error_get_descr(result));
		goto close_comch;
	}

	exit_status = EXIT_SUCCESS;

close_comch:
	(void)time_sync_host_close_comch_client(&ts_cfg);
destroy_clock:
	time_sync_common_destroy_clock(&ts_cfg);
close_devs:
	(void)time_sync_common_close_devs(&ts_cfg);
destroy_argp:
	(void)doca_argp_destroy();

	return exit_status;
}
