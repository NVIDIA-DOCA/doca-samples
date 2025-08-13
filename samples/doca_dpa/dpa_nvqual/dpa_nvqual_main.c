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

#include <dpa_common.h>
#include <dpa_nvqual_common_defs.h>

DOCA_LOG_REGISTER(DPA_NVQUAL_TEST);

/**
 * Sample's logic
 */
doca_error_t dpa_nvqual(struct dpa_nvqual_config *nvqual_argp_cfg);
/**
 * Sample's parameters registration
 */
doca_error_t dpa_nvqual_register_params(void);

/**
 * @brief Sample's main function
 *
 * @argc [in]: sample arguments length
 * @argv [in]: sample arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct dpa_nvqual_config nvqual_argp_cfg;
	doca_error_t err = DOCA_SUCCESS;
	struct doca_log_backend *sdk_log = NULL;
	int exit_status = EXIT_FAILURE;

	strcpy(nvqual_argp_cfg.dev_name, "");
	for (int i = 0; i < DPA_NVQUAL_MAX_EUS; i++) {
		nvqual_argp_cfg.excluded_eus[i] = false;
	}
	nvqual_argp_cfg.excluded_eus_size = 0;
	nvqual_argp_cfg.test_duration_sec = 0;
	nvqual_argp_cfg.user_factor = 0;

	err = doca_log_backend_create_standard();
	if (err != DOCA_SUCCESS)
		goto sample_exit;

	err = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (err != DOCA_SUCCESS)
		goto sample_exit;
	err = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (err != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	err = doca_argp_init(NULL, &nvqual_argp_cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(err));
		goto sample_exit;
	}

	err = dpa_nvqual_register_params();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample parameters: %s", doca_error_get_descr(err));
		goto argp_cleanup;
	}

	err = doca_argp_start(argc, argv);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(err));
		goto argp_cleanup;
	}

	err = dpa_nvqual(&nvqual_argp_cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_nvqual() encountered an error: %s", doca_error_get_descr(err));
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
