/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_ctx.h>
#include <doca_log.h>

#include "urom_common.h"

DOCA_LOG_REGISTER(UROM::SAMPLES : COMMON);

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_address_callback(void *param, void *config)
{
	struct urom_common_cfg *urom_cfg = (struct urom_common_cfg *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(urom_cfg->device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

doca_error_t register_urom_common_params(void)
{
	doca_error_t result;
	struct doca_argp_param *device_param;

	/* Create and register device param */
	result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<IB device name>");
	doca_argp_param_set_description(device_param, "IB device name.");
	doca_argp_param_set_callback(device_param, device_address_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t start_urom_service(struct doca_pe *pe,
				struct doca_dev *dev,
				uint64_t nb_workers,
				struct doca_urom_service **service)
{
	enum doca_ctx_states state;
	struct doca_urom_service *inst;
	doca_error_t result, tmp_result;

	/* Create service context */
	result = doca_urom_service_create(&inst);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_pe_connect_ctx(pe, doca_urom_service_as_ctx(inst));
	if (result != DOCA_SUCCESS)
		goto service_cleanup;

	result = doca_urom_service_set_max_workers(inst, nb_workers);
	if (result != DOCA_SUCCESS)
		goto service_cleanup;

	result = doca_urom_service_set_dev(inst, dev);
	if (result != DOCA_SUCCESS)
		goto service_cleanup;

	result = doca_ctx_start(doca_urom_service_as_ctx(inst));
	if (result != DOCA_SUCCESS)
		goto service_cleanup;

	result = doca_ctx_get_state(doca_urom_service_as_ctx(inst), &state);
	if (result != DOCA_SUCCESS || state != DOCA_CTX_STATE_RUNNING)
		goto service_stop;

	*service = inst;
	return DOCA_SUCCESS;

service_stop:
	tmp_result = doca_ctx_stop(doca_urom_service_as_ctx(inst));
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop UROM service");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

service_cleanup:
	tmp_result = doca_urom_service_destroy(inst);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM service");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t start_urom_worker(struct doca_pe *pe,
			       struct doca_urom_service *service,
			       uint64_t worker_id,
			       uint32_t *gid,
			       uint64_t nb_tasks,
			       doca_cpu_set_t *cpuset,
			       char **env,
			       size_t env_count,
			       uint64_t plugins,
			       struct doca_urom_worker **worker)
{
	enum doca_ctx_states state;
	struct doca_urom_worker *inst;
	doca_error_t result, tmp_result;

	result = doca_urom_worker_create(&inst);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_urom_worker_set_service(inst, service);
	if (result != DOCA_SUCCESS)
		goto worker_cleanup;

	result = doca_pe_connect_ctx(pe, doca_urom_worker_as_ctx(inst));
	if (result != DOCA_SUCCESS)
		goto worker_cleanup;

	result = doca_urom_worker_set_id(inst, worker_id);
	if (result != DOCA_SUCCESS)
		goto worker_cleanup;

	if (gid != NULL) {
		result = doca_urom_worker_set_gid(inst, *gid);
		if (result != DOCA_SUCCESS)
			goto worker_cleanup;
	}

	if (env != NULL) {
		result = doca_urom_worker_set_env(inst, env, env_count);
		if (result != DOCA_SUCCESS)
			goto worker_cleanup;
	}

	result = doca_urom_worker_set_max_inflight_tasks(inst, nb_tasks);
	if (result != DOCA_SUCCESS)
		goto worker_cleanup;

	result = doca_urom_worker_set_plugins(inst, plugins);
	if (result != DOCA_SUCCESS)
		goto worker_cleanup;

	if (cpuset != NULL) {
		result = doca_urom_worker_set_cpuset(inst, *cpuset);
		if (result != DOCA_SUCCESS)
			goto worker_cleanup;
	}

	result = doca_ctx_start(doca_urom_worker_as_ctx(inst));
	if (result != DOCA_ERROR_IN_PROGRESS)
		goto worker_cleanup;

	result = doca_ctx_get_state(doca_urom_worker_as_ctx(inst), &state);
	if (result != DOCA_SUCCESS)
		goto worker_stop;

	if (state != DOCA_CTX_STATE_STARTING) {
		result = DOCA_ERROR_BAD_STATE;
		goto worker_stop;
	}

	*worker = inst;
	return DOCA_SUCCESS;

worker_stop:
	tmp_result = doca_ctx_stop(doca_urom_worker_as_ctx(inst));
	if (tmp_result != DOCA_SUCCESS && tmp_result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to request stop UROM worker");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	do {
		doca_pe_progress(pe);
		doca_ctx_get_state(doca_urom_worker_as_ctx(inst), &state);
	} while (state != DOCA_CTX_STATE_IDLE);

worker_cleanup:
	tmp_result = doca_urom_worker_destroy(inst);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM worker");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t start_urom_domain(struct doca_pe *pe,
			       struct doca_urom_domain_oob_coll *oob,
			       uint64_t *worker_ids,
			       struct doca_urom_worker **workers,
			       size_t nb_workers,
			       struct urom_domain_buffer_attrs *buffers,
			       size_t nb_buffers,
			       struct doca_urom_domain **domain)
{
	size_t i;
	doca_error_t result, tmp_result;
	enum doca_ctx_states state;
	struct doca_urom_domain *inst;

	result = doca_urom_domain_create(&inst);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create domain");
		return result;
	}

	result = doca_pe_connect_ctx(pe, doca_urom_domain_as_ctx(inst));
	if (result != DOCA_SUCCESS)
		goto domain_destroy;

	result = doca_urom_domain_set_oob(inst, oob);
	if (result != DOCA_SUCCESS)
		goto domain_destroy;

	result = doca_urom_domain_set_workers(inst, worker_ids, workers, nb_workers);
	if (result != DOCA_SUCCESS)
		goto domain_destroy;

	if (nb_workers != 0 && buffers != NULL) {
		result = doca_urom_domain_set_buffers_count(inst, nb_buffers);
		if (result != DOCA_SUCCESS)
			goto domain_destroy;

		for (i = 0; i < nb_buffers; i++) {
			result = doca_urom_domain_add_buffer(inst,
							     buffers[i].buffer,
							     buffers[i].buf_len,
							     buffers[i].memh,
							     buffers[i].memh_len,
							     buffers[i].mkey,
							     buffers[i].mkey_len);
			if (result != DOCA_SUCCESS)
				goto domain_destroy;
		}
	}

	result = doca_ctx_start(doca_urom_domain_as_ctx(inst));
	if (result != DOCA_ERROR_IN_PROGRESS)
		goto domain_stop;

	result = doca_ctx_get_state(doca_urom_domain_as_ctx(inst), &state);
	if (result != DOCA_SUCCESS)
		goto domain_stop;

	if (state != DOCA_CTX_STATE_STARTING) {
		result = DOCA_ERROR_BAD_STATE;
		goto domain_stop;
	}

	*domain = inst;
	return DOCA_SUCCESS;

domain_stop:
	tmp_result = doca_ctx_stop(doca_urom_domain_as_ctx(inst));
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop UROM domain");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

domain_destroy:
	tmp_result = doca_urom_domain_destroy(inst);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM domain");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}
