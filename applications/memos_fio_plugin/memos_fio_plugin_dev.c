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
#include <string.h>

#include <doca_log.h>
#include <doca_kv_dev.h>
#include <doca_kv_nvme_kernel_dev.h>

#include "memos_fio_utils.h"
#include "memos_fio_plugin_dev.h"

DOCA_LOG_REGISTER(MEMOS_FIO_PLUGIN_DEV);

/** Buffer size for the hex rendering of an NGUID, including the terminating NUL. */
#define MEMOS_FIO_PLUGIN_DEV_NGUID_STR_LEN (2 * DOCA_KV_DEV_NGUID_LEN + 1)

/**
 * An NGUID is fixed-length binary data and is not NUL-terminated, so it must never be logged with %s
 * directly. Render it as a bounded hex string instead.
 */
static void memos_fio_plugin_dev_nguid_to_str(const uint8_t nguid[DOCA_KV_DEV_NGUID_LEN],
					      char str[MEMOS_FIO_PLUGIN_DEV_NGUID_STR_LEN])
{
	for (uint16_t i = 0; i < DOCA_KV_DEV_NGUID_LEN; i++)
		(void)snprintf(&str[2 * i], 3, "%02x", nguid[i]);
}

doca_error_t memos_fio_plugin_dev_create(const struct memos_fio_plugin_dev_attr *attr,
					 struct doca_kv_nvme_kernel_dev **nkvdev)
{
	doca_error_t result;
	struct doca_kv_nvme_kernel_dev *local_nkvdev = NULL;
	struct doca_kv_nvme_kernel_dev_create_attr *dev_attr = NULL;
	uint8_t nguid[DOCA_KV_DEV_NGUID_LEN];
	uint16_t nguid_len = DOCA_KV_DEV_NGUID_LEN;
	char nguid_str[MEMOS_FIO_PLUGIN_DEV_NGUID_STR_LEN];

	RETURN_INVALID_ON_NULL(attr);
	RETURN_INVALID_ON_NULL(nkvdev);
	RETURN_INVALID_ON_NULL(attr->path);

	if (!memos_fio_utils_get_nguid(attr->path, nguid, nguid_len)) {
		DOCA_LOG_ERR("Failed to get NGUID for %s", attr->path);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_kv_nvme_kernel_dev_create_attr_create(&dev_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA KV device attributes: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_nvme_kernel_dev_create_attr_set_path(dev_attr, attr->path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set device path '%s': %s", attr->path, doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_dev_create_attr_set_nguid(doca_kv_nvme_kernel_dev_create_attr_as_kv_dev_create_attr(dev_attr),
						   nguid);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set NGUID: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_kv_nvme_kernel_dev_create(dev_attr, &local_nkvdev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA NVMe kernel KV device: %s", doca_error_get_descr(result));
		goto fail;
	}

	(void)doca_kv_nvme_kernel_dev_create_attr_destroy(dev_attr);
	dev_attr = NULL;

	memos_fio_plugin_dev_nguid_to_str(nguid, nguid_str);
	DOCA_LOG_DBG("nkvdev=%p nguid=0x%s - CREATED", local_nkvdev, nguid_str);

	*nkvdev = local_nkvdev;
	return DOCA_SUCCESS;

fail:
	if (dev_attr != NULL)
		(void)doca_kv_nvme_kernel_dev_create_attr_destroy(dev_attr);
	if (local_nkvdev != NULL)
		(void)doca_kv_nvme_kernel_dev_destroy(local_nkvdev);
	return result;
}

doca_error_t memos_fio_plugin_dev_destroy(struct doca_kv_nvme_kernel_dev *nkvdev)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;
	struct doca_kv_dev *kvdev;

	RETURN_INVALID_ON_NULL(nkvdev);

	kvdev = doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev);
	if (kvdev == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV device interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Destroy fails with DOCA_ERROR_IN_USE while any I/O context is still bound. */
	doca_err = doca_kv_nvme_kernel_dev_destroy(nkvdev);
	if (doca_err != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);

	DOCA_LOG_DBG("nkvdev (%p) - DESTROYED", nkvdev);
	return ret_err;
}

doca_error_t memos_fio_plugin_dev_start(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe)
{
	RETURN_INVALID_ON_NULL(nkvdev);
	(void)pe;

	DOCA_LOG_DBG("nkvdev (%p) - STARTED (no-op)", nkvdev);
	return DOCA_SUCCESS;
}

doca_error_t memos_fio_plugin_dev_stop(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe)
{
	RETURN_INVALID_ON_NULL(nkvdev);
	(void)pe;

	DOCA_LOG_DBG("nkvdev (%p) - STOPPED (no-op)", nkvdev);
	return DOCA_SUCCESS;
}

doca_error_t memos_fio_plugin_dev_wait_running(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe)
{
	struct doca_kv_dev *kvdev;

	(void)pe;

	RETURN_INVALID_ON_NULL(nkvdev);

	kvdev = doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev);
	if (kvdev == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV device interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_DBG("nkvdev (%p) - RUNNING", nkvdev);
	return DOCA_SUCCESS;
}

doca_error_t memos_fio_plugin_dev_wait_idle(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe)
{
	struct doca_kv_dev *kvdev;

	(void)pe;

	RETURN_INVALID_ON_NULL(nkvdev);

	kvdev = doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev);
	if (kvdev == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV device interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_DBG("nkvdev (%p) - STOPPED (IDLE)", nkvdev);

	return DOCA_SUCCESS;
}

doca_error_t memos_fio_plugin_dev_get_kvdev_size(struct doca_kv_nvme_kernel_dev *nkvdev, uint64_t *size)
{
	const struct doca_kv_dev *kvdev;
	doca_error_t result;

	RETURN_INVALID_ON_NULL(nkvdev);
	RETURN_INVALID_ON_NULL(size);

	kvdev = doca_kv_nvme_kernel_dev_as_kv_dev(nkvdev);
	if (kvdev == NULL) {
		DOCA_LOG_ERR("Failed to get DOCA KV device interface");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_kv_dev_get_size(kvdev, size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DOCA KV device size: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}
