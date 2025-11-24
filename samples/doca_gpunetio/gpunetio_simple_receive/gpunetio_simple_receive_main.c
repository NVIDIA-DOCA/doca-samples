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

#include <doca_argp.h>
#include "gpunetio_common.h"

DOCA_LOG_REGISTER(GPUNETIO_SIMPLE_RECEIVE::MAIN);

/*
 * ARGP Callback - Set shared QP execution mode (THREAD, WARP or BLOCK)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t exec_scope_callback(void *param, void *config)
{
	struct sample_simple_recv_cfg *sample_cfg = (struct sample_simple_recv_cfg *)config;
	const uint32_t exec_scope = *(uint32_t *)param;

	if (exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD && exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP &&
	    exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK) {
		DOCA_LOG_ERR("Exec scope must be included between 0 and 2");
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->exec_scope = (uint32_t)exec_scope;

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
	struct sample_simple_recv_cfg *sample_cfg = (struct sample_simple_recv_cfg *)config;
	char *pci_address = (char *)param;
	size_t len;

	len = strnlen(pci_address, MAX_PCI_ADDRESS_LEN);
	if (len >= MAX_PCI_ADDRESS_LEN) {
		DOCA_LOG_ERR("PCI address too long. Max %d", MAX_PCI_ADDRESS_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(sample_cfg->gpu_pcie_addr, pci_address, len + 1);

	return DOCA_SUCCESS;
}

/*
 * Get NIC PCIe address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t nic_pci_address_callback(void *param, void *config)
{
	struct sample_simple_recv_cfg *sample_cfg = (struct sample_simple_recv_cfg *)config;
	char *pci_address = (char *)param;
	size_t len;

	len = strnlen(pci_address, MAX_PCI_ADDRESS_LEN);
	if (len >= MAX_PCI_ADDRESS_LEN) {
		DOCA_LOG_ERR("PCI address too long. Max %d", MAX_PCI_ADDRESS_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(sample_cfg->nic_pcie_addr, pci_address, len + 1);

	return DOCA_SUCCESS;
}

/*
 * Register sample command line parameters.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_sample_params(void)
{
	doca_error_t result;
	struct doca_argp_param *exec_param, *gpu_param, *nic_param;

	result = doca_argp_param_create(&exec_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(exec_param, "e");
	doca_argp_param_set_long_name(exec_param, "exec-scope");
	doca_argp_param_set_description(exec_param,
					"Shared QP mode to test: per-thread (0) per-warp (1) per-block (2).");
	doca_argp_param_set_callback(exec_param, exec_scope_callback);
	doca_argp_param_set_type(exec_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(exec_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&gpu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(gpu_param, "g");
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

	result = doca_argp_param_create(&nic_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(nic_param, "n");
	doca_argp_param_set_long_name(nic_param, "nic");
	doca_argp_param_set_arguments(nic_param, "<NIC PCIe address>");
	doca_argp_param_set_description(nic_param, "DOCA device PCIe address used by the sample");
	doca_argp_param_set_callback(nic_param, nic_pci_address_callback);
	doca_argp_param_set_type(nic_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(nic_param);
	result = doca_argp_register_param(nic_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
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
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	struct sample_simple_recv_cfg sample_cfg;
	int exit_status = EXIT_FAILURE;
	cudaError_t cuda_ret;

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

	/* Default mode if not set from command line */
	sample_cfg.exec_scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK;

	result = doca_argp_init(NULL, &sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_sample_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
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
	cuda_ret = cudaDeviceGetByPCIBusId(&sample_cfg.cuda_id, sample_cfg.gpu_pcie_addr);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Invalid GPU bus id provided %s", sample_cfg.gpu_pcie_addr);
		goto argp_cleanup;
	}

	cudaSetDevice(sample_cfg.cuda_id);

	DOCA_LOG_INFO("Sample configuration:\n\tGPU %s\n\tNIC %s\n\tShared QP exec scope %s\n\t",
		      sample_cfg.gpu_pcie_addr,
		      sample_cfg.nic_pcie_addr,
		      ((sample_cfg.exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD) ?
			       "Thread" :
			       (sample_cfg.exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP ? "Warp" : "Block")));

	result = gpunetio_simple_receive(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("gpunetio_simple_receive() encountered an error: %s", doca_error_get_descr(result));
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
