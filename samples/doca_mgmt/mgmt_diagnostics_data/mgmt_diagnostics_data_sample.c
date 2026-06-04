/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <stdio.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_mgmt_diagnostics_data.h>

DOCA_LOG_REGISTER(MGMT_DIAGNOSTICS_DATA::SAMPLE);

/**
 * Check if the device supports diagnostics data and print the result.
 *
 * @param dev [in]: DOCA device to check
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_diagnostics_data_supported(struct doca_dev *dev)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	doca_error_t result;

	result = doca_mgmt_dev_ctx_create(dev, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mgmt_cap_diagnostics_data_multi_domain_is_supported(dev_ctx);
	if (result == DOCA_SUCCESS) {
		printf("Diagnostics data multi-domain: supported\n");
	} else if (result == DOCA_ERROR_NOT_SUPPORTED) {
		printf("Diagnostics data multi-domain: unsupported\n");
		result = DOCA_SUCCESS;
	} else {
		DOCA_LOG_ERR("Failed to check diagnostics data support: %s", doca_error_get_descr(result));
	}

	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}

/**
 * Get diagnostics data configuration
 *
 * @param dev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_diagnostics_data_get(struct doca_dev *dev)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_diagnostics_data *dd_handle;
	uint8_t multi_domain;
	doca_error_t result;

	result = doca_mgmt_dev_ctx_create(dev, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mgmt_diagnostics_data_create(&dd_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create diagnostics data handle: %s", doca_error_get_descr(result));
		goto destroy_dev_ctx;
	}

	result = doca_mgmt_diagnostics_data_query_for_dev(dev_ctx, dd_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query diagnostics data: %s", doca_error_get_descr(result));
		goto destroy_dd_handle;
	}

	result = doca_mgmt_diagnostics_data_get_multi_domain(dd_handle, &multi_domain);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get multi_domain: %s", doca_error_get_descr(result));
		goto destroy_dd_handle;
	}

	printf("multi_domain: %s\n", multi_domain ? "true" : "false");
	result = DOCA_SUCCESS;

destroy_dd_handle:
	if (doca_mgmt_diagnostics_data_destroy(dd_handle) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy diagnostics data handle");

destroy_dev_ctx:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}

/**
 * Set diagnostics data configuration for the device
 *
 * @param dev [in]: DOCA device
 * @param multi_domain [in]: Value to write for multi_domain (true = enable, false = disable)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_diagnostics_data_set(struct doca_dev *dev, bool multi_domain)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_diagnostics_data *dd_handle;
	doca_error_t result;

	result = doca_mgmt_dev_ctx_create(dev, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mgmt_diagnostics_data_create(&dd_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create diagnostics data handle: %s", doca_error_get_descr(result));
		goto destroy_dev_ctx;
	}

	result = doca_mgmt_diagnostics_data_set_multi_domain(dd_handle, multi_domain ? 1 : 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diagnostics data multi_domain: %s", doca_error_get_descr(result));
		goto destroy_dd_handle;
	}

	result = doca_mgmt_diagnostics_data_modify_for_dev(dev_ctx, dd_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify diagnostics data: %s", doca_error_get_descr(result));
		goto destroy_dd_handle;
	}

destroy_dd_handle:
	if (doca_mgmt_diagnostics_data_destroy(dd_handle) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy diagnostics data handle");

destroy_dev_ctx:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}
