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

#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_log.h>

#include "dma_common.h"

DOCA_LOG_REGISTER(DPU_LOCAL_DMA_COPY::MAIN);

/* Sample's Logic */
doca_error_t dma_local_copy(const struct dma_config *dma_conf, char *dst_buffer, char *src_buffer, size_t length);

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct dma_config dma_conf;
	char *dst_buffer = NULL, *src_buffer = NULL;
	size_t length;
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	int num_tasks = DEFAULT_NUM_TASKS;
	int i = 0;

	/* Set the default configuration values (Example values) */
	strcpy(dma_conf.pci_address, "03:00.0");
	strcpy(dma_conf.cpy_txt, "This is a sample piece of text");
	dma_conf.num_dst_buf = 1;
	dma_conf.num_src_buf = 1;
	dma_conf.num_tasks = DEFAULT_NUM_TASKS;
	dma_conf.max_num_range_profiling_displayed = 0;
	/* No need to set export_desc_path and buf_info_path which are only needed for DMA across devices */

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create standard logger backend: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create logger backend for internal SDK errors and warnings: %s",
			     doca_error_get_descr(result));
		goto sample_exit;
	}
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SDK level: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init(NULL, &dma_conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_dma_params(false, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register DMA sample parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

#ifndef DOCA_ARCH_DPU
	if (dma_conf.max_num_range_profiling_displayed == 0) {
		DOCA_LOG_ERR("Local DMA copy can run only on the DPU");
		goto argp_cleanup;
	}
#endif

	num_tasks = dma_conf.num_tasks;
	length = strlen(dma_conf.cpy_txt) + 1;
	dst_buffer = (char *)calloc(1, length * num_tasks);
	if (dst_buffer == NULL) {
		DOCA_LOG_ERR("Destination buffer allocation failed");
		goto argp_cleanup;
	}

	src_buffer = (char *)malloc(length * num_tasks);
	if (src_buffer == NULL) {
		DOCA_LOG_ERR("Source buffer allocation failed");
		goto dst_buffer_cleanup;
	}

	for (i = 0; i < num_tasks; i++) {
		memcpy(src_buffer + i * length, dma_conf.cpy_txt, length);
	}

	result = dma_local_copy(&dma_conf, dst_buffer, src_buffer, length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dma_local_copy() encountered an error: %s", doca_error_get_descr(result));
		goto src_buffer_cleanup;
	}

	exit_status = EXIT_SUCCESS;

src_buffer_cleanup:
	if (src_buffer != NULL)
		free(src_buffer);
dst_buffer_cleanup:
	if (dst_buffer != NULL)
		free(dst_buffer);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
