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

/*
 * Minimal standalone sample for the memos_fio_plugin library.
 *
 * It reuses the same modules the fio ioengine is built from:
 *   - memos_fio_plugin_dev    : doca_kv_nvme_kernel_dev lifecycle
 *   - memos_fio_plugin_thread : per-thread progress engine + async store/retrieve
 *   - memos_fio_utils         : logging, buffers, offset -> KV key hashing
 *
 * Flow: create+start a KV device, create+start one I/O thread, STORE a value,
 * RETRIEVE it back into a fresh buffer, and verify the two match.
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_types.h>

#include "memos_fio_plugin_defs.h"
#include "memos_fio_plugin_dev.h"
#include "memos_fio_plugin_thread.h"
#include "memos_fio_utils.h"
#include "memos_fio.h"

DOCA_LOG_REGISTER(MEMOS_FIO_SAMPLE);

/* Spin cap so a stuck operation fails instead of looping forever. */
#define MEMOS_FIO_SAMPLE_MAX_SPINS 100000000ULL
#define MEMOS_FIO_SAMPLE_VALUE_LEN_ALIGNMENT 4096U
#define MEMOS_FIO_SAMPLE_VALUE_LEN_DEFAULT (8U * 1024 * 1024)
#define MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT (2U * 1024 * 1024)

/* Human-readable name for an operation (for logging). */
static const char *memos_fio_op_name(enum memos_fio_operation op)
{
	switch (op) {
	case MEMOS_FIO_OP_STORE:
		return "store";
	case MEMOS_FIO_OP_RETRIEVE:
		return "retrieve";
	case MEMOS_FIO_OP_BOTH:
	default:
		return "both";
	}
}

/**
 * Progress the thread's PE until exactly one completion is recorded, then
 * return that operation's status. Mirrors the fio getevents/event path.
 */
static doca_error_t memos_fio_sample_wait_one(struct memos_fio_plugin_thread *thread)
{
	const struct memos_fio_plugin_thread_cpl_data *cpl;
	uint64_t spins = 0;

	memos_fio_plugin_thread_reset_cpl_data(thread);

	while (memos_fio_plugin_thread_get_cpls(thread) == 0) {
		memos_fio_plugin_thread_progress(thread);
		if (++spins > MEMOS_FIO_SAMPLE_MAX_SPINS) {
			DOCA_LOG_ERR("Timed out waiting for KV operation to complete");
			return DOCA_ERROR_TIME_OUT;
		}
	}

	cpl = memos_fio_plugin_thread_get_cpl_data(thread, 0);
	if (cpl == NULL)
		return DOCA_ERROR_UNEXPECTED;

	return cpl->status;
}

int memos_fio_run(const struct memos_fio_sample_cfg *cfg)
{
	struct doca_log_backend *sdk_log = NULL;
	struct doca_kv_nvme_kernel_dev *nkvdev = NULL;
	struct memos_fio_plugin_thread *thread = NULL;
	const struct memos_fio_plugin_ns *ns;
	uint8_t *wbuf = NULL;
	uint8_t *rbuf = NULL;
	uint8_t key[MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE];
	uint64_t key_u64;
	size_t alloc_len = 0;
	doca_error_t result;
	int ret = EXIT_FAILURE;

	if (cfg == NULL || cfg->dev_path == NULL || cfg->value_len == 0)
		return EXIT_FAILURE;

	/* An out-of-range operation would skip both I/O blocks below and still report success. */
	if (cfg->operation != MEMOS_FIO_OP_STORE && cfg->operation != MEMOS_FIO_OP_RETRIEVE &&
	    cfg->operation != MEMOS_FIO_OP_BOTH)
		return EXIT_FAILURE;

	if (!memos_fio_utils_init_log(DOCA_LOG_LEVEL_INFO, DOCA_LOG_LEVEL_WARNING, &sdk_log)) {
		fprintf(stderr, "Failed to initialize DOCA logging\n");
		return EXIT_FAILURE;
	}

	DOCA_LOG_INFO("KV sample: dev=%s op=%s offset=%" PRIu64 " value_len=%u hash_type=%u",
		      cfg->dev_path,
		      memos_fio_op_name(cfg->operation),
		      cfg->offset,
		      cfg->value_len,
		      cfg->hash_type);

	/* 1. Create the KV device (opens the path, reads NGUID, configures kvdev). */
	const struct memos_fio_plugin_dev_attr dev_attr = {.path = cfg->dev_path};

	result = memos_fio_plugin_dev_create(&dev_attr, &nkvdev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create KV device for '%s': %s", cfg->dev_path, doca_error_get_descr(result));
		goto out;
	}

	/* 2. Start it and confirm it reached the running state. */
	result = memos_fio_plugin_dev_start(nkvdev, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start KV device: %s", doca_error_get_descr(result));
		goto out;
	}

	result = memos_fio_plugin_dev_wait_running(nkvdev, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("KV device did not reach running state: %s", doca_error_get_descr(result));
		goto out;
	}

	/*
	 * 3. Create a single I/O thread bound to that device. Passing pe=NULL makes
	 *    the thread create and own its own progress engine, and create_kv_io=false
	 *    defers the kvdev_io creation to memos_fio_plugin_thread_start() (which the
	 *    started device requires).
	 */
	struct memos_fio_plugin_dev dev = {
		.kvdev = nkvdev,
		.ns_file_name = cfg->dev_path,
	};
	const struct memos_fio_plugin_create_thread_attr thr_attr = {
		.kvdevs = &dev,
		.kvdevs_count = 1,
		.pe = NULL,
		.tidx = 0,
		.q_depth = MEMOS_FIO_PLUGIN_DEFAULT_IO_Q_DEPTH,
		.iov_entry_count = 1,
		.hash_type = cfg->hash_type,
		.create_kv_io = false,
	};

	result = memos_fio_plugin_thread_create(&thr_attr, &thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create KV I/O thread: %s", doca_error_get_descr(result));
		goto out;
	}

	/* 4. Start the thread: creates (if needed) and starts the kvdev_io. */
	result = memos_fio_plugin_thread_start(thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start KV I/O thread: %s", doca_error_get_descr(result));
		goto out;
	}

	/* Resolve the per-namespace binding used to submit store/retrieve tasks. */
	ns = memos_fio_plugin_thread_get_ns(thread, cfg->dev_path);
	if (ns == NULL) {
		DOCA_LOG_ERR("Failed to resolve namespace binding for '%s'", cfg->dev_path);
		goto out;
	}

	/*
	 * 5. Allocate hugepage-backed value buffers (same as fio mem=mmaphuge / 2M).
	 * Round up in size_t: value_len is uint32_t, so rounding in 32-bit arithmetic wraps to a
	 * short allocation for value_len within one alignment unit of UINT32_MAX.
	 */
	alloc_len =
		(((size_t)cfg->value_len + MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT - 1) / MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT) *
		MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT;
	wbuf = memos_fio_utils_allocate_aligned_buffer(MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT, alloc_len);
	rbuf = memos_fio_utils_allocate_aligned_buffer(MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT, alloc_len);
	if (wbuf == NULL || rbuf == NULL) {
		DOCA_LOG_ERR("Failed to allocate %zu-byte hugepage value buffers (need free 2MiB hugepages)",
			     alloc_len);
		goto out;
	}

	/* Fill the value with a recognizable pattern; clear the retrieve buffer. */
	for (uint32_t i = 0; i < cfg->value_len; i++)
		wbuf[i] = (uint8_t)(i & 0xff);
	memset(rbuf, 0, cfg->value_len);

	/* Same 8-byte key the thread will submit (hash(cfg->offset)). */
	memos_fio_utils_offset_to_key(cfg->hash_type, cfg->offset, key);
	memcpy(&key_u64, key, sizeof(key_u64));

	/*
	 * 6. STORE (operation == store or both). The key is derived from cfg->offset
	 *    via the thread's hash_type, so a later retrieve only has to reuse the same
	 *    offset. io_idx selects the iovec slot (0 is always valid for q_depth >= 1).
	 */
	if (cfg->operation == MEMOS_FIO_OP_STORE || cfg->operation == MEMOS_FIO_OP_BOTH) {
		result = memos_fio_plugin_thread_start_store_op(thread,
								ns,
								(union doca_data){.u64 = 0},
								cfg->offset,
								wbuf,
								cfg->value_len,
								0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit STORE: %s", doca_error_get_descr(result));
			goto out;
		}

		result = memos_fio_sample_wait_one(thread);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("STORE failed: %s", doca_error_get_descr(result));
			goto out;
		}
		DOCA_LOG_INFO("STORE ok: key=0x%016" PRIx64 " (offset=%" PRIu64 ") len=%u",
			      key_u64,
			      cfg->offset,
			      cfg->value_len);
	}

	/*
	 * 7. RETRIEVE (operation == retrieve or both) the same key into the fresh
	 *    buffer, then verify it against the deterministic pattern in wbuf. In
	 *    retrieve-only mode this checks a value stored by an earlier run that used
	 *    the same offset and value_len.
	 */
	if (cfg->operation == MEMOS_FIO_OP_RETRIEVE || cfg->operation == MEMOS_FIO_OP_BOTH) {
		result = memos_fio_plugin_thread_start_retrieve_op(thread,
								   ns,
								   (union doca_data){.u64 = 0},
								   cfg->offset,
								   rbuf,
								   cfg->value_len,
								   0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit RETRIEVE: %s", doca_error_get_descr(result));
			goto out;
		}

		result = memos_fio_sample_wait_one(thread);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("RETRIEVE failed: %s", doca_error_get_descr(result));
			goto out;
		}
		DOCA_LOG_INFO("RETRIEVE ok: key=0x%016" PRIx64 " (offset=%" PRIu64 ") len=%u",
			      key_u64,
			      cfg->offset,
			      cfg->value_len);

		if (memcmp(wbuf, rbuf, cfg->value_len) != 0) {
			DOCA_LOG_ERR("VERIFY failed: retrieved value does not match expected value");
			goto out;
		}
		DOCA_LOG_INFO("VERIFY ok: retrieved value matches expected value");
	}

	ret = EXIT_SUCCESS;

out:
	memos_fio_utils_free_aligned_buffer(rbuf, MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT, alloc_len);
	memos_fio_utils_free_aligned_buffer(wbuf, MEMOS_FIO_SAMPLE_BUFFER_ALIGNMENT, alloc_len);

	if (thread != NULL) {
		(void)memos_fio_plugin_thread_stop(thread);
		(void)memos_fio_plugin_thread_destroy(thread);
	}

	if (nkvdev != NULL) {
		(void)memos_fio_plugin_dev_stop(nkvdev, NULL);
		(void)memos_fio_plugin_dev_wait_idle(nkvdev, NULL);
		(void)memos_fio_plugin_dev_destroy(nkvdev);
	}

	return ret;
}

static void memos_fio_sample_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -d, --device <path>    NVMe character device path      (required)\n");
	printf("  -l, --value-len <n>    4KiB-aligned value size         (default: 8388608 / 8MiB)\n");
	printf("  -t, --hash-type <n>    offset->key hash: 1=xxh64, 2=splitmix64, 3=murmur3_64 (default: 2)\n");
	printf("  -o, --offset <n>       byte offset -> KV key           (default: 0)\n");
	printf("  -p, --operation <op>   store | retrieve | both         (default: both)\n");
	printf("  -h, --help             print this help and exit\n");
}

/* Outcome of memos_fio_parse_args(). */
enum memos_fio_parse_result {
	MEMOS_FIO_PARSE_OK,    /**< Options parsed; proceed to run. */
	MEMOS_FIO_PARSE_EXIT,  /**< Usage printed (--help); exit success. */
	MEMOS_FIO_PARSE_ERROR, /**< Invalid option; exit failure. */
};

/* Parse a whole non-negative number, rejecting garbage / overflow with an error. */
static bool memos_fio_parse_u64(const char *name, const char *arg, uint64_t *out)
{
	char *end = NULL;
	unsigned long long val;

	errno = 0;
	val = strtoull(arg, &end, 0);
	if (arg[0] == '\0' || end == NULL || *end != '\0' || errno != 0) {
		fprintf(stderr, "Invalid value for --%s: '%s'\n", name, arg);
		return false;
	}

	*out = (uint64_t)val;
	return true;
}

/* As memos_fio_parse_u64(), but also reject values that do not fit in a uint32_t. */
static bool memos_fio_parse_u32(const char *name, const char *arg, uint32_t *out)
{
	uint64_t val;

	if (!memos_fio_parse_u64(name, arg, &val))
		return false;

	if (val > UINT32_MAX) {
		fprintf(stderr, "Value for --%s is too large: '%s' (max %u)\n", name, arg, UINT32_MAX);
		return false;
	}

	*out = (uint32_t)val;
	return true;
}

/* Parse the operation name (store/retrieve/both), logging an error on mismatch. */
static bool memos_fio_parse_operation(const char *arg, enum memos_fio_operation *out)
{
	if (strcmp(arg, "store") == 0)
		*out = MEMOS_FIO_OP_STORE;
	else if (strcmp(arg, "retrieve") == 0)
		*out = MEMOS_FIO_OP_RETRIEVE;
	else if (strcmp(arg, "both") == 0)
		*out = MEMOS_FIO_OP_BOTH;
	else {
		fprintf(stderr, "Invalid --operation: '%s' (use store, retrieve, or both)\n", arg);
		return false;
	}

	return true;
}

/* Fill @p cfg with defaults, then override from command-line options. */
static enum memos_fio_parse_result memos_fio_parse_args(int argc, char **argv, struct memos_fio_sample_cfg *cfg)
{
	static const struct option long_opts[] = {
		{"device", required_argument, NULL, 'd'},
		{"value-len", required_argument, NULL, 'l'},
		{"hash-type", required_argument, NULL, 't'},
		{"offset", required_argument, NULL, 'o'},
		{"operation", required_argument, NULL, 'p'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int opt;

	*cfg = (struct memos_fio_sample_cfg){
		.dev_path = NULL,
		.offset = 0,
		.value_len = MEMOS_FIO_SAMPLE_VALUE_LEN_DEFAULT,
		.hash_type = MEMOS_FIO_PLUGIN_HASH_TYPE_SPLITMIX64,
		.operation = MEMOS_FIO_OP_BOTH,
	};

	while ((opt = getopt_long(argc, argv, "d:l:t:o:p:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cfg->dev_path = optarg;
			break;
		case 'l':
			if (!memos_fio_parse_u32("value-len", optarg, &cfg->value_len))
				return MEMOS_FIO_PARSE_ERROR;
			break;
		case 't':
			if (!memos_fio_parse_u32("hash-type", optarg, &cfg->hash_type))
				return MEMOS_FIO_PARSE_ERROR;
			break;
		case 'o':
			if (!memos_fio_parse_u64("offset", optarg, &cfg->offset))
				return MEMOS_FIO_PARSE_ERROR;
			break;
		case 'p':
			if (!memos_fio_parse_operation(optarg, &cfg->operation))
				return MEMOS_FIO_PARSE_ERROR;
			break;
		case 'h':
			memos_fio_sample_usage(argv[0]);
			return MEMOS_FIO_PARSE_EXIT;
		default:
			memos_fio_sample_usage(argv[0]);
			return MEMOS_FIO_PARSE_ERROR;
		}
	}

	if (cfg->dev_path == NULL || cfg->dev_path[0] == '\0') {
		fprintf(stderr, "Missing required -d, --device <path>\n");
		return MEMOS_FIO_PARSE_ERROR;
	}

	if (cfg->value_len < MEMOS_FIO_SAMPLE_VALUE_LEN_ALIGNMENT ||
	    cfg->value_len % MEMOS_FIO_SAMPLE_VALUE_LEN_ALIGNMENT != 0) {
		fprintf(stderr,
			"Invalid --value-len=%u: must be at least %u and aligned to %u bytes\n",
			cfg->value_len,
			MEMOS_FIO_SAMPLE_VALUE_LEN_ALIGNMENT,
			MEMOS_FIO_SAMPLE_VALUE_LEN_ALIGNMENT);
		return MEMOS_FIO_PARSE_ERROR;
	}

	if (cfg->hash_type != MEMOS_FIO_PLUGIN_HASH_TYPE_XXH64 &&
	    cfg->hash_type != MEMOS_FIO_PLUGIN_HASH_TYPE_SPLITMIX64 &&
	    cfg->hash_type != MEMOS_FIO_PLUGIN_HASH_TYPE_MURMUR3_64) {
		fprintf(stderr, "Invalid --hash-type=%u (use 1=xxh64, 2=splitmix64, 3=murmur3_64)\n", cfg->hash_type);
		return MEMOS_FIO_PARSE_ERROR;
	}

	return MEMOS_FIO_PARSE_OK;
}

int main(int argc, char **argv)
{
	struct memos_fio_sample_cfg cfg;

	switch (memos_fio_parse_args(argc, argv, &cfg)) {
	case MEMOS_FIO_PARSE_OK:
		break;
	case MEMOS_FIO_PARSE_EXIT:
		return EXIT_SUCCESS;
	case MEMOS_FIO_PARSE_ERROR:
	default:
		return EXIT_FAILURE;
	}

	return memos_fio_run(&cfg);
}
