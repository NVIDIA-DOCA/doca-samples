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
#include <signal.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_error.h>

#include "vblk_pci_dev_core.h"
#include "pci_spec_tlp.h"

DOCA_LOG_REGISTER(VBLK_PCI_DEV);

/* Global signal handling */
volatile bool force_quit = false;

/**
 * @brief Signal handler for graceful shutdown
 *
 * Handles SIGINT and SIGTERM signals to allow for clean application termination.
 * Sets the global force_quit flag to signal the main loop to exit gracefully.
 *
 * @param[in] signum Signal number received
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/**
 * @brief ARGP callback for device name parameter
 *
 * Validates and stores the emulation manager device name provided via command line.
 *
 * @param[in] param Input parameter containing device name string
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if name too long
 */
static doca_error_t device_name_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	char *device_name = (char *)param;

	int len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered emulation manager device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(cfg->device_name, device_name, len + 1);
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for number of queues parameter
 *
 * Validates and stores the number of VirtIO queues.
 *
 * @param[in] param Input parameter containing queue count
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if invalid
 */
static doca_error_t num_queues_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	int nq = *(int *)param;

	if (nq < 1 || nq > VBLK_PCI_DEV_MAX_QUEUES_PER_EP) {
		DOCA_LOG_ERR("num_queues must be in range [1, %d]", VBLK_PCI_DEV_MAX_QUEUES_PER_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->num_queues = (uint16_t)nq;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for IO context CPU mask parameter
 *
 * Validates and stores the CPU mask for IO contexts.
 *
 * @param[in] param Input parameter containing CPU mask
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if invalid
 */
static doca_error_t io_ctx_mask_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	int mask = *(int *)param;

	if (mask <= 0 || mask > ((1 << VBLK_PCI_DEV_MAX_CORES) - 1)) {
		DOCA_LOG_ERR("io_ctx_mask must be in [0x1, 0x%x] (max %d cores)",
			     (1 << VBLK_PCI_DEV_MAX_CORES) - 1,
			     VBLK_PCI_DEV_MAX_CORES);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->io_ctx_mask = (uint64_t)mask;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for TLP core index parameter
 *
 * Validates and stores the core index for TLP handling.
 *
 * @param[in] param Input parameter containing core index
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if invalid
 */
static doca_error_t tlp_core_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	int core = *(int *)param;

	if (core < 0 || core >= VBLK_PCI_DEV_MAX_CORES) {
		DOCA_LOG_ERR("tlp_core_idx must be in range [0, %d)", VBLK_PCI_DEV_MAX_CORES);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->tlp_core_idx = (uint8_t)core;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for offload engine core index parameter
 *
 * Validates and stores the core index for offload engine.
 *
 * @param[in] param Input parameter containing core index
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if invalid
 */
static doca_error_t offload_engine_core_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	int core = *(int *)param;

	if (core < 0 || core >= VBLK_PCI_DEV_MAX_CORES) {
		DOCA_LOG_ERR("offload_engine_core_idx must be in range [0, %d)", VBLK_PCI_DEV_MAX_CORES);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->offload_engine_core_idx = (uint8_t)core;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for stats IOS period parameter
 *
 * Validates and stores the stats query period.
 *
 * @param[in] param Input parameter containing period value
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success
 */
static doca_error_t stats_ios_period_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;

	cfg->stats_ios_period = *(uint32_t *)param;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for seg_max parameter
 *
 * @param[in] param Input parameter containing seg_max value
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if invalid
 */
static doca_error_t seg_max_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	uint32_t seg_max = *(uint32_t *)param;

	if (seg_max < 1 || seg_max > VBLK_CTRLS_MAX_SEG_MAX) {
		DOCA_LOG_ERR("seg_max must be in [1, %d]", VBLK_CTRLS_MAX_SEG_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->seg_max = (uint16_t)seg_max;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for indirect descriptor feature toggle
 *
 * @param[in] param Input parameter (boolean)
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success
 */
static doca_error_t indirect_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;

	(void)param;
	cfg->indirect_enabled = true;
	return DOCA_SUCCESS;
}

static doca_error_t num_ep_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	int value = *(int *)param;

	if (value < 1 || value > MAX_NUM_EP) {
		DOCA_LOG_ERR("num-ep must be in range [1, %d]", MAX_NUM_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->num_ep = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t hotplug_mode_callback(void *param, void *config)
{
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	int mode = *(int *)param;

	cfg->hotplug_mode = (mode == 1);
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback to set data path provider
 *
 * Validates and stores the data path provider name provided via command line.
 *
 * @param[in] param Input parameter containing provider name string
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if invalid provider name
 */
static doca_error_t set_provider_callback(void *param, void *config)
{
	bool datapath_on_dpa = true;
	struct vblk_pci_dev_config *cfg = (struct vblk_pci_dev_config *)config;
	char *provider_name = (char *)param;

	if (provider_name) {
		if (!strcmp(provider_name, "DPU")) {
			datapath_on_dpa = false;
		} else if (strcmp(provider_name, "DPA")) {
			DOCA_LOG_ERR("Invalid data path provider value:%s", provider_name);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	cfg->datapath_on_dpa = datapath_on_dpa;

	return DOCA_SUCCESS;
}

/**
 * @brief Register command line parameters for the application
 *
 * Registers all ARGP parameters for VirtIO Block PCI device configuration.
 *
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t register_vblk_pci_dev_params(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	/* Device name parameter (mandatory) */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "d");
	doca_argp_param_set_long_name(param, "emulation-manager");
	doca_argp_param_set_arguments(param, "<mlx5 device name>");
	doca_argp_param_set_description(param, "mlx5 device that manages tlp emulation devices");
	doca_argp_param_set_callback(param, device_name_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(param);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Number of queues parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "q");
	doca_argp_param_set_long_name(param, "num-queues");
	doca_argp_param_set_arguments(param, "<1-255>");
	doca_argp_param_set_description(
		param,
		"Number of virtio queues per endpoint, must be >= IO context count (default: 255)");
	doca_argp_param_set_callback(param, num_queues_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* IO context CPU mask parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "io-ctx-mask");
	doca_argp_param_set_arguments(param, "<mask>");
	doca_argp_param_set_description(
		param,
		"IO contexts CPU mask, up to 15 cores; TLP core must not be included (default: 0x00FF = cores 0-7)");
	doca_argp_param_set_callback(param, io_ctx_mask_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* TLP core parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "tlp-core-idx");
	doca_argp_param_set_arguments(param, "<core>");
	doca_argp_param_set_description(param, "TLP core, must not be in io-ctx-mask (default: 15)");
	doca_argp_param_set_callback(param, tlp_core_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Offload engine core parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "offload-engine-core-idx");
	doca_argp_param_set_arguments(param, "<core>");
	doca_argp_param_set_description(param, "Offload engine core, must be in io-ctx-mask (default: 0)");
	doca_argp_param_set_callback(param, offload_engine_core_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Stats IOS period parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "stats-ios-period");
	doca_argp_param_set_arguments(param, "<ios-period>");
	doca_argp_param_set_description(
		param,
		"Stats are queried after every <ios-period> requests are handled. The default value of 0 means stats are not queried");
	doca_argp_param_set_callback(param, stats_ios_period_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* seg_max parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "seg-max");
	doca_argp_param_set_arguments(param, "<seg_max>");
	doca_argp_param_set_description(
		param,
		"seg_max PCI register value, should be [1, max{queue_size - 2, 128}] (default: 126)");
	doca_argp_param_set_callback(param, seg_max_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Indirect descriptor feature parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "indirect");
	doca_argp_param_set_description(param, "enable use indirect descriptor feature, default: disabled");
	doca_argp_param_set_callback(param, indirect_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Number of endpoints parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "n");
	doca_argp_param_set_long_name(param, "num-ep");
	doca_argp_param_set_arguments(param, "<1-32>");
	doca_argp_param_set_description(
		param,
		"Number of endpoints to create (1-32, default: 1), some host BIOS does not support "
		"large number of endpoints. Suggest to use number <= 20");
	doca_argp_param_set_callback(param, num_ep_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Hotplug mode parameter */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "H");
	doca_argp_param_set_long_name(param, "hotplug-mode");
	doca_argp_param_set_arguments(param, "<0|1>");
	doca_argp_param_set_description(param,
					"Set hotplug mode: 1 for hotplug mode (default), "
					"0 for static mode. In hotplug mode, enter commands to control devices:\n"
					"  plug <DSP_IDX>   - Plug device to DSP slot\n"
					"  unplug <DSP_IDX> - Unplug device from DSP slot\n"
					"Example: plug 0");
	doca_argp_param_set_callback(param, hotplug_mode_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Data Path Provider */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "provider");
	doca_argp_param_set_arguments(param, "<datapath provider name>");
	doca_argp_param_set_description(param, "Set data path provider, {DPA or DPU, default: DPA}");
	doca_argp_param_set_callback(param, set_provider_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * @brief VirtIO Block PCI Device application main function
 *
 * Main entry point for the VirtIO Block PCI device application.
 * Handles argument parsing and delegates to vblk_pci_dev_run() for core logic.
 *
 * @param[in] argc Command line arguments count
 * @param[in] argv Array of command line arguments
 * @return EXIT_SUCCESS on success, EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct vblk_pci_dev_config config = {
		.num_ep = VBLK_PCI_DEV_DEFAULT_NUM_EP,
		.num_queues = VBLK_PCI_DEV_DEFAULT_NUM_QUEUES,
		.seg_max = 126,
		.io_ctx_mask = VBLK_PCI_DEV_DEFAULT_IO_CTX_MASK,
		.tlp_core_idx = VBLK_PCI_DEV_DEFAULT_TLP_CORE_IDX,
		.offload_engine_core_idx = VBLK_PCI_DEV_DEFAULT_OFFLOAD_ENGINE_CORE_IDX,
		.stats_ios_period = 0,
		.datapath_on_dpa = true,
		.hotplug_mode = true,
	};
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	doca_error_t result;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	DOCA_LOG_INFO("Starting DOCA VirtIO Block PCI Device application");

	/* Parse application arguments */
	result = doca_argp_init("doca_vblk_pci_dev", &config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = register_vblk_pci_dev_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register the program parameters: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	/* Setup signal handlers for graceful termination */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	DOCA_LOG_INFO("Configuration:");
	DOCA_LOG_INFO("  Device: %s", config.device_name);
	DOCA_LOG_INFO("  Endpoints: %u", config.num_ep);
	DOCA_LOG_INFO("  Queues/EP: %u", config.num_queues);
	DOCA_LOG_INFO("  Hotplug: %s", config.hotplug_mode ? "enabled" : "disabled");
	DOCA_LOG_INFO("  IO Context Mask: 0x%lx", config.io_ctx_mask);
	DOCA_LOG_INFO("  TLP Core: %u", config.tlp_core_idx);
	DOCA_LOG_INFO("  Offload Engine Core: %u", config.offload_engine_core_idx);
	DOCA_LOG_INFO("Runtime commands (enter via stdin):");
	DOCA_LOG_INFO("  cap <GB>  - Set block device capacity in GB (e.g. cap 1)");
	DOCA_LOG_INFO(" Datapath Provider: %s", config.datapath_on_dpa ? "DPA" : "DPU");

	/* Run the core VirtIO Block PCI device logic */
	result = vblk_pci_dev_run(&config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("VirtIO Block PCI device failed: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	exit_status = EXIT_SUCCESS;

destroy_argp:
	doca_argp_destroy();
	return exit_status;
}
