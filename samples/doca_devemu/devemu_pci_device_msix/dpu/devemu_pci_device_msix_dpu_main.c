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

#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_log.h>

#include <devemu_pci_common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_MSIX_DPU::MAIN);

/* Sample's Logic */
doca_error_t devemu_pci_device_msix_dpu(const char *pci_address, const char *emulated_dev_vuid, uint16_t msix_idx);

/* Configuration struct */
struct devemu_pci_cfg {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* device PCI address */
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE];	      /* VUID of emulated device with MSI-X regions */
	uint16_t msix_idx;			      /* Index of MSI-X to raise */
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
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *addr = (char *)param;

	return parse_pci_address(addr, conf->pci_address);
}

/*
 * ARGP Callback - Handle VUID parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vuid_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *vuid = (char *)param;

	return parse_vuid(vuid, conf->vuid);
}

/*
 * ARGP Callback - Handle MSI-X index parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t msix_index_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	int msix = *(int *)param;

	if (msix < 0 || msix >= PCI_TYPE_NUM_MSIX) {
		DOCA_LOG_ERR("Entered MSI-X index must be in [0, %u (PCI_TYPE_NUM_MSIX)) but received %d instead",
			     PCI_TYPE_NUM_MSIX,
			     msix);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->msix_idx = (uint16_t)msix;

	return DOCA_SUCCESS;
}

/*
 * Register MSI-X index command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_msix_index_param(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	/* Create and register PCI address param */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "x");
	doca_argp_param_set_long_name(param, "msix-index");
	doca_argp_param_set_description(
		param,
		"DOCA Devemu device MSI-X vector index. Positive integer that is lower than PCI_TYPE_NUM_MSIX");
	doca_argp_param_set_callback(param, msix_index_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_devemu_pci_params(void)
{
	doca_error_t result;

	result = register_pci_address_param(pci_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_vuid_param(
		"DOCA Devemu emulated device VUID. Sample will use this device to raise MSI-X towards Host",
		vuid_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_msix_index_param();
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
	struct devemu_pci_cfg devemu_pci_cfg;
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values (Example values) */
	strcpy(devemu_pci_cfg.pci_address, "0000:03:00.0");
	strcpy(devemu_pci_cfg.vuid, "");

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
	result = doca_argp_init(NULL, &devemu_pci_cfg);
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

	if (*devemu_pci_cfg.vuid == 0) {
		DOCA_LOG_ERR("The VUID parameter is missing");
		goto argp_cleanup;
	}

	result = devemu_pci_device_msix_dpu(devemu_pci_cfg.pci_address, devemu_pci_cfg.vuid, devemu_pci_cfg.msix_idx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("devemu_pci_device_msix_dpu() encountered an error: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

#else // DOCA_ARCH_DPU
	(void)argc;
	(void)argv;

	DOCA_LOG_ERR("PCI Emulated Device MSI-X DPU can run only on the DPU");
	exit_status = EXIT_FAILURE;

#endif // DOCA_ARCH_DPU

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
