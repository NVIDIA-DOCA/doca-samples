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

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_dma.h"
#include "memos_initiator_ref_app_io.h"
#include "memos_initiator_ref_app_kv.h"
#include "memos_initiator_ref_app_nvme_spec.h"
#include "memos_initiator_ref_app_worker.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_devemu_nvme_cq.h>
#include <doca_devemu_nvme_dev.h>
#include <doca_devemu_nvme_io.h>
#include <doca_devemu_nvme_sq.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_IO);

/* Bounded wait for an asynchronous IO queue disable to settle (mirrors the admin teardown waits). */
#define MEMOS_INITIATOR_REF_APP_IO_DISABLE_TIMEOUT_US (1 * 1000 * 1000) /* 1 s */
#define MEMOS_INITIATOR_REF_APP_IO_DISABLE_SLEEP_NS (10 * 1000)		/* 10 us */
/* MEMOS_INITIATOR_REF_APP_IO_DEFER_MAX_US (the --io-defer-max-us default) lives in memos_initiator_ref_app_core.h. */

/* Bounds on memos_initiator_ref_app_io_defer_drain(): pump the worker PE until it reports no work this many times
 * in a row (the CQE DMA completion arrives a few ticks after the DMA is posted), giving up after the
 * iteration cap so a wedged DMA cannot hang teardown. */
#define MEMOS_INITIATOR_REF_APP_IO_DEFER_DRAIN_IDLE_TICKS 16u
#define MEMOS_INITIATOR_REF_APP_IO_DEFER_DRAIN_MAX_ITERS 1000000u
/* IO queues are never physically contiguous-required to the host, but we only support contiguous
 * queue memory; priority is ignored by this app. */

/* One qid slot: the CQ and/or SQ created with that qid. sq_cqid records which CQ an SQ posts to so
 * Delete-IO-CQ can refuse while a SQ still references it. cq_w/sq_w record the per-core worker each
 * queue was bound to (its io ctx), so delete can quiesce the right worker. */
struct memos_initiator_ref_app_ioq {
	struct doca_devemu_nvme_cq *cq;
	struct doca_devemu_nvme_sq *sq;
	struct memos_initiator_ref_app_worker *cq_w;
	struct memos_initiator_ref_app_worker *sq_w;
	uint16_t sq_cqid;
	uint16_t qid;
};

/* qid-indexed registry; slots[0] is the (unused) admin qid, slots[1..max_io_queues] are IO. Sized
 * at init from app->max_io_queues. */
struct memos_initiator_ref_app_ioq_registry {
	uint16_t num_slots;
	struct memos_initiator_ref_app_ioq slots[];
};

static uint16_t io_status(uint8_t sct, uint8_t sc)
{
	return NVME_STATUS(sct, sc);
}

/* Drive a worker's io PE (plus the control PE) once while it is paused, so an async IO queue disable
 * drains its CQE DMA. The worker thread is quiesced by the caller, so the main thread is the sole
 * progressor of w->io_pe here. */
static void io_pe_idle_sleep(struct memos_initiator_ref_app_resources *app, struct memos_initiator_ref_app_worker *w)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_IO_DISABLE_SLEEP_NS};
	int worked = 0;

	/* The worker is paused, so the main thread is the sole progressor of io_pe here - and of the KV
	 * worker entry a dispatched command lands on, since a queue always resolves to its own worker. */
	if (w != NULL && w->io_pe != NULL)
		worked += doca_pe_progress(w->io_pe);
	worked += doca_pe_progress(app->pe);

	/* This pump can dispatch new SQEs, and with --io-defer-count the handler parks their completions
	 * on the worker. The worker thread is paused and already flushed, so nothing else would release
	 * them: complete them here or they stay held while the queue is torn down around them. Drain
	 * rather than flush, because releasing a completion only queues its CQE for DMA - the caller
	 * polling this pump can see the queue disabled and tear it down before another tick pushes that
	 * DMA out, and the host would then wait for a CQE that no longer has a queue to arrive on. */
	if (w != NULL)
		memos_initiator_ref_app_io_defer_drain(w);

	if (worked == 0) {
		struct timespec ts = sleep_ts;

		(void)nanosleep(&ts, NULL);
	}
}

static bool io_disable_deadline_passed(const struct timespec *start)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (size_t)((now.tv_sec - start->tv_sec) * 1000000LL + (now.tv_nsec - start->tv_nsec) / 1000LL) >=
	       MEMOS_INITIATOR_REF_APP_IO_DISABLE_TIMEOUT_US;
}

/* Disable a CQ and, if the disable is asynchronous, drive the worker's PE until it reaches DISABLED.
 * Caller must have paused the worker so the main thread is the sole progressor of w->io_pe. */
static void io_cq_disable_and_wait(struct memos_initiator_ref_app_resources *app,
				   struct memos_initiator_ref_app_worker *w,
				   struct doca_devemu_nvme_cq *cq)
{
	struct timespec start;
	enum doca_devemu_nvme_cq_states state;
	doca_error_t result;

	result = doca_devemu_nvme_cq_get_state(cq, &state);
	if (result != DOCA_SUCCESS || state != DOCA_DEVEMU_NVME_CQ_STATE_ENABLED)
		return;

	result = doca_devemu_nvme_cq_disable(cq);
	if (result == DOCA_SUCCESS)
		return;
	if (result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("IO teardown: failed to disable CQ: %s", doca_error_get_descr(result));
		return;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (doca_devemu_nvme_cq_get_state(cq, &state) != DOCA_SUCCESS ||
		    state == DOCA_DEVEMU_NVME_CQ_STATE_DISABLED)
			return;
		if (io_disable_deadline_passed(&start)) {
			DOCA_LOG_ERR("IO teardown: timed out waiting for CQ to disable");
			return;
		}
		io_pe_idle_sleep(app, w);
	}
}

/* SQ counterpart of io_cq_disable_and_wait. */
static void io_sq_disable_and_wait(struct memos_initiator_ref_app_resources *app,
				   struct memos_initiator_ref_app_worker *w,
				   struct doca_devemu_nvme_sq *sq)
{
	struct timespec start;
	enum doca_devemu_nvme_sq_states state;
	doca_error_t result;

	result = doca_devemu_nvme_sq_get_state(sq, &state);
	if (result != DOCA_SUCCESS || state != DOCA_DEVEMU_NVME_SQ_STATE_ENABLED)
		return;

	result = doca_devemu_nvme_sq_disable(sq);
	if (result == DOCA_SUCCESS)
		return;
	if (result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("IO teardown: failed to disable SQ: %s", doca_error_get_descr(result));
		return;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (doca_devemu_nvme_sq_get_state(sq, &state) != DOCA_SUCCESS ||
		    state == DOCA_DEVEMU_NVME_SQ_STATE_DISABLED)
			return;
		if (io_disable_deadline_passed(&start)) {
			DOCA_LOG_ERR("IO teardown: timed out waiting for SQ to disable");
			return;
		}
		io_pe_idle_sleep(app, w);
	}
}

/* An IO queue id the app can serve: in range, and with a registry to record it in. The registry is
 * built during admin bring-up and released when the controller is disabled, so a create that
 * arrives outside that window has nowhere to go and is refused rather than dereferenced. */
static bool io_qid_valid(struct memos_initiator_ref_app_resources *app, uint16_t qid)
{
	return app->ioq != NULL && qid >= 1 && qid <= app->max_io_queues;
}

doca_error_t memos_initiator_ref_app_io_init(struct memos_initiator_ref_app_resources *app)
{
	uint16_t num_slots;

	if (app->ioq != NULL)
		return DOCA_SUCCESS;

	/* slots[0] is the admin qid placeholder; IO qids are 1..max_io_queues. */
	num_slots = (uint16_t)(app->max_io_queues + 1);
	app->ioq = calloc(1, sizeof(*app->ioq) + (size_t)num_slots * sizeof(struct memos_initiator_ref_app_ioq));
	if (app->ioq == NULL) {
		DOCA_LOG_ERR("Failed to allocate IO queue registry");
		return DOCA_ERROR_NO_MEMORY;
	}
	app->ioq->num_slots = num_slots;
	return DOCA_SUCCESS;
}

uint16_t memos_initiator_ref_app_io_create_cq(struct memos_initiator_ref_app_resources *app,
					      uint16_t qid,
					      uint16_t qsize,
					      uint16_t iv,
					      uint8_t ien,
					      uint8_t pc,
					      uint64_t prp1)
{
	struct memos_initiator_ref_app_ioq *slot;
	struct memos_initiator_ref_app_worker *w;
	struct doca_devemu_nvme_cq_create_attr *cq_attr = NULL;
	struct doca_devemu_nvme_cq *cq;
	doca_error_t result;

	if (!io_qid_valid(app, qid) || app->ioq->slots[qid].cq != NULL) {
		DOCA_LOG_WARN("Create-IO-CQ: invalid/duplicate qid=%u -> INVALID_QID", qid);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QID);
	}
	if (qsize < 2 || qsize > app->max_io_queue_size) {
		DOCA_LOG_WARN("Create-IO-CQ: qid=%u qsize=%u out of range (max %u) -> INVALID_QSIZE",
			      qid,
			      qsize,
			      app->max_io_queue_size);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QSIZE);
	}
	w = memos_initiator_ref_app_worker_for_qid(app, qid);
	if (w == NULL) {
		DOCA_LOG_ERR("Create-IO-CQ qid=%u: no IO worker configured", qid);
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}
	slot = &app->ioq->slots[qid];

	/* Create/configure/start run on the main thread against the dev PE (they touch nvme_dev). */
	result = doca_devemu_nvme_cq_create_attr_create(&cq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-CQ qid=%u: attr create failed: %s", qid, doca_error_get_descr(result));
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	(void)doca_devemu_nvme_cq_create_attr_set_qid(cq_attr, qid);
	(void)doca_devemu_nvme_cq_create_attr_set_qsize(cq_attr, qsize);
	(void)doca_devemu_nvme_cq_create_attr_set_interrupt_vector(cq_attr, iv);
	(void)doca_devemu_nvme_cq_create_attr_set_interrupt_enable(cq_attr, ien);
	(void)doca_devemu_nvme_cq_create_attr_set_physically_contiguous(cq_attr, pc);
	(void)doca_devemu_nvme_cq_create_attr_set_prp1(cq_attr, prp1);

	result = doca_devemu_nvme_cq_create_from_dev(cq_attr, app->nvme_dev, DOCA_DEVEMU_NVME_CQ_CREATE_FLAG_NONE, &cq);
	(void)doca_devemu_nvme_cq_create_attr_destroy(cq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-CQ qid=%u: create failed: %s", qid, doca_error_get_descr(result));
		/* The library refuses what it cannot address - a non-contiguous queue, for one - and
		 * that is the host asking for an unsupported CDW11, not a device failure. */
		if (result == DOCA_ERROR_NOT_SUPPORTED)
			return io_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD);
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	/* Bind + enable touch the worker's io ctx / datapath worker, so quiesce that worker first. */
	memos_initiator_ref_app_worker_pause(w);
	result = doca_devemu_nvme_cq_bind(cq, w->io);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-CQ qid=%u: bind failed: %s", qid, doca_error_get_descr(result));
		memos_initiator_ref_app_worker_resume(w);
		goto stop;
	}
	result = doca_devemu_nvme_cq_enable(cq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-CQ qid=%u: enable failed: %s", qid, doca_error_get_descr(result));
		(void)doca_devemu_nvme_cq_unbind(cq);
		memos_initiator_ref_app_worker_resume(w);
		goto stop;
	}
	memos_initiator_ref_app_worker_resume(w);

	slot->cq = cq;
	slot->cq_w = w;
	slot->qid = qid;
	DOCA_LOG_INFO("IO CQ up: qid=%u depth=%u iv=%u ien=%u base=0x%016" PRIx64 " core=%d",
		      qid,
		      qsize,
		      iv,
		      ien,
		      prp1,
		      w->core);
	return NVME_SC_SUCCESS;

stop:
	(void)doca_devemu_nvme_cq_destroy(cq, DOCA_DEVEMU_NVME_CQ_DESTROY_FLAG_NONE);
	return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
}

uint16_t memos_initiator_ref_app_io_create_sq(struct memos_initiator_ref_app_resources *app,
					      uint16_t qid,
					      uint16_t qsize,
					      uint16_t cqid,
					      uint8_t qprio,
					      uint8_t pc,
					      uint64_t prp1)
{
	struct memos_initiator_ref_app_ioq *slot;
	struct memos_initiator_ref_app_worker *w;
	struct doca_devemu_nvme_cq *cq;
	struct doca_devemu_nvme_sq_create_attr *sq_attr = NULL;
	struct doca_devemu_nvme_sq *sq;
	doca_error_t result;

	(void)qprio; /* priority is ignored by this app */

	if (!io_qid_valid(app, qid) || app->ioq->slots[qid].sq != NULL) {
		DOCA_LOG_WARN("Create-IO-SQ: invalid/duplicate qid=%u -> INVALID_QID", qid);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QID);
	}
	if (qsize < 2 || qsize > app->max_io_queue_size) {
		DOCA_LOG_WARN("Create-IO-SQ: qid=%u qsize=%u out of range (max %u) -> INVALID_QSIZE",
			      qid,
			      qsize,
			      app->max_io_queue_size);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QSIZE);
	}
	if (!io_qid_valid(app, cqid) || app->ioq->slots[cqid].cq == NULL) {
		DOCA_LOG_WARN("Create-IO-SQ: qid=%u references missing cqid=%u -> CQ_INVALID", qid, cqid);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_CQ_INVALID);
	}
	slot = &app->ioq->slots[qid];
	cq = app->ioq->slots[cqid].cq;
	/* An SQ's CQ must be bound to the same io ctx, so this SQ rides the CQ's worker (not its own
	 * qid's), which satisfies doca_devemu_nvme_sq_bind()'s "CQ bound to an io ctx" check. */
	w = app->ioq->slots[cqid].cq_w;
	if (w == NULL) {
		DOCA_LOG_ERR("Create-IO-SQ qid=%u: cqid=%u has no worker", qid, cqid);
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	result = doca_devemu_nvme_sq_create_attr_create(&sq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-SQ qid=%u: attr create failed: %s", qid, doca_error_get_descr(result));
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	(void)doca_devemu_nvme_sq_create_attr_set_qid(sq_attr, qid);
	(void)doca_devemu_nvme_sq_create_attr_set_qsize(sq_attr, qsize);
	(void)doca_devemu_nvme_sq_create_attr_set_cq(sq_attr, cq);
	(void)doca_devemu_nvme_sq_create_attr_set_qprio(sq_attr, qprio);
	(void)doca_devemu_nvme_sq_create_attr_set_physically_contiguous(sq_attr, pc);
	(void)doca_devemu_nvme_sq_create_attr_set_prp1(sq_attr, prp1);

	result = doca_devemu_nvme_sq_create(sq_attr, DOCA_DEVEMU_NVME_SQ_CREATE_FLAG_NONE, &sq);
	(void)doca_devemu_nvme_sq_create_attr_destroy(sq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-SQ qid=%u: create failed: %s", qid, doca_error_get_descr(result));
		/* The library refuses what it cannot address - a non-contiguous queue, for one - and
		 * that is the host asking for an unsupported CDW11, not a device failure. */
		if (result == DOCA_ERROR_NOT_SUPPORTED)
			return io_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD);
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	/* Bind with the slot as user data so the dispatcher routes this SQ's requests to the IO path;
	 * bind + enable touch the worker's io ctx, so quiesce it first. */
	memos_initiator_ref_app_worker_pause(w);
	result = doca_devemu_nvme_sq_bind(sq, slot);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-SQ qid=%u: bind failed: %s", qid, doca_error_get_descr(result));
		memos_initiator_ref_app_worker_resume(w);
		goto stop;
	}
	result = doca_devemu_nvme_sq_enable(sq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Create-IO-SQ qid=%u: enable failed: %s", qid, doca_error_get_descr(result));
		(void)doca_devemu_nvme_sq_unbind(sq);
		memos_initiator_ref_app_worker_resume(w);
		goto stop;
	}
	memos_initiator_ref_app_worker_resume(w);

	slot->sq = sq;
	slot->sq_w = w;
	slot->sq_cqid = cqid;
	slot->qid = qid;
	DOCA_LOG_INFO("IO SQ up: qid=%u depth=%u cqid=%u base=0x%016" PRIx64 " core=%d",
		      qid,
		      qsize,
		      cqid,
		      prp1,
		      w->core);
	return NVME_SC_SUCCESS;

stop:
	(void)doca_devemu_nvme_sq_destroy(sq, DOCA_DEVEMU_NVME_SQ_DESTROY_FLAG_NONE);
	return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
}

uint16_t memos_initiator_ref_app_io_delete_sq(struct memos_initiator_ref_app_resources *app, uint16_t qid)
{
	struct memos_initiator_ref_app_ioq *slot;
	struct memos_initiator_ref_app_worker *w;
	doca_error_t result;

	if (!io_qid_valid(app, qid) || app->ioq->slots[qid].sq == NULL) {
		DOCA_LOG_WARN("Delete-IO-SQ: invalid qid=%u -> INVALID_QID", qid);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QID);
	}
	slot = &app->ioq->slots[qid];
	w = slot->sq_w;

	/* disable + flush + unbind touch the worker's io ctx / datapath, so quiesce it. */
	memos_initiator_ref_app_worker_pause(w);
	io_sq_disable_and_wait(app, w, slot->sq);
	(void)doca_devemu_nvme_sq_flush(slot->sq);
	result = doca_devemu_nvme_sq_unbind(slot->sq);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
		DOCA_LOG_ERR("Delete-IO-SQ qid=%u: unbind failed: %s", qid, doca_error_get_descr(result));
	memos_initiator_ref_app_worker_resume(w);

	/* Keep the slot if the library still owns the queue: clearing it would lose the only handle,
	 * and telling the host the queue is gone while it is not invites a Create on the same qid. */
	result = doca_devemu_nvme_sq_destroy(slot->sq, DOCA_DEVEMU_NVME_SQ_DESTROY_FLAG_NONE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Delete-IO-SQ qid=%u: destroy failed: %s", qid, doca_error_get_descr(result));
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	slot->sq = NULL;
	slot->sq_w = NULL;
	slot->sq_cqid = 0;
	DOCA_LOG_INFO("IO SQ deleted: qid=%u", qid);
	return NVME_SC_SUCCESS;
}

uint16_t memos_initiator_ref_app_io_delete_cq(struct memos_initiator_ref_app_resources *app, uint16_t qid)
{
	struct memos_initiator_ref_app_ioq *slot;
	struct memos_initiator_ref_app_worker *w;
	doca_error_t result;
	uint16_t i;

	if (!io_qid_valid(app, qid) || app->ioq->slots[qid].cq == NULL) {
		DOCA_LOG_WARN("Delete-IO-CQ: invalid qid=%u -> INVALID_QID", qid);
		return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QID);
	}
	/* A CQ may not be deleted while any SQ still posts completions to it. */
	for (i = 1; i <= app->max_io_queues; ++i) {
		if (app->ioq->slots[i].sq != NULL && app->ioq->slots[i].sq_cqid == qid) {
			DOCA_LOG_WARN("Delete-IO-CQ qid=%u: SQ qid=%u still bound -> INVALID_QUEUE_DELETION", qid, i);
			return io_status(NVME_SCT_CMD_SPECIFIC, NVME_SC_INVALID_QUEUE_DELETION);
		}
	}
	slot = &app->ioq->slots[qid];
	w = slot->cq_w;

	memos_initiator_ref_app_worker_pause(w);
	io_cq_disable_and_wait(app, w, slot->cq);
	result = doca_devemu_nvme_cq_unbind(slot->cq);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
		DOCA_LOG_ERR("Delete-IO-CQ qid=%u: unbind failed: %s", qid, doca_error_get_descr(result));
	memos_initiator_ref_app_worker_resume(w);

	result = doca_devemu_nvme_cq_destroy(slot->cq, DOCA_DEVEMU_NVME_CQ_DESTROY_FLAG_NONE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Delete-IO-CQ qid=%u: destroy failed: %s", qid, doca_error_get_descr(result));
		return io_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR);
	}

	slot->cq = NULL;
	slot->cq_w = NULL;
	DOCA_LOG_INFO("IO CQ deleted: qid=%u", qid);
	return NVME_SC_SUCCESS;
}

void memos_initiator_ref_app_io_teardown(struct memos_initiator_ref_app_resources *app)
{
	uint16_t qid;

	if (app->ioq == NULL)
		return;

	/* SQs first (a started SQ pins its CQ), then CQs. */
	for (qid = 1; qid <= app->max_io_queues; ++qid)
		if (app->ioq->slots[qid].sq != NULL)
			(void)memos_initiator_ref_app_io_delete_sq(app, qid);
	for (qid = 1; qid <= app->max_io_queues; ++qid)
		if (app->ioq->slots[qid].cq != NULL)
			(void)memos_initiator_ref_app_io_delete_cq(app, qid);

	/* A queue whose destroy failed keeps its slot, because the library still owns the object and
	 * the handle is the only way back to it. Freeing the registry would drop those handles, so it
	 * is kept - and leaked - rather than losing the queues it still describes. */
	for (qid = 1; qid <= app->max_io_queues; ++qid)
		if (app->ioq->slots[qid].sq != NULL || app->ioq->slots[qid].cq != NULL) {
			DOCA_LOG_ERR(
				"IO teardown incomplete: qid=%u is still owned by the library; keeping the queue registry",
				qid);
			return;
		}

	free(app->ioq);
	app->ioq = NULL;
	DOCA_LOG_INFO("IO queues torn down");
}

/*********************************************************************************************************************
 * Debug/test completion deferral (--io-defer-count / --io-defer-reverse)
 *********************************************************************************************************************/

static bool io_defer_deadline_passed(const struct timespec *start, uint32_t max_us)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (size_t)((now.tv_sec - start->tv_sec) * 1000000LL + (now.tv_nsec - start->tv_nsec) / 1000LL) >= max_us;
}

/*
 * Post the completions held at [from, defer_count) and truncate the list to 'from', so the oldest
 * 'from' requests stay held. Keeping the oldest held while newer commands complete is what opens the
 * window this option exists to exercise: the host frees a tag per completion and keeps submitting, so
 * its SQ tail wraps around and reuses the slot of a command the controller still owns.
 */
static void io_defer_release_from(struct memos_initiator_ref_app_worker *w, uint32_t from)
{
	const bool reverse = w->app->cfg.io_defer_reverse;
	const uint32_t last = w->defer_count;
	uint32_t remaining;
	uint32_t released = 0;

	if (from >= last)
		return;

	/* Take every entry out of the array before completing it, keeping the count in step. Lowering
	 * the count once and then walking the old range is not enough: req_complete may re-enter the
	 * handler through the worker PE, and the entry that handler parks lands on the first free slot -
	 * inside the range still being walked - so the walk would complete the newly parked request in
	 * place of an older one and leave that older one with no completion at all. Only the batch this
	 * call set out to release is released; anything parked while it runs waits for the next one. */
	for (remaining = last - from; remaining > 0 && w->defer_count > from; remaining--) {
		struct memos_initiator_ref_app_deferred_req entry;

		if (reverse) {
			entry = w->defer[--w->defer_count];
		} else {
			entry = w->defer[from];
			w->defer_count--;
			memmove(&w->defer[from], &w->defer[from + 1], (w->defer_count - from) * sizeof(w->defer[0]));
		}
		released++;
		doca_devemu_nvme_req_complete(entry.req, 0, 0, entry.status);
	}

	DOCA_LOG_DBG("Worker (core %d): released %u deferred completion(s) %s, %u still held",
		     w->core,
		     released,
		     reverse ? "newest-first" : "oldest-first",
		     w->defer_count);
}

void memos_initiator_ref_app_io_defer_flush(struct memos_initiator_ref_app_worker *w)
{
	if (w == NULL)
		return;

	io_defer_release_from(w, 0);
}

void memos_initiator_ref_app_io_defer_drain(struct memos_initiator_ref_app_worker *w)
{
	unsigned int consecutive_idle = 0;
	unsigned int iter;

	if (w == NULL || w->io_pe == NULL)
		return;

	/* Completing a request only writes its CQE into the library's CQ shadow and queues that CQ for
	 * DMA; the transfer to host memory needs PE progress. Disabling a CQ discards a DMA that is merely
	 * queued (the CQ is pulled off the worker's run list and drain blocks re-arming), so a completion
	 * released this late is lost unless it is pushed out before the caller tears the queue down - and
	 * a host still waiting on that command blocks its own teardown indefinitely.
	 *
	 * Progress can also dispatch a new SQE, whose completion the handler parks like any other, so
	 * releasing once up front would leave those behind: the batch is released on every tick, and a
	 * tick only counts as idle when it both moved nothing and left nothing held. */
	for (iter = 0; iter < MEMOS_INITIATOR_REF_APP_IO_DEFER_DRAIN_MAX_ITERS; iter++) {
		memos_initiator_ref_app_io_defer_flush(w);

		if (doca_pe_progress(w->io_pe) != 0 || w->defer_count != 0) {
			consecutive_idle = 0;
			continue;
		}
		if (++consecutive_idle >= MEMOS_INITIATOR_REF_APP_IO_DEFER_DRAIN_IDLE_TICKS)
			return;
	}

	DOCA_LOG_WARN("Worker (core %d): deferred completion drain did not settle in %u ticks",
		      w->core,
		      (unsigned int)MEMOS_INITIATOR_REF_APP_IO_DEFER_DRAIN_MAX_ITERS);
}

bool memos_initiator_ref_app_io_defer_hold(struct memos_initiator_ref_app_worker *w,
					   struct doca_devemu_nvme_req *req,
					   uint16_t status)
{
	uint32_t target;

	if (w == NULL)
		return false;

	target = w->app->cfg.io_defer_count;
	if (target == 0)
		return false;

	/* Defensive: the batch size is validated against the array bound at parse time, so a full array
	 * means something else has gone wrong. Flushing usually empties it, but it is not guaranteed to:
	 * a release completes requests, and completing one can re-enter this function and park another,
	 * so the count can come back as high as it went in. Decline the deferral in that case - the
	 * caller completes the request itself - rather than writing one past the end of the array. */
	if (w->defer_count >= MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER) {
		memos_initiator_ref_app_io_defer_flush(w);

		if (w->defer_count >= MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER) {
			DOCA_LOG_ERR(
				"Worker (core %d): %u deferred completions still held after a flush; completing this one now",
				w->core,
				w->defer_count);
			return false;
		}
	}

	if (w->defer_count == 0)
		(void)clock_gettime(CLOCK_MONOTONIC, &w->defer_since);

	w->defer[w->defer_count].req = req;
	w->defer[w->defer_count].status = status;
	w->defer_count++;

	/* The arrival of the target-th command releases the batch, minus the oldest io_defer_keep
	 * requests, which stay held until they age out in _defer_progress(). */
	if (w->defer_count >= target)
		io_defer_release_from(w, w->app->cfg.io_defer_keep);

	return true;
}

void memos_initiator_ref_app_io_defer_progress(struct memos_initiator_ref_app_worker *w, bool pe_idle)
{
	if (w == NULL || w->defer_count == 0)
		return;

	/* The worker has nothing left to fetch, so holding the batch back until it fills would only add
	 * latency - a host that keeps fewer than --io-defer-count commands in flight (a partition scan,
	 * say) would otherwise pay the age-out timeout on every single batch. Release it now, keeping the
	 * oldest --io-defer-keep requests, which are the ones this option exists to hold. */
	if (pe_idle)
		io_defer_release_from(w, w->app->cfg.io_defer_keep);

	/* Safety valve for whatever is still held - the kept requests, or a batch on a worker whose PE
	 * never goes idle: bound the hold so the host cannot wait on them indefinitely. */
	if (w->defer_count != 0 && io_defer_deadline_passed(&w->defer_since, w->app->cfg.io_defer_max_us))
		memos_initiator_ref_app_io_defer_flush(w);
}

/* Index of the worker driving this SQ - selects its KV I/O context, as its DMA engine is selected
 * below. Both belong to the thread already handling this queue. */
static uint32_t io_worker_idx(const struct memos_initiator_ref_app_resources *app,
			      const struct memos_initiator_ref_app_ioq *slot)
{
	if (slot == NULL || slot->sq_w == NULL || app->workers == NULL)
		return 0;
	return (uint32_t)(slot->sq_w - app->workers);
}

/* The DMA engine a command must use: the one owned by the worker driving its SQ, so the transfer is
 * submitted and drained on the thread already handling this queue. */
static struct memos_initiator_ref_app_dma *io_worker_dma(struct memos_initiator_ref_app_ioq *slot)
{
	return slot != NULL && slot->sq_w != NULL ? slot->sq_w->dma : NULL;
}

/* The largest value the namespace currently advertises, which is what commands are checked against. */
static uint32_t io_kv_max_value_len(const struct memos_initiator_ref_app_resources *app)
{
	uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN];
	uint32_t max_value_len;
	uint16_t max_key_len;
	uint64_t ns_size;

	if (memos_initiator_ref_app_kv_get_geometry(app, &max_key_len, &max_value_len, &ns_size, nguid) != DOCA_SUCCESS)
		return MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN;

	return max_value_len;
}

/* Common front half of every KV command: pull the key out of the SQE and range-check it. Returns
 * SUCCESS, or the status the command should be failed with. */
static uint16_t io_kv_parse_key(const struct memos_initiator_ref_app_resources *app,
				const void *sqe,
				uint8_t key[NVME_KV_MAX_KEY_LEN],
				uint8_t *key_len)
{
	uint16_t max_key_len = MEMOS_INITIATOR_REF_APP_KV_MAX_KEY_LEN;
	uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN];
	uint32_t max_value_len;
	uint64_t ns_size;

	/* Enforce the limits advertised in CNS 05h, which come from the target. */
	if (memos_initiator_ref_app_kv_get_geometry(app, &max_key_len, &max_value_len, &ns_size, nguid) != DOCA_SUCCESS)
		max_key_len = MEMOS_INITIATOR_REF_APP_KV_MAX_KEY_LEN;

	*key_len = nvme_kv_sqe_key_len(sqe);
	if (*key_len == 0 || *key_len > max_key_len)
		return NVME_SC_KV_INVALID_KEY_SIZE;

	nvme_kv_sqe_get_key(sqe, key);

	/* The SQE dwords above key_len are reserved; zero them so a key is identified by its
	 * significant bytes alone and stale dwords cannot make two identical keys compare unequal. */
	if (*key_len < NVME_KV_MAX_KEY_LEN)
		memset(key + *key_len, 0, NVME_KV_MAX_KEY_LEN - *key_len);

	return NVME_SC_SUCCESS;
}

static void io_kv_store(struct memos_initiator_ref_app_resources *app,
			struct memos_initiator_ref_app_ioq *slot,
			struct doca_devemu_nvme_req *req,
			const void *sqe,
			uint16_t qid,
			uint16_t cid)
{
	uint8_t key[NVME_KV_MAX_KEY_LEN];
	uint32_t value_len = nvme_kv_sqe_value_len(sqe);
	uint32_t opts = nvme_kv_sqe_store_opts(sqe);
	struct doca_buf *value_buf = NULL;
	uint32_t value_list_len = 0;
	uint8_t key_len;
	uint16_t status;

	status = io_kv_parse_key(app, sqe, key, &key_len);
	if (status != NVME_SC_SUCCESS) {
		DOCA_LOG_WARN("KV Store qid=%u cid=%u bad key length %u", qid, cid, nvme_kv_sqe_key_len(sqe));
		doca_devemu_nvme_req_complete(req, 0, 0, status);
		return;
	}

	if (value_len > io_kv_max_value_len(app)) {
		DOCA_LOG_WARN("KV Store qid=%u cid=%u value length %u exceeds MXVL %u",
			      qid,
			      cid,
			      value_len,
			      io_kv_max_value_len(app));
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_KV_INVALID_VALUE_SIZE);
		return;
	}

	/* A zero-length value is legal and carries no host buffer. */
	if (value_len != 0) {
		doca_error_t res = memos_initiator_ref_app_dma_host_buf_get(app,
									    io_worker_dma(slot),
									    sqe,
									    value_len,
									    /* host_is_src */ true,
									    &value_buf,
									    &value_list_len);

		if (res != DOCA_SUCCESS) {
			DOCA_LOG_WARN("KV Store qid=%u cid=%u cannot map host buffer: %s",
				      qid,
				      cid,
				      doca_error_get_name(res));
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_DATA_XFER_ERROR);
			return;
		}
	}

	status = memos_initiator_ref_app_kv_store(app,
						  io_worker_idx(app, slot),
						  key,
						  key_len,
						  value_buf,
						  value_list_len,
						  value_len,
						  opts);
	memos_initiator_ref_app_dma_host_buf_put(value_buf);

	DOCA_LOG_DBG("KV Store qid=%u cid=%u key_len=%u value_len=%u opts=0x%x -> status=0x%04x",
		     qid,
		     cid,
		     key_len,
		     value_len,
		     opts,
		     status);
	doca_devemu_nvme_req_complete(req, 0, 0, status);
}

static void io_kv_retrieve(struct memos_initiator_ref_app_resources *app,
			   struct memos_initiator_ref_app_ioq *slot,
			   struct doca_devemu_nvme_req *req,
			   const void *sqe,
			   uint16_t qid,
			   uint16_t cid)
{
	uint8_t key[NVME_KV_MAX_KEY_LEN];
	uint32_t host_buf_len = nvme_kv_sqe_value_len(sqe);
	struct doca_buf *value_buf = NULL;
	uint32_t value_list_len = 0;
	uint32_t value_len = 0;
	uint8_t key_len;
	uint16_t status;

	status = io_kv_parse_key(app, sqe, key, &key_len);
	if (status != NVME_SC_SUCCESS) {
		DOCA_LOG_WARN("KV Retrieve qid=%u cid=%u bad key length %u", qid, cid, nvme_kv_sqe_key_len(sqe));
		doca_devemu_nvme_req_complete(req, 0, 0, status);
		return;
	}

	/* A zero-capacity Retrieve would be the command set's size probe, but the initiator's retrieve
	 * task requires a non-zero value length, so it is rejected rather than issued as an invalid
	 * task. */
	if (host_buf_len == 0 || host_buf_len > io_kv_max_value_len(app)) {
		DOCA_LOG_WARN("KV Retrieve qid=%u cid=%u host buffer %u outside 1..%u",
			      qid,
			      cid,
			      host_buf_len,
			      io_kv_max_value_len(app));
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_KV_INVALID_VALUE_SIZE);
		return;
	}

	{
		doca_error_t res = memos_initiator_ref_app_dma_host_buf_get(app,
									    io_worker_dma(slot),
									    sqe,
									    host_buf_len,
									    /* host_is_src */ false,
									    &value_buf,
									    &value_list_len);

		if (res != DOCA_SUCCESS) {
			DOCA_LOG_WARN("KV Retrieve qid=%u cid=%u cannot map host buffer: %s",
				      qid,
				      cid,
				      doca_error_get_name(res));
			doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_DATA_XFER_ERROR);
			return;
		}
	}

	status = memos_initiator_ref_app_kv_retrieve(app,
						     io_worker_idx(app, slot),
						     key,
						     key_len,
						     value_buf,
						     value_list_len,
						     host_buf_len,
						     &value_len);
	memos_initiator_ref_app_dma_host_buf_put(value_buf);

	DOCA_LOG_DBG("KV Retrieve qid=%u cid=%u key_len=%u host_buf=%u -> value_len=%u status=0x%04x",
		     qid,
		     cid,
		     key_len,
		     host_buf_len,
		     value_len,
		     status);

	/* DW0 carries the stored length even when it exceeded the host's buffer. */
	doca_devemu_nvme_req_complete(req, status == NVME_SC_SUCCESS ? value_len : 0, 0, status);
}

static void io_kv_exist(struct memos_initiator_ref_app_resources *app,
			struct memos_initiator_ref_app_ioq *slot,
			struct doca_devemu_nvme_req *req,
			const void *sqe,
			uint16_t qid,
			uint16_t cid)
{
	uint8_t key[NVME_KV_MAX_KEY_LEN];
	uint8_t key_len;
	uint16_t status;

	status = io_kv_parse_key(app, sqe, key, &key_len);
	if (status == NVME_SC_SUCCESS)
		status = memos_initiator_ref_app_kv_exist(app, io_worker_idx(app, slot), key, key_len);

	DOCA_LOG_DBG("KV Exist qid=%u cid=%u key_len=%u -> status=0x%04x", qid, cid, key_len, status);
	doca_devemu_nvme_req_complete(req, 0, 0, status);
}

void memos_initiator_ref_app_io_handle(struct doca_devemu_nvme_req *req,
				       const void *sqe,
				       void *sq_user_data,
				       void *req_user_data)
{
	struct memos_initiator_ref_app_ioq *slot = (struct memos_initiator_ref_app_ioq *)sq_user_data;
	uint16_t qid = slot != NULL ? slot->qid : 0;
	uint8_t opcode = nvme_sqe_opcode(sqe);
	uint16_t cid = nvme_sqe_cid(sqe);
	uint32_t nsid = nvme_sqe_nsid(sqe);
	struct memos_initiator_ref_app_resources *app = slot != NULL && slot->sq_w != NULL ? slot->sq_w->app : NULL;

	(void)req_user_data;

	if (app == NULL) {
		DOCA_LOG_ERR("IO command qid=%u opcode=0x%02x cid=%u arrived on an unbound SQ", qid, opcode, cid);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INTERNAL_ERROR);
		return;
	}

	if (nvme_sqe_psdt(sqe) != NVME_PSDT_PRP) {
		DOCA_LOG_WARN("IO command qid=%u opcode=0x%02x cid=%u uses SGL -> INVALID_FIELD", qid, opcode, cid);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_FIELD);
		return;
	}

	if (nsid != MEMOS_INITIATOR_REF_APP_NS_NSID) {
		DOCA_LOG_WARN("IO command qid=%u opcode=0x%02x cid=%u nsid=%u not present -> INVALID_NS",
			      qid,
			      opcode,
			      cid,
			      nsid);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_NS);
		return;
	}

	switch (opcode) {
	case NVME_KV_OPC_STORE:
		io_kv_store(app, slot, req, sqe, qid, cid);
		break;
	case NVME_KV_OPC_RETRIEVE:
		io_kv_retrieve(app, slot, req, sqe, qid, cid);
		break;
	case NVME_KV_OPC_EXIST:
		io_kv_exist(app, slot, req, sqe, qid, cid);
		break;
	default:
		/* Includes KV Delete (0x10) and List (0x06): defined by the command set but never issued
		 * by libdoca_kv, so this app does not carry an implementation for them. */
		DOCA_LOG_WARN("IO command qid=%u opcode=0x%02x cid=%u not implemented -> INVALID_OPCODE",
			      qid,
			      opcode,
			      cid);
		doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_OPCODE);
		break;
	}
}
