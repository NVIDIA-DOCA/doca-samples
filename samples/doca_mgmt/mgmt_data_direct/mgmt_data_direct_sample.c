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

#include <doca_error.h>
#include <doca_log.h>
#include <doca_mgmt.h>
#include <doca_mgmt_device_caps_general.h>

DOCA_LOG_REGISTER(MGMT_DATA_DIRECT::SAMPLE);

doca_error_t mgmt_data_direct_get(struct doca_dev *dev, struct doca_dev_rep *dev_rep)
{
	struct doca_mgmt_dev_ctx *ctx;
	struct doca_mgmt_dev_rep_ctx *rep_ctx;
	struct doca_mgmt_device_caps_general *device_caps_general;
	uint8_t data_direct;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create the DOCA management device representor context */
	result = doca_mgmt_dev_rep_ctx_create(ctx, dev_rep, &rep_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device representor context: %s",
			     doca_error_get_descr(result));
		goto out;
	}

	/* Create the device caps general handle */
	result = doca_mgmt_device_caps_general_create(&device_caps_general);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create device caps general handle: %s", doca_error_get_descr(result));
		goto out_rep_ctx_destroy;
	}

	/* Execute the get command */
	result = doca_mgmt_device_caps_general_get(rep_ctx, device_caps_general);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get device caps general configuration: %s", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

	/* Get the data direct attribute */
	result = doca_mgmt_device_caps_general_get_data_direct(device_caps_general, &data_direct);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get data direct attribute: %s", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

	DOCA_LOG_INFO("Data direct: %s", data_direct ? "ENABLED" : "DISABLED");

out_device_caps_general_destroy:
	if (doca_mgmt_device_caps_general_destroy(device_caps_general) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy device caps general handle");

out_rep_ctx_destroy:
	if (doca_mgmt_dev_rep_ctx_destroy(rep_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device representor context");

out:
	if (doca_mgmt_dev_ctx_destroy(ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}

doca_error_t mgmt_data_direct_set(struct doca_dev *dev, struct doca_dev_rep *dev_rep, bool enabled)
{
	struct doca_mgmt_dev_ctx *ctx;
	struct doca_mgmt_dev_rep_ctx *rep_ctx;
	struct doca_mgmt_device_caps_general *device_caps_general;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create the DOCA management device representor context */
	result = doca_mgmt_dev_rep_ctx_create(ctx, dev_rep, &rep_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device representor context: %s",
			     doca_error_get_descr(result));
		goto out;
	}

	/* Create the device caps general handle */
	result = doca_mgmt_device_caps_general_create(&device_caps_general);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create device caps general handle: %s", doca_error_get_descr(result));
		goto out_rep_ctx_destroy;
	}

	/* Set the attributes */
	result = doca_mgmt_device_caps_general_set_data_direct(device_caps_general, enabled);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set data direct attribute: %s", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

	/* Execute the set command */
	result = doca_mgmt_device_caps_general_set(rep_ctx, device_caps_general);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set device caps general configuration: %s", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

out_device_caps_general_destroy:
	if (doca_mgmt_device_caps_general_destroy(device_caps_general) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy device caps general handle");

out_rep_ctx_destroy:
	if (doca_mgmt_dev_rep_ctx_destroy(rep_ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device representor context");

out:
	if (doca_mgmt_dev_ctx_destroy(ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

	return result;
}