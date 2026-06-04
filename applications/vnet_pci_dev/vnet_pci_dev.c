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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sched.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_error.h>

#include <utils.h>

#include "vnet_pci_dev_core.h"
#include "vnet_pci_dev_lu.h"

DOCA_LOG_REGISTER(VNET_PCI_DEV);

/* Global signal handling */
static volatile bool force_quit = false;

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
 * @brief ARGP callback for PCI address parameter
 *
 * Validates and stores the PCI address provided via command line arguments.
 * The PCI address must be in standard format (e.g., "0000:03:00.0").
 *
 * @param[in] param Input parameter containing PCI address string
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if address too long
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	const char *addr = param;

	if (strlen(addr) >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(vnet_cfg->pci_address, addr, DOCA_DEVINFO_PCI_ADDR_SIZE);
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for IB device name parameter
 *
 * @param[in] param IB device name string
 * @param[in] config Application configuration
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if name too long
 */
static doca_error_t ibdev_name_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	const char *name = param;

	if (strlen(name) >= DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("IB device name exceeding the maximum size of %d", DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(vnet_cfg->ibdev_name, name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for MAC address parameter
 *
 * Validates and stores the MAC address provided via command line arguments.
 * The MAC address must be in standard format (e.g., "52:54:00:12:34:56").
 *
 * @param[in] param Input parameter containing MAC address string
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if address too long
 */
static doca_error_t mac_address_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	const char *mac = param;

	if (strlen(mac) >= sizeof(vnet_cfg->mac_addr)) {
		DOCA_LOG_ERR("Entered MAC address exceeding the maximum size of %zu", sizeof(vnet_cfg->mac_addr) - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(vnet_cfg->mac_addr, mac, sizeof(vnet_cfg->mac_addr));
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for MTU parameter
 *
 * Validates and stores the Maximum Transmission Unit (MTU) value provided
 * via command line arguments. MTU must be within valid range.
 *
 * @param[in] param Input parameter containing MTU value
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if MTU invalid
 */
static doca_error_t mtu_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	int mtu = *(int *)param;

	if (mtu < 68 || mtu > 9000) {
		DOCA_LOG_ERR("MTU must be between 68 and 9000");
		return DOCA_ERROR_INVALID_VALUE;
	}

	vnet_cfg->mtu = mtu;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for network speed parameter
 *
 * Validates and stores the network link speed provided via command line.
 * Speed must be a positive value representing Mbps (Megabits per second).
 *
 * @param[in] param Input parameter containing speed value in Mbps
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if speed invalid
 *
 * @note Common values: 10, 100, 1000, 10000 Mbps
 */
static doca_error_t speed_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	int speed = *(int *)param;

	if (speed <= 0) {
		DOCA_LOG_ERR("Speed must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	vnet_cfg->speed = speed;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for duplex mode parameter
 *
 * Validates and stores the network duplex mode provided via command line.
 * Supports half-duplex (0) and full-duplex (1) modes.
 *
 * @param[in] param Input parameter containing duplex mode (0 or 1)
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if mode invalid
 *
 * @note 0 = half-duplex, 1 = full-duplex (recommended)
 */
static doca_error_t duplex_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	int duplex = *(int *)param;

	if (duplex != 0 && duplex != 1) {
		DOCA_LOG_ERR("Duplex must be 0 (half) or 1 (full)");
		return DOCA_ERROR_INVALID_VALUE;
	}

	vnet_cfg->duplex = duplex;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle maximum queue pairs parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t max_queue_pairs_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	int max_queue_pairs = *(int *)param;

	if (max_queue_pairs < 1 || max_queue_pairs > VNET_CTRL_MAX_QUEUES_PAIRS) {
		DOCA_LOG_ERR("Maximum queue pairs must be between 1 and %u", VNET_CTRL_MAX_QUEUES_PAIRS);
		return DOCA_ERROR_INVALID_VALUE;
	}

	vnet_cfg->max_queue_pairs = max_queue_pairs;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP callback for VirtQueue size parameter
 *
 * Validates and stores the VirtQueue size (number of entries per queue).
 * Size must be a power of 2, between 16 and 32768 (VirtIO spec limit).
 *
 * @param[in] param Input parameter containing queue size value
 * @param[in,out] config Program configuration context to update
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if size invalid
 */
static doca_error_t queue_size_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *vnet_cfg = config;
	int queue_size = *(int *)param;

	/* VirtIO spec: queue size must be power of 2, max 32768 */
	if (queue_size < VNET_MIN_QUEUE_SIZE || queue_size > VNET_MAX_QUEUE_SIZE) {
		DOCA_LOG_ERR("Queue size must be between %u and %u", VNET_MIN_QUEUE_SIZE, VNET_MAX_QUEUE_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check if power of 2 */
	if ((queue_size & (queue_size - 1)) != 0) {
		DOCA_LOG_ERR("Queue size must be a power of 2 (e.g., 256, 512, 1024)");
		return DOCA_ERROR_INVALID_VALUE;
	}

	vnet_cfg->queue_size = queue_size;
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
	struct vnet_pci_dev_config *vnet_cfg = (struct vnet_pci_dev_config *)config;
	int value = *(int *)param;

	if (value < 1 || value > MAX_NUM_EP) {
		DOCA_LOG_ERR("Invalid num_ep value: %d. Must be between 1 and MAX_NUM_EP(%d)", value, MAX_NUM_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}
	vnet_cfg->num_ep = (uint32_t)value;
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
	struct vnet_pci_dev_config *conf = (struct vnet_pci_dev_config *)config;
	int mode = *(int *)param;
	if (mode != 0 && mode != 1) {
		DOCA_LOG_ERR("Invalid hotplug mode value: %d. Must be 0 (static) or 1 (hotplug)", mode);
		return DOCA_ERROR_INVALID_VALUE;
	}
	conf->hotplug_mode = (mode == 1);
	return DOCA_SUCCESS;
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
		"Number of endpoints to create (1-32, default: 1), some host BIOS does not support large number of endpoints. Suggest to use number <= 20");
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

	doca_argp_param_set_short_name(param, "H");
	doca_argp_param_set_long_name(param, "hotplug-mode");
	doca_argp_param_set_description(
		param,
		"Set hotplug mode: 0 for static mode (all EPs created at startup), 1 for hotplug mode (default). "
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

static doca_error_t tlp_core_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *conf = (struct vnet_pci_dev_config *)config;
	int core = *(int *)param;

	if (core < -1 || core >= CPU_SETSIZE) {
		DOCA_LOG_ERR("Invalid TLP core index: %d. Use -1 for auto or 0..%d", core, CPU_SETSIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->tlp_core_idx = core;
	return DOCA_SUCCESS;
}

static doca_error_t worker_core_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *conf = (struct vnet_pci_dev_config *)config;
	int core = *(int *)param;

	if (core < -1 || core >= CPU_SETSIZE) {
		DOCA_LOG_ERR("Invalid worker core index: %d. Use -1 for auto or 0..%d", core, CPU_SETSIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->worker_core_idx = core;
	return DOCA_SUCCESS;
}

static doca_error_t mq_core_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *conf = (struct vnet_pci_dev_config *)config;
	int core = *(int *)param;

	if (core < -1 || core >= CPU_SETSIZE) {
		DOCA_LOG_ERR("Invalid MQ core index: %d. Use -1 for auto or 0..%d", core, CPU_SETSIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->mq_core_idx = core;
	return DOCA_SUCCESS;
}

/*
 * Live update mode callback
 *
 * @param [in]: parameter value (string: "none", "active", "standby")
 * @config [in/out]: application configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vnet_lu_mode_callback(void *param, void *config)
{
	struct vnet_pci_dev_config *app_config = (struct vnet_pci_dev_config *)config;
	const char *mode_str = (const char *)param;

	if (strcmp(mode_str, "none") == 0)
		app_config->vnet_lu_mode = VNET_LU_MODE_NONE;
	else if (strcmp(mode_str, "active") == 0)
		app_config->vnet_lu_mode = VNET_LU_MODE_ACTIVE;
	else if (strcmp(mode_str, "standby") == 0)
		app_config->vnet_lu_mode = VNET_LU_MODE_STANDBY;
	else {
		DOCA_LOG_ERR("Invalid --lu-mode value: '%s' (expected: none, active, standby)", mode_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the application
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_vnet_pci_dev_params(void)
{
	struct doca_argp_param *pci_param, *ibdev_param, *mac_param, *mtu_param, *speed_param, *duplex_param,
		*queues_param, *queue_size_param, *tlp_core_param, *worker_core_param, *mq_core_param;
	doca_error_t result;

	/* Create and register PCI address parameter */
	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI address (default: 0000:03:00.0)");
	doca_argp_param_set_callback(pci_param, pci_address_callback);
	doca_argp_param_set_type(pci_param, DOCA_ARGP_TYPE_STRING);
	/* PCI address is now optional with default value */
	result = doca_argp_register_param(pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register IB device name parameter */
	result = doca_argp_param_create(&ibdev_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(ibdev_param, "d");
	doca_argp_param_set_long_name(ibdev_param, "ibdev-name");
	doca_argp_param_set_description(ibdev_param, "IB device name (e.g., mlx5_bond_0_pci_dev). Overrides -p");
	doca_argp_param_set_callback(ibdev_param, ibdev_name_callback);
	doca_argp_param_set_type(ibdev_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(ibdev_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register MAC address parameter */
	result = doca_argp_param_create(&mac_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(mac_param, "m");
	doca_argp_param_set_long_name(mac_param, "mac-addr");
	doca_argp_param_set_description(mac_param, "MAC address for the vnet device (default: 52:54:00:12:34:56)");
	doca_argp_param_set_callback(mac_param, mac_address_callback);
	doca_argp_param_set_type(mac_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(mac_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register MTU parameter */
	result = doca_argp_param_create(&mtu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(mtu_param, "t");
	doca_argp_param_set_long_name(mtu_param, "mtu");
	doca_argp_param_set_description(mtu_param, "MTU size (default: 1500, range: 68-9000)");
	doca_argp_param_set_callback(mtu_param, mtu_callback);
	doca_argp_param_set_type(mtu_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(mtu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register speed parameter */
	result = doca_argp_param_create(&speed_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(speed_param, "s");
	doca_argp_param_set_long_name(speed_param, "speed");
	doca_argp_param_set_description(speed_param, "Link speed in Mbps (default: 100000)");
	doca_argp_param_set_callback(speed_param, speed_callback);
	doca_argp_param_set_type(speed_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(speed_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register duplex parameter */
	result = doca_argp_param_create(&duplex_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(duplex_param, "D");
	doca_argp_param_set_long_name(duplex_param, "duplex");
	doca_argp_param_set_description(duplex_param, "Duplex mode: 0=half, 1=full (default: 1)");
	doca_argp_param_set_callback(duplex_param, duplex_callback);
	doca_argp_param_set_type(duplex_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(duplex_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of queues parameter */
	result = doca_argp_param_create(&queues_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(queues_param, "q");
	doca_argp_param_set_long_name(queues_param, "max-queue-pairs");
	doca_argp_param_set_description(queues_param, "Maximum queue pairs (supported by the device)");
	doca_argp_param_set_callback(queues_param, max_queue_pairs_callback);
	doca_argp_param_set_type(queues_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(queues_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register VirtQueue size parameter */
	result = doca_argp_param_create(&queue_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(queue_size_param, "z");
	doca_argp_param_set_long_name(queue_size_param, "queue-size");
	doca_argp_param_set_description(queue_size_param,
					"VirtQueue size in entries (power of 2, 16-4096, default: 1024)");
	doca_argp_param_set_callback(queue_size_param, queue_size_callback);
	doca_argp_param_set_type(queue_size_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(queue_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_num_ep();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_hotplug_mode();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&tlp_core_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(tlp_core_param, "tlp-core");
	doca_argp_param_set_description(tlp_core_param, "CPU core for TLP progress thread (-1 = auto, default)");
	doca_argp_param_set_callback(tlp_core_param, tlp_core_callback);
	doca_argp_param_set_type(tlp_core_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(tlp_core_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&worker_core_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(worker_core_param, "worker-core");
	doca_argp_param_set_description(worker_core_param,
					"CPU core for PCI config worker thread (-1 = auto, default)");
	doca_argp_param_set_callback(worker_core_param, worker_core_callback);
	doca_argp_param_set_type(worker_core_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(worker_core_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&mq_core_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(mq_core_param, "mq-core");
	doca_argp_param_set_description(mq_core_param,
					"CPU core for MQ start threads (-1 = auto single core, default)");
	doca_argp_param_set_callback(mq_core_param, mq_core_callback);
	doca_argp_param_set_type(mq_core_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(mq_core_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register live update mode parameter */
	{
		struct doca_argp_param *lu_param;

		result = doca_argp_param_create(&lu_param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_long_name(lu_param, "lu-mode");
		doca_argp_param_set_description(lu_param, "Live update mode: none, active, standby (default: none)");
		doca_argp_param_set_callback(lu_param, vnet_lu_mode_callback);
		doca_argp_param_set_type(lu_param, DOCA_ARGP_TYPE_STRING);
		result = doca_argp_register_param(lu_param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * VirtIO Net PCI Device application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct vnet_pci_dev_config config = {.pci_address = "0000:03:00.0",   /* Default PCI address (full format) */
					     .ibdev_name = "",		      /* Empty = use pci_address */
					     .mac_addr = "52:54:00:12:34:56", /* Default MAC */
					     .mtu = 1500,
					     .speed = 100000,			      /* 100 Gbps for Gen5 */
					     .duplex = 1,			      /* Full duplex */
					     .max_queue_pairs = VNET_MAX_QUEUE_PAIRS, /* Default Max QPs */
					     .queue_size = VNET_DEFAULT_QUEUE_SIZE,   /* Default queue size */
					     .num_ep = 1,			/* Default Number of Endpoints (1) */
					     .hotplug_mode = true,		/* Default: hotplug mode */
					     .vnet_lu_mode = VNET_LU_MODE_NONE, /* Default LU Mode (none) */
					     .tlp_core_idx = -1,		/* Auto affinity */
					     .worker_core_idx = -1,		/* Auto affinity */
					     .mq_core_idx = -1};		/* Auto single-core affinity */
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

	DOCA_LOG_INFO("Starting DOCA VirtIO Net PCI Device application");

	/* Parse application arguments */
	result = doca_argp_init("doca_vnet_pci_dev", &config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = register_vnet_pci_dev_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register the program parameters: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	if (config.tlp_core_idx >= 0 && config.tlp_core_idx == config.worker_core_idx) {
		DOCA_LOG_ERR("tlp-core (%d) must not equal worker-core (%d)",
			     config.tlp_core_idx,
			     config.worker_core_idx);
		goto destroy_argp;
	}
	if (config.mq_core_idx >= 0 && config.mq_core_idx == config.tlp_core_idx) {
		DOCA_LOG_ERR("mq-core (%d) must not equal tlp-core (%d)", config.mq_core_idx, config.tlp_core_idx);
		goto destroy_argp;
	}

	/* PCI address is now set with default value "03:00.0" */

	/* Setup signal handlers for graceful termination */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	DOCA_LOG_INFO("Configuration:");
	if (config.ibdev_name[0] != '\0')
		DOCA_LOG_INFO("  IB Device: %s (overrides PCI address)", config.ibdev_name);
	else
		DOCA_LOG_INFO("  PCI Address: %s", config.pci_address);
	DOCA_LOG_INFO("  MAC Address: %s", config.mac_addr);
	DOCA_LOG_INFO("  MTU: %u", config.mtu);
	DOCA_LOG_INFO("  Speed: %u Mbps", config.speed);
	DOCA_LOG_INFO("  Duplex: %s", config.duplex ? "Full" : "Half");
	DOCA_LOG_INFO("  Max Queue Pairs: %u (Total VQs: %u)", config.max_queue_pairs, config.max_queue_pairs * 2 + 1);
	DOCA_LOG_INFO("  Queue Size: %u entries", config.queue_size);
	DOCA_LOG_INFO("  Number of Endpoints: %u", config.num_ep);
	DOCA_LOG_INFO("  Hotplug Mode: %s", config.hotplug_mode ? "Hotplug" : "Static");
	if (config.tlp_core_idx < 0)
		DOCA_LOG_INFO("  TLP Core: auto");
	else
		DOCA_LOG_INFO("  TLP Core: %d", config.tlp_core_idx);
	if (config.worker_core_idx < 0)
		DOCA_LOG_INFO("  Worker Core: auto");
	else
		DOCA_LOG_INFO("  Worker Core: %d", config.worker_core_idx);
	if (config.mq_core_idx < 0)
		DOCA_LOG_INFO("  MQ Core: auto (single CPU)");
	else
		DOCA_LOG_INFO("  MQ Core: %d", config.mq_core_idx);
	DOCA_LOG_INFO("  LU Mode: %s",
		      config.vnet_lu_mode == VNET_LU_MODE_ACTIVE  ? "active" :
		      config.vnet_lu_mode == VNET_LU_MODE_STANDBY ? "standby" :
								    "none");

	/* Run the core VirtIO Net PCI device logic */
	result = vnet_pci_dev_run(&config, &force_quit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("VirtIO Net PCI device failed: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	exit_status = EXIT_SUCCESS;

destroy_argp:
	doca_argp_destroy();
	return exit_status;
}
