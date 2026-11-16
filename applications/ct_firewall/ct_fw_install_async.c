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

/* CT Firewall completion callbacks and asynchronous install and re-enable paths. */

#include <assert.h>
#include <string.h>

#include <doca_log.h>

#include <flow_ct_common.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::ASYNC);

/* Callbacks also fire for control-plane entries. */
static struct ct_fw_ctx *g_cb_app;

static const struct ct_fw_key zero_key;

void ct_fw_cb_init(struct ct_fw_ctx *app)
{
	g_cb_app = app;
}

/* NULL for a control-plane status. */
static struct ct_fw_worker *worker_of_status(struct ct_fw_ctx *app, const struct entries_status *status)
{
	uint16_t i;

	for (i = 0; i < app->nb_workers; i++) {
		if (&app->workers[i].ct_status == status)
			return &app->workers[i];
	}
	return NULL;
}

/* NULL for a control-plane queue. */
static struct ct_fw_worker *worker_of_queue(struct ct_fw_ctx *app, uint16_t ct_queue)
{
	uint16_t idx;

	if (ct_queue < app->nb_queues)
		return NULL;
	idx = ct_queue - app->nb_queues;
	if (idx >= app->nb_workers)
		return NULL;
	return &app->workers[idx];
}

/* The key may be stale: the connection must hold this handle. */
static struct ct_fw_conn *conn_of_entry(struct ct_fw_worker *worker,
					struct doca_flow_pipe *ct_pipe,
					uint16_t ct_queue,
					struct doca_flow_pipe_entry *entry)
{
	struct ct_fw_conn *conn;
	struct ct_fw_key key;
	void *priv;

	priv = doca_flow_ct_entry_get_priv_data(ct_queue, ct_pipe, entry);
	if (priv == NULL)
		return NULL;
	key = *(struct ct_fw_key *)priv;
	if (memcmp(&key, &zero_key, sizeof(key)) == 0)
		return NULL;
	conn = ct_fw_conn_lookup(&worker->table, &key);
	if (conn == NULL || conn->entry != entry)
		return NULL;
	return conn;
}

/* Callback-safe: never polls; a refusal is retried later. */
static void async_honor_remove(struct ct_fw_worker *worker, uint16_t ct_queue, struct ct_fw_conn *conn)
{
	doca_error_t result;

	/* A reset connection never gets its SYN-ACK. */
	ct_fw_conn_drop_held(worker, conn);
	if (conn->removing || worker->ct_queue_room < CT_FW_ENTRY_OPS)
		return;
	result = ct_fw_rm_entry_counted(worker, ct_queue, conn->entry);
	if (result == DOCA_ERROR_AGAIN || result == DOCA_ERROR_IN_PROGRESS)
		return;
	if (result == DOCA_SUCCESS) {
		ct_fw_queue_posted(worker);
		CT_FW_STAT_INC(worker->stats.conns_aged);
		ct_fw_conn_removal_posted(worker, conn);
		return;
	} else if (result != DOCA_ERROR_NOT_FOUND) {
		/* The rules may remain: stop, keeping the entry. */
		(void)ct_fw_fail_stop(worker, "removal", result);
		return;
	}
	conn->entry = NULL;
	ct_fw_conn_free_later(worker, conn);
}

static void handle_aged_entry(struct ct_fw_ctx *app,
			      struct ct_fw_worker *worker,
			      struct doca_flow_pipe_entry *entry,
			      uint16_t pipe_queue)
{
	struct doca_flow_pipe *ct_pipe = app->pipes.ct;
	struct ct_fw_conn *conn = conn_of_entry(worker, ct_pipe, pipe_queue, entry);
	doca_error_t result;

	/* Not repeated, so owe the removal while an operation is in flight; --sync removes it from the loop. */
	if (conn != NULL && conn->removing)
		return;
	if (conn != NULL && (conn->wv_op_inflight || app->config.sync_mode != CT_FW_ASYNC)) {
		conn->remove_requested = true;
		return;
	}

	/* No polling here: no room means owed. --sync waits for an ownerless removal from the loop. */
	if (conn == NULL && app->config.sync_mode != CT_FW_ASYNC)
		result = DOCA_ERROR_AGAIN;
	else
		result = worker->ct_queue_room >= CT_FW_ENTRY_OPS ? ct_fw_rm_entry_counted(worker, pipe_queue, entry) :
								    DOCA_ERROR_AGAIN;
	if (result == DOCA_ERROR_AGAIN || result == DOCA_ERROR_IN_PROGRESS) {
		/* Retried by the connection's next punt or the sweep, else by the worker loop. */
		if (conn != NULL)
			conn->remove_requested = true;
		else if (worker->nb_owed_rm < CT_FW_MAX_PENDING_FREE)
			worker->owed_rm[worker->nb_owed_rm++] = entry;
		else {
			CT_FW_STAT_INC(worker->stats.aged_rm_failed); /* kept until shutdown */
			if (ct_fw_log_due(worker->now_tsc, &worker->owed_rm_log_tsc))
				DOCA_LOG_WARN(
					"Worker %u: owed-removal list full (%u of %u); an idle entry is not retried and stays in "
					"hardware until shutdown",
					worker->worker_id,
					worker->nb_owed_rm,
					(unsigned int)CT_FW_MAX_PENDING_FREE);
		}
		return;
	}
	if (result == DOCA_SUCCESS) {
		ct_fw_queue_posted(worker);
		CT_FW_STAT_INC(worker->stats.conns_aged);
		if (conn != NULL) {
			ct_fw_conn_removal_posted(worker, conn);
			return;
		}
	} else if (result != DOCA_ERROR_NOT_FOUND) {
		/* The rules may remain: stop, keeping the entry. */
		(void)ct_fw_fail_stop(worker, "removal", result);
		return;
	}
	if (conn != NULL) {
		conn->entry = NULL;
		ct_fw_conn_free_later(worker, conn);
	}
}

void ct_fw_retry_owed_rm(struct ct_fw_worker *worker)
{
	uint32_t i, kept = 0;
	doca_error_t result;

	for (i = 0; i < worker->nb_owed_rm; i++) {
		struct doca_flow_pipe_entry *entry = worker->owed_rm[i];

		result = DOCA_ERROR_AGAIN;
		if (!ct_fw_failed(worker) && ct_fw_queue_make_room(worker))
			result = ct_fw_rm_entry_counted(worker, worker->ct_queue, entry);
		if (result == DOCA_SUCCESS) {
			ct_fw_queue_posted(worker);
			CT_FW_STAT_INC(worker->stats.conns_aged);
			if (worker->app->config.sync_mode != CT_FW_ASYNC)
				result = ct_fw_sync_wait(worker, entry, DOCA_FLOW_ENTRY_OP_DEL);
		}
		if (result == DOCA_SUCCESS || result == DOCA_ERROR_NOT_FOUND)
			continue;
		/* Still busy, or a failure that stops: keep the entry. */
		if (result != DOCA_ERROR_AGAIN && result != DOCA_ERROR_IN_PROGRESS)
			(void)ct_fw_fail_stop(worker, "removal", result);
		worker->owed_rm[kept++] = entry;
	}
	worker->nb_owed_rm = kept;
}

/* Only the handle is dropped. */
static void handle_failed_add(struct ct_fw_ctx *app,
			      struct ct_fw_worker *worker,
			      struct doca_flow_pipe_entry *entry,
			      uint16_t pipe_queue)
{
	struct ct_fw_conn *conn = conn_of_entry(worker, app->pipes.ct, pipe_queue, entry);

	if (conn == NULL)
		return;
	DOCA_LOG_DBG("Add completion reported failure; connection is released and re-adopted on its next segment");
	conn->entry = NULL;
	conn->state = CT_FW_CONN_HALF_OPEN;
	ct_fw_conn_drop_held(worker, conn);
	ct_fw_conn_free_later(worker, conn);
}

/* A callback transmits nothing. */
static void replay_later(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (worker->nb_pending_replay >= CT_FW_MAX_PENDING_REPLAY) {
		if (ct_fw_log_due(worker->now_tsc, &worker->replay_log_tsc))
			DOCA_LOG_WARN("Worker %u: replay list full (%u of %u); a held packet is dropped",
				      worker->worker_id,
				      worker->nb_pending_replay,
				      CT_FW_MAX_PENDING_REPLAY);
		ct_fw_conn_drop_held(worker, conn);
		return;
	}
	worker->pending_replay[worker->nb_pending_replay++] = conn->key;
}

static bool ring_push(struct ct_fw_slot_ring *ring, uint16_t idx)
{
	if (ring->count >= CT_FW_WV_POOL_SIZE)
		return false;
	ring->idx[(ring->head + ring->count) % CT_FW_WV_POOL_SIZE] = idx;
	ring->count++;
	return true;
}

static uint16_t ring_pop(struct ct_fw_slot_ring *ring)
{
	uint16_t idx = ring->idx[ring->head];

	ring->head = (uint16_t)((ring->head + 1) % CT_FW_WV_POOL_SIZE);
	ring->count--;
	return idx;
}

/* A slot is on at most one ring, so a failed push means the slot accounting is broken: stop. */
static void slot_push(struct ct_fw_worker *worker, struct ct_fw_slot_ring *ring, uint16_t idx, const char *what)
{
	if (ring_push(ring, idx))
		return;
	assert(!"window-validation slot ring full");
	(void)ct_fw_fail_stop(worker, what, DOCA_ERROR_FULL);
}

/* Held by conn's operation at step from submission until its completion is consumed. */
static struct ct_fw_wv_slot *wv_slot_take(struct ct_fw_worker *worker, struct ct_fw_conn *conn, enum ct_fw_wv_step step)
{
	struct ct_fw_wv_slot *slot;
	uint16_t idx;

	if (worker->nb_wv_free == 0)
		return NULL;
	idx = worker->wv_free[--worker->nb_wv_free];
	slot = &worker->wv_slots[idx];
	memset(&slot->rec, 0, sizeof(slot->rec));
	slot->rec.key = conn->key;
	slot->rec.entry = conn->entry;
	slot->rec.step = step;
	slot->submit_tsc = worker->now_tsc;
	slot->state = CT_FW_WV_SLOT_INFLIGHT;
	slot->abandoned = false;
	conn->wv_buf = idx;
	return slot;
}

static void wv_slot_free(struct ct_fw_worker *worker, uint16_t idx)
{
	struct ct_fw_wv_slot *slot = &worker->wv_slots[idx];

	/* A slot freed twice means the slot accounting is broken: stop. */
	if (slot->state == CT_FW_WV_SLOT_FREE || worker->nb_wv_free >= CT_FW_WV_POOL_SIZE) {
		assert(!"window-validation slot freed twice");
		(void)ct_fw_fail_stop(worker, "window-validation slot", DOCA_ERROR_BAD_STATE);
		return;
	}
	slot->state = CT_FW_WV_SLOT_FREE;
	slot->abandoned = false;
	worker->wv_free[worker->nb_wv_free++] = idx;
}

bool ct_fw_wv_slot_admit(const struct ct_fw_worker *worker, bool install)
{
	return worker->nb_wv_free > (install ? CT_FW_WV_REENABLE_RESERVE : 0);
}

bool ct_fw_install_admitted(struct ct_fw_worker *worker, uint64_t *refused)
{
	const struct ct_fw_config *cfg = &worker->app->config;

	/* Only an asynchronous profile write takes a slot. */
	if (cfg->sync_mode != CT_FW_ASYNC || !cfg->enable_window_validation || ct_fw_wv_slot_admit(worker, true))
		return true;
	CT_FW_STAT_INC(*refused);
	if (ct_fw_log_due(worker->now_tsc, &worker->wv_limit_log_tsc))
		DOCA_LOG_WARN("Worker %u: window-validation operations at the limit (%u of %u); new installs wait",
			      worker->worker_id,
			      ct_fw_wv_ops_in_use(worker),
			      (unsigned int)CT_FW_WV_POOL_SIZE);
	return false;
}

void ct_fw_worker_async_init(struct ct_fw_worker *worker)
{
	uint16_t i;

	for (i = 0; i < CT_FW_WV_POOL_SIZE; i++) {
		worker->wv_slots[i].state = CT_FW_WV_SLOT_FREE;
		worker->wv_slots[i].abandoned = false;
		worker->wv_free[i] = CT_FW_WV_POOL_SIZE - 1 - i;
	}
	worker->nb_wv_free = CT_FW_WV_POOL_SIZE;
	worker->nb_wv_orphans = 0;
	memset(&worker->wv_ready, 0, sizeof(worker->wv_ready));
	memset(&worker->add_owed, 0, sizeof(worker->add_owed));
	worker->nb_pending_replay = 0;
	worker->nb_owed_rm = 0;
}

void ct_fw_wv_slot_watchdog(struct ct_fw_worker *worker, uint64_t now_tsc, uint64_t watchdog_cycles)
{
	uint16_t i;

	for (i = 0; i < CT_FW_WV_POOL_SIZE; i++) {
		const struct ct_fw_wv_slot *slot = &worker->wv_slots[i];

		/* Also a released connection's operation, which no connection's watchdog sees. */
		if ((slot->state == CT_FW_WV_SLOT_INFLIGHT || slot->state == CT_FW_WV_SLOT_ORPHAN) &&
		    now_tsc - slot->submit_tsc > watchdog_cycles) {
			(void)ct_fw_fail_stop(worker, "operation did not complete", DOCA_ERROR_TIME_OUT);
			return;
		}
	}
}

void ct_fw_conn_reenable_async(struct ct_fw_worker *worker, struct ct_fw_conn *conn, bool reuse)
{
	struct doca_flow_pipe *ct_pipe = worker->app->pipes.ct;
	struct ct_fw_wv_slot *slot;
	doca_error_t result;

	/* One operation per entry; a query in flight also answers a new SYN. */
	if (conn->wv_op_inflight) {
		if (reuse && conn->wv_step == CT_FW_WV_QUERY)
			conn->reuse_pending = true;
		return;
	}
	if (conn->entry == NULL || conn->ct_queue != worker->ct_queue)
		return;
	/* Kept until a query or update says the entry validates again. */
	conn->reenable_owed = true;
	if (!ct_fw_wv_slot_admit(worker, false)) {
		if (ct_fw_log_due(worker->now_tsc, &worker->wv_limit_log_tsc))
			DOCA_LOG_WARN(
				"Worker %u: window-validation operations at the limit (%u of %u); re-enable deferred to the "
				"next punt",
				worker->worker_id,
				ct_fw_wv_ops_in_use(worker),
				(unsigned int)CT_FW_WV_POOL_SIZE);
		return; /* the next punt retries */
	}
	slot = wv_slot_take(worker, conn, CT_FW_WV_QUERY);

	/* Set before a completion can arrive. */
	conn->wv_step = CT_FW_WV_QUERY;
	conn->wv_op_inflight = true;
	conn->op_tsc = worker->now_tsc;
	conn->reuse_pending = reuse;
	result = doca_flow_ct_tcp_window_validation_query(ct_pipe,
							  worker->ct_queue,
							  conn->entry,
							  &slot->rec.profile,
							  true);
	if (result == DOCA_ERROR_TIME_OUT) {
		(void)ct_fw_fail_stop(worker, "re-enable query", result);
		return;
	}
	if (result != DOCA_SUCCESS) {
		/* The next punt retries. */
		ct_fw_log_reenable_failure(worker, true, result);
		conn->wv_step = CT_FW_WV_NONE;
		conn->wv_op_inflight = false;
		wv_slot_free(worker, conn->wv_buf);
	}
}

/* A full queue gets one poll and one retry, never a wait. */
static doca_error_t async_update_profile(struct ct_fw_worker *worker,
					 struct doca_flow_pipe_entry *entry,
					 const struct doca_flow_ct_tcp_window_validation *prof)
{
	struct doca_flow_pipe *ct_pipe = worker->app->pipes.ct;
	doca_error_t result;

	result = doca_flow_ct_tcp_window_validation_update(ct_pipe, worker->ct_queue, entry, prof, true);
	if (result != DOCA_ERROR_AGAIN)
		return result;
	ct_fw_queue_poll(worker);
	if (ct_fw_failed(worker))
		return DOCA_ERROR_TIME_OUT;
	return doca_flow_ct_tcp_window_validation_update(ct_pipe, worker->ct_queue, entry, prof, true);
}

/* Rebuilt from conn: after a profile write the submit context is gone. */
static doca_error_t async_add_entry(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	struct ct_fw_ctx *app = worker->app;
	struct doca_flow_ct_match match_origin, match_reply;
	struct doca_flow_ct_actions act_origin, act_reply;
	uint32_t entry_flags;
	doca_error_t result;
	bool has_actions;

	if (!ct_fw_queue_make_room(worker))
		return ct_fw_failed(worker) ? DOCA_ERROR_TIME_OUT : DOCA_ERROR_AGAIN;
	has_actions =
		ct_fw_build_add_inputs(app, conn, &match_origin, &match_reply, &act_origin, &act_reply, &entry_flags);
	result = doca_flow_ct_add_entry(worker->ct_queue,
					app->pipes.ct,
					entry_flags,
					&match_origin,
					&match_reply,
					has_actions ? &act_origin : NULL,
					has_actions ? &act_reply : NULL,
					NULL,
					NULL,
					app->config.aging_timeout_s,
					&worker->ct_status,
					conn->entry);
	if (result == DOCA_SUCCESS)
		ct_fw_queue_posted(worker);
	return result;
}

/* Give up an install before its add: roll back and release. */
static void async_install_abort(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	DOCA_LOG_DBG("Async install: given up before the add; connection released once its entry is returned");
	ct_fw_conn_drop_held(worker, conn);
	(void)ct_fw_conn_return_entry(worker, conn, CT_FW_OWED_ROLLBACK);
	ct_fw_conn_release(worker, conn);
}

/*
 * The add waits for the profile-write completion: the add puts the rules in hardware at once, and the profile must
 * be written before the entry meets traffic (doca_flow_ct_tcp_window_validation_update()).
 */
doca_error_t ct_fw_submit_async(struct ct_fw_worker *worker,
				struct ct_fw_conn *conn,
				const struct ct_fw_submit_ctx *ctx)
{
	struct ct_fw_ctx *app = worker->app;
	struct doca_flow_pipe *ct_pipe = app->pipes.ct;
	uint16_t ct_queue = worker->ct_queue;
	uint32_t prepare_flags = ctx->prepare_flags;
	struct ct_fw_wv_slot *slot = NULL;
	bool conn_found = false;
	doca_error_t result;

	/* Prepare binds the window-validation resource; the profile write holds a slot (admitted by the caller). */
	if (ctx->prof != NULL) {
		prepare_flags |= DOCA_FLOW_CT_ENTRY_FLAGS_TCP_WINDOW_VALIDATION;
		slot = wv_slot_take(worker, conn, CT_FW_WV_PENDING_ADD);
		if (slot == NULL)
			return ct_fw_submit_on_error(worker, conn, DOCA_ERROR_AGAIN);
	}

	result = doca_flow_ct_entry_prepare(ct_queue,
					    ct_pipe,
					    prepare_flags,
					    ctx->match_origin,
					    ctx->hash_origin,
					    ctx->match_reply,
					    ctx->hash_reply,
					    &conn->entry,
					    &conn_found);
	if (result != DOCA_SUCCESS) {
		if (slot != NULL)
			wv_slot_free(worker, conn->wv_buf);
		return ct_fw_submit_on_error(worker, conn, result);
	}
	/* Cannot happen without a duplicate filter (CT_FW_PREPARE_FLAGS); a guard in case the flags change. The
	 * handle belongs to another connection: never rolled back or removed. */
	if (conn_found) {
		conn->entry = NULL;
		if (slot != NULL)
			wv_slot_free(worker, conn->wv_buf);
		return ct_fw_submit_on_error(worker, conn, DOCA_ERROR_UNEXPECTED);
	}

	/* By key: positions are reused. */
	if (!ct_fw_conn_store_key(worker, conn)) {
		(void)ct_fw_conn_return_entry(worker, conn, CT_FW_OWED_ROLLBACK);
		if (slot != NULL)
			wv_slot_free(worker, conn->wv_buf);
		conn->state = CT_FW_CONN_HALF_OPEN;
		return DOCA_ERROR_BAD_STATE;
	}

	conn->ct_queue = ct_queue;

	if (slot != NULL) {
		slot->rec.entry = conn->entry;
		result = async_update_profile(worker, conn->entry, ctx->prof);
		/* A timeout latches before any rollback. */
		if (result == DOCA_ERROR_TIME_OUT || ct_fw_failed(worker))
			return ct_fw_fail_stop(worker, "install", result);
		if (result != DOCA_SUCCESS) {
			(void)ct_fw_conn_return_entry(worker, conn, CT_FW_OWED_ROLLBACK);
			wv_slot_free(worker, conn->wv_buf);
			return ct_fw_submit_on_error(worker, conn, result);
		}
		conn->wv_step = CT_FW_WV_PENDING_ADD;
		conn->wv_op_inflight = true;
	} else {
		/* No profile to wait for. */
		result = async_add_entry(worker, conn);
		if (result == DOCA_ERROR_TIME_OUT || ct_fw_failed(worker))
			return ct_fw_fail_stop(worker, "install", result);
		if (result != DOCA_SUCCESS) {
			(void)ct_fw_conn_return_entry(worker, conn, CT_FW_OWED_ROLLBACK);
			return ct_fw_submit_on_error(worker, conn, result);
		}
	}

	/* conn stays valid: completions only queue releases. */
	ct_fw_queue_poll(worker);

	return ct_fw_failed(worker) ? DOCA_ERROR_TIME_OUT : DOCA_SUCCESS;
}

/* What becomes of a popped slot once its completion is acted on. */
enum wv_fate {
	WV_FATE_FREE = 0, /**< the step is over */
	WV_FATE_INFLIGHT, /**< the slot carries the next operation (re-enable update) */
	WV_FATE_OWED,	  /**< the add waits on add_owed */
};

/* Profile written, no CT queue room for the add: keep everything and retry from the worker loop. */
static void add_owe(struct ct_fw_worker *worker, struct ct_fw_conn *conn, uint16_t idx)
{
	struct ct_fw_wv_slot *slot = &worker->wv_slots[idx];

	slot->state = CT_FW_WV_SLOT_ADD_OWED;
	slot->rec.step = CT_FW_WV_ADD_OWED;
	/* Every "operation outstanding" guard still applies; op_tsc keeps the install's start for the watchdog. */
	conn->wv_op_inflight = true;
	conn->wv_step = CT_FW_WV_ADD_OWED;
	CT_FW_STAT_INC(worker->stats.install_retries);
	DOCA_LOG_DBG("Async install: no CT queue room for the add; retried from the worker loop");
	slot_push(worker, &worker->add_owed, idx, "owed add list");
}

/* The connection may be released here; conn is never read by the caller afterwards. */
static enum wv_fate wv_act(struct ct_fw_worker *worker, struct ct_fw_conn *conn, uint16_t idx)
{
	struct ct_fw_wv_slot *slot = &worker->wv_slots[idx];
	struct doca_flow_pipe *ct_pipe = worker->app->pipes.ct;
	struct doca_flow_ct_tcp_window_validation prof;
	enum wv_fate fate = WV_FATE_FREE;
	doca_error_t result;

	switch (ct_fw_wv_apply(worker, conn, &slot->rec, &prof)) {
	case CT_FW_WV_ACT_ADD:
		/* Behind an owed add: keep the order and poll no more. */
		if (worker->add_owed.count != 0) {
			add_owe(worker, conn, idx);
			return WV_FATE_OWED;
		}
		result = async_add_entry(worker, conn);
		if (result == DOCA_ERROR_TIME_OUT || ct_fw_failed(worker)) {
			(void)ct_fw_fail_stop(worker, "install", result);
			return WV_FATE_FREE;
		}
		if (result == DOCA_ERROR_AGAIN) {
			add_owe(worker, conn, idx);
			return WV_FATE_OWED;
		}
		if (result != DOCA_SUCCESS) {
			ct_fw_log_offload_failure(worker, result);
			async_install_abort(worker, conn);
			return WV_FATE_FREE;
		}
		break;
	case CT_FW_WV_ACT_RELEASE:
		/* The profile write completed with a failure: a failed install, as a failed add. */
		if (slot->rec.status != DOCA_SUCCESS) {
			CT_FW_STAT_INC(worker->stats.install_failures);
			ct_fw_log_offload_failure(worker, slot->rec.status);
		}
		async_install_abort(worker, conn);
		return WV_FATE_FREE;
	case CT_FW_WV_ACT_REMOVE:
		/* The client's retransmitted SYN opens the new connection. */
		if (ct_fw_conn_teardown_entry(worker, conn)) {
			ct_fw_conn_release(worker, conn);
			return WV_FATE_FREE;
		}
		break; /* owed: honored below or by the sweep */
	case CT_FW_WV_ACT_UPDATE:
		/* The query's slot carries the update; set before its completion can arrive. */
		slot->rec.step = CT_FW_WV_UPDATE;
		slot->submit_tsc = worker->now_tsc;
		slot->state = CT_FW_WV_SLOT_INFLIGHT;
		result = doca_flow_ct_tcp_window_validation_update(ct_pipe, worker->ct_queue, conn->entry, &prof, true);
		if (result == DOCA_ERROR_TIME_OUT) {
			(void)ct_fw_fail_stop(worker, "re-enable", result);
			return WV_FATE_INFLIGHT; /* outcome unknown: the slot is left as is */
		}
		if (result != DOCA_SUCCESS) {
			/* The next punt starts over from the query. */
			ct_fw_log_reenable_failure(worker, false, result);
			conn->wv_step = CT_FW_WV_NONE;
			conn->wv_op_inflight = false;
		} else {
			fate = WV_FATE_INFLIGHT;
		}
		break;
	case CT_FW_WV_ACT_NONE:
	default:
		break;
	}

	/* Never a second removal after a fatal one. */
	if (ct_fw_failed(worker))
		return fate;
	/* A removal held back by the operation is honored now. */
	if (!conn->wv_op_inflight && conn->remove_requested && conn->state == CT_FW_CONN_OFFLOADED)
		async_honor_remove(worker, worker->ct_queue, conn);
	return fate;
}

void ct_fw_wv_abandon(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	struct ct_fw_wv_slot *slot = conn->wv_buf < CT_FW_WV_POOL_SIZE ? &worker->wv_slots[conn->wv_buf] : NULL;

	/* Only the slot this connection's step holds. */
	if (conn->wv_op_inflight && slot != NULL && slot->rec.entry == conn->entry &&
	    memcmp(&slot->rec.key, &conn->key, sizeof(slot->rec.key)) == 0) {
		switch (slot->state) {
		case CT_FW_WV_SLOT_INFLIGHT:
			/* Defensive: release with an operation outstanding does not happen today. */
			slot->state = CT_FW_WV_SLOT_ORPHAN;
			worker->nb_wv_orphans++;
			break;
		case CT_FW_WV_SLOT_READY:
		case CT_FW_WV_SLOT_ADD_OWED:
			slot->abandoned = true; /* freed when popped */
			break;
		default:
			break;
		}
	}
	conn->wv_op_inflight = false;
	conn->wv_step = CT_FW_WV_NONE;
}

void ct_fw_wv_drain(struct ct_fw_worker *worker)
{
	uint32_t pops;

	/* A slot completes at most twice per drain (query, then update); more waits for the next iteration. */
	for (pops = 0; pops < 2 * CT_FW_WV_POOL_SIZE && worker->wv_ready.count != 0 && !ct_fw_failed(worker); pops++) {
		uint16_t idx = ring_pop(&worker->wv_ready);
		struct ct_fw_wv_slot *slot = &worker->wv_slots[idx];
		struct ct_fw_conn *conn;

		if (slot->abandoned) {
			wv_slot_free(worker, idx);
			continue;
		}
		/* Only if the connection still owns the entry at that step with this slot. */
		conn = ct_fw_conn_lookup(&worker->table, &slot->rec.key);
		if (conn == NULL || conn->entry != slot->rec.entry || !conn->wv_op_inflight || conn->wv_buf != idx ||
		    conn->wv_step != slot->rec.step) {
			wv_slot_free(worker, idx);
			continue;
		}
		if (wv_act(worker, conn, idx) == WV_FATE_FREE)
			wv_slot_free(worker, idx);
	}
}

void ct_fw_retry_owed_adds(struct ct_fw_worker *worker)
{
	uint32_t n = worker->add_owed.count;
	bool blocked = false;

	/* The adds owed at entry, each visited once, oldest first. */
	for (; n > 0 && !ct_fw_failed(worker); n--) {
		uint16_t idx = ring_pop(&worker->add_owed);
		struct ct_fw_wv_slot *slot = &worker->wv_slots[idx];
		struct ct_fw_conn *conn;
		doca_error_t result;

		if (slot->abandoned) {
			wv_slot_free(worker, idx);
			continue;
		}
		conn = ct_fw_conn_lookup(&worker->table, &slot->rec.key);
		if (conn == NULL || conn->entry != slot->rec.entry || conn->wv_buf != idx ||
		    conn->wv_step != CT_FW_WV_ADD_OWED) {
			/* Cannot happen: a release marks the slot abandoned. Never touch conn. */
			assert(!"owed add without its connection");
			wv_slot_free(worker, idx);
			continue;
		}
		/* An RST cancels even behind a blocked head; it needs no queue room. */
		if (conn->remove_requested) {
			conn->wv_op_inflight = false;
			conn->wv_step = CT_FW_WV_NONE;
			wv_slot_free(worker, idx);
			async_install_abort(worker, conn);
			continue;
		}
		/* Keep the order among the adds still owed. */
		if (blocked) {
			slot_push(worker, &worker->add_owed, idx, "owed add list");
			continue;
		}
		result = async_add_entry(worker, conn);
		if (result == DOCA_ERROR_AGAIN && !ct_fw_failed(worker)) {
			blocked = true;
			slot_push(worker, &worker->add_owed, idx, "owed add list");
			continue;
		}
		conn->wv_op_inflight = false;
		conn->wv_step = CT_FW_WV_NONE;
		wv_slot_free(worker, idx);
		if (result == DOCA_ERROR_TIME_OUT || ct_fw_failed(worker)) {
			(void)ct_fw_fail_stop(worker, "install", result);
			return;
		}
		if (result != DOCA_SUCCESS) {
			ct_fw_log_offload_failure(worker, result);
			async_install_abort(worker, conn);
		}
		/* On success the add completion makes the entry live and replays the held packet. */
	}
}

/* Never processes the CT queue or submits an install. */
void ct_fw_entry_cb(struct doca_flow_pipe_entry *entry,
		    uint16_t pipe_queue,
		    enum doca_flow_entry_status status,
		    enum doca_flow_entry_op op,
		    void *user_ctx)
{
	struct entries_status *entry_status = (struct entries_status *)user_ctx;
	struct ct_fw_ctx *app = g_cb_app;
	struct ct_fw_worker *worker;
	struct doca_flow_pipe *ct_pipe;
	struct ct_fw_conn *conn;

	if (entry_status == NULL)
		return;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		/* Each failed operation is counted, also several in one poll; the flag is read for control-plane
		 * statuses only. */
		worker = app != NULL ? worker_of_status(app, entry_status) : NULL;
		if (worker != NULL)
			ct_fw_note_failed_completion(worker);
		else
			entry_status->failure = true;
	}

	if (op == DOCA_FLOW_ENTRY_OP_AGED) {
		if (app == NULL || __atomic_load_n(&app->failed, __ATOMIC_RELAXED))
			return;
		worker = worker_of_status(app, entry_status);
		if (worker != NULL && worker->ct_queue == pipe_queue)
			handle_aged_entry(app, worker, entry, pipe_queue);
		return;
	}
	/* Only control-plane statuses count entries. */
	worker = app != NULL ? worker_of_status(app, entry_status) : NULL;
	if (worker == NULL) {
		entry_status->nb_processed++;
		return;
	}
	/* --sync: record the awaited completion; the waiter acts on it. */
	if (app->config.sync_mode != CT_FW_ASYNC) {
		/* Also an idle entry's removal (no connection owns it). */
		if (op == DOCA_FLOW_ENTRY_OP_DEL && status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
			(void)ct_fw_fail_stop(worker, "removal", DOCA_ERROR_BAD_STATE);
		if (entry == worker->sync_entry && op == worker->sync_op && !worker->sync_done) {
			worker->sync_done = true;
			worker->sync_ok = status == DOCA_FLOW_ENTRY_STATUS_SUCCESS;
		}
		return;
	}
	/* Nothing changes after a queue failure. */
	if (__atomic_load_n(&app->failed, __ATOMIC_RELAXED))
		return;
	/* A removal failing at completion may leave its rules: stop, as --sync does. */
	if (op == DOCA_FLOW_ENTRY_OP_DEL && status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		(void)ct_fw_fail_stop(worker, "removal", DOCA_ERROR_BAD_STATE);
		return;
	}
	/* The held slot is released once its removal completed. */
	if (op == DOCA_FLOW_ENTRY_OP_DEL) {
		conn = conn_of_entry(worker, app->pipes.ct, pipe_queue, entry);
		if (conn != NULL && conn->removing) {
			conn->removing = false;
			conn->entry = NULL;
			ct_fw_conn_free_later(worker, conn);
		}
		return;
	}
	if (op != DOCA_FLOW_ENTRY_OP_ADD)
		return;
	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		handle_failed_add(app, worker, entry, pipe_queue);
		return;
	}

	ct_pipe = app->pipes.ct;
	conn = conn_of_entry(worker, ct_pipe, pipe_queue, entry);
	if (conn == NULL)
		return;
	if (conn->state != CT_FW_CONN_INSTALLING && conn->state != CT_FW_CONN_ADOPTING)
		return;

	conn->state = CT_FW_CONN_OFFLOADED;
	conn->ct_queue = pipe_queue;
	ct_fw_conn_count_installed(worker, conn);
	/* Mid-stream: data queued before the entry went live missed it. */
	if (conn->install_mode == CT_FW_INSTALL_THRESHOLD || conn->install_mode == CT_FW_INSTALL_ADOPT)
		ct_fw_backlog_arm(worker, conn);
	/* A reset arrived during the install. */
	if (conn->remove_requested) {
		async_honor_remove(worker, pipe_queue, conn);
		return;
	}
	if (conn->held_clone != NULL)
		replay_later(worker, conn);
}

/* Index of the slot whose query buffer is profile; false outside the worker's slots. */
static bool slot_of_profile(const struct ct_fw_worker *worker,
			    const struct doca_flow_ct_tcp_window_validation *profile,
			    uint16_t *idx)
{
	uintptr_t base = (uintptr_t)&worker->wv_slots[0].rec.profile;
	uintptr_t off = (uintptr_t)profile - base;

	if ((uintptr_t)profile < base || off % sizeof(struct ct_fw_wv_slot) != 0 ||
	    off / sizeof(struct ct_fw_wv_slot) >= CT_FW_WV_POOL_SIZE)
		return false;
	*idx = (uint16_t)(off / sizeof(struct ct_fw_wv_slot));
	return true;
}

/* The ORPHAN slot of entry; scanned only while orphans exist. */
static bool orphan_of_entry(const struct ct_fw_worker *worker, const struct doca_flow_pipe_entry *entry, uint16_t *idx)
{
	uint16_t i;

	for (i = 0; worker->nb_wv_orphans != 0 && i < CT_FW_WV_POOL_SIZE; i++) {
		if (worker->wv_slots[i].state == CT_FW_WV_SLOT_ORPHAN && worker->wv_slots[i].rec.entry == entry) {
			*idx = i;
			return true;
		}
	}
	return false;
}

void ct_fw_async_wv_comp_cb(struct doca_flow_pipe *pipe,
			    uint16_t queue,
			    doca_error_t status,
			    struct doca_flow_pipe_entry *entry,
			    struct doca_flow_ct_tcp_window_validation *profile,
			    void *user_ctx)
{
	struct ct_fw_ctx *app = g_cb_app;
	struct ct_fw_worker *worker;
	struct ct_fw_wv_slot *slot;
	struct ct_fw_conn *conn = NULL;
	uint16_t idx;

	(void)user_ctx;

	if (app == NULL || __atomic_load_n(&app->failed, __ATOMIC_RELAXED))
		return;
	worker = worker_of_queue(app, queue);
	if (worker == NULL)
		return;

	if (profile != NULL) {
		/* A query: the buffer is its slot's own. */
		if (!slot_of_profile(worker, profile, &idx)) {
			DOCA_LOG_DBG("Window-validation completion returned a buffer outside the worker's slots");
			return;
		}
	} else {
		/* An update carries no profile: the slot comes from the connection, else from the orphans. */
		conn = conn_of_entry(worker, pipe, queue, entry);
		if (conn != NULL && conn->wv_op_inflight && conn->wv_buf < CT_FW_WV_POOL_SIZE) {
			idx = conn->wv_buf;
		} else if (!orphan_of_entry(worker, entry, &idx)) {
			DOCA_LOG_DBG("Window-validation completion (%s) for no outstanding operation",
				     doca_error_get_descr(status));
			return;
		}
	}
	slot = &worker->wv_slots[idx];
	if (slot->state == CT_FW_WV_SLOT_ORPHAN && slot->rec.entry == entry) {
		worker->nb_wv_orphans--;
		wv_slot_free(worker, idx);
		return;
	}
	if (slot->state != CT_FW_WV_SLOT_INFLIGHT || slot->rec.entry != entry ||
	    slot->rec.step != (conn != NULL ? conn->wv_step : CT_FW_WV_QUERY)) {
		DOCA_LOG_DBG("Window-validation completion (%s) for no outstanding operation",
			     doca_error_get_descr(status));
		return;
	}
	/* A query with no profile back fails. */
	if (profile == NULL && slot->rec.step == CT_FW_WV_QUERY && status == DOCA_SUCCESS)
		status = DOCA_ERROR_BAD_STATE;
	slot->rec.status = status;
	slot->state = CT_FW_WV_SLOT_READY;
	slot_push(worker, &worker->wv_ready, idx, "window-validation completion ring");
}
