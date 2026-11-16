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
 * Per-core IO datapath workers for the memos_initiator_ref_app reference application.
 *
 * Each worker owns a private progress engine, an NVMe io ctx (which in turn owns the library's
 * datapath worker: doorbell drain, CQE DMA, MSI-X) and a thread pinned to one core of --core-mask.
 * Host IO queues are bound round-robin across the workers by qid. The main thread keeps ownership of
 * the device/admin/BAR control plane; queue lifecycle (create/start/bind/enable/disable/stop) runs
 * there too, so a per-worker quiesce handshake (pause/resume) serializes the brief enable/disable
 * window against the worker's busy-poll loop.
 */

#ifndef MEMOS_INITIATOR_REF_APP_WORKER_H_
#define MEMOS_INITIATOR_REF_APP_WORKER_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <doca_error.h>

struct memos_initiator_ref_app_resources;
struct memos_initiator_ref_app_dma;
struct doca_pe;
struct doca_devemu_nvme_io;
struct doca_devemu_nvme_req;

/* Upper bound on IO completions a worker may hold for --io-defer-count; the runtime count is
 * validated against this at argument-parse time. Sized to the largest IO queue this app accepts
 * (MEMOS_INITIATOR_REF_APP_MAX_QUEUE_SIZE) so a batch can span a whole queue - note that holding a full queue
 * depth stalls the host instead of exercising it, so useful values stay well below the depth. */
#define MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER 4096

/* One IO completion held back by --io-defer-count, replayed verbatim on release. */
struct memos_initiator_ref_app_deferred_req {
	struct doca_devemu_nvme_req *req;
	uint16_t status;
};

/**
 * @brief A single per-core IO datapath worker.
 */
struct memos_initiator_ref_app_worker {
	struct memos_initiator_ref_app_resources *app;
	struct doca_pe *io_pe;
	struct doca_devemu_nvme_io *io;
	/* This worker's own DMA engine, connected to io_pe. KV Store/Retrieve move value bytes through
	 * it. Private to the worker precisely because a doca_dma ctx must be submitted to and drained by
	 * the same thread; the admin engine on app->dma cannot be borrowed here. */
	struct memos_initiator_ref_app_dma *dma;
	pthread_t tid;
	int core;	      /* CPU this worker's thread is pinned to. */
	bool thread_started;  /* tid is valid and joinable. */
	atomic_int pause_req; /* main -> worker: stop progressing the io PE. */
	atomic_int paused;    /* worker -> main: acknowledged, not inside doca_pe_progress. */
	atomic_bool running;  /* cleared by workers_stop to make the thread exit. */
	/* Debug/test completion deferral (--io-defer-count). Touched only by whichever thread is
	 * currently progressing this worker - its own thread, or the main thread while it is paused -
	 * so no synchronization is needed. */
	struct memos_initiator_ref_app_deferred_req defer[MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER];
	uint32_t defer_count;
	struct timespec defer_since; /* When the oldest request of the current batch was held. */
};

/**
 * @brief Work out how many workers and IO queue pairs this run will use, from the core mask and
 * the per-core queue count, clamped to the hard cap.
 *
 * Separate from memos_initiator_ref_app_workers_start() because the device has to be told its queue-pair count
 * while it is still idle, which is well before any worker exists.
 *
 * @param app  application state. Requires cfg.core_mask and cfg.num_io_queues_per_core.
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if the core mask is empty.
 */
doca_error_t memos_initiator_ref_app_plan_io_queues(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Create + start one worker (PE, io ctx, pinned thread) per set bit in cfg.core_mask.
 *
 * Must be called after app->nvme_dev is RUNNING (io ctx creation requires it), before the function
 * is powered on, and after memos_initiator_ref_app_plan_io_queues().
 *
 * @param app  application state. Requires nvme_dev and the counts from memos_initiator_ref_app_plan_io_queues().
 * @return DOCA_SUCCESS on success, an error otherwise (partial state is cleaned up).
 */
doca_error_t memos_initiator_ref_app_workers_start(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Join every worker thread, then stop and destroy its io ctx and PE.
 *
 * Must run after all IO queues have been unbound from the worker io ctxs (memos_initiator_ref_app_io_teardown)
 * and before app->nvme_dev is stopped. No-op if workers were never started.
 *
 * @param app  application state.
 */
void memos_initiator_ref_app_workers_stop(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Select the worker that owns a given IO queue id (round-robin: (qid-1) % num_workers).
 *
 * @param app  application state with started workers.
 * @param qid  IO queue id (>= 1).
 * @return the owning worker, or NULL if no workers are configured.
 */
struct memos_initiator_ref_app_worker *memos_initiator_ref_app_worker_for_qid(
	struct memos_initiator_ref_app_resources *app,
	uint16_t qid);

/**
 * @brief Quiesce a worker so the main thread can safely touch its io ctx / queues.
 *
 * Requests the worker thread to leave its busy-poll loop and spins until it acknowledges. While
 * paused, the main thread is the sole accessor of that worker's PE, io ctx and bound queues, so it
 * may bind/enable/disable queues and pump the worker PE for async drains. No-op if the worker has
 * no running thread.
 *
 * @param w  the worker to pause.
 */
void memos_initiator_ref_app_worker_pause(struct memos_initiator_ref_app_worker *w);

/**
 * @brief Release a worker paused by memos_initiator_ref_app_worker_pause(), resuming its busy-poll loop.
 *
 * @param w  the worker to resume.
 */
void memos_initiator_ref_app_worker_resume(struct memos_initiator_ref_app_worker *w);

#endif /* MEMOS_INITIATOR_REF_APP_WORKER_H_ */
