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
#include <doca_telemetry_diag.h>
#include "telemetry_diag_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_DIAG::MAIN);

#define MAX_DESCRIPTION_LEN 256
#define DATA_ID_STRING_MAX_LEN 20

#define DEFAULT_SAMPLE_PERIOD_NS 100000 /* 100 usec */
#define DEFAULT_LOG_MAX_NUM_SAMPLES 10
#define DEFAULT_MAX_NUM_SAMPLES_PER_READ 128
#define DEFAULT_SAMPLE_MODE DOCA_TELEMETRY_DIAG_SAMPLE_MODE_REPETITIVE
#define DEFAULT_OUTPUT_FORMAT DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_1
#define DEFAULT_TOTAL_RUN_TIME_SECS 1
#define DEFAULT_OUTPUT_PATH "/tmp/out.csv"
#define DEFAULT_FORCE_OWNERSHIP 0
#define DEFAULT_DATA_IDS_PATH "/0"
#define DEFAULT_EXAMPLE_JSON_PATH "/0"

#define JSON_NAME_KEY "name"
#define JSON_DATA_ID_KEY "data_id"

#define DATA_ID_PORT_0_RX_BYTES 0x1020000100000000
#define DATA_ID_PORT_0_TX_BYTES 0x1140000100000000
#define DATA_ID_PORT_0_RX_PACKETS 0x1020000300000000
#define DATA_ID_PORT_0_TX_PACKETS 0x1140000300000000

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(telemetry_diag_sample_cfg->pci_addr, pci_address, len + 1);
	telemetry_diag_sample_cfg->pci_set = true;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle data ids file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t data_ids_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	char *json_path = (char *)param;
	int len;

	len = strnlen(json_path, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME);
	if (len == TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid path: data-ids file name length exceeds the maximum of %d characters",
			     TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (access(json_path, F_OK) == -1) {
		DOCA_LOG_ERR("JSON file was not found %s", json_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	strncpy(telemetry_diag_sample_cfg->data_ids_input_path, json_path, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
	telemetry_diag_sample_cfg->import_json = true;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle output file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t output_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME);
	if (len == TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid path: output file name length exceeds the maximum of %d characters",
			     TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(telemetry_diag_sample_cfg->output_path, file, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
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
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	uint32_t *run_time = (uint32_t *)param;

	telemetry_diag_sample_cfg->run_time = *run_time;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle sample_period parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sample_period_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	uint32_t *sample_period = (uint32_t *)param;

	telemetry_diag_sample_cfg->sample_period = *sample_period;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle log_max_num_samples parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t log_max_num_samples_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	uint32_t *log_max_num_samples = (uint32_t *)param;

	if (*log_max_num_samples > __UINT8_MAX__) {
		DOCA_LOG_ERR("Parameter log_max_num_samples larger than uint8. log_max_num_samples=%d",
			     *log_max_num_samples);
		return DOCA_ERROR_INVALID_VALUE;
	}
	telemetry_diag_sample_cfg->log_max_num_samples = *((uint8_t *)log_max_num_samples);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle max_num_samples_per_read parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t max_num_samples_per_read_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	uint32_t *max_num_samples_per_read = (uint32_t *)param;

	if (*max_num_samples_per_read > __UINT8_MAX__) {
		DOCA_LOG_ERR("Parameter max_num_samples_per_read larger than uint8. max_num_samples_per_read=%d",
			     *max_num_samples_per_read);
		return DOCA_ERROR_INVALID_VALUE;
	}
	telemetry_diag_sample_cfg->max_num_samples_per_read = *max_num_samples_per_read;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle sample_mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sample_mode_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	enum doca_telemetry_diag_sample_mode sample_mode = *(enum doca_telemetry_diag_sample_mode *)param;

	telemetry_diag_sample_cfg->sample_mode = sample_mode;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle output_format parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t output_format_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	enum doca_telemetry_diag_output_format output_format = *(enum doca_telemetry_diag_output_format *)param;

	telemetry_diag_sample_cfg->output_format = output_format;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle force_ownership parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t force_ownership_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	bool force_ownership = *(bool *)param;

	telemetry_diag_sample_cfg->force_ownership = !!force_ownership;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - example json file parameter
 * If set, create a new file in the location given and fill with the example data_ids json
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t example_json_file_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME);
	if (len == TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid path: output file name length exceeds the maximum of %d characters",
			     TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(telemetry_diag_sample_cfg->data_ids_example_export_path, file, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
	telemetry_diag_sample_cfg->export_json = true;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle reconfig_sample_period parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t reconfig_sample_period_callback(void *param, void *config)
{
	struct telemetry_diag_sample_cfg *telemetry_diag_sample_cfg = (struct telemetry_diag_sample_cfg *)config;
	uint32_t *reconfig_sample_period = (uint32_t *)param;

	telemetry_diag_sample_cfg->reconfig_sample_period = *reconfig_sample_period;
	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_telemetry_diag_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *data_ids_param, *run_time_param, *sample_period_param,
		*log_max_num_samples_param, *max_num_samples_per_read_param, *sample_mode_param, *output_format_param,
		*output_param, *force_ownership_param, *generate_example_json, *reconfig_sample_period_param;

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

	result = doca_argp_param_create(&data_ids_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(data_ids_param, "di");
	doca_argp_param_set_long_name(data_ids_param, "data-ids");
	doca_argp_param_set_description(data_ids_param, "Path to data ids JSON file");
	doca_argp_param_set_callback(data_ids_param, data_ids_callback);
	doca_argp_param_set_type(data_ids_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(data_ids_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&output_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(output_param, "o");
	doca_argp_param_set_long_name(output_param, "output");
	doca_argp_param_set_description(output_param, "Output CSV file - default: \"" DEFAULT_OUTPUT_PATH "\"");
	doca_argp_param_set_callback(output_param, output_callback);
	doca_argp_param_set_type(output_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(output_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&run_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(run_time_param, "rt");
	doca_argp_param_set_long_name(run_time_param, "sample-run-time");
	doca_argp_param_set_description(run_time_param, "Total sample run time, in seconds");
	doca_argp_param_set_callback(run_time_param, run_time_callback);
	doca_argp_param_set_type(run_time_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(run_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&sample_period_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(sample_period_param, "sp");
	doca_argp_param_set_long_name(sample_period_param, "sample-period");
	doca_argp_param_set_description(sample_period_param, "Sample period, in nanoseconds");
	doca_argp_param_set_callback(sample_period_param, sample_period_callback);
	doca_argp_param_set_type(sample_period_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(sample_period_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&log_max_num_samples_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(log_max_num_samples_param, "ns");
	doca_argp_param_set_long_name(log_max_num_samples_param, "log-num-samples");
	doca_argp_param_set_description(log_max_num_samples_param, "Log max number of samples");
	doca_argp_param_set_callback(log_max_num_samples_param, log_max_num_samples_callback);
	doca_argp_param_set_type(log_max_num_samples_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(log_max_num_samples_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&max_num_samples_per_read_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(max_num_samples_per_read_param, "sr");
	doca_argp_param_set_long_name(max_num_samples_per_read_param, "max-samples-per-read");
	doca_argp_param_set_description(max_num_samples_per_read_param, "Max num samples per read");
	doca_argp_param_set_callback(max_num_samples_per_read_param, max_num_samples_per_read_callback);
	doca_argp_param_set_type(max_num_samples_per_read_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(max_num_samples_per_read_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&sample_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(sample_mode_param, "sm");
	doca_argp_param_set_long_name(sample_mode_param, "sample-mode");
	doca_argp_param_set_description(sample_mode_param, "sample mode (0 - single, 1 - repetitive, 2 - on demand)");
	doca_argp_param_set_callback(sample_mode_param, sample_mode_callback);
	doca_argp_param_set_type(sample_mode_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(sample_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&output_format_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(output_format_param, "of");
	doca_argp_param_set_long_name(output_format_param, "output-format");
	doca_argp_param_set_description(output_format_param, "output format");
	doca_argp_param_set_callback(output_format_param, output_format_callback);
	doca_argp_param_set_type(output_format_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(output_format_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&force_ownership_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(force_ownership_param, "f");
	doca_argp_param_set_long_name(force_ownership_param, "force-ownership");
	doca_argp_param_set_description(force_ownership_param, "Force ownership when creating context");
	doca_argp_param_set_callback(force_ownership_param, force_ownership_callback);
	doca_argp_param_set_type(force_ownership_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(force_ownership_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_argp_param_create(&generate_example_json);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(generate_example_json, "e");
	doca_argp_param_set_long_name(generate_example_json, "example-json-path");
	doca_argp_param_set_description(generate_example_json,
					"Generate an example json file with the default data_ids to the given path \
and exit immediately. This file can be used as input later on. \
All other flags are ignored");
	doca_argp_param_set_callback(generate_example_json, example_json_file_callback);
	doca_argp_param_set_type(generate_example_json, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(generate_example_json);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&reconfig_sample_period_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(reconfig_sample_period_param, "rs");
	doca_argp_param_set_long_name(reconfig_sample_period_param, "reconfig-sample-period");
	doca_argp_param_set_description(
		reconfig_sample_period_param,
		"Reconfig sample period, in nanoseconds. It means after the first sampling run with the sample_period, if the reconfig_sample_period is non-0, before the current diag instance is stopped and destroyed, the second sampling run will be done based on the reconfig_sample_period.");
	doca_argp_param_set_callback(reconfig_sample_period_param, reconfig_sample_period_callback);
	doca_argp_param_set_type(reconfig_sample_period_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(reconfig_sample_period_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Parses the "data-ids" array from a JSON object and stores the results
 * in the globally allocated data_ids_struct.
 *
 * @cfg [in]: the sample configuration
 * @json_data_ids [in]: JSON object containing the "data-ids" array.
 * @array_len [in]: The length of the "data-ids" array in json_data_ids.
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t parse_json_data_ids(struct telemetry_diag_sample_cfg *cfg,
					struct json_object *json_data_ids,
					int array_len)
{
	struct json_object *json_entry, *json_data_id, *json_name;
	const char *data_id_str;

	// Loop through each element in the "data-ids" array
	for (int i = 0; i < array_len; i++) {
		json_entry = json_object_array_get_idx(json_data_ids, i); // Get JSON object for the
									  // current entry.

		// Extract "data_id" field, handle error if it's missing or invalid
		if (!json_object_object_get_ex(json_entry, JSON_DATA_ID_KEY, &json_data_id)) {
			DOCA_LOG_ERR("Missing or invalid \"%s\" field in data_ids JSON entry %d", JSON_DATA_ID_KEY, i);
			return DOCA_ERROR_INVALID_VALUE;
		}

		// Extract "name" field, handle error if it's missing or invalid
		if (!json_object_object_get_ex(json_entry, JSON_NAME_KEY, &json_name)) {
			DOCA_LOG_ERR("Missing or invalid \"%s\" field in data_ids JSON entry %d", JSON_NAME_KEY, i);
			return DOCA_ERROR_INVALID_VALUE;
		}

		// Parse the "data_id" field as a hexadecimal string and convert it to uint64_t
		data_id_str = json_object_get_string(json_data_id);
		if (sscanf(data_id_str, "%lx", &cfg->data_ids_struct[i].data_id) != 1) {
			DOCA_LOG_ERR("Failed to parse data_id (expected hexadecimal number): '%s'", data_id_str);
			return DOCA_ERROR_INVALID_VALUE;
		}

		strncpy(cfg->data_ids_struct[i].name, json_object_get_string(json_name), MAX_NAME_SIZE - 1);
		cfg->data_ids_struct[i].name[MAX_NAME_SIZE - 1] = '\0';
	}

	// Store the total number of parsed data-ids
	cfg->num_data_ids = array_len;

	return DOCA_SUCCESS;
}

/*
 * Sets default data IDs and their corresponding names.
 *
 * @cfg [in]: the sample configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t set_default_data_ids(struct telemetry_diag_sample_cfg *cfg)
{
	cfg->num_data_ids = 4;

	cfg->data_ids_struct = calloc(cfg->num_data_ids, sizeof(struct data_id_entry));
	if (!cfg->data_ids_struct) {
		DOCA_LOG_ERR("Failed to allocate memory for data_ids_struct");
		return DOCA_ERROR_NO_MEMORY;
	}

	cfg->data_ids_struct[0].data_id = DATA_ID_PORT_0_RX_BYTES;
	strncpy(cfg->data_ids_struct[0].name, "port_0_rx_bytes", MAX_NAME_SIZE - 1);

	cfg->data_ids_struct[1].data_id = DATA_ID_PORT_0_TX_BYTES;
	strncpy(cfg->data_ids_struct[1].name, "port_0_tx_bytes", MAX_NAME_SIZE - 1);

	cfg->data_ids_struct[2].data_id = DATA_ID_PORT_0_RX_PACKETS;
	strncpy(cfg->data_ids_struct[2].name, "port_0_rx_packets", MAX_NAME_SIZE - 1);

	cfg->data_ids_struct[3].data_id = DATA_ID_PORT_0_TX_PACKETS;
	strncpy(cfg->data_ids_struct[3].name, "port_0_tx_packets", MAX_NAME_SIZE - 1);

	return DOCA_SUCCESS;
}

/*
 * Reads and parses a JSON file containing "data-ids" entries or sets default data IDs if specified.
 * The function checks if the user wants to use default data IDs, handles file I/O (if a JSON file is specified),
 * and calls parse_json_data_ids to handle parsing of the relevant data.
 *
 * @cfg [in]: the sample configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */

static doca_error_t parse_and_read_data_ids_json_file(struct telemetry_diag_sample_cfg *cfg)
{
	// Check if the user wants to use default data IDs.
	if (!cfg->import_json) {
		return set_default_data_ids(cfg);
	}

	FILE *json_fp;
	long temp_length;
	size_t file_length;
	char *json_data = NULL;
	struct json_object *parsed_json, *json_data_ids;
	int array_len = 0;
	doca_error_t result;

	json_fp = fopen(cfg->data_ids_input_path, "r");
	if (json_fp == NULL) {
		DOCA_LOG_ERR("JSON file open failed");
		return DOCA_ERROR_IO_FAILED;
	}

	// Get the size of the file by seeking to the end and then rewinding
	if (fseek(json_fp, 0, SEEK_END) != 0) {
		DOCA_LOG_ERR("Failed to seek to end of JSON file");
		result = DOCA_ERROR_IO_FAILED;
		goto close_file;
	}

	temp_length = ftell(json_fp);
	if (temp_length < 0) {
		DOCA_LOG_ERR("Failed to get file length using ftell");
		result = DOCA_ERROR_IO_FAILED;
		goto close_file;
	}
	file_length = (size_t)temp_length;
	rewind(json_fp);

	// Allocate memory to store the file content
	json_data = malloc(file_length + 1);
	if (json_data == NULL) {
		DOCA_LOG_ERR("Failed to allocate data buffer for the json file");
		result = DOCA_ERROR_NO_MEMORY;
		goto close_file;
	}

	// Read the entire file into the buffer
	if (fread(json_data, 1, file_length, json_fp) < file_length)
		DOCA_LOG_DBG("EOF reached");

	json_data[file_length] = '\0';

	parsed_json = json_tokener_parse(json_data);

	// Check for the presence of the "data-ids" array in the JSON object
	if (!json_object_object_get_ex(parsed_json, "data-ids", &json_data_ids)) {
		DOCA_LOG_ERR("Missing \"data-ids\" parameter in the data ids JSON file");
		result = DOCA_ERROR_INVALID_VALUE;
		goto json_release;
	}

	array_len = json_object_array_length(json_data_ids); // Get the length of the "data-ids" array
	if (array_len == 0) {
		DOCA_LOG_ERR("The \"data-ids\" array in the JSON file is empty");
		result = DOCA_ERROR_INVALID_VALUE;
		goto json_release;
	}

	cfg->data_ids_struct = malloc(sizeof(struct data_id_entry) * array_len); // Allocate memory for data_id_entry
										 // structures.

	// Check if memory allocation was successful
	if (!cfg->data_ids_struct) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate memory for data_ids_struct");
		goto json_release;
	}

	result = parse_json_data_ids(cfg, json_data_ids, array_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse data-ids");
		free(cfg->data_ids_struct);
		goto json_release;
	}

json_release:
	json_object_put(parsed_json);
	free(json_data);
close_file:
	fclose(json_fp);
	return result;
}

/*
 * create pair of name/data_id to be in the data_ids json
 *
 * @name [in]: data_id name
 * @data_id_string [in]: data_id value
 *
 * @return: a json object with the new pair
 */
static json_object *create_data_id_pair(char *name, char *data_id_string)
{
	json_object *data_id, *pair, *name_obj;

	pair = json_object_new_object();
	if (pair == NULL) {
		DOCA_LOG_ERR("Failed to create pair json object");
		return NULL;
	}

	name_obj = json_object_new_string(name);
	if (name_obj == NULL) {
		DOCA_LOG_ERR("Failed to create new name json object");
		goto put_pair;
	}

	if (json_object_object_add(pair, JSON_NAME_KEY, name_obj)) {
		DOCA_LOG_ERR("Failed to add name to default data_ids json");
		json_object_put(name_obj);
		goto put_pair;
	}

	data_id = json_object_new_string(data_id_string);
	if (data_id == NULL) {
		DOCA_LOG_ERR("Failed to create new data_id json object");
		goto put_pair;
	}

	if (json_object_object_add(pair, JSON_DATA_ID_KEY, data_id)) {
		DOCA_LOG_ERR("Failed to add data_id to default data_ids json");
		json_object_put(data_id);
		goto put_pair;
	}

	return pair;

put_pair:
	json_object_put(pair);
	return NULL;
}

/*
 * create an example json object that contains the default data_ids.
 *
 * @cfg [in]: the sample configuration
 * @example_json [out]: new json object
 */
static doca_error_t create_default_json(struct telemetry_diag_sample_cfg *cfg, struct json_object *example_json)
{
	json_object *data_id_array = json_object_new_array();
	json_object *pair;
	char data_id_string[DATA_ID_STRING_MAX_LEN] = {0};

	if (data_id_array == NULL) {
		DOCA_LOG_ERR("Failed to create data_id_array json object");
		return DOCA_ERROR_NO_MEMORY;
	}

	for (uint32_t i = 0; i < cfg->num_data_ids; i++) {
		if (snprintf(data_id_string, sizeof(data_id_string), "0x%lx", cfg->data_ids_struct[i].data_id) < 0) {
			DOCA_LOG_ERR("Failed to create data_id_string");
			goto put_data_id_array;
		}
		pair = create_data_id_pair(cfg->data_ids_struct[i].name, data_id_string);
		if (pair == NULL) {
			DOCA_LOG_ERR("Failed to create data_id pair");
			goto put_data_id_array;
		}
		if (json_object_array_add(data_id_array, pair)) {
			DOCA_LOG_ERR("Failed to add data_id to array");
			goto put_pair;
		}
	}

	if (json_object_object_add(example_json, "data-ids", data_id_array)) {
		DOCA_LOG_ERR("Failed to add data_id_array to example json");
		goto put_data_id_array;
	}
	return DOCA_SUCCESS;

put_pair:
	json_object_put(pair);
put_data_id_array:
	json_object_put(data_id_array);
	return DOCA_ERROR_NO_MEMORY;
}

/*
 * dump the default data_ids into an example json file
 *
 * @cfg [in]: the sample configuration
 */
static doca_error_t generate_example_json(struct telemetry_diag_sample_cfg *cfg)
{
	json_object *example_json;
	FILE *json_file;
	doca_error_t result;
	const char *exported_json;

	result = set_default_data_ids(cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create default data_ids struct with error=%s", doca_error_get_name(result));
		return result;
	}

	json_file = fopen(cfg->data_ids_example_export_path, "w");
	if (json_file == NULL) {
		DOCA_LOG_ERR("Failed to open output file \"%s\" with errno=%s (%d)",
			     cfg->data_ids_example_export_path,
			     strerror(errno),
			     errno);
		result = DOCA_ERROR_NO_MEMORY;
		goto free_data_ids;
	}
	example_json = json_object_new_object();
	if (example_json == NULL) {
		DOCA_LOG_ERR("Failed to create example json");
		result = DOCA_ERROR_NO_MEMORY;
		goto close_file;
	}

	result = create_default_json(cfg, example_json);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create example json");
		goto put_json_object;
	}

	exported_json = json_object_to_json_string_ext(example_json, JSON_C_TO_STRING_PRETTY);
	if (fwrite(exported_json, sizeof(char), strlen(exported_json), json_file) == 0) {
		DOCA_LOG_ERR("Failed to write json to file");
		result = DOCA_ERROR_IO_FAILED;
		goto put_json_object;
	}

	result = DOCA_SUCCESS;

put_json_object:
	json_object_put(example_json);
close_file:
	fclose(json_file);
free_data_ids:
	free(cfg->data_ids_struct);
	return result;
}

/*
 * Set the default parameters to be used in the sample.
 *
 * @cfg [in]: the sample configuration
 */
static void set_default_params(struct telemetry_diag_sample_cfg *cfg)
{
	cfg->sample_period = DEFAULT_SAMPLE_PERIOD_NS;
	cfg->log_max_num_samples = DEFAULT_LOG_MAX_NUM_SAMPLES;
	cfg->max_num_samples_per_read = DEFAULT_MAX_NUM_SAMPLES_PER_READ;
	cfg->run_time = DEFAULT_TOTAL_RUN_TIME_SECS;
	cfg->force_ownership = DEFAULT_FORCE_OWNERSHIP;
	cfg->sample_mode = DEFAULT_SAMPLE_MODE;
	cfg->output_format = DEFAULT_OUTPUT_FORMAT;
	cfg->pci_set = false;
	cfg->import_json = false;
	cfg->export_json = false;
	strncpy(cfg->output_path, DEFAULT_OUTPUT_PATH, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
	strncpy(cfg->data_ids_input_path, DEFAULT_DATA_IDS_PATH, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
	strncpy(cfg->data_ids_example_export_path, DEFAULT_EXAMPLE_JSON_PATH, TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME - 1);
	cfg->reconfig_sample_period = 0;
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
	struct telemetry_diag_sample_cfg sample_cfg = {};
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

	result = register_telemetry_diag_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	if (sample_cfg.export_json) {
		result = generate_example_json(&sample_cfg);
		if (result == DOCA_SUCCESS) {
			DOCA_LOG_INFO("Example data_ids json exported successfully, exiting");
			exit_status = EXIT_SUCCESS;
		} else {
			DOCA_LOG_INFO("Example data_ids json export failed");
		}
		goto argp_cleanup;
	}

	if (!sample_cfg.pci_set) {
		DOCA_LOG_ERR("PCI address must be provided");
		goto argp_cleanup;
	}

	result = parse_and_read_data_ids_json_file(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to list the data ids with error=%s", doca_error_get_name(result));
		goto argp_cleanup;
	}

	result = telemetry_diag_sample_run(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("telemetry_diag_sample_run() encountered an error: %s", doca_error_get_name(result));
		goto free_data_ids;
	}

	exit_status = EXIT_SUCCESS;

free_data_ids:
	free(sample_cfg.data_ids_struct);

argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
