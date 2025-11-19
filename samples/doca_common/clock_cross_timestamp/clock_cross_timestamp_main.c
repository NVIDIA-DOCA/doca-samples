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

#include <stdlib.h>
#include <stdbool.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>

#include "clock_cross_timestamp_sample.h"

DOCA_LOG_REGISTER(CLOCK_CROSS_TIMESTAMP::MAIN);

#define NIC_CLOCK_MASK (DOCA_CLOCK_NIC_FREE_RUNNING | DOCA_CLOCK_NIC_REAL_TIME | DOCA_CLOCK_NIC_DPA_TIMER)

/*
 * Convert a clock type string to lib internal representation
 *
 * @clock_type [in]: clock string
 * @return: uint64_t representation of clock, 0 on error
 */
static uint64_t get_clock_id(char *clock_type)
{
	if (strcmp(clock_type, "nic_fr") == 0)
		return DOCA_CLOCK_NIC_FREE_RUNNING;
	if (strcmp(clock_type, "nic_rt") == 0)
		return DOCA_CLOCK_NIC_REAL_TIME;
	if (strcmp(clock_type, "nic_dpa") == 0)
		return DOCA_CLOCK_NIC_DPA_TIMER;
	if (strcmp(clock_type, "host_cyles") == 0)
		return DOCA_CLOCK_HOST_COUNTER_CYCLES;
	if (strcmp(clock_type, "host_rt") == 0)
		return DOCA_CLOCK_HOST_REAL_TIME;
	if (strcmp(clock_type, "host_mon") == 0)
		return DOCA_CLOCK_HOST_MONOTONIC;
	if (strcmp(clock_type, "host_mon_raw") == 0)
		return DOCA_CLOCK_HOST_MONOTONIC_RAW;

	return 0;
}

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct clock_cross_timestamp_sample_cfg *cfg = (struct clock_cross_timestamp_sample_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(cfg->pci_addr, pci_address, len + 1);
	cfg->pci_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle primary clock parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prim_clock_callback(void *param, void *config)
{
	struct clock_cross_timestamp_sample_cfg *cfg = (struct clock_cross_timestamp_sample_cfg *)config;
	char *clock = (char *)param;

	cfg->primary_clock = get_clock_id(clock);

	if (cfg->primary_clock == 0) {
		DOCA_LOG_ERR("Unrecognised clock type: %s", clock);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle nic clock parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nic_clock_callback(void *param, void *config)
{
	struct clock_cross_timestamp_sample_cfg *cfg = (struct clock_cross_timestamp_sample_cfg *)config;
	char *clock = (char *)param;

	cfg->nic_clock = get_clock_id(clock);

	if (cfg->nic_clock == 0) {
		DOCA_LOG_ERR("Unrecognised clock type: %s", clock);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if ((cfg->nic_clock & NIC_CLOCK_MASK) == 0) {
		DOCA_LOG_ERR("Input clock (%s) is not a NIC clock", clock);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_clock_cross_timestamp_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *prim_clock, *nic_clock;

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

	result = doca_argp_param_create(&prim_clock);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(prim_clock, "c");
	doca_argp_param_set_long_name(prim_clock, "primary-clock");
	doca_argp_param_set_description(
		prim_clock,
		"'nic_fr', 'nic_rt', 'nic_dpa', 'host_cyles', 'host_rt', 'host_mon', or 'host_mon_raw'");
	doca_argp_param_set_mandatory(prim_clock);
	doca_argp_param_set_callback(prim_clock, prim_clock_callback);
	doca_argp_param_set_type(prim_clock, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(prim_clock);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&nic_clock);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(nic_clock, "n");
	doca_argp_param_set_long_name(nic_clock, "nic-clock");
	doca_argp_param_set_description(nic_clock, "'nic_fr', 'nic_rt', or 'nic_dpa'");
	doca_argp_param_set_callback(nic_clock, nic_clock_callback);
	doca_argp_param_set_type(nic_clock, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(nic_clock);
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
int main(int argc, char *argv[])
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	struct clock_cross_timestamp_sample_cfg cfg = {};
	int exit_status = EXIT_FAILURE;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

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
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_name(result));
		goto sample_exit;
	}

	result = register_clock_cross_timestamp_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	if (!cfg.pci_set) {
		DOCA_LOG_ERR("PCI address must be provided");
		goto argp_cleanup;
	}

	/* Run the sample's core function */
	result = run_clock_cross_timestamp_sample(&cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("run_clock_cross_timestamp_sample() encountered an error: %s",
			     doca_error_get_descr(result));
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
