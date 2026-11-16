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

#include <doca_ctx.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <doca_kv_io.h>
#include <doca_kv_nvme_kernel_dev.h>
#include <doca_kv_nvme_kernel_io.h>

#include "memos_fio_utils.h"
#include "memos_fio_plugin_defs.h"
#include "memos_fio_plugin_dev_io.h"

DOCA_LOG_REGISTER(MEMOS_FIO_PLUGIN_DEV_IO);

doca_error_t memos_fio_plugin_dev_io_create(const struct memos_fio_plugin_dev_io_attr *attr,
					    struct doca_kv_nvme_kernel_io **kio)
{
	doca_error_t result;
	struct doca_kv_nvme_kernel_io *local_kio = NULL;
	struct doca_kv_io *kv_io = NULL;

	RETURN_INVALID_ON_NULL(attr);
	RETURN_INVALID_ON_NULL(kio);
	RETURN_INVALID_ON_NULL(attr->kvdev);
	RETURN_INVALID_ON_NULL(attr->pe);

	result = doca_kv_nvme_kernel_io_create(&local_kio);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA NVMe kernel KV I/O context: %s", doca_error_get_descr(result));
		goto fail;
	}

	kv_io = doca_kv_nvme_kernel_io_as_kv_io(local_kio);
	if (kv_io == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV I/O context interface");
		result = DOCA_ERROR_DRIVER;
		goto fail;
	}

	result = doca_pe_connect_ctx(attr->pe, doca_kv_io_as_ctx(kv_io));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to KV I/O context: %s", doca_error_get_name(result));
		goto fail;
	}

	result = doca_kv_io_set_num_tasks(kv_io, attr->num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set number of tasks: %s", doca_error_get_descr(result));
		goto fail;
	}

	/* The context refuses to start unless every task type has both callbacks. */
	result = doca_kv_io_task_store_register_completion_cb(
		kv_io,
		(doca_kv_io_task_store_completion_cb_t)attr->store_completion_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register store task completion callback: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_io_task_store_register_error_cb(kv_io,
							 (doca_kv_io_task_store_completion_cb_t)attr->store_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register store task error callback: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_io_task_retrieve_register_completion_cb(
		kv_io,
		(doca_kv_io_task_retrieve_completion_cb_t)attr->retrieve_completion_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register retrieve task completion callback: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_io_task_retrieve_register_error_cb(
		kv_io,
		(doca_kv_io_task_retrieve_completion_cb_t)attr->retrieve_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register retrieve task error callback: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_io_task_exist_register_completion_cb(
		kv_io,
		(doca_kv_io_task_exist_completion_cb_t)attr->exist_completion_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register exist task completion callback: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_io_task_exist_register_error_cb(kv_io,
							 (doca_kv_io_task_exist_completion_cb_t)attr->exist_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register exist task error callback: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_ctx_set_user_data(doca_kv_io_as_ctx(kv_io), attr->user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set user data: %s", doca_error_get_descr(result));
		goto fail;
	}

	if (attr->state_changed_cb) {
		result = doca_ctx_set_state_changed_cb(doca_kv_io_as_ctx(kv_io),
						       (doca_ctx_state_changed_callback_t)attr->state_changed_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set state changed callback: %s", doca_error_get_descr(result));
			goto fail;
		}
	}

	DOCA_LOG_DBG("kvdev_io=%p udata=%p - CREATED", local_kio, attr->user_data.ptr);

	*kio = local_kio;
	return DOCA_SUCCESS;

fail:
	if (local_kio != NULL)
		(void)memos_fio_plugin_dev_io_destroy(local_kio);
	return result;
}

doca_error_t memos_fio_plugin_dev_io_destroy(struct doca_kv_nvme_kernel_io *kio)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;
	struct doca_kv_io *kv_io;

	RETURN_INVALID_ON_NULL(kio);

	kv_io = doca_kv_nvme_kernel_io_as_kv_io(kio);
	if (kv_io == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV I/O context interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!memos_fio_utils_check_ctx_state(doca_kv_io_as_ctx(kv_io),
					     DOCA_CTX_STATE_IDLE,
					     "Failed to destroy DOCA KV I/O context, stop it first")) {
		return DOCA_ERROR_BAD_STATE;
	}

	doca_err = doca_kv_nvme_kernel_io_destroy(kio);
	if (doca_err != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);

	return ret_err;
}

doca_error_t memos_fio_plugin_dev_io_start(struct doca_kv_nvme_kernel_io *kio,
					   struct doca_kv_nvme_kernel_dev *nkvdev,
					   struct doca_pe *pe)
{
	doca_error_t result;
	struct doca_kv_io *kv_io;
	struct doca_ctx *ctx;
	bool ctx_started = false;
	bool dev_bound = false;

	RETURN_INVALID_ON_NULL(kio);

	kv_io = doca_kv_nvme_kernel_io_as_kv_io(kio);
	if (kv_io == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV I/O context interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx = doca_kv_io_as_ctx(kv_io);
	if (!memos_fio_utils_check_ctx_state(ctx,
					     DOCA_CTX_STATE_IDLE,
					     "Failed to start DOCA KV I/O context, bad state")) {
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start KV I/O context: %s", doca_error_get_descr(result));
		goto fail;
	}
	ctx_started = true;

	/* Devices are bound only once the context is running. */
	result = doca_kv_io_bind_dev(doca_kv_nvme_kernel_io_as_kv_io(kio), doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind device to KV I/O context: %s", doca_error_get_descr(result));
		goto fail;
	}
	dev_bound = true;

	DOCA_LOG_DBG("kio (%p) - STARTED and bound", kio);

	if (pe) {
		result = memos_fio_utils_wait_state(ctx, pe, DOCA_CTX_STATE_RUNNING);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to wait for DOCA KV I/O context to start: %s",
				     doca_error_get_descr(result));
			goto fail;
		}
	}

	DOCA_LOG_DBG("kio (%p) - RUNNING", kio);

	return DOCA_SUCCESS;

fail:
	/*
	 * Never hand back a running context on an error return: the caller has no handle
	 * on which to stop it, and doca_ctx_stop() would later refuse while still bound.
	 */
	if (dev_bound)
		(void)doca_kv_io_unbind_dev(doca_kv_nvme_kernel_io_as_kv_io(kio),
					    doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev));

	if (ctx_started) {
		(void)doca_ctx_stop(ctx);
		if (pe != NULL)
			(void)memos_fio_utils_wait_state(ctx, pe, DOCA_CTX_STATE_IDLE);
	}

	return result;
}

doca_error_t memos_fio_plugin_dev_io_stop(struct doca_kv_nvme_kernel_io *kio,
					  struct doca_kv_nvme_kernel_dev *nkvdev,
					  struct doca_pe *pe)
{
	doca_error_t result;
	struct doca_kv_io *kv_io;
	struct doca_ctx *ctx;

	RETURN_INVALID_ON_NULL(kio);

	kv_io = doca_kv_nvme_kernel_io_as_kv_io(kio);
	if (kv_io == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV I/O context interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx = doca_kv_io_as_ctx(kv_io);

	if (!memos_fio_utils_check_ctx_state(ctx, DOCA_CTX_STATE_RUNNING, NULL)) {
		DOCA_LOG_DBG("kio (%p) - No need to stop, already in IDLE state", kio);
		return DOCA_SUCCESS;
	}

	/*
	 * Every device must be unbound before the context can stop: doca_ctx_stop()
	 * fails with DOCA_ERROR_IN_USE while any binding remains.
	 */
	if (nkvdev != NULL) {
		result = doca_kv_io_unbind_dev(doca_kv_nvme_kernel_io_as_kv_io(kio),
					       doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev));
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_NOT_FOUND) {
			DOCA_LOG_ERR("Failed to unbind device from KV I/O context: %s", doca_error_get_descr(result));
			goto fail;
		}
	}

	DOCA_LOG_DBG("kio (%p) - STOP initiated", kio);

	result = doca_ctx_stop(ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to stop DOCA KV I/O context: %s", doca_error_get_descr(result));
		goto fail;
	}

	if (pe) {
		result = memos_fio_utils_wait_state(ctx, pe, DOCA_CTX_STATE_IDLE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to wait for DOCA KV I/O context to stop: %s",
				     doca_error_get_descr(result));
			goto fail;
		}

		DOCA_LOG_DBG("kio (%p) - STOPPED", kio);
	}

	return DOCA_SUCCESS;

fail:
	return result;
}
