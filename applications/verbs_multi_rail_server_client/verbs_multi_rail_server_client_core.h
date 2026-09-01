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

#ifndef _VERBS_MULTI_RAIL_SERVER_CLIENT_CORE_H_
#define _VERBS_MULTI_RAIL_SERVER_CLIENT_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <doca_error.h>

#define VERBS_MULTI_RAIL_MAX_PATH 512 /**< Maximum length of a JSON path argument */
#define VERBS_MULTI_RAIL_DEFAULT_CHUNKS_TOTAL \
	131072u /**< Default per-iteration payload budget when --chunks-total is omitted */
#define VERBS_MULTI_RAIL_TCP_PORT_DEFAULT 5000u /**< Default TCP control port for the OOB handshake */
#define VERBS_MULTI_RAIL_MAX_QPS 4096u		/**< Hard ceiling on --num-qps */

/**
 * @brief Traffic mode selected at startup; both sides must agree.
 *
 * SEND_RECV: server posts SENDs, client posts RECVs.
 * RDMA_WRITE: server writes directly into the client's MR using the rkey exchanged OOB.
 */
enum verbs_multi_rail_traffic {
	VERBS_MULTI_RAIL_TRAFFIC_SEND_RECV = 0,
	VERBS_MULTI_RAIL_TRAFFIC_RDMA_WRITE,
};

/**
 * @brief Per-QP attributes (one instance per QP).
 *
 * Loaded from the optional --qp-json file, which is a JSON array of length --num-qps:
 * one object per QP, so different QPs may use different attribute values. Omitted keys
 * in any element fall back to built-in defaults. Fields map directly onto
 * doca_verbs_qp_attr_set_* calls.
 */
struct verbs_multi_rail_qp_params {
	uint32_t qp_depth;   /**< Number of WRs the SQ and RQ can hold (also drives buf_bytes = chunks_total * qp_depth)
			      */
	uint16_t pkey_index; /**< IB partition key table index */
	uint16_t port_num;   /**< Local IB device port number (1-based) */
	uint16_t ack_timeout;	/**< QP ACK timeout exponent */
	uint16_t retry_cnt;	/**< Number of retries on a missing ACK */
	uint16_t rnr_retry;	/**< Number of retries on RNR NAK */
	uint16_t min_rnr_timer; /**< Minimum RNR NAK timer value */
	uint32_t gid_index;	/**< Local GID table index used to select source address */
};

/**
 * @brief Top-level application configuration populated by verbs_multi_rail_register_params() / doca_argp.
 *
 * Fields tagged "[handshake]" must match between the server and client at startup; the
 * OOB handshake compares them and aborts both sides with DOCA_ERROR_BAD_CONFIG on
 * mismatch. All other fields are per-side and may legitimately differ.
 */
struct verbs_multi_rail_cfg {
	uint32_t num_qps;				      /**< [handshake] Number of QPs to create */
	char qp_json_path[VERBS_MULTI_RAIL_MAX_PATH];	      /**< Optional path to per-QP attributes JSON */
	char dev_json_path[VERBS_MULTI_RAIL_MAX_PATH];	      /**< Optional path to device-mapping JSON */
	char multi_rail_json_path[VERBS_MULTI_RAIL_MAX_PATH]; /**< Optional path to doca_verbs_multi_rail_cfg JSON;
								 ignored unless use_multi_rail */
	bool qp_json_set;				      /**< Whether --qp-json was provided */
	bool dev_json_set;				      /**< Whether --dev-json was provided */
	bool multi_rail_json_set;			      /**< Whether --multi-rail-json was provided */
	size_t chunks_total; /**< [handshake] Per-iteration payload budget in bytes (split across active QPs each round;
				also the size of each QP's buffer slot) */
	enum verbs_multi_rail_traffic traffic; /**< [handshake] SEND_RECV or RDMA_WRITE */
	bool is_server;			       /**< True for server (sender), false for client */
	bool use_multi_rail;		       /**< [handshake] Enable DOCA Verbs Dynamic Flow orchestration */
	char server_ip[128];		       /**< Server IPv4 address (client-only; ignored on server) */
	uint16_t tcp_port;		       /**< TCP control port for OOB handshake */
};

/**
 * @brief Register all command-line parameters with doca_argp.
 *
 * Must be called after doca_argp_init() and before doca_argp_start(). The struct
 * verbs_multi_rail_cfg passed as the user-data pointer to doca_argp_start() is populated by
 * the registered callbacks.
 *
 * @return: DOCA_SUCCESS on success, or an error propagated from doca_argp_param_create() /
 *          doca_argp_register_param() on failure
 */
doca_error_t verbs_multi_rail_register_params(void);

/**
 * @brief Main lifecycle: connect, exchange QPs, run traffic until Ctrl-C, then tear down.
 *
 * The flow:
 *   1. Validate cfg, install SIGINT/SIGTERM handlers, allocate per-QP state.
 *   2. Open DOCA device(s), create CQs/QPs, register MRs.
 *   3. Open TCP control socket (server: accept; client: connect).
 *   4. Exchange handshake (must-match params: num_qps, chunks_total, traffic, use_multi_rail) and
 *      per-QP wire state. On mismatch the local side logs a specific per-field message
 *      and closes the TCP socket; the peer detects EOF on its own recv and aborts via
 *      DOCA_ERROR_CONNECTION_ABORTED. No application-level "mismatch" message is sent.
 *   5. Connect QPs to RTS, optionally bring up multi-rail probe-UC QPs, spawn the recovery thread.
 *   6. Run the selected traffic loop (server posts; client receives) until Ctrl-C arrives
 *      and the shutdown handshake completes on both sides.
 *   7. Tear down: join recovery thread, destroy QPs/MRs/CQs/AHs/PDs/contexts, destroy the
 *      doca_verbs_multi_rail / doca_verbs_multi_rail_cfg objects and the multi-rail mutex (when use_multi_rail), and
 *      free top-level arrays.
 *
 * @cfg [in]: Application configuration; must not be NULL. cfg->num_qps must satisfy
 *            1 <= num_qps <= VERBS_MULTI_RAIL_MAX_QPS. In client mode, cfg->server_ip must be
 *            a non-empty IPv4 address string.
 * @return: DOCA_SUCCESS on a clean shutdown, or one of: DOCA_ERROR_INVALID_VALUE
 *          (bad cfg), DOCA_ERROR_NO_MEMORY, DOCA_ERROR_NOT_FOUND (no DOCA device),
 *          DOCA_ERROR_CONNECTION_ABORTED (TCP), DOCA_ERROR_BAD_CONFIG (handshake
 *          mismatch), DOCA_ERROR_IO_FAILED (post_send/recv), DOCA_ERROR_DRIVER
 *          (verbs), DOCA_ERROR_INITIALIZATION (pthread_create), DOCA_ERROR_UNEXPECTED
 *          (unexpected WC opcode in the traffic loop, or unsupported IB link layer
 *          at device probe time), DOCA_ERROR_TIME_OUT (multi-rail probe timeout, surfaced
 *          via the recovery thread; use_multi_rail mode only), or any error propagated from
 *          doca_verbs_* setup
 */
doca_error_t verbs_multi_rail_run(struct verbs_multi_rail_cfg *cfg);

#endif /* _VERBS_MULTI_RAIL_SERVER_CLIENT_CORE_H_ */
