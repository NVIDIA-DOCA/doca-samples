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
#include <doca_error.h>
#include <doca_log.h>

#include <log_common.h>

DOCA_LOG_REGISTER(LOG_LIMITS_CLIENT::MAIN);

/*
 * Sample's Logic
 *
 * @cfg [in]: Log sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t run_log_limits_client(const struct log_sample_config *cfg);

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
	struct doca_log_backend *app_log;
	struct doca_log_backend *sdk_log;
	struct log_sample_config log_conf = {0};
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values (Example values) */
	result = set_default_log_sample_config(&log_conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set default log sample config: %s", doca_error_get_name(result));
		goto sample_exit;
	}
	log_conf.is_client = true;

	/* Register a logger backend for application logs */
	result = doca_log_backend_create_with_file(stdout, &app_log);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create application logger backend: %s", doca_error_get_name(result));
		goto sample_exit;
	}
	result = doca_log_backend_set_level_upper_limit(app_log, DOCA_LOG_LEVEL_ERROR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set application log level upper limit: %s", doca_error_get_name(result));
		goto sample_exit;
	}

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create SDK logger backend: %s", doca_error_get_name(result));
		goto sample_exit;
	}
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SDK log level: %s", doca_error_get_name(result));
		goto sample_exit;
	}

	DOCA_LOG_INFO("Starting the sample");

	/* Initialize argparser */
	result = doca_argp_init(NULL, &log_conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_name(result));
		goto sample_exit;
	}
	/* Register sample parameters */
	result = register_log_sample_params(log_conf.is_client);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample parameters: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}
	/* Start argparser */
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	/* After parsing the arguments, print the sample configuration */
	result = print_log_sample_config(&log_conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to print sample config: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	/* Start the sample */
	result = run_log_limits_client(&log_conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run sample: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
