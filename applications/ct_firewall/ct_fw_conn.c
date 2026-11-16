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

/* CT Firewall per-worker connection table and hardware entry lifecycle. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>
#include <doca_flow_net.h>

#include <flow_ct_common.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::CONN);

static void nat_reply_key(const struct ct_fw_nat *nat, const struct ct_fw_key *key, struct ct_fw_key *rkey);

static bool key_equal(const struct ct_fw_key *a, const struct ct_fw_key *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* Names must be unique per process. */
static struct rte_hash *conn_hash_create(const char *kind, uint32_t entries)
{
	static uint32_t seq;
	char name[RTE_HASH_NAMESIZE];
	struct rte_hash_parameters params = {
		.name = name,
		.entries = RTE_MAX(entries, CT_FW_HASH_MIN_ENTRIES),
		.key_len = sizeof(struct ct_fw_key),
		.hash_func = rte_hash_crc,
		.hash_func_init_val = 0,
		.socket_id = SOCKET_ID_ANY,
		.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE,
	};
	struct rte_hash *hash;

	snprintf(name, sizeof(name), "ct_fw_%s_%u", kind, seq++);
	hash = rte_hash_create(&params);
	if (hash == NULL)
		DOCA_LOG_ERR("Failed to create %s table of %u entries: %s", kind, entries, rte_strerror(rte_errno));
	return hash;
}

doca_error_t ct_fw_conn_table_init(struct ct_fw_conn_table *table, uint32_t capacity, const struct ct_fw_config *cfg)
{
	int32_t max_key_id;

	if (table == NULL || capacity == 0 || cfg == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	memset(table, 0, sizeof(*table));
	table->hash = conn_hash_create("conn", capacity);
	if (table->hash == NULL)
		return DOCA_ERROR_NO_MEMORY;
	/* add_key positions index the pool. */
	max_key_id = rte_hash_max_key_id(table->hash);
	table->capacity = max_key_id < 0 ? 0 : (uint32_t)max_key_id + 1;
	table->conns = calloc(table->capacity, sizeof(*table->conns));
	if (table->conns == NULL) {
		DOCA_LOG_ERR("Failed to allocate connection table of %u slots", table->capacity);
		ct_fw_conn_table_fini(table);
		return DOCA_ERROR_NO_MEMORY;
	}
	/* Only NAT uses the reply index. */
	if (cfg->nat[CT_FW_FAMILY_IPV4].enabled || cfg->nat[CT_FW_FAMILY_IPV6].enabled) {
		table->reply_hash = conn_hash_create("reply", capacity);
		if (table->reply_hash == NULL) {
			ct_fw_conn_table_fini(table);
			return DOCA_ERROR_NO_MEMORY;
		}
	}
	table->max_load = capacity;
	table->cfg = cfg;
	return DOCA_SUCCESS;
}

void ct_fw_conn_table_fini(struct ct_fw_conn_table *table)
{
	uint32_t i;

	if (table == NULL)
		return;
	for (i = 0; table->conns != NULL && i < table->capacity; i++)
		if (table->conns[i].in_use)
			ct_fw_conn_release_clone(&table->conns[i]);
	free(table->conns);
	rte_hash_free(table->hash);
	rte_hash_free(table->reply_hash);
	memset(table, 0, sizeof(*table));
}

struct ct_fw_conn *ct_fw_conn_lookup(struct ct_fw_conn_table *table, const struct ct_fw_key *key)
{
	int32_t pos = rte_hash_lookup(table->hash, key);

	return pos < 0 ? NULL : &table->conns[pos];
}

/* Remove only this connection's mapping. */
static void conn_reply_index_drop(struct ct_fw_conn_table *table, const struct ct_fw_conn *conn)
{
	const struct ct_fw_nat *nat = ct_fw_conn_nat(table->cfg, conn);
	struct ct_fw_key rkey;
	void *data;

	if (!nat->enabled || table->reply_hash == NULL)
		return;
	nat_reply_key(nat, &conn->key, &rkey);
	if (rte_hash_lookup_data(table->reply_hash, &rkey, &data) >= 0 &&
	    (uintptr_t)data == (uintptr_t)(conn - table->conns))
		(void)rte_hash_del_key(table->reply_hash, &rkey);
}

bool ct_fw_log_due(uint64_t now_tsc, uint64_t *last_tsc)
{
	if (*last_tsc != 0 && now_tsc - *last_tsc < rte_get_timer_hz())
		return false;
	*last_tsc = now_tsc;
	return true;
}

static struct ct_fw_conn *conn_table_full(struct ct_fw_conn_table *table)
{
	if (ct_fw_log_due(rte_get_timer_cycles(), &table->full_log_tsc))
		DOCA_LOG_WARN("Worker %u: connection table full (%u of %u connections); new connections are dropped",
			      table->worker_id,
			      table->count,
			      table->max_load);
	return NULL;
}

struct ct_fw_conn *ct_fw_conn_insert(struct ct_fw_conn_table *table, const struct ct_fw_key *key)
{
	struct ct_fw_conn *slot = ct_fw_conn_lookup(table, key);
	int32_t pos;

	/* An existing key is returned at the cap. */
	if (slot != NULL)
		return slot;
	if (table->count >= table->max_load)
		return conn_table_full(table);
	pos = rte_hash_add_key(table->hash, key);
	if (pos < 0)
		return conn_table_full(table);
	slot = &table->conns[pos];
	memset(slot, 0, sizeof(*slot));
	slot->key = *key;
	slot->in_use = true;
	table->count++;
	return slot;
}

void ct_fw_conn_remove(struct ct_fw_conn_table *table, struct ct_fw_conn *conn)
{
	if (conn == NULL || !conn->in_use)
		return;
	conn_reply_index_drop(table, conn);
	ct_fw_conn_release_clone(conn);
	if (rte_hash_del_key(table->hash, &conn->key) < 0)
		DOCA_LOG_DBG("Removed connection was not in the table");
	memset(conn, 0, sizeof(*conn));
	table->count--;
}

/* Only while the connection keeps this tuple. */
struct ct_fw_conn *ct_fw_conn_lookup_reply(struct ct_fw_conn_table *table, const struct ct_fw_key *reply_key)
{
	struct ct_fw_conn *conn;
	struct ct_fw_key rkey;
	void *data;

	if (table->reply_hash == NULL || rte_hash_lookup_data(table->reply_hash, reply_key, &data) < 0)
		return NULL;
	conn = &table->conns[(uintptr_t)data];
	if (!conn->in_use)
		return NULL;
	nat_reply_key(ct_fw_conn_nat(table->cfg, conn), &conn->key, &rkey);
	return key_equal(&rkey, reply_key) ? conn : NULL;
}

void ct_fw_conn_reply_index_add(struct ct_fw_conn_table *table, const struct ct_fw_conn *conn)
{
	const struct ct_fw_nat *nat = ct_fw_conn_nat(table->cfg, conn);
	struct ct_fw_key rkey;

	if (!nat->enabled || table->reply_hash == NULL)
		return;
	nat_reply_key(nat, &conn->key, &rkey);
	/* A shared tuple maps to the newest. */
	if (rte_hash_add_key_data(table->reply_hash, &rkey, (void *)(uintptr_t)(conn - table->conns)) != 0 &&
	    ct_fw_log_due(rte_get_timer_cycles(), &table->reply_full_log_tsc))
		DOCA_LOG_WARN("Too many translated connections; replies to some may be dropped");
}

/* Give back the per-state count (COUNTING, ECHO). */
static void conn_leave_state(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (conn->state == CT_FW_CONN_COUNTING && worker->counting > 0)
		worker->counting--;
	else if (conn->state == CT_FW_CONN_ECHO && worker->echo_conns > 0)
		worker->echo_conns--;
}

/* What every release or reuse of a slot gives back; the hash position and the count are the caller's. */
static void conn_cleanup(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	conn_leave_state(worker, conn);
	ct_fw_conn_drop_held(worker, conn);
	if (conn->wv_op_inflight)
		ct_fw_wv_abandon(worker, conn);
	conn_reply_index_drop(&worker->table, conn);
	ct_fw_conn_release_clone(conn);
}

void ct_fw_conn_release(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (ct_fw_conn_retained(conn) || ct_fw_failed(worker))
		return;
	conn_cleanup(worker, conn);
	ct_fw_conn_remove(&worker->table, conn);
}

struct ct_fw_conn *ct_fw_conn_open(struct ct_fw_worker *worker, const struct ct_fw_key *key)
{
	struct ct_fw_conn *slot = ct_fw_conn_insert(&worker->table, key);
	struct ct_fw_key k;

	if (slot == NULL || !slot->free_pending)
		return slot;
	/* A queued release is reused in place: same position, same count. The key may be the slot's own. */
	k = *key;
	conn_cleanup(worker, slot);
	memset(slot, 0, sizeof(*slot));
	slot->key = k;
	slot->in_use = true;
	return slot;
}

void ct_fw_conn_free_later(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	if (conn == NULL || !conn->in_use || conn->free_pending || ct_fw_conn_retained(conn))
		return;
	conn->free_pending = true;
	/* A full queue leaves it to the sweep. */
	if (worker->nb_pending_free < CT_FW_MAX_PENDING_FREE)
		worker->pending_free[worker->nb_pending_free++] = conn->key;
}

void ct_fw_conn_drain_pending_free(struct ct_fw_worker *worker)
{
	uint32_t i;

	for (i = 0; i < worker->nb_pending_free; i++) {
		struct ct_fw_conn *conn = ct_fw_conn_lookup(&worker->table, &worker->pending_free[i]);

		/* Already released or recycled. */
		if (conn == NULL || !conn->free_pending)
			continue;
		ct_fw_conn_release(worker, conn);
	}
	worker->nb_pending_free = 0;
}

doca_error_t ct_fw_rm_entry_counted(struct ct_fw_worker *worker, uint16_t queue, struct doca_flow_pipe_entry *entry)
{
	struct doca_flow_pipe *ct_pipe = worker->app->pipes.ct;
	struct doca_flow_resource_query origin = {0}, reply = {0};
	uint64_t last_hit_s;
	bool read = doca_flow_ct_query_entry(queue, ct_pipe, 0, entry, &origin, &reply, &last_hit_s) == DOCA_SUCCESS;
	doca_error_t result = doca_flow_ct_rm_entry(queue, ct_pipe, CT_FW_RM_FLAGS, entry);

	/* Counted once per successful removal. */
	if (result == DOCA_SUCCESS && read) {
		CT_FW_STAT_ADD(worker->stats.conn_origin_pkts, origin.counter.total_pkts);
		CT_FW_STAT_ADD(worker->stats.conn_origin_bytes, origin.counter.total_bytes);
		CT_FW_STAT_ADD(worker->stats.conn_reply_pkts, reply.counter.total_pkts);
		CT_FW_STAT_ADD(worker->stats.conn_reply_bytes, reply.counter.total_bytes);
	}
	return result;
}

void ct_fw_conn_removal_posted(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	conn->removing = true;
	conn->remove_requested = true;
	conn->op_tsc = worker->now_tsc;
}

bool ct_fw_conn_teardown_entry(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	doca_error_t result;

	/* A posted removal completes on its own. */
	if (conn->removing)
		return false;
	if (conn->wv_op_inflight || !ct_fw_queue_make_room(worker)) {
		conn->remove_requested = true;
		return false;
	}
	result = ct_fw_rm_entry_counted(worker, worker->ct_queue, conn->entry);
	if (result == DOCA_ERROR_AGAIN || result == DOCA_ERROR_IN_PROGRESS) {
		/* The entry is still busy. */
		conn->remove_requested = true;
		return false;
	}
	if (result == DOCA_SUCCESS) {
		ct_fw_queue_posted(worker);
		CT_FW_STAT_INC(worker->stats.conns_aged);
		/* Async: the slot is held until the removal completes. */
		if (worker->app->config.sync_mode != CT_FW_SYNC) {
			ct_fw_conn_removal_posted(worker, conn);
			return false;
		}
		/* --sync waits for the removal; a failed one stops, keeping the entry. */
		if (ct_fw_sync_wait(worker, conn->entry, DOCA_FLOW_ENTRY_OP_DEL) != DOCA_SUCCESS) {
			(void)ct_fw_fail_stop(worker, "removal", DOCA_ERROR_BAD_STATE);
			return false;
		}
	} else if (result != DOCA_ERROR_NOT_FOUND) {
		/* The rules may remain: stop, keeping the entry. */
		(void)ct_fw_fail_stop(worker, "removal", result);
		return false;
	}
	conn->entry = NULL;
	return true;
}

bool ct_fw_conn_return_entry(struct ct_fw_worker *worker, struct ct_fw_conn *conn, enum ct_fw_entry_owed owed)
{
	struct doca_flow_pipe *ct_pipe = worker->app->pipes.ct;
	doca_error_t result = DOCA_SUCCESS;

	if (conn->entry == NULL)
		owed = CT_FW_OWED_NONE;
	if (owed == CT_FW_OWED_ROLLBACK) {
		result = doca_flow_ct_entry_prepare_rollback(worker->ct_queue, ct_pipe, conn->entry);
		/* Past its add, or still completing it: remove instead. */
		if (result == DOCA_ERROR_IN_PROGRESS || result == DOCA_ERROR_BAD_STATE)
			owed = CT_FW_OWED_REMOVE;
		else if (result != DOCA_ERROR_AGAIN)
			owed = CT_FW_OWED_NONE;
	}
	/* A failed install has no counters to read. */
	if (owed == CT_FW_OWED_REMOVE) {
		result = ct_fw_queue_make_room(worker) ?
				 doca_flow_ct_rm_entry(worker->ct_queue, ct_pipe, CT_FW_RM_FLAGS, conn->entry) :
				 DOCA_ERROR_AGAIN;
		if (result == DOCA_SUCCESS) {
			ct_fw_queue_posted(worker);
			if (worker->app->config.sync_mode == CT_FW_SYNC)
				result = ct_fw_sync_wait(worker, conn->entry, DOCA_FLOW_ENTRY_OP_DEL);
		}
		/* A removal that failed may leave rules: stop, keeping the entry. */
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_AGAIN && result != DOCA_ERROR_IN_PROGRESS &&
		    result != DOCA_ERROR_NOT_FOUND) {
			(void)ct_fw_fail_stop(worker, "failed install cleanup", result);
			return false;
		}
		if (result != DOCA_ERROR_AGAIN && result != DOCA_ERROR_IN_PROGRESS)
			owed = CT_FW_OWED_NONE;
	}
	if (result != DOCA_SUCCESS)
		DOCA_LOG_DBG("Failed install: entry rollback or removal returned %s", doca_error_get_descr(result));
	conn->entry_owed = owed;
	if (owed != CT_FW_OWED_NONE) {
		/* Keep the slot; a queued release would recycle it. */
		conn->free_pending = false;
		ct_fw_conn_drop_held(worker, conn);
		return false;
	}
	conn->entry = NULL;
	return true;
}

/* A connection operation is outstanding. */
static bool conn_op_outstanding(const struct ct_fw_conn *conn)
{
	return ct_fw_conn_retained(conn) || conn->state == CT_FW_CONN_INSTALLING ||
	       conn->state == CT_FW_CONN_ADOPTING || conn->wv_op_inflight || conn->removing;
}

void ct_fw_conn_sweep_round(struct ct_fw_worker *worker, uint64_t now_tsc, uint64_t watchdog_cycles)
{
	struct ct_fw_conn_table *table = &worker->table;
	uint64_t left;

	/* Before the empty-table return: a released connection's operation is not in the table. */
	ct_fw_wv_slot_watchdog(worker, now_tsc, watchdog_cycles);
	if (ct_fw_failed(worker))
		return;
	/* An idle worker is never behind. */
	if (table->conns == NULL || table->count == 0) {
		worker->sweep_left = 0;
		return;
	}
	if (worker->sweep_left != 0) {
		CT_FW_STAT_INC(worker->stats.sweep_lagged);
		if (ct_fw_log_due(now_tsc, &worker->sweep_lag_log_tsc))
			DOCA_LOG_WARN("Worker %u: sweep behind: %u slots left at the round start",
				      worker->worker_id,
				      worker->sweep_left);
	}
	/* Debt is carried, up to two passes over the table. */
	left = (uint64_t)worker->sweep_left + RTE_MAX(worker->sweep_budget, (uint32_t)CT_FW_SWEEP_MIN_PER_ROUND);
	worker->sweep_left = (uint32_t)RTE_MIN(left, 2 * (uint64_t)table->capacity);
}

void ct_fw_conn_sweep(struct ct_fw_worker *worker,
		      uint64_t now_tsc,
		      uint64_t idle_cycles,
		      uint64_t unconfirmed_cycles,
		      uint64_t watchdog_cycles)
{
	struct ct_fw_conn_table *table = &worker->table;
	uint32_t slice = RTE_MIN(worker->sweep_left, CT_FW_SWEEP_SLICE);
	uint32_t n;

	for (n = 0; n < slice; n++) {
		struct ct_fw_conn *slot;
		bool release = false;

		/* Also when the table empties mid-slice. */
		if (table->conns == NULL || table->count == 0) {
			worker->sweep_left = 0;
			return;
		}
		if (worker->sweep_cursor >= table->capacity)
			worker->sweep_cursor = 0;
		slot = &table->conns[worker->sweep_cursor++];
		worker->sweep_left--;

		/* An unused slot is skipped before anything else is read. */
		if (slot->in_use) {
			uint64_t idle = now_tsc - slot->last_seen_tsc;
			bool retained = ct_fw_conn_retained(slot);

			/* Retry an owed cleanup before the watchdog. */
			if (retained) {
				release = ct_fw_conn_return_entry(worker, slot, slot->entry_owed);
				if (ct_fw_failed(worker))
					return;
			}
			/* Polled every loop, so an overdue completion is lost. */
			if (!release && conn_op_outstanding(slot) && now_tsc - slot->op_tsc > watchdog_cycles) {
				(void)ct_fw_fail_stop(worker, "operation did not complete", DOCA_ERROR_TIME_OUT);
				return;
			}
			if (retained) {
				/* Retried above. */
			} else if (slot->free_pending) {
				release = true; /* the drain queue could not hold it */
			} else if (slot->state == CT_FW_CONN_HALF_OPEN || slot->state == CT_FW_CONN_COUNTING) {
				/* Unconfirmed until both directions are seen. */
				bool unconfirmed = slot->state == CT_FW_CONN_HALF_OPEN ||
						   slot->counted_dirs != CT_FW_COUNT_DIR_BOTH;

				if (idle > (unconfirmed ? unconfirmed_cycles : idle_cycles)) {
					CT_FW_STAT_INC(worker->stats.conns_sw_expired);
					release = true;
				}
			} else if (slot->state == CT_FW_CONN_ECHO) {
				release = idle > unconfirmed_cycles;
			} else if (slot->state == CT_FW_CONN_OFFLOADED) {
				/* An idle connection gets no punt to retry the removal. */
				if (slot->remove_requested && !slot->wv_op_inflight &&
				    ct_fw_conn_teardown_entry(worker, slot))
					release = true;
			}
			if (ct_fw_failed(worker))
				return;
		}
		if (release)
			ct_fw_conn_release(worker, slot);
	}
}

struct rte_mbuf *ct_fw_conn_take_held(struct ct_fw_conn *conn)
{
	struct rte_mbuf *m = conn->held_clone;

	conn->held_clone = NULL;
	conn->held_forward = false;
	return m;
}

void ct_fw_conn_release_clone(struct ct_fw_conn *conn)
{
	rte_pktmbuf_free(ct_fw_conn_take_held(conn));
}

void ct_fw_conn_drop_held(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	bool synack = conn->held_forward;
	struct rte_mbuf *m;

	if (ct_fw_failed(worker))
		return;
	m = ct_fw_conn_take_held(conn);
	if (m == NULL)
		return;
	if (synack)
		CT_FW_STAT_INC(worker->stats.install_drops);
	rte_pktmbuf_free(m);
}

doca_error_t ct_fw_fail_stop(struct ct_fw_worker *worker, const char *what, doca_error_t result)
{
	struct ct_fw_ctx *app = worker->app;

	if (__atomic_exchange_n(&app->failed, 1, __ATOMIC_RELAXED) == 0) {
		DOCA_LOG_ERR("Worker %u: CT queue %u failed (%s: %s); stopping",
			     worker->worker_id,
			     worker->ct_queue,
			     what,
			     doca_error_get_descr(result));
		*app->force_quit = 1;
	}
	return DOCA_ERROR_TIME_OUT;
}

void ct_fw_backlog_arm(const struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	conn->backlog_burst = worker->rx_bursts;
	conn->backlog_rx = worker->stats.rx_packets;
	conn->backlog_tsc = rte_get_timer_cycles(); /* now, not the burst's start: the wait may have been long */
	conn->backlog_settled = worker->backlog_settle == 0;
}

void ct_fw_reenable_landed(const struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	conn->reenable_owed = false;
	ct_fw_backlog_arm(worker, conn);
}

enum ct_fw_wv_action ct_fw_wv_apply(struct ct_fw_worker *worker,
				    struct ct_fw_conn *conn,
				    const struct ct_fw_pending_wv *rec,
				    struct doca_flow_ct_tcp_window_validation *prof)
{
	switch (rec->step) {
	case CT_FW_WV_PENDING_ADD:
		conn->wv_op_inflight = false;
		conn->wv_step = CT_FW_WV_NONE;
		return (rec->status == DOCA_SUCCESS && !conn->remove_requested) ? CT_FW_WV_ACT_ADD :
										  CT_FW_WV_ACT_RELEASE;
	case CT_FW_WV_QUERY:
		/* A new SYN frees the tuple only if both sides closed. */
		if (rec->status == DOCA_SUCCESS && conn->reuse_pending && ct_fw_profile_closed(&rec->profile)) {
			conn->reuse_pending = false;
			conn->reenable_owed = false;
			conn->wv_op_inflight = false;
			conn->wv_step = CT_FW_WV_NONE;
			return CT_FW_WV_ACT_REMOVE;
		}
		conn->reuse_pending = false;
		/* Rewriting a live entry would rewind it. */
		if (rec->status == DOCA_SUCCESS && rec->profile.enable == 0) {
			*prof = rec->profile;
			prof->enable = 1;
			conn->wv_step = CT_FW_WV_UPDATE;
			return CT_FW_WV_ACT_UPDATE;
		}
		if (rec->status == DOCA_SUCCESS)
			conn->reenable_owed = false;
		else
			ct_fw_log_reenable_failure(worker, true, rec->status);
		break;
	case CT_FW_WV_UPDATE:
		if (rec->status == DOCA_SUCCESS) {
			CT_FW_STAT_INC(worker->stats.conns_reenabled);
			ct_fw_reenable_landed(worker, conn);
		} else {
			ct_fw_log_reenable_failure(worker, false, rec->status);
		}
		break;
	case CT_FW_WV_NONE:
	default:
		break;
	}
	conn->wv_op_inflight = false;
	conn->wv_step = CT_FW_WV_NONE;
	return CT_FW_WV_ACT_NONE;
}

/* SYN and FIN each take one sequence number. */
static uint32_t obs_end(const struct ct_fw_tcp_obs *obs)
{
	return obs->seq + obs->payload_len + ((obs->flags & RTE_TCP_SYN_FLAG) ? 1 : 0) +
	       ((obs->flags & RTE_TCP_FIN_FLAG) ? 1 : 0);
}

/* End of the data and SYN sequence space (FIN not included). */
static uint32_t seq_end(const struct ct_fw_tcp_obs *obs)
{
	return obs->seq + obs->payload_len + ((obs->flags & RTE_TCP_SYN_FLAG) ? 1 : 0);
}

/* A SYN's window is never scaled (RFC 7323). */
static uint32_t obs_window_bytes(const struct ct_fw_tcp_obs *obs, uint8_t ws)
{
	uint32_t win = obs->window;

	if (ws != CT_FW_WS_DISABLED && !(obs->flags & RTE_TCP_SYN_FLAG))
		win <<= ws;
	return win;
}

/* reply_end is sent_end while the peer has sent only a bare SYN. */
static void fill_dir(struct doca_flow_ct_tcp_window_validation_dir *dir,
		     const struct ct_fw_tcp_obs *obs,
		     const struct ct_fw_tcp_obs *peer,
		     bool disable_scale,
		     uint8_t ws,
		     uint8_t peer_ws)
{
	bool peer_bare_syn = (peer->flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) == RTE_TCP_SYN_FLAG;
	uint32_t own_win = obs_window_bytes(obs, disable_scale ? CT_FW_WS_DISABLED : ws);
	uint32_t peer_win = obs_window_bytes(peer, disable_scale ? CT_FW_WS_DISABLED : peer_ws);

	dir->sent_end = obs_end(obs);
	dir->reply_end = peer_bare_syn ? dir->sent_end : peer->ack + peer_win;
	dir->max_win = own_win > 0 ? own_win : 1;
	dir->max_ack = obs->ack;
	dir->close_initiated = 0;
	dir->last_ack_seen = 0;
	dir->data_unacked = 0;
	dir->scale = disable_scale ? CT_FW_WS_DISABLED : ws;
}

uint32_t ct_fw_obs_advance(struct ct_fw_tcp_obs *obs, const struct ct_fw_tcp_obs *seg, bool established)
{
	uint32_t counted = 0;
	int32_t adv;

	if (!established) {
		/* First segment: the baseline. */
		obs->seq = seg->seq;
		obs->payload_len = seg->payload_len;
		obs->ack = seg->ack;
		obs->window = seg->window;
		obs->flags = seg->flags;
		return seg->payload_len;
	}
	/* The first segment after the SYN retires its sequence number; the high-water end is kept. */
	if ((obs->flags & RTE_TCP_SYN_FLAG) && !(seg->flags & RTE_TCP_SYN_FLAG)) {
		obs->seq += 1;
		obs->flags = (uint8_t)((obs->flags & ~RTE_TCP_SYN_FLAG) | (seg->flags & RTE_TCP_ACK_FLAG));
	}
	/* A retransmission or reordering neither advances nor counts; a SYN takes one sequence number, not payload. */
	adv = (int32_t)(seq_end(seg) - seq_end(obs));
	if (adv > 0) {
		counted = (uint32_t)adv > seg->payload_len ? seg->payload_len : (uint32_t)adv;
		obs->seq = seg->seq;
		obs->payload_len = seg->payload_len;
		obs->flags = seg->flags;
	}
	/* The ACK never moves back; the window follows it. */
	if ((seg->flags & RTE_TCP_ACK_FLAG) && (int32_t)(seg->ack - obs->ack) >= 0) {
		obs->ack = seg->ack;
		obs->window = seg->window;
	}
	return counted;
}

bool ct_fw_obs_is_next(const struct ct_fw_tcp_obs *obs, const struct ct_fw_tcp_obs *seg)
{
	return seg->seq == obs_end(obs);
}

bool ct_fw_conn_count_segment(struct ct_fw_conn *conn,
			      const struct ct_fw_tcp_obs *seg,
			      bool is_reply,
			      uint32_t offload_after)
{
	struct ct_fw_tcp_obs *obs = is_reply ? &conn->synack : &conn->syn;
	uint8_t dir_bit = is_reply ? CT_FW_COUNT_DIR_REPLY : CT_FW_COUNT_DIR_ORIGIN;
	bool established = (conn->counted_dirs & dir_bit) != 0;
	uint32_t counted;

	if (seg->flags & RTE_TCP_FIN_FLAG) {
		/* Any FIN rules the install out; only an in-sequence FIN closes its side. */
		conn->fin_seen = true;
		if (established && ct_fw_obs_is_next(obs, seg))
			conn->fin_dirs |= dir_bit;
	}
	counted = ct_fw_obs_advance(obs, seg, established);
	conn->counted_dirs |= dir_bit;
	if (is_reply)
		conn->bytes_reply += counted;
	else
		conn->bytes_origin += counted;
	/* A SYN or SYN-ACK retransmission never triggers the install. */
	return !conn->fin_seen && !(seg->flags & RTE_TCP_SYN_FLAG) && conn->counted_dirs == CT_FW_COUNT_DIR_BOTH &&
	       conn->bytes_origin + conn->bytes_reply >= offload_after;
}

bool ct_fw_profile_closed(const struct doca_flow_ct_tcp_window_validation *prof)
{
	return prof->state == DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_STATE_TCP_LAST_ACK ||
	       prof->state == DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_STATE_TCP_TIME_WAIT;
}

void ct_fw_profile_build(const struct ct_fw_config *cfg,
			 const struct ct_fw_conn *conn,
			 enum ct_fw_install_mode mode,
			 bool last_dir_reply,
			 struct doca_flow_ct_tcp_window_validation *prof)
{
	/* Adoption never saw the window scale negotiated. */
	bool disable_scale = (mode == CT_FW_INSTALL_ADOPT);

	memset(prof, 0, sizeof(*prof));
	prof->enable = 1;
	/* Adoption never saw the SYN, so SACK is assumed. */
	prof->selective_ack = conn->syn.sack_seen || mode == CT_FW_INSTALL_ADOPT;
	prof->liberal_mode = cfg->liberal_mode;
	prof->max_ack_window = cfg->max_ack_window;

	switch (mode) {
	case CT_FW_INSTALL_ADOPT:
		prof->state = DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_STATE_TCP_ESTABLISHED;
		prof->last_index = DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_TCP_LAST_ACK;
		prof->live_connection = 1;
		prof->liberal_mode = 1;
		break;
	case CT_FW_INSTALL_THRESHOLD:
		prof->state = DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_STATE_TCP_ESTABLISHED;
		prof->last_index = DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_TCP_LAST_ACK;
		prof->live_connection = 1;
		break;
	case CT_FW_INSTALL_HANDSHAKE:
	default:
		prof->state = DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_STATE_TCP_SYN_RECV;
		prof->last_index = DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_TCP_LAST_SYNACK;
		prof->live_connection = 0;
		break;
	}

	if (mode == CT_FW_INSTALL_HANDSHAKE) {
		/* The last packet is the reply SYN-ACK. */
		const struct ct_fw_tcp_obs *baseline = &conn->synack;

		prof->last_direction = 1; /* 0 origin, 1 reply */
		prof->last_window = baseline->window;
		prof->last_seq = baseline->seq;
		prof->last_ack = baseline->ack;
		prof->last_end = obs_end(baseline);
	} else {
		const struct ct_fw_tcp_obs *last = last_dir_reply ? &conn->synack : &conn->syn;

		prof->last_direction = last_dir_reply ? 1 : 0;
		prof->last_window = last->window;
		prof->last_seq = last->seq;
		prof->last_ack = last->ack;
		prof->last_end = obs_end(last);
	}

	/* Scaling needs both peers' option. */
	{
		bool ws_negotiated = conn->syn.ws != CT_FW_WS_DISABLED && conn->synack.ws != CT_FW_WS_DISABLED;
		uint8_t ws_origin = ws_negotiated ? conn->syn.ws : CT_FW_WS_DISABLED;
		uint8_t ws_reply = ws_negotiated ? conn->synack.ws : CT_FW_WS_DISABLED;

		fill_dir(&prof->original_dir, &conn->syn, &conn->synack, disable_scale, ws_origin, ws_reply);
		fill_dir(&prof->reply_dir, &conn->synack, &conn->syn, disable_scale, ws_reply, ws_origin);
	}
}

/* Reply tuple of key for the hardware match and the reply index. */
static void nat_reply_key(const struct ct_fw_nat *nat, const struct ct_fw_key *key, struct ct_fw_key *rkey)
{
	*rkey = *key;
	memcpy(rkey->src, ct_fw_nat_addr_set(nat->new_dst, key->family) ? nat->new_dst : key->dst, sizeof(rkey->src));
	memcpy(rkey->dst, ct_fw_nat_addr_set(nat->new_src, key->family) ? nat->new_src : key->src, sizeof(rkey->dst));
	rkey->src_port = key->dst_port;
	rkey->dst_port = key->src_port;
}

void ct_fw_conn_nat_reply_key(const struct ct_fw_config *cfg,
			      const struct ct_fw_key *key,
			      uint32_t origin_port_code,
			      struct ct_fw_key *rkey)
{
	nat_reply_key(ct_fw_nat_for(cfg, key->family, origin_port_code), key, rkey);
}

bool ct_fw_conn_reply_is_plain_reverse(const struct ct_fw_conn_table *table, const struct ct_fw_conn *conn)
{
	const struct ct_fw_nat *nat = ct_fw_conn_nat(table->cfg, conn);
	struct ct_fw_key rkey, plain;

	if (!nat->enabled)
		return true;
	nat_reply_key(nat, &conn->key, &rkey);
	plain = conn->key;
	memcpy(plain.src, conn->key.dst, sizeof(plain.src));
	memcpy(plain.dst, conn->key.src, sizeof(plain.dst));
	plain.src_port = conn->key.dst_port;
	plain.dst_port = conn->key.src_port;
	return key_equal(&rkey, &plain);
}

/* Absolute addresses: hardware and software agree. */
void ct_fw_nat_translate(const struct ct_fw_config *cfg,
			 const struct ct_fw_conn *conn,
			 bool reply,
			 uint8_t out_src[CT_FW_IPV6_ADDR_BYTES],
			 uint8_t out_dst[CT_FW_IPV6_ADDR_BYTES])
{
	const struct ct_fw_nat *nat = ct_fw_conn_nat(cfg, conn);
	const struct ct_fw_key *key = &conn->key;

	if (!reply) {
		memcpy(out_src,
		       ct_fw_nat_addr_set(nat->new_src, key->family) ? nat->new_src : key->src,
		       CT_FW_IPV6_ADDR_BYTES);
		memcpy(out_dst,
		       ct_fw_nat_addr_set(nat->new_dst, key->family) ? nat->new_dst : key->dst,
		       CT_FW_IPV6_ADDR_BYTES);
	} else {
		memcpy(out_src, key->dst, CT_FW_IPV6_ADDR_BYTES);
		memcpy(out_dst, key->src, CT_FW_IPV6_ADDR_BYTES);
	}
}

static void fill_match_dir(struct doca_flow_ct_match *match,
			   const struct ct_fw_key *key,
			   uint32_t zone_meta,
			   bool reversed)
{
	uint16_t sport = reversed ? key->dst_port : key->src_port;
	uint16_t dport = reversed ? key->src_port : key->dst_port;

	memset(match, 0, sizeof(*match));
	if (key->family == CT_FW_FAMILY_IPV6) {
		memcpy(match->ipv6.src_ip, reversed ? key->dst : key->src, sizeof(match->ipv6.src_ip));
		memcpy(match->ipv6.dst_ip, reversed ? key->src : key->dst, sizeof(match->ipv6.dst_ip));
		match->ipv6.l4_port.src_port = rte_cpu_to_be_16(sport);
		match->ipv6.l4_port.dst_port = rte_cpu_to_be_16(dport);
		match->ipv6.next_proto = key->protocol;
		match->ipv6.metadata = zone_meta;
	} else {
		memcpy(&match->ipv4.src_ip, reversed ? key->dst : key->src, sizeof(match->ipv4.src_ip));
		memcpy(&match->ipv4.dst_ip, reversed ? key->src : key->dst, sizeof(match->ipv4.dst_ip));
		match->ipv4.l4_port.src_port = rte_cpu_to_be_16(sport);
		match->ipv4.l4_port.dst_port = rte_cpu_to_be_16(dport);
		match->ipv4.next_proto = key->protocol;
		match->ipv4.metadata = zone_meta;
	}
}

void ct_fw_build_match(const struct ct_fw_ctx *app,
		       const struct ct_fw_conn *conn,
		       struct doca_flow_ct_match *match_origin,
		       struct doca_flow_ct_match *match_reply)
{
	struct doca_flow_meta meta;
	uint32_t zone_meta;

	/* Both directions carry the connection's zone. */
	memset(&meta, 0, sizeof(meta));
	doca_flow_ct_meta_prepare(app->ports[0], &meta, conn->key.zone, false);
	zone_meta = meta.pkt_meta;

	fill_match_dir(match_origin, &conn->key, zone_meta, false);

	if (ct_fw_conn_nat(&app->config, conn)->enabled) {
		/* With NAT the reply matches the translated tuple. */
		struct ct_fw_key rkey;

		nat_reply_key(ct_fw_conn_nat(&app->config, conn), &conn->key, &rkey);
		fill_match_dir(match_reply, &rkey, zone_meta, false);
	} else {
		fill_match_dir(match_reply, &conn->key, zone_meta, true);
	}
}

/* The template rewrites every marked field, so all are set. */
static void fill_nat_action(struct doca_flow_ct_actions *action,
			    const struct ct_fw_config *cfg,
			    const struct ct_fw_conn *conn,
			    bool reply)
{
	const struct ct_fw_key *key = &conn->key;
	bool v6 = key->family == CT_FW_FAMILY_IPV6;
	uint8_t out_src[CT_FW_IPV6_ADDR_BYTES], out_dst[CT_FW_IPV6_ADDR_BYTES];

	memset(action, 0, sizeof(*action));
	action->resource_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	/* Template by L4 type, for the checksum fix-up. */
	{
		bool udp = (key->protocol == DOCA_FLOW_PROTO_UDP);

		action->data.action_idx = udp ? CT_FW_ACT_IDX_UDP : CT_FW_ACT_IDX_TCP;
	}

	ct_fw_nat_translate(cfg, conn, reply, out_src, out_dst);

	if (v6) {
		memcpy(action->data.ip6.src_ip, out_src, sizeof(action->data.ip6.src_ip));
		memcpy(action->data.ip6.dst_ip, out_dst, sizeof(action->data.ip6.dst_ip));
	} else {
		memcpy(&action->data.ip4.src_ip, out_src, sizeof(action->data.ip4.src_ip));
		memcpy(&action->data.ip4.dst_ip, out_dst, sizeof(action->data.ip4.dst_ip));
	}
	action->data.l4_port.src_port = rte_cpu_to_be_16(reply ? key->dst_port : key->src_port);
	action->data.l4_port.dst_port = rte_cpu_to_be_16(reply ? key->src_port : key->dst_port);
}

bool ct_fw_build_add_inputs(const struct ct_fw_ctx *app,
			    const struct ct_fw_conn *conn,
			    struct doca_flow_ct_match *match_origin,
			    struct doca_flow_ct_match *match_reply,
			    struct doca_flow_ct_actions *act_origin,
			    struct doca_flow_ct_actions *act_reply,
			    uint32_t *entry_flags)
{
	*entry_flags = DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
		       DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY | DOCA_FLOW_CT_ENTRY_FLAGS_COUNTER_ORIGIN |
		       DOCA_FLOW_CT_ENTRY_FLAGS_COUNTER_REPLY;
	if (conn->key.family == CT_FW_FAMILY_IPV6)
		*entry_flags |= DOCA_FLOW_CT_ENTRY_FLAGS_IPV6_ORIGIN | DOCA_FLOW_CT_ENTRY_FLAGS_IPV6_REPLY;

	ct_fw_build_match(app, conn, match_origin, match_reply);
	/* Only a translating connection needs actions. */
	if (!ct_fw_conn_nat(&app->config, conn)->enabled)
		return false;
	fill_nat_action(act_origin, &app->config, conn, false);
	fill_nat_action(act_reply, &app->config, conn, true);
	return true;
}

static doca_error_t conn_offload(struct ct_fw_worker *worker,
				 struct ct_fw_conn *conn,
				 enum ct_fw_install_mode mode,
				 bool last_dir_reply)
{
	struct ct_fw_ctx *app = worker->app;
	struct doca_flow_ct_match match_origin, match_reply;
	struct doca_flow_ct_actions act_origin, act_reply;
	struct doca_flow_ct_tcp_window_validation prof;
	const struct doca_flow_ct_tcp_window_validation *prof_ptr = NULL;
	bool is_ipv6 = conn->key.family == CT_FW_FAMILY_IPV6;
	uint32_t prepare_flags = CT_FW_PREPARE_FLAGS;
	doca_be32_t zone_field = rte_cpu_to_be_32(CT_FW_META_ZONE_MASK);
	uint32_t hash_origin, hash_reply;
	uint32_t entry_flags;
	bool has_actions;

	has_actions =
		ct_fw_build_add_inputs(app, conn, &match_origin, &match_reply, &act_origin, &act_reply, &entry_flags);

	if (app->config.enable_window_validation && mode != CT_FW_INSTALL_UDP) {
		ct_fw_profile_build(&app->config, conn, mode, last_dir_reply, &prof);
		prof_ptr = &prof;
	}

	hash_origin = flow_ct_hash_6tuple(&match_origin, zone_field, is_ipv6);
	hash_reply = flow_ct_hash_6tuple(&match_reply, zone_field, is_ipv6);

	conn->install_mode = mode;
	conn->state = (mode == CT_FW_INSTALL_ADOPT) ? CT_FW_CONN_ADOPTING : CT_FW_CONN_INSTALLING;
	conn->op_tsc = worker->now_tsc;

	{
		struct ct_fw_submit_ctx ctx = {
			.match_origin = &match_origin,
			.match_reply = &match_reply,
			.act_origin = has_actions ? &act_origin : NULL,
			.act_reply = has_actions ? &act_reply : NULL,
			.prof = prof_ptr,
			.hash_origin = hash_origin,
			.hash_reply = hash_reply,
			.prepare_flags = prepare_flags,
			.entry_flags = entry_flags,
			.timeout_s = app->config.aging_timeout_s,
		};

		return worker->submit(worker, conn, &ctx);
	}
}

void ct_fw_conn_count_installed(struct ct_fw_worker *worker, const struct ct_fw_conn *conn)
{
	switch (conn->install_mode) {
	case CT_FW_INSTALL_ADOPT:
		CT_FW_STAT_INC(worker->stats.conns_adopted);
		break;
	case CT_FW_INSTALL_THRESHOLD:
		CT_FW_STAT_INC(worker->stats.conns_threshold_installed);
		break;
	case CT_FW_INSTALL_UDP:
		CT_FW_STAT_INC(worker->stats.conns_udp_installed);
		break;
	case CT_FW_INSTALL_HANDSHAKE:
	default:
		CT_FW_STAT_INC(worker->stats.conns_installed);
		break;
	}
}

void ct_fw_note_failed_completion(struct ct_fw_worker *worker)
{
	CT_FW_STAT_INC(worker->stats.install_failures);
	if (ct_fw_log_due(worker->now_tsc, &worker->failure_log_tsc))
		DOCA_LOG_WARN("Worker %u: a CT operation completed with a failure (install_failures=%" PRIu64 ")",
			      worker->worker_id,
			      worker->stats.install_failures);
}

void ct_fw_queue_poll(struct ct_fw_worker *worker)
{
	/* No minimum room: a stalled queue must not block the loop. */
	doca_error_t result = doca_flow_ct_entries_process(worker->app->ports[0],
							   worker->ct_queue,
							   0,
							   CT_FW_CT_QUEUE_RESERVE,
							   &worker->ct_queue_room);

	/* A timeout may be reported only once. */
	if (result == DOCA_ERROR_TIME_OUT) {
		(void)ct_fw_fail_stop(worker, "completion poll", result);
		return;
	}
	/* On failure the accounted room stands. */
	if (result != DOCA_SUCCESS)
		DOCA_LOG_DBG("Worker %u: failed to process CT entries: %s",
			     worker->worker_id,
			     doca_error_get_descr(result));
}

bool ct_fw_queue_make_room(struct ct_fw_worker *worker)
{
	if (ct_fw_failed(worker))
		return false;
	if (worker->ct_queue_room >= CT_FW_ENTRY_OPS)
		return true;
	ct_fw_queue_poll(worker);
	return !ct_fw_failed(worker) && worker->ct_queue_room >= CT_FW_ENTRY_OPS;
}

void ct_fw_log_offload_failure(struct ct_fw_worker *worker, doca_error_t result)
{
	/* Capacity and entry-state refusals are expected. */
	if (result == DOCA_ERROR_IN_PROGRESS || result == DOCA_ERROR_BAD_STATE || result == DOCA_ERROR_AGAIN ||
	    result == DOCA_ERROR_FULL)
		DOCA_LOG_DBG("Connection not offloaded: %s", doca_error_get_descr(result));
	else if (ct_fw_log_due(worker->now_tsc, &worker->offload_log_tsc))
		DOCA_LOG_ERR("Failed to offload connection: %s", doca_error_get_descr(result));
}

bool ct_fw_conn_store_key(struct ct_fw_worker *worker, const struct ct_fw_conn *conn)
{
	void *priv = doca_flow_ct_entry_get_priv_data(worker->ct_queue, worker->app->pipes.ct, conn->entry);

	if (priv == NULL) {
		if (ct_fw_log_due(worker->now_tsc, &worker->key_log_tsc))
			DOCA_LOG_ERR("Failed to store the connection key in its CT entry");
		return false;
	}
	*(struct ct_fw_key *)priv = conn->key;
	return true;
}

void ct_fw_log_reenable_failure(struct ct_fw_worker *worker, bool query, doca_error_t result)
{
	if (result == DOCA_ERROR_AGAIN)
		return;
	if (query && ct_fw_log_due(worker->now_tsc, &worker->query_log_tsc))
		DOCA_LOG_ERR("Failed to query window-validation for re-enable: %s", doca_error_get_descr(result));
	else if (!query && ct_fw_log_due(worker->now_tsc, &worker->reenable_log_tsc))
		DOCA_LOG_ERR("Failed to re-enable window-validation: %s", doca_error_get_descr(result));
}

doca_error_t ct_fw_submit_on_error(struct ct_fw_worker *worker, struct ct_fw_conn *conn, doca_error_t result)
{
	/* Fatal: the connection keeps its state. */
	if (result == DOCA_ERROR_TIME_OUT || ct_fw_failed(worker))
		return ct_fw_fail_stop(worker, "install", result);
	if (result == DOCA_ERROR_FULL)
		CT_FW_STAT_INC(worker->stats.pool_exhausted);
	ct_fw_log_offload_failure(worker, result);
	conn->state = CT_FW_CONN_HALF_OPEN;
	/* Keep the entry only while it is owed back. */
	if (!ct_fw_conn_retained(conn))
		conn->entry = NULL;
	return result;
}

/* --sync: a poll error or the watchdog leaves an operation's outcome unknown, so it stops. */
static bool sync_poll(struct ct_fw_worker *worker, uint64_t deadline)
{
	doca_error_t result = doca_flow_ct_entries_process(worker->app->ports[0],
							   worker->ct_queue,
							   0,
							   CT_FW_CT_QUEUE_RESERVE,
							   &worker->ct_queue_room);

	if (result == DOCA_SUCCESS && rte_get_tsc_cycles() > deadline)
		result = DOCA_ERROR_TIME_OUT;
	if (result != DOCA_SUCCESS) {
		(void)ct_fw_fail_stop(worker, "completion poll", result);
		return false;
	}
	/* A completion handled in this poll may have stopped the application. */
	return !ct_fw_failed(worker);
}

static uint64_t sync_deadline(void)
{
	return rte_get_tsc_cycles() + rte_get_tsc_hz() * CT_FW_OP_WATCHDOG_S;
}

doca_error_t ct_fw_sync_wait(struct ct_fw_worker *worker,
			     struct doca_flow_pipe_entry *entry,
			     enum doca_flow_entry_op op)
{
	uint64_t deadline = sync_deadline();
	bool polled;

	worker->sync_entry = entry;
	worker->sync_op = op;
	worker->sync_done = false;
	do {
		polled = sync_poll(worker, deadline);
	} while (polled && !worker->sync_done);
	worker->sync_entry = NULL;
	if (!polled)
		return DOCA_ERROR_TIME_OUT;
	return worker->sync_ok ? DOCA_SUCCESS : DOCA_ERROR_BAD_STATE;
}

/* An entry that must not stay (its add failed, or its key was not stored): remove it, retrying while the library
 * is still busy with it, and wait for the removal; else stop. */
static void sync_remove_failed_add(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	uint64_t deadline = sync_deadline();
	doca_error_t result;

	for (;;) {
		result = doca_flow_ct_rm_entry(worker->ct_queue, worker->app->pipes.ct, CT_FW_RM_FLAGS, conn->entry);
		if (result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_AGAIN)
			break;
		if (!sync_poll(worker, deadline))
			return;
	}
	if (result == DOCA_SUCCESS) {
		ct_fw_queue_posted(worker);
		result = ct_fw_sync_wait(worker, conn->entry, DOCA_FLOW_ENTRY_OP_DEL);
	} else if (result == DOCA_ERROR_NOT_FOUND) {
		result = DOCA_SUCCESS;
	}
	if (result != DOCA_SUCCESS) {
		(void)ct_fw_fail_stop(worker, "failed install cleanup", result);
		return;
	}
	conn->entry = NULL;
}

/* --sync install: the entry is live on return. */
doca_error_t ct_fw_submit_sync(struct ct_fw_worker *worker, struct ct_fw_conn *conn, const struct ct_fw_submit_ctx *ctx)
{
	struct ct_fw_ctx *app = worker->app;
	doca_error_t result;

	result = flow_ct_create_entry(app->ports[0],
				      worker->ct_queue,
				      app->pipes.ct,
				      ctx->prepare_flags,
				      ctx->entry_flags & ~DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT, /* the app waits below */
				      ctx->match_origin,
				      ctx->match_reply,
				      ctx->hash_origin,
				      ctx->hash_reply,
				      ctx->act_origin,
				      ctx->act_reply,
				      ctx->prof,
				      NULL,
				      NULL,
				      ctx->timeout_s,
				      &worker->ct_status,
				      &conn->entry);
	if (result != DOCA_SUCCESS) {
		/* Not rolled back: owed back to the sweep. */
		if (conn->entry != NULL && result == DOCA_ERROR_AGAIN)
			conn->entry_owed = CT_FW_OWED_ROLLBACK;
		return ct_fw_submit_on_error(worker, conn, result);
	}
	/* Live only once this add completes successfully. */
	result = ct_fw_sync_wait(worker, conn->entry, DOCA_FLOW_ENTRY_OP_ADD);
	if (result == DOCA_ERROR_BAD_STATE)
		sync_remove_failed_add(worker, conn);
	if (result != DOCA_SUCCESS)
		return ct_fw_submit_on_error(worker, conn, result);

	/* Aging finds the connection by this key: without it the entry is removed. */
	if (!ct_fw_conn_store_key(worker, conn)) {
		sync_remove_failed_add(worker, conn);
		return ct_fw_submit_on_error(worker, conn, DOCA_ERROR_BAD_STATE);
	}

	conn->state = CT_FW_CONN_OFFLOADED;
	conn->ct_queue = worker->ct_queue;
	ct_fw_conn_count_installed(worker, conn);
	return DOCA_SUCCESS;
}

doca_error_t ct_fw_conn_install(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	/* At the window-validation limit the SYN-ACK is dropped (wv_limit_drops) and its retransmission retries. */
	if (!ct_fw_install_admitted(worker, &worker->stats.wv_limit_drops))
		return DOCA_ERROR_FULL;
	return conn_offload(worker, conn, CT_FW_INSTALL_HANDSHAKE, false);
}

doca_error_t ct_fw_conn_install_udp(struct ct_fw_worker *worker, struct ct_fw_conn *conn)
{
	return conn_offload(worker, conn, CT_FW_INSTALL_UDP, false);
}

doca_error_t ct_fw_conn_install_midstream(struct ct_fw_worker *worker,
					  struct ct_fw_conn *conn,
					  enum ct_fw_install_mode mode,
					  bool last_dir_reply)
{
	conn_leave_state(worker, conn);
	return conn_offload(worker, conn, mode, last_dir_reply);
}

doca_error_t ct_fw_conn_query_profile(struct ct_fw_worker *worker,
				      struct ct_fw_conn *conn,
				      struct doca_flow_ct_tcp_window_validation *prof)
{
	doca_error_t result;

	if (conn->entry == NULL)
		return DOCA_ERROR_BAD_STATE;
	/* Update and query must run on the queue that allocated the entry. */
	if (worker->ct_queue != conn->ct_queue)
		return DOCA_ERROR_INVALID_VALUE;
	result = doca_flow_ct_tcp_window_validation_query(worker->app->pipes.ct,
							  worker->ct_queue,
							  conn->entry,
							  prof,
							  true);
	if (result == DOCA_ERROR_TIME_OUT)
		return ct_fw_fail_stop(worker, "re-enable query", result);
	if (result != DOCA_SUCCESS)
		ct_fw_log_reenable_failure(worker, true, result);
	return result;
}

doca_error_t ct_fw_conn_reenable(struct ct_fw_worker *worker,
				 struct ct_fw_conn *conn,
				 const struct doca_flow_ct_tcp_window_validation *queried,
				 bool *rewritten)
{
	struct doca_flow_pipe *ct_pipe = worker->app->pipes.ct;
	struct doca_flow_ct_tcp_window_validation prof;
	doca_error_t result;

	if (queried != NULL) {
		prof = *queried;
	} else {
		result = ct_fw_conn_query_profile(worker, conn, &prof);
		if (result != DOCA_SUCCESS)
			return result;
	}

	/* Only a disabled entry is rewritten. */
	if (prof.enable == 1)
		return DOCA_SUCCESS;

	prof.enable = 1;
	result = doca_flow_ct_tcp_window_validation_update(ct_pipe, worker->ct_queue, conn->entry, &prof, true);
	if (result == DOCA_ERROR_TIME_OUT)
		return ct_fw_fail_stop(worker, "re-enable", result);
	if (result != DOCA_SUCCESS) {
		ct_fw_log_reenable_failure(worker, false, result);
		return result;
	}

	CT_FW_STAT_INC(worker->stats.conns_reenabled);
	*rewritten = true;
	return DOCA_SUCCESS;
}
