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

#ifndef MEMOS_FIO_PLUGIN_DEFS_H_
#define MEMOS_FIO_PLUGIN_DEFS_H_

#include <stdint.h>

#include <doca_log.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio_plugin_defs.h
 * @brief Shared limits, defaults, and types for the memos_fio fio ioengine.
 */

/** Maximum fio threads (numjobs) the plugin supports. */
#define MEMOS_FIO_PLUGIN_IO_COUNT_MAX 32

/** Maximum namespaces parsed from filename= (colon-separated /dev/ngXnY paths). */
#define MEMOS_FIO_PLUGIN_NSID_COUNT_MAX 8

/** Buffer size for one NVMe character device path (e.g. "/dev/ng0n1"). */
#define MEMOS_FIO_PLUGIN_NSID_PATH_LEN_MAX 64

/** Default per-thread queue depth when fio iodepth is not set. */
#define MEMOS_FIO_PLUGIN_DEFAULT_IO_Q_DEPTH 16

/** Upper bound used when validating engine options at init. */
#define MEMOS_FIO_PLUGIN_DEFAULT_IO_Q_DEPTH_MAX 256

/** Default DOCA_LOG_* lower limit for plugin messages. */
#define MEMOS_FIO_PLUGIN_DEFAULT_FIO_LOG_LEVEL DOCA_LOG_LEVEL_INFO

/** Default DOCA SDK (internal library) log limit. */
#define MEMOS_FIO_PLUGIN_DEFAULT_DOCA_LOG_LEVEL DOCA_LOG_LEVEL_WARNING

/** Fixed KV key size (bytes) for memos-backed namespaces. */
#define MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE 8U

/** @deprecated Use MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE. */
#define MEMOS_FIO_PLUGIN_KEY_SIZE MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE

/**
 * When set to 1, the last fio thread exiting triggers full global teardown.
 * When 0, each thread may stop its own KV I/O contexts on cleanup.
 */
#define MEMOS_FIO_PLUGIN_SYNC_CLEANUP_ON_THREAD_EXIT 1

/** Maximum iovecs per KV store/retrieve (fio engine option kv_iovcnt; array size in task_ctx). */
#define MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX 56

/**
 * Default kv_iovcnt when the engine option is unset: a single iovec spanning the whole io_u buffer.
 * Any value in [1, MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX] is accepted; the buffer is split evenly and
 * the last iovec absorbs the remainder, so the block size must be at least kv_iovcnt bytes.
 */
#define MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_DEFAULT 1

/* Expand a numeric limit into a string literal for fio option defaults and help text. */
#define MEMOS_FIO_PLUGIN_STRINGIFY_(x) #x
#define MEMOS_FIO_PLUGIN_STRINGIFY(x) MEMOS_FIO_PLUGIN_STRINGIFY_(x)

struct doca_kv_nvme_kernel_dev;
struct doca_kv_nvme_kernel_io;

/** One opened NVMe kernel KV device bound to a namespace path. */
struct memos_fio_plugin_dev {
	struct doca_kv_nvme_kernel_dev *kvdev;
	/** NVMe character device path (e.g. "/dev/ng0n1"); matches a segment from filename=. */
	const char *ns_file_name;
};

/**
 * Per-namespace I/O binding for a fio thread: device plus that thread's kvdev_io context.
 * Stored in fio FILE_ENG_DATA after open_file.
 */
struct memos_fio_plugin_ns {
	struct memos_fio_plugin_dev kvdev;
	struct doca_kv_nvme_kernel_io *kvdev_io;
};

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_PLUGIN_DEFS_H_ */
