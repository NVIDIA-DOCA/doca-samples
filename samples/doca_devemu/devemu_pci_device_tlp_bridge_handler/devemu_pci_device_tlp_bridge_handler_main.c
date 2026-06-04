/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_log.h>

#include <devemu_pci_common.h>
#include "devemu_pci_device_tlp_bridge_handler_config.h"

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_TLP_BRIDGE_HANDLER_DPU::MAIN);

/* Sample's Logic */
doca_error_t devemu_pci_tlp_bridge_handler_dpu(const char *pci_address,
					       uint32_t num_dev_types,
					       uint32_t num_ep,
					       bool hotplug_mode);

/* Configuration struct */
struct bridge_multi_pf_config {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Device PCI address */
	uint32_t num_dev_types;			      /* Number of device types to create (1-8) */
	uint32_t num_ep;			      /* Number of endpoints to create (1-32) */
	bool hotplug_mode;			      /* Hotplug mode: 1 for hotplug, 0 for static (default) */
};

#ifdef DOCA_ARCH_DPU

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_callback(void *param, void *config)
{
	struct bridge_multi_pf_config *conf = (struct bridge_multi_pf_config *)config;
	const char *addr = (char *)param;

	return parse_pci_address(addr, conf->pci_address);
}

/*
 * Callback for parsing num_dev_types parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Sample configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_dev_types_callback(void *param, void *config)
{
	struct bridge_multi_pf_config *conf = (struct bridge_multi_pf_config *)config;
	int value = *(int *)param;

	if (value < MIN_TLP_PCI_TYPE_NUM || value > MAX_TLP_PCI_TYPE_NUM) {
		DOCA_LOG_ERR("Invalid num_dev_types value: %d. Must be between %d and %d",
			     value,
			     MIN_TLP_PCI_TYPE_NUM,
			     MAX_TLP_PCI_TYPE_NUM);
		return DOCA_ERROR_INVALID_VALUE;
	}
	conf->num_dev_types = (uint32_t)value;
	return DOCA_SUCCESS;
}

/*
 * Callback for parsing num_ep parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Sample configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_ep_callback(void *param, void *config)
{
	struct bridge_multi_pf_config *conf = (struct bridge_multi_pf_config *)config;
	int value = *(int *)param;

	if (value < 1 || value > MAX_NUM_EP) {
		DOCA_LOG_ERR("Invalid num_ep value: %d. Must be between 1 and MAX_NUM_EP(%d)", value, MAX_NUM_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}
	conf->num_ep = (uint32_t)value;
	return DOCA_SUCCESS;
}

/*
 * Callback for parsing hotplug_mode parameter
 *
 * @param [in]: Input parameter (integer: 0 for static mode, 1 for hotplug mode)
 * @config [out]: Sample configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hotplug_mode_callback(void *param, void *config)
{
	struct bridge_multi_pf_config *conf = (struct bridge_multi_pf_config *)config;
	int mode = *(int *)param;
	if (mode != 0 && mode != 1) {
		DOCA_LOG_ERR("Invalid hotplug mode value: %d. Must be 0 (static) or 1 (hotplug)", mode);
		return DOCA_ERROR_INVALID_VALUE;
	}
	conf->hotplug_mode = (mode == 1);
	return DOCA_SUCCESS;
}

/*
 * Register num_dev_types command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_num_dev_types(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(param, "t");
	doca_argp_param_set_long_name(param, "num-dev-types");
	doca_argp_param_set_description(param, "Number of device types to create (1-8, default: 1)");
	doca_argp_param_set_callback(param, num_dev_types_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);

	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));

	return result;
}

/*
 * Register num_ep command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_num_ep(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(param, "n");
	doca_argp_param_set_long_name(param, "num-ep");
	doca_argp_param_set_description(
		param,
		"Number of endpoints to create (1-32, default: 4), some host BIOS does not support large number of endpoints. Suggest to use number <= 20");
	doca_argp_param_set_callback(param, num_ep_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);

	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));

	return result;
}

/*
 * Register hotplug_mode command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hotplug_mode(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(param, "m");
	doca_argp_param_set_long_name(param, "hotplug-mode");
	doca_argp_param_set_description(
		param,
		"Set hotplug mode: 1 to enable hotplug mode for dynamic EP creation, 0 for static mode (default). "
		"In hotplug mode, enter commands to control devices:\n"
		"  plug <DSP_IDX>   - Plug device to DSP slot\n"
		"  unplug <DSP_IDX> - Unplug device from DSP slot\n"
		"Example: plug 0");
	doca_argp_param_set_callback(param, hotplug_mode_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);

	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));

	return result;
}

/*
 * Register all command line parameters for the sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_devemu_pci_params(void)
{
	doca_error_t result;

	result = register_pci_address_param(pci_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_num_dev_types();
	if (result != DOCA_SUCCESS)
		return result;

	result = register_num_ep();
	if (result != DOCA_SUCCESS)
		return result;

	result = register_hotplug_mode();
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

#endif // DOCA_ARCH_DPU

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct bridge_multi_pf_config cfg;
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values */
	strncpy(cfg.pci_address, "0000:03:00.0", sizeof(cfg.pci_address));
	cfg.num_ep = 4;
	cfg.hotplug_mode = false;
	cfg.num_dev_types = MIN_TLP_PCI_TYPE_NUM;

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

#ifdef DOCA_ARCH_DPU
	result = doca_argp_init("doca_devemu_pci_tlp_bridge_handler", &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_devemu_pci_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample command line parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = devemu_pci_tlp_bridge_handler_dpu(cfg.pci_address, cfg.num_dev_types, cfg.num_ep, cfg.hotplug_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("devemu_pci_tlp_bridge_handler_dpu() encountered an error: %s",
			     doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

#else // DOCA_ARCH_DPU
	(void)argc;
	(void)argv;

	DOCA_LOG_ERR("PCI Emulated Device TLP Bridge Handler DPU can run only on the DPU");
	exit_status = EXIT_FAILURE;

#endif // DOCA_ARCH_DPU

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
