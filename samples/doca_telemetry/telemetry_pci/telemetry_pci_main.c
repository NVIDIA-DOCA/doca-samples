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

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_telemetry_pci.h>
#include "telemetry_pci_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_PCI::MAIN);

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct telemetry_pci_sample_cfg *telemetry_pci_sample_cfg = (struct telemetry_pci_sample_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(telemetry_pci_sample_cfg->pci_addr, pci_address, len + 1);
	telemetry_pci_sample_cfg->pci_set = true;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DPN parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpn_callback(void *param, void *config)
{
	struct telemetry_pci_sample_cfg *telemetry_pci_sample_cfg = (struct telemetry_pci_sample_cfg *)config;
	char *dpn_str = (char *)param;
	char *dpn_str_end = dpn_str + strlen(dpn_str);

	telemetry_pci_sample_cfg->dpn.depth = strtoul(dpn_str, &dpn_str, 0);
	if (*dpn_str != '.') {
		DOCA_LOG_ERR("Invalid DPN value: %s", dpn_str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	++dpn_str; /* skip past the dot */

	telemetry_pci_sample_cfg->dpn.pci_index = strtoul(dpn_str, &dpn_str, 0);
	if (*dpn_str != '.') {
		DOCA_LOG_ERR("Invalid DPN value: %s", dpn_str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	++dpn_str; /* skip past the dot */

	telemetry_pci_sample_cfg->dpn.node = strtoul(dpn_str, &dpn_str, 0);
	if (dpn_str != dpn_str_end) {
		DOCA_LOG_ERR("Invalid DPN value: %s", dpn_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_telemetry_pci_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param;
	struct doca_argp_param *dpn_param;

	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI device address");
	doca_argp_param_set_callback(pci_param, pci_address_callback);
	doca_argp_param_set_type(pci_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&dpn_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(dpn_param, "d");
	doca_argp_param_set_long_name(dpn_param, "dpn");
	doca_argp_param_set_description(dpn_param, "Desired DPN (depth.pci_index.node) value");
	doca_argp_param_set_callback(dpn_param, dpn_callback);
	doca_argp_param_set_type(dpn_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpn_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

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
	doca_error_t result;
	int exit_status = EXIT_FAILURE;
	struct telemetry_pci_sample_cfg sample_cfg = {};
	struct doca_log_backend *sdk_log;

	sample_cfg.pci_set = false;

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

	result = doca_argp_init(NULL, &sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_name(result));
		goto sample_exit;
	}

	result = register_telemetry_pci_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	if (!sample_cfg.pci_set) {
		DOCA_LOG_ERR("PCI address must be provided");
		goto argp_cleanup;
	}

	result = telemetry_pci_sample_run(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("telemetry_pci_sample_run() encountered an error: %s", doca_error_get_name(result));
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
