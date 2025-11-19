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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_telemetry_adp_retx.h>
#include "telemetry_adp_retx_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_ADP_RETX::MAIN);

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(sample_cfg->pci_addr, pci_address, len + 1);
	sample_cfg->pci_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle time_unit parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t time_unit_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	char *time_unit = (char *)param;

	if (strcmp(time_unit, "nsec") == 0)
		sample_cfg->time_unit = DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_NSEC;
	else if (strcmp(time_unit, "usec") == 0)
		sample_cfg->time_unit = DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC;
	else if (strcmp(time_unit, "usec_100") == 0)
		sample_cfg->time_unit = DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC_100;
	else if (strcmp(time_unit, "msec") == 0)
		sample_cfg->time_unit = DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_MSEC;
	else
		return DOCA_ERROR_INVALID_VALUE;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle bin_width_mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t width_mode_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	char *mode = (char *)param;

	if (strcmp(mode, "fixed") == 0)
		sample_cfg->bin_width_mode = DOCA_TELEMETRY_ADP_RETX_HIST_BIN_WIDTH_MODE_FIXED;
	else if (strcmp(mode, "double") == 0)
		sample_cfg->bin_width_mode = DOCA_TELEMETRY_ADP_RETX_HIST_BIN_WIDTH_MODE_DOUBLE;
	else
		return DOCA_ERROR_INVALID_VALUE;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle bin_num parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t bin_num_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	uint32_t *num = (uint32_t *)param;

	sample_cfg->bin_num = *num;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle vhca id parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vhca_id_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	uint32_t *id = (uint32_t *)param;

	/* vhca id is a 16 bit value - higher values are reserved */
	if (*id > 0xFFFF) {
		DOCA_LOG_ERR("VHCA_ID exceeded max value of 0xFFFF");
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->vhca_id = *id;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle bin0 width parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t bin0_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	uint32_t *width = (uint32_t *)param;

	/* bin width is a 16 bit value */
	if (*width > 0xFFFF) {
		DOCA_LOG_ERR("Bin width exceeds max value of 0xFFFF");
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->bin0_width = *width;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle bin1 width parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t bin1_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	uint32_t *width = (uint32_t *)param;

	/* bin width is a 16 bit value */
	if (*width > 0xFFFF) {
		DOCA_LOG_ERR("Bin width exceeds max value of 0xFFFF");
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->bin1_width = *width;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle clear on read parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t clear_on_read_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	bool clear_on_read = *(bool *)param;

	sample_cfg->clear_on_read = !!clear_on_read;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle wait time parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t wait_time_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	uint32_t *time = (uint32_t *)param;

	sample_cfg->wait_time = *time;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle number of reads parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_reads_callback(void *param, void *config)
{
	struct telemetry_adp_retx_sample_cfg *sample_cfg = (struct telemetry_adp_retx_sample_cfg *)config;
	uint32_t *reads = (uint32_t *)param;

	sample_cfg->num_reads = *reads;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_telemetry_adp_retx_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *time_unit_param, *bin_width_mode_param, *bin_num_param, *vhca_id_param,
		*bin0_param, *bin1_param, *wait_time_param, *num_reads_param, *clear_on_read_param;

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

	result = doca_argp_param_create(&time_unit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(time_unit_param, "u");
	doca_argp_param_set_long_name(time_unit_param, "time-unit");
	doca_argp_param_set_description(time_unit_param, "Time unit to use - 'nsec', 'usec', 'usec_100', or 'msec'");
	doca_argp_param_set_callback(time_unit_param, time_unit_callback);
	doca_argp_param_set_type(time_unit_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(time_unit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&bin_width_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(bin_width_mode_param, "w");
	doca_argp_param_set_long_name(bin_width_mode_param, "width-mode");
	doca_argp_param_set_description(bin_width_mode_param, "Bin width mode to use - 'fixed', or 'double'");
	doca_argp_param_set_callback(bin_width_mode_param, width_mode_callback);
	doca_argp_param_set_type(bin_width_mode_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(bin_width_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&bin_num_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(bin_num_param, "n");
	doca_argp_param_set_long_name(bin_num_param, "number-bins");
	doca_argp_param_set_description(bin_num_param, "The number of bins to configure the histogram for");
	doca_argp_param_set_callback(bin_num_param, bin_num_callback);
	doca_argp_param_set_type(bin_num_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(bin_num_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&vhca_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(vhca_id_param, "vid");
	doca_argp_param_set_long_name(vhca_id_param, "vhca-id");
	doca_argp_param_set_description(vhca_id_param, "VHCA ID to get histogram events from");
	doca_argp_param_set_callback(vhca_id_param, vhca_id_callback);
	doca_argp_param_set_type(vhca_id_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(vhca_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&bin0_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(bin0_param, "b0");
	doca_argp_param_set_long_name(bin0_param, "bin-0-width");
	doca_argp_param_set_description(bin0_param, "Width of bin 0 to configure histogram");
	doca_argp_param_set_callback(bin0_param, bin0_callback);
	doca_argp_param_set_type(bin0_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(bin0_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&bin1_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(bin1_param, "b1");
	doca_argp_param_set_long_name(bin1_param, "bin-1-width");
	doca_argp_param_set_description(bin1_param, "Width of bin 1 to configure histogram");
	doca_argp_param_set_callback(bin1_param, bin1_callback);
	doca_argp_param_set_type(bin1_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(bin1_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&clear_on_read_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(clear_on_read_param, "c");
	doca_argp_param_set_long_name(clear_on_read_param, "clear-on-read");
	doca_argp_param_set_description(clear_on_read_param, "Reset histogram after each read");
	doca_argp_param_set_callback(clear_on_read_param, clear_on_read_callback);
	doca_argp_param_set_type(clear_on_read_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(clear_on_read_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&wait_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(wait_time_param, "t");
	doca_argp_param_set_long_name(wait_time_param, "wait-time");
	doca_argp_param_set_description(wait_time_param, "Time in seconds to wait between reading histogram bins");
	doca_argp_param_set_callback(wait_time_param, wait_time_callback);
	doca_argp_param_set_type(wait_time_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(wait_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&num_reads_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(num_reads_param, "r");
	doca_argp_param_set_long_name(num_reads_param, "num-reads");
	doca_argp_param_set_description(num_reads_param, "Number of times to read histogram bins");
	doca_argp_param_set_callback(num_reads_param, num_reads_callback);
	doca_argp_param_set_type(num_reads_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_reads_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Set the default parameters to be used in the sample.
 *
 * @cfg [in]: the sample configuration
 */
static void set_default_params(struct telemetry_adp_retx_sample_cfg *cfg)
{
	cfg->time_unit = DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC;
	cfg->bin_width_mode = DOCA_TELEMETRY_ADP_RETX_HIST_BIN_WIDTH_MODE_FIXED;
	cfg->bin_num = 2;
	cfg->vhca_id = NO_VHCA_ID;
	cfg->bin0_width = 10;
	cfg->bin1_width = 10;
	cfg->clear_on_read = false;
	cfg->wait_time = 3;
	cfg->num_reads = 1;
};

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
	struct telemetry_adp_retx_sample_cfg sample_cfg = {};
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

	result = register_telemetry_adp_retx_params();
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

	result = telemetry_adp_retx_sample_run(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("telemetry_adp_retx_sample_run() encountered an error: %s", doca_error_get_name(result));
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
