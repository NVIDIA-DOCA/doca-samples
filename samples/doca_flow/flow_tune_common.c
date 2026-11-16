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

#include <doca_log.h>

#include <doca_flow_tune_server.h>

#include "flow_tune_common.h"

DOCA_LOG_REGISTER(FLOW_TUNE_COMMON);

static bool tune_server_started;

doca_error_t flow_tune_configure(struct doca_flow_cfg *flow_cfg, enum doca_flow_tune_profile profile)
{
	struct doca_flow_tune_cfg *tune_cfg;
	doca_error_t result;

	if (profile == DOCA_FLOW_TUNE_PROFILE_NONE)
		return DOCA_SUCCESS;

	result = doca_flow_tune_cfg_create(&tune_cfg);
	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_NOT_SUPPORTED)
			DOCA_LOG_ERR("DOCA Flow Tune was requested but isn't supported in this runtime version");
		else
			DOCA_LOG_ERR("Failed to create doca_flow_tune_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_tune_cfg_set_profile(tune_cfg, profile);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_tune_cfg profile: %s", doca_error_get_descr(result));
		goto destroy_tune_cfg;
	}

	result = doca_flow_cfg_set_tune_cfg(flow_cfg, tune_cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to set doca_flow_cfg tune_cfg: %s", doca_error_get_descr(result));

destroy_tune_cfg:
	doca_flow_tune_cfg_destroy(tune_cfg);
	return result;
}

doca_error_t flow_tune_server_start(enum doca_flow_tune_profile profile)
{
	struct doca_flow_tune_server_cfg *server_cfg;
	doca_error_t result;

	if (profile == DOCA_FLOW_TUNE_PROFILE_NONE)
		return DOCA_SUCCESS;

	result = doca_flow_tune_server_cfg_create(&server_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_tune_server_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_tune_server_init(server_cfg);
	if (result == DOCA_SUCCESS)
		tune_server_started = true;
	else if (result == DOCA_ERROR_NOT_SUPPORTED)
		DOCA_LOG_ERR("DOCA Flow Tune Server was requested but isn't supported in this runtime version");
	else
		DOCA_LOG_ERR("Failed to initialize doca_flow_tune_server: %s", doca_error_get_descr(result));

	doca_flow_tune_server_cfg_destroy(server_cfg);
	return result;
}

void flow_tune_server_stop(void)
{
	if (!tune_server_started)
		return;

	doca_flow_tune_server_destroy();
	tune_server_started = false;
}
