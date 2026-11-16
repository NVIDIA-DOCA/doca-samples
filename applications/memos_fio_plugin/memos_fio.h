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

#ifndef MEMOS_FIO_H_
#define MEMOS_FIO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memos_fio.h
 * @brief Minimal standalone sample that exercises the memos_fio_plugin library.
 *
 * The fio ioengine (libdoca_memos_fio_plugin.so) is built on top of a small set of
 * reusable modules (memos_fio_plugin_dev, memos_fio_plugin_thread, memos_fio_utils). This
 * sample shows how to drive those modules directly - without fio - to perform a
 * single KV store + retrieve round-trip against a DOCA NVMe kernel KV namespace.
 */

/** Which KV operation(s) the sample performs. */
enum memos_fio_operation {
	MEMOS_FIO_OP_STORE,    /**< Store only. */
	MEMOS_FIO_OP_RETRIEVE, /**< Retrieve only (verify against the deterministic pattern). */
	MEMOS_FIO_OP_BOTH,     /**< Store then retrieve then verify (default). */
};

/** Configuration for one KV store/retrieve round-trip. */
struct memos_fio_sample_cfg {
	const char *dev_path;		    /**< Required NVMe character device path (e.g. "/dev/ng0n1"). */
	uint64_t offset;		    /**< fio-style byte offset; hashed into the 8-byte KV key. */
	uint32_t value_len;		    /**< 4KiB-aligned value size; minimum 4KiB, default 8MiB. */
	uint32_t hash_type;		    /**< Offset-to-key hash (enum memos_fio_plugin_hash_type). */
	enum memos_fio_operation operation; /**< Store, retrieve, or both. */
};

/**
 * @brief Run one store + retrieve + verify round-trip.
 *
 * @param [in] cfg Sample configuration.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
 */
int memos_fio_run(const struct memos_fio_sample_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* MEMOS_FIO_H_ */
