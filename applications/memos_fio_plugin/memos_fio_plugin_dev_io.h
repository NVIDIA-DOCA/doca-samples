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

#ifndef MEMOS_FIO_PLUGIN_DEV_IO_H_
#define MEMOS_FIO_PLUGIN_DEV_IO_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>
#include <doca_types.h>

#include "memos_fio_plugin_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio_plugin_dev_io.h
 * @brief Per-thread doca_kv_nvme_kernel_io lifecycle and callbacks.
 */

struct doca_pe;
struct doca_kv_nvme_kernel_io;
struct doca_kv_nvme_kernel_dev;

/** Configuration for memos_fio_plugin_dev_io_create(). */
struct memos_fio_plugin_dev_io_attr {
	struct memos_fio_plugin_dev *kvdev; /**< Namespace device (uses kvdev->kvdev). */
	struct doca_pe *pe;		    /**< Progress engine for this I/O context. */
	union doca_data user_data;	    /**< Passed to doca_ctx_set_user_data (thread pointer). */
	void *store_completion_cb;	    /**< doca_kv_io_task_store_completion_cb_t on success. */
	void *store_error_cb;		    /**< doca_kv_io_task_store_completion_cb_t on error. */
	void *retrieve_completion_cb;	    /**< doca_kv_io_task_retrieve_completion_cb_t on success. */
	void *retrieve_error_cb;	    /**< doca_kv_io_task_retrieve_completion_cb_t on error. */
	void *exist_completion_cb;	    /**< doca_kv_io_task_exist_completion_cb_t on success. */
	void *exist_error_cb;		    /**< doca_kv_io_task_exist_completion_cb_t on error. */
	void *state_changed_cb;		    /**< doca_ctx state-changed callback. */
	uint32_t num_tasks;		    /**< Task pool size (typically fio iodepth). */
};

/** Create and configure a doca_kv_nvme_kernel_io for one namespace on a thread. */
doca_error_t memos_fio_plugin_dev_io_create(const struct memos_fio_plugin_dev_io_attr *attr,
					    struct doca_kv_nvme_kernel_io **kio);

/** Destroy I/O context; it must be idle, with every bound device already unbound. */
doca_error_t memos_fio_plugin_dev_io_destroy(struct doca_kv_nvme_kernel_io *kio);

/**
 * @brief Start KV I/O context (doca_ctx_start).
 *
 * If @p pe is not NULL, waits until the context reaches running state.
 */
doca_error_t memos_fio_plugin_dev_io_start(struct doca_kv_nvme_kernel_io *kio,
					   struct doca_kv_nvme_kernel_dev *nkvdev,
					   struct doca_pe *pe);

/**
 * @brief Stop KV I/O context (doca_ctx_stop).
 *
 * If @p pe is not NULL, waits until the context reaches idle state.
 */
doca_error_t memos_fio_plugin_dev_io_stop(struct doca_kv_nvme_kernel_io *kio,
					  struct doca_kv_nvme_kernel_dev *nkvdev,
					  struct doca_pe *pe);

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_PLUGIN_DEV_IO_H_ */
