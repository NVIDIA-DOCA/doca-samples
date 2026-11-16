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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>

#include "lifeguard_deposit_sample.h"

DOCA_LOG_REGISTER(LIFEGUARD_DEPOSIT::MAIN);

/*
 * Copy a NUL-terminated string into a fixed buffer. Reject empty and truncated input.
 *
 * @dst [out]: Destination buffer
 * @dst_size [in]: Size of dst, including space for the NUL
 * @src [in]: Source string
 * @what [in]: Parameter name used in the error log
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t copy_bounded_string(char *dst, size_t dst_size, const char *src, const char *what)
{
	size_t len;

	len = strnlen(src, dst_size);
	if (len == 0 || len >= dst_size) {
		DOCA_LOG_ERR("Invalid %s", what);
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(dst, src, len + 1);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle device parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_callback(void *param, void *config)
{
	struct lifeguard_deposit_cfg *cfg = config;

	return copy_bounded_string(cfg->device, sizeof(cfg->device), param, "device");
}

/*
 * ARGP Callback - Handle lifeguard socket path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t socket_path_callback(void *param, void *config)
{
	struct lifeguard_deposit_cfg *cfg = config;

	return copy_bounded_string(cfg->socket_path, sizeof(cfg->socket_path), param, "socket path");
}

/*
 * Register one string parameter.
 *
 * @short_name [in]: Short option character
 * @long_name [in]: Long option name
 * @description [in]: Help text
 * @mandatory [in]: Whether the parameter is required
 * @callback [in]: ARGP callback
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_string_param(const char *short_name,
					  const char *long_name,
					  const char *description,
					  bool mandatory,
					  doca_argp_param_cb_t callback)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, short_name);
	doca_argp_param_set_long_name(param, long_name);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	if (mandatory)
		doca_argp_param_set_mandatory(param);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_lifeguard_deposit_params(void)
{
	doca_error_t result;

	result = register_string_param("p",
				       "device",
				       "Interface name (for example p0) or PCI address",
				       true,
				       device_callback);
	if (result != DOCA_SUCCESS)
		return result;

	return register_string_param("s",
				     "socket",
				     "Lifeguard socket path (default: " DOCA_LIFEGUARD_DEFAULT_SOCKET_PATH ")",
				     false,
				     socket_path_callback);
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
	struct lifeguard_deposit_cfg cfg = {0};
	struct doca_log_backend *sdk_log;
	doca_error_t result;
	int exit_status = EXIT_FAILURE;

	memcpy(cfg.socket_path, DOCA_LIFEGUARD_DEFAULT_SOCKET_PATH, sizeof(DOCA_LIFEGUARD_DEFAULT_SOCKET_PATH));

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init("doca_lifeguard_deposit", &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_lifeguard_deposit_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = lifeguard_deposit(&cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("lifeguard_deposit() encountered an error: %s", doca_error_get_descr(result));
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
