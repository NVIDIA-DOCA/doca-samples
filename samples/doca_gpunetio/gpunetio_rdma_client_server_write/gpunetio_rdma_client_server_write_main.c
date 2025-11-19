/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_log.h>
#include <doca_argp.h>

#include "rdma_common.h"

DOCA_LOG_REGISTER(GPURDMA_WRITE_REQUESTER::MAIN);

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_address_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(rdma_cfg->device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set GID index
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t gid_index_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const int gid_index = *(uint32_t *)param;

	if (gid_index < 0) {
		DOCA_LOG_ERR("GID index for DOCA RDMA must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rdma_cfg->is_gid_index_set = true;
	rdma_cfg->gid_index = (uint32_t)gid_index;

	return DOCA_SUCCESS;
}

/*
 * Get GPU PCIe address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t gpu_pci_address_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *pci_address = (char *)param;
	size_t len;

	len = strnlen(pci_address, MAX_PCI_ADDRESS_LEN);
	if (len >= MAX_PCI_ADDRESS_LEN) {
		DOCA_LOG_ERR("PCI address too long. Max %d", MAX_PCI_ADDRESS_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(rdma_cfg->gpu_pcie_addr, pci_address, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle client parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t client_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *server_ip_addr = (char *)param;
	int len;

	len = strnlen(server_ip_addr, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(rdma_cfg->server_ip_addr, server_ip_addr, len + 1);
	rdma_cfg->is_server = false;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle use_rdma_cm parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t use_rdma_cm_param_callback(void *param, void *config)
{
	(void)param;
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;

	rdma_cfg->use_rdma_cm = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cm_port parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cm_port_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const int cm_port = *(uint32_t *)param;

	if (cm_port < 0) {
		DOCA_LOG_ERR("Server listening port number for DOCA RDMA-CM must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rdma_cfg->cm_port = (uint32_t)cm_port;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cm server addr parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cm_addr_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *addr = (char *)param;
	int addr_len = strnlen(addr, SERVER_ADDR_LEN + 1);

	if (addr_len == SERVER_ADDR_LEN) {
		DOCA_LOG_ERR("Entered server address exceeded buffer size: %d", SERVER_ADDR_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->cm_addr, addr, addr_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cm server addr type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cm_addr_type_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *type = (char *)param;
	int type_len = strnlen(type, SERVER_ADDR_TYPE_LEN + 1);

	if (type_len == SERVER_ADDR_TYPE_LEN) {
		DOCA_LOG_ERR("Entered server address type exceeded buffer size: %d", SERVER_ADDR_TYPE_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcasecmp(type, "ip4") == 0 || strcasecmp(type, "ipv4") == 0)
		rdma_cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_IPv4;
	else if (strcasecmp(type, "ip6") == 0 || strcasecmp(type, "ipv6") == 0)
		rdma_cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_IPv6;
	else if (strcasecmp(type, "gid") == 0)
		rdma_cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_GID;
	else {
		DOCA_LOG_ERR("Entered wrong server address type, the accepted server address type are: "
			     "ip4, ipv4, IP4, IPv4, IPV4, "
			     "ip6, ipv6, IP6, IPv6, IPV6, "
			     "gid, GID");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Register sample argp params
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_rdma_params(void)
{
	doca_error_t result;
	struct doca_argp_param *client_param;
	struct doca_argp_param *device_param;
	struct doca_argp_param *gid_index_param;
	struct doca_argp_param *gpu_param;
	struct doca_argp_param *use_rdma_cm_param;
	struct doca_argp_param *cm_port_param;
	struct doca_argp_param *cm_addr_param;
	struct doca_argp_param *cm_addr_type_param;

	/* Create and register client param */
	result = doca_argp_param_create(&client_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(client_param, "c");
	doca_argp_param_set_long_name(client_param, "client");
	doca_argp_param_set_arguments(client_param, "<Sample is client, requires server OOB IP>");
	doca_argp_param_set_description(client_param, "Sample is client, requires server OOB IP");
	doca_argp_param_set_callback(client_param, client_param_callback);
	doca_argp_param_set_type(client_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(client_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register device param */
	result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<IB device name>");
	doca_argp_param_set_description(device_param, "IB device name");
	doca_argp_param_set_callback(device_param, device_address_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	result = doca_argp_register_param(device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register GPU param */
	result = doca_argp_param_create(&gpu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gpu_param, "gpu");
	doca_argp_param_set_long_name(gpu_param, "gpu");
	doca_argp_param_set_arguments(gpu_param, "<GPU PCIe address>");
	doca_argp_param_set_description(gpu_param, "GPU PCIe address to be used by the sample");
	doca_argp_param_set_callback(gpu_param, gpu_pci_address_callback);
	doca_argp_param_set_type(gpu_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(gpu_param);
	result = doca_argp_register_param(gpu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register gid_index param */
	result = doca_argp_param_create(&gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gid_index_param, "gid");
	doca_argp_param_set_long_name(gid_index_param, "gid-index");
	doca_argp_param_set_description(gid_index_param, "GID index for DOCA RDMA (optional)");
	doca_argp_param_set_callback(gid_index_param, gid_index_callback);
	doca_argp_param_set_type(gid_index_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register user_rdma_cm param */
	result = doca_argp_param_create(&use_rdma_cm_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(use_rdma_cm_param, "cm");
	doca_argp_param_set_long_name(use_rdma_cm_param, "use-rdma-cm");
	doca_argp_param_set_description(use_rdma_cm_param, "Whether to use rdma-cm or oob to setup connection");
	doca_argp_param_set_callback(use_rdma_cm_param, use_rdma_cm_param_callback);
	doca_argp_param_set_type(use_rdma_cm_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(use_rdma_cm_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cm_port_param */
	result = doca_argp_param_create(&cm_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cm_port_param, "p");
	doca_argp_param_set_long_name(cm_port_param, "port");
	doca_argp_param_set_arguments(cm_port_param, "<port-num>");
	doca_argp_param_set_description(cm_port_param, "CM port number");
	doca_argp_param_set_callback(cm_port_param, cm_port_param_callback);
	doca_argp_param_set_type(cm_port_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(cm_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cm_addr_param */
	result = doca_argp_param_create(&cm_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cm_addr_param, "sa");
	doca_argp_param_set_long_name(cm_addr_param, "server-addr");
	doca_argp_param_set_arguments(cm_addr_param, "<server address>");
	doca_argp_param_set_description(cm_addr_param,
					"RDMA cm server device address, required only when using rdma_cm");
	doca_argp_param_set_callback(cm_addr_param, cm_addr_param_callback);
	doca_argp_param_set_type(cm_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(cm_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cm_addr_type_param */
	result = doca_argp_param_create(&cm_addr_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cm_addr_type_param, "sat");
	doca_argp_param_set_long_name(cm_addr_type_param, "server-addr-type");
	doca_argp_param_set_arguments(cm_addr_type_param, "<server address type>");
	doca_argp_param_set_description(
		cm_addr_type_param,
		"RDMA cm server device address type: IPv4, IPv6 or GID, required only when using rdma_cm");
	doca_argp_param_set_callback(cm_addr_type_param, cm_addr_type_param_callback);
	doca_argp_param_set_type(cm_addr_type_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(cm_addr_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Verify CM's parameters received from the user
 *
 * @cfg [in]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t verify_cm_params(struct rdma_config *cfg)
{
	if (cfg->use_rdma_cm) {
		if (cfg->is_server) {
			if (cfg->cm_addr[0] != '\0') {
				DOCA_LOG_ERR(
					"Invalid input: when using CM, only client needs to provide the server cm_addr");
				return DOCA_ERROR_INVALID_VALUE;
			}
		} else {
			if (cfg->cm_addr[0] == '\0') {
				DOCA_LOG_ERR("Invalid input: when using CM, client must provide the server cm_addr");
				return DOCA_ERROR_INVALID_VALUE;
			}
		}
	} else {
		if (cfg->cm_addr[0] != '\0') {
			DOCA_LOG_ERR("Invalid input: server cm_addr is needed only in case of RDMA CM");
			return DOCA_ERROR_INVALID_VALUE;
		}
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
int main(int argc, char **argv)
{
	struct rdma_config cfg = {0};
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	cudaError_t cuda_ret;
	int cuda_id;

	/* Set the default configuration values (Example values) */
	cfg.is_server = true;
	cfg.cm_port = DEFAULT_CM_PORT;
	cfg.cm_addr_type = DOCA_RDMA_ADDR_TYPE_IPv4;
	cfg.gid_index = 0;
	cfg.use_rdma_cm = false;

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

	/* Initialize argparser */
	result = doca_argp_init(NULL, &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	/* Register RDMA common params */
	result = register_rdma_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Start argparser */
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Verify params */
	result = verify_cm_params(&cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/*
	 * A CUDA context must be initialized before calling GPUNetIO functions.
	 * cudaFree(0) triggers tje CUDA runtime initialization and report any errors.
	 */
	cuda_ret = cudaFree(0);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("CUDA initialization failed: %s\n", cudaGetErrorString(cuda_ret));
		goto argp_cleanup;
	}

	/* In a multi-GPU system, ensure CUDA refers to the right GPU device */
	cuda_ret = cudaDeviceGetByPCIBusId(&cuda_id, cfg.gpu_pcie_addr);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Invalid GPU bus id provided %s", cfg.gpu_pcie_addr);
		goto argp_cleanup;
	}

	cudaSetDevice(cuda_id);

	/* Start sample */
	if (cfg.is_server) {
		result = rdma_write_server(&cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("rdma_write_server() failed: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else {
		result = rdma_write_client(&cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("rdma_write_client() failed: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}
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
