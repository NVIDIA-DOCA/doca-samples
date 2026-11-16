/*
 * Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <string.h>

#include <doca_argp.h>
#include <doca_config.h>
#include <doca_log.h>

#include <utils.h>

#include "file_compression_core.h"

#define SERVER_NAME "file_compression_server" /* CC server name */

DOCA_LOG_REGISTER(FILE_COMPRESSION);

/*
 * File Compression application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct file_compression_config compress_cfg = {
		.mode = NO_VALID_INPUT,
	};

	struct comch_cfg *comch_cfg;
	struct compress_resources resources = {0};
	doca_error_t result;
	struct doca_log_backend *sdk_log;

#ifdef DOCA_ARCH_HOST
	compress_cfg.mode = CLIENT;
#else
	compress_cfg.mode = SERVER;
#endif

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Parse cmdline/json arguments */
	result = doca_argp_init(NULL, &compress_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = register_file_compression_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application params: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = file_compression_init(&compress_cfg, &resources);
	if (result != DOCA_SUCCESS) {
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = comch_utils_init(SERVER_NAME,
				  compress_cfg.cc_dev_pci_addr,
				  compress_cfg.cc_dev_rep_pci_addr,
				  &compress_cfg,
				  client_recv_event_cb,
				  server_recv_event_cb,
				  &comch_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize a comch: %s", doca_error_get_descr(result));
		file_compression_cleanup(&compress_cfg, &resources);
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	/* Start client/server logic */
	if (compress_cfg.mode == CLIENT)
		result = file_compression_client(comch_cfg, &compress_cfg, &resources);
	else
		result = file_compression_server(comch_cfg, &compress_cfg, &resources);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("File compression encountered errors");
		result = comch_utils_destroy(comch_cfg);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Comch");
		file_compression_cleanup(&compress_cfg, &resources);
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = comch_utils_destroy(comch_cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA Comch");

	file_compression_cleanup(&compress_cfg, &resources);

	/* ARGP cleanup */
	doca_argp_destroy();

	return EXIT_SUCCESS;
}
