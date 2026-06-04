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

#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_log.h>

#include <devemu_pci_common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_TLP_HANDLER_DPU::MAIN);

/* Max length (including null terminator) for the shared memory directory path */
#define TLP_CHANNEL_SHM_DIR_PATH_MAX 256

/* Configuration struct */
struct devemu_pci_cfg {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* device PCI address */
	bool is_handover_destination; /* true if this instance is the destination in a TLP channel handover */
	char shm_dir_path[TLP_CHANNEL_SHM_DIR_PATH_MAX]; /* shared memory directory path; empty = standalone */
};

/* Sample's Logic */
doca_error_t devemu_pci_device_tlp_handler_dpu(const char *pci_address,
					       bool is_handover_destination,
					       const char *shm_dir_path);

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
 * ARGP Callback - Handle handover destination parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handover_destination_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;

	conf->is_handover_destination = *(bool *)param;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle shared memory directory path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t shm_dir_path_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *path = (const char *)param;

	size_t path_len = strnlen(path, TLP_CHANNEL_SHM_DIR_PATH_MAX);
	if (path_len == TLP_CHANNEL_SHM_DIR_PATH_MAX) {
		DOCA_LOG_ERR("shm-dir-path exceeds max length of %d", TLP_CHANNEL_SHM_DIR_PATH_MAX - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (path_len == 0 || path[0] != '/') {
		DOCA_LOG_ERR("shm-dir-path must be an absolute path (starting with '/')");
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(conf->shm_dir_path, path, path_len + 1);

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
	struct doca_argp_param *handover_destination_param;
	struct doca_argp_param *shm_dir_path_param;

	result = register_pci_address_param(pci_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&handover_destination_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_short_name(handover_destination_param, "d");
	doca_argp_param_set_long_name(handover_destination_param, "handover-destination");
	doca_argp_param_set_description(
		handover_destination_param,
		"Run as a TLP channel handover destination: connect to a source app and take over the TLP channel");
	doca_argp_param_set_callback(handover_destination_param, handover_destination_callback);
	doca_argp_param_set_type(handover_destination_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(handover_destination_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&shm_dir_path_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_short_name(shm_dir_path_param, "s");
	doca_argp_param_set_long_name(shm_dir_path_param, "shm-dir-path");
	doca_argp_param_set_description(
		shm_dir_path_param,
		"Absolute directory path for TLP channel shared memory. "
		"When provided, enables live-upgrade handover (source: exports channel; destination: imports from this path). "
		"When omitted, the sample runs in standalone mode without handover support.");
	doca_argp_param_set_callback(shm_dir_path_param, shm_dir_path_callback);
	doca_argp_param_set_type(shm_dir_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(shm_dir_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

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
	struct devemu_pci_cfg devemu_pci_cfg = {0};
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values (Example values) */
	strncpy(devemu_pci_cfg.pci_address, "0000:03:00.0", sizeof(devemu_pci_cfg.pci_address));
	devemu_pci_cfg.is_handover_destination = false;
	*devemu_pci_cfg.shm_dir_path = '\0';

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_INFO);
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

	if (devemu_pci_cfg.is_handover_destination && *devemu_pci_cfg.shm_dir_path == '\0') {
		DOCA_LOG_ERR("--shm-dir-path must be provided when running as handover destination");
		goto argp_cleanup;
	}

	/* Run sample logic */
	result = devemu_pci_device_tlp_handler_dpu(devemu_pci_cfg.pci_address,
						   devemu_pci_cfg.is_handover_destination,
						   devemu_pci_cfg.shm_dir_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("devemu_pci_device_tlp_handler_dpu() encountered an error: %s",
			     doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

#else // DOCA_ARCH_DPU
	(void)argc;
	(void)argv;

	DOCA_LOG_ERR("PCI Emulated Device TLP Handler DPU can run only on the DPU");
	exit_status = EXIT_FAILURE;

#endif // DOCA_ARCH_DPU

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
