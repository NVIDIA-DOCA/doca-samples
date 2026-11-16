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
#define _GNU_SOURCE /* pthread_setaffinity_np, CPU_SET */
#endif

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_admin.h"
#include "memos_initiator_ref_app_dma.h"
#include "memos_initiator_ref_app_io.h"
#include "memos_initiator_ref_app_worker.h"

#include <inttypes.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <doca_ctx.h>
#include <doca_devemu_nvme_io.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_WORKER);

/* Idle backoff: when the controller is not enabled the worker has no IO to drain, so it sleeps
 * instead of pegging its core. Once enabled it busy-polls. */
#define MEMOS_INITIATOR_REF_APP_WORKER_IDLE_SLEEP_NS (10 * 1000) /* 10 us */
/* Bounded wait for a worker io ctx to reach a target state during bring-up / teardown. */
#define MEMOS_INITIATOR_REF_APP_WORKER_CTX_TIMEOUT_US (5 * 1000 * 1000) /* 5 s */

static size_t worker_elapsed_us(const struct timespec *start)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (size_t)((now.tv_sec - start->tv_sec) * 1000000LL + (now.tv_nsec - start->tv_nsec) / 1000LL);
}

/* Drive the worker io PE (and the control PE for any cross-PE dependency) until the io ctx reaches
 * `target`. Runs on the main thread while the worker's own thread is not yet started or already
 * joined, so the main thread is the sole progressor of io_pe here. */
static doca_error_t worker_ctx_wait_state(struct memos_initiator_ref_app_worker *w, enum doca_ctx_states target)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_WORKER_IDLE_SLEEP_NS};
	struct doca_ctx *ctx = doca_devemu_nvme_io_as_ctx(w->io);
	struct timespec start;
	enum doca_ctx_states state;
	doca_error_t result;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		result = doca_ctx_get_state(ctx, &state);
		if (result != DOCA_SUCCESS)
			return result;
		if (state == target)
			return DOCA_SUCCESS;
		if (worker_elapsed_us(&start) >= MEMOS_INITIATOR_REF_APP_WORKER_CTX_TIMEOUT_US) {
			DOCA_LOG_ERR("Worker (core %d): io ctx did not reach state %d (state=%d)",
				     w->core,
				     (int)target,
				     (int)state);
			return DOCA_ERROR_TIME_OUT;
		}
		int worked = doca_pe_progress(w->io_pe);

		if (w->app->pe != NULL)
			worked += doca_pe_progress(w->app->pe);
		if (worked == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

static void *worker_thread_fn(void *arg)
{
	struct memos_initiator_ref_app_worker *w = arg;
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_WORKER_IDLE_SLEEP_NS};

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		/* Quiesce handshake: when the main thread requests a pause, acknowledge and spin out of
		 * the PE until released, so it is the sole accessor of this io ctx / PE meanwhile. */
		if (atomic_load_explicit(&w->pause_req, memory_order_acquire)) {
			/* Settle any deferred completions before handing the io ctx to the main thread: a held
			 * request keeps the library's per-SQ outstanding count non-zero, which would stall the
			 * queue-disable flush the main thread is about to run. Drain rather than just flush, so
			 * the CQEs reach host memory before the queues are disabled underneath them. */
			memos_initiator_ref_app_io_defer_drain(w);
			atomic_store_explicit(&w->paused, 1, memory_order_release);
			while (atomic_load_explicit(&w->pause_req, memory_order_acquire) &&
			       atomic_load_explicit(&w->running, memory_order_relaxed))
				sched_yield();
			atomic_store_explicit(&w->paused, 0, memory_order_release);
			continue;
		}

		bool pe_idle = true;

		if (__atomic_load_n(&w->app->admin_up, __ATOMIC_RELAXED)) {
			int n = doca_pe_progress(w->io_pe);

			/* Datapath debug: only fires when the worker actually drains an IO event, so it
			 * tells us whether the IO SQ doorbell is reaching this worker at all. */
			if (n > 0)
				DOCA_LOG_DBG("Worker (core %d): io_pe progressed %d event(s)", w->core, n);
			pe_idle = (n == 0);
		} else {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}

		/* Service the deferral list (no-op unless --io-defer-count). Outside the admin_up check on
		 * purpose: a controller disable must not strand requests that are already held, and a host
		 * waiting on one blocks its own teardown. */
		memos_initiator_ref_app_io_defer_progress(w, pe_idle);
	}

	/* The thread owns the deferral list, so nothing else can release it after we return. */
	memos_initiator_ref_app_io_defer_drain(w);
	return NULL;
}

void memos_initiator_ref_app_worker_pause(struct memos_initiator_ref_app_worker *w)
{
	if (w == NULL || !w->thread_started)
		return;

	atomic_store_explicit(&w->pause_req, 1, memory_order_release);
	while (!atomic_load_explicit(&w->paused, memory_order_acquire) &&
	       atomic_load_explicit(&w->running, memory_order_relaxed))
		sched_yield();
}

void memos_initiator_ref_app_worker_resume(struct memos_initiator_ref_app_worker *w)
{
	if (w == NULL || !w->thread_started)
		return;

	atomic_store_explicit(&w->pause_req, 0, memory_order_release);
}

struct memos_initiator_ref_app_worker *memos_initiator_ref_app_worker_for_qid(
	struct memos_initiator_ref_app_resources *app,
	uint16_t qid)
{
	if (app->workers == NULL || app->num_workers == 0 || qid == 0)
		return NULL;
	return &app->workers[(uint32_t)(qid - 1) % app->num_workers];
}

/* Create the io ctx + PE for one worker and start it to RUNNING (still on the main thread). */
static doca_error_t worker_bring_up_ctx(struct memos_initiator_ref_app_resources *app,
					struct memos_initiator_ref_app_worker *w)
{
	struct doca_ctx *ctx;
	doca_error_t result;

	result = doca_pe_create(&w->io_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Worker (core %d): failed to create io PE: %s", w->core, doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_nvme_io_create(&w->io);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Worker (core %d): failed to create io ctx: %s", w->core, doca_error_get_descr(result));
		w->io = NULL;
		return result;
	}

	result = memos_initiator_ref_app_io_register_dispatch(app, w->io);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Worker (core %d): failed to register dispatcher: %s",
			     w->core,
			     doca_error_get_descr(result));
		return result;
	}

	ctx = doca_devemu_nvme_io_as_ctx(w->io);
	result = doca_pe_connect_ctx(w->io_pe, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Worker (core %d): failed to connect io ctx to PE: %s",
			     w->core,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Worker (core %d): failed to start io ctx: %s", w->core, doca_error_get_descr(result));
		return result;
	}

	result = worker_ctx_wait_state(w, DOCA_CTX_STATE_RUNNING);
	if (result != DOCA_SUCCESS)
		return result;

	/* The KV datapath moves value bytes through this engine. It rides io_pe, so it is created here
	 * - before the poll thread launches - and destroyed in workers_stop() after that thread is
	 * joined, keeping every submit and drain on one thread. */
	result = memos_initiator_ref_app_dma_engine_create(app,
							   w->io_pe,
							   MEMOS_INITIATOR_REF_APP_DMA_IO_POOL_SIZE,
							   &w->dma);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Worker (core %d): failed to create DMA engine: %s",
			     w->core,
			     doca_error_get_descr(result));

	return result;
}

/* Launch the pinned poll thread for an already-RUNNING worker io ctx. */
static doca_error_t worker_launch_thread(struct memos_initiator_ref_app_worker *w)
{
	cpu_set_t cpuset;
	int rc;

	atomic_store_explicit(&w->running, true, memory_order_relaxed);
	atomic_store_explicit(&w->pause_req, 0, memory_order_relaxed);
	atomic_store_explicit(&w->paused, 0, memory_order_relaxed);

	rc = pthread_create(&w->tid, NULL, worker_thread_fn, w);
	if (rc != 0) {
		DOCA_LOG_ERR("Worker (core %d): failed to create thread: %d", w->core, rc);
		atomic_store_explicit(&w->running, false, memory_order_relaxed);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	w->thread_started = true;

	CPU_ZERO(&cpuset);
	CPU_SET(w->core, &cpuset);
	rc = pthread_setaffinity_np(w->tid, sizeof(cpuset), &cpuset);
	if (rc != 0)
		DOCA_LOG_WARN("Worker (core %d): failed to set CPU affinity: %d (continuing unpinned)", w->core, rc);

	return DOCA_SUCCESS;
}

doca_error_t memos_initiator_ref_app_plan_io_queues(struct memos_initiator_ref_app_resources *app)
{
	uint32_t num_workers = (uint32_t)__builtin_popcountll(app->cfg.core_mask);
	uint64_t total;

	if (num_workers == 0) {
		DOCA_LOG_ERR("core-mask is empty");
		return DOCA_ERROR_INVALID_VALUE;
	}

	total = (uint64_t)num_workers * app->cfg.num_io_queues_per_core;
	if (total > MEMOS_INITIATOR_REF_APP_MAX_IO_QUEUES_HARD) {
		DOCA_LOG_WARN("Requested %" PRIu64 " IO queues (%u cores x %u/core) exceeds hard cap %u; clamping",
			      total,
			      num_workers,
			      app->cfg.num_io_queues_per_core,
			      MEMOS_INITIATOR_REF_APP_MAX_IO_QUEUES_HARD);
		total = MEMOS_INITIATOR_REF_APP_MAX_IO_QUEUES_HARD;
	}

	app->num_workers = num_workers;
	app->max_io_queues = (uint32_t)total;

	return DOCA_SUCCESS;
}

doca_error_t memos_initiator_ref_app_workers_start(struct memos_initiator_ref_app_resources *app)
{
	const long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const uint32_t num_workers = app->num_workers;
	uint32_t idx;
	doca_error_t result;

	app->workers = calloc(num_workers, sizeof(*app->workers));
	if (app->workers == NULL) {
		DOCA_LOG_ERR("Failed to allocate %u IO workers", num_workers);
		return DOCA_ERROR_NO_MEMORY;
	}

	idx = 0;
	for (int cpu = 0; cpu < 64; ++cpu) {
		if ((app->cfg.core_mask & (1ULL << cpu)) == 0)
			continue;
		if (ncpus > 0 && cpu >= ncpus) {
			DOCA_LOG_ERR("core-mask bit %d exceeds available CPUs (%ld)", cpu, ncpus);
			result = DOCA_ERROR_INVALID_VALUE;
			goto fail;
		}

		app->workers[idx].app = app;
		app->workers[idx].core = cpu;

		result = worker_bring_up_ctx(app, &app->workers[idx]);
		if (result != DOCA_SUCCESS)
			goto fail;
		result = worker_launch_thread(&app->workers[idx]);
		if (result != DOCA_SUCCESS)
			goto fail;
		++idx;
	}

	DOCA_LOG_INFO("IO workers up: %u core(s), mask 0x%" PRIx64 ", %u queue(s)/core -> %u IO queue(s) advertised",
		      num_workers,
		      app->cfg.core_mask,
		      app->cfg.num_io_queues_per_core,
		      app->max_io_queues);
	if (app->cfg.io_defer_count > 0)
		DOCA_LOG_WARN(
			"IO completion deferral active (debug mode): batches of %u released %s, keeping %u oldest held, max hold %u us",
			app->cfg.io_defer_count,
			app->cfg.io_defer_reverse ? "newest-first (out of order)" : "oldest-first (in order)",
			app->cfg.io_defer_keep,
			app->cfg.io_defer_max_us);
	return DOCA_SUCCESS;

fail:
	memos_initiator_ref_app_workers_stop(app);
	return result;
}

void memos_initiator_ref_app_workers_stop(struct memos_initiator_ref_app_resources *app)
{
	uint32_t i;
	doca_error_t result;

	if (app->workers == NULL)
		return;

	for (i = 0; i < app->num_workers; ++i) {
		struct memos_initiator_ref_app_worker *w = &app->workers[i];

		/* Stop the poll thread first so the main thread becomes the sole progressor of io_pe. */
		if (w->thread_started) {
			atomic_store_explicit(&w->running, false, memory_order_relaxed);
			(void)pthread_join(w->tid, NULL);
			w->thread_started = false;
		}

		/* Before the io ctx: the engine's completions are drained on io_pe, which this thread now
		 * owns exclusively, and tearing it down releases any request still mid-transfer. */
		/* A failure leaves the engine allocated for its pending tasks; the worker drops it either way. */
		(void)memos_initiator_ref_app_dma_engine_destroy(app, w->dma);
		w->dma = NULL;

		if (w->io != NULL) {
			struct doca_ctx *ctx = doca_devemu_nvme_io_as_ctx(w->io);

			result = doca_ctx_stop(ctx);
			if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS &&
			    result != DOCA_ERROR_BAD_STATE)
				DOCA_LOG_ERR("Worker (core %d): failed to stop io ctx: %s",
					     w->core,
					     doca_error_get_descr(result));
			(void)worker_ctx_wait_state(w, DOCA_CTX_STATE_IDLE);

			result = doca_devemu_nvme_io_destroy(w->io);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Worker (core %d): failed to destroy io ctx: %s",
					     w->core,
					     doca_error_get_descr(result));
			w->io = NULL;
		}

		if (w->io_pe != NULL) {
			result = doca_pe_destroy(w->io_pe);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Worker (core %d): failed to destroy io PE: %s",
					     w->core,
					     doca_error_get_descr(result));
			w->io_pe = NULL;
		}
	}

	free(app->workers);
	app->workers = NULL;
	/* num_workers and max_io_queues describe the plan, not the live threads, and must survive a
	 * stop: the controller-enable and FLR-recovery paths call workers_start() again without
	 * re-planning, and it sizes its allocation from num_workers. Every reader of the array
	 * already checks 'workers' for NULL. */
	DOCA_LOG_INFO("IO workers stopped");
}
