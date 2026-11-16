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
 * IO queue management for the memos_initiator_ref_app reference application. Owns a qid-indexed registry of the IO
 * Completion/Submission queues created by the host via the Create/Delete IO CQ/SQ admin commands,
 * and the IO request handler. IO queues are bound round-robin (by qid) across the per-core worker
 * io ctxs (memos_initiator_ref_app_worker), each on its own pinned thread; the shared dispatcher routes requests
 * to the IO handler based on the per-SQ binding user data. Queue lifecycle runs on the main thread
 * and quiesces the owning worker around bind/enable/disable.
 *
 * The IO handler dispatches the KV Store, Retrieve and Exist opcodes against the namespace's backing
 * store, and rejects SGL, unknown namespaces and unsupported opcodes.
 */

#ifndef MEMOS_INITIATOR_REF_APP_IO_H_
#define MEMOS_INITIATOR_REF_APP_IO_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>

struct memos_initiator_ref_app_resources;
struct memos_initiator_ref_app_worker;
struct doca_devemu_nvme_req;

/* Compile-time hard upper bound on the IO queue depth (entries) this app will ever accept. The
 * runtime limit (app->max_io_queue_size, queried from the device) is clamped to this. Matches the
 * firmware-canned NVMe CAP.MQES of 0x0fff (4096 entries). No per-depth arrays are sized by this; it
 * is purely a sanity ceiling on the host-requested qsize. */
#define MEMOS_INITIATOR_REF_APP_MAX_QUEUE_SIZE 4096
/* Fallback IO queue depth (entries) used when the device capability query fails. */
#define MEMOS_INITIATOR_REF_APP_DEFAULT_QUEUE_SIZE 1024

/**
 * @brief Allocate the IO queue registry. Called from admin bring-up once the shared io ctx is up.
 *
 * @param app  application state.
 * @return DOCA_SUCCESS on success, DOCA_ERROR_NO_MEMORY otherwise.
 */
doca_error_t memos_initiator_ref_app_io_init(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Tear down every live IO SQ then CQ and free the registry. No-op if not initialized.
 *
 * Must run while the shared io ctx is still up (queues are unbound from it) and before the admin
 * queues / io ctx are torn down.
 *
 * @param app  application state.
 */
void memos_initiator_ref_app_io_teardown(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Create an IO Completion Queue (Create-IO-CQ admin command).
 *
 * @param app    application state.
 * @param qid    completion queue identifier (1..app->max_io_queues).
 * @param qsize  queue depth, 1-based (host CDW10 value + 1).
 * @param iv     MSI-X interrupt vector.
 * @param ien    interrupts-enabled flag.
 * @param pc     physically-contiguous flag.
 * @param prp1   queue base address (DPTR.PRP1).
 * @return the NVMe completion status to return to the host (0 on success).
 */
uint16_t memos_initiator_ref_app_io_create_cq(struct memos_initiator_ref_app_resources *app,
					      uint16_t qid,
					      uint16_t qsize,
					      uint16_t iv,
					      uint8_t ien,
					      uint8_t pc,
					      uint64_t prp1);

/**
 * @brief Create an IO Submission Queue (Create-IO-SQ admin command).
 *
 * @param app    application state.
 * @param qid    submission queue identifier (1..app->max_io_queues).
 * @param qsize  queue depth, 1-based (host CDW10 value + 1).
 * @param cqid   identifier of the completion queue this SQ posts to.
 * @param qprio  queue priority (ignored by this app).
 * @param pc     physically-contiguous flag.
 * @param prp1   queue base address (DPTR.PRP1).
 * @return the NVMe completion status to return to the host (0 on success).
 */
uint16_t memos_initiator_ref_app_io_create_sq(struct memos_initiator_ref_app_resources *app,
					      uint16_t qid,
					      uint16_t qsize,
					      uint16_t cqid,
					      uint8_t qprio,
					      uint8_t pc,
					      uint64_t prp1);

/**
 * @brief Delete an IO Submission Queue (Delete-IO-SQ admin command).
 *
 * @param app  application state.
 * @param qid  submission queue identifier.
 * @return the NVMe completion status to return to the host (0 on success).
 */
uint16_t memos_initiator_ref_app_io_delete_sq(struct memos_initiator_ref_app_resources *app, uint16_t qid);

/**
 * @brief Delete an IO Completion Queue (Delete-IO-CQ admin command). Refused while a SQ references it.
 *
 * @param app  application state.
 * @param qid  completion queue identifier.
 * @return the NVMe completion status to return to the host (0 on success).
 */
uint16_t memos_initiator_ref_app_io_delete_cq(struct memos_initiator_ref_app_resources *app, uint16_t qid);

/**
 * @brief IO request handler, invoked by the admin dispatcher for requests on an IO SQ.
 *
 * Rejects SGL (PSDT != 0 -> INVALID_FIELD) and any NSID other than the single namespace
 * (-> INVALID_NS). Dispatches KV Store, Retrieve and Exist; any other opcode -> INVALID_OPCODE. A
 * command is issued and completed inside this call.
 *
 * @param req            the arrived request (ownership taken until completed).
 * @param sqe            the 64-byte submission queue entry, little-endian.
 * @param sq_user_data   the originating SQ's binding user data (its registry slot).
 * @param req_user_data  the per-request scratch buffer, valid until the request is completed.
 */
void memos_initiator_ref_app_io_handle(struct doca_devemu_nvme_req *req,
				       const void *sqe,
				       void *sq_user_data,
				       void *req_user_data);

/**
 * @brief Hold an IO completion back for later release (--io-defer-count).
 *
 * Debug/test hook: instead of completing inline, the request is parked on the owning worker until the
 * configured batch size is reached (or the batch ages out, see memos_initiator_ref_app_io_defer_progress()). With
 * --io-defer-reverse the batch is released newest-first, so a later command completes before the
 * earlier ones it arrived with - the pattern that makes a host free a command tag and reuse an SQ
 * slot while the controller still owns an older command.
 *
 * Must be called on the thread that owns the worker (its own thread, or the main thread while the
 * worker is paused).
 *
 * @param w       worker owning the SQ the request arrived on. NULL disables deferral.
 * @param req     the request (ownership passes to the deferral list when held).
 * @param status  NVMe completion status to post on release.
 * @return true if the request was held (caller must not complete it), false to complete inline.
 */
bool memos_initiator_ref_app_io_defer_hold(struct memos_initiator_ref_app_worker *w,
					   struct doca_devemu_nvme_req *req,
					   uint16_t status);

/**
 * @brief Service the deferral list once per worker loop iteration.
 *
 * Releases a batch that will not fill: once the worker's PE has no work left there is nothing more to
 * wait for, so the batch is completed immediately (minus the oldest --io-defer-keep requests). Without
 * that, a host keeping fewer than --io-defer-count commands in flight - a partition scan, for instance
 * - would pay the full --io-defer-max-us age-out on every batch, which is slow enough to look like a
 * hang. Whatever is still held after that is bounded by the age-out.
 *
 * @param w        the worker to service. No-op when nothing is held.
 * @param pe_idle  true when the worker's progress engine reported no work this iteration.
 */
void memos_initiator_ref_app_io_defer_progress(struct memos_initiator_ref_app_worker *w, bool pe_idle);

/**
 * @brief Immediately complete every request held by a worker.
 *
 * Called before the worker acknowledges a pause and when its thread exits, so held requests never
 * outlive the thread that can release them (a held request keeps the library's per-SQ outstanding
 * count non-zero, which would stall the queue-disable flush).
 *
 * @param w  the worker to drain. No-op when nothing is held.
 */
void memos_initiator_ref_app_io_defer_flush(struct memos_initiator_ref_app_worker *w);

/**
 * @brief Complete every held request and push the resulting CQEs all the way to the host.
 *
 * memos_initiator_ref_app_io_defer_flush() only writes the CQEs into the library's CQ shadow and queues the DMA;
 * the transfer needs progress engine ticks. Disabling a CQ discards a DMA that is merely queued, so a
 * completion released just before teardown would never reach the host and the host would wait on that
 * command forever. Use this wherever held requests must be settled before the queues are touched.
 *
 * Must be called on the thread that owns the worker's PE.
 *
 * @param w  the worker to drain. No-op when nothing is held and the PE is idle.
 */
void memos_initiator_ref_app_io_defer_drain(struct memos_initiator_ref_app_worker *w);

#endif /* MEMOS_INITIATOR_REF_APP_IO_H_ */
