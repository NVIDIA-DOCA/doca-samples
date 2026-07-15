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
#include <doca_mgmt_cc_global_status.h>

#include "common.h"

DOCA_LOG_REGISTER(MGMT_CC_GLOBAL_STATUS::SAMPLE);

#define CC_GLOBAL_STATUS_PROTOCOL_RP "rp"
#define CC_GLOBAL_STATUS_PROTOCOL_NP "np"

static doca_error_t mgmt_cc_global_status_protocol_type_from_str(const char *protocol,
								 enum doca_mgmt_cc_global_status_protocol *cc_protocol)
{
	if (strcmp(protocol, CC_GLOBAL_STATUS_PROTOCOL_RP) == 0)
		*cc_protocol = DOCA_MGMT_CC_GLOBAL_STATUS_PROTOCOL_RP;
	else if (strcmp(protocol, CC_GLOBAL_STATUS_PROTOCOL_NP) == 0)
		*cc_protocol = DOCA_MGMT_CC_GLOBAL_STATUS_PROTOCOL_NP;
	else
		return DOCA_ERROR_INVALID_VALUE;

	return DOCA_SUCCESS;
}

doca_error_t mgmt_cc_global_status_get(const char *dev_pci_addr, uint8_t priority, char *protocol)
{
	enum doca_mgmt_cc_global_status_protocol cc_protocol;
	struct doca_dev *dev;
	struct doca_mgmt_dev_ctx *ctx;
	struct doca_mgmt_cc_global_status *cc;
	uint8_t enabled;
	doca_error_t result;

	/* Get the congestion control protocol */
	result = mgmt_cc_global_status_protocol_type_from_str(protocol, &cc_protocol);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get congestion control protocol from string: %s", doca_error_get_descr(result));
		return result;
	}

	/* Open the DOCA device */
	result = open_doca_device_with_pci(dev_pci_addr, NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		goto out;
	}

	/* Create the cc global status handle */
	result = doca_mgmt_cc_global_status_create(&cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create cc global status handle: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Set the attributes */
	result = doca_mgmt_cc_global_status_set_priority(cc, priority);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cc global status priority: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	result = doca_mgmt_cc_global_status_set_protocol(cc, cc_protocol);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cc global status protocol: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	/* Execute the get command */
	result = doca_mgmt_cc_global_status_get(ctx, cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get cc global status configuration: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	/* Get the enabled attribute */
	result = doca_mgmt_cc_global_status_get_enabled(cc, &enabled);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get cc global status enabled attribute: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	DOCA_LOG_INFO("Congestion control global status for device %s, priority %d and protocol %s is %s",
		      dev_pci_addr,
		      priority,
		      protocol,
		      enabled ? "enabled" : "disabled");
	result = DOCA_SUCCESS;

out_cc_destroy:
	if (doca_mgmt_cc_global_status_destroy(cc) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy cc global status handle");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

out:
	if (doca_dev_close(dev) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to close DOCA device");

	return result;
}

doca_error_t mgmt_cc_global_status_set(const char *dev_pci_addr, uint8_t priority, char *protocol, bool enabled)
{
	enum doca_mgmt_cc_global_status_protocol cc_protocol;
	struct doca_dev *dev;
	struct doca_mgmt_dev_ctx *ctx;
	struct doca_mgmt_cc_global_status *cc;
	doca_error_t result;

	/* Get the congestion control protocol */
	result = mgmt_cc_global_status_protocol_type_from_str(protocol, &cc_protocol);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get congestion control protocol from string: %s", doca_error_get_descr(result));
		return result;
	}

	/* Open the DOCA device */
	result = open_doca_device_with_pci(dev_pci_addr, NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create(dev, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		goto out;
	}

	/* Create the cc global status handle */
	result = doca_mgmt_cc_global_status_create(&cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create cc global status handle: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Set the attributes */
	result = doca_mgmt_cc_global_status_set_priority(cc, priority);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cc global status priority: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	result = doca_mgmt_cc_global_status_set_protocol(cc, cc_protocol);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cc global status protocol: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	result = doca_mgmt_cc_global_status_set_enabled(cc, enabled);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cc global status enabled: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	/* Execute the set command */
	result = doca_mgmt_cc_global_status_set(ctx, cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cc global status configuration: %s", doca_error_get_descr(result));
		goto out_cc_destroy;
	}

	DOCA_LOG_INFO("Congestion control global status for device %s, priority %d and protocol %s was set to %s",
		      dev_pci_addr,
		      priority,
		      protocol,
		      enabled ? "enabled" : "disabled");
	result = DOCA_SUCCESS;

out_cc_destroy:
	if (doca_mgmt_cc_global_status_destroy(cc) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy cc global status handle");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(ctx) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA management device context");

out:
	if (doca_dev_close(dev) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to close DOCA device");

	return result;
}