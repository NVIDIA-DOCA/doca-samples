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

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_mgmt_icm_quota.h>

DOCA_LOG_REGISTER(MGMT_ICM_QUOTA::SAMPLE);

#define ICM_QUOTA_LIMIT_UNLIMITED UINT64_MAX

/*
 * Format size value with appropriate unit
 *
 * @value [in]: Size value in bytes
 * @buffer [out]: Buffer to store the formatted string
 * @buffer_size [in]: Size of the buffer
 * @has_unlimited [in]: Whether the value can be unlimited
 */
static void format_size(uint32_t size_4k, char *buffer, size_t buffer_size, bool has_unlimited)
{
	const char *units[] = {"B", "K", "M", "G", "T"};
	int unit_idx = 0;
	uint64_t remainder = 0;
	uint64_t size_k = (uint64_t)size_4k * 4;
	uint64_t size_formatted = (uint64_t)size_4k * 4096;
	double size_formatted_frac = (double)size_formatted;

	if (size_4k == DOCA_MGMT_ICM_QUOTA_LIMIT_UNLIMITED && has_unlimited) {
		snprintf(buffer, buffer_size, "unlimited");
		return;
	}

	/* Find the most appropriate unit */
	while (size_formatted >= 1024 && unit_idx < 4) {
		remainder = size_formatted % 1024;
		size_formatted /= 1024;
		size_formatted_frac /= 1024.0;
		unit_idx++;
	}

	if (remainder != 0)
		snprintf(buffer, buffer_size, "%" PRIu64 "K (%.2f%s)", size_k, size_formatted_frac, units[unit_idx]);
	else
		snprintf(buffer, buffer_size, "%" PRIu64 "K (%" PRIu64 "%s)", size_k, size_formatted, units[unit_idx]);
}

/*
 * Print the ICM configuration
 *
 * @icm [in]: ICM handle
 * @get_limit [in]: Whether to get the limit
 * @get_cur_alloc [in]: Whether to get the current allocated ICM
 * @get_max_reached [in]: Whether to get the max reached ICM
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t print_icm_quota_config(struct doca_mgmt_icm_quota *icm_quota,
					   bool get_limit,
					   bool get_cur_alloc,
					   bool get_max_reached)
{
	uint32_t limit;
	uint32_t cur_alloc;
	uint32_t max_reached;
	char size_str[64];
	doca_error_t result;

	if (get_limit) {
		result = doca_mgmt_icm_quota_get_limit(icm_quota, &limit);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get ICM quota limit: %s", doca_error_get_descr(result));
			return result;
		}
		format_size(limit, size_str, sizeof(size_str), true);
		printf("limit:              %s\n", size_str);
	}

	if (get_cur_alloc) {
		result = doca_mgmt_icm_quota_get_current_allocation(icm_quota, &cur_alloc);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get current allocated ICM quota: %s", doca_error_get_descr(result));
			return result;
		}
		format_size(cur_alloc, size_str, sizeof(size_str), false);
		printf("current_allocation: %s\n", size_str);
	}

	if (get_max_reached) {
		result = doca_mgmt_icm_quota_get_max_reached(icm_quota, &max_reached);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get max reached ICM quota: %s", doca_error_get_descr(result));
			return result;
		}
		format_size(max_reached, size_str, sizeof(size_str), false);
		printf("max_reached:        %s\n", size_str);
	}

	return DOCA_SUCCESS;
}

/*
 * Get the ICM quota configuration for a device or device representor.
 * If a device representor is provided, ICM quota configuration for the device representor will be retrieved.
 * Otherwise, ICM quota onfiguration for the device will be retrieved.
 *
 * @dev [in]: The device to get its ICM quota configuration
 * @dev_rep [in]: The device representor to get its ICM quota configuration. This parameter is optional and can be NULL.
 * @get_limit [in]: Whether to get the ICM quota limit
 * @get_cur_alloc [in]: Whether to get the current allocated ICM quota
 * @get_max_reached [in]: Whether to get the max reached ICM quota
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_icm_quota_get(struct doca_dev *dev,
				struct doca_dev_rep *dev_rep,
				bool get_limit,
				bool get_cur_alloc,
				bool get_max_reached)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_dev_rep_ctx *dev_rep_ctx = NULL;
	struct doca_mgmt_icm_quota *icm_quota;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	if (dev_rep != NULL) {
		/* Create the DOCA management device representor context */
		result = doca_mgmt_dev_rep_ctx_create(dev_ctx, dev_rep, &dev_rep_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create DOCA management device representor context: %s",
				     doca_error_get_descr(result));
			goto out_dev_ctx_destroy;
		}

		/* Create the ICM quota handle for device representor */
		result = doca_mgmt_icm_quota_create_for_dev_rep(dev_rep_ctx, &icm_quota);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ICM quota handle for device representor: %s",
				     doca_error_get_descr(result));
			goto out_dev_rep_ctx_destroy;
		}
	} else {
		/* Create the ICM quota handle for device */
		result = doca_mgmt_icm_quota_create_for_dev(dev_ctx, &icm_quota);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ICM quota handle for device: %s", doca_error_get_descr(result));
			goto out_dev_ctx_destroy;
		}
	}

	/* Execute the query command */
	result = doca_mgmt_icm_quota_query(icm_quota);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query ICM quota configuration: %s", doca_error_get_descr(result));
		goto out_icm_quota_destroy;
	}

	/* Get and display the requested attributes */
	result = print_icm_quota_config(icm_quota, get_limit, get_cur_alloc, get_max_reached);

out_icm_quota_destroy:
	if (doca_mgmt_icm_quota_destroy(icm_quota) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy ICM quota handle");

out_dev_rep_ctx_destroy:
	if (dev_rep_ctx != NULL && doca_mgmt_dev_rep_ctx_destroy(dev_rep_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device representor context");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Convert value in bytes to value in 4KB
 *
 * @value [in]: Value in bytes
 * @return: Value in 4KB
 */
static uint32_t mgmt_icm_quota_bytes_to_4k(uint64_t value)
{
	if (value == ICM_QUOTA_LIMIT_UNLIMITED)
		return DOCA_MGMT_ICM_QUOTA_LIMIT_UNLIMITED;
	else
		return value / 4096;
}

/*
 * Set the ICM quota configuration for a device or device representor.
 * If a device representor is provided, ICM quota configuration for the device representor will be set.
 * Otherwise, ICM quota configuration for the device will be set.
 *
 * @dev [in]: The device to set its ICM quota configuration
 * @dev_rep [in]: The device representor to set its ICM quota configuration. This parameter is optional and can be NULL.
 * @limit_set [in]: Whether to set the ICM quota limit
 * @limit [in]: The ICM quota limit value
 * @reset_max_reached [in]: Whether to reset the max reached ICM quota
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_icm_quota_set(struct doca_dev *dev,
				struct doca_dev_rep *dev_rep,
				bool limit_set,
				uint64_t limit,
				bool reset_max_reached)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_dev_rep_ctx *dev_rep_ctx = NULL;
	struct doca_mgmt_icm_quota *icm_quota;
	uint32_t limit_4k;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	if (dev_rep != NULL) {
		/* Create the DOCA management device representor context */
		result = doca_mgmt_dev_rep_ctx_create(dev_ctx, dev_rep, &dev_rep_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create DOCA management device representor context: %s",
				     doca_error_get_descr(result));
			goto out_dev_ctx_destroy;
		}

		/* Create the ICM quota handle for device representor */
		result = doca_mgmt_icm_quota_create_for_dev_rep(dev_rep_ctx, &icm_quota);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ICM quota handle for device representor: %s",
				     doca_error_get_descr(result));
			goto out_dev_rep_ctx_destroy;
		}
	} else {
		/* Create the ICM quota handle for device */
		result = doca_mgmt_icm_quota_create_for_dev(dev_ctx, &icm_quota);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ICM quota handle for device: %s", doca_error_get_descr(result));
			goto out_dev_ctx_destroy;
		}
	}

	/* Set the attributes */
	if (limit_set) {
		limit_4k = mgmt_icm_quota_bytes_to_4k(limit);
		result = doca_mgmt_icm_quota_set_limit(icm_quota, limit_4k);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set ICM quota limit: %s", doca_error_get_descr(result));
			goto out_icm_quota_destroy;
		}
	}

	if (reset_max_reached) {
		result = doca_mgmt_icm_quota_set_reset_max_reached(icm_quota);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set reset max reached: %s", doca_error_get_descr(result));
			goto out_icm_quota_destroy;
		}
	}

	/* Execute the modify command */
	result = doca_mgmt_icm_quota_modify(icm_quota);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to modify ICM quota configuration: %s", doca_error_get_descr(result));

out_icm_quota_destroy:
	if (doca_mgmt_icm_quota_destroy(icm_quota) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy ICM quota handle");

out_dev_rep_ctx_destroy:
	if (dev_rep_ctx != NULL && doca_mgmt_dev_rep_ctx_destroy(dev_rep_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device representor context");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Get the ICM quota capabilities for a device.
 *
 * @dev [in]: The device to get its ICM quota capabilities
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_icm_quota_caps(struct doca_dev *dev)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	uint32_t max_limit;
	char size_str[64];
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Check if ICM quota is supported */
	result = doca_mgmt_cap_icm_quota_is_supported(dev_ctx);
	if (result == DOCA_ERROR_NOT_SUPPORTED) {
		printf("ICM quota:          unsupported\n");
		result = DOCA_SUCCESS;
		goto out_dev_ctx_destroy;
	} else if (result == DOCA_SUCCESS) {
		printf("ICM quota:          supported\n");
	} else {
		DOCA_LOG_ERR("Failed to check ICM quota is supported: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Get the maximum ICM quota limit */
	result = doca_mgmt_cap_icm_quota_get_max_limit(dev_ctx, &max_limit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get ICM quota max limit: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}
	format_size(max_limit, size_str, sizeof(size_str), true);
	printf("max limit:          %s\n", size_str);

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}
