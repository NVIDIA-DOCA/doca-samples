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

#ifndef MEMOS_FIO_UTILS_H_
#define MEMOS_FIO_UTILS_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "memos_fio_plugin_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio_utils.h
 * @brief Shared helpers for the memos_fio fio plugin (logging, parsing, device query).
 */

/** fio offset → KV key hash (engine option hash_type). */
enum memos_fio_plugin_hash_type {
	MEMOS_FIO_PLUGIN_HASH_TYPE_XXH64 = 1,
	MEMOS_FIO_PLUGIN_HASH_TYPE_SPLITMIX64 = 2,
	MEMOS_FIO_PLUGIN_HASH_TYPE_MURMUR3_64 = 3,
};

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

/**
 * @brief log formatted message as error, will include function name automatically
 *
 * @param [in] format string that defines the format of printed message, similar to string passed to printf
 * @param [in] ... any number of arguments, which will be interpolated in the format string. similar to printf
 */
#define LOG_ERR_FORMAT(format, ...) \
	do { \
		DOCA_LOG_ERR("%s failed: " format, __func__, ##__VA_ARGS__); \
	} while (0)

/**
 * @brief log msg as error. Includes the function name automatically
 *
 * @param [in] msg describes the error that occurred
 */
#define LOG_ERR(msg) \
	do { \
		LOG_ERR_FORMAT("%s", (msg)); \
	} while (0)

/**
 * @brief verifies that variable received from user is not NULL
 * logs error and returns expression in case of null
 *
 * @param [in] ptr_var_name any pointer type that can be compared with NULL
 * @param [in] exp expression that is returned in case variable is NULL
 */
#define RETURN_EXP_ON_NULL(ptr_var_name, exp) \
	do { \
		if ((ptr_var_name) == NULL) { \
			LOG_ERR_FORMAT("variable '%s' is NULL", (#ptr_var_name)); \
			return (exp); \
		} \
	} while (0)

/**
 * @brief verifies that variable received from user is not NULL
 * logs error and returns invalid value in case of null
 *
 * @param [in] ptr_var_name any pointer type that can be compared with NULL
 */
#define RETURN_INVALID_ON_NULL(ptr_var_name) \
	do { \
		RETURN_EXP_ON_NULL(ptr_var_name, DOCA_ERROR_INVALID_VALUE); \
	} while (0)

/**
 * @brief verifies that variable received from user is not NULL
 * logs error and returns null in case of null
 *
 * @param [in] ptr_var_name any pointer type that can be compared with NULL
 */
#define RETURN_NULL_ON_NULL(ptr_var_name) \
	do { \
		RETURN_EXP_ON_NULL(ptr_var_name, NULL); \
	} while (0)

/** No-op in release builds; use to silence hot-path debug logs. */
#define DISABLE_DOCA_LOG_DBG(...)

/** Progress @p ctx until it reaches @p desired_state. */
doca_error_t memos_fio_utils_wait_state(struct doca_ctx *ctx, struct doca_pe *pe, enum doca_ctx_states desired_state);

struct doca_dev;

/** Open a DOCA device by InfiniBand device name (legacy helper; KV path uses /dev/ngXnY). */
doca_error_t memos_fio_utils_open_device(const char *device_name, struct doca_dev **doca_device);

/** Return current doca_ctx state without progressing. */
static inline enum doca_ctx_states memos_fio_utils_get_ctx_state(const struct doca_ctx *ctx)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_IDLE;

	(void)doca_ctx_get_state(ctx, &state);
	return state;
}

/** Log and return false if @p ctx is not in @p check_state. */
bool memos_fio_utils_check_ctx_state(const struct doca_ctx *ctx,
				     enum doca_ctx_states check_state,
				     const char *error_msg);

/**
 * @brief Allocate an IO value buffer the same way fio does with mem=mmaphuge.
 *
 * Uses 2MiB hugetlb pages (MAP_HUGETLB | MAP_HUGE_2MB), rounded up to at least
 * max(alignment, 2MiB). Pass the same @p alignment and @p size to
 * memos_fio_utils_free_aligned_buffer().
 */
void *memos_fio_utils_allocate_aligned_buffer(size_t alignment, size_t size);

/** Free a buffer from memos_fio_utils_allocate_aligned_buffer(). */
void memos_fio_utils_free_aligned_buffer(void *buffer, size_t alignment, size_t size);

/**
 * @brief Parse fio filename as colon-separated NVMe character device paths.
 *
 * Splits @p file_name (e.g. "/dev/ng0n1:/dev/ng0n0") in place by writing '\\0' over
 * ':' delimiters. @p nsids[i] then points at each path segment within @p file_name.
 *
 * @param [in,out] file_name Writable colon-separated device paths (fio filename).
 * @param [out] nsids Output pointers; must hold at least @p nsid_arr_size entries.
 * @param [in] nsid_arr_size Maximum number of paths to store.
 * @param [out] nsids_count Number of paths written on success.
 *
 * @return true on success, false on parse error.
 */
bool memos_fio_utils_parse_nsid_file_name(char *file_name,
					  const char **nsids,
					  uint32_t nsid_arr_size,
					  uint32_t *nsids_count);

/**
 * @brief Validate fio-log-level / doca-log-level engine option.
 *
 * Accepts 0 (use default in init) or any value from enum doca_log_level.
 */
bool memos_fio_utils_validate_doca_log_level(uint32_t level);

/**
 * @brief Validate numjobs against online CPU count and plugin maximum.
 *
 * Prints a message to stderr and returns false if num_io_threads is too large.
 */
bool memos_fio_utils_validate_num_io_threads(uint32_t num_io_threads);

/** Create standard and SDK log backends and set global log limits. */
bool memos_fio_utils_init_log(uint32_t fio_log_level, uint32_t doca_log_level, struct doca_log_backend **sdk_log);

/**
 * @brief Read namespace NGUID from an NVMe character device path.
 *
 * Opens @p kvdev_path (e.g. /dev/ng0n1) and issues NVMe Identify Namespace,
 * matching the query doca_kv_nvme_kernel_dev_create() performs when it attaches.
 *
 * @param [in] kvdev_path NVMe character device path.
 * @param [out] nguid Buffer for NGUID bytes.
 * @param [in] nguid_len Size of @p nguid; must be at least DOCA_KV_DEV_NGUID_LEN.
 *
 * @return true on success, false on error.
 */
bool memos_fio_utils_get_nguid(const char *kvdev_path, uint8_t *nguid, uint16_t nguid_len);

/**
 * @brief Derive an 8-byte memos KV key from an fio file offset.
 *
 * @p hash_type selects the hash (see @ref memos_fio_plugin_hash_type).
 */
void memos_fio_utils_offset_to_key(uint32_t hash_type, uint64_t offset, uint8_t key[MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_UTILS_H_ */
