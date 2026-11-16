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
#include <stdio.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(MEMOS_SIMPLE_TARGET::MAIN);

#define MEMOS_TRADDR_SIZE 256
#define MEMOS_TRSVCID_SIZE 32
#define MEMOS_PATH_SIZE 512

/* Sample configuration populated from the command line. */
struct memos_simple_target_cfg {
	char traddr[MEMOS_TRADDR_SIZE];	  /* NVMe-oF transport address (IP) */
	char trsvcid[MEMOS_TRSVCID_SIZE]; /* NVMe-oF transport service id (port) */
	char file_path[MEMOS_PATH_SIZE];  /* Backing file for the namespace */
};

/* Sample's Logic */
doca_error_t memos_simple_target_run(const char *traddr, const char *trsvcid, const char *file_path);

/*
 * ARGP callback - transport address (IP).
 */
static doca_error_t addr_callback(void *param, void *config)
{
	struct memos_simple_target_cfg *cfg = (struct memos_simple_target_cfg *)config;
	const char *addr = (const char *)param;

	if (strlen(addr) >= sizeof(cfg->traddr)) {
		DOCA_LOG_ERR("Transport address too long (max %zu)", sizeof(cfg->traddr) - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	(void)snprintf(cfg->traddr, sizeof(cfg->traddr), "%s", addr);
	return DOCA_SUCCESS;
}

/*
 * ARGP callback - transport service id (port number).
 */
static doca_error_t port_callback(void *param, void *config)
{
	struct memos_simple_target_cfg *cfg = (struct memos_simple_target_cfg *)config;
	const char *port = (const char *)param;

	if (strlen(port) >= sizeof(cfg->trsvcid)) {
		DOCA_LOG_ERR("Port too long (max %zu)", sizeof(cfg->trsvcid) - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	(void)snprintf(cfg->trsvcid, sizeof(cfg->trsvcid), "%s", port);
	return DOCA_SUCCESS;
}

/*
 * ARGP callback - backing file path.
 */
static doca_error_t file_callback(void *param, void *config)
{
	struct memos_simple_target_cfg *cfg = (struct memos_simple_target_cfg *)config;
	const char *file_path = (const char *)param;

	if (strlen(file_path) >= sizeof(cfg->file_path)) {
		DOCA_LOG_ERR("File path too long (max %zu)", sizeof(cfg->file_path) - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	(void)snprintf(cfg->file_path, sizeof(cfg->file_path), "%s", file_path);
	return DOCA_SUCCESS;
}

/*
 * Register a single mandatory string ARGP parameter.
 *
 * @short_name [in]: short option name
 * @long_name [in]: long option name
 * @description [in]: help description
 * @callback [in]: option callback
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_string_param(const char *short_name,
					  const char *long_name,
					  const char *arguments,
					  const char *description,
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
	doca_argp_param_set_arguments(param, arguments);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(param);

	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_memos_simple_target_params(void)
{
	doca_error_t result;

	result = register_string_param("a", "addr", "<ip>", "Transport address (IP) to listen on", addr_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_string_param("p", "port", "<port>", "Transport service id (port number)", port_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_string_param("f", "file", "<path>", "Backing file for the namespace", file_callback);
	if (result != DOCA_SUCCESS)
		return result;

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
	struct memos_simple_target_cfg cfg = {0};
	struct doca_log_backend *sdk_log;
	doca_error_t result;
	int exit_status = EXIT_FAILURE;

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

	result = doca_argp_init(NULL, &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_memos_simple_target_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = memos_simple_target_run(cfg.traddr, cfg.trsvcid, cfg.file_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("memos_simple_target_run() encountered an error: %s", doca_error_get_descr(result));
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
