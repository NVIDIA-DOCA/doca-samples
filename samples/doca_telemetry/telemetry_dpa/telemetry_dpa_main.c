/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <json-c/json.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_telemetry_dpa.h>
#include "telemetry_dpa_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_DPA::MAIN);

#define DEFAULT_TOTAL_RUN_TIME_MILLISECS 2000
#define DEFAULT_MAX_PERF_EVENT_SAMPLES 0

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct telemetry_dpa_sample_cfg *telemetry_dpa_sample_cfg = (struct telemetry_dpa_sample_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(telemetry_dpa_sample_cfg->pci_addr, pci_address, len + 1);
	telemetry_dpa_sample_cfg->pci_set = true;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle sample time parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_time_callback(void *param, void *config)
{
	struct telemetry_dpa_sample_cfg *telemetry_dpa_sample_cfg = (struct telemetry_dpa_sample_cfg *)config;
	uint32_t *run_time = (uint32_t *)param;

	telemetry_dpa_sample_cfg->run_time = *run_time;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle counter type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t counter_type_callback(void *param, void *config)
{
	struct telemetry_dpa_sample_cfg *telemetry_dpa_sample_cfg = (struct telemetry_dpa_sample_cfg *)config;
	enum doca_telemetry_dpa_counter_type *counter_type = (enum doca_telemetry_dpa_counter_type *)param;

	if (*counter_type != DOCA_TELEMETRY_DPA_COUNTER_TYPE_CUMULATIVE_EVENT &&
	    *counter_type != DOCA_TELEMETRY_DPA_COUNTER_TYPE_EVENT_TRACER) {
		DOCA_LOG_ERR("Entered counter type=%d is invalid", *counter_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	telemetry_dpa_sample_cfg->counter_type = *counter_type;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle event samples parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t event_samples_callback(void *param, void *config)
{
	struct telemetry_dpa_sample_cfg *telemetry_dpa_sample_cfg = (struct telemetry_dpa_sample_cfg *)config;
	uint32_t *max_event_sample = (uint32_t *)param;

	telemetry_dpa_sample_cfg->max_event_sample = *max_event_sample;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle process_id parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t process_id_callback(void *param, void *config)
{
	struct telemetry_dpa_sample_cfg *telemetry_dpa_sample_cfg = (struct telemetry_dpa_sample_cfg *)config;
	uint32_t *process_id = (uint32_t *)param;

	telemetry_dpa_sample_cfg->process_id = *process_id;
	telemetry_dpa_sample_cfg->process_id_set = true;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle thread_id parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t thread_id_callback(void *param, void *config)
{
	struct telemetry_dpa_sample_cfg *telemetry_dpa_sample_cfg = (struct telemetry_dpa_sample_cfg *)config;
	uint32_t *thread_id = (uint32_t *)param;

	telemetry_dpa_sample_cfg->thread_id = *thread_id;
	telemetry_dpa_sample_cfg->thread_id_set = true;
	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_telemetry_dpa_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *run_time_param, *counter_type_param, *event_samples_param,
		*process_id_param, *thread_id_param;

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

	result = doca_argp_param_create(&run_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(run_time_param, "rt");
	doca_argp_param_set_long_name(run_time_param, "sample-run-time");
	doca_argp_param_set_description(run_time_param, "Total sample run time, in miliseconds");
	doca_argp_param_set_callback(run_time_param, run_time_callback);
	doca_argp_param_set_type(run_time_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(run_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&counter_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(counter_type_param, "ct");
	doca_argp_param_set_long_name(counter_type_param, "counter-type");
	doca_argp_param_set_description(counter_type_param, "Counter type, cumulus (0) or event (1)");
	doca_argp_param_set_callback(counter_type_param, counter_type_callback);
	doca_argp_param_set_type(counter_type_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(counter_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&event_samples_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(event_samples_param, "es");
	doca_argp_param_set_long_name(event_samples_param, "event-samples");
	doca_argp_param_set_description(event_samples_param,
					"Set the maximum number of perf event samples to retrieve");
	doca_argp_param_set_callback(event_samples_param, event_samples_callback);
	doca_argp_param_set_type(event_samples_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(event_samples_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&process_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(process_id_param, "pi");
	doca_argp_param_set_long_name(process_id_param, "process-id");
	doca_argp_param_set_description(process_id_param, "Specific process id to address");
	doca_argp_param_set_callback(process_id_param, process_id_callback);
	doca_argp_param_set_type(process_id_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(process_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&thread_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(thread_id_param, "ti");
	doca_argp_param_set_long_name(thread_id_param, "thread-id");
	doca_argp_param_set_description(thread_id_param, "Specific thread id to address");
	doca_argp_param_set_callback(thread_id_param, thread_id_callback);
	doca_argp_param_set_type(thread_id_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(thread_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Set the default parameters to be used in the sample.
 *
 * @cfg [in]: the sample configuration
 */
static void set_default_params(struct telemetry_dpa_sample_cfg *cfg)
{
	cfg->run_time = DEFAULT_TOTAL_RUN_TIME_MILLISECS;
	cfg->pci_set = false;
	cfg->process_id_set = false;
	cfg->thread_id_set = false;
	cfg->counter_type = DOCA_TELEMETRY_DPA_COUNTER_TYPE_CUMULATIVE_EVENT;
	cfg->max_event_sample = DEFAULT_MAX_PERF_EVENT_SAMPLES;
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
	struct telemetry_dpa_sample_cfg sample_cfg = {};
	struct doca_log_backend *sdk_log;

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

	set_default_params(&sample_cfg);

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init(NULL, &sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_name(result));
		goto sample_exit;
	}

	result = register_telemetry_dpa_params();
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

	result = telemetry_dpa_sample_run(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("telemetry_dpa_sample_run() encountered an error: %s", doca_error_get_name(result));
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
