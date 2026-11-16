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
 * DMA helper for the memos_initiator_ref_app reference application. Moves payloads between a controller-local
 * scratch buffer and the host memory addressed by a command's PRP entries, then completes the NVMe
 * request from the DMA completion callback.
 *
 * Scope: PRP1 / PRP2 only. A transfer that would need a PRP list (more than two pages) or an SGL
 * is rejected. MDTS=1 caps a host transfer at two pages, so a spec-compliant host never builds one.
 *
 * An engine is not a singleton. A doca_dma ctx must be drained by whichever PE it is connected to,
 * and this app has several: admin commands run on app->admin_pe on the main thread, while KV IO
 * commands are dispatched on a per-worker io_pe on a pinned worker thread. Each therefore owns its
 * own engine, created with the PE that will drain it - sharing one across threads would mean
 * submitting to a ctx whose completions another thread is polling.
 */

#ifndef MEMOS_INITIATOR_REF_APP_DMA_H_
#define MEMOS_INITIATOR_REF_APP_DMA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <doca_error.h>

struct memos_initiator_ref_app_resources;
struct memos_initiator_ref_app_dma;
struct doca_devemu_nvme_req;
struct doca_buf;
struct doca_pe;

/* Largest single payload we DMA. Admin structures (Identify data, log pages) are <= 4 KiB, and the
 * KV namespace advertises MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN == this, so one value is always
 * one slot. */
#define MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE 4096
/* Concurrent DMA operations, per engine; each op owns one scratch slot for its lifetime, so an
 * engine costs pool_size * MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE of scratch.
 *
 * Admin commands are effectively serialized, so a handful of slots is plenty. A KV worker is not:
 * its bound is the host's queue depth, which this app permits up to
 * MEMOS_INITIATOR_REF_APP_MAX_QUEUE_SIZE per queue across several queues - far too many to
 * pre-allocate. The IO pool is therefore sized to cover realistic depths (1 MiB of scratch per
 * worker) and exhaustion beyond it is handled as backpressure, not as an error: see the retryable
 * completion in memos_initiator_ref_app_dma_to_host(). */
#define MEMOS_INITIATOR_REF_APP_DMA_ADMIN_POOL_SIZE 8
#define MEMOS_INITIATOR_REF_APP_DMA_IO_POOL_SIZE 256

/**
 * @brief Create a DMA engine bound to @p pe.
 *
 * Creates a doca_dma context on app->dev, connects it to @p pe (which must be the PE whose owner
 * thread will both submit through and drain this engine), allocates a page-aligned scratch region of
 * @p pool_size slots of MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE wrapped by a local doca_mmap, and a
 * doca_buf_inventory sized for the in-flight buffers. On any failure all partial resources are
 * released. Must be called after app->host_mmap is cached (host PRP addresses resolve against it).
 *
 * @param app        application state with a started nvme_dev and a cached host_mmap.
 * @param pe         progress engine that will drain this engine's completions.
 * @param pool_size  concurrent operations (and scratch slots) to provision.
 * @param out_eng    set to the new engine on success.
 * @return DOCA_SUCCESS on success, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_dma_engine_create(struct memos_initiator_ref_app_resources *app,
						       struct doca_pe *pe,
						       uint32_t pool_size,
						       struct memos_initiator_ref_app_dma **out_eng);

/**
 * @brief Destroy an engine created by memos_initiator_ref_app_dma_engine_create(). No-op on NULL.
 *
 * Must run before app->host_mmap is destroyed and after the queues feeding this engine are quiesced,
 * so no DMA remains in flight.
 *
 * @param app  application state.
 * @param eng  the engine to destroy.
 * @return DOCA_SUCCESS when the engine is gone; otherwise the failure that kept it alive, in which
 * case everything a pending task can reach is deliberately retained.
 */
doca_error_t memos_initiator_ref_app_dma_engine_destroy(struct memos_initiator_ref_app_resources *app,
							struct memos_initiator_ref_app_dma *eng);

/**
 * @brief Bring up the admin DMA engine on app->admin_pe, published as app->dma.
 *
 * @param app  application state with a started nvme_dev and a cached host_mmap.
 * @return DOCA_SUCCESS on success, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_dma_init(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Tear down the admin DMA engine (reverse of memos_initiator_ref_app_dma_init). No-op if not up.
 *
 * @param app  application state.
 * @return DOCA_SUCCESS when the engine is gone; otherwise the failure that kept it alive, in which
 * case app->dma is still set and everything a pending task can reach is deliberately retained. A
 * caller that goes on to restart the datapath must treat that as fatal rather than build a second
 * engine beside the first.
 */
doca_error_t memos_initiator_ref_app_dma_teardown(struct memos_initiator_ref_app_resources *app);

/**
 * @brief DMA a payload to the host on the admin engine and complete the request.
 *
 * Copies @p len bytes into a scratch slot, resolves the host destination from the command's
 * PRP1/PRP2 (PRP-list and SGL transfers are rejected) and submits a doca_dma memcpy. The request is
 * completed from the DMA callback, with @p success_status on success or DATA_XFER_ERROR on failure,
 * and is owned by this call afterward.
 */
doca_error_t memos_initiator_ref_app_dma_to_host(struct memos_initiator_ref_app_resources *app,
						 struct doca_devemu_nvme_req *req,
						 const void *sqe,
						 const void *src,
						 size_t len,
						 uint16_t success_status);

/**
 * @brief Wrap a command's host value buffer as a doca_buf list, for use in place.
 *
 * @param host_is_src  true when the host buffer is the source (Store), false when it is the
 *                     destination (Retrieve) and its data length is the operation's output.
 * @return DOCA_SUCCESS, DOCA_ERROR_NOT_SUPPORTED if the transfer needs a PRP list, or an error from
 *         building the buffer list.
 */
doca_error_t memos_initiator_ref_app_dma_host_buf_get(struct memos_initiator_ref_app_resources *app,
						      struct memos_initiator_ref_app_dma *eng,
						      const void *sqe,
						      size_t len,
						      bool host_is_src,
						      struct doca_buf **head,
						      uint32_t *list_len);

/** Release a list from memos_initiator_ref_app_dma_host_buf_get(); frees every chained buf. */
void memos_initiator_ref_app_dma_host_buf_put(struct doca_buf *head);

#endif /* MEMOS_INITIATOR_REF_APP_DMA_H_ */
