/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

#include <rte_dev.h>
#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_rdma_bridge.h>

#include <flow_common.h>
#include <flow_switch_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_SWITCH::MAIN);

/* Sample's Logic */
doca_error_t flow_switch(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx);

/* Maximum length for device arguments string */
#define MAX_DEVARGS_LEN 1024

/* Number of DPDK PF ports to configure initially */
#define NB_DPDK_PF_PORTS 1

/* Total number of DOCA Flow ports (1 PF + 2 representors) */
#define NB_DOCA_FLOW_PORTS 3

/* Port to device mapping entry */
struct port_dev_mapping {
	struct doca_dev *dev; /* DOCA device */
	bool valid;	      /* Is this entry valid? */
};

/* Global port to device mapping table */
static struct port_dev_mapping port_to_dev[RTE_MAX_ETHPORTS] = {0};

/*
 * Add port to device mapping
 *
 * @port_id [in]: DPDK port ID
 * @dev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_port_dev_mapping(uint16_t port_id, struct doca_dev *dev)
{
	if (port_id >= RTE_MAX_ETHPORTS) {
		DOCA_LOG_ERR("Port ID %u exceeds maximum %d", port_id, RTE_MAX_ETHPORTS);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (port_to_dev[port_id].valid) {
		DOCA_LOG_WARN("Port ID %u mapping already exists, overwriting", port_id);
	}

	port_to_dev[port_id].dev = dev;
	port_to_dev[port_id].valid = true;

	DOCA_LOG_DBG("Mapped DPDK port %u to DOCA device %p", port_id, dev);

	return DOCA_SUCCESS;
}

/*
 * Find and map DPDK port by matching the PCI address
 *
 * @pci_addr [in]: PCI address of the device (e.g., "0000:08:00.0")
 * @dev [in]: DOCA device associated with the port
 * @return: Port ID on success, or negative value if not found
 */
static int find_port_by_pci_addr(const char *pci_addr, struct doca_dev *dev)
{
	struct rte_eth_dev_info dev_info;
	const struct rte_devargs *devargs;
	doca_error_t result;
	int ret;

	/* Scan all valid DPDK ports to find the one matching the PCI address */
	for (uint16_t port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
		if (!rte_eth_dev_is_valid_port(port_id))
			continue;

		ret = rte_eth_dev_info_get(port_id, &dev_info);
		if (ret < 0) {
			DOCA_LOG_WARN("Failed to get info for port %u: %s", port_id, strerror(-ret));
			continue;
		}

		if (!dev_info.device)
			continue;

		devargs = rte_dev_devargs(dev_info.device);
		if (!devargs)
			continue;

		/* Match port by comparing the PCI address (devargs->name) */
		if (strcmp(devargs->name, pci_addr) == 0) {
			result = add_port_dev_mapping(port_id, dev);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_WARN("Failed to add port mapping for port %u", port_id);
				return -1;
			}
			DOCA_LOG_INFO("Found port %u for PCI address %s", port_id, pci_addr);
			return port_id;
		}
	}

	DOCA_LOG_WARN("No port found for PCI address: %s", pci_addr);
	return -1;
}

/*
 * Probe DPDK device with extended devargs
 *
 * @pci_addr [in]: PCI address of the device (e.g., "0000:08:00.0")
 * @base_devargs [in]: Base device arguments string
 * @dev [in]: Already opened DOCA device to use for probing
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t probe_device_with_devargs(const char *pci_addr, const char *base_devargs, struct doca_dev *dev)
{
	int len;
	int port_id = -1;
	doca_error_t result;
	int dup_cmd_fd = -1;
	struct ibv_pd *pd = NULL;
	char devargs[MAX_DEVARGS_LEN] = {0};

	if (!pci_addr || !base_devargs || !dev) {
		DOCA_LOG_ERR("Invalid parameters for device probe");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Get protection domain from device */
	result = doca_rdma_bridge_get_dev_pd(dev, &pd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PD for device %s: %s", pci_addr, doca_error_get_descr(result));
		return result;
	}

	/* Duplicate cmd_fd for this probe */
	dup_cmd_fd = dup(pd->context->cmd_fd);
	if (dup_cmd_fd < 0) {
		DOCA_LOG_ERR("Failed to duplicate cmd_fd for device %s", pci_addr);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	/* Construct complete device arguments string */
	len = snprintf(devargs,
		       sizeof(devargs),
		       "%s,pd_handle=%u,cmd_fd=%d,%s",
		       pci_addr,
		       pd->handle,
		       dup_cmd_fd,
		       base_devargs);
	if (len < 0 || len >= (int)sizeof(devargs)) {
		DOCA_LOG_ERR("Device arguments string too long or formatting error");
		result = DOCA_ERROR_INVALID_VALUE;
		goto cleanup_dup_fd;
	}

	DOCA_LOG_INFO("Probing device with devargs: %s", devargs);

	/* Probe the device */
	if (rte_dev_probe(devargs) != 0) {
		DOCA_LOG_ERR("Failed to probe device: %s", devargs);
		result = DOCA_ERROR_DRIVER;
		goto cleanup_dup_fd;
	}

	/* Find and map the port by matching the PCI address */
	port_id = find_port_by_pci_addr(pci_addr, dev);
	if (port_id < 0) {
		DOCA_LOG_ERR("Failed to find port for PCI address: %s", pci_addr);
		if (rte_eal_hotplug_remove("pci", pci_addr) != 0)
			DOCA_LOG_WARN("Failed to remove device %s during error cleanup", pci_addr);
		return DOCA_ERROR_NOT_FOUND;
	}

	DOCA_LOG_INFO("Device probed successfully, found port %d", port_id);

	return DOCA_SUCCESS;

cleanup_dup_fd:
	if (dup_cmd_fd >= 0)
		(void)close(dup_cmd_fd);
	return result;
}

/*
 * Probe DPDK PF device using rte_dev_probe from argp context
 *
 * @ctx [in]: flow switch context with devices from argp
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t rte_probe_pf_from_argp(struct flow_switch_ctx *ctx)
{
	doca_error_t result;
	struct doca_dev *dev;
	char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};

	if (ctx->devs_ctx.nb_devs == 0 || ctx->devs_ctx.devs_manager[0].doca_dev == NULL) {
		DOCA_LOG_ERR("No device provided via argp (use -a or -r option)");
		return DOCA_ERROR_INVALID_VALUE;
	}

	dev = ctx->devs_ctx.devs_manager[0].doca_dev;
	result = doca_devinfo_get_pci_addr_str(doca_dev_as_devinfo(dev), pci_addr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PCI address from device: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_INFO("Using device from argp context: %s", pci_addr);

	result = probe_device_with_devargs(pci_addr, FLOW_SWITCH_DEV_ARGS, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to probe PF proxy port: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Remove the manually probed PCI device
 *
 * This function stops/closes the probed port and removes the hot-plugged device.
 * Call it before dpdk_queues_and_ports_fini() so the device is still attached.
 */
static void remove_probed_pci_device(void)
{
	struct rte_eth_dev_info dev_info = {0};
	uint16_t port_id = RTE_MAX_ETHPORTS;

	for (uint16_t i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (port_to_dev[i].valid) {
			port_id = i;
			break;
		}
	}

	if (port_id == RTE_MAX_ETHPORTS) {
		DOCA_LOG_WARN("Failed to find probed port during cleanup");
		return;
	}

	if (!rte_eth_dev_is_valid_port(port_id)) {
		DOCA_LOG_WARN("Probed port %u is no longer valid during cleanup", port_id);
		return;
	}

	if (rte_eth_dev_info_get(port_id, &dev_info) != 0 || dev_info.device == NULL) {
		DOCA_LOG_WARN("Failed to get rte_device for port %d during cleanup", port_id);
		return;
	}

	if (rte_eth_dev_stop(port_id) != 0) {
		DOCA_LOG_WARN("rte_eth_dev_stop() failed for port %u", port_id);
	}

	if (rte_eth_dev_close(port_id) != 0) {
		DOCA_LOG_WARN("rte_eth_dev_close() failed for port %u", port_id);
	}

	if (rte_dev_remove(dev_info.device) != 0) {
		DOCA_LOG_WARN("Failed to remove probed device during cleanup on port %u", port_id);
		return;
	}

	port_to_dev[port_id].dev = NULL;
	port_to_dev[port_id].valid = false;
	DOCA_LOG_INFO("Removed probed device on port %u", port_id);
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
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = NB_DPDK_PF_PORTS,
		.port_config.nb_queues = 1,
		.port_config.switch_mode = 1,
	};
	struct flow_switch_ctx ctx = {0};

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

	result = doca_argp_init(NULL, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_doca_flow_switch_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow param: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Register common flow statistics parameters */
	result = register_flow_stats_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register stats parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Probe DPDK PF device using rte_dev_probe from argp context */
	result = rte_probe_pf_from_argp(&ctx);
	if (result != DOCA_SUCCESS)
		goto dpdk_cleanup;

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues");
		goto dpdk_cleanup;
	}

	/* run sample - pass actual number of ports (1 device + 2 reps = 3) */
	result = flow_switch(dpdk_config.port_config.nb_queues, NB_DOCA_FLOW_PORTS, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_switch() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	remove_probed_pci_device();
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini();
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	/* Destroy DOCA devices */
	destroy_doca_flow_devs(&ctx.devs_ctx);
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
