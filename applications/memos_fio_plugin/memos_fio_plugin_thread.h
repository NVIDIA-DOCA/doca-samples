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

#ifndef MEMOS_FIO_PLUGIN_THREAD_H_
#define MEMOS_FIO_PLUGIN_THREAD_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

#include <doca_error.h>
#include <doca_types.h>

#include "memos_fio_plugin_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio_plugin_thread.h
 * @brief Per-fio-thread state: progress engine, async KV ops, and completion batching.
 *
 * Maps fio READ/WRITE to KV retrieve/store. Completions are collected for getevents/event.
 */

struct doca_pe;
struct memos_fio_plugin_thread;

/** One completed async operation (status + user_data from the DOCA task). */
struct memos_fio_plugin_thread_cpl_data {
	union doca_data udata; /**< Typically points to struct io_u. */
	doca_error_t status;
};

/** Arguments for memos_fio_plugin_thread_create(). */
struct memos_fio_plugin_create_thread_attr {
	struct memos_fio_plugin_dev *kvdevs; /**< Array of namespace devices for this thread. */
	uint32_t kvdevs_count;		     /**< Length of kvdevs (<= MEMOS_FIO_PLUGIN_NSID_COUNT_MAX). */
	struct doca_pe *pe;		     /**< Shared PE; NULL creates a thread-private progress engine. */
	uint32_t tidx;			     /**< fio thread index (0 .. numjobs-1). */
	uint32_t q_depth;		     /**< Queue depth / DOCA task pool size. */
	uint32_t iov_entry_count;	     /**< iovecs per I/O slot (kv_iovcnt). */
	uint32_t hash_type;		     /**< Offset-to-key hash (memos_fio_plugin_hash_type). */
	bool create_kv_io; /**< If true, create kvdev_io contexts during create; else defer to start. */
};

/** Allocate thread context; optionally creates per-namespace kvdev_io. */
doca_error_t memos_fio_plugin_thread_create(const struct memos_fio_plugin_create_thread_attr *attr,
					    struct memos_fio_plugin_thread **thread);

/** Stop (if started), destroy kvdev_io, free thread. */
doca_error_t memos_fio_plugin_thread_destroy(struct memos_fio_plugin_thread *thread);

/** Start all per-namespace kvdev_io contexts. */
doca_error_t memos_fio_plugin_thread_start(struct memos_fio_plugin_thread *thread);

/** Stop all kvdev_io contexts on this thread. */
doca_error_t memos_fio_plugin_thread_stop(struct memos_fio_plugin_thread *thread);

/** Return thread index from create attr. */
uint32_t memos_fio_plugin_thread_get_tidx(const struct memos_fio_plugin_thread *thread);

/** Return progress engine used to complete KV tasks for this thread. */
struct doca_pe *memos_fio_plugin_thread_get_pe(struct memos_fio_plugin_thread *thread);

/** Drive async completions (doca_pe_progress). Call from getevents loop. */
void memos_fio_plugin_thread_progress(struct memos_fio_plugin_thread *thread);

/**
 * @brief Submit async KV retrieve (fio READ).
 *
 * @p io_offset is expanded to an 8-byte KV key per thread @c hash_type. @p io_idx selects the iovec slot.
 * @p io_buf is written by the device with the retrieved value, so it must be writable.
 */
doca_error_t memos_fio_plugin_thread_start_retrieve_op(struct memos_fio_plugin_thread *thread,
						       const struct memos_fio_plugin_ns *fio_ns,
						       union doca_data user_data,
						       uint64_t io_offset,
						       uint8_t *io_buf,
						       uint64_t io_buflen,
						       uint32_t io_idx);

/**
 * @brief Submit async KV store (fio WRITE).
 *
 * @p io_offset is expanded to an 8-byte KV key per thread @c hash_type. @p io_idx selects the iovec slot.
 */
doca_error_t memos_fio_plugin_thread_start_store_op(struct memos_fio_plugin_thread *thread,
						    const struct memos_fio_plugin_ns *fio_ns,
						    union doca_data user_data,
						    uint64_t io_offset,
						    uint8_t *io_buf,
						    uint64_t io_buflen,
						    uint32_t io_idx);

/** Completion entry by index for memos_fio_plugin_event() (0 .. get_cpls()-1). */
const struct memos_fio_plugin_thread_cpl_data *memos_fio_plugin_thread_get_cpl_data(
	const struct memos_fio_plugin_thread *thread,
	uint32_t index);

/** Reset completion count at the start of a getevents batch. */
void memos_fio_plugin_thread_reset_cpl_data(struct memos_fio_plugin_thread *thread);

/** Number of completions recorded since the last reset. */
uint32_t memos_fio_plugin_thread_get_cpls(const struct memos_fio_plugin_thread *thread);

/** Look up namespace binding by device path (matches fio file_name). */
const struct memos_fio_plugin_ns *memos_fio_plugin_thread_get_ns(const struct memos_fio_plugin_thread *thread,
								 const char *nsid_file_name);

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_PLUGIN_THREAD_H_ */
