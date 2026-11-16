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

#include <inttypes.h>

#include <doca_log.h>
#include <doca_flow.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::STATS);

/* In pipeline build order. */
static const char *const syndrome_labels[] = {"valid", "state_change", "invalid", "disabled"};

/* Queueless: safe while the workers run. */
static bool ct_fw_query_entry(struct doca_flow_pipe_entry *entry, struct doca_flow_resource_query *out)
{
	/*
	 * The call writes one result per queried item of an ordered-list pipe entry, so its output is
	 * an array; the basic and control pipe entries queried here have one.
	 */
	struct doca_flow_resource_query data[1] = {0};
	bool ok = entry != NULL && doca_flow_resource_query_entry(entry, data) == DOCA_SUCCESS;

	*out = ok ? data[0] : (struct doca_flow_resource_query){0};
	return ok;
}

/* Live report: a failed query of an existing entry is noted in *failed. */
static bool ct_fw_query_live(struct doca_flow_pipe_entry *entry, struct doca_flow_resource_query *out, bool *failed)
{
	if (ct_fw_query_entry(entry, out))
		return true;
	if (entry != NULL)
		*failed = true;
	return false;
}

/* An absent pipe adds 0; false if the query fails. */
static bool ct_fw_query_miss(struct doca_flow_pipe *pipe, uint64_t *total)
{
	struct doca_flow_resource_query q = {0};

	if (pipe == NULL)
		return true;
	if (doca_flow_resource_query_pipe_miss(pipe, &q) != DOCA_SUCCESS)
		return false;
	*total += q.counter.total_pkts;
	return true;
}

#define CT_FW_STATS_ADD(total, w, field) ((total)->field += __atomic_load_n(&(w)->field, __ATOMIC_RELAXED))

void ct_fw_stats_accumulate(struct ct_fw_stats *total, const struct ct_fw_stats *w)
{
	CT_FW_STATS_ADD(total, w, rx_packets);
	CT_FW_STATS_ADD(total, w, tx_packets);
	CT_FW_STATS_ADD(total, w, conns_installed);
	CT_FW_STATS_ADD(total, w, conns_adopted);
	CT_FW_STATS_ADD(total, w, conns_threshold_installed);
	CT_FW_STATS_ADD(total, w, conns_udp_installed);
	CT_FW_STATS_ADD(total, w, conns_reenabled);
	CT_FW_STATS_ADD(total, w, conns_aged);
	CT_FW_STATS_ADD(total, w, conns_sw_expired);
	CT_FW_STATS_ADD(total, w, aged_rm_failed);
	CT_FW_STATS_ADD(total, w, reinjections);
	CT_FW_STATS_ADD(total, w, pool_exhausted);
	CT_FW_STATS_ADD(total, w, frag_drops);
	CT_FW_STATS_ADD(total, w, frag_reassembled);
	CT_FW_STATS_ADD(total, w, state_drops);
	CT_FW_STATS_ADD(total, w, wrong_uplink_drops);
	CT_FW_STATS_ADD(total, w, reuse_drops);
	CT_FW_STATS_ADD(total, w, parse_drops);
	CT_FW_STATS_ADD(total, w, other_forwarded);
	CT_FW_STATS_ADD(total, w, install_failures);
	CT_FW_STATS_ADD(total, w, install_drops);
	CT_FW_STATS_ADD(total, w, nat_other_drops);
	CT_FW_STATS_ADD(total, w, conn_origin_pkts);
	CT_FW_STATS_ADD(total, w, conn_origin_bytes);
	CT_FW_STATS_ADD(total, w, conn_reply_pkts);
	CT_FW_STATS_ADD(total, w, conn_reply_bytes);
	CT_FW_STATS_ADD(total, w, install_retries);
	CT_FW_STATS_ADD(total, w, wv_limit_drops);
	CT_FW_STATS_ADD(total, w, wv_limit_defers);
	CT_FW_STATS_ADD(total, w, sweep_lagged);
}

static void ct_fw_stats_total(const struct ct_fw_ctx *app, struct ct_fw_stats *total)
{
	uint16_t i;

	*total = (struct ct_fw_stats){0};
	for (i = 0; i < app->nb_workers; i++)
		ct_fw_stats_accumulate(total, &app->workers[i].stats);
}

/* Hit counters of entries still in place. */
static void ct_fw_stats_open_at_exit(const struct ct_fw_ctx *app, struct ct_fw_stats *open)
{
	uint16_t i;
	uint32_t c;

	*open = (struct ct_fw_stats){0};
	for (i = 0; i < app->nb_workers; i++) {
		const struct ct_fw_worker *worker = &app->workers[i];

		for (c = 0; worker->table.conns != NULL && c < worker->table.capacity; c++) {
			const struct ct_fw_conn *conn = &worker->table.conns[c];
			struct doca_flow_resource_query origin = {0}, reply = {0};
			uint64_t last_hit_s;

			if (!conn->in_use || conn->entry == NULL || conn->state != CT_FW_CONN_OFFLOADED)
				continue;
			if (doca_flow_ct_query_entry(worker->ct_queue,
						     app->pipes.ct,
						     0,
						     conn->entry,
						     &origin,
						     &reply,
						     &last_hit_s) != DOCA_SUCCESS)
				continue;
			open->conn_origin_pkts += origin.counter.total_pkts;
			open->conn_origin_bytes += origin.counter.total_bytes;
			open->conn_reply_pkts += reply.counter.total_pkts;
			open->conn_reply_bytes += reply.counter.total_bytes;
		}
	}
}

void ct_fw_stats_dump(const struct ct_fw_ctx *app)
{
	struct ct_fw_stats total;
	uint16_t i;

	ct_fw_stats_total(app, &total);

	DOCA_LOG_INFO("==================== CT Firewall Stats ====================");
	DOCA_LOG_INFO("rx_packets=%" PRIu64 " tx_packets=%" PRIu64, total.rx_packets, total.tx_packets);
	DOCA_LOG_INFO("conns_installed=%" PRIu64 " conns_adopted=%" PRIu64 " conns_threshold_installed=%" PRIu64
		      " conns_udp_installed=%" PRIu64 " conns_reenabled=%" PRIu64 " conns_aged=%" PRIu64
		      " reinjections=%" PRIu64,
		      total.conns_installed,
		      total.conns_adopted,
		      total.conns_threshold_installed,
		      total.conns_udp_installed,
		      total.conns_reenabled,
		      total.conns_aged,
		      total.reinjections);
	DOCA_LOG_INFO("pool_exhausted=%" PRIu64, total.pool_exhausted);
	DOCA_LOG_INFO("frag_drops=%" PRIu64 " frag_reassembled=%" PRIu64 " state_drops=%" PRIu64,
		      total.frag_drops,
		      total.frag_reassembled,
		      total.state_drops);
	DOCA_LOG_INFO("conns_sw_expired=%" PRIu64, total.conns_sw_expired);
	DOCA_LOG_INFO("aged_rm_failed=%" PRIu64, total.aged_rm_failed);
	DOCA_LOG_INFO("parse_drops=%" PRIu64, total.parse_drops);
	DOCA_LOG_INFO("wrong_uplink_drops=%" PRIu64, total.wrong_uplink_drops);
	DOCA_LOG_INFO("reuse_drops=%" PRIu64, total.reuse_drops);
	DOCA_LOG_INFO("other_forwarded=%" PRIu64 " nat_other_drops=%" PRIu64,
		      total.other_forwarded,
		      total.nat_other_drops);
	DOCA_LOG_INFO("install_failures=%" PRIu64, total.install_failures);
	DOCA_LOG_INFO("install_drops=%" PRIu64, total.install_drops);
	{
		uint32_t in_use = 0;

		for (i = 0; app->config.sync_mode == CT_FW_ASYNC && i < app->nb_workers; i++)
			in_use += ct_fw_wv_ops_in_use(&app->workers[i]);
		DOCA_LOG_INFO("install_retries=%" PRIu64 " wv_limit_drops=%" PRIu64 " wv_limit_defers=%" PRIu64
			      " wv_ops_in_use=%u",
			      total.install_retries,
			      total.wv_limit_drops,
			      total.wv_limit_defers,
			      in_use);
	}
	DOCA_LOG_INFO("sweep_lagged=%" PRIu64, total.sweep_lagged);

	{
		const struct ct_fw_pipes *pipes = &app->pipes;
		struct doca_flow_resource_query query;
		uint32_t e;

		/* Off: nothing is validated; "valid" counts the TCP packets forwarded in hardware. */
		DOCA_LOG_INFO("---------------------- TCP window validation%s ----------------------",
			      app->config.enable_window_validation ? "" : " (off)");
		for (e = 0; e < pipes->nb_syn_entries && e < ARRAY_DIM(pipes->syn_entries); e++) {
			const char *label = e < ARRAY_DIM(syndrome_labels) ? syndrome_labels[e] : "unknown";

			if (pipes->syn_entries[e] == NULL)
				continue;
			if (ct_fw_query_entry(pipes->syn_entries[e], &query))
				DOCA_LOG_INFO("window_validation[%s]: pkts=%" PRIu64 " bytes=%" PRIu64,
					      label,
					      query.counter.total_pkts,
					      query.counter.total_bytes);
			else
				DOCA_LOG_WARN("window_validation[%s]: counter query failed", label);
		}

		DOCA_LOG_INFO("---------------------- Forwarding ----------------------");
		if (ct_fw_query_entry(pipes->ingress_tcp_entry, &query))
			DOCA_LOG_DBG("tcp_admitted: pkts=%" PRIu64 " bytes=%" PRIu64,
				     query.counter.total_pkts,
				     query.counter.total_bytes);
		else
			DOCA_LOG_DBG("tcp_admitted: counter query failed");
		/* Compare with reinjections. */
		if (ct_fw_query_entry(pipes->reinject_drop_entry, &query))
			DOCA_LOG_INFO("reinject_to_ct: pkts=%" PRIu64 " bytes=%" PRIu64,
				      query.counter.total_pkts,
				      query.counter.total_bytes);
		else
			DOCA_LOG_WARN("reinject_to_ct: counter query failed");
		/* Their entry was already gone. */
		if (ct_fw_query_entry(pipes->miss_mark_drop_entry, &query))
			DOCA_LOG_INFO("reinject_missed: pkts=%" PRIu64 " bytes=%" PRIu64,
				      query.counter.total_pkts,
				      query.counter.total_bytes);
		else
			DOCA_LOG_WARN("reinject_missed: counter query failed");
		/* Marked clones excluded. */
		if (ct_fw_query_entry(pipes->ct_miss_entry, &query))
			DOCA_LOG_INFO("ct_misses: pkts=%" PRIu64 " bytes=%" PRIu64,
				      query.counter.total_pkts,
				      query.counter.total_bytes);
		else
			DOCA_LOG_WARN("ct_misses: counter query failed");
		if (ct_fw_query_entry(pipes->ipv6_drop_entry, &query))
			DOCA_LOG_INFO("ipv6_dropped: pkts=%" PRIu64 " bytes=%" PRIu64,
				      query.counter.total_pkts,
				      query.counter.total_bytes);
		else if (pipes->ipv6_drop_entry != NULL)
			DOCA_LOG_WARN("ipv6_dropped: counter query failed");
		for (e = 0; e < pipes->nb_fwd_entries; e++) {
			if (pipes->fwd_entries[e] == NULL)
				continue;
			if (ct_fw_query_entry(pipes->fwd_entries[e], &query))
				DOCA_LOG_INFO("fast_path_forward[port %u]: pkts=%" PRIu64 " bytes=%" PRIu64,
					      e,
					      query.counter.total_pkts,
					      query.counter.total_bytes);
			else
				DOCA_LOG_WARN("fast_path_forward[port %u]: counter query failed", e);
		}
		for (e = 0; e < pipes->nb_egress_entries; e++) {
			if (pipes->egress_entries[e] == NULL)
				continue;
			if (ct_fw_query_entry(pipes->egress_entries[e], &query))
				DOCA_LOG_INFO("slow_path_forward[port %u]: pkts=%" PRIu64 " bytes=%" PRIu64,
					      e,
					      query.counter.total_pkts,
					      query.counter.total_bytes);
			else
				DOCA_LOG_WARN("slow_path_forward[port %u]: counter query failed", e);
		}
		/* Unknown ingress port, or a transmit with no destination. */
		{
			uint64_t fast = 0, to_sw = 0;

			if (ct_fw_query_miss(pipes->port_forward, &fast) && ct_fw_query_miss(pipes->rss_v4, &to_sw) &&
			    ct_fw_query_miss(pipes->rss_v6, &to_sw) &&
			    ct_fw_query_entry(pipes->egress_drop_entry, &query))
				DOCA_LOG_INFO("unknown_port_drops: fast_path=%" PRIu64 " to_software=%" PRIu64
					      " transmit=%" PRIu64,
					      fast,
					      to_sw,
					      query.counter.total_pkts);
			else
				DOCA_LOG_WARN("unknown_port_drops: counter query failed");
		}
	}

	{
		struct ct_fw_stats open;

		ct_fw_stats_open_at_exit(app, &open);
		DOCA_LOG_INFO("---------------------- Connections (hardware) ----------------------");
		DOCA_LOG_INFO("conn_totals[origin]: pkts=%" PRIu64 " bytes=%" PRIu64,
			      total.conn_origin_pkts,
			      total.conn_origin_bytes);
		DOCA_LOG_INFO("conn_totals[reply]: pkts=%" PRIu64 " bytes=%" PRIu64,
			      total.conn_reply_pkts,
			      total.conn_reply_bytes);
		DOCA_LOG_INFO("conn_totals_open_at_exit[origin]: pkts=%" PRIu64 " bytes=%" PRIu64,
			      open.conn_origin_pkts,
			      open.conn_origin_bytes);
		DOCA_LOG_INFO("conn_totals_open_at_exit[reply]: pkts=%" PRIu64 " bytes=%" PRIu64,
			      open.conn_reply_pkts,
			      open.conn_reply_bytes);
	}

	if (app->nb_workers > 1) {
		DOCA_LOG_INFO("---------------------- Per-Worker RX/TX ----------------------");
		for (i = 0; i < app->nb_workers; i++)
			DOCA_LOG_INFO("worker[%u]: rx_packets=%" PRIu64 " tx_packets=%" PRIu64,
				      app->workers[i].worker_id,
				      app->workers[i].stats.rx_packets,
				      app->workers[i].stats.tx_packets);
	}
	DOCA_LOG_INFO("=============================================================");
}

void ct_fw_stats_print_periodic(const struct ct_fw_ctx *app, uint64_t uptime_s)
{
	const struct ct_fw_pipes *pipes = &app->pipes;
	struct doca_flow_resource_query query;
	struct ct_fw_stats total;
	uint64_t wv[ARRAY_DIM(syndrome_labels)] = {0};
	uint64_t reinject_to_ct = 0;
	uint64_t reinject_missed = 0;
	bool query_failed = false;
	uint32_t e;

	ct_fw_stats_total(app, &total);
	for (e = 0; e < pipes->nb_syn_entries && e < ARRAY_DIM(wv); e++)
		if (ct_fw_query_live(pipes->syn_entries[e], &query, &query_failed))
			wv[e] = query.counter.total_pkts;

	/* Names differ from the shutdown summary. */
	DOCA_LOG_INFO("-------- CT Firewall live counters (uptime %" PRIu64 "s) --------", uptime_s);
	DOCA_LOG_INFO("rx=%" PRIu64 " tx=%" PRIu64 " pool-exhausted=%" PRIu64,
		      total.rx_packets,
		      total.tx_packets,
		      total.pool_exhausted);
	DOCA_LOG_INFO("connections: installed=%" PRIu64 " adopted=%" PRIu64 " threshold=%" PRIu64 " udp=%" PRIu64
		      " re-enabled=%" PRIu64 " aged=%" PRIu64 " sw-expired=%" PRIu64,
		      total.conns_installed,
		      total.conns_adopted,
		      total.conns_threshold_installed,
		      total.conns_udp_installed,
		      total.conns_reenabled,
		      total.conns_aged,
		      total.conns_sw_expired);
	DOCA_LOG_INFO("frag-drops=%" PRIu64 " frag-reassembled=%" PRIu64 " state-drops=%" PRIu64 " parse-drops=%" PRIu64
		      " other-forwarded=%" PRIu64 " nat-other-drops=%" PRIu64 " install-failures=%" PRIu64
		      " install-drops=%" PRIu64,
		      total.frag_drops,
		      total.frag_reassembled,
		      total.state_drops,
		      total.parse_drops,
		      total.other_forwarded,
		      total.nat_other_drops,
		      total.install_failures,
		      total.install_drops);
	DOCA_LOG_INFO("wrong-uplink-drops=%" PRIu64, total.wrong_uplink_drops);
	DOCA_LOG_INFO("reuse-drops=%" PRIu64, total.reuse_drops);
	DOCA_LOG_INFO("install-retries=%" PRIu64 " wv-limit-drops=%" PRIu64 " wv-limit-defers=%" PRIu64
		      " sweep-lagged=%" PRIu64,
		      total.install_retries,
		      total.wv_limit_drops,
		      total.wv_limit_defers,
		      total.sweep_lagged);
	DOCA_LOG_INFO("connection-totals: origin-pkts=%" PRIu64 " origin-bytes=%" PRIu64 " reply-pkts=%" PRIu64
		      " reply-bytes=%" PRIu64,
		      total.conn_origin_pkts,
		      total.conn_origin_bytes,
		      total.conn_reply_pkts,
		      total.conn_reply_bytes);
	DOCA_LOG_INFO("window-validation%s: valid=%" PRIu64 " state_change=%" PRIu64 " invalid=%" PRIu64
		      " disabled=%" PRIu64,
		      app->config.enable_window_validation ? "" : " (off)",
		      wv[0],
		      wv[1],
		      wv[2],
		      wv[3]);
	if (ct_fw_query_live(pipes->reinject_drop_entry, &query, &query_failed))
		reinject_to_ct = query.counter.total_pkts;
	if (ct_fw_query_live(pipes->miss_mark_drop_entry, &query, &query_failed))
		reinject_missed = query.counter.total_pkts;
	DOCA_LOG_INFO("reinject: emitted=%" PRIu64 " to_ct=%" PRIu64 " missed=%" PRIu64,
		      total.reinjections,
		      reinject_to_ct,
		      reinject_missed);
	for (e = 0; e < pipes->nb_fwd_entries; e++)
		if (ct_fw_query_live(pipes->fwd_entries[e], &query, &query_failed))
			DOCA_LOG_INFO("fast_path[port %u]=%" PRIu64, e, query.counter.total_pkts);
	for (e = 0; e < pipes->nb_egress_entries; e++)
		if (ct_fw_query_live(pipes->egress_entries[e], &query, &query_failed))
			DOCA_LOG_INFO("slow_path[port %u]=%" PRIu64, e, query.counter.total_pkts);
	if (query_failed)
		DOCA_LOG_WARN("live counters: a hardware counter query failed; its value shows as 0 or is omitted");
}
