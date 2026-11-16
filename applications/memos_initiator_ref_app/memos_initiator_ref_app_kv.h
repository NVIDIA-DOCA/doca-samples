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
 * The key/value store backing this app's single KV namespace: a DOCA Memos Initiator volume on a
 * remote NVMe-oF target, named entirely by runtime parameters so one binary serves any fabric.
 *
 * Each IO worker owns an I/O context and a progress engine, and is the only thread that touches
 * them. An operation is issued and awaited inline on the calling worker, with no lock, and values
 * move directly between the host buffers and the target.
 */

#ifndef MEMOS_INITIATOR_REF_APP_KV_H_
#define MEMOS_INITIATOR_REF_APP_KV_H_

#include <stddef.h>
#include <stdint.h>

#include <doca_error.h>

struct memos_initiator_ref_app_resources;
struct doca_buf;
struct doca_dev;

/**
 * @brief Connect the target, attach its namespace and start the I/O context. Idempotent per app.
 *
 * @param app  application state; the store is published on app->kv.
 * @return DOCA_SUCCESS on success, an error if the target could not be reached or attached.
 */
doca_error_t memos_initiator_ref_app_kv_init(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Detach the namespace, disconnect the target and release everything. No-op if never up.
 *
 * Must run after every IO worker has stopped, so no command is in flight.
 *
 * @param app  application state.
 */
void memos_initiator_ref_app_kv_teardown(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Store a value, replacing any previous value for the same key.
 *
 * The initiator reads straight out of @p value_buf, which the caller owns and releases.
 *
 * @param value_buf       buffer list holding the value, NULL for a zero-length value.
 * @param value_list_len  number of buffers in @p value_buf, 0 when it is NULL.
 * @param opts       NVME_KV_SQE_STORE_SIKE / _SINKE bits. The initiator store task has no
 *                   equivalent, so a non-zero value is rejected rather than silently downgraded to
 *                   an unconditional store.
 * @return an NVMe status code translated from the initiator's outcome.
 */
uint16_t memos_initiator_ref_app_kv_store(struct memos_initiator_ref_app_resources *app,
					  uint32_t worker_idx,
					  const uint8_t *key,
					  uint8_t key_len,
					  struct doca_buf *value_buf,
					  uint32_t value_list_len,
					  uint32_t len,
					  uint32_t opts);

/**
 * @brief Retrieve a value into @p value_buf, which the caller owns and releases.
 *
 * At most @p out_cap bytes are written, while @p out_len reports the *stored* length, which the KV
 * command set requires the completion to carry in CQE DW0 - a host that supplied a short buffer
 * learns the real size from it.
 *
 * @param value_buf       buffer list to receive the value. Never NULL: the initiator cannot retrieve
 *                        into a zero-length buffer, so a Retrieve without one is rejected earlier.
 * @param value_list_len  number of buffers in @p value_buf.
 * @param out_cap         capacity of @p value_buf (the host buffer size from the command), non-zero.
 * @param out_len         set to the stored value length on success.
 * @return SUCCESS, a status forwarded from the target (KV_KEY_NOT_EXIST when the key is absent), or
 *         one translated from a local failure.
 */
uint16_t memos_initiator_ref_app_kv_retrieve(struct memos_initiator_ref_app_resources *app,
					     uint32_t worker_idx,
					     const uint8_t *key,
					     uint8_t key_len,
					     struct doca_buf *value_buf,
					     uint32_t value_list_len,
					     uint32_t out_cap,
					     uint32_t *out_len);

/**
 * @brief Report whether a key is present.
 *
 * @param app      application state with an initialised backend.
 * @param key      key bytes.
 * @param key_len  significant key length in bytes.
 * @return SUCCESS if present, KV_KEY_NOT_EXIST otherwise.
 */
uint16_t memos_initiator_ref_app_kv_exist(struct memos_initiator_ref_app_resources *app,
					  uint32_t worker_idx,
					  const uint8_t *key,
					  uint8_t key_len);

/**
 * @brief Progress the control engine once, from the main thread only.
 *
 * Must be called regularly while the backend is up, not only during a command: the initiator sends
 * its keep-alive messages from this engine, and a target drops a connection gone quiet for ~30 s.
 *
 * @param app  application state.
 * @return number of events processed.
 */
int memos_initiator_ref_app_kv_progress(struct memos_initiator_ref_app_resources *app);

/** The --rdma-dev device, or NULL if the backend is not up. The host PRP mmap must include it. */
struct doca_dev *memos_initiator_ref_app_kv_get_dev(const struct memos_initiator_ref_app_resources *app);

/** Length of an NVMe namespace GUID. */
#define MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN 16

/**
 * @brief Report the namespace geometry discovered from the target.
 *
 * Identify CNS 05h and the namespace identity descriptors are built from these, so the namespace the
 * host sees describes the volume actually backing it rather than a compiled-in guess. Values are
 * clamped to what this application can move.
 *
 * @param app            application state with an initialised backend.
 * @param max_key_len    set to the advertised maximum key length.
 * @param max_value_len  set to the advertised maximum value length.
 * @param ns_size        set to the namespace size in bytes.
 * @param nguid          filled with the target namespace's GUID.
 * @return DOCA_SUCCESS, or DOCA_ERROR_BAD_STATE if the backend is not up.
 */
doca_error_t memos_initiator_ref_app_kv_get_geometry(const struct memos_initiator_ref_app_resources *app,
						     uint16_t *max_key_len,
						     uint32_t *max_value_len,
						     uint64_t *ns_size,
						     uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN]);

#endif /* MEMOS_INITIATOR_REF_APP_KV_H_ */
