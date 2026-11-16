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

#ifndef MEMOS_FIO_PLUGIN_DEV_H_
#define MEMOS_FIO_PLUGIN_DEV_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio_plugin_dev.h
 * @brief NVMe kernel KV device (doca_kv_nvme_kernel_dev) lifecycle for the fio plugin.
 *
 * NGUID is read from the device at create time; it is not passed in by the caller.
 */

struct doca_kv_nvme_kernel_dev;
struct doca_pe;

/** Input for memos_fio_plugin_dev_create(). */
struct memos_fio_plugin_dev_attr {
	/** NVMe character device path (e.g. "/dev/ng0n1"). */
	const char *path;
};

/**
 * @brief Create a doca_kv_nvme_kernel_dev for the given path.
 *
 * Opens the path, queries namespace NGUID, and configures the KV device.
 */
doca_error_t memos_fio_plugin_dev_create(const struct memos_fio_plugin_dev_attr *attr,
					 struct doca_kv_nvme_kernel_dev **nkvdev);

/** Destroy a device created by memos_fio_plugin_dev_create(). */
doca_error_t memos_fio_plugin_dev_destroy(struct doca_kv_nvme_kernel_dev *nkvdev);

/** No-op: a device attaches in memos_fio_plugin_dev_create(). Kept for its call sites. */
doca_error_t memos_fio_plugin_dev_start(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe);

/** No-op: a device detaches in memos_fio_plugin_dev_destroy(). Kept for its call sites. */
doca_error_t memos_fio_plugin_dev_stop(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe);

/** Succeeds for any created device: there is no started state to wait for. */
doca_error_t memos_fio_plugin_dev_wait_running(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe);

/** Succeeds for any created device: idleness is a property of the I/O context. */
doca_error_t memos_fio_plugin_dev_wait_idle(struct doca_kv_nvme_kernel_dev *nkvdev, struct doca_pe *pe);

/** Return namespace capacity in bytes (doca_kv_dev_get_size). */
doca_error_t memos_fio_plugin_dev_get_kvdev_size(struct doca_kv_nvme_kernel_dev *nkvdev, uint64_t *size);

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_PLUGIN_DEV_H_ */
