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

#include <stdbool.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <samples/common.h>
#include "rdma_verbs_wait_cq_wr_sample.h"

DOCA_LOG_REGISTER(wait_cq_wr::MAIN);

#define DEFAULT_IS_IPV6 0

static struct wait_cq_wr_cfg app_cfg;

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_name_callback(void *param, void *config)
{
	struct wait_cq_wr_cfg *cfg = (struct wait_cq_wr_cfg *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, IBV_DEVICE_NAME_SIZE);
	if (len == IBV_DEVICE_NAME_SIZE) {
		DOCA_LOG_ERR("Entered device name exceeding the maximum size of %d", IBV_DEVICE_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(cfg->ibv_device_name, device_name, len + 1);
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
	struct wait_cq_wr_cfg *cfg = (struct wait_cq_wr_cfg *)config;
	const int gid_index = *(uint32_t *)param;

	if (gid_index < 0) {
		DOCA_LOG_ERR("GID index for DOCA RDMA must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	cfg->gid_index = (uint32_t)gid_index;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set IPv6 flag
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t is_ipv6_callback(void *param, void *config)
{
	struct wait_cq_wr_cfg *cfg = (struct wait_cq_wr_cfg *)config;
	const bool is_ipv6 = *(bool *)param;

	cfg->is_ipv6 = !!is_ipv6;
	return DOCA_SUCCESS;
}

static doca_error_t register_params(void)
{
	doca_error_t result;
	struct doca_argp_param *device_param, *gid_index_param, *is_ipv6_param;

	result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device-name");
	doca_argp_param_set_description(device_param, "IBV device name to be used.");
	doca_argp_param_set_callback(device_param, device_name_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	result = doca_argp_register_param(device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gid_index_param, "gi");
	doca_argp_param_set_long_name(gid_index_param, "gid-index");
	doca_argp_param_set_description(gid_index_param,
					"GID index to use. If none provided, a matching GID will be searched.");
	doca_argp_param_set_callback(gid_index_param, gid_index_callback);
	doca_argp_param_set_type(gid_index_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&is_ipv6_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(is_ipv6_param, "ipv6");
	doca_argp_param_set_description(is_ipv6_param, "Use IPv6 instead of IPv4");
	doca_argp_param_set_callback(is_ipv6_param, is_ipv6_callback);
	doca_argp_param_set_type(is_ipv6_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(is_ipv6_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

static void set_default_params(struct wait_cq_wr_cfg *cfg)
{
	cfg->is_ipv6 = DEFAULT_IS_IPV6;
}

int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto exit;

	DOCA_LOG_INFO("Starting the sample");

	memset(&app_cfg, 0, sizeof(app_cfg));

	set_default_params(&app_cfg);

	result = doca_argp_init("wait_cq_wr", &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto exit;
	}

	result = register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = send_chained_messages(&app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send chained messages: %s", doca_error_get_descr(result));
	} else {
		DOCA_LOG_INFO("Sample finished successfully");
		exit_status = EXIT_SUCCESS;
	}

argp_cleanup:
	doca_argp_destroy();
exit:
	return exit_status;
}
