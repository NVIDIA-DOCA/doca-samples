/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <ctype.h>

#include <doca_argp.h>
#include <doca_config.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_log.h>

#include <devemu_pci_common.h>

#define MEM_BUF_LEN (4 * 1024) /* Mem buffer size. It's the same as Host side */

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_DMA_DPU::MAIN);

/* Sample's Logic */
doca_error_t devemu_pci_device_dma_dpu(const char *pci_address,
				       const char *devemu_name,
				       const char *emulated_dev_vuid,
				       uint64_t host_dma_mem_iova,
				       const char *write_data);

/* Configuration struct */
struct devemu_pci_cfg {
	char devemu_manager_pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Emulated device manager PCI address */
	char dma_dev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE];	     /* DMA device name */
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE];			     /* VUID of emulated device */
	uint64_t host_dma_mem_iova;				     /* IOVA of host DMA memory */
	char write_data[MEM_BUF_LEN];				     /* Data to write to host memory */
};

#ifdef DOCA_ARCH_DPU

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_name_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	char *device_name = (char *)param;

	int len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(conf->dma_dev_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

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

	return parse_pci_address(addr, conf->devemu_manager_pci_address);
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
 * ARGP Callback - Handle mem address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mem_address_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *mem_addr = (char *)param;
	int len = strlen(mem_addr);

	if (len <= 2 || mem_addr[0] != '0' || tolower(mem_addr[1]) != 'x' ||
	    sscanf(mem_addr + 2, "%lx", &conf->host_dma_mem_iova) != 1) {
		DOCA_LOG_ERR(
			"Entered host DMA memory address does not match supported formats: 0x12345678 or 0X12345678");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Stateful Region Index parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t write_data_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *write_data = (char *)param;
	int data_len = strnlen(write_data, MEM_BUF_LEN) + 1;

	/* Check using > to make static code analysis satisfied */
	if (data_len > MEM_BUF_LEN) {
		DOCA_LOG_ERR("Entered write data exceeds max stateful region size of %d", MEM_BUF_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(conf->write_data, write_data, data_len - 1);
	conf->write_data[data_len - 1] = 0;

	return DOCA_SUCCESS;
}

/*
 * Register device emulation manager PCI address command line parameter
 *
 * @pci_callback [in]: Callback called for parsing the device emulation manager PCI address command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_devemu_manager_pci_address_param(doca_argp_param_cb_t pci_callback)
{
	struct doca_argp_param *param;
	doca_error_t result;

	/* Create and register device emulation manager PCI address param */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "p");
	doca_argp_param_set_long_name(param, "pci-addr");
	doca_argp_param_set_description(
		param,
		"The DOCA device emulation manager PCI address. Format: XXXX:XX:XX.X or XX:XX.X");
	doca_argp_param_set_callback(param, pci_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register DMA IB device name command line parameter
 *
 * @device_name_callback [in]: Callback called for parsing the DMA IB device name command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_dma_device_name_param(doca_argp_param_cb_t device_name_callback)
{
	struct doca_argp_param *param;
	doca_error_t result;

	/* Create and register DMA IB device name param */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "d");
	doca_argp_param_set_long_name(param, "device-name");
	doca_argp_param_set_description(param, "The IB device name to be used for DMA operations");
	doca_argp_param_set_callback(param, device_name_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register write data command line parameter
 *
 * @write_data_callback [in]: Callback called for parsing the write data command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_write_data_param(doca_argp_param_cb_t write_data_callback)
{
	struct doca_argp_param *write_data_param;
	doca_error_t result;

	result = doca_argp_param_create(&write_data_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(write_data_param, "w");
	doca_argp_param_set_long_name(write_data_param, "write-data");
	doca_argp_param_set_description(
		write_data_param,
		"Data to write to the host memory, if not provided then the sample will do a read instead. ASCII string");
	doca_argp_param_set_callback(write_data_param, write_data_callback);
	doca_argp_param_set_type(write_data_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(write_data_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register host memory address command line parameter
 *
 * @mem_address_callback [in]: Callback called for parsing the memory address command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_mem_address_param(doca_argp_param_cb_t mem_address_callback)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "a");
	doca_argp_param_set_long_name(param, "addr");
	doca_argp_param_set_description(param, "Host DMA memory IOVA address. Format: 0x12345678 or 0X12345678");
	doca_argp_param_set_callback(param, mem_address_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
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

	result = register_devemu_manager_pci_address_param(pci_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_dma_device_name_param(device_name_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_vuid_param("DOCA Devemu emulated device VUID.", vuid_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_mem_address_param(mem_address_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_write_data_param(write_data_callback);
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
	strcpy(devemu_pci_cfg.devemu_manager_pci_address, "0000:03:00.0");
	strcpy(devemu_pci_cfg.dma_dev_name, "");
	strcpy(devemu_pci_cfg.vuid, "");
	strcpy(devemu_pci_cfg.write_data, "This is a sample piece of data from DPU!");
	devemu_pci_cfg.host_dma_mem_iova = 0x1000000;

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

	result = devemu_pci_device_dma_dpu(devemu_pci_cfg.devemu_manager_pci_address,
					   devemu_pci_cfg.dma_dev_name,
					   devemu_pci_cfg.vuid,
					   devemu_pci_cfg.host_dma_mem_iova,
					   devemu_pci_cfg.write_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("devemu_pci_device_dma_dpu() encountered an error: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

#else // DOCA_ARCH_DPU
	(void)argc;
	(void)argv;

	DOCA_LOG_ERR("PCI Emulated Device DMA DPU can run only on the DPU");
	exit_status = EXIT_FAILURE;

#endif // DOCA_ARCH_DPU

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
