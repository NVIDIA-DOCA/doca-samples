/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"

#include <doca_log.h>

/**
 * Callback that is invoked once SPDK starts the application as part of spdk_app_start
 *
 * @cookie [in]: Argument that was passed along with this callback in spdk_app_start
 */
static void nvmf_tgt_started(void *cookie)
{
	(void)cookie;
}

/*
 * NVMe emulation application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;
	struct spdk_app_opts opts = {};
	int rc;
	doca_error_t result;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_log_level_set_global_sdk_limit(DOCA_LOG_LEVEL_ERROR);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_ERROR);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* default value in opts */
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "nvmf";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL, NULL, NULL)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, nvmf_tgt_started, NULL);
	spdk_app_fini();

	if (rc != 0)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}