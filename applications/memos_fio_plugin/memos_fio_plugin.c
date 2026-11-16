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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* fio's os-linux.h: SCHED_IDLE, gettid, CPU_* */
#endif

#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config-host.h"
#include "io_ddir.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include "fio.h"
#include "optgroup.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <doca_error.h>
#include <doca_kv_dev.h>
#include <doca_kv_io.h>
#include <doca_log.h>
#include <doca_kv_nvme_kernel_dev.h>
#include <doca_pe.h>

#include "memos_fio_plugin.h"
#include "memos_fio_plugin_defs.h"
#include "memos_fio_plugin_dev.h"
#include "memos_fio_plugin_thread.h"
#include "memos_fio_utils.h"

DOCA_LOG_REGISTER(MEMOS_FIO_PLUGIN);

/* fio options */
static struct fio_option options[] = {

	{
		.name = "kv_iovcnt",
		.lname = "KV IOV Count",
		.type = FIO_OPT_INT,
		.off1 = offsetof(struct memos_fio_plugin_options, kv_iovcnt),
		.def = MEMOS_FIO_PLUGIN_STRINGIFY(MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_DEFAULT),
		.minval = 1,
		.maxval = MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX,
		.help = "Number of iovecs the io_u buffer is split across per KV operation (1-" MEMOS_FIO_PLUGIN_STRINGIFY(
			MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX) ", default " MEMOS_FIO_PLUGIN_STRINGIFY(MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_DEFAULT) ")",
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_INVALID,
	},
	{
		.name = "fio-log-level",
		.lname = "fio plugin log level",
		.type = FIO_OPT_INT,
		.off1 = offsetof(struct memos_fio_plugin_options, fio_log_level),
		.help = "FIO plugin log level",
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_INVALID,
	},
	{
		.name = "doca-log-level",
		.lname = "doca library log level",
		.type = FIO_OPT_INT,
		.off1 = offsetof(struct memos_fio_plugin_options, doca_log_level),
		.help = "DOCA library log level",
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_INVALID,
	},
	{
		.name = "hash_type",
		.lname = "hash_type",
		.type = FIO_OPT_INT,
		.off1 = offsetof(struct memos_fio_plugin_options, hash_type),
		.help = "fio offset to KV key hash: 1=xxh64, 2=splitmix64 (default), 3=murmur3_64",
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_INVALID,
	},
	{
		.name = NULL,
	},
};

struct memos_fio_plugin_ctx {
	struct memos_fio_plugin_options *options;
	pthread_mutex_t lock;
	uint32_t total_threads;
	uint32_t started_threads;
	uint32_t max_io_size;

	struct doca_log_backend *sdk_log;
	bool init_done;
	bool init_failed;

	struct doca_pe *admin_pe;
	struct memos_fio_plugin_dev fio_devs[MEMOS_FIO_PLUGIN_NSID_COUNT_MAX];
	uint32_t kvdev_count;
	struct memos_fio_plugin_thread *kvio_threads[MEMOS_FIO_PLUGIN_IO_COUNT_MAX];
	char nsids_str[MEMOS_FIO_PLUGIN_NSID_COUNT_MAX * MEMOS_FIO_PLUGIN_NSID_PATH_LEN_MAX];
	const char *nsids[MEMOS_FIO_PLUGIN_NSID_COUNT_MAX];
};

static const struct memos_fio_plugin_dev *memos_fio_plugin_find_kvdev_by_nsid_file_name(
	const struct memos_fio_plugin_ctx *mctx,
	const char *nsid_file_name)
{
	for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
		DOCA_LOG_DBG(">>> ns_file_name[%u]='%s' nsid_file_name='%s'",
			     i,
			     mctx->fio_devs[i].ns_file_name,
			     nsid_file_name);

		if (strcmp(mctx->fio_devs[i].ns_file_name, nsid_file_name) == 0)
			return &mctx->fio_devs[i];
	}

	return NULL;
}

static doca_error_t memos_fio_plugin_get_kvdev_size(const struct memos_fio_plugin_ctx *mctx,
						    const char *nsid_file_name,
						    uint64_t *size)
{
	const struct memos_fio_plugin_dev *fio_dev =
		memos_fio_plugin_find_kvdev_by_nsid_file_name(mctx, nsid_file_name);

	if (fio_dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	return memos_fio_plugin_dev_get_kvdev_size(fio_dev->kvdev, size);
}

struct memos_fio_plugin_ctx_init_attr {
	const char *nsids;
	uint32_t num_io_threads;
	uint32_t io_thread_q_depth;
	uint32_t max_io_size;
	uint32_t kv_iovcnt;
};

static struct memos_fio_plugin_ctx memos_fio_plugin_ctx_gl = {
	.options = NULL,
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.total_threads = 0,
	.started_threads = 0,
	.max_io_size = 0,
	.sdk_log = NULL,
	.init_done = false,
	.init_failed = false,
	.admin_pe = NULL,
	.fio_devs = {0},
	.kvdev_count = 0,
	.kvio_threads = {0},
};

static doca_error_t memos_fio_plugin_global_ctx_stop(struct memos_fio_plugin_ctx *mctx);

static inline uint32_t memos_fio_plugin_td_thread_num_to_idx(struct thread_data *td)
{
	return td->thread_number - 1;
}

static doca_error_t memos_fio_plugin_validate_configuration(struct memos_fio_plugin_options *opts,
							    const struct memos_fio_plugin_ctx_init_attr *attr)
{
	uint32_t lib_max_tasks = 0;
	uint32_t max_q_depth;

	if (opts == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	if (attr == NULL) {
		fprintf(stderr, "BAD configuration: missing internal init parameters\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* fio numjobs must fit both the online logical CPU count and MEMOS_FIO_PLUGIN_IO_COUNT_MAX;
	 * the helper reports which bound was exceeded.
	 */
	if (!memos_fio_utils_validate_num_io_threads(attr->num_io_threads)) {
		DOCA_LOG_ERR("Invalid num_io_threads=%" PRIu32, attr->num_io_threads);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (attr->kv_iovcnt == 0 || attr->kv_iovcnt > MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX) {
		DOCA_LOG_ERR("kv_iovcnt=%u out of range (min=1 max=%u)",
			     attr->kv_iovcnt,
			     MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The io_u buffer is split kv_iovcnt ways, so every iovec needs at least one byte. */
	if (attr->kv_iovcnt > attr->max_io_size) {
		DOCA_LOG_ERR("kv_iovcnt=%u exceeds block size %u; each iovec must cover at least one byte",
			     attr->kv_iovcnt,
			     attr->max_io_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * iodepth becomes the I/O context's task count, so the library bounds it too.
	 * Take the stricter of the two: this plugin's own limit is the tighter one
	 * today, but reading the cap keeps the two from silently diverging, and a
	 * value rejected here would otherwise fail much later, at doca_ctx_start().
	 */
	max_q_depth = MEMOS_FIO_PLUGIN_DEFAULT_IO_Q_DEPTH_MAX;
	if (doca_kv_io_cap_get_max_tasks(&lib_max_tasks) == DOCA_SUCCESS && lib_max_tasks < max_q_depth)
		max_q_depth = lib_max_tasks;

	if (attr->io_thread_q_depth > max_q_depth) {
		DOCA_LOG_ERR("iodepth=%u out of range (max=%u)", attr->io_thread_q_depth, max_q_depth);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (opts->hash_type == 0)
		opts->hash_type = MEMOS_FIO_PLUGIN_HASH_TYPE_SPLITMIX64;

	if (opts->hash_type != MEMOS_FIO_PLUGIN_HASH_TYPE_XXH64 &&
	    opts->hash_type != MEMOS_FIO_PLUGIN_HASH_TYPE_SPLITMIX64 &&
	    opts->hash_type != MEMOS_FIO_PLUGIN_HASH_TYPE_MURMUR3_64) {
		DOCA_LOG_ERR("invalid hash_type=%" PRIu32 " (use 1=xxh64, 2=splitmix64, 3=murmur3_64)",
			     opts->hash_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/**
 * Validate one started namespace against job parameters (mirrors fio_doca_kv_validate_caps).
 */
static doca_error_t memos_fio_plugin_validate_device_caps(struct doca_kv_dev *kvdev,
							  const char *ns_path,
							  const struct memos_fio_plugin_ctx_init_attr *attr,
							  uint64_t job_bs_read,
							  uint64_t job_bs_write,
							  uint64_t job_size)
{
	(void)attr;
	uint16_t max_key_len = 0;
	uint32_t max_value_len = 0;
	uint64_t dev_size = 0;
	doca_error_t result;

	result = doca_kv_dev_get_max_key_len(kvdev, &max_key_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max_key_len for '%s': %s", ns_path, doca_error_get_descr(result));
		return result;
	}

	result = doca_kv_dev_get_max_value_len(kvdev, &max_value_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max_value_len for '%s': %s", ns_path, doca_error_get_descr(result));
		return result;
	}

	result = doca_kv_dev_get_size(kvdev, &dev_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get namespace size for '%s': %s", ns_path, doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("namespace '%s': size=%" PRIu64 " max_key=%u max_value=%u",
		      ns_path,
		      dev_size,
		      max_key_len,
		      max_value_len);

	if (max_key_len > 0 && MEMOS_FIO_PLUGIN_KEY_SIZE > max_key_len) {
		DOCA_LOG_ERR("key_size=%u exceeds device max_key_len=%u for '%s'",
			     MEMOS_FIO_PLUGIN_KEY_SIZE,
			     max_key_len,
			     ns_path);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (max_value_len > 0 && job_bs_write > max_value_len) {
		DOCA_LOG_ERR("write bs=%" PRIu64 " exceeds device max_value_len=%u for '%s'",
			     job_bs_write,
			     max_value_len,
			     ns_path);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (max_value_len > 0 && job_bs_read > max_value_len) {
		DOCA_LOG_ERR("read bs=%" PRIu64 " exceeds device max_value_len=%u for '%s'",
			     job_bs_read,
			     max_value_len,
			     ns_path);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (job_size > 0 && dev_size > 0 && job_size > dev_size) {
		DOCA_LOG_ERR("size=%" PRIu64 " exceeds device capacity=%" PRIu64 " for '%s'",
			     job_size,
			     dev_size,
			     ns_path);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/** Validate all namespaces against the created devices. */
static doca_error_t memos_fio_plugin_validate_caps(const struct memos_fio_plugin_ctx *mctx,
						   const struct memos_fio_plugin_ctx_init_attr *attr)
{
	uint64_t job_bs_read = attr->max_io_size;
	uint64_t job_bs_write = attr->max_io_size;

	for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
		struct doca_kv_dev *kvdev = doca_kv_nvme_kernel_dev_as_kv_dev(mctx->fio_devs[i].kvdev);
		doca_error_t result;

		if (kvdev == NULL) {
			DOCA_LOG_ERR("Failed to get DOCA KV device interface for '%s'", mctx->fio_devs[i].ns_file_name);
			return DOCA_ERROR_DRIVER;
		}

		result = memos_fio_plugin_validate_device_caps(kvdev,
							       mctx->fio_devs[i].ns_file_name,
							       attr,
							       job_bs_read,
							       job_bs_write,
							       attr->max_io_size);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t memos_fio_plugin_init_configuration(struct memos_fio_plugin_ctx *mctx,
							struct memos_fio_plugin_options *opts,
							const struct memos_fio_plugin_ctx_init_attr *attr)
{
	doca_error_t result = DOCA_SUCCESS;
	uint32_t nsids_count = 0;

	if (opts->fio_log_level == 0)
		opts->fio_log_level = MEMOS_FIO_PLUGIN_DEFAULT_FIO_LOG_LEVEL;

	if (opts->doca_log_level == 0)
		opts->doca_log_level = MEMOS_FIO_PLUGIN_DEFAULT_DOCA_LOG_LEVEL;

	if (!memos_fio_utils_init_log(opts->fio_log_level, opts->doca_log_level, &mctx->sdk_log)) {
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	result = memos_fio_plugin_validate_configuration(opts, attr);
	if (result != DOCA_SUCCESS)
		goto fail;

	if (attr->nsids == NULL || attr->nsids[0] == '\0') {
		DOCA_LOG_ERR("fio filename is required (colon-separated device paths, e.g. /dev/ng0n1:/dev/ng0n0)");
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	strncpy(mctx->nsids_str, attr->nsids, sizeof(mctx->nsids_str) - 1);
	mctx->nsids_str[sizeof(mctx->nsids_str) - 1] = '\0';

	if (!memos_fio_utils_parse_nsid_file_name(mctx->nsids_str,
						  mctx->nsids,
						  MEMOS_FIO_PLUGIN_NSID_COUNT_MAX,
						  &nsids_count)) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Failed to parse device paths from filename");
		goto fail;
	}

	if (nsids_count == 0) {
		DOCA_LOG_ERR("No device paths found in filename: '%s'", attr->nsids);
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	mctx->kvdev_count = nsids_count;

	return DOCA_SUCCESS;

fail:
	return result;
}

static void memos_fio_plugin_dump_configuration(const struct memos_fio_plugin_ctx *mctx,
						const struct memos_fio_plugin_ctx_init_attr *attr)
{
	/* Dump configuration options*/

	DOCA_LOG_INFO("nsids_file_name=%s io_threads=%u io_q_depth=%u io_size=%u kv_iovcnt=%u hash_type=%" PRIu32
		      " lib-lvl=%" PRIu32 " plugin-lvl=%" PRIu32,

		      mctx->nsids_str,

		      attr->num_io_threads,
		      attr->io_thread_q_depth,
		      attr->max_io_size,
		      attr->kv_iovcnt,

		      mctx->options->hash_type,
		      mctx->options->doca_log_level,
		      mctx->options->fio_log_level);
}

static doca_error_t memos_fio_plugin_global_ctx_init(struct memos_fio_plugin_ctx *mctx,
						     const struct memos_fio_plugin_ctx_init_attr *attr)
{
	struct memos_fio_plugin_options *opts = mctx->options;
	doca_error_t result = DOCA_SUCCESS;

	if (mctx->init_done)
		return DOCA_SUCCESS;

	if (opts == NULL) {
		DOCA_LOG_ERR("memos_fio_plugin: engine options is missing");
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	/* Initialize initiator configuration */

	result = memos_fio_plugin_init_configuration(mctx, opts, attr);
	if (result != DOCA_SUCCESS)
		goto fail;

	mctx->total_threads = attr->num_io_threads;
	mctx->max_io_size = attr->max_io_size;

	for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
		const struct memos_fio_plugin_dev_attr dev_attr = {
			.path = mctx->nsids[i],
		};

		result = memos_fio_plugin_dev_create(&dev_attr, &mctx->fio_devs[i].kvdev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create NVMe kernel KV device: %s", doca_error_get_descr(result));
			goto fail;
		}

		mctx->fio_devs[i].ns_file_name = mctx->nsids[i];
	}

	result = doca_pe_create(&mctx->admin_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create admin progress engine: %s", doca_error_get_descr(result));
		goto fail;
	}

	DOCA_LOG_DBG("ADMIN PE - CREATED [PE=%p]", mctx->admin_pe);

	for (uint32_t i = 0; i < attr->num_io_threads; i++) {
		const struct memos_fio_plugin_create_thread_attr thr_attr = {
			.kvdevs = &mctx->fio_devs[0],
			.kvdevs_count = mctx->kvdev_count,
			/* Each fio thread needs its own PE: sharing admin_pe races doca_pe_progress()
			 * and can double-invoke completion callbacks / doca_task_free().
			 */
			.pe = NULL,
			.tidx = i,
			.q_depth = attr->io_thread_q_depth,
			.iov_entry_count = mctx->options->kv_iovcnt,
			.hash_type = mctx->options->hash_type,
			/* create_kv_io is false because we will create kv_io for each thread in
			 * memos_fio_plugin_global_ctx_start function (bad design, but it's a workaround)
			 */
			.create_kv_io = false,
		};

		result = memos_fio_plugin_thread_create(&thr_attr, &mctx->kvio_threads[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create KV I/O thread: %s", doca_error_get_descr(result));
			goto fail;
		}
	}

	memos_fio_plugin_dump_configuration(&memos_fio_plugin_ctx_gl, attr);
	return DOCA_SUCCESS;

fail:
	mctx->init_done = true;
	mctx->init_failed = true;
	return result;
}

static doca_error_t memos_fio_plugin_global_ctx_start(struct memos_fio_plugin_ctx *mctx)
{
	doca_error_t result;

	/* Start all kv_dev devices */
	for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
		result = memos_fio_plugin_dev_start(mctx->fio_devs[i].kvdev, mctx->admin_pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start NVMe kernel KV device: %s", doca_error_get_descr(result));
			goto fail;
		}

		result = memos_fio_plugin_dev_wait_running(mctx->fio_devs[i].kvdev, mctx->admin_pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("NVMe kernel KV device did not reach running state: %s",
				     doca_error_get_descr(result));
			goto fail;
		}
	}

	/* Start kv_io for all threads */
	for (uint32_t i = 0; i < mctx->total_threads; i++) {
		result = memos_fio_plugin_thread_start(mctx->kvio_threads[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start KV I/O thread: %s", doca_error_get_descr(result));
			goto fail;
		}
	}

	return DOCA_SUCCESS;

fail:
	mctx->init_done = true;
	mctx->init_failed = true;
	(void)memos_fio_plugin_global_ctx_stop(mctx);
	return result;
}

static doca_error_t memos_fio_plugin_global_ctx_stop(struct memos_fio_plugin_ctx *mctx)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	/* Stop kv_io for all threads */
	for (uint32_t i = 0; i < mctx->total_threads; i++) {
		doca_err = memos_fio_plugin_thread_stop(mctx->kvio_threads[i]);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop KV I/O thread: %s", doca_error_get_descr(doca_err));
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
		}
	}

	return ret_err;
}

static doca_error_t memos_fio_plugin_global_ctx_cleanup(struct memos_fio_plugin_ctx *mctx)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	if (!mctx->init_done)
		return 0;

	if (MEMOS_FIO_PLUGIN_SYNC_CLEANUP_ON_THREAD_EXIT) {
		/* At this step all kv_io threads should be stopped & destroyed */

		doca_err = memos_fio_plugin_global_ctx_stop(mctx);
		if (doca_err != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);

		/* Destroy kv_io for all threads */
		for (uint32_t i = 0; i < mctx->total_threads; i++) {
			if (mctx->kvio_threads[i]) {
				doca_err = memos_fio_plugin_thread_destroy(mctx->kvio_threads[i]);
				if (doca_err != DOCA_SUCCESS)
					DOCA_ERROR_PROPAGATE(ret_err, doca_err);
			}
			mctx->kvio_threads[i] = NULL;
		}

		/* Stop kv_dev devices */

		for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
			doca_err = memos_fio_plugin_dev_stop(mctx->fio_devs[i].kvdev, mctx->admin_pe);
			if (doca_err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop NVMe kernel KV device: %s",
					     doca_error_get_descr(doca_err));
				DOCA_ERROR_PROPAGATE(ret_err, doca_err);
			}
		}
	}

	/* Wait for all kv_dev devices to stop */
	for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
		doca_err = memos_fio_plugin_dev_wait_idle(mctx->fio_devs[i].kvdev, mctx->admin_pe);
		if (doca_err != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	/* Destroy all kv_dev devices */
	for (uint32_t i = 0; i < mctx->kvdev_count; i++) {
		doca_err = memos_fio_plugin_dev_destroy(mctx->fio_devs[i].kvdev);
		if (doca_err != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
		mctx->fio_devs[i].kvdev = NULL;
	}

	doca_err = doca_pe_destroy(mctx->admin_pe);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy admin progress engine: %s", doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	mctx->admin_pe = NULL;
	mctx->kvdev_count = 0;
	mctx->init_done = false;
	return DOCA_SUCCESS;
}

static inline void memos_fio_plugin_file_set_open(struct thread_data *td,
						  struct fio_file *f,
						  uint64_t ns_capacity_bytes)
{
	f->real_file_size = ns_capacity_bytes;
	f->io_size = ns_capacity_bytes;

	/* If you want fio to use whole namespace when size is not set in fio config file */
	f->io_size = ns_capacity_bytes;

	/* Setting FIO_FILE_size_known via fio_file_set_size_known(f) does two things:
	 * - Tells fio not to call generic_get_file_size() for this file, since you already provided the size.
	 * - Lets fio proceed to compute f->io_size directly from real_file_size and the job’s size / filesize options.
	 */
	fio_file_set_size_known(f);

	DOCA_LOG_DBG("THREAD=%d (%p) - size was not configured in fio config file, using kvdev size (%lu bytes)",
		     td->thread_number,
		     td,
		     ns_capacity_bytes);
}

static bool memos_fio_plugin_setup_file_system(struct thread_data *td, struct memos_fio_plugin_ctx *mctx)
{
	struct fio_file *f;
	uint64_t ns_capacity_bytes = 0;
	uint32_t i;
	doca_error_t result;

	if (td->o.size)
		return true;

	for_each_file(td, f, i)
	{
		/* If size was not set in fio config file use the kvdev size */

		result = memos_fio_plugin_get_kvdev_size(mctx, f->file_name, &ns_capacity_bytes);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get kvdev size for nsid_file_name: '%s': %s",
				     f->file_name,
				     doca_error_get_descr(result));
			return false;
		}

		memos_fio_plugin_file_set_open(td, f, ns_capacity_bytes);
	}

	return true;
}

static int memos_fio_plugin_setup(struct thread_data *td)
{
	struct memos_fio_plugin_thread *mtd;
	struct memos_fio_plugin_ctx *mctx = &memos_fio_plugin_ctx_gl;
	doca_error_t result;
	uint32_t tidx;
	bool counted = false;

	pthread_mutex_lock(&mctx->lock);

	if (mctx->init_failed)
		goto ctx_init_failed;

	counted = true;
	if (mctx->started_threads++ == 0) {
		uint32_t max_r_size = (uint32_t)td->o.max_bs[DDIR_READ];
		uint32_t max_w_size = (uint32_t)td->o.max_bs[DDIR_WRITE];

		mctx->options = td->eo;

		const struct memos_fio_plugin_ctx_init_attr ctx_attr = {
			.nsids = td->o.filename,
			.num_io_threads = td->o.numjobs,
			.io_thread_q_depth = td->o.iodepth ? td->o.iodepth : MEMOS_FIO_PLUGIN_DEFAULT_IO_Q_DEPTH,
			.max_io_size = max_w_size > max_r_size ? max_w_size : max_r_size,
			.kv_iovcnt = mctx->options->kv_iovcnt ? mctx->options->kv_iovcnt :
								MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_DEFAULT,
		};

		/* Initialize global context:
		 * - validate configuration
		 * - initialize log
		 * - initialize device
		 * - initialize admin progress engine
		 * - initialize kv_dev(s)
		 */
		result = memos_fio_plugin_global_ctx_init(mctx, &ctx_attr);
		if (result != DOCA_SUCCESS)
			goto ctx_init_failed;

		/* Start global context:
		 * - start all kv_dev devices
		 * - wait for all kv_dev devices to reach running state
		 * - start all kv_io for all threads
		 * - wait for all kv_io_dev devices to reach running state
		 */
		result = memos_fio_plugin_global_ctx_start(mctx);
		if (result != DOCA_SUCCESS)
			goto ctx_init_failed;

		result = memos_fio_plugin_validate_caps(mctx, &ctx_attr);

		if (result != DOCA_SUCCESS) {
			mctx->init_done = true;
			mctx->init_failed = true;
			goto ctx_init_failed;
		} else {
			mctx->init_done = true;
		}
	}

	/* Setup 'file system' (namespaces) for thread .
	 * If the size was not set in the fio config file,
	 * use the size of the namespace:
	 * - Go through all files of the thread 'td' and set the size of the file
	 *   as the size of the namespace.
	 */
	if (!memos_fio_plugin_setup_file_system(td, mctx)) {
		DOCA_LOG_ERR("Failed to setup file system for thread %d", td->thread_number);
		goto ctx_init_failed;
	}

	tidx = memos_fio_plugin_td_thread_num_to_idx(td);
	mtd = mctx->kvio_threads[tidx];
	DOCA_LOG_DBG("SETUP td #%u td=%p mtd=%p", tidx, td, mtd);
	td->io_ops_data = mtd;
	pthread_mutex_unlock(&mctx->lock);
	return 0;

ctx_init_failed:
	/* fio never calls the cleanup callback for a thread whose setup failed, because
	 * td->io_ops_data was left unset. Undo the optimistic accounting here and release the
	 * global context directly once this thread is its last user, otherwise the devices,
	 * admin progress engine and thread contexts built so far are never destroyed.
	 */
	if (counted) {
		--mctx->started_threads;
		if (mctx->started_threads == 0)
			(void)memos_fio_plugin_global_ctx_cleanup(mctx);
	}

	pthread_mutex_unlock(&mctx->lock);
	return 1;
}

static void memos_fio_plugin_cleanup(struct thread_data *td)
{
	struct memos_fio_plugin_thread *mtd = (struct memos_fio_plugin_thread *)td->io_ops_data;
	struct memos_fio_plugin_ctx *mctx = &memos_fio_plugin_ctx_gl;
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	if (mtd == NULL)
		return;

	DOCA_LOG_DBG("CLEAN td #%d (%p)", td->thread_number, mtd);

	pthread_mutex_lock(&memos_fio_plugin_ctx_gl.lock);

	--mctx->started_threads;

	/*
	 * These two conditions coincide when total_threads is 1, so they must not be chained:
	 * the single thread has to both release the options alias and tear down the global context.
	 */
	if (mctx->started_threads == mctx->total_threads - 1) {
		/* First thread to exit drops 'its' options, as mctx->options aliases the first thread's td->eo. */
		mctx->options = NULL;
	}

	if (mctx->started_threads == 0) {
		doca_err = memos_fio_plugin_global_ctx_cleanup(mctx);
		if (doca_err != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	if (ret_err != DOCA_SUCCESS)
		DOCA_LOG_DBG("CLEAN FAILED for td #%d (%p): %s", td->thread_number, mtd, doca_error_get_descr(ret_err));

	pthread_mutex_unlock(&mctx->lock);
}

static int memos_fio_plugin_open(struct thread_data *td, struct fio_file *f)
{
	struct memos_fio_plugin_thread *mtd = (struct memos_fio_plugin_thread *)td->io_ops_data;
	const struct memos_fio_plugin_ns *fio_ns;

	fio_ns = memos_fio_plugin_thread_get_ns(mtd, f->file_name);
	if (fio_ns == NULL) {
		DOCA_LOG_ERR("THREAD=%d (%p) - fio_ns not found for nsid: '%s'", td->thread_number, td, f->file_name);
		return 1;
	}

	/* Set the volume as the engine data */
	DOCA_LOG_DBG("THREAD=%d (%p) --> fio_ns [kvdev=%p kvdev_io=%p ns_name='%s']",
		     td->thread_number,
		     td,

		     fio_ns->kvdev.kvdev,
		     fio_ns->kvdev_io,
		     fio_ns->kvdev.ns_file_name);

	f->filetype = FIO_TYPE_BLOCK;
	f->flags |= FIO_FILE_open;

	FILE_SET_ENG_DATA(f, (void *)fio_ns);
	return 0;
}

static enum fio_q_status memos_fio_plugin_queue(struct thread_data *td, struct io_u *io_u)
{
	struct memos_fio_plugin_thread *mtd = (struct memos_fio_plugin_thread *)td->io_ops_data;
	struct fio_file *f;
	const struct memos_fio_plugin_ns *fio_ns;
	doca_error_t result;

	f = io_u->file;
	fio_ns = FILE_ENG_DATA(f);

	switch (io_u->ddir) {
	case DDIR_READ:
		/* Map fio READ -> memos RETRIEVE */
		result = memos_fio_plugin_thread_start_retrieve_op(mtd,
								   fio_ns,
								   (union doca_data){.ptr = io_u},
								   io_u->offset,
								   io_u->buf,
								   io_u->buflen,
								   io_u->index);
		break;

	case DDIR_WRITE:
		/* Map fio WRITE -> memos STORE */
		result = memos_fio_plugin_thread_start_store_op(mtd,
								fio_ns,
								(union doca_data){.ptr = io_u},
								io_u->offset,
								io_u->buf,
								io_u->buflen,
								io_u->index);
		break;

	default:
		/* For now, reject other types like TRIM/VERIFY */
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("THREAD=%d (%p) - Invalid I/O direction: %d", td->thread_number, td, io_u->ddir);
		break;
	}

	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_INVALID_VALUE)
			io_u->error = EINVAL;
		else
			io_u->error = EIO;
		return FIO_Q_COMPLETED;
	}

	return FIO_Q_QUEUED; /* if async; or FIO_Q_COMPLETED if you do sync */
}

static int memos_fio_plugin_getevents(struct thread_data *td,
				      unsigned int min,
				      unsigned int max,
				      const struct timespec *t)
{
	(void)max;
	uint64_t timeout = 0;
	struct timespec t0, t1;
	struct memos_fio_plugin_thread *mtd;
	uint32_t cpls;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		(void)clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	mtd = (struct memos_fio_plugin_thread *)td->io_ops_data;

	memos_fio_plugin_thread_reset_cpl_data(mtd);

	for (;;) {
		memos_fio_plugin_thread_progress(mtd);

		cpls = memos_fio_plugin_thread_get_cpls(mtd);
		if (cpls >= min)
			break;

		if (t) {
			uint64_t elapse;

			(void)clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L) + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout)
				break;
		}
	}

	return memos_fio_plugin_thread_get_cpls(mtd);
}

static int memos_fio_plugin_doca_error_to_errno(doca_error_t status)
{
	switch (status) {
	case DOCA_SUCCESS:
		return 0;
	case DOCA_ERROR_NOT_FOUND:
		return ENOENT;
	/*
	 * Retrieve whose device result length != submitted fio bs. Ignore via:
	 *   --continue_on_error=read --ignore_error=ENOENT:ENODATA
	 */
	case DOCA_ERROR_EMPTY:
		return ENODATA;
	case DOCA_ERROR_ALREADY_EXIST:
		return EEXIST;
	case DOCA_ERROR_INVALID_VALUE:
	case DOCA_ERROR_BAD_CONFIG:
		return EINVAL;
	case DOCA_ERROR_NO_MEMORY:
		return ENOMEM;
	case DOCA_ERROR_FULL:
		return ENOSPC;
	case DOCA_ERROR_NOT_PERMITTED:
	case DOCA_ERROR_AUTHENTICATION:
		return EACCES;
	case DOCA_ERROR_IN_USE:
		return EBUSY;
	case DOCA_ERROR_AGAIN:
		return EAGAIN;
	case DOCA_ERROR_TIME_OUT:
		return ETIMEDOUT;
	case DOCA_ERROR_NOT_SUPPORTED:
		return EOPNOTSUPP;
	case DOCA_ERROR_TOO_BIG:
		return EOVERFLOW;
	case DOCA_ERROR_DRIVER:
	case DOCA_ERROR_IO_FAILED:
	case DOCA_ERROR_OPERATING_SYSTEM:
	case DOCA_ERROR_UNEXPECTED:
	default:
		return EIO;
	}
}

static struct io_u *memos_fio_plugin_event(struct thread_data *td, int event)
{
	struct memos_fio_plugin_thread *mtd;
	struct io_u *io_u;
	const struct memos_fio_plugin_thread_cpl_data *cpl_data;
	uint32_t cpls;

	mtd = (struct memos_fio_plugin_thread *)td->io_ops_data;

	cpls = memos_fio_plugin_thread_get_cpls(mtd);
	if (!(event >= 0 && ((uint32_t)event < cpls))) {
		DOCA_LOG_ERR("Thread #%d (%p): INVALID event index: %d (cpls=%d)",
			     memos_fio_plugin_thread_get_tidx(mtd),
			     mtd,
			     event,
			     cpls);
		return NULL;
	}

	cpl_data = (struct memos_fio_plugin_thread_cpl_data *)memos_fio_plugin_thread_get_cpl_data(mtd, event);
	if (cpl_data == NULL || cpl_data->udata.ptr == NULL) {
		DOCA_LOG_ERR("Thread #%d (%p): NULL completion udata for event %d",
			     memos_fio_plugin_thread_get_tidx(mtd),
			     mtd,
			     event);
		return NULL;
	}

	io_u = (struct io_u *)cpl_data->udata.ptr;
	io_u->error = memos_fio_plugin_doca_error_to_errno(cpl_data->status);
	return io_u;
}

/* ioengine_ops describes the callbacks and capabilities fio uses. */
__attribute__((visibility("default"))) struct ioengine_ops ioengine = {
	/* Engine name for --ioengine selection. */
	.name = "memos_fio",
	/* Expected fio ioengine ABI version. */
	.version = FIO_IOOPS_VERSION,
	/* Capability flags for fio's core scheduling/IO paths. */
	.flags = FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_DISKLESSIO | FIO_MEMALIGN,
	/* Thread lifecycle hooks. */
	.setup = memos_fio_plugin_setup,
	/* IO submission and completion hooks. */
	.queue = memos_fio_plugin_queue,
	.getevents = memos_fio_plugin_getevents,
	.event = memos_fio_plugin_event,
	/* Cleanup hooks. */
	.cleanup = memos_fio_plugin_cleanup,
	/* File open/close hooks. */
	.open_file = memos_fio_plugin_open,
	.option_struct_size = sizeof(struct memos_fio_plugin_options),
	.options = options,
};

static void fio_init memos_fio_plugin_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit memos_fio_plugin_unregister(void)
{
	unregister_ioengine(&ioengine);
}
