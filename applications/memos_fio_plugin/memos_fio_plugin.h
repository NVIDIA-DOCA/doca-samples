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

#ifndef MEMOS_FIO_PLUGIN_H_
#define MEMOS_FIO_PLUGIN_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio_plugin.h
 * @brief fio engine option layout for ioengine=memos_fio.
 *
 * Field order and types must match the option table in memos_fio_plugin.c.
 * Target device paths come from the standard fio filename= option, not from here.
 */

/** fio engine-specific options (must match memos_fio_plugin.c option table layout). */
struct memos_fio_plugin_options {
	void *pad; /**< Required padding; fio expects the first member for alignment. */

	uint32_t kv_iovcnt; /**< Number of iovecs per KV operation (1..MEMOS_FIO_PLUGIN_IOV_ENTRY_COUNT_MAX). */

	uint32_t fio_log_level;	 /**< DOCA_LOG_* lower limit; 0 = default. */
	uint32_t doca_log_level; /**< DOCA SDK (internal library) log limit; 0 = default. */
	uint32_t hash_type;	 /**< Offset-to-key hash; 0 = default (splitmix64). */
};

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_PLUGIN_H_ */
