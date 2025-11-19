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

#include <stdlib.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include <gpunetio_dma_common.h>

DOCA_LOG_REGISTER(GPU_DMA_MEMCPY::MAIN);

/*
 * ARGP Callback - Handle GPU PCIe address parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t gpu_pci_address_callback(void *param, void *config)
{
	struct gpu_dma_config *gpu_dma_cfg = (struct gpu_dma_config *)config;
	char *gpu_pci_address = (char *)param;
	int len;

	len = strnlen(gpu_pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("PCI address too long. Max %d", DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(gpu_dma_cfg->gpu_pcie_addr, gpu_pci_address, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle NIC PCIe address parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nic_pci_address_callback(void *param, void *config)
{
	struct gpu_dma_config *gpu_dma_cfg = (struct gpu_dma_config *)config;
	char *nic_pci_address = (char *)param;
	int len;

	len = strnlen(nic_pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("PCI address too long. Max %d", DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(gpu_dma_cfg->nic_pcie_addr, nic_pci_address, len + 1);

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_gpu_dma_memcpy_sample_params(void)
{
	doca_error_t status;
	struct doca_argp_param *nic_param;
	struct doca_argp_param *gpu_param;

	status = doca_argp_param_create(&gpu_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}

	doca_argp_param_set_short_name(gpu_param, "g");
	doca_argp_param_set_long_name(gpu_param, "gpu");
	doca_argp_param_set_arguments(gpu_param, "<GPU PCIe address>");
	doca_argp_param_set_description(gpu_param, "GPU PCIe address to be used by the sample");
	doca_argp_param_set_callback(gpu_param, gpu_pci_address_callback);
	doca_argp_param_set_type(gpu_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(gpu_param);
	status = doca_argp_register_param(gpu_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&nic_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}

	doca_argp_param_set_short_name(nic_param, "n");
	doca_argp_param_set_long_name(nic_param, "nic");
	doca_argp_param_set_arguments(nic_param, "<NIC PCIe address>");
	doca_argp_param_set_description(nic_param, "NIC PCIe address to be used by the sample");
	doca_argp_param_set_callback(nic_param, nic_pci_address_callback);
	doca_argp_param_set_type(nic_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(nic_param);
	status = doca_argp_register_param(nic_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(status));
		return status;
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
	doca_error_t status;
	struct gpu_dma_config cfg = {0};
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	int cuda_id = 0;
	cudaError_t cuda_ret;

	/* Register a logger backend */
	status = doca_log_backend_create_standard();
	if (status != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	status = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (status != DOCA_SUCCESS)
		goto sample_exit;
	status = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (status != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	/* Initialize argparser */
	status = doca_argp_init(NULL, &cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(status));
		goto sample_exit;
	}

	status = register_gpu_dma_memcpy_sample_params();
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(status));
		goto argp_cleanup;
	}

	status = doca_argp_start(argc, argv);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(status));
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

	status = gpunetio_dma_memcpy(&cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("gpunetio_dma_memcpy() encountered an error: %s", doca_error_get_descr(status));
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
