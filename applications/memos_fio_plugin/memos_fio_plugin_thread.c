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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <doca_ctx.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <doca_kv_io.h>
#include <doca_kv_nvme_kernel_dev.h>
#include <doca_kv_nvme_kernel_io.h>

#include "memos_fio_plugin_dev_io.h"
#include "memos_fio_plugin_defs.h"
#include "memos_fio_utils.h"
#include "memos_fio_plugin_thread.h"

DOCA_LOG_REGISTER(MEMOS_FIO_PLUGIN_THREAD);

#define MEMOS_FIO_PLUGIN_THREAD_DBG_INFIGHTS 100000

struct iovec_entry {
	struct iovec iov[MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX];
};

/*
 * Per-queue-slot context for an in-flight task. The DOCA task's user_data carries the slot
 * index (io_idx); this entry holds the real fio user_data and enough metadata for the
 * completion path to classify the op and check retrieve length against the submitted bs.
 */
struct memos_fio_plugin_thread_task_ctx {
	union doca_data user_data; /* Typically points to struct io_u. */
	uint64_t expected_len;	   /* Submitted io_buflen (fio bs / xfer length). */
	struct iovec_entry iovec_entry;
};

struct memos_fio_plugin_thread {
	struct memos_fio_plugin_ns ns_arr[MEMOS_FIO_PLUGIN_NSID_COUNT_MAX];
	uint32_t ns_arr_count;

	struct doca_pe *pe;
	struct memos_fio_plugin_thread_cpl_data *cpl_data;
	uint32_t cpls;

	bool pe_owner;

	uint32_t tidx;
	uint32_t q_depth;

	/* Number of iovec elements used per slot (kv_iovcnt). */
	uint32_t iov_entry_count;

	/* Per-io_idx metadata for in-flight retrieve/store tasks (size == q_depth). */
	struct memos_fio_plugin_thread_task_ctx *task_ctx;

	uint32_t inflights;
	uint32_t hash_type;
	bool started;
	enum doca_ctx_states state;
};

static void memos_fio_plugin_thread_task_completion_cb(struct doca_task *task,
						       doca_error_t status,
						       union doca_data task_udata,
						       union doca_data ctx_udata);

static void memos_fio_plugin_thread_task_fail_cb(struct doca_task *task,
						 union doca_data task_udata,
						 union doca_data ctx_udata);

static void memos_fio_plugin_thread_store_completion_cb(struct doca_kv_io_task_store *task,
							union doca_data task_udata,
							union doca_data ctx_udata);

static void memos_fio_plugin_thread_store_fail_cb(struct doca_kv_io_task_store *task,
						  union doca_data task_udata,
						  union doca_data ctx_udata);

static void memos_fio_plugin_thread_retrieve_completion_cb(struct doca_kv_io_task_retrieve *task,
							   union doca_data task_udata,
							   union doca_data ctx_udata);

static void memos_fio_plugin_thread_retrieve_fail_cb(struct doca_kv_io_task_retrieve *task,
						     union doca_data task_udata,
						     union doca_data ctx_udata);

static void memos_fio_plugin_thread_exist_completion_cb(struct doca_kv_io_task_exist *task,
							union doca_data task_udata,
							union doca_data ctx_udata);

static void memos_fio_plugin_thread_exist_fail_cb(struct doca_kv_io_task_exist *task,
						  union doca_data task_udata,
						  union doca_data ctx_udata);

static void memos_fio_plugin_thread_state_changed_cb(const union doca_data udata,
						     struct doca_ctx *ctx,
						     enum doca_ctx_states prev_state,
						     enum doca_ctx_states next_state);

static inline doca_error_t memos_fio_plugin_thread_prepare_task_ctx(struct memos_fio_plugin_thread *thread,
								    uint32_t io_idx,
								    union doca_data user_data,
								    uint64_t expected_len,
								    union doca_data *task_udata)
{
	struct memos_fio_plugin_thread_task_ctx *ctx;

	if (io_idx >= thread->q_depth || thread->task_ctx == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	ctx = &thread->task_ctx[io_idx];
	ctx->user_data = user_data;
	ctx->expected_len = expected_len;

	*task_udata = (union doca_data){.u64 = io_idx};
	return DOCA_SUCCESS;
}

static inline const struct memos_fio_plugin_thread_task_ctx *memos_fio_plugin_thread_lookup_task_ctx(
	const struct memos_fio_plugin_thread *thread,
	union doca_data task_udata)
{
	uint32_t io_idx = (uint32_t)task_udata.u64;

	if (thread->task_ctx == NULL || io_idx >= thread->q_depth)
		return NULL;

	return &thread->task_ctx[io_idx];
}

/* Compare device retrieve length to submitted fio bs; mismatch -> DOCA_ERROR_EMPTY (ENODATA). */
static inline doca_error_t memos_fio_plugin_thread_check_retrieve_result_len(
	const struct memos_fio_plugin_thread *thread,
	struct doca_kv_io_task_retrieve *retrieve_task,
	const struct memos_fio_plugin_thread_task_ctx *task_ctx)
{
	uint32_t result_len = doca_kv_io_task_retrieve_get_result_value_len(retrieve_task);

	if ((uint64_t)result_len == task_ctx->expected_len)
		return DOCA_SUCCESS;

	DOCA_LOG_DBG("Thread #%u (%p): retrieve task=%p result_len=%u != expected_len=%" PRIu64 " -> DOCA_ERROR_EMPTY",
		     thread->tidx,
		     thread,
		     retrieve_task,
		     result_len,
		     task_ctx->expected_len);
	return DOCA_ERROR_EMPTY;
}

static doca_error_t memos_fio_plugin_thread_create_kv_io(struct memos_fio_plugin_thread *thread)
{
	struct memos_fio_plugin_dev_io_attr io_attr;
	doca_error_t result;
	uint32_t kvdevs_count = thread->ns_arr_count;

	for (uint32_t i = 0; i < kvdevs_count; i++) {
		io_attr = (struct memos_fio_plugin_dev_io_attr){
			.kvdev = &thread->ns_arr[i].kvdev,
			.pe = thread->pe,
			.user_data = {.ptr = thread},
			.num_tasks = thread->q_depth,
			.store_completion_cb = memos_fio_plugin_thread_store_completion_cb,
			.store_error_cb = memos_fio_plugin_thread_store_fail_cb,
			.retrieve_completion_cb = memos_fio_plugin_thread_retrieve_completion_cb,
			.retrieve_error_cb = memos_fio_plugin_thread_retrieve_fail_cb,
			.exist_completion_cb = memos_fio_plugin_thread_exist_completion_cb,
			.exist_error_cb = memos_fio_plugin_thread_exist_fail_cb,
			.state_changed_cb = memos_fio_plugin_thread_state_changed_cb,
		};

		result = memos_fio_plugin_dev_io_create(&io_attr, &thread->ns_arr[i].kvdev_io);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create KV I/O context for thread %u: %s",
				     thread->tidx,
				     doca_error_get_descr(result));
			goto fail;
		}

		DOCA_LOG_DBG("kvdev_io - CREATED [PE=%p tidx=%u qdepth=%u user_data=%p]",
			     thread->pe,
			     thread->tidx,
			     thread->q_depth,
			     io_attr.user_data.ptr);
	}

	return DOCA_SUCCESS;

fail:
	return result;
}

static doca_error_t memos_fio_plugin_thread_destroy_kv_io(struct memos_fio_plugin_thread *thread)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	for (uint32_t i = 0; i < thread->ns_arr_count; i++) {
		if (thread->ns_arr[i].kvdev_io == NULL)
			continue;
		doca_err = memos_fio_plugin_dev_io_destroy(thread->ns_arr[i].kvdev_io);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy KV I/O context for thread %u: %s",
				     thread->tidx,
				     doca_error_get_descr(doca_err));
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
		}
	}

	return ret_err;
}

doca_error_t memos_fio_plugin_thread_create(const struct memos_fio_plugin_create_thread_attr *attr,
					    struct memos_fio_plugin_thread **thread)
{
	struct memos_fio_plugin_thread *mtd = NULL;
	doca_error_t result = DOCA_SUCCESS;

	RETURN_INVALID_ON_NULL(attr);
	RETURN_INVALID_ON_NULL(thread);
	RETURN_INVALID_ON_NULL(attr->kvdevs);

	if (attr->q_depth == 0)
		return DOCA_ERROR_INVALID_VALUE;

	if (attr->kvdevs_count == 0 || attr->kvdevs_count > MEMOS_FIO_PLUGIN_NSID_COUNT_MAX) {
		DOCA_LOG_ERR("Invalid kvdevs_count %u for thread %u", attr->kvdevs_count, attr->tidx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	mtd = calloc(1, sizeof(*mtd));
	if (mtd == NULL) {
		DOCA_LOG_ERR("memos_fio_plugin_thread_create: failed to allocate thread context");
		return DOCA_ERROR_NO_MEMORY;
	}

	mtd->cpl_data = calloc(attr->q_depth, sizeof(*mtd->cpl_data));
	if (mtd->cpl_data == NULL) {
		DOCA_LOG_ERR("memos_fio_plugin_thread_create: failed to allocate completion data");
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}

	mtd->task_ctx = calloc(attr->q_depth, sizeof(*mtd->task_ctx));
	if (mtd->task_ctx == NULL) {
		DOCA_LOG_ERR("memos_fio_plugin_thread_create: failed to allocate task context array");
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}

	mtd->cpls = 0;

	if (attr->pe) {
		mtd->pe = attr->pe;
		mtd->pe_owner = false;
	} else {
		result = doca_pe_create(&mtd->pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create progress engine for thread %u: %s",
				     attr->tidx,
				     doca_error_get_descr(result));
			goto fail;
		}
		mtd->pe_owner = true;
	}

	mtd->ns_arr_count = attr->kvdevs_count;
	mtd->q_depth = attr->q_depth;

	for (uint32_t i = 0; i < attr->kvdevs_count; i++) {
		mtd->ns_arr[i].kvdev = attr->kvdevs[i];
	}

	if (attr->create_kv_io) {
		result = memos_fio_plugin_thread_create_kv_io(mtd);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create KV I/O contexts for thread %u: %s",
				     attr->tidx,
				     doca_error_get_descr(result));
			goto fail;
		}
	}

	mtd->tidx = attr->tidx;
	mtd->iov_entry_count = attr->iov_entry_count;
	mtd->hash_type = attr->hash_type;
	mtd->started = false;
	mtd->inflights = 0;

	*thread = mtd;
	return DOCA_SUCCESS;

fail:
	(void)memos_fio_plugin_thread_destroy(mtd);
	return result;
}

doca_error_t memos_fio_plugin_thread_destroy(struct memos_fio_plugin_thread *thread)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	RETURN_INVALID_ON_NULL(thread);

	DOCA_LOG_DBG("THREAD #%u: destroy initiated", thread->tidx);

	doca_err = memos_fio_plugin_thread_destroy_kv_io(thread);
	if (doca_err != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);

	if (thread->pe_owner && thread->pe) {
		doca_err = doca_pe_destroy(thread->pe);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine for thread %u: %s",
				     thread->tidx,
				     doca_error_get_descr(doca_err));
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
		}
	}

	thread->pe = NULL;

	free(thread->cpl_data);
	thread->cpl_data = NULL;

	free(thread->task_ctx);
	thread->task_ctx = NULL;

	DOCA_LOG_DBG("THREAD=%u (%p) - destroyed", thread->tidx, thread);

	free(thread);
	return ret_err;
}

/* A store or exist completion carries no device-reported length to disagree with. */
static void memos_fio_plugin_thread_store_completion_cb(struct doca_kv_io_task_store *task,
							union doca_data task_udata,
							union doca_data ctx_udata)
{
	memos_fio_plugin_thread_task_completion_cb(doca_kv_io_task_store_as_task(task),
						   DOCA_SUCCESS,
						   task_udata,
						   ctx_udata);
}

static void memos_fio_plugin_thread_store_fail_cb(struct doca_kv_io_task_store *task,
						  union doca_data task_udata,
						  union doca_data ctx_udata)
{
	memos_fio_plugin_thread_task_fail_cb(doca_kv_io_task_store_as_task(task), task_udata, ctx_udata);
}

/* A short read is not a device error, so the length is checked here and reported as one. */
static void memos_fio_plugin_thread_retrieve_completion_cb(struct doca_kv_io_task_retrieve *task,
							   union doca_data task_udata,
							   union doca_data ctx_udata)
{
	struct memos_fio_plugin_thread *thread = (struct memos_fio_plugin_thread *)ctx_udata.ptr;
	const struct memos_fio_plugin_thread_task_ctx *task_ctx;
	doca_error_t status = DOCA_SUCCESS;

	task_ctx = memos_fio_plugin_thread_lookup_task_ctx(thread, task_udata);
	if (task_ctx != NULL)
		status = memos_fio_plugin_thread_check_retrieve_result_len(thread, task, task_ctx);

	memos_fio_plugin_thread_task_completion_cb(doca_kv_io_task_retrieve_as_task(task),
						   status,
						   task_udata,
						   ctx_udata);
}

static void memos_fio_plugin_thread_retrieve_fail_cb(struct doca_kv_io_task_retrieve *task,
						     union doca_data task_udata,
						     union doca_data ctx_udata)
{
	memos_fio_plugin_thread_task_fail_cb(doca_kv_io_task_retrieve_as_task(task), task_udata, ctx_udata);
}

static void memos_fio_plugin_thread_exist_completion_cb(struct doca_kv_io_task_exist *task,
							union doca_data task_udata,
							union doca_data ctx_udata)
{
	memos_fio_plugin_thread_task_completion_cb(doca_kv_io_task_exist_as_task(task),
						   DOCA_SUCCESS,
						   task_udata,
						   ctx_udata);
}

static void memos_fio_plugin_thread_exist_fail_cb(struct doca_kv_io_task_exist *task,
						  union doca_data task_udata,
						  union doca_data ctx_udata)
{
	memos_fio_plugin_thread_task_fail_cb(doca_kv_io_task_exist_as_task(task), task_udata, ctx_udata);
}

static void memos_fio_plugin_thread_task_completion_cb(struct doca_task *task,
						       doca_error_t status,
						       union doca_data task_udata,
						       union doca_data ctx_udata)
{
	struct memos_fio_plugin_thread *thread = (struct memos_fio_plugin_thread *)ctx_udata.ptr;
	struct memos_fio_plugin_thread_cpl_data *cpl_data;
	const struct memos_fio_plugin_thread_task_ctx *task_ctx;

	task_ctx = memos_fio_plugin_thread_lookup_task_ctx(thread, task_udata);
	if (task_ctx == NULL) {
		DOCA_LOG_ERR("Thread #%u (%p): task=%p completed with invalid io_idx=%" PRIu64 " (dropping completion)",
			     thread->tidx,
			     thread,
			     task,
			     task_udata.u64);
		if (thread->inflights > 0)
			--thread->inflights;
		doca_task_free(task);
		return;
	}

	cpl_data = &thread->cpl_data[thread->cpls++];
	cpl_data->udata = task_ctx->user_data;

	cpl_data->status = status;

	--thread->inflights;

	DISABLE_DOCA_LOG_DBG("Thread #%u (%p): task=%p completed status=%s (inflights=%u)",
			     thread->tidx,
			     thread,
			     task,
			     doca_error_get_name(cpl_data->status),
			     thread->inflights);

	doca_task_free(task);
}

static void memos_fio_plugin_thread_task_fail_cb(struct doca_task *task,
						 union doca_data task_udata,
						 union doca_data ctx_udata)
{
	struct memos_fio_plugin_thread *thread = (struct memos_fio_plugin_thread *)ctx_udata.ptr;
	struct memos_fio_plugin_thread_cpl_data *cpl_data;
	const struct memos_fio_plugin_thread_task_ctx *task_ctx;
	doca_error_t status = doca_task_get_status(task);

	task_ctx = memos_fio_plugin_thread_lookup_task_ctx(thread, task_udata);
	if (task_ctx == NULL) {
		DOCA_LOG_ERR("Thread #%u (%p): task=%p failed with invalid io_idx=%" PRIu64
			     " status=%s (dropping completion)",
			     thread->tidx,
			     thread,
			     task,
			     task_udata.u64,
			     doca_error_get_name(status));
		if (thread->inflights > 0)
			--thread->inflights;
		doca_task_free(task);
		return;
	}

	cpl_data = &thread->cpl_data[thread->cpls++];
	cpl_data->status = status;
	cpl_data->udata = task_ctx->user_data;

	if (cpl_data->status != DOCA_ERROR_NOT_FOUND)
		DOCA_LOG_ERR("Thread #%u (%p): task=%p failed, status: %s",
			     thread->tidx,
			     thread,
			     task,
			     doca_error_get_name(cpl_data->status));

	--thread->inflights;

	DISABLE_DOCA_LOG_DBG("Thread #%u (%p): task=%p failed, status: %s (inflights=%u)",
			     thread->tidx,
			     thread,
			     task,
			     doca_error_get_name(cpl_data->status),
			     thread->inflights);

	doca_task_free(task);
}

static void memos_fio_plugin_thread_state_changed_cb(const union doca_data udata,
						     struct doca_ctx *ctx,
						     enum doca_ctx_states prev_state,
						     enum doca_ctx_states next_state)
{
	struct memos_fio_plugin_thread *thread = (struct memos_fio_plugin_thread *)udata.ptr;

	(void)ctx;

	thread->state = next_state;

	DOCA_LOG_DBG("Thread #%u (%p): state changed %u=>%u", thread->tidx, thread, prev_state, next_state);
}

doca_error_t memos_fio_plugin_thread_start(struct memos_fio_plugin_thread *thread)
{
	doca_error_t result;

	RETURN_INVALID_ON_NULL(thread);

	if (thread->started)
		return DOCA_SUCCESS;

	if (thread->ns_arr[0].kvdev_io == NULL) {
		/* Create kv_io for the thread */

		result = memos_fio_plugin_thread_create_kv_io(thread);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create KV I/O contexts for thread %u: %s",
				     thread->tidx,
				     doca_error_get_descr(result));
			goto fail_kv_io;
		}
	}

	for (uint32_t i = 0; i < thread->ns_arr_count; i++) {
		if (thread->ns_arr[i].kvdev_io == NULL)
			continue;

		result = memos_fio_plugin_dev_io_start(thread->ns_arr[i].kvdev_io,
						       thread->ns_arr[i].kvdev.kvdev,
						       thread->pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start KV I/O context for thread %u: %s",
				     thread->tidx,
				     doca_error_get_descr(result));

			/*
			 * Unwind the namespaces that did start. thread->started stays false, so
			 * memos_fio_plugin_thread_stop() will not do it for us, and a context
			 * left running can never be destroyed - it also pins its device binding.
			 */
			while (i-- > 0) {
				if (thread->ns_arr[i].kvdev_io == NULL)
					continue;

				(void)memos_fio_plugin_dev_io_stop(thread->ns_arr[i].kvdev_io,
								   thread->ns_arr[i].kvdev.kvdev,
								   thread->pe);
			}

			goto fail_kio;
		}
	}

	thread->started = true;
	return DOCA_SUCCESS;

fail_kio:
fail_kv_io:
	return result;
}

doca_error_t memos_fio_plugin_thread_stop(struct memos_fio_plugin_thread *thread)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	RETURN_INVALID_ON_NULL(thread);

	if (!thread->started)
		return DOCA_SUCCESS;

	DOCA_LOG_DBG("THREAD #%u: stop initiated, inflights=%u", thread->tidx, thread->inflights);

	for (uint32_t i = 0; i < thread->ns_arr_count; i++) {
		if (thread->ns_arr[i].kvdev_io == NULL)
			continue;

		doca_err = memos_fio_plugin_dev_io_stop(thread->ns_arr[i].kvdev_io,
							thread->ns_arr[i].kvdev.kvdev,
							thread->pe);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop KV I/O context for thread %u: %s",
				     thread->tidx,
				     doca_error_get_descr(doca_err));
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
		}
	}

	DOCA_LOG_DBG("THREAD #%u: stop completed, inflights=%u", thread->tidx, thread->inflights);

	thread->started = false;
	return ret_err;
}

void memos_fio_plugin_thread_progress(struct memos_fio_plugin_thread *thread)
{
	if (thread != NULL && thread->pe != NULL)
		doca_pe_progress(thread->pe);
}

struct doca_pe *memos_fio_plugin_thread_get_pe(struct memos_fio_plugin_thread *thread)
{
	return thread ? thread->pe : NULL;
}

uint32_t memos_fio_plugin_thread_get_tidx(const struct memos_fio_plugin_thread *thread)
{
	return thread ? thread->tidx : 0;
}

static doca_error_t memos_fio_plugin_thread_populate_io_iovec(struct memos_fio_plugin_thread *thread,
							      uint32_t io_idx,
							      uint8_t *io_buf,
							      uint64_t io_buflen,
							      const struct iovec **iov,
							      uint32_t *iovcnt)
{
	/* Populate the slot's iovec: split io_buf into iov_entry_count chunks. */

	struct iovec_entry *iov_entry;
	uint32_t iov_entry_count = thread->iov_entry_count;

	if (!(thread->task_ctx && io_buf && io_buflen))
		return DOCA_ERROR_INVALID_VALUE;

	if (io_idx >= thread->q_depth)
		return DOCA_ERROR_INVALID_VALUE;

	/* A smaller-than-max block (bsrange) must still leave at least one byte per iovec. */
	if (io_buflen < iov_entry_count)
		return DOCA_ERROR_INVALID_VALUE;

	const uint32_t chunk_size = io_buflen / iov_entry_count;
	const uint32_t remaining_size = io_buflen % iov_entry_count;

	iov_entry = &thread->task_ctx[io_idx].iovec_entry;

	if (iov_entry_count == 1) {
		iov_entry->iov[0].iov_base = io_buf;
		iov_entry->iov[0].iov_len = io_buflen;
	} else {
		for (uint32_t i = 0; i < iov_entry_count; i++) {
			iov_entry->iov[i].iov_base = io_buf + i * chunk_size;
			iov_entry->iov[i].iov_len = chunk_size;
		}

		if (remaining_size > 0) {
			iov_entry->iov[iov_entry_count - 1].iov_len = chunk_size + remaining_size;
		}
	}

	*iov = iov_entry->iov;
	*iovcnt = iov_entry_count;
	return DOCA_SUCCESS;
}

doca_error_t memos_fio_plugin_thread_start_retrieve_op(struct memos_fio_plugin_thread *thread,
						       const struct memos_fio_plugin_ns *fio_ns,
						       union doca_data user_data,
						       uint64_t io_offset,
						       uint8_t *io_buf,
						       uint64_t io_buflen,
						       uint32_t io_idx)
{
	struct doca_kv_io_task_retrieve *retrieve_task;
	const struct iovec *iov;
	uint32_t iovcnt;
	doca_error_t result;
	uint8_t key[MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE];
	struct doca_kv_io *kv_io;
	union doca_data task_udata;

	memos_fio_utils_offset_to_key(thread->hash_type, io_offset, key);

	result = memos_fio_plugin_thread_populate_io_iovec(thread, io_idx, io_buf, io_buflen, &iov, &iovcnt);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to populate I/O iovec for thread %u: [io_idx=%u io_buf=%p io_buflen=%lu] %s",
			     thread->tidx,
			     io_idx,
			     io_buf,
			     io_buflen,
			     doca_error_get_descr(result));
		return result;
	}

	result = memos_fio_plugin_thread_prepare_task_ctx(thread, io_idx, user_data, io_buflen, &task_udata);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Invalid io_idx=%u for retrieve on thread %u (q_depth=%u)",
			     io_idx,
			     thread->tidx,
			     thread->q_depth);
		return result;
	}

	kv_io = doca_kv_nvme_kernel_io_as_kv_io((struct doca_kv_nvme_kernel_io *)fio_ns->kvdev_io);
	result = doca_kv_io_task_retrieve_alloc_init(kv_io,
						     doca_kv_nvme_kernel_dev_as_kv_dev(fio_ns->kvdev.kvdev),
						     task_udata,
						     &retrieve_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate retrieve task for thread %u: %s",
			     thread->tidx,
			     doca_error_get_descr(result));
		return result;
	}

	doca_kv_io_task_retrieve_set_key_value_conf(retrieve_task, key, sizeof(key), iov, iovcnt, io_buflen);

	result = doca_task_submit(doca_kv_io_task_retrieve_as_task(retrieve_task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit retrieve task for thread %u: %s",
			     thread->tidx,
			     doca_error_get_descr(result));
		doca_task_free(doca_kv_io_task_retrieve_as_task(retrieve_task));
		return result;
	}

	++thread->inflights;

	DISABLE_DOCA_LOG_DBG("Thread #%u: RETRIEVE [io_idx=%u task=%p udata=%p offset=%" PRIu64 " len=%" PRIu64
			     "] submitted",
			     thread->tidx,
			     io_idx,
			     retrieve_task,
			     user_data.ptr,
			     io_offset,
			     io_buflen);
	return DOCA_SUCCESS;
}

doca_error_t memos_fio_plugin_thread_start_store_op(struct memos_fio_plugin_thread *thread,
						    const struct memos_fio_plugin_ns *fio_ns,
						    union doca_data user_data,
						    uint64_t io_offset,
						    uint8_t *io_buf,
						    uint64_t io_buflen,
						    uint32_t io_idx)
{
	struct doca_kv_io_task_store *store_task;
	const struct iovec *iov;
	uint32_t iovcnt;
	doca_error_t result;
	uint8_t key[MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE];
	struct doca_kv_io *kv_io;
	union doca_data task_udata;

	memos_fio_utils_offset_to_key(thread->hash_type, io_offset, key);

	result = memos_fio_plugin_thread_populate_io_iovec(thread, io_idx, io_buf, io_buflen, &iov, &iovcnt);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to populate I/O iovec for thread %u: [io_idx=%u io_buf=%p io_buflen=%lu] %s",
			     thread->tidx,
			     io_idx,
			     io_buf,
			     io_buflen,
			     doca_error_get_descr(result));
		return result;
	}

	result = memos_fio_plugin_thread_prepare_task_ctx(thread, io_idx, user_data, io_buflen, &task_udata);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Invalid io_idx=%u for store on thread %u (q_depth=%u)",
			     io_idx,
			     thread->tidx,
			     thread->q_depth);
		return result;
	}

	kv_io = doca_kv_nvme_kernel_io_as_kv_io((struct doca_kv_nvme_kernel_io *)fio_ns->kvdev_io);
	result = doca_kv_io_task_store_alloc_init(kv_io,
						  doca_kv_nvme_kernel_dev_as_kv_dev(fio_ns->kvdev.kvdev),
						  task_udata,
						  &store_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate store task for thread %u: %s",
			     thread->tidx,
			     doca_error_get_descr(result));
		return result;
	}

	doca_kv_io_task_store_set_key_value_conf(store_task, key, sizeof(key), iov, iovcnt, io_buflen);

	result = doca_task_submit(doca_kv_io_task_store_as_task(store_task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit store task for thread %u: %s",
			     thread->tidx,
			     doca_error_get_descr(result));
		doca_task_free(doca_kv_io_task_store_as_task(store_task));
		return result;
	}

	++thread->inflights;

	DISABLE_DOCA_LOG_DBG("Thread #%u: STORE [io_idx=%u task=%p udata=%p offset=%" PRIu64 " len=%" PRIu64
			     "] submitted (inflights=%u)",
			     thread->tidx,
			     io_idx,
			     store_task,
			     user_data.ptr,
			     io_offset,
			     io_buflen,
			     thread->inflights);
	return DOCA_SUCCESS;
}

const struct memos_fio_plugin_thread_cpl_data *memos_fio_plugin_thread_get_cpl_data(
	const struct memos_fio_plugin_thread *thread,
	uint32_t index)
{
	if (thread == NULL || index >= thread->cpls)
		return NULL;

	return &thread->cpl_data[index];
}

void memos_fio_plugin_thread_reset_cpl_data(struct memos_fio_plugin_thread *thread)
{
	if (thread != NULL)
		thread->cpls = 0;
}

uint32_t memos_fio_plugin_thread_get_cpls(const struct memos_fio_plugin_thread *thread)
{
	return thread ? thread->cpls : 0;
}

const struct memos_fio_plugin_ns *memos_fio_plugin_thread_get_ns(const struct memos_fio_plugin_thread *thread,
								 const char *nsid_file_name)
{
	if (thread == NULL || nsid_file_name == NULL)
		return NULL;

	for (uint32_t i = 0; i < thread->ns_arr_count; i++) {
		if (thread->ns_arr[i].kvdev.ns_file_name != NULL &&
		    strcmp(thread->ns_arr[i].kvdev.ns_file_name, nsid_file_name) == 0)
			return &thread->ns_arr[i];
	}

	return NULL;
}
