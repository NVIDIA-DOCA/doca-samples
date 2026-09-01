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

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sched.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

#include <infiniband/verbs.h>
#include <json-c/json.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_rdma_bridge.h>
#include <doca_verbs.h>
#include <doca_verbs_multi_rail.h>
#include <doca_verbs_bridge.h>

#include "verbs_multi_rail_server_client_core.h"

DOCA_LOG_REGISTER(VERBS_MULTI_RAIL_SERVER_CLIENT::CORE);

#define VERBS_MULTI_RAIL_DEFAULT_QP_DEPTH 128
#define VERBS_MULTI_RAIL_DEFAULT_PKEY_INDEX 0
#define VERBS_MULTI_RAIL_DEFAULT_PORT_NUM 1
#define VERBS_MULTI_RAIL_DEFAULT_ACK_TIMEOUT 14
#define VERBS_MULTI_RAIL_DEFAULT_RETRY_CNT 7
#define VERBS_MULTI_RAIL_DEFAULT_RNR_RETRY 7
#define VERBS_MULTI_RAIL_DEFAULT_MIN_RNR_TIMER 1
#define VERBS_MULTI_RAIL_DEFAULT_GID_INDEX 0

#define VERBS_MULTI_RAIL_MAGIC 0x56444631u
#define VERBS_MULTI_RAIL_HOP_LIMIT 255
#define VERBS_MULTI_RAIL_MIN_RQ_WR 4u
#define VERBS_MULTI_RAIL_MAX_SEND_SEGS 1u
#define VERBS_MULTI_RAIL_MAX_RECV_SEGS 1u
#define VERBS_MULTI_RAIL_BW_SAMPLE_NS 1000000000ull
#define VERBS_MULTI_RAIL_BW_CHECK_LOOPS 1024u
#define VERBS_MULTI_RAIL_RECOVERY_POLL_US 100000u
#define VERBS_MULTI_RAIL_CQ_POLL_BATCH 16
#define VERBS_MULTI_RAIL_PROBE_CQ_SIZE 256u
#define VERBS_MULTI_RAIL_PROBE_SQ_WR 128u
#define VERBS_MULTI_RAIL_PROBE_RQ_WR 128u
#define VERBS_MULTI_RAIL_MAX_POST_COUNT_GAP 1000

enum verbs_multi_rail_recovery_stage {
	STAGE_SENDER_FLUSH = 0,
	STAGE_SENDER_WAIT,
	STAGE_SENDER_RECOVERED,
	STAGE_RECEIVER_IGNORE,
	STAGE_RECEIVER_FLUSH,
	STAGE_RECEIVER_RECOVERED,
};

enum verbs_multi_rail_recovery_msg_type {
	VERBS_MULTI_RAIL_RECOVERY_MSG_QP_INACTIVE = 1,
	VERBS_MULTI_RAIL_RECOVERY_MSG_QP_RECOVERED = 2,
	VERBS_MULTI_RAIL_RECOVERY_MSG_SHUTDOWN = 3,
};

struct verbs_multi_rail_recovery_msg {
	uint32_t type; /* enum verbs_multi_rail_recovery_msg_type */
	uint32_t qp_index;
} __attribute__((packed));

struct verbs_multi_rail_qp_state_wire {
	uint64_t buf_va;
	uint64_t buf_bytes;
	uint32_t rkey;
	uint32_t qpn;
	uint8_t gid[DOCA_GID_BYTE_LENGTH];
	uint16_t lid;
	uint16_t reserved0;
} __attribute__((packed));

/*
 * Wire-format handshake exchanged once over TCP at startup, before any IB resources are
 * created. Carries the parameters that must match between server and client: any mismatch
 * causes IB-level errors (chunks_total buffer mismatch) or protocol deadlock (use_multi_rail probe-UC
 * exchange, traffic mode), so we reject the connection here with a specific log message.
 */
struct verbs_multi_rail_handshake {
	uint32_t magic;
	uint32_t num_qps;
	uint64_t chunks_total;
	uint32_t traffic;	 /* enum verbs_multi_rail_traffic */
	uint32_t use_multi_rail; /* bool, padded to 32 bits */
} __attribute__((packed));

struct verbs_multi_rail_dev_instance {
	char name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1];
	struct doca_verbs_context *verbs_ctx;
	struct doca_verbs_pd *verbs_pd;
	struct doca_dev *doca_dev;
	struct ibv_pd *ibv_pd;
	/* Per-device multi-rail probe path (UC); separate send/recv CQs as required by
	 * doca_verbs_multi_rail_set_probe_qp(). */
	struct doca_verbs_cq *probe_send_cq;
	struct doca_verbs_cq *probe_recv_cq;
	struct doca_verbs_qp *probe_uc_qp;
};

struct verbs_multi_rail_qp_state {
	struct verbs_multi_rail_qp_params cfg;
	struct verbs_multi_rail_dev_instance *dev_instance;

	struct doca_verbs_cq *cq;
	struct doca_verbs_qp *qp;
	struct doca_verbs_ah_attr *ah;

	enum doca_verbs_addr_type addr_type;
	uint16_t lid;
	struct doca_verbs_gid gid;

	void *buf;
	size_t buf_bytes;
	struct ibv_mr *mr;
	uint32_t rkey;
	uint32_t qpn;

	struct verbs_multi_rail_qp_state_wire remote;
	std::atomic<bool> active;
	enum verbs_multi_rail_recovery_stage recovery_stage;
	/*
	 * Monotonic-clock timestamp (ns) of the last inactive transition for this QP. Set
	 * by the runner when it observes the failing WC, read on the matching success WC
	 * to compute downtime. uint64_t subtraction is modulo 2^64, so the delta is correct
	 * even across the (practically unreachable) clock wrap-around.
	 */
	uint64_t t_inactive_ns;
};

static volatile sig_atomic_t verbs_multi_rail_force_quit;
static volatile sig_atomic_t verbs_multi_rail_local_shutdown_requested;
static std::atomic<bool> verbs_multi_rail_local_shutdown_sent;
static std::atomic<bool> verbs_multi_rail_peer_shutdown_received;
static doca_error_t recovery_thread_status;
static std::atomic<uint32_t> verbs_multi_rail_total_active_qp;

static void verbs_multi_rail_signal_handler(int signum)
{
	(void)signum;
	verbs_multi_rail_local_shutdown_requested = 1;
}

struct verbs_multi_rail_bw_samples {
	double *gbps;
	uint32_t count;
	uint32_t capacity;
	uint64_t last_sample_ns;
	uint64_t last_sample_bytes;
};

static uint64_t verbs_multi_rail_timespec_to_ns(const struct timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static doca_error_t verbs_multi_rail_bw_samples_append(struct verbs_multi_rail_bw_samples *samples, double gbps)
{
	double *new_arr;
	uint32_t new_cap;

	if (samples->count < samples->capacity) {
		samples->gbps[samples->count++] = gbps;
		return DOCA_SUCCESS;
	}

	new_cap = samples->capacity == 0 ? 16 : samples->capacity * 2;
	new_arr = static_cast<double *>(realloc(samples->gbps, sizeof(*new_arr) * new_cap));
	if (new_arr == NULL)
		return DOCA_ERROR_NO_MEMORY;

	samples->gbps = new_arr;
	samples->capacity = new_cap;
	samples->gbps[samples->count++] = gbps;
	return DOCA_SUCCESS;
}

static uint32_t seconds_time = 0;

static doca_error_t verbs_multi_rail_bw_samples_update(struct verbs_multi_rail_bw_samples *samples,
						       uint64_t total_bytes,
						       uint64_t now_ns)
{
	uint64_t elapsed_ns;
	double bytes_per_ns;

	if (now_ns <= samples->last_sample_ns)
		return DOCA_SUCCESS;

	elapsed_ns = now_ns - samples->last_sample_ns;
	if (elapsed_ns < VERBS_MULTI_RAIL_BW_SAMPLE_NS)
		return DOCA_SUCCESS;

	double diff_bytes = (double)(total_bytes - samples->last_sample_bytes);
	bytes_per_ns = diff_bytes / (double)elapsed_ns;
	while (elapsed_ns >= VERBS_MULTI_RAIL_BW_SAMPLE_NS) {
		uint64_t next_sample_bytes;
		double sec_bytes;
		doca_error_t err;

		next_sample_bytes =
			samples->last_sample_bytes + (uint64_t)(bytes_per_ns * VERBS_MULTI_RAIL_BW_SAMPLE_NS);
		sec_bytes = bytes_per_ns * VERBS_MULTI_RAIL_BW_SAMPLE_NS;
		err = verbs_multi_rail_bw_samples_append(samples, sec_bytes * 8.0 / 1e9);
		if (err != DOCA_SUCCESS)
			return err;

		DOCA_LOG_INFO("%u second: BW %.3f Gbps", seconds_time++, sec_bytes * 8.0 / 1e9);

		samples->last_sample_ns += VERBS_MULTI_RAIL_BW_SAMPLE_NS;
		samples->last_sample_bytes = next_sample_bytes;
		elapsed_ns = now_ns - samples->last_sample_ns;
	}

	return DOCA_SUCCESS;
}

static uint32_t verbs_multi_rail_split_wr_len(uint32_t num_active, uint64_t msg_total, bool is_first)
{
	uint64_t base = msg_total / (uint64_t)num_active;
	uint64_t split_rem = msg_total % (uint64_t)num_active;
	uint64_t len_u64;

	if (is_first)
		len_u64 = base + split_rem;
	else
		len_u64 = base;

	return (uint32_t)len_u64;
}

static void verbs_multi_rail_qp_params_defaults(struct verbs_multi_rail_qp_params *p)
{
	p->qp_depth = VERBS_MULTI_RAIL_DEFAULT_QP_DEPTH;
	p->pkey_index = VERBS_MULTI_RAIL_DEFAULT_PKEY_INDEX;
	p->port_num = VERBS_MULTI_RAIL_DEFAULT_PORT_NUM;
	p->ack_timeout = VERBS_MULTI_RAIL_DEFAULT_ACK_TIMEOUT;
	p->retry_cnt = VERBS_MULTI_RAIL_DEFAULT_RETRY_CNT;
	p->rnr_retry = VERBS_MULTI_RAIL_DEFAULT_RNR_RETRY;
	p->min_rnr_timer = VERBS_MULTI_RAIL_DEFAULT_MIN_RNR_TIMER;
	p->gid_index = VERBS_MULTI_RAIL_DEFAULT_GID_INDEX;
}

static doca_error_t recv_all(int fd, void *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = recv(fd, (char *)buf + off, len - off, 0);

		if (n == 0)
			return DOCA_ERROR_CONNECTION_ABORTED;
		if (n < 0)
			return DOCA_ERROR_CONNECTION_ABORTED;
		off += (size_t)n;
	}
	return DOCA_SUCCESS;
}

static doca_error_t send_all(int fd, const void *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = send(fd, (const char *)buf + off, len - off, 0);

		if (n < 0)
			return DOCA_ERROR_CONNECTION_ABORTED;
		off += (size_t)n;
	}
	return DOCA_SUCCESS;
}

struct verbs_multi_rail_recovery_thread_ctx {
	int sock_fd;
	uint32_t num_qps;
	bool is_sender;
	bool use_multi_rail;
	size_t chunks_total;
	enum verbs_multi_rail_traffic traffic;
	struct verbs_multi_rail_qp_state *qp_states;
	uint8_t rx_buf[sizeof(struct verbs_multi_rail_recovery_msg)];
	size_t rx_off;
	struct doca_verbs_multi_rail *verbs_multi_rail;
	pthread_mutex_t *verbs_multi_rail_mutex;
};

static void verbs_multi_rail_set_qp_inactive(struct verbs_multi_rail_qp_state *qp_state,
					     enum verbs_multi_rail_recovery_stage stage)
{
	bool was_active;

	qp_state->recovery_stage = stage;
	was_active = qp_state->active.exchange(false, std::memory_order_seq_cst);
	if (was_active)
		verbs_multi_rail_total_active_qp.fetch_sub(1, std::memory_order_acq_rel);
}

static doca_error_t verbs_multi_rail_send_recovery_msg(int sock_fd,
						       enum verbs_multi_rail_recovery_msg_type msg_type,
						       uint32_t qp_index)
{
	struct verbs_multi_rail_recovery_msg msg = {
		htonl((uint32_t)msg_type),
		htonl(qp_index),
	};

	return send_all(sock_fd, &msg, sizeof(msg));
}

static doca_error_t verbs_multi_rail_try_recv_recovery_msg(struct verbs_multi_rail_recovery_thread_ctx *ctx,
							   bool *has_msg,
							   struct verbs_multi_rail_recovery_msg *msg_out)
{
	struct verbs_multi_rail_recovery_msg wire_msg;
	ssize_t n;

	n = recv(ctx->sock_fd,
		 ctx->rx_buf + ctx->rx_off,
		 sizeof(struct verbs_multi_rail_recovery_msg) - ctx->rx_off,
		 MSG_DONTWAIT);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			*has_msg = false;
			return DOCA_SUCCESS;
		}
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	if (n == 0)
		return DOCA_ERROR_CONNECTION_ABORTED;

	ctx->rx_off += (size_t)n;
	if (ctx->rx_off < sizeof(struct verbs_multi_rail_recovery_msg)) {
		*has_msg = false;
		return DOCA_SUCCESS;
	}

	/* We copy the wire message to a local variable to avoid potential issues with aliasing */
	memcpy(&wire_msg, ctx->rx_buf, sizeof(wire_msg));
	ctx->rx_off = 0;

	msg_out->type = ntohl(wire_msg.type);
	msg_out->qp_index = ntohl(wire_msg.qp_index);
	*has_msg = true;
	return DOCA_SUCCESS;
}

static void verbs_multi_rail_drain_cq(struct doca_verbs_cq *cq)
{
	struct ibv_wc wc;

	while (doca_verbs_bridge_poll_cq(cq, 1, &wc) == 1)
		;
}

static doca_error_t json_get_u32(struct json_object *obj, const char *key, uint32_t *out)
{
	struct json_object *field;

	if (!json_object_object_get_ex(obj, key, &field))
		return DOCA_ERROR_NOT_FOUND;
	if (json_object_get_type(field) != json_type_int)
		return DOCA_ERROR_INVALID_VALUE;
	*out = (uint32_t)json_object_get_int(field);
	return DOCA_SUCCESS;
}

static doca_error_t json_get_u16(struct json_object *obj, const char *key, uint16_t *out)
{
	uint32_t v;
	doca_error_t err;

	err = json_get_u32(obj, key, &v);
	if (err != DOCA_SUCCESS)
		return err;
	if (v > UINT16_MAX)
		return DOCA_ERROR_INVALID_VALUE;
	*out = (uint16_t)v;
	return DOCA_SUCCESS;
}

static doca_error_t json_get_double(struct json_object *obj, const char *key, double *out)
{
	struct json_object *field;
	enum json_type t;

	if (!json_object_object_get_ex(obj, key, &field))
		return DOCA_ERROR_NOT_FOUND;
	t = json_object_get_type(field);
	if (t == json_type_double)
		*out = json_object_get_double(field);
	else if (t == json_type_int)
		*out = (double)json_object_get_int64(field);
	else
		return DOCA_ERROR_INVALID_VALUE;
	return DOCA_SUCCESS;
}

/*
 * Optional multi-rail cfg JSON keys map to doca_verbs_multi_rail_cfg_set_* (lb_callbacks is application-specific and
 * not loaded).
 */
static doca_error_t verbs_multi_rail_parse_lb_mode_field(struct json_object *obj,
							 enum doca_verbs_multi_rail_lb_mode *mode_out)
{
	struct json_object *field;
	const char *s;
	int v;

	if (!json_object_object_get_ex(obj, "lb_mode", &field))
		return DOCA_ERROR_NOT_FOUND;

	if (json_object_get_type(field) == json_type_int) {
		v = json_object_get_int(field);
		if (v < 0 || v > (int)DOCA_VERBS_MULTI_RAIL_LB_MODE_PORT_SPEED)
			return DOCA_ERROR_INVALID_VALUE;
		*mode_out = (enum doca_verbs_multi_rail_lb_mode)v;
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(field) != json_type_string)
		return DOCA_ERROR_INVALID_VALUE;
	s = json_object_get_string(field);
	if (strcasecmp(s, "measured_bw") == 0) {
		*mode_out = DOCA_VERBS_MULTI_RAIL_LB_MODE_MEASURED_BW;
		return DOCA_SUCCESS;
	}
	if (strcasecmp(s, "port_speed") == 0) {
		*mode_out = DOCA_VERBS_MULTI_RAIL_LB_MODE_PORT_SPEED;
		return DOCA_SUCCESS;
	}
	return DOCA_ERROR_INVALID_VALUE;
}

static doca_error_t verbs_multi_rail_apply_cfg_from_json(struct json_object *obj,
							 struct doca_verbs_multi_rail_cfg *verbs_multi_rail_cfg)
{
	doca_error_t err;
	double d;
	uint32_t u32;
	enum doca_verbs_multi_rail_lb_mode lb_mode;

	if (json_get_double(obj, "bw_decay_factor", &d) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_bw_decay_factor(verbs_multi_rail_cfg, d);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg bw_decay_factor: %s", doca_error_get_name(err));
			return err;
		}
	}
	if (json_get_double(obj, "max_bw_drop_factor", &d) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_max_bw_drop_factor(verbs_multi_rail_cfg, d);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg max_bw_drop_factor: %s", doca_error_get_name(err));
			return err;
		}
	}
	if (json_get_u32(obj, "min_chunk_size", &u32) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_min_chunk_size(verbs_multi_rail_cfg, u32);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg min_chunk_size: %s", doca_error_get_name(err));
			return err;
		}
	}
	if (json_get_u32(obj, "chunk_alignment", &u32) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_chunk_alignment(verbs_multi_rail_cfg, u32);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg chunk_alignment: %s", doca_error_get_name(err));
			return err;
		}
	}
	if (json_get_u32(obj, "min_splitable_message_size", &u32) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_min_splitable_message_size(verbs_multi_rail_cfg, u32);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg min_splitable_message_size: %s", doca_error_get_name(err));
			return err;
		}
	}
	err = verbs_multi_rail_parse_lb_mode_field(obj, &lb_mode);
	if (err == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_lb_mode(verbs_multi_rail_cfg, lb_mode);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg lb_mode: %s", doca_error_get_name(err));
			return err;
		}
	} else if (err != DOCA_ERROR_NOT_FOUND) {
		DOCA_LOG_ERR("multi-rail cfg lb_mode: invalid value");
		return err;
	}
	if (json_get_u32(obj, "probe_count", &u32) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_probe_count(verbs_multi_rail_cfg, u32);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg probe_count: %s", doca_error_get_name(err));
			return err;
		}
	}
	if (json_get_u32(obj, "probe_timeout_s", &u32) == DOCA_SUCCESS) {
		err = doca_verbs_multi_rail_cfg_set_probe_timeout_s(verbs_multi_rail_cfg, u32);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("multi-rail cfg probe_timeout_s: %s", doca_error_get_name(err));
			return err;
		}
	}
	return DOCA_SUCCESS;
}

static doca_error_t verbs_multi_rail_parse_cfg_json(const char *path,
						    struct doca_verbs_multi_rail_cfg *verbs_multi_rail_cfg)
{
	struct json_object *root;
	doca_error_t err = DOCA_SUCCESS;

	root = json_object_from_file(path);
	if (root == NULL) {
		DOCA_LOG_ERR("Failed to open or parse multi-rail config JSON '%s'", path);
		return DOCA_ERROR_IO_FAILED;
	}
	if (json_object_get_type(root) != json_type_object) {
		DOCA_LOG_ERR("multi-rail config JSON must be a single object");
		err = DOCA_ERROR_INVALID_VALUE;
		goto out;
	}
	err = verbs_multi_rail_apply_cfg_from_json(root, verbs_multi_rail_cfg);
out:
	json_object_put(root);
	return err;
}

static void merge_qp_obj_into_params(struct json_object *obj, struct verbs_multi_rail_qp_params *p)
{
	uint16_t u16;
	uint32_t u32;

	if (json_get_u32(obj, "qp_depth", &u32) == DOCA_SUCCESS)
		p->qp_depth = u32;
	if (json_get_u16(obj, "pkey_index", &u16) == DOCA_SUCCESS)
		p->pkey_index = u16;
	if (json_get_u16(obj, "port_num", &u16) == DOCA_SUCCESS)
		p->port_num = u16;
	if (json_get_u16(obj, "ack_timeout", &u16) == DOCA_SUCCESS)
		p->ack_timeout = u16;
	if (json_get_u16(obj, "retry_cnt", &u16) == DOCA_SUCCESS)
		p->retry_cnt = u16;
	if (json_get_u16(obj, "rnr_retry", &u16) == DOCA_SUCCESS)
		p->rnr_retry = u16;
	if (json_get_u16(obj, "min_rnr_timer", &u16) == DOCA_SUCCESS)
		p->min_rnr_timer = u16;
	if (json_get_u32(obj, "gid_index", &u32) == DOCA_SUCCESS)
		p->gid_index = u32;
}

static doca_error_t verbs_multi_rail_parse_qp_json(const char *path,
						   uint32_t num_qps,
						   struct verbs_multi_rail_qp_params *per_qp)
{
	struct json_object *root;
	doca_error_t err = DOCA_SUCCESS;
	uint32_t i;
	int alen;

	root = json_object_from_file(path);
	if (root == NULL) {
		DOCA_LOG_ERR("Failed to open or parse QP JSON file '%s'", path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (json_object_get_type(root) != json_type_array) {
		DOCA_LOG_ERR("QP JSON must be an array of one object per QP (length must equal --num-qps)");
		err = DOCA_ERROR_INVALID_VALUE;
		goto out;
	}

	alen = json_object_array_length(root);
	if (alen != (int)num_qps) {
		DOCA_LOG_ERR("QP JSON array length %d does not match --num-qps %" PRIu32, alen, num_qps);
		err = DOCA_ERROR_INVALID_VALUE;
		goto out;
	}

	for (i = 0; i < num_qps; i++) {
		struct json_object *elem = json_object_array_get_idx(root, (int)i);

		if (elem == NULL || json_object_get_type(elem) != json_type_object) {
			DOCA_LOG_ERR("QP JSON entry %u must be an object", i);
			err = DOCA_ERROR_INVALID_VALUE;
			goto out;
		}
		/* Start from built-in defaults so partial per-QP objects are well-defined. */
		verbs_multi_rail_qp_params_defaults(&per_qp[i]);
		merge_qp_obj_into_params(elem, &per_qp[i]);
	}

out:
	json_object_put(root);
	return err;
}

static doca_error_t verbs_multi_rail_pick_default_ibdev(char *out_name, size_t out_len)
{
	struct doca_devinfo **list = NULL;
	uint32_t nb = 0;
	doca_error_t status;

	status = doca_devinfo_create_list(&list, &nb);
	if (status != DOCA_SUCCESS || nb == 0 || list == NULL) {
		DOCA_LOG_ERR("No DOCA devices available");
		if (list != NULL)
			(void)doca_devinfo_destroy_list(list);
		return status != DOCA_SUCCESS ? status : DOCA_ERROR_NOT_FOUND;
	}
	status = doca_devinfo_get_ibdev_name(list[0], out_name, out_len - 1);
	(void)doca_devinfo_destroy_list(list);
	return status;
}

static doca_error_t verbs_multi_rail_parse_dev_json(const char *path,
						    uint32_t num_qps,
						    char (*names)[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1],
						    const char *default_dev)
{
	struct json_object *root;
	doca_error_t err = DOCA_SUCCESS;
	uint32_t i;

	for (i = 0; i < num_qps; i++)
		snprintf(names[i], DOCA_DEVINFO_IBDEV_NAME_SIZE + 1, "%s", default_dev);

	root = json_object_from_file(path);
	if (root == NULL) {
		DOCA_LOG_ERR("Failed to open or parse device map JSON '%s'", path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (json_object_get_type(root) == json_type_array) {
		int alen = json_object_array_length(root);

		if (alen != (int)num_qps) {
			DOCA_LOG_ERR("Device JSON array length %d is not equal to num_qps %" PRIu32, alen, num_qps);
			err = DOCA_ERROR_INVALID_VALUE;
			goto out;
		}
		for (i = 0; i < num_qps; i++) {
			struct json_object *elem = json_object_array_get_idx(root, (int)i);

			if (elem == NULL || json_object_get_type(elem) != json_type_string) {
				err = DOCA_ERROR_INVALID_VALUE;
				goto out;
			}
			strncpy(names[i], json_object_get_string(elem), DOCA_DEVINFO_IBDEV_NAME_SIZE);
			names[i][DOCA_DEVINFO_IBDEV_NAME_SIZE] = '\0';
		}
	} else {
		err = DOCA_ERROR_INVALID_VALUE;
	}

out:
	json_object_put(root);
	return err;
}

static doca_error_t open_verbs_context_for_ibdev(const char *device_name, struct doca_verbs_context **verbs_ctx_out)
{
	struct doca_devinfo **devinfo_list = NULL;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
	uint32_t nb_devs = 0;
	doca_error_t status;
	doca_error_t ctx_status = DOCA_SUCCESS;
	struct doca_verbs_context *verbs_ctx = NULL;

	status = doca_devinfo_create_list(&devinfo_list, &nb_devs);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create devinfo list: %s", doca_error_get_name(status));
		return status;
	}

	for (uint32_t i = 0; i < nb_devs; i++) {
		status = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, sizeof(ibdev_name) - 1);
		if (status == DOCA_SUCCESS && strcmp(ibdev_name, device_name) == 0) {
			ctx_status = doca_verbs_context_create(devinfo_list[i],
							       DOCA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
							       &verbs_ctx);
			if (ctx_status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to create verbs context: %s", doca_error_get_name(ctx_status));
			break;
		}
	}

	status = doca_devinfo_destroy_list(devinfo_list);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_name(status));
		if (verbs_ctx != NULL)
			(void)doca_verbs_context_destroy(verbs_ctx);
		return status;
	}

	if (ctx_status != DOCA_SUCCESS)
		return ctx_status;
	if (verbs_ctx == NULL) {
		DOCA_LOG_ERR("RDMA device '%s' was not found", device_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	*verbs_ctx_out = verbs_ctx;
	return DOCA_SUCCESS;
}

static doca_error_t create_verbs_cq(struct doca_verbs_context *verbs_ctx,
				    uint32_t cq_size,
				    struct doca_verbs_cq **cq_out)
{
	struct doca_verbs_cq_attr *attr = NULL;
	struct doca_verbs_cq *cq = NULL;
	doca_error_t status;
	doca_error_t t;

	status = doca_verbs_cq_attr_create(&attr);
	if (status != DOCA_SUCCESS)
		return status;
	status = doca_verbs_cq_attr_set_cq_size(attr, cq_size);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_cq_attr_set_external_datapath_en(attr, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_cq_attr_set_entry_size(attr, DOCA_VERBS_CQ_ENTRY_SIZE_64);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_cq_create(verbs_ctx, attr, &cq);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_cq_attr_destroy(attr);
	if (status != DOCA_SUCCESS)
		goto destroy_cq;
	*cq_out = cq;
	return DOCA_SUCCESS;

destroy_cq:
	t = doca_verbs_cq_destroy(cq);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy CQ: %s", doca_error_get_name(t));
destroy_attr:
	if (attr != NULL) {
		t = doca_verbs_cq_attr_destroy(attr);
		if (t != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy CQ attr: %s", doca_error_get_name(t));
	}
	return status;
}

static doca_error_t create_verbs_qp(struct doca_verbs_context *verbs_ctx,
				    struct doca_verbs_pd *pd,
				    struct doca_verbs_cq *cq,
				    uint32_t rq_wr,
				    uint32_t sq_wr,
				    struct doca_verbs_qp **qp_out)
{
	struct doca_verbs_qp_init_attr *init = NULL;
	struct doca_verbs_qp *qp = NULL;
	doca_error_t status;
	doca_error_t t;

	status = doca_verbs_qp_init_attr_create(&init);
	if (status != DOCA_SUCCESS)
		return status;
	status = doca_verbs_qp_init_attr_set_pd(init, pd);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_sq_wr(init, sq_wr);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_rq_wr(init, rq_wr);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_qp_type(init, DOCA_VERBS_QP_TYPE_RC);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_send_cq(init, cq);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_receive_cq(init, cq);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_external_datapath_en(init, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_send_max_sges(init, VERBS_MULTI_RAIL_MAX_SEND_SEGS);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_receive_max_sges(init, VERBS_MULTI_RAIL_MAX_RECV_SEGS);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_create(verbs_ctx, init, &qp);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_destroy(init);
	if (status != DOCA_SUCCESS)
		goto destroy_qp;
	*qp_out = qp;
	return DOCA_SUCCESS;

destroy_qp:
	t = doca_verbs_qp_destroy(qp);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy QP: %s", doca_error_get_name(t));
destroy_init:
	if (init != NULL) {
		t = doca_verbs_qp_init_attr_destroy(init);
		if (t != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy QP init attr: %s", doca_error_get_name(t));
	}
	return status;
}

static doca_error_t create_verbs_uc_qp(struct doca_verbs_context *verbs_ctx,
				       struct doca_verbs_pd *pd,
				       struct doca_verbs_cq *send_cq,
				       struct doca_verbs_cq *recv_cq,
				       uint32_t sq_wr,
				       uint32_t rq_wr,
				       struct doca_verbs_qp **qp_out)
{
	struct doca_verbs_qp_init_attr *init = NULL;
	struct doca_verbs_qp *qp = NULL;
	doca_error_t status;
	doca_error_t t;

	status = doca_verbs_qp_init_attr_create(&init);
	if (status != DOCA_SUCCESS)
		return status;
	status = doca_verbs_qp_init_attr_set_pd(init, pd);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_qp_type(init, DOCA_VERBS_QP_TYPE_UC);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_sq_wr(init, sq_wr);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_rq_wr(init, rq_wr);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_send_cq(init, send_cq);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_receive_cq(init, recv_cq);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_external_datapath_en(init, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_send_max_sges(init, VERBS_MULTI_RAIL_MAX_SEND_SEGS);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_receive_max_sges(init, VERBS_MULTI_RAIL_MAX_RECV_SEGS);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_max_inline_data(init, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_set_sq_sig_all(init, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_create(verbs_ctx, init, &qp);
	if (status != DOCA_SUCCESS)
		goto destroy_init;
	status = doca_verbs_qp_init_attr_destroy(init);
	if (status != DOCA_SUCCESS)
		goto destroy_qp;
	*qp_out = qp;
	return DOCA_SUCCESS;

destroy_qp:
	t = doca_verbs_qp_destroy(qp);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy UC QP: %s", doca_error_get_name(t));
destroy_init:
	if (init != NULL) {
		t = doca_verbs_qp_init_attr_destroy(init);
		if (t != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy QP init attr: %s", doca_error_get_name(t));
	}
	return status;
}

static doca_error_t create_ah_attr(struct doca_verbs_context *verbs_ctx,
				   uint32_t gid_index,
				   enum doca_verbs_addr_type addr_type,
				   struct doca_verbs_ah_attr **ah_out)
{
	doca_error_t status;
	doca_error_t t;
	struct doca_verbs_ah_attr *ah = NULL;

	status = doca_verbs_ah_attr_create(verbs_ctx, &ah);
	if (status != DOCA_SUCCESS)
		return status;
	status = doca_verbs_ah_attr_set_addr_type(ah, addr_type);
	if (status != DOCA_SUCCESS)
		goto err;
	status = doca_verbs_ah_attr_set_sgid_index(ah, gid_index);
	if (status != DOCA_SUCCESS)
		goto err;
	if (addr_type == DOCA_VERBS_ADDR_TYPE_IPv4) {
		status = doca_verbs_ah_attr_set_hop_limit(ah, VERBS_MULTI_RAIL_HOP_LIMIT);
		if (status != DOCA_SUCCESS)
			goto err;
	}
	*ah_out = ah;
	return DOCA_SUCCESS;
err:
	t = doca_verbs_ah_attr_destroy(ah);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy AH attr: %s", doca_error_get_name(t));
	return status;
}

static doca_error_t connect_qp_state_qp(struct verbs_multi_rail_qp_state *qp_state)
{
	struct doca_verbs_qp_attr *verbs_qp_attr = NULL;
	int qp_attr_mask = 0;
	doca_error_t status;
	doca_error_t t;

	if (qp_state->addr_type == DOCA_VERBS_ADDR_TYPE_IPv4) {
		struct doca_verbs_gid remote_gid;

		memcpy(remote_gid.raw, qp_state->remote.gid, sizeof(remote_gid.raw));
		status = doca_verbs_ah_attr_set_gid(qp_state->ah, remote_gid);
		if (status != DOCA_SUCCESS)
			return status;
	} else {
		status = doca_verbs_ah_attr_set_dlid(qp_state->ah, qp_state->remote.lid);
		if (status != DOCA_SUCCESS)
			return status;
	}

	status = doca_verbs_qp_attr_create(&verbs_qp_attr);
	if (status != DOCA_SUCCESS)
		return status;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_INIT);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_allow_remote_write(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_allow_remote_read(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_pkey_index(verbs_qp_attr, qp_state->cfg.pkey_index);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_port_num(verbs_qp_attr, qp_state->cfg.port_num);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_modify(qp_state->qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
					      DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_READ | DOCA_VERBS_QP_ATTR_PKEY_INDEX |
					      DOCA_VERBS_QP_ATTR_PORT_NUM);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTR);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_rq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_dest_qp_num(verbs_qp_attr, qp_state->remote.qpn);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_min_rnr_timer(verbs_qp_attr, qp_state->cfg.min_rnr_timer);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_path_mtu(verbs_qp_attr, DOCA_MTU_SIZE_1K_BYTES);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_ah_attr(verbs_qp_attr, qp_state->ah);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	qp_attr_mask |= DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN | DOCA_VERBS_QP_ATTR_DEST_QP_NUM |
			DOCA_VERBS_QP_ATTR_MIN_RNR_TIMER | DOCA_VERBS_QP_ATTR_PATH_MTU | DOCA_VERBS_QP_ATTR_AH_ATTR;

	status = doca_verbs_qp_modify(qp_state->qp, verbs_qp_attr, qp_attr_mask);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTS);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_sq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_ack_timeout(verbs_qp_attr, qp_state->cfg.ack_timeout);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_retry_cnt(verbs_qp_attr, qp_state->cfg.retry_cnt);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_rnr_retry(verbs_qp_attr, qp_state->cfg.rnr_retry);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_modify(qp_state->qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_SQ_PSN |
					      DOCA_VERBS_QP_ATTR_ACK_TIMEOUT | DOCA_VERBS_QP_ATTR_RETRY_CNT |
					      DOCA_VERBS_QP_ATTR_RNR_RETRY);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	return status;

destroy_attr:
	t = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy QP attr: %s", doca_error_get_name(t));
	return status;
}

/*
 * Bring UC probe QP to RTS. ref_qp must already be connected so its AH reflects the peer path.
 * Applied DOCA coding conventions.
 */
static doca_error_t connect_uc_qp_peer(struct doca_verbs_qp *uc_qp,
				       struct verbs_multi_rail_qp_state *ref_qp,
				       uint32_t remote_uc_qpn)
{
	struct doca_verbs_qp_attr *verbs_qp_attr = NULL;
	doca_error_t status;
	doca_error_t t;

	status = doca_verbs_qp_attr_create(&verbs_qp_attr);
	if (status != DOCA_SUCCESS)
		return status;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_INIT);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_pkey_index(verbs_qp_attr, ref_qp->cfg.pkey_index);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_port_num(verbs_qp_attr, ref_qp->cfg.port_num);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_allow_remote_write(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_modify(uc_qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_PKEY_INDEX |
					      DOCA_VERBS_QP_ATTR_PORT_NUM | DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTR);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_rq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_dest_qp_num(verbs_qp_attr, remote_uc_qpn);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_path_mtu(verbs_qp_attr, DOCA_MTU_SIZE_1K_BYTES);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_ah_attr(verbs_qp_attr, ref_qp->ah);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_modify(uc_qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN |
					      DOCA_VERBS_QP_ATTR_DEST_QP_NUM | DOCA_VERBS_QP_ATTR_PATH_MTU |
					      DOCA_VERBS_QP_ATTR_AH_ATTR);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTS);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_attr_set_sq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_modify(uc_qp, verbs_qp_attr, DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_SQ_PSN);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	return status;

destroy_attr:
	t = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy QP attr: %s", doca_error_get_name(t));
	return status;
}

static struct verbs_multi_rail_qp_state *verbs_multi_rail_first_qp_on_dev(struct verbs_multi_rail_qp_state *qp_states,
									  uint32_t num_qps,
									  struct verbs_multi_rail_dev_instance *dev)
{
	uint32_t q;

	for (q = 0; q < num_qps; q++) {
		if (qp_states[q].dev_instance == dev)
			return &qp_states[q];
	}
	return NULL;
}

static doca_error_t verbs_multi_rail_qp_move_to_err_or_rst(struct verbs_multi_rail_qp_state *qp_state,
							   enum doca_verbs_qp_state state)
{
	struct doca_verbs_qp_attr *verbs_qp_attr = NULL;
	doca_error_t status;
	doca_error_t t;

	status = doca_verbs_qp_attr_create(&verbs_qp_attr);
	if (status != DOCA_SUCCESS)
		return status;

	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, state);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;
	status = doca_verbs_qp_modify(qp_state->qp, verbs_qp_attr, DOCA_VERBS_QP_ATTR_NEXT_STATE);
	if (status != DOCA_SUCCESS)
		goto destroy_attr;

	status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	return status;

destroy_attr:
	t = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (t != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy QP attr: %s", doca_error_get_name(t));
	return status;
}

static doca_error_t verbs_multi_rail_recover_qp(struct verbs_multi_rail_qp_state *qp_state)
{
	doca_error_t status;

	status = verbs_multi_rail_qp_move_to_err_or_rst(qp_state, DOCA_VERBS_QP_STATE_ERR);
	if (status != DOCA_SUCCESS)
		return status;
	verbs_multi_rail_drain_cq(qp_state->cq);
	status = verbs_multi_rail_qp_move_to_err_or_rst(qp_state, DOCA_VERBS_QP_STATE_RST);
	if (status != DOCA_SUCCESS)
		return status;
	return connect_qp_state_qp(qp_state);
}

static doca_error_t verbs_multi_rail_prime_recv_qp(struct verbs_multi_rail_qp_state *qp_state, size_t chunks_total)
{
	uint32_t pre = 0;
	uint32_t depth = qp_state->cfg.qp_depth;

	while (pre < depth) {
		struct ibv_sge sge = {};
		struct ibv_recv_wr wr = {};
		struct ibv_recv_wr *bad_wr = NULL;
		uint64_t off = (uint64_t)pre * chunks_total;
		int ret;

		sge.addr = (uint64_t)(uintptr_t)((uint8_t *)qp_state->buf + off);
		sge.length = (uint32_t)chunks_total;
		sge.lkey = qp_state->mr->lkey;
		wr.wr_id = pre;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		ret = doca_verbs_bridge_post_recv(qp_state->qp, &wr, &bad_wr);
		if (ret != 0 || bad_wr != NULL)
			return DOCA_ERROR_IO_FAILED;
		pre++;
	}

	return DOCA_SUCCESS;
}

static doca_error_t verbs_multi_rail_handle_recovery_message(struct verbs_multi_rail_recovery_thread_ctx *ctx)
{
	struct verbs_multi_rail_recovery_msg msg;
	bool has_msg = false;
	doca_error_t status;

	status = verbs_multi_rail_try_recv_recovery_msg(ctx, &has_msg, &msg);
	if (status != DOCA_SUCCESS)
		return status;
	if (!has_msg)
		return DOCA_SUCCESS;

	if (msg.type == VERBS_MULTI_RAIL_RECOVERY_MSG_SHUTDOWN) {
		verbs_multi_rail_peer_shutdown_received.store(true, std::memory_order_release);
		/* Trigger a local SHUTDOWN reply on the next recovery-loop iteration if we haven't sent ours yet. */
		verbs_multi_rail_local_shutdown_requested = 1;
		return DOCA_SUCCESS;
	}

	if (msg.qp_index >= ctx->num_qps)
		return DOCA_ERROR_INVALID_VALUE;

	if (ctx->is_sender) {
		if (msg.type == VERBS_MULTI_RAIL_RECOVERY_MSG_QP_RECOVERED)
			ctx->qp_states[msg.qp_index].recovery_stage = STAGE_SENDER_RECOVERED;
	} else {
		if (msg.type == VERBS_MULTI_RAIL_RECOVERY_MSG_QP_INACTIVE) {
			verbs_multi_rail_set_qp_inactive(&ctx->qp_states[msg.qp_index], STAGE_RECEIVER_FLUSH);
			if (ctx->use_multi_rail) {
				pthread_mutex_lock(ctx->verbs_multi_rail_mutex);
				status = doca_verbs_multi_rail_update_qp(ctx->verbs_multi_rail,
									 DOCA_VERBS_MULTI_RAIL_QP_OP_REMOVE,
									 msg.qp_index,
									 NULL);
				pthread_mutex_unlock(ctx->verbs_multi_rail_mutex);
				if (status != DOCA_SUCCESS) {
					DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp REMOVE failed for QP %u: %s",
						     msg.qp_index,
						     doca_error_get_name(status));
					return status;
				}
			}
		}
	}

	return DOCA_SUCCESS;
}

static doca_error_t verbs_multi_rail_process_qp_recovery(const struct verbs_multi_rail_recovery_thread_ctx *ctx,
							 uint32_t q)
{
	struct verbs_multi_rail_qp_state *qp_state = &ctx->qp_states[q];
	doca_error_t status;

	if (qp_state->active.load(std::memory_order_acquire))
		return DOCA_SUCCESS;
	switch (qp_state->recovery_stage) {
	case STAGE_SENDER_FLUSH:
		status = verbs_multi_rail_recover_qp(qp_state);
		if (status != DOCA_SUCCESS)
			return status;
		if (ctx->use_multi_rail) {
			status = doca_verbs_multi_rail_probe_start(ctx->verbs_multi_rail, q);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_probe_start failed for QP %u: %s",
					     q,
					     doca_error_get_name(status));
				return status;
			}
		}
		status = verbs_multi_rail_send_recovery_msg(ctx->sock_fd, VERBS_MULTI_RAIL_RECOVERY_MSG_QP_INACTIVE, q);
		if (status != DOCA_SUCCESS)
			return status;
		qp_state->recovery_stage = STAGE_SENDER_WAIT;
		break;
	case STAGE_SENDER_RECOVERED:
		if (ctx->use_multi_rail) {
			enum doca_verbs_multi_rail_probe_status probe_status;

			status = doca_verbs_multi_rail_probe_get_status(ctx->verbs_multi_rail, q, &probe_status);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_probe_get_status failed for QP %u: %s",
					     q,
					     doca_error_get_name(status));
				return status;
			}
			if (probe_status == DOCA_VERBS_MULTI_RAIL_PROBE_TIMEOUT) {
				DOCA_LOG_ERR("Probe timed out for QP %u", q);
				return DOCA_ERROR_TIME_OUT;
			}
			if (probe_status != DOCA_VERBS_MULTI_RAIL_PROBE_RECOVERED) {
				/* Probe still in progress; retry on the next recovery-loop iteration. */
				break;
			}
		}
		qp_state->active.store(true, std::memory_order_release);
		verbs_multi_rail_total_active_qp.fetch_add(1, std::memory_order_release);
		if (ctx->use_multi_rail) {
			pthread_mutex_lock(ctx->verbs_multi_rail_mutex);
			status = doca_verbs_multi_rail_update_qp(ctx->verbs_multi_rail,
								 DOCA_VERBS_MULTI_RAIL_QP_OP_RECOVER,
								 q,
								 qp_state->qp);
			pthread_mutex_unlock(ctx->verbs_multi_rail_mutex);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp RECOVER failed for QP %u: %s",
					     q,
					     doca_error_get_name(status));
				return status;
			}
		}
		break;
	case STAGE_RECEIVER_FLUSH:
		status = verbs_multi_rail_recover_qp(qp_state);
		if (status != DOCA_SUCCESS)
			return status;
		if (ctx->traffic == VERBS_MULTI_RAIL_TRAFFIC_SEND_RECV) {
			status = verbs_multi_rail_prime_recv_qp(qp_state, ctx->chunks_total);
			if (status != DOCA_SUCCESS)
				return status;
		}
		if (ctx->use_multi_rail) {
			status = doca_verbs_multi_rail_probe_start(ctx->verbs_multi_rail, q);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_probe_start failed for QP %u: %s",
					     q,
					     doca_error_get_name(status));
				return status;
			}
		}
		status =
			verbs_multi_rail_send_recovery_msg(ctx->sock_fd, VERBS_MULTI_RAIL_RECOVERY_MSG_QP_RECOVERED, q);
		if (status != DOCA_SUCCESS)
			return status;
		qp_state->recovery_stage = STAGE_RECEIVER_RECOVERED;
		break;
	case STAGE_RECEIVER_RECOVERED:
		if (ctx->use_multi_rail) {
			enum doca_verbs_multi_rail_probe_status probe_status;

			status = doca_verbs_multi_rail_probe_get_status(ctx->verbs_multi_rail, q, &probe_status);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_probe_get_status failed for QP %u: %s",
					     q,
					     doca_error_get_name(status));
				return status;
			}
			if (probe_status == DOCA_VERBS_MULTI_RAIL_PROBE_TIMEOUT) {
				DOCA_LOG_ERR("Probe timed out for QP %u", q);
				return DOCA_ERROR_TIME_OUT;
			}
			if (probe_status != DOCA_VERBS_MULTI_RAIL_PROBE_RECOVERED) {
				/* Probe still in progress; retry on the next recovery-loop iteration. */
				break;
			}
		}
		qp_state->active.store(true, std::memory_order_release);
		verbs_multi_rail_total_active_qp.fetch_add(1, std::memory_order_release);
		if (ctx->use_multi_rail) {
			pthread_mutex_lock(ctx->verbs_multi_rail_mutex);
			status = doca_verbs_multi_rail_update_qp(ctx->verbs_multi_rail,
								 DOCA_VERBS_MULTI_RAIL_QP_OP_RECOVER,
								 q,
								 qp_state->qp);
			pthread_mutex_unlock(ctx->verbs_multi_rail_mutex);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp RECOVER failed for QP %u: %s",
					     q,
					     doca_error_get_name(status));
				return status;
			}
		}
		break;
	default:
		/* Nothing to do for other stages */
		break;
	}

	return DOCA_SUCCESS;
}

static void *verbs_multi_rail_recovery_thread_main(void *arg)
{
	struct verbs_multi_rail_recovery_thread_ctx *ctx = (struct verbs_multi_rail_recovery_thread_ctx *)arg;

	while (!verbs_multi_rail_force_quit) {
		doca_error_t status;
		uint32_t q;

		status = verbs_multi_rail_handle_recovery_message(ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Recovery socket handling failed: %s", doca_error_get_name(status));
			recovery_thread_status = status;
			verbs_multi_rail_force_quit = 1;
			break;
		}

		for (q = 0; q < ctx->num_qps; q++) {
			status = verbs_multi_rail_process_qp_recovery(ctx, q);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Recovery flow failed for QP %u: %s", q, doca_error_get_name(status));
				recovery_thread_status = status;
				verbs_multi_rail_force_quit = 1;
				break;
			}
		}

		/*
		 * Shutdown handshake: when our local request is set (by SIGINT or by receiving a peer
		 * SHUTDOWN), send our SHUTDOWN once. The data path keeps running until both sides have
		 * exchanged SHUTDOWN, at which point force_quit ends the loops and the existing
		 * teardown path runs.
		 */
		if (verbs_multi_rail_local_shutdown_requested &&
		    !verbs_multi_rail_local_shutdown_sent.load(std::memory_order_acquire)) {
			status = verbs_multi_rail_send_recovery_msg(ctx->sock_fd,
								    VERBS_MULTI_RAIL_RECOVERY_MSG_SHUTDOWN,
								    0);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_WARN("Failed to send SHUTDOWN to peer (likely disconnected): %s",
					      doca_error_get_name(status));
				verbs_multi_rail_force_quit = 1;
			} else {
				DOCA_LOG_INFO("Sent SHUTDOWN to peer");
				verbs_multi_rail_local_shutdown_sent.store(true, std::memory_order_release);
			}
		}
		if (verbs_multi_rail_local_shutdown_sent.load(std::memory_order_acquire) &&
		    verbs_multi_rail_peer_shutdown_received.load(std::memory_order_acquire)) {
			DOCA_LOG_INFO("Shutdown handshake complete; stopping data path");
			verbs_multi_rail_force_quit = 1;
		}

		usleep(VERBS_MULTI_RAIL_RECOVERY_POLL_US);
	}

	return NULL;
}

static const char *verbs_multi_rail_traffic_str(uint32_t t)
{
	switch (t) {
	case VERBS_MULTI_RAIL_TRAFFIC_SEND_RECV:
		return "send_recv";
	case VERBS_MULTI_RAIL_TRAFFIC_RDMA_WRITE:
		return "rdma_write";
	default:
		return "unknown";
	}
}

/*
 * Validate that the peer's must-match parameters agree with ours. Logs a specific message
 * for each diverging field and returns DOCA_ERROR_BAD_CONFIG on any mismatch. The caller
 * then proceeds through the normal cleanup chain, which closes the TCP socket and lets the
 * peer detect EOF on its own recv -- so both sides shut down gracefully.
 */
static doca_error_t verbs_multi_rail_check_peer_params(const struct verbs_multi_rail_handshake *local,
						       const struct verbs_multi_rail_handshake *peer,
						       const char *peer_role)
{
	bool ok = true;

	if (peer->magic != VERBS_MULTI_RAIL_MAGIC) {
		DOCA_LOG_ERR("Handshake magic mismatch from %s: expected 0x%08x, got 0x%08x",
			     peer_role,
			     VERBS_MULTI_RAIL_MAGIC,
			     peer->magic);
		ok = false;
	}
	if (peer->num_qps != local->num_qps) {
		DOCA_LOG_ERR("Handshake parameter mismatch from %s: num_qps local=%u peer=%u",
			     peer_role,
			     local->num_qps,
			     peer->num_qps);
		ok = false;
	}
	if (peer->chunks_total != local->chunks_total) {
		DOCA_LOG_ERR("Handshake parameter mismatch from %s: chunks_total local=%" PRIu64 " peer=%" PRIu64,
			     peer_role,
			     local->chunks_total,
			     peer->chunks_total);
		ok = false;
	}
	if (peer->traffic != local->traffic) {
		DOCA_LOG_ERR("Handshake parameter mismatch from %s: traffic local=%s peer=%s",
			     peer_role,
			     verbs_multi_rail_traffic_str(local->traffic),
			     verbs_multi_rail_traffic_str(peer->traffic));
		ok = false;
	}
	if (peer->use_multi_rail != local->use_multi_rail) {
		DOCA_LOG_ERR("Handshake parameter mismatch from %s: use_multi_rail local=%s peer=%s",
			     peer_role,
			     local->use_multi_rail ? "true" : "false",
			     peer->use_multi_rail ? "true" : "false");
		ok = false;
	}
	if (!ok) {
		DOCA_LOG_ERR(
			"Closing connection due to parameter mismatch -- ensure server and client are launched with matching --num-qps, --chunks-total, --traffic, and --use-multi-rail");
		return DOCA_ERROR_BAD_CONFIG;
	}
	return DOCA_SUCCESS;
}

static doca_error_t exchange_oob(int sock_fd,
				 const struct verbs_multi_rail_cfg *cfg,
				 struct verbs_multi_rail_qp_state *qp_states)
{
	struct verbs_multi_rail_handshake hs = {
		VERBS_MULTI_RAIL_MAGIC,
		cfg->num_qps,
		(uint64_t)cfg->chunks_total,
		(uint32_t)cfg->traffic,
		cfg->use_multi_rail ? 1u : 0u,
	};
	struct verbs_multi_rail_handshake peer = {};
	const char *peer_role = cfg->is_server ? "client" : "server";
	doca_error_t status;
	uint32_t i;

	if (cfg->is_server) {
		status = recv_all(sock_fd, &peer, sizeof(peer));
		if (status != DOCA_SUCCESS)
			return status;
		status = verbs_multi_rail_check_peer_params(&hs, &peer, peer_role);
		if (status != DOCA_SUCCESS)
			return status;
		status = send_all(sock_fd, &hs, sizeof(hs));
		if (status != DOCA_SUCCESS)
			return status;
	} else {
		status = send_all(sock_fd, &hs, sizeof(hs));
		if (status != DOCA_SUCCESS)
			return status;
		status = recv_all(sock_fd, &peer, sizeof(peer));
		if (status != DOCA_SUCCESS)
			return status;
		status = verbs_multi_rail_check_peer_params(&hs, &peer, peer_role);
		if (status != DOCA_SUCCESS)
			return status;
	}

	for (i = 0; i < cfg->num_qps; i++) {
		struct verbs_multi_rail_qp_state_wire local_wire = {};

		local_wire.buf_va = htobe64((uint64_t)(uintptr_t)qp_states[i].buf);
		local_wire.buf_bytes = htobe64((uint64_t)qp_states[i].buf_bytes);
		local_wire.rkey = htonl(qp_states[i].rkey);
		local_wire.qpn = htonl(qp_states[i].qpn);
		memcpy(local_wire.gid, qp_states[i].gid.raw, sizeof(local_wire.gid));
		local_wire.lid = htons(qp_states[i].lid);

		if (cfg->is_server) {
			status = recv_all(sock_fd, &qp_states[i].remote, sizeof(qp_states[i].remote));
			if (status != DOCA_SUCCESS)
				return status;
			status = send_all(sock_fd, &local_wire, sizeof(local_wire));
			if (status != DOCA_SUCCESS)
				return status;
		} else {
			status = send_all(sock_fd, &local_wire, sizeof(local_wire));
			if (status != DOCA_SUCCESS)
				return status;
			status = recv_all(sock_fd, &qp_states[i].remote, sizeof(qp_states[i].remote));
			if (status != DOCA_SUCCESS)
				return status;
		}

		qp_states[i].remote.buf_va = be64toh(qp_states[i].remote.buf_va);
		qp_states[i].remote.buf_bytes = be64toh(qp_states[i].remote.buf_bytes);
		qp_states[i].remote.rkey = ntohl(qp_states[i].remote.rkey);
		qp_states[i].remote.qpn = ntohl(qp_states[i].remote.qpn);
		qp_states[i].remote.lid = ntohs(qp_states[i].remote.lid);
	}

	return DOCA_SUCCESS;
}

static doca_error_t exchange_probe_uc_oob(int sock_fd,
					  uint32_t nb_dev,
					  uint32_t *local_uc_qpns,
					  uint32_t *peer_uc_qpns_out,
					  bool is_server)
{
	doca_error_t status;
	uint32_t i;

	for (i = 0; i < nb_dev; i++) {
		if (is_server) {
			status = recv_all(sock_fd, &peer_uc_qpns_out[i], sizeof(peer_uc_qpns_out[i]));
			if (status != DOCA_SUCCESS)
				return status;
			status = send_all(sock_fd, &local_uc_qpns[i], sizeof(local_uc_qpns[i]));
			if (status != DOCA_SUCCESS)
				return status;
		} else {
			status = send_all(sock_fd, &local_uc_qpns[i], sizeof(local_uc_qpns[i]));
			if (status != DOCA_SUCCESS)
				return status;
			status = recv_all(sock_fd, &peer_uc_qpns_out[i], sizeof(peer_uc_qpns_out[i]));
			if (status != DOCA_SUCCESS)
				return status;
		}
	}

	return DOCA_SUCCESS;
}

static doca_error_t tcp_server_accept(uint16_t port, int *listen_fd_out, int *conn_fd_out)
{
	struct sockaddr_in addr = {};
	int listen_fd;
	int conn_fd;
	int opt = 1;

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		return DOCA_ERROR_CONNECTION_ABORTED;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
		goto err_listen;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
		goto err_listen;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto err_listen;
	if (listen(listen_fd, 1) < 0)
		goto err_listen;
	conn_fd = accept(listen_fd, NULL, NULL);
	if (conn_fd < 0)
		goto err_listen;
	*listen_fd_out = listen_fd;
	*conn_fd_out = conn_fd;
	return DOCA_SUCCESS;

err_listen:
	close(listen_fd);
	return DOCA_ERROR_CONNECTION_ABORTED;
}

static doca_error_t tcp_client_connect(const char *server_ip, uint16_t port, int *conn_fd_out)
{
	struct sockaddr_in addr = {};
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return DOCA_ERROR_CONNECTION_ABORTED;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
		close(fd);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	*conn_fd_out = fd;
	return DOCA_SUCCESS;
}

static doca_error_t run_server_send_recv(struct verbs_multi_rail_cfg *cfg,
					 struct verbs_multi_rail_qp_state *qp_states,
					 double *bw_gbps_out,
					 struct doca_verbs_multi_rail *verbs_multi_rail,
					 pthread_mutex_t *verbs_multi_rail_mutex)
{
	uint64_t *posted_pkts = NULL;
	uint64_t *completed_pkts = NULL;
	uint64_t *chunks_split = NULL;
	uint64_t *values = NULL;     /* per-QP cumulative posted-count priority */
	bool *in_queue = NULL;	     /* per-QP membership of the scheduling set */
	uint64_t values_sum = 0;     /* sum of values[q] over in-queue QPs (refreshed each loop) */
	uint64_t min_value = 0;	     /* min of values[q] over in-queue QPs (refreshed each loop) */
	uint32_t in_queue_count = 0; /* number of in-queue QPs (refreshed each loop) */
	uint64_t total_completed_bytes = 0;
	struct timespec t0, t1;
	struct verbs_multi_rail_bw_samples samples = {};
	uint32_t check_loop = 0;
	size_t chunks_total = cfg->chunks_total;
	uint32_t q;
	bool is_recoverable = false;
	doca_error_t err = DOCA_SUCCESS;

	posted_pkts = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	completed_pkts = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	chunks_split = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	values = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	in_queue = static_cast<bool *>(calloc(cfg->num_qps, sizeof(bool)));
	if (posted_pkts == NULL || completed_pkts == NULL || chunks_split == NULL || values == NULL ||
	    in_queue == NULL) {
		err = DOCA_ERROR_NO_MEMORY;
		goto out;
	}

	for (q = 0; q < cfg->num_qps; q++) {
		values[q] = 0;
		in_queue[q] = true;
		if (cfg->use_multi_rail) {
			pthread_mutex_lock(verbs_multi_rail_mutex);
			err = doca_verbs_multi_rail_update_qp(verbs_multi_rail,
							      DOCA_VERBS_MULTI_RAIL_QP_OP_ADD,
							      q,
							      qp_states[q].qp);
			if (err != DOCA_SUCCESS) {
				pthread_mutex_unlock(verbs_multi_rail_mutex);
				DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp ADD failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				goto out;
			}
			err = doca_verbs_multi_rail_set_probe_qp(verbs_multi_rail,
								 q,
								 qp_states[q].dev_instance->probe_uc_qp,
								 cfg->is_server ? 1 : 0);
			pthread_mutex_unlock(verbs_multi_rail_mutex);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_set_probe_qp failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				goto out;
			}
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
		err = DOCA_ERROR_DRIVER;
		goto out;
	}
	samples.last_sample_ns = verbs_multi_rail_timespec_to_ns(&t0);
	samples.last_sample_bytes = 0.0;
	DOCA_LOG_INFO("Server send/recv traffic started. Press Ctrl-C to stop and print average BW.");

	while (!verbs_multi_rail_force_quit) {
		uint32_t num_active;

		if ((check_loop++ % VERBS_MULTI_RAIL_BW_CHECK_LOOPS) == 0) {
			struct timespec now_ts;

			if (clock_gettime(CLOCK_MONOTONIC, &now_ts) != 0) {
				err = DOCA_ERROR_DRIVER;
				goto out;
			}
			err = verbs_multi_rail_bw_samples_update(&samples,
								 total_completed_bytes,
								 verbs_multi_rail_timespec_to_ns(&now_ts));
			if (err != DOCA_SUCCESS)
				goto out;
		}

		num_active = verbs_multi_rail_total_active_qp.load(std::memory_order_acquire);
		if (num_active == 0)
			continue;

		/* Poll completions first: drain each active QP CQ in bursts until empty */
		for (q = 0; q < cfg->num_qps; q++) {
			struct ibv_wc wc_batch[VERBS_MULTI_RAIL_CQ_POLL_BATCH];
			bool cq_error = false;

			if (!qp_states[q].active.load(std::memory_order_acquire))
				continue;

			for (;;) {
				int n;
				int i;

				if (verbs_multi_rail_force_quit)
					goto finalize_send_recv_stats;

				n = doca_verbs_bridge_poll_cq(qp_states[q].cq,
							      VERBS_MULTI_RAIL_CQ_POLL_BATCH,
							      wc_batch);
				if (n < 0) {
					DOCA_LOG_ERR("poll_cq failed on QP %u", q);
					err = DOCA_ERROR_IO_FAILED;
					goto out;
				}
				if (n == 0)
					break;

				for (i = 0; i < n; i++) {
					struct ibv_wc *wc = &wc_batch[i];

					if (wc->status != IBV_WC_SUCCESS) {
						err = doca_verbs_multi_rail_error_is_recoverable(wc->status,
												 &is_recoverable);
						if (err != DOCA_SUCCESS) {
							DOCA_LOG_ERR(
								"doca_verbs_multi_rail_error_is_recoverable failed: %s",
								doca_error_get_name(err));
							goto out;
						}

						DOCA_LOG_ERR(
							"Send CQ error: %s, Found an error WC in QP#%u, is_recoverable: %d",
							ibv_wc_status_str(wc->status),
							q,
							is_recoverable);

						struct timespec t_evt;
						uint64_t t_evt_ns;

						if (clock_gettime(CLOCK_MONOTONIC, &t_evt) != 0) {
							err = DOCA_ERROR_DRIVER;
							goto out;
						}
						t_evt_ns = verbs_multi_rail_timespec_to_ns(&t_evt);
						verbs_multi_rail_set_qp_inactive(&qp_states[q], STAGE_SENDER_FLUSH);
						posted_pkts[q] = completed_pkts[q];
						cq_error = true;
						if (in_queue[q]) {
							qp_states[q].t_inactive_ns = t_evt_ns;
							DOCA_LOG_INFO("Received an error WC for QP#%u at %" PRIu64
								      " ns, marking as inactive",
								      q,
								      t_evt_ns);
							in_queue[q] = false;
						}
						if (cfg->use_multi_rail) {
							pthread_mutex_lock(verbs_multi_rail_mutex);
							err = doca_verbs_multi_rail_update_qp(
								verbs_multi_rail,
								DOCA_VERBS_MULTI_RAIL_QP_OP_REMOVE,
								q,
								NULL);
							pthread_mutex_unlock(verbs_multi_rail_mutex);
							if (err != DOCA_SUCCESS) {
								DOCA_LOG_ERR(
									"doca_verbs_multi_rail_update_qp REMOVE failed for QP %u: %s",
									q,
									doca_error_get_name(err));
								goto out;
							}
						}
						break;
					}
					if (wc->opcode != IBV_WC_SEND) {
						DOCA_LOG_ERR("Unexpected WC opcode on server");
						err = DOCA_ERROR_UNEXPECTED;
						goto out;
					}
					if (cfg->use_multi_rail) {
						pthread_mutex_lock(verbs_multi_rail_mutex);
						err = doca_verbs_multi_rail_update(verbs_multi_rail,
										   DOCA_VERBS_MULTI_RAIL_UPDATE_COMP,
										   q,
										   wc->wr_id,
										   0);
						pthread_mutex_unlock(verbs_multi_rail_mutex);
						if (err != DOCA_SUCCESS) {
							DOCA_LOG_ERR(
								"doca_verbs_multi_rail_update COMP failed for QP %u: %s",
								q,
								doca_error_get_name(err));
							goto out;
						}
					}
					completed_pkts[q]++;
					total_completed_bytes += (uint64_t)wc->wr_id;
				}
				if (!cq_error && !in_queue[q]) {
					struct timespec t_evt;
					uint64_t t_evt_ns;
					/*
					 * Downtime: uint64_t subtraction is well-defined modulo 2^64, so the
					 * result is correct even if the monotonic clock were to wrap between
					 * the inactive and the recovery event (in practice CLOCK_MONOTONIC
					 * ns spans > 500 years before wrapping).
					 */
					uint64_t downtime_ns;

					if (clock_gettime(CLOCK_MONOTONIC, &t_evt) != 0) {
						err = DOCA_ERROR_DRIVER;
						goto out;
					}
					t_evt_ns = verbs_multi_rail_timespec_to_ns(&t_evt);
					downtime_ns = t_evt_ns - qp_states[q].t_inactive_ns;
					DOCA_LOG_INFO("Received success WC for QP#%u at %" PRIu64
						      " ns, marking as active (downtime: %" PRIu64 " ns)",
						      q,
						      t_evt_ns,
						      downtime_ns);
					/* Re-admit at the fair average of the QPs currently in the scheduling set. */
					values[q] = in_queue_count != 0 ? values_sum / in_queue_count : 0;
					in_queue[q] = true;
				}
				if (cq_error)
					break;
			}
		}

		/*
		 * Per-QP scheduling snapshot (sum/min/count over in-queue QPs). It is accumulated during the
		 * post loop below and committed at its end, so each iteration schedules using the values
		 * tracked over the previous iteration instead of recomputing them in a separate pass.
		 */
		uint64_t next_values_sum = 0;
		uint64_t next_min_value = UINT64_MAX;
		uint32_t next_in_queue_count = 0;
		bool is_first_post = true;

		num_active = verbs_multi_rail_total_active_qp.load(std::memory_order_acquire);
		if (num_active == 0)
			continue;
		if (cfg->use_multi_rail) {
			pthread_mutex_lock(verbs_multi_rail_mutex);
			err = doca_verbs_multi_rail_get_post_split(verbs_multi_rail, chunks_total, chunks_split);
			pthread_mutex_unlock(verbs_multi_rail_mutex);
			if (err == DOCA_ERROR_EMPTY) {
				DOCA_LOG_WARN("doca_verbs_multi_rail_get_post_split returned EMPTY");
				continue;
			}
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_get_post_split failed: %s",
					     doca_error_get_name(err));
				goto out;
			}
		}
		for (q = 0; q < cfg->num_qps; q++) {
			uint32_t depth = qp_states[q].cfg.qp_depth;
			uint32_t max_out = depth > 1 ? depth - 1 : 1;
			uint64_t outstanding;
			uint64_t posted, max_posted;

			/*
			 * Accumulate this QP's value into the next iteration's snapshot before any posting
			 * below. Done here (ahead of the skip checks) so in-queue QPs that are skipped this
			 * round still count toward the minimum and average.
			 */
			if (in_queue[q]) {
				next_values_sum += values[q];
				next_in_queue_count++;
				if (values[q] < next_min_value)
					next_min_value = values[q];
			}

			if (!qp_states[q].active.load(std::memory_order_acquire))
				continue;
			if (cfg->use_multi_rail && chunks_split[q] == 0)
				continue;

			outstanding = posted_pkts[q] - completed_pkts[q];
			posted = 0;
			max_posted = VERBS_MULTI_RAIL_MAX_POST_COUNT_GAP - (values[q] - min_value);
			while (outstanding < max_out && (!in_queue[q] || posted < max_posted)) {
				struct ibv_sge sge = {};
				struct ibv_send_wr wr = {};
				struct ibv_send_wr *bad_wr = NULL;
				uint32_t wr_len;
				uint64_t off;
				int ret;

				wr_len = cfg->use_multi_rail ? chunks_split[q] :
							       verbs_multi_rail_split_wr_len(num_active,
											     (uint64_t)chunks_total,
											     is_first_post);
				off = (posted_pkts[q] % depth) * (uint64_t)chunks_total;
				sge.addr = (uint64_t)(uintptr_t)((uint8_t *)qp_states[q].buf + off);
				sge.length = wr_len;
				sge.lkey = qp_states[q].mr->lkey;
				wr.wr_id = wr_len; /* set this to WR length to get it back in WC */
				wr.sg_list = &sge;
				wr.num_sge = 1;
				wr.opcode = IBV_WR_SEND;
				wr.send_flags = IBV_SEND_SIGNALED;

				ret = doca_verbs_bridge_post_send(qp_states[q].qp, &wr, &bad_wr);
				if (ret != 0 || bad_wr != NULL) {
					DOCA_LOG_ERR("post_send failed");
					err = DOCA_ERROR_IO_FAILED;
					goto out;
				}
				if (cfg->use_multi_rail) {
					pthread_mutex_lock(verbs_multi_rail_mutex);
					err = doca_verbs_multi_rail_update(verbs_multi_rail,
									   DOCA_VERBS_MULTI_RAIL_UPDATE_POST,
									   q,
									   wr.wr_id,
									   wr_len);
					pthread_mutex_unlock(verbs_multi_rail_mutex);
					if (err != DOCA_SUCCESS) {
						DOCA_LOG_ERR("doca_verbs_multi_rail_update POST failed for QP %u: %s",
							     q,
							     doca_error_get_name(err));
						goto out;
					}
				}
				posted_pkts[q]++;
				outstanding++;
				posted++;
			}
			if (in_queue[q]) {
				values[q] += posted;
			}
			is_first_post = false;
		}

		/* Carry the snapshot accumulated above into the next iteration. */
		values_sum = next_values_sum;
		min_value = next_in_queue_count != 0 ? next_min_value : 0;
		in_queue_count = next_in_queue_count;
	}

finalize_send_recv_stats:
	if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
		err = DOCA_ERROR_DRIVER;
		goto out;
	}

	{
		double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
		double total_bytes = (double)total_completed_bytes;
		uint64_t end_ns = verbs_multi_rail_timespec_to_ns(&t1);

		err = verbs_multi_rail_bw_samples_update(&samples, total_completed_bytes, end_ns);
		if (err != DOCA_SUCCESS)
			goto out;
		if (sec > 0)
			*bw_gbps_out = total_bytes * 8.0 / sec / 1e9;
		else
			*bw_gbps_out = 0;
		DOCA_LOG_INFO("Stopped sending. Average BW: %.3f Gbps (%.2f GB completed)",
			      *bw_gbps_out,
			      total_bytes / (1024.0 * 1024.0 * 1024.0));
	}

out:
	free(samples.gbps);
	free(posted_pkts);
	free(completed_pkts);
	free(chunks_split);
	free(values);
	free(in_queue);

	if (err == DOCA_SUCCESS)
		err = recovery_thread_status;
	return err;
}

static doca_error_t run_client_send_recv(struct verbs_multi_rail_cfg *cfg,
					 struct verbs_multi_rail_qp_state *qp_states,
					 struct doca_verbs_multi_rail *verbs_multi_rail,
					 pthread_mutex_t *verbs_multi_rail_mutex)
{
	size_t chunks_total = cfg->chunks_total;
	uint64_t *posted_pkts = NULL;
	uint32_t q;
	doca_error_t err = DOCA_SUCCESS;

	posted_pkts = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	if (posted_pkts == NULL) {
		err = DOCA_ERROR_NO_MEMORY;
		goto out;
	}

	for (q = 0; q < cfg->num_qps; q++) {
		if (cfg->use_multi_rail) {
			pthread_mutex_lock(verbs_multi_rail_mutex);
			err = doca_verbs_multi_rail_update_qp(verbs_multi_rail,
							      DOCA_VERBS_MULTI_RAIL_QP_OP_ADD,
							      q,
							      qp_states[q].qp);
			if (err != DOCA_SUCCESS) {
				pthread_mutex_unlock(verbs_multi_rail_mutex);
				DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp ADD failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				goto out;
			}
			err = doca_verbs_multi_rail_set_probe_qp(verbs_multi_rail,
								 q,
								 qp_states[q].dev_instance->probe_uc_qp,
								 cfg->is_server ? 1 : 0);
			pthread_mutex_unlock(verbs_multi_rail_mutex);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_set_probe_qp failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				goto out;
			}
		}
	}

	for (q = 0; q < cfg->num_qps; q++) {
		uint32_t depth = qp_states[q].cfg.qp_depth;
		uint32_t pre = 0;

		if (!qp_states[q].active.load(std::memory_order_acquire))
			continue;
		while (pre < depth) {
			struct ibv_sge sge = {};
			struct ibv_recv_wr wr = {};
			struct ibv_recv_wr *bad_wr = NULL;
			uint64_t off = (uint64_t)pre * chunks_total;
			int ret;

			sge.addr = (uint64_t)(uintptr_t)((uint8_t *)qp_states[q].buf + off);
			sge.length = (uint32_t)chunks_total;
			sge.lkey = qp_states[q].mr->lkey;
			wr.wr_id = q;
			wr.sg_list = &sge;
			wr.num_sge = 1;
			ret = doca_verbs_bridge_post_recv(qp_states[q].qp, &wr, &bad_wr);
			if (ret != 0 || bad_wr != NULL) {
				err = DOCA_ERROR_IO_FAILED;
				goto out;
			}
			posted_pkts[q]++;
			pre++;
		}
	}
	DOCA_LOG_INFO("Client receive loop started. Press Ctrl-C to stop.");

	while (!verbs_multi_rail_force_quit) {
		for (q = 0; q < cfg->num_qps; q++) {
			struct ibv_wc wc;
			uint32_t depth = qp_states[q].cfg.qp_depth;

			if (!qp_states[q].active.load(std::memory_order_acquire))
				continue;
			if (doca_verbs_bridge_poll_cq(qp_states[q].cq, 1, &wc) != 1)
				continue;
			if (wc.status != IBV_WC_SUCCESS) {
				DOCA_LOG_ERR("Recv CQ error: %s", ibv_wc_status_str(wc.status));
				verbs_multi_rail_set_qp_inactive(&qp_states[q], STAGE_RECEIVER_IGNORE);
				continue;
			}
			if (wc.opcode != IBV_WC_RECV) {
				DOCA_LOG_ERR("Unexpected recv WC opcode");
				err = DOCA_ERROR_UNEXPECTED;
				goto out;
			}

			{
				struct ibv_sge sge = {};
				struct ibv_recv_wr wr = {};
				struct ibv_recv_wr *bad_wr = NULL;
				uint64_t off = (posted_pkts[q] % depth) * chunks_total;
				int ret;

				sge.addr = (uint64_t)(uintptr_t)((uint8_t *)qp_states[q].buf + off);
				sge.length = (uint32_t)chunks_total;
				sge.lkey = qp_states[q].mr->lkey;
				wr.wr_id = q;
				wr.sg_list = &sge;
				wr.num_sge = 1;
				ret = doca_verbs_bridge_post_recv(qp_states[q].qp, &wr, &bad_wr);
				if (ret != 0 || bad_wr != NULL) {
					err = DOCA_ERROR_IO_FAILED;
					goto out;
				}
				posted_pkts[q]++;
			}
		}
	}

out:
	free(posted_pkts);

	if (err == DOCA_SUCCESS)
		err = recovery_thread_status;
	return err;
}

static doca_error_t run_server_rdma_write(struct verbs_multi_rail_cfg *cfg,
					  struct verbs_multi_rail_qp_state *qp_states,
					  double *bw_gbps_out,
					  struct doca_verbs_multi_rail *verbs_multi_rail,
					  pthread_mutex_t *verbs_multi_rail_mutex)
{
	uint64_t *posted_pkts = NULL;
	uint64_t *completed_pkts = NULL;
	uint64_t *chunks_split = NULL;
	uint64_t *values = NULL;     /* per-QP cumulative posted-count priority */
	bool *in_queue = NULL;	     /* per-QP membership of the scheduling set */
	uint64_t values_sum = 0;     /* sum of values[q] over in-queue QPs (refreshed each loop) */
	uint64_t min_value = 0;	     /* min of values[q] over in-queue QPs (refreshed each loop) */
	uint32_t in_queue_count = 0; /* number of in-queue QPs (refreshed each loop) */
	uint64_t total_completed_bytes = 0;
	struct timespec t0, t1;
	struct verbs_multi_rail_bw_samples samples = {};
	uint32_t check_loop = 0;
	size_t chunks_total = cfg->chunks_total;
	uint32_t q;
	bool is_recoverable = false;
	doca_error_t err = DOCA_SUCCESS;

	posted_pkts = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	completed_pkts = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	chunks_split = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	values = static_cast<uint64_t *>(calloc(cfg->num_qps, sizeof(uint64_t)));
	in_queue = static_cast<bool *>(calloc(cfg->num_qps, sizeof(bool)));
	if (posted_pkts == NULL || completed_pkts == NULL || chunks_split == NULL || values == NULL ||
	    in_queue == NULL) {
		err = DOCA_ERROR_NO_MEMORY;
		goto out;
	}

	for (q = 0; q < cfg->num_qps; q++) {
		values[q] = 0;
		in_queue[q] = true;
		if (cfg->use_multi_rail) {
			pthread_mutex_lock(verbs_multi_rail_mutex);
			err = doca_verbs_multi_rail_update_qp(verbs_multi_rail,
							      DOCA_VERBS_MULTI_RAIL_QP_OP_ADD,
							      q,
							      qp_states[q].qp);
			if (err != DOCA_SUCCESS) {
				pthread_mutex_unlock(verbs_multi_rail_mutex);
				DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp ADD failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				goto out;
			}
			err = doca_verbs_multi_rail_set_probe_qp(verbs_multi_rail,
								 q,
								 qp_states[q].dev_instance->probe_uc_qp,
								 cfg->is_server ? 1 : 0);
			pthread_mutex_unlock(verbs_multi_rail_mutex);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_set_probe_qp failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				goto out;
			}
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
		err = DOCA_ERROR_DRIVER;
		goto out;
	}
	samples.last_sample_ns = verbs_multi_rail_timespec_to_ns(&t0);
	samples.last_sample_bytes = 0.0;
	DOCA_LOG_INFO("Server RDMA write traffic started. Press Ctrl-C to stop and print average BW.");

	while (!verbs_multi_rail_force_quit) {
		uint32_t num_active;

		if ((check_loop++ % VERBS_MULTI_RAIL_BW_CHECK_LOOPS) == 0) {
			struct timespec now_ts;

			if (clock_gettime(CLOCK_MONOTONIC, &now_ts) != 0) {
				err = DOCA_ERROR_DRIVER;
				goto out;
			}
			err = verbs_multi_rail_bw_samples_update(&samples,
								 total_completed_bytes,
								 verbs_multi_rail_timespec_to_ns(&now_ts));
			if (err != DOCA_SUCCESS)
				goto out;
		}

		num_active = verbs_multi_rail_total_active_qp.load(std::memory_order_acquire);
		if (num_active == 0)
			continue;

		for (q = 0; q < cfg->num_qps; q++) {
			struct ibv_wc wc_batch[VERBS_MULTI_RAIL_CQ_POLL_BATCH];
			bool cq_error = false;

			if (!qp_states[q].active.load(std::memory_order_acquire))
				continue;

			for (;;) {
				int n;
				int i;

				if (verbs_multi_rail_force_quit)
					goto finalize_rdma_write_stats;

				n = doca_verbs_bridge_poll_cq(qp_states[q].cq,
							      VERBS_MULTI_RAIL_CQ_POLL_BATCH,
							      wc_batch);
				if (n < 0) {
					DOCA_LOG_ERR("poll_cq failed on QP %u", q);
					err = DOCA_ERROR_IO_FAILED;
					goto out;
				}
				if (n == 0)
					break;

				for (i = 0; i < n; i++) {
					struct ibv_wc *wc = &wc_batch[i];

					if (wc->status != IBV_WC_SUCCESS) {
						err = doca_verbs_multi_rail_error_is_recoverable(wc->status,
												 &is_recoverable);
						if (err != DOCA_SUCCESS) {
							DOCA_LOG_ERR(
								"doca_verbs_multi_rail_error_is_recoverable failed: %s",
								doca_error_get_name(err));
							goto out;
						}

						DOCA_LOG_ERR(
							"RDMA write CQ error: %s, Found an error WC in QP#%u, is_recoverable: %d",
							ibv_wc_status_str(wc->status),
							q,
							is_recoverable);

						struct timespec t_evt;
						uint64_t t_evt_ns;

						if (clock_gettime(CLOCK_MONOTONIC, &t_evt) != 0) {
							err = DOCA_ERROR_DRIVER;
							goto out;
						}
						t_evt_ns = verbs_multi_rail_timespec_to_ns(&t_evt);
						verbs_multi_rail_set_qp_inactive(&qp_states[q], STAGE_SENDER_FLUSH);
						posted_pkts[q] = completed_pkts[q];
						cq_error = true;
						if (in_queue[q]) {
							qp_states[q].t_inactive_ns = t_evt_ns;
							DOCA_LOG_INFO("Received an error WC for QP#%u at %" PRIu64
								      " ns, marking as inactive",
								      q,
								      t_evt_ns);
							in_queue[q] = false;
						}
						if (cfg->use_multi_rail) {
							pthread_mutex_lock(verbs_multi_rail_mutex);
							err = doca_verbs_multi_rail_update_qp(
								verbs_multi_rail,
								DOCA_VERBS_MULTI_RAIL_QP_OP_REMOVE,
								q,
								NULL);
							pthread_mutex_unlock(verbs_multi_rail_mutex);
							if (err != DOCA_SUCCESS) {
								DOCA_LOG_ERR(
									"doca_verbs_multi_rail_update_qp REMOVE failed for QP %u: %s",
									q,
									doca_error_get_name(err));
								goto out;
							}
						}
						break;
					}
					if (wc->opcode != IBV_WC_RDMA_WRITE) {
						DOCA_LOG_ERR("Unexpected WC opcode on RDMA write server path");
						err = DOCA_ERROR_UNEXPECTED;
						goto out;
					}
					if (cfg->use_multi_rail) {
						pthread_mutex_lock(verbs_multi_rail_mutex);
						err = doca_verbs_multi_rail_update(verbs_multi_rail,
										   DOCA_VERBS_MULTI_RAIL_UPDATE_COMP,
										   q,
										   wc->wr_id,
										   0);
						pthread_mutex_unlock(verbs_multi_rail_mutex);
						if (err != DOCA_SUCCESS) {
							DOCA_LOG_ERR(
								"doca_verbs_multi_rail_update COMP failed for QP %u: %s",
								q,
								doca_error_get_name(err));
							goto out;
						}
					}
					completed_pkts[q]++;
					total_completed_bytes += (uint64_t)wc->wr_id;
				}
				if (!cq_error && !in_queue[q]) {
					struct timespec t_evt;
					uint64_t t_evt_ns;
					/* uint64_t subtraction is modulo 2^64 -- correct across clock wrap. */
					uint64_t downtime_ns;

					if (clock_gettime(CLOCK_MONOTONIC, &t_evt) != 0) {
						err = DOCA_ERROR_DRIVER;
						goto out;
					}
					t_evt_ns = verbs_multi_rail_timespec_to_ns(&t_evt);
					downtime_ns = t_evt_ns - qp_states[q].t_inactive_ns;
					DOCA_LOG_INFO("Received success WC for QP#%u at %" PRIu64
						      " ns, marking as active (downtime: %" PRIu64 " ns)",
						      q,
						      t_evt_ns,
						      downtime_ns);
					/* Re-admit at the fair average of the QPs currently in the scheduling set. */
					values[q] = in_queue_count != 0 ? values_sum / in_queue_count : 0;
					in_queue[q] = true;
				}
				if (cq_error)
					break;
			}
		}

		/*
		 * Per-QP scheduling snapshot (sum/min/count over in-queue QPs). It is accumulated during the
		 * post loop below and committed at its end, so each iteration schedules using the values
		 * tracked over the previous iteration instead of recomputing them in a separate pass.
		 */
		uint64_t next_values_sum = 0;
		uint64_t next_min_value = UINT64_MAX;
		uint32_t next_in_queue_count = 0;
		bool is_first_post = true;

		num_active = verbs_multi_rail_total_active_qp.load(std::memory_order_acquire);
		if (num_active == 0)
			continue;
		if (cfg->use_multi_rail) {
			pthread_mutex_lock(verbs_multi_rail_mutex);
			err = doca_verbs_multi_rail_get_post_split(verbs_multi_rail, chunks_total, chunks_split);
			pthread_mutex_unlock(verbs_multi_rail_mutex);
			if (err == DOCA_ERROR_EMPTY) {
				DOCA_LOG_WARN("doca_verbs_multi_rail_get_post_split returned EMPTY");
				continue;
			}
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_get_post_split failed: %s",
					     doca_error_get_name(err));
				goto out;
			}
		}
		for (q = 0; q < cfg->num_qps; q++) {
			uint32_t depth = qp_states[q].cfg.qp_depth;
			uint64_t remote_depth = qp_states[q].remote.buf_bytes / (uint64_t)chunks_total;
			uint64_t ring_depth = remote_depth < (uint64_t)depth ? remote_depth : (uint64_t)depth;
			uint32_t max_out = depth > 1 ? depth - 1 : 1;
			uint64_t outstanding;
			uint64_t posted, max_posted;

			/*
			 * Accumulate this QP's value into the next iteration's snapshot before any posting
			 * below. Done here (ahead of the skip checks) so in-queue QPs that are skipped this
			 * round still count toward the minimum and average.
			 */
			if (in_queue[q]) {
				next_values_sum += values[q];
				next_in_queue_count++;
				if (values[q] < next_min_value)
					next_min_value = values[q];
			}

			if (!qp_states[q].active.load(std::memory_order_acquire))
				continue;
			if (cfg->use_multi_rail && chunks_split[q] == 0)
				continue;

			outstanding = posted_pkts[q] - completed_pkts[q];
			posted = 0;
			max_posted = VERBS_MULTI_RAIL_MAX_POST_COUNT_GAP - (values[q] - min_value);
			while (outstanding < max_out && (!in_queue[q] || posted < max_posted)) {
				struct ibv_sge sge = {};
				struct ibv_send_wr wr = {};
				struct ibv_send_wr *bad_wr = NULL;
				uint64_t local_off;
				uint64_t remote_off;
				uint32_t wr_len;
				int ret;

				wr_len = cfg->use_multi_rail ? chunks_split[q] :
							       verbs_multi_rail_split_wr_len(num_active,
											     (uint64_t)chunks_total,
											     is_first_post);

				local_off = (posted_pkts[q] % depth) * (uint64_t)chunks_total;
				remote_off = (posted_pkts[q] % ring_depth) * (uint64_t)chunks_total;

				sge.addr = (uint64_t)(uintptr_t)((uint8_t *)qp_states[q].buf + local_off);
				sge.length = wr_len;
				sge.lkey = qp_states[q].mr->lkey;
				wr.wr_id = wr_len;
				wr.sg_list = &sge;
				wr.num_sge = 1;
				wr.opcode = IBV_WR_RDMA_WRITE;
				wr.send_flags = IBV_SEND_SIGNALED;
				wr.wr.rdma.remote_addr = qp_states[q].remote.buf_va + remote_off;
				wr.wr.rdma.rkey = qp_states[q].remote.rkey;

				ret = doca_verbs_bridge_post_send(qp_states[q].qp, &wr, &bad_wr);
				if (ret != 0 || bad_wr != NULL) {
					DOCA_LOG_ERR("post RDMA write failed");
					err = DOCA_ERROR_IO_FAILED;
					goto out;
				}
				if (cfg->use_multi_rail) {
					pthread_mutex_lock(verbs_multi_rail_mutex);
					err = doca_verbs_multi_rail_update(verbs_multi_rail,
									   DOCA_VERBS_MULTI_RAIL_UPDATE_POST,
									   q,
									   wr.wr_id,
									   wr_len);
					pthread_mutex_unlock(verbs_multi_rail_mutex);
					if (err != DOCA_SUCCESS) {
						DOCA_LOG_ERR("doca_verbs_multi_rail_update POST failed for QP %u: %s",
							     q,
							     doca_error_get_name(err));
						goto out;
					}
				}
				posted_pkts[q]++;
				outstanding++;
				posted++;
			}
			if (in_queue[q]) {
				values[q] += posted;
			}
			is_first_post = false;
		}

		/* Carry the snapshot accumulated above into the next iteration. */
		values_sum = next_values_sum;
		min_value = next_in_queue_count != 0 ? next_min_value : 0;
		in_queue_count = next_in_queue_count;
	}

finalize_rdma_write_stats:
	if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
		err = DOCA_ERROR_DRIVER;
		goto out;
	}

	{
		double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
		double total_bytes = (double)total_completed_bytes;
		uint64_t end_ns = verbs_multi_rail_timespec_to_ns(&t1);

		err = verbs_multi_rail_bw_samples_update(&samples, total_completed_bytes, end_ns);
		if (err != DOCA_SUCCESS)
			goto out;
		if (sec > 0)
			*bw_gbps_out = total_bytes * 8.0 / sec / 1e9;
		else
			*bw_gbps_out = 0;
		DOCA_LOG_INFO("Stopped RDMA write. Average BW: %.3f Gbps (%.2f GB completed)",
			      *bw_gbps_out,
			      total_bytes / (1024.0 * 1024.0 * 1024.0));
	}

out:
	free(samples.gbps);
	free(posted_pkts);
	free(completed_pkts);
	free(chunks_split);
	free(values);
	free(in_queue);

	if (err == DOCA_SUCCESS)
		err = recovery_thread_status;
	return err;
}

static doca_error_t run_client_rdma_write(struct verbs_multi_rail_cfg *cfg,
					  struct verbs_multi_rail_qp_state *qp_states,
					  struct doca_verbs_multi_rail *verbs_multi_rail,
					  pthread_mutex_t *verbs_multi_rail_mutex)
{
	doca_error_t err = DOCA_SUCCESS;

	if (cfg->use_multi_rail) {
		for (uint32_t q = 0; q < cfg->num_qps; q++) {
			pthread_mutex_lock(verbs_multi_rail_mutex);
			err = doca_verbs_multi_rail_update_qp(verbs_multi_rail,
							      DOCA_VERBS_MULTI_RAIL_QP_OP_ADD,
							      q,
							      qp_states[q].qp);
			if (err != DOCA_SUCCESS) {
				pthread_mutex_unlock(verbs_multi_rail_mutex);
				DOCA_LOG_ERR("doca_verbs_multi_rail_update_qp ADD failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				return err;
			}
			err = doca_verbs_multi_rail_set_probe_qp(verbs_multi_rail,
								 q,
								 qp_states[q].dev_instance->probe_uc_qp,
								 cfg->is_server ? 1 : 0);
			pthread_mutex_unlock(verbs_multi_rail_mutex);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("doca_verbs_multi_rail_set_probe_qp failed for QP %u: %s",
					     q,
					     doca_error_get_name(err));
				return err;
			}
		}
	}

	DOCA_LOG_INFO("Client RDMA write mode running. Press Ctrl-C to stop.");
	while (!verbs_multi_rail_force_quit)
		sched_yield();

	if (err == DOCA_SUCCESS)
		err = recovery_thread_status;
	return err;
}

static doca_error_t destroy_dev_instance(struct verbs_multi_rail_dev_instance *dev_instance)
{
	doca_error_t status = DOCA_SUCCESS;
	doca_error_t t;

	if (dev_instance->probe_uc_qp != NULL) {
		t = doca_verbs_qp_destroy(dev_instance->probe_uc_qp);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		dev_instance->probe_uc_qp = NULL;
	}
	if (dev_instance->probe_send_cq != NULL) {
		t = doca_verbs_cq_destroy(dev_instance->probe_send_cq);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		dev_instance->probe_send_cq = NULL;
	}
	if (dev_instance->probe_recv_cq != NULL) {
		t = doca_verbs_cq_destroy(dev_instance->probe_recv_cq);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		dev_instance->probe_recv_cq = NULL;
	}

	if (dev_instance->doca_dev != NULL) {
		t = doca_dev_close(dev_instance->doca_dev);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		dev_instance->doca_dev = NULL;
	}
	if (dev_instance->verbs_pd != NULL) {
		t = doca_verbs_pd_destroy(dev_instance->verbs_pd);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		dev_instance->verbs_pd = NULL;
	}
	if (dev_instance->verbs_ctx != NULL) {
		t = doca_verbs_context_destroy(dev_instance->verbs_ctx);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		dev_instance->verbs_ctx = NULL;
	}
	dev_instance->ibv_pd = NULL;
	return status;
}

static doca_error_t destroy_qp_state(struct verbs_multi_rail_qp_state *qp_state)
{
	doca_error_t status = DOCA_SUCCESS;
	doca_error_t t;

	if (qp_state->mr != NULL) {
		if (ibv_dereg_mr(qp_state->mr) != 0)
			DOCA_ERROR_PROPAGATE(status, DOCA_ERROR_DRIVER);
		qp_state->mr = NULL;
	}
	free(qp_state->buf);
	qp_state->buf = NULL;

	if (qp_state->qp != NULL) {
		t = doca_verbs_qp_destroy(qp_state->qp);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		qp_state->qp = NULL;
	}
	if (qp_state->cq != NULL) {
		t = doca_verbs_cq_destroy(qp_state->cq);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		qp_state->cq = NULL;
	}
	if (qp_state->ah != NULL) {
		t = doca_verbs_ah_attr_destroy(qp_state->ah);
		if (t != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(status, t);
		qp_state->ah = NULL;
	}
	qp_state->dev_instance = NULL;
	return status;
}

doca_error_t verbs_multi_rail_run(struct verbs_multi_rail_cfg *cfg)
{
	struct verbs_multi_rail_qp_params *qp_params = NULL;
	char(*dev_names)[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = NULL;
	struct verbs_multi_rail_dev_instance *dev_instances = NULL;
	uint32_t nb_dev_instances = 0;
	struct verbs_multi_rail_qp_state *qp_states = NULL;
	char default_dev[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
	doca_error_t status = DOCA_SUCCESS;
	uint32_t i;
	int sock_fd = -1;
	int listen_fd = -1;
	pthread_t recovery_thread;
	bool recovery_thread_started = false;
	struct verbs_multi_rail_recovery_thread_ctx recovery_ctx = {};
	struct doca_verbs_multi_rail *verbs_multi_rail = NULL;
	struct doca_verbs_multi_rail_cfg *verbs_multi_rail_cfg = NULL;
	pthread_mutex_t verbs_multi_rail_mutex = {};
	bool verbs_multi_rail_mutex_initialized = false;
	double bw = 0;
	uint32_t *peer_uc_qpns = NULL;
	uint32_t *local_uc_qpns = NULL;

	verbs_multi_rail_force_quit = 0;
	verbs_multi_rail_local_shutdown_requested = 0;
	verbs_multi_rail_local_shutdown_sent.store(false, std::memory_order_release);
	verbs_multi_rail_peer_shutdown_received.store(false, std::memory_order_release);
	seconds_time = 0;
	(void)signal(SIGINT, verbs_multi_rail_signal_handler);
	(void)signal(SIGTERM, verbs_multi_rail_signal_handler);

	if (!cfg->use_multi_rail && cfg->multi_rail_json_set) {
		DOCA_LOG_WARN("multi_rail_json_path is ignored when --use-multi-rail is not set");
		cfg->multi_rail_json_set = false;
	}

	if (cfg->chunks_total == 0)
		cfg->chunks_total = VERBS_MULTI_RAIL_DEFAULT_CHUNKS_TOTAL;
	if (cfg->tcp_port == 0)
		cfg->tcp_port = VERBS_MULTI_RAIL_TCP_PORT_DEFAULT;

	if (cfg->num_qps == 0 || cfg->num_qps > VERBS_MULTI_RAIL_MAX_QPS) {
		DOCA_LOG_ERR("num_qps must be between 1 and %u", VERBS_MULTI_RAIL_MAX_QPS);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (!cfg->is_server && strlen(cfg->server_ip) == 0) {
		DOCA_LOG_ERR("Client mode requires --server-ip");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (cfg->is_server && strlen(cfg->server_ip) > 0)
		DOCA_LOG_WARN("--server-ip is ignored in server mode");

	verbs_multi_rail_total_active_qp.store(0, std::memory_order_relaxed);

	if (cfg->use_multi_rail) {
		int mutex_ret = pthread_mutex_init(&verbs_multi_rail_mutex, NULL);
		if (mutex_ret != 0) {
			DOCA_LOG_ERR("Failed to initialize multi_rail mutex: %s", strerror(mutex_ret));
			status = DOCA_ERROR_OPERATING_SYSTEM;
			goto cleanup_alloc;
		}
		verbs_multi_rail_mutex_initialized = true;
		status = doca_verbs_multi_rail_cfg_create(&verbs_multi_rail_cfg);
		if (status != DOCA_SUCCESS)
			goto cleanup_alloc;
		if (cfg->multi_rail_json_set) {
			status = verbs_multi_rail_parse_cfg_json(cfg->multi_rail_json_path, verbs_multi_rail_cfg);
			if (status != DOCA_SUCCESS)
				goto cleanup_alloc;
		}
		status = doca_verbs_multi_rail_create(cfg->num_qps, &verbs_multi_rail);
		if (status != DOCA_SUCCESS)
			goto cleanup_alloc;
		status = doca_verbs_multi_rail_apply_cfg(verbs_multi_rail, verbs_multi_rail_cfg);
		if (status != DOCA_SUCCESS)
			goto cleanup_alloc;
	}

	qp_params = static_cast<struct verbs_multi_rail_qp_params *>(
		calloc(cfg->num_qps, sizeof(struct verbs_multi_rail_qp_params)));
	dev_names = static_cast<char(*)[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1]>(
		calloc(cfg->num_qps, DOCA_DEVINFO_IBDEV_NAME_SIZE + 1));
	qp_states = static_cast<struct verbs_multi_rail_qp_state *>(
		calloc(cfg->num_qps, sizeof(struct verbs_multi_rail_qp_state)));
	dev_instances = static_cast<struct verbs_multi_rail_dev_instance *>(
		calloc(cfg->num_qps, sizeof(struct verbs_multi_rail_dev_instance)));
	if (qp_params == NULL || dev_names == NULL || qp_states == NULL || dev_instances == NULL) {
		status = DOCA_ERROR_NO_MEMORY;
		goto cleanup_alloc;
	}

	for (i = 0; i < cfg->num_qps; i++)
		verbs_multi_rail_qp_params_defaults(&qp_params[i]);

	if (cfg->qp_json_set) {
		status = verbs_multi_rail_parse_qp_json(cfg->qp_json_path, cfg->num_qps, qp_params);
		if (status != DOCA_SUCCESS)
			goto cleanup_alloc;
	}

	status = verbs_multi_rail_pick_default_ibdev(default_dev, sizeof(default_dev));
	if (status != DOCA_SUCCESS)
		goto cleanup_alloc;

	if (cfg->dev_json_set) {
		status = verbs_multi_rail_parse_dev_json(cfg->dev_json_path, cfg->num_qps, dev_names, default_dev);
		if (status != DOCA_SUCCESS)
			goto cleanup_alloc;
	} else {
		for (i = 0; i < cfg->num_qps; i++)
			snprintf(dev_names[i], DOCA_DEVINFO_IBDEV_NAME_SIZE + 1, "%s", default_dev);
	}

	for (i = 0; i < cfg->num_qps; i++) {
		bool found = false;

		qp_states[i].cfg = qp_params[i];

		/* Check if we already have a device instance for this device name */
		for (uint32_t bi = 0; bi < nb_dev_instances; bi++) {
			if (strcmp(dev_instances[bi].name, dev_names[i]) == 0) {
				qp_states[i].dev_instance = &dev_instances[bi];
				found = true;
				break;
			}
		}
		if (!found) {
			struct verbs_multi_rail_dev_instance *dev_instance = &dev_instances[nb_dev_instances];

			strncpy(dev_instance->name, dev_names[i], DOCA_DEVINFO_IBDEV_NAME_SIZE);
			status = open_verbs_context_for_ibdev(dev_names[i], &dev_instance->verbs_ctx);
			if (status != DOCA_SUCCESS)
				goto cleanup_partial;
			/*
			 * Register the slot with the cleanup counter BEFORE the remaining sub-steps
			 * so any subsequent failure includes this dev_instance in the cleanup_partial
			 * sweep. destroy_dev_instance NULL-checks each field, so it tolerates a
			 * partially-initialized slot.
			 */
			nb_dev_instances++;
			status = doca_verbs_pd_create(dev_instance->verbs_ctx, &dev_instance->verbs_pd);
			if (status != DOCA_SUCCESS)
				goto cleanup_partial;
			status = doca_verbs_pd_as_doca_dev(dev_instance->verbs_pd, &dev_instance->doca_dev);
			if (status != DOCA_SUCCESS)
				goto cleanup_partial;
			status = doca_rdma_bridge_get_dev_pd(dev_instance->doca_dev, &dev_instance->ibv_pd);
			if (status != DOCA_SUCCESS)
				goto cleanup_partial;
			qp_states[i].dev_instance = dev_instance;
		}
	}

	for (i = 0; i < cfg->num_qps; i++) {
		struct verbs_multi_rail_qp_state *qp_state = &qp_states[i];
		struct ibv_port_attr port_attr;
		union ibv_gid rgid;
		uint32_t rq_wr;
		uint32_t sq_wr;
		uint32_t cq_sz;

		rq_wr = qp_state->cfg.qp_depth < VERBS_MULTI_RAIL_MIN_RQ_WR ? VERBS_MULTI_RAIL_MIN_RQ_WR :
									      qp_state->cfg.qp_depth;
		sq_wr = qp_state->cfg.qp_depth;
		cq_sz = sq_wr * 2;

		if (ibv_query_gid(qp_state->dev_instance->ibv_pd->context,
				  qp_state->cfg.port_num,
				  qp_state->cfg.gid_index,
				  &rgid) != 0) {
			status = DOCA_ERROR_DRIVER;
			goto cleanup_partial;
		}
		memcpy(qp_state->gid.raw, rgid.raw, sizeof(qp_state->gid.raw));

		if (ibv_query_port(qp_state->dev_instance->ibv_pd->context, qp_state->cfg.port_num, &port_attr) != 0) {
			status = DOCA_ERROR_DRIVER;
			goto cleanup_partial;
		}
		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)
			qp_state->addr_type = DOCA_VERBS_ADDR_TYPE_IPv4;
		else if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
			qp_state->addr_type = DOCA_VERBS_ADDR_TYPE_IB_NO_GRH;
			qp_state->lid = port_attr.lid;
		} else {
			status = DOCA_ERROR_UNEXPECTED;
			goto cleanup_partial;
		}

		status = create_ah_attr(qp_state->dev_instance->verbs_ctx,
					qp_state->cfg.gid_index,
					qp_state->addr_type,
					&qp_state->ah);
		if (status != DOCA_SUCCESS)
			goto cleanup_partial;

		status = create_verbs_cq(qp_state->dev_instance->verbs_ctx, cq_sz, &qp_state->cq);
		if (status != DOCA_SUCCESS)
			goto cleanup_partial;

		status = create_verbs_qp(qp_state->dev_instance->verbs_ctx,
					 qp_state->dev_instance->verbs_pd,
					 qp_state->cq,
					 rq_wr,
					 sq_wr,
					 &qp_state->qp);
		if (status != DOCA_SUCCESS)
			goto cleanup_partial;

		qp_state->qpn = doca_verbs_qp_get_qpn(qp_state->qp);

		if (qp_state->cfg.qp_depth == 0) {
			DOCA_LOG_ERR("qp_depth must be greater than zero");
			status = DOCA_ERROR_INVALID_VALUE;
			goto cleanup_partial;
		}

		/* qp_depth was checked > 0 above, so the division is safe. */
		if (cfg->chunks_total > SIZE_MAX / qp_state->cfg.qp_depth) {
			DOCA_LOG_ERR("chunks_total * qp_depth overflows size_t");
			status = DOCA_ERROR_INVALID_VALUE;
			goto cleanup_partial;
		}
		qp_state->buf_bytes = cfg->chunks_total * qp_state->cfg.qp_depth;
		qp_state->buf = calloc(1, qp_state->buf_bytes);
		if (qp_state->buf == NULL) {
			status = DOCA_ERROR_NO_MEMORY;
			goto cleanup_partial;
		}

		qp_state->mr = ibv_reg_mr(qp_state->dev_instance->ibv_pd,
					  qp_state->buf,
					  qp_state->buf_bytes,
					  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (qp_state->mr == NULL) {
			status = DOCA_ERROR_DRIVER;
			goto cleanup_partial;
		}
		qp_state->rkey = qp_state->mr->rkey;
		qp_state->active.store(true, std::memory_order_release);
		verbs_multi_rail_total_active_qp.fetch_add(1, std::memory_order_release);
	}

	if (cfg->is_server) {
		status = tcp_server_accept(cfg->tcp_port, &listen_fd, &sock_fd);
		if (status != DOCA_SUCCESS)
			goto cleanup_partial;
	} else {
		status = tcp_client_connect(cfg->server_ip, cfg->tcp_port, &sock_fd);
		if (status != DOCA_SUCCESS)
			goto cleanup_partial;
	}

	if (sock_fd < 0) {
		DOCA_LOG_ERR("Invalid connection socket fd");
		status = DOCA_ERROR_UNEXPECTED;
		goto tcp_cleanup;
	}

	status = exchange_oob(sock_fd, cfg, qp_states);
	if (status != DOCA_SUCCESS)
		goto tcp_cleanup;

	if (cfg->use_multi_rail) {
		local_uc_qpns = static_cast<uint32_t *>(calloc(nb_dev_instances, sizeof(uint32_t)));
		peer_uc_qpns = static_cast<uint32_t *>(calloc(nb_dev_instances, sizeof(uint32_t)));
		if (local_uc_qpns == NULL || peer_uc_qpns == NULL) {
			status = DOCA_ERROR_NO_MEMORY;
			goto tcp_cleanup;
		}

		for (i = 0; i < nb_dev_instances; i++) {
			struct verbs_multi_rail_dev_instance *di = &dev_instances[i];

			status = create_verbs_cq(di->verbs_ctx, VERBS_MULTI_RAIL_PROBE_CQ_SIZE, &di->probe_send_cq);
			if (status != DOCA_SUCCESS)
				goto tcp_cleanup;
			status = create_verbs_cq(di->verbs_ctx, VERBS_MULTI_RAIL_PROBE_CQ_SIZE, &di->probe_recv_cq);
			if (status != DOCA_SUCCESS)
				goto tcp_cleanup;
			status = create_verbs_uc_qp(di->verbs_ctx,
						    di->verbs_pd,
						    di->probe_send_cq,
						    di->probe_recv_cq,
						    VERBS_MULTI_RAIL_PROBE_SQ_WR,
						    VERBS_MULTI_RAIL_PROBE_RQ_WR,
						    &di->probe_uc_qp);
			if (status != DOCA_SUCCESS)
				goto tcp_cleanup;
			local_uc_qpns[i] = doca_verbs_qp_get_qpn(di->probe_uc_qp);
		}

		status = exchange_probe_uc_oob(sock_fd, nb_dev_instances, local_uc_qpns, peer_uc_qpns, cfg->is_server);
		if (status != DOCA_SUCCESS)
			goto tcp_cleanup;
	}

	for (i = 0; i < cfg->num_qps; i++) {
		status = connect_qp_state_qp(&qp_states[i]);
		if (status != DOCA_SUCCESS)
			goto tcp_cleanup;
		qp_states[i].active.store(true, std::memory_order_release);
	}

	if (cfg->use_multi_rail) {
		for (i = 0; i < nb_dev_instances; i++) {
			struct verbs_multi_rail_dev_instance *di = &dev_instances[i];
			struct verbs_multi_rail_qp_state *ref_qp =
				verbs_multi_rail_first_qp_on_dev(qp_states, cfg->num_qps, di);

			if (ref_qp == NULL) {
				DOCA_LOG_ERR("No QP bound to device instance '%s'", di->name);
				status = DOCA_ERROR_UNEXPECTED;
				goto tcp_cleanup;
			}
			status = connect_uc_qp_peer(di->probe_uc_qp, ref_qp, peer_uc_qpns[i]);
			if (status != DOCA_SUCCESS)
				goto tcp_cleanup;
		}
	}

	recovery_ctx.sock_fd = sock_fd;
	recovery_ctx.num_qps = cfg->num_qps;
	recovery_ctx.is_sender = cfg->is_server;
	recovery_ctx.use_multi_rail = cfg->use_multi_rail;
	recovery_ctx.chunks_total = cfg->chunks_total;
	recovery_ctx.traffic = cfg->traffic;
	recovery_ctx.qp_states = qp_states;
	recovery_ctx.verbs_multi_rail = verbs_multi_rail;
	recovery_ctx.verbs_multi_rail_mutex = &verbs_multi_rail_mutex;
	if (pthread_create(&recovery_thread, NULL, verbs_multi_rail_recovery_thread_main, &recovery_ctx) != 0) {
		status = DOCA_ERROR_INITIALIZATION;
		goto tcp_cleanup;
	}
	recovery_thread_started = true;

	if (cfg->traffic == VERBS_MULTI_RAIL_TRAFFIC_SEND_RECV) {
		if (cfg->is_server)
			status = run_server_send_recv(cfg, qp_states, &bw, verbs_multi_rail, &verbs_multi_rail_mutex);
		else
			status = run_client_send_recv(cfg, qp_states, verbs_multi_rail, &verbs_multi_rail_mutex);
	} else {
		if (cfg->is_server)
			status = run_server_rdma_write(cfg, qp_states, &bw, verbs_multi_rail, &verbs_multi_rail_mutex);
		else
			status = run_client_rdma_write(cfg, qp_states, verbs_multi_rail, &verbs_multi_rail_mutex);
	}

tcp_cleanup:
	verbs_multi_rail_force_quit = 1;
	if (recovery_thread_started)
		(void)pthread_join(recovery_thread, NULL);
	if (status == DOCA_SUCCESS) {
		status = recovery_thread_status;
	}
	if (listen_fd >= 0)
		close(listen_fd);
	if (sock_fd >= 0)
		close(sock_fd);

cleanup_partial:
	/*
	 * Destroy the multi_rail instance before any QP or CQ it references.
	 */
	if (cfg->use_multi_rail) {
		if (verbs_multi_rail != NULL) {
			(void)doca_verbs_multi_rail_destroy(verbs_multi_rail);
			verbs_multi_rail = NULL;
		}
	}

	verbs_multi_rail_total_active_qp.store(0, std::memory_order_relaxed);
	for (i = 0; i < cfg->num_qps; i++)
		(void)destroy_qp_state(&qp_states[i]);

	for (i = 0; i < nb_dev_instances; i++)
		(void)destroy_dev_instance(&dev_instances[i]);

cleanup_alloc:
	free(dev_instances);
	free(qp_states);
	free(dev_names);
	free(qp_params);
	free(local_uc_qpns);
	free(peer_uc_qpns);
	if (cfg->use_multi_rail) {
		if (verbs_multi_rail_cfg != NULL)
			doca_verbs_multi_rail_cfg_destroy(verbs_multi_rail_cfg);
		/* Only reached on the early-failure paths that jump straight here, before any QP exists */
		if (verbs_multi_rail != NULL)
			doca_verbs_multi_rail_destroy(verbs_multi_rail);
		if (verbs_multi_rail_mutex_initialized)
			pthread_mutex_destroy(&verbs_multi_rail_mutex);
	}
	return status;
}

static doca_error_t set_num_qps(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;
	char *end = NULL;
	unsigned long v;

	v = strtoul((char *)param, &end, 10);
	if (end == param || *end != '\0' || v == 0 || v > VERBS_MULTI_RAIL_MAX_QPS)
		return DOCA_ERROR_INVALID_VALUE;
	cfg->num_qps = (uint32_t)v;
	return DOCA_SUCCESS;
}

static doca_error_t set_qp_json(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;

	strncpy(cfg->qp_json_path, (char *)param, sizeof(cfg->qp_json_path) - 1);
	cfg->qp_json_path[sizeof(cfg->qp_json_path) - 1] = '\0';
	cfg->qp_json_set = true;
	return DOCA_SUCCESS;
}

static doca_error_t set_dev_json(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;

	strncpy(cfg->dev_json_path, (char *)param, sizeof(cfg->dev_json_path) - 1);
	cfg->dev_json_path[sizeof(cfg->dev_json_path) - 1] = '\0';
	cfg->dev_json_set = true;
	return DOCA_SUCCESS;
}

static doca_error_t set_multi_rail_json(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;

	strncpy(cfg->multi_rail_json_path, (char *)param, sizeof(cfg->multi_rail_json_path) - 1);
	cfg->multi_rail_json_path[sizeof(cfg->multi_rail_json_path) - 1] = '\0';
	cfg->multi_rail_json_set = true;
	return DOCA_SUCCESS;
}

static doca_error_t set_chunks_total(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;
	char *end = NULL;
	unsigned long v;

	v = strtoul((char *)param, &end, 10);
	if (end == param || *end != '\0' || v == 0)
		return DOCA_ERROR_INVALID_VALUE;
	cfg->chunks_total = (size_t)v;
	return DOCA_SUCCESS;
}

static doca_error_t set_traffic(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;
	const char *s = (const char *)param;

	if (strcasecmp(s, "send") == 0 || strcasecmp(s, "send_recv") == 0)
		cfg->traffic = VERBS_MULTI_RAIL_TRAFFIC_SEND_RECV;
	else if (strcasecmp(s, "write") == 0 || strcasecmp(s, "rdma_write") == 0)
		cfg->traffic = VERBS_MULTI_RAIL_TRAFFIC_RDMA_WRITE;
	else
		return DOCA_ERROR_INVALID_VALUE;
	return DOCA_SUCCESS;
}

static doca_error_t set_server_flag(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;

	(void)param;
	cfg->is_server = true;
	return DOCA_SUCCESS;
}

static doca_error_t set_use_multi_rail_flag(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;

	(void)param;
	cfg->use_multi_rail = true;
	return DOCA_SUCCESS;
}

static doca_error_t set_server_ip(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;

	strncpy(cfg->server_ip, (char *)param, sizeof(cfg->server_ip) - 1);
	cfg->server_ip[sizeof(cfg->server_ip) - 1] = '\0';
	return DOCA_SUCCESS;
}

static doca_error_t set_tcp_port(void *param, void *opaque)
{
	struct verbs_multi_rail_cfg *cfg = (struct verbs_multi_rail_cfg *)opaque;
	char *end = NULL;
	unsigned long v;

	v = strtoul((char *)param, &end, 10);
	if (end == param || *end != '\0' || v > 65535)
		return DOCA_ERROR_INVALID_VALUE;
	cfg->tcp_port = (uint16_t)v;
	return DOCA_SUCCESS;
}

doca_error_t verbs_multi_rail_register_params(void)
{
	doca_error_t ret;
	struct doca_argp_param *p = NULL;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_short_name(p, "q");
	doca_argp_param_set_long_name(p, "num-qps");
	doca_argp_param_set_description(p, "Number of RC queue pairs (mandatory)");
	doca_argp_param_set_callback(p, set_num_qps);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(p);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "qp-json");
	doca_argp_param_set_description(
		p,
		"Optional JSON file: array of one object per QP (length must equal --num-qps); per-object keys: qp_depth, pkey_index, port_num, ack_timeout, retry_cnt, rnr_retry, min_rnr_timer, gid_index. Omitted keys keep built-in defaults. Default behavior when omitted: built-in defaults are used for every QP.");
	doca_argp_param_set_callback(p, set_qp_json);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "dev-json");
	doca_argp_param_set_description(
		p,
		"Optional JSON array of ibdev names by QP index (e.g. [\"mlx5_0\", \"mlx5_1\"]). Default behavior when omitted: all QPs use the first discovered DOCA device.");
	doca_argp_param_set_callback(p, set_dev_json);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "multi-rail-json");
	doca_argp_param_set_description(
		p,
		"Optional JSON file for doca_verbs_multi_rail_cfg when --use-multi-rail: optional keys bw_decay_factor, max_bw_drop_factor, min_chunk_size, chunk_alignment, min_splitable_message_size, lb_mode (measured_bw|port_speed or 0-1), probe_count, probe_timeout_s. Omitted keys keep library defaults.");
	doca_argp_param_set_callback(p, set_multi_rail_json);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "chunks-total");
	doca_argp_param_set_description(
		p,
		"Per-iteration payload budget in bytes. Each outer iteration of the sender posts up to "
		"chunks_total bytes total, distributed across the currently-active QPs (each WR carries "
		"~chunks_total / num_active bytes; the first active QP absorbs the modular remainder). "
		"Each QP's local buffer slot is allocated as chunks_total bytes -- this is sized for the "
		"worst case where only one QP is active and that QP must carry the full budget per WR. "
		"Receivers pre-post RECVs of chunks_total bytes for the same reason. Must match between "
		"server and client; checked at handshake. Default: 131072.");
	doca_argp_param_set_callback(p, set_chunks_total);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "traffic");
	doca_argp_param_set_description(p, "Traffic type: send or write (default: send)");
	doca_argp_param_set_callback(p, set_traffic);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "server");
	doca_argp_param_set_description(
		p,
		"Run as server. Default behavior when omitted: client mode (requires --server-ip).");
	doca_argp_param_set_callback(p, set_server_flag);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_BOOLEAN);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "use-multi-rail");
	doca_argp_param_set_description(p, "Use DOCA Verbs Multi-Rail. Default behavior when omitted: disabled.");
	doca_argp_param_set_callback(p, set_use_multi_rail_flag);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_BOOLEAN);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "server-ip");
	doca_argp_param_set_description(
		p,
		"Server IPv4 address. Mandatory in client mode; ignored in server mode. Default behavior when omitted: none.");
	doca_argp_param_set_callback(p, set_server_ip);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	ret = doca_argp_param_create(&p);
	if (ret != DOCA_SUCCESS)
		return ret;
	doca_argp_param_set_long_name(p, "tcp-port");
	doca_argp_param_set_description(p, "TCP port for out-of-band QP metadata exchange (default: 5000)");
	doca_argp_param_set_callback(p, set_tcp_port);
	doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(p);
	if (ret != DOCA_SUCCESS)
		return ret;

	return DOCA_SUCCESS;
}
