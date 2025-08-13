/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "verbs_common.h"

DOCA_LOG_REGISTER(VERBS_PUT_SIGNAL::MAIN);

/*
 * Register sample argp params
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_verbs_params(void)
{
	doca_error_t status;
	struct doca_argp_param *client_param;
	struct doca_argp_param *nic_handler_param;
	struct doca_argp_param *device_param;
	struct doca_argp_param *gid_index_param;
	struct doca_argp_param *gpu_device_param;
	struct doca_argp_param *iters_param;
	struct doca_argp_param *exec_param;
	struct doca_argp_param *threads_param;

	status = doca_argp_param_create(&client_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(client_param, "c");
	doca_argp_param_set_long_name(client_param, "client");
	doca_argp_param_set_arguments(client_param, "<Sample is client, requires server OOB IP>");
	doca_argp_param_set_description(client_param, "Sample is client, requires server OOB IP");
	doca_argp_param_set_callback(client_param, client_param_callback);
	doca_argp_param_set_type(client_param, DOCA_ARGP_TYPE_STRING);
	status = doca_argp_register_param(client_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&nic_handler_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(nic_handler_param, "n");
	doca_argp_param_set_long_name(nic_handler_param, "nic handler");
	doca_argp_param_set_arguments(nic_handler_param, "<NIC handler type (0: AUTO, 1: CPU Proxy 2: GPU DB)>");
	doca_argp_param_set_description(nic_handler_param, "NIC handler type");
	doca_argp_param_set_callback(nic_handler_param, nic_handler_callback);
	doca_argp_param_set_type(nic_handler_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(nic_handler_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&device_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<NIC device name>");
	doca_argp_param_set_description(device_param, "NIC device name");
	doca_argp_param_set_callback(device_param, nic_device_name_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	status = doca_argp_register_param(device_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&gpu_device_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(gpu_device_param, "g");
	doca_argp_param_set_long_name(gpu_device_param, "gpu");
	doca_argp_param_set_arguments(gpu_device_param, "<GPU device PCIe address>");
	doca_argp_param_set_description(gpu_device_param, "GPU device PCIe address");
	doca_argp_param_set_callback(gpu_device_param, gpu_pcie_addr_callback);
	doca_argp_param_set_type(gpu_device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(gpu_device_param);
	status = doca_argp_register_param(gpu_device_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&gid_index_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(gid_index_param, "gid");
	doca_argp_param_set_long_name(gid_index_param, "gid-index");
	doca_argp_param_set_description(gid_index_param, "GID index for DOCA RDMA (optional)");
	doca_argp_param_set_callback(gid_index_param, gid_index_callback);
	doca_argp_param_set_type(gid_index_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(gid_index_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&iters_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(iters_param, "i");
	doca_argp_param_set_long_name(iters_param, "iters");
	doca_argp_param_set_description(iters_param, "Number of iterations (optional)");
	doca_argp_param_set_callback(iters_param, iters_callback);
	doca_argp_param_set_type(iters_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(iters_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&exec_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(exec_param, "e");
	doca_argp_param_set_long_name(exec_param, "exec-shared");
	doca_argp_param_set_description(exec_param, "Shared QP mode to test: per-thread (0) per-warp (1)");
	doca_argp_param_set_callback(exec_param, exec_callback);
	doca_argp_param_set_type(exec_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(exec_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_argp_param_create(&threads_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(threads_param, "t");
	doca_argp_param_set_long_name(threads_param, "thread");
	doca_argp_param_set_description(threads_param, "Number of CUDA Threads (optional)");
	doca_argp_param_set_callback(threads_param, threads_callback);
	doca_argp_param_set_type(threads_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(threads_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
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
	struct verbs_config cfg = {0};
	doca_error_t status;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values */
	cfg.is_server = true;
	cfg.gid_index = DEFAULT_GID_INDEX;
	cfg.num_iters = NUM_ITERS;
	cfg.cuda_threads = CUDA_THREADS_BW;
	cfg.nic_handler = DOCA_GPUNETIO_VERBS_NIC_HANDLER_AUTO;

	status = doca_log_backend_create_standard();
	if (status != DOCA_SUCCESS)
		goto sample_exit;

	status = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (status != DOCA_SUCCESS)
		goto sample_exit;
	status = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (status != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	status = doca_argp_init("doca_gpunetio_verbs_put_bw", &cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(status));
		goto sample_exit;
	}

	status = register_verbs_params();
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample parameters: %s", doca_error_get_descr(status));
		goto argp_cleanup;
	}

	status = doca_argp_start(argc, argv);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(status));
		goto argp_cleanup;
	}

	if (cfg.is_server) {
		status = verbs_server(&cfg);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("verbs_server() failed: %s", doca_error_get_descr(status));
			goto argp_cleanup;
		}
	} else {
		status = verbs_client(&cfg);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("verbs_client() failed: %s", doca_error_get_descr(status));
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
