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
#include "memos_initiator_ref_app_kv.h"
#include "memos_initiator_ref_app_kv_info.h"
#include "memos_initiator_ref_app_nvme_spec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_memos_initiator.h>
#include <doca_memos_initiator_dev.h>
#include <doca_memos_initiator_dev_io.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_KV);

#define MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS (10 * 1000) /* 10 us */

/*
 * One I/O context per worker, each with its own engine and completion slot. Nothing here is shared,
 * so a worker needs no lock: it is the only thread that ever touches its own entry, and it polls its
 * engine only while one of its own commands is in flight.
 */
struct memos_initiator_ref_app_kv_worker {
	struct doca_memos_initiator_dev_io *io;
	struct doca_pe *pe;

	/* Completion hand-off for this worker's in-flight task. */
	bool done;
	doca_error_t task_status;

	/* Submitted minus completed is what this worker still owes; a non-zero difference at teardown
	 * names the worker whose command never came back. */
	uint64_t submitted;
	uint64_t completed;
	uint64_t failed;
};

struct memos_initiator_ref_app_kv {
	struct doca_memos_initiator_dev *dev;
	struct doca_memos_initiator_dev_target *target;
	struct doca_memos_initiator_dev_volume *volume;
	struct doca_memos_initiator_dev_target_ns *target_ns;
	/* Carries the device, target and volume - everything the control plane owns. The main thread
	 * is its only progressor, which is where the initiator's keep-alive messages come from. */
	struct doca_pe *pe;
	/* The fabric-side device from --rdma-dev, owned by this module. Not app->dev, which is the
	 * emulation manager facing the host: on a BlueField the port with a route to the target is a
	 * different device, and the wrong one fails at connect with "Failed to resolve RDMA device". */
	struct doca_dev *doca_dev;

	struct memos_initiator_ref_app_kv_worker *workers;
	uint32_t num_workers;

	/* Negotiated with the target at attach time and reported to the host via Identify CNS 05h. */
	uint16_t max_key_len;
	uint32_t max_value_len;
	uint64_t ns_size;
	uint32_t nsid;
	uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN];

	enum doca_memos_initiator_dev_target_state target_state;
	bool attach_done;
	bool detach_done;
	struct memos_initiator_ref_app_kv_info_stats info_stats;
	doca_error_t attach_status;

	bool dev_added;
	bool initiator_up;
};

static size_t kv_elapsed_us(const struct timespec *start)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (size_t)((now.tv_sec - start->tv_sec) * 1000000LL + (now.tv_nsec - start->tv_nsec) / 1000LL);
}

/* Unbounded: the initiator delivers every event awaited here, and giving up would abandon an
 * operation it still owns. */
static void kvw_wait_done(struct memos_initiator_ref_app_kv_worker *kvw)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};

	while (!kvw->done) {
		if (doca_pe_progress(kvw->pe) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

/*
 * Progress every engine the backend owns. Only safe while no worker thread is running - bring-up and
 * teardown - where the main thread is the sole progressor. Connecting brings up the I/O queue
 * connections, which live on the per-worker engines, so driving only the control engine stalls it.
 */
static int kv_progress_all(struct memos_initiator_ref_app_kv *kv)
{
	int worked = doca_pe_progress(kv->pe);
	uint32_t i;

	for (i = 0; i < kv->num_workers; ++i)
		if (kv->workers[i].pe != NULL)
			worked += doca_pe_progress(kv->workers[i].pe);

	return worked;
}

static int kv_progress_all_cb(void *ctx)
{
	return kv_progress_all((struct memos_initiator_ref_app_kv *)ctx);
}

/* Same, on the control engine: target and volume events. */
static void kv_poll_until(struct memos_initiator_ref_app_kv *kv, const bool *done_flag)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};

	while (!*done_flag) {
		if (kv_progress_all(kv) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

/* Initiator contexts start asynchronously; acting on one early fails with "Context is not running". */
static doca_error_t kv_ctx_wait_running(struct memos_initiator_ref_app_kv *kv, struct doca_ctx *ctx, const char *what)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};
	struct timespec start;
	enum doca_ctx_states state;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (doca_ctx_get_state(ctx, &state) != DOCA_SUCCESS)
			return DOCA_ERROR_BAD_STATE;
		if (state == DOCA_CTX_STATE_RUNNING)
			return DOCA_SUCCESS;
		if (kv_progress_all(kv) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
		if (kv_elapsed_us(&start) >= MEMOS_INITIATOR_REF_APP_KV_CONNECT_TIMEOUT_US) {
			DOCA_LOG_ERR("Timed out starting %s (state=%d)", what, (int)state);
			return DOCA_ERROR_TIME_OUT;
		}
	}
}

/* Start @ctx and drive @pe until it is RUNNING. */
static doca_error_t kv_ctx_start_and_wait_on(struct doca_pe *pe, struct doca_ctx *ctx, const char *what)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};
	enum doca_ctx_states state;
	doca_error_t result = doca_ctx_start(ctx);

	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start %s: %s", what, doca_error_get_name(result));
		return result;
	}

	for (;;) {
		if (doca_ctx_get_state(ctx, &state) != DOCA_SUCCESS)
			return DOCA_ERROR_BAD_STATE;
		if (state == DOCA_CTX_STATE_RUNNING)
			return DOCA_SUCCESS;
		if (doca_pe_progress(pe) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

/*
 * DISCONNECTED is the only state a target can be destroyed from, and disconnecting is asynchronous.
 * Skipping this wait unravels the whole teardown: destroy refuses, so the device cannot stop, so its
 * contexts stay on the engine, so pe_destroy fails, so the library will not tear down.
 */
static void kv_target_disconnect_and_wait(struct memos_initiator_ref_app_kv *kv)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};
	enum doca_memos_initiator_dev_target_state state;

	if (doca_memos_initiator_dev_target_get_state(kv->target, &state) != DOCA_SUCCESS)
		return;

	if (state != DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECTED &&
	    state != DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECT_IN_PROGRESS)
		(void)doca_memos_initiator_dev_target_disconnect(kv->target);

	for (;;) {
		if (doca_memos_initiator_dev_target_get_state(kv->target, &state) != DOCA_SUCCESS)
			return;
		if (state == DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECTED)
			return;
		if (kv_progress_all(kv) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

/* Without the drain the context stays attached and doca_pe_destroy() later fails. */
static void kv_ctx_wait_idle(struct memos_initiator_ref_app_kv *kv, struct doca_ctx *ctx)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};
	enum doca_ctx_states state;

	for (;;) {
		if (doca_ctx_get_state(ctx, &state) != DOCA_SUCCESS || state == DOCA_CTX_STATE_IDLE)
			return;
		if (doca_pe_progress(kv->pe) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

/*
 * Stop @ctx and drive every engine until it is IDLE. An I/O context only reaches IDLE once the
 * device context it belongs to has drained it, and that one lives on the control engine, so waiting
 * on the I/O context's own engine alone never completes.
 */
static void kv_ctx_stop_and_wait(struct memos_initiator_ref_app_kv *kv, struct doca_ctx *ctx)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};
	enum doca_ctx_states state;
	doca_error_t result = doca_ctx_stop(ctx);

	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE)
		DOCA_LOG_ERR("Failed to stop context: %s", doca_error_get_name(result));

	for (;;) {
		if (doca_ctx_get_state(ctx, &state) != DOCA_SUCCESS || state == DOCA_CTX_STATE_IDLE)
			return;
		if (kv_progress_all(kv) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

/*****************************************************************************
 * Initiator callbacks
 *****************************************************************************/

static void kv_task_completed(struct doca_task *task, union doca_data ctx_user_data)
{
	struct memos_initiator_ref_app_kv_worker *kvw = (struct memos_initiator_ref_app_kv_worker *)ctx_user_data.ptr;

	kvw->task_status = doca_task_get_status(task);
	kvw->completed++;
	kvw->done = true;
}

static void kv_task_failed(struct doca_task *task, union doca_data ctx_user_data)
{
	struct memos_initiator_ref_app_kv_worker *kvw = (struct memos_initiator_ref_app_kv_worker *)ctx_user_data.ptr;

	kvw->task_status = doca_task_get_status(task);
	if (kvw->task_status == DOCA_SUCCESS)
		kvw->task_status = DOCA_ERROR_IO_FAILED;
	kvw->completed++;
	kvw->failed++;
	kvw->done = true;
}

static void kv_store_completion_cb(struct doca_memos_initiator_dev_io_task_store *task,
				   union doca_data task_user_data,
				   union doca_data ctx_user_data)
{
	(void)task_user_data;
	kv_task_completed(doca_memos_initiator_dev_io_task_store_as_task(task), ctx_user_data);
}

static void kv_store_error_cb(struct doca_memos_initiator_dev_io_task_store *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	(void)task_user_data;
	kv_task_failed(doca_memos_initiator_dev_io_task_store_as_task(task), ctx_user_data);
}

static void kv_retrieve_completion_cb(struct doca_memos_initiator_dev_io_task_retrieve *task,
				      union doca_data task_user_data,
				      union doca_data ctx_user_data)
{
	(void)task_user_data;
	kv_task_completed(doca_memos_initiator_dev_io_task_retrieve_as_task(task), ctx_user_data);
}

static void kv_retrieve_error_cb(struct doca_memos_initiator_dev_io_task_retrieve *task,
				 union doca_data task_user_data,
				 union doca_data ctx_user_data)
{
	(void)task_user_data;
	kv_task_failed(doca_memos_initiator_dev_io_task_retrieve_as_task(task), ctx_user_data);
}

static void kv_exist_completion_cb(struct doca_memos_initiator_dev_io_task_exist *task,
				   union doca_data task_user_data,
				   union doca_data ctx_user_data)
{
	(void)task_user_data;
	kv_task_completed(doca_memos_initiator_dev_io_task_exist_as_task(task), ctx_user_data);
}

static void kv_exist_error_cb(struct doca_memos_initiator_dev_io_task_exist *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	(void)task_user_data;
	kv_task_failed(doca_memos_initiator_dev_io_task_exist_as_task(task), ctx_user_data);
}

/* The event carries no state, so read it back off the target. */
static void kv_target_state_change_cb(struct doca_memos_initiator_dev_target *target, union doca_data reg_user_data)
{
	struct memos_initiator_ref_app_kv *kv = (struct memos_initiator_ref_app_kv *)reg_user_data.ptr;
	enum doca_memos_initiator_dev_target_state state;
	doca_error_t result;

	result = doca_memos_initiator_dev_target_get_state(target, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get target state on a state change event: %s", doca_error_get_name(result));
		return;
	}

	kv->target_state = state;
	/* The library only reaches CONNECT_IN_PROGRESS here on an unsolicited drop, and reconnects on
	 * its own indefinitely - so report a drop, do not act on it. */
	if (state == DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_CONNECT_IN_PROGRESS)
		DOCA_LOG_WARN("Target connection dropped; the initiator is reconnecting");
	DOCA_LOG_INFO("Target state -> %s", memos_initiator_ref_app_kv_info_target_state_str(state));
}

/* The namespace attached. Neither callback carries a status, so the outcome is which of the two
 * fires - this one, or kv_volume_ns_attach_err_cb() below. */
static void kv_volume_ns_attach_cb(struct doca_memos_initiator_dev_volume *volume,
				   struct doca_memos_initiator_dev_target_ns *ns,
				   union doca_data reg_user_data)
{
	struct memos_initiator_ref_app_kv *kv = (struct memos_initiator_ref_app_kv *)reg_user_data.ptr;

	(void)volume;
	(void)ns;
	kv->attach_status = DOCA_SUCCESS;
	kv->attach_done = true;
}

/* The attach failed. Registered separately from the success callback, which is how the initiator
 * reports the outcome - neither callback carries a status argument. */
static void kv_volume_ns_attach_err_cb(struct doca_memos_initiator_dev_volume *volume,
				       struct doca_memos_initiator_dev_target_ns *ns,
				       union doca_data reg_user_data)
{
	struct memos_initiator_ref_app_kv *kv = (struct memos_initiator_ref_app_kv *)reg_user_data.ptr;

	(void)volume;
	(void)ns;
	kv->attach_status = DOCA_ERROR_IO_FAILED;
	kv->attach_done = true;
}

/* Unsolicited "the target's namespace list changed" event. This application adopts one namespace at
 * bring-up and does not re-scan, so there is nothing to do - but the handler is registered anyway,
 * because the device context will not reach RUNNING until every event family has one. */
static void kv_target_ns_info_changed_cb(struct doca_memos_initiator_dev_target *target, union doca_data reg_user_data)
{
	(void)target;
	(void)reg_user_data;
	DOCA_LOG_INFO("Target namespace list changed; this application does not re-scan");
}

/* Detach completion, success and failure. Only reached during teardown, where nothing waits on the
 * outcome; registered because the device context requires the handlers to exist. */
static void kv_volume_ns_detach_cb(struct doca_memos_initiator_dev_volume *volume,
				   struct doca_memos_initiator_dev_target_ns *ns,
				   union doca_data reg_user_data)
{
	struct memos_initiator_ref_app_kv *kv = (struct memos_initiator_ref_app_kv *)reg_user_data.ptr;

	(void)volume;
	(void)ns;
	kv->detach_done = true;
}

/*****************************************************************************
 * Translation between DOCA errors and NVMe KV status codes
 *****************************************************************************/

/*
 * Map an initiator task outcome onto the KV status the host expects. A miss must leave the initiator
 * as NOT_FOUND and reach the host as KV_KEY_NOT_EXIST, or the caller sees the wrong errno.
 */
static uint16_t kv_status_from_doca_error(doca_error_t err)
{
	switch (err) {
	case DOCA_SUCCESS:
		return NVME_SC_SUCCESS;
	case DOCA_ERROR_NOT_FOUND:
		return NVME_SC_KV_KEY_NOT_EXIST;
	case DOCA_ERROR_ALREADY_EXIST:
		return NVME_SC_KV_KEY_EXISTS;
	case DOCA_ERROR_NO_MEMORY:
		return NVME_SC_KV_CAPACITY_EXCEEDED;
	case DOCA_ERROR_INVALID_VALUE:
		return NVME_SC_INVALID_FIELD;
	case DOCA_ERROR_BAD_STATE:
		return NVME_SC_NS_NOT_READY;
	default:
		return NVME_SC_KV_UNRECOVERED_ERROR;
	}
}

/*****************************************************************************
 * Task submission
 *****************************************************************************/

/* Submits and waits. Does NOT free @task: the caller's typed getters are valid only until it is. */
static doca_error_t kv_submit_and_wait(struct memos_initiator_ref_app_kv_worker *kvw,
				       struct doca_task *task,
				       const char *what)
{
	doca_error_t result;

	kvw->done = false;
	kvw->task_status = DOCA_SUCCESS;

	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit %s task: %s", what, doca_error_get_name(result));
		return result;
	}
	kvw->submitted++;

	kvw_wait_done(kvw);

	return kvw->task_status;
}

/*
 * Prefer @nvme_status, the status the target itself returned: it is already the KV command set's
 * own, so it reaches the host unmangled. Translate @err only when there is nothing to forward - a
 * transport failure, a timeout, or a task that never reached the target.
 */
static uint16_t kv_host_status(doca_error_t err, uint16_t nvme_status, const char *what)
{
	/* A target status is checked first: it is the authoritative outcome, and reporting success on
	 * top of one would tell the host a command committed when the target rejected it. */
	if (nvme_status != NVME_SC_SUCCESS) {
		DOCA_LOG_DBG("KV %s failed: forwarding target status 0x%04x (%s)",
			     what,
			     nvme_status,
			     doca_error_get_name(err));
		return nvme_status;
	}

	if (err == DOCA_SUCCESS)
		return NVME_SC_SUCCESS;

	DOCA_LOG_DBG("KV %s failed with no target status: translating %s", what, doca_error_get_name(err));
	return kv_status_from_doca_error(err);
}

/*****************************************************************************
 * Bring-up
 *****************************************************************************/

/* Open the DOCA device whose ibdev name matches @ibdev_name. */
static doca_error_t kv_open_rdma_dev(const char *ibdev_name, struct doca_dev **out_dev)
{
	struct doca_devinfo **devinfo_list;
	uint32_t nb_devs = 0;
	uint32_t i;
	doca_error_t result;

	result = doca_devinfo_create_list(&devinfo_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to list DOCA devices: %s", doca_error_get_name(result));
		return result;
	}

	result = DOCA_ERROR_NOT_FOUND;
	for (i = 0; i < nb_devs; ++i) {
		char name[DOCA_DEVINFO_IBDEV_NAME_SIZE];

		if (doca_devinfo_get_ibdev_name(devinfo_list[i], name, sizeof(name)) != DOCA_SUCCESS)
			continue;
		if (strcmp(name, ibdev_name) != 0)
			continue;

		result = doca_dev_open(devinfo_list[i], out_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to open RDMA device '%s': %s", ibdev_name, doca_error_get_name(result));
		break;
	}

	if (result == DOCA_ERROR_NOT_FOUND)
		DOCA_LOG_ERR(
			"No DOCA device with ibdev name '%s'; --rdma-dev must name the port with a route to the target",
			ibdev_name);

	(void)doca_devinfo_destroy_list(devinfo_list);
	return result;
}

/* Connect the target named by the --traddr / --trsvcid / --subnqn / --hostnqn parameters. */
static doca_error_t kv_target_connect(struct memos_initiator_ref_app_resources *app,
				      struct memos_initiator_ref_app_kv *kv)
{
	const struct memos_initiator_ref_app_cfg *cfg = &app->cfg;
	struct doca_memos_initiator_dev_target_create_attr *attr = NULL;
	doca_error_t result;

	result = doca_memos_initiator_dev_target_create_attr_create(&attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create target attributes: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_memos_initiator_dev_target_create_attr_set_trtype(attr, MEMOS_INITIATOR_REF_APP_TRTYPE_RDMA);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create_attr_set_adrfam(attr,
										MEMOS_INITIATOR_REF_APP_ADRFAM_IPV4);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create_attr_set_traddr(attr, cfg->traddr);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create_attr_set_trsvcid(attr, cfg->trsvcid);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create_attr_set_subnqn(attr, cfg->subnqn);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create_attr_set_hostnqn(attr, cfg->hostnqn);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create_attr_set_reconnect_delay(
			attr,
			MEMOS_INITIATOR_REF_APP_KV_RECONNECT_DELAY_SEC);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_target_create(kv->dev, attr, &kv->target);
	(void)doca_memos_initiator_dev_target_create_attr_destroy(attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create target: %s", doca_error_get_name(result));
		return result;
	}

	DOCA_LOG_INFO("Connecting to target %s:%s subnqn=%s", cfg->traddr, cfg->trsvcid, cfg->subnqn);
	result = doca_memos_initiator_dev_target_connect(kv->target);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect target: %s", doca_error_get_name(result));
		return result;
	}

	/* The state-change callback is the only signal that the connection settled. */
	{
		const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_KV_POLL_SLEEP_NS};
		struct timespec start;

		(void)clock_gettime(CLOCK_MONOTONIC, &start);
		bool saw_in_progress = false;

		while (kv->target_state != DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_CONNECTED) {
			if (kv->target_state == DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_CONNECT_IN_PROGRESS)
				saw_in_progress = true;
			/* Only a fall *back* to DISCONNECTED is a refusal - it is also the state the target
			 * starts in, so this has to wait until a connect attempt has been observed. */
			if (saw_in_progress && kv->target_state == DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECTED) {
				DOCA_LOG_ERR("Target %s:%s refused the connection", cfg->traddr, cfg->trsvcid);
				return DOCA_ERROR_CONNECTION_ABORTED;
			}
			if (kv_progress_all(kv) == 0) {
				struct timespec ts = sleep_ts;

				(void)nanosleep(&ts, NULL);
			}
			if (kv_elapsed_us(&start) >= MEMOS_INITIATOR_REF_APP_KV_CONNECT_TIMEOUT_US) {
				DOCA_LOG_ERR("Timed out connecting to target %s:%s", cfg->traddr, cfg->trsvcid);
				return DOCA_ERROR_TIME_OUT;
			}
		}
	}

	DOCA_LOG_INFO("Target connected");
	return DOCA_SUCCESS;
}

/*
 * Take the first namespace the target exports and adopt its geometry. A reference application does
 * not need namespace selection: the target is configured to export the one namespace it is meant to
 * serve, and taking the first keeps the app free of a --nsid the operator would have to discover.
 */
static doca_error_t kv_adopt_first_ns(struct memos_initiator_ref_app_kv *kv)
{
	struct doca_memos_initiator_dev_ns_info **ns_info_list = NULL;
	struct doca_memos_initiator_dev_ns_info *ns_info;
	uint32_t nb_ns = 0;
	doca_error_t result;

	result = doca_memos_initiator_dev_target_create_ns_info_list(kv->target, &ns_info_list, &nb_ns);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to list target namespaces: %s", doca_error_get_name(result));
		return result;
	}
	if (ns_info_list == NULL || nb_ns == 0) {
		DOCA_LOG_ERR("Target exports no namespaces");
		(void)doca_memos_initiator_dev_destroy_ns_info_list(ns_info_list);
		return DOCA_ERROR_NOT_FOUND;
	}
	if (nb_ns > 1)
		DOCA_LOG_INFO("Target exports %u namespaces; serving the first", nb_ns);

	ns_info = ns_info_list[0];
	result = doca_memos_initiator_dev_ns_info_get_nsid(ns_info, &kv->nsid);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_ns_info_get_max_key_len(ns_info, &kv->max_key_len);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_ns_info_get_max_value_len(ns_info, &kv->max_value_len);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_ns_info_get_size(ns_info, &kv->ns_size);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_ns_info_get_nguid(ns_info, kv->nguid, sizeof(kv->nguid));
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to read namespace info: %s", doca_error_get_name(result));

	/* The target namespace handle is created from the ns_info entry itself, so it has to be taken
	 * before the list is released - the nsid alone is not enough to name it. */
	if (result == DOCA_SUCCESS) {
		result = doca_memos_initiator_dev_target_ns_create(kv->target, ns_info, &kv->target_ns);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to create target ns %u: %s", kv->nsid, doca_error_get_name(result));
	}

	(void)doca_memos_initiator_dev_destroy_ns_info_list(ns_info_list);
	if (result != DOCA_SUCCESS)
		return result;

	/* Clamp to MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN: advertising the target's larger limit would
	 * invite commands the IO path rejects against that same bound. */
	if (kv->max_value_len > MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN) {
		DOCA_LOG_INFO("Target namespace %u allows %u B values; advertising %u B to the host",
			      kv->nsid,
			      kv->max_value_len,
			      MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN);
		kv->max_value_len = MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN;
	}
	if (kv->max_key_len > MEMOS_INITIATOR_REF_APP_KV_MAX_KEY_LEN)
		kv->max_key_len = MEMOS_INITIATOR_REF_APP_KV_MAX_KEY_LEN;

	DOCA_LOG_INFO("Adopted target namespace nsid=%u: max_key=%u max_value=%u size=%" PRIu64,
		      kv->nsid,
		      kv->max_key_len,
		      kv->max_value_len,
		      kv->ns_size);
	return DOCA_SUCCESS;
}

/* Create the volume the I/O tasks address and attach the target namespace to it. */
static doca_error_t kv_volume_attach(struct memos_initiator_ref_app_kv *kv)
{
	struct doca_memos_initiator_dev_volume_create_attr *attr = NULL;
	doca_error_t result;

	result = doca_memos_initiator_dev_volume_create_attr_create(&attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create volume attributes: %s", doca_error_get_name(result));
		return result;
	}

	result = doca_memos_initiator_dev_volume_create_attr_set_max_key_len(attr, kv->max_key_len);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_volume_create_attr_set_max_value_len(attr, kv->max_value_len);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_volume_create_attr_set_max_attached_ns(attr, 1);
	/* One unencrypted volume. Setting a DEK or TEK would enable volume encryption, which needs
	 * target support and key management this application cannot express. */
	if (result == DOCA_SUCCESS)
		result =
			doca_memos_initiator_dev_volume_create_attr_set_tenant_id(attr,
										  MEMOS_INITIATOR_REF_APP_KV_TENANT_ID);
	if (result == DOCA_SUCCESS)
		result = doca_memos_initiator_dev_volume_create(kv->dev, attr, &kv->volume);
	(void)doca_memos_initiator_dev_volume_create_attr_destroy(attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create volume: %s", doca_error_get_name(result));
		return result;
	}

	kv->attach_done = false;
	kv->attach_status = DOCA_SUCCESS;
	result = doca_memos_initiator_dev_volume_attach_ns(kv->volume, kv->target_ns);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach ns to volume: %s", doca_error_get_name(result));
		return result;
	}

	kv_poll_until(kv, &kv->attach_done);
	if (kv->attach_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Namespace attach failed: %s", doca_error_get_name(kv->attach_status));
		return kv->attach_status;
	}

	DOCA_LOG_INFO("Volume ready: nsid=%u attached", kv->nsid);
	return DOCA_SUCCESS;
}

doca_error_t memos_initiator_ref_app_kv_init(struct memos_initiator_ref_app_resources *app)
{
	struct memos_initiator_ref_app_kv *kv;
	union doca_data ctx_user_data = {0};
	doca_error_t result;
	uint32_t i;

	if (app->kv != NULL)
		return DOCA_SUCCESS;

	if (app->cfg.rdma_dev[0] == '\0' || app->cfg.traddr[0] == '\0' || app->cfg.trsvcid[0] == '\0' ||
	    app->cfg.subnqn[0] == '\0' || app->cfg.hostnqn[0] == '\0') {
		DOCA_LOG_ERR(
			"Serving the KV namespace needs a target: pass --rdma-dev, --traddr, --trsvcid, --subnqn and --hostnqn");
		return DOCA_ERROR_INVALID_VALUE;
	}

	kv = calloc(1, sizeof(*kv));
	if (kv == NULL) {
		DOCA_LOG_ERR("Failed to allocate the KV backend state");
		return DOCA_ERROR_NO_MEMORY;
	}
	kv->num_workers = app->num_workers > 0 ? app->num_workers : 1;
	kv->workers = calloc(kv->num_workers, sizeof(*kv->workers));
	if (kv->workers == NULL) {
		free(kv);
		return DOCA_ERROR_NO_MEMORY;
	}
	app->kv = kv;

	result = kv_open_rdma_dev(app->cfg.rdma_dev, &kv->doca_dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	memos_initiator_ref_app_kv_info_log_device_caps(kv->doca_dev);
	memos_initiator_ref_app_kv_info_log_stats_caps();

	/* Order matters: the initiator sizes itself from the devices it has been given, so
	 * doca_memos_initiator_init() fails with "At least one device must be added" if it runs first. */
	result = doca_memos_initiator_add_dev(kv->doca_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to the memos initiator: %s", doca_error_get_name(result));
		goto fail;
	}
	kv->dev_added = true;

	result = doca_memos_initiator_init();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init the memos initiator: %s", doca_error_get_name(result));
		goto fail;
	}
	kv->initiator_up = true;

	result = doca_pe_create(&kv->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the KV progress engine: %s", doca_error_get_name(result));
		goto fail;
	}

	result = doca_memos_initiator_dev_create(&kv->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the memos initiator device: %s", doca_error_get_name(result));
		goto fail;
	}

	result = doca_memos_initiator_dev_set_num_io_ctxs(kv->dev, kv->num_workers);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the io ctx count: %s", doca_error_get_name(result));
		goto fail;
	}

	/* Every event family needs a handler before the device starts, or it stalls in STARTING with no
	 * error. Two are no-ops here. */
	{
		union doca_data ev_user_data = {.ptr = kv};
		union doca_data stats_user_data = {.ptr = &kv->info_stats};

		result = doca_memos_initiator_dev_event_target_state_change_register(kv->dev,
										     kv_target_state_change_cb,
										     ev_user_data);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_event_target_ns_info_changed_register(
				kv->dev,
				kv_target_ns_info_changed_cb,
				ev_user_data);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_event_volume_ns_attach_register(kv->dev,
											  kv_volume_ns_attach_cb,
											  kv_volume_ns_attach_err_cb,
											  ev_user_data);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_event_volume_ns_detach_register(kv->dev,
											  kv_volume_ns_detach_cb,
											  kv_volume_ns_detach_cb,
											  ev_user_data);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_event_volume_collect_stats_register(
				kv->dev,
				memos_initiator_ref_app_kv_info_collect_stats_cb,
				stats_user_data);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_event_volume_reset_stats_register(
				kv->dev,
				memos_initiator_ref_app_kv_info_stats_completion_cb,
				stats_user_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register initiator events: %s", doca_error_get_name(result));
			goto fail;
		}
	}

	/* Do not wait yet: the device only finishes starting once its I/O contexts are started. Waiting
	 * here stalls in STARTING forever, with nothing logged to say why. */
	result = doca_pe_connect_ctx(kv->pe, doca_memos_initiator_dev_as_ctx(kv->dev));
	if (result == DOCA_SUCCESS) {
		result = doca_ctx_start(doca_memos_initiator_dev_as_ctx(kv->dev));
		if (result == DOCA_ERROR_IN_PROGRESS)
			result = DOCA_SUCCESS;
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start the initiator device: %s", doca_error_get_name(result));
		goto fail;
	}

	/* One I/O context per worker, each on its own engine. Nothing is shared between them, so a
	 * worker serves its commands without coordinating with any other thread. */
	for (i = 0; i < kv->num_workers; ++i) {
		struct memos_initiator_ref_app_kv_worker *kvw = &kv->workers[i];

		result = doca_pe_create(&kvw->pe);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_create(kv->dev, &kvw->io);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create memos io ctx %u: %s", i, doca_error_get_name(result));
			goto fail;
		}

		result = doca_memos_initiator_dev_io_set_num_tasks(kvw->io, MEMOS_INITIATOR_REF_APP_KV_NUM_TASKS);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_task_store_register_completion_cb(kvw->io,
											       kv_store_completion_cb);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_task_store_register_error_cb(kvw->io, kv_store_error_cb);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_task_retrieve_register_completion_cb(
				kvw->io,
				kv_retrieve_completion_cb);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_task_retrieve_register_error_cb(kvw->io,
											     kv_retrieve_error_cb);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_task_exist_register_completion_cb(kvw->io,
											       kv_exist_completion_cb);
		if (result == DOCA_SUCCESS)
			result = doca_memos_initiator_dev_io_task_exist_register_error_cb(kvw->io, kv_exist_error_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure memos io ctx %u: %s", i, doca_error_get_name(result));
			goto fail;
		}

		ctx_user_data.ptr = kvw;
		result = doca_ctx_set_user_data(doca_memos_initiator_dev_io_as_ctx(kvw->io), ctx_user_data);
		if (result == DOCA_SUCCESS)
			result = doca_pe_connect_ctx(kvw->pe, doca_memos_initiator_dev_io_as_ctx(kvw->io));
		if (result == DOCA_SUCCESS)
			result = kv_ctx_start_and_wait_on(kvw->pe,
							  doca_memos_initiator_dev_io_as_ctx(kvw->io),
							  "a memos io ctx");
		if (result != DOCA_SUCCESS)
			goto fail;
	}

	/* Now the device can finish starting. */
	result = kv_ctx_wait_running(kv, doca_memos_initiator_dev_as_ctx(kv->dev), "the initiator device");
	if (result != DOCA_SUCCESS)
		goto fail;

	result = kv_target_connect(app, kv);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = kv_adopt_first_ns(kv);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = kv_volume_attach(kv);
	if (result != DOCA_SUCCESS)
		goto fail;

	memos_initiator_ref_app_kv_info_log_sdk_view(kv->dev, kv->workers[0].io, kv->target, kv->target_ns, kv->volume);

	DOCA_LOG_INFO("KV backend up on %s:%s nsid=%u: max key %u B, max value %u B",
		      app->cfg.traddr,
		      app->cfg.trsvcid,
		      kv->nsid,
		      kv->max_key_len,
		      kv->max_value_len);
	return DOCA_SUCCESS;

fail:
	memos_initiator_ref_app_kv_teardown(app);
	return result;
}

struct doca_dev *memos_initiator_ref_app_kv_get_dev(const struct memos_initiator_ref_app_resources *app)
{
	return app->kv != NULL ? app->kv->doca_dev : NULL;
}

void memos_initiator_ref_app_kv_teardown(struct memos_initiator_ref_app_resources *app)
{
	struct memos_initiator_ref_app_kv *kv = app->kv;
	uint32_t i;

	if (kv == NULL)
		return;

	for (i = 0; i < kv->num_workers; ++i) {
		const struct memos_initiator_ref_app_kv_worker *kvw = &kv->workers[i];

		DOCA_LOG_INFO("KV worker %u: submitted=%" PRIu64 " completed=%" PRIu64 " failed=%" PRIu64
			      " in_flight=%" PRIu64,
			      i,
			      kvw->submitted,
			      kvw->completed,
			      kvw->failed,
			      kvw->submitted - kvw->completed);
	}

	memos_initiator_ref_app_kv_info_report_stats(kv_progress_all_cb, kv, &kv->info_stats, kv->volume, kv->target_ns);

	if (kv->volume != NULL) {
		/* The volume cannot be destroyed with a namespace attached, and this event is the in-flight
		 * I/O flush point. */
		if (kv->target_ns != NULL) {
			kv->detach_done = false;
			if (doca_memos_initiator_dev_volume_detach_ns(kv->target_ns) == DOCA_SUCCESS)
				kv_poll_until(kv, &kv->detach_done);
		}
		(void)doca_memos_initiator_dev_volume_destroy(kv->volume);
		kv->volume = NULL;
	}
	if (kv->target_ns != NULL) {
		(void)doca_memos_initiator_dev_target_ns_destroy(kv->target_ns);
		kv->target_ns = NULL;
	}
	if (kv->target != NULL) {
		kv_target_disconnect_and_wait(kv);
		(void)doca_memos_initiator_dev_target_destroy(kv->target);
		kv->target = NULL;
	}
	/*
	 * Top-down, mirroring the start order: an I/O context refuses to stop while its device runs
	 * ("Device is not stopping"), and the device only finishes stopping once they have drained.
	 */
	if (kv->dev != NULL) {
		doca_error_t result = doca_ctx_stop(doca_memos_initiator_dev_as_ctx(kv->dev));

		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to stop the initiator device: %s", doca_error_get_name(result));
	}
	for (i = 0; i < kv->num_workers; ++i) {
		struct memos_initiator_ref_app_kv_worker *kvw = &kv->workers[i];

		if (kvw->io != NULL) {
			kv_ctx_stop_and_wait(kv, doca_memos_initiator_dev_io_as_ctx(kvw->io));
			(void)doca_memos_initiator_dev_io_destroy(kvw->io);
			kvw->io = NULL;
		}
		if (kvw->pe != NULL) {
			(void)doca_pe_destroy(kvw->pe);
			kvw->pe = NULL;
		}
	}
	if (kv->dev != NULL) {
		kv_ctx_wait_idle(kv, doca_memos_initiator_dev_as_ctx(kv->dev));
		(void)doca_memos_initiator_dev_destroy(kv->dev);
		kv->dev = NULL;
	}
	if (kv->pe != NULL) {
		(void)doca_pe_destroy(kv->pe);
		kv->pe = NULL;
	}
	if (kv->initiator_up) {
		(void)doca_memos_initiator_teardown();
		kv->initiator_up = false;
	}
	if (kv->dev_added) {
		(void)doca_memos_initiator_rm_dev(kv->doca_dev);
		kv->dev_added = false;
	}
	if (kv->doca_dev != NULL) {
		(void)doca_dev_close(kv->doca_dev);
		kv->doca_dev = NULL;
	}

	free(kv->workers);
	free(kv);
	app->kv = NULL;
}

/*
 * Retrieve needs its own submit-and-wait: the value length the target actually returned lives on the
 * typed task and has to be read before the task is freed, which the generic helper cannot do.
 */
static doca_error_t kv_submit_and_wait_retrieve(struct memos_initiator_ref_app_kv_worker *kvw,
						struct doca_memos_initiator_dev_io_task_retrieve *task,
						uint32_t *out_len,
						uint16_t *nvme_status)
{
	struct doca_task *base = doca_memos_initiator_dev_io_task_retrieve_as_task(task);
	doca_error_t result;

	kvw->done = false;
	kvw->task_status = DOCA_SUCCESS;

	result = doca_task_submit(base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit retrieve task: %s", doca_error_get_name(result));
		doca_task_free(base);
		return result;
	}
	kvw->submitted++;

	kvw_wait_done(kvw);
	result = kvw->task_status;

	if (kvw->done)
		*nvme_status = doca_memos_initiator_dev_io_task_retrieve_get_result_nvme_status(task);

	/* The stored length, which may exceed what the host offered and what was written. */
	if (result == DOCA_SUCCESS)
		*out_len = doca_memos_initiator_dev_io_task_retrieve_get_result_value_len(task);

	doca_task_free(base);
	return result;
}

uint16_t memos_initiator_ref_app_kv_store(struct memos_initiator_ref_app_resources *app,
					  uint32_t worker_idx,
					  const uint8_t *key,
					  uint8_t key_len,
					  struct doca_buf *value_buf,
					  uint32_t value_list_len,
					  uint32_t len,
					  uint32_t opts)
{
	struct memos_initiator_ref_app_kv *kv = app->kv;
	struct memos_initiator_ref_app_kv_worker *kvw;
	struct doca_memos_initiator_dev_io_task_store *task;
	union doca_data task_user_data = {0};
	uint16_t nvme_status = NVME_SC_SUCCESS;
	doca_error_t result;

	if (kv == NULL || kv->volume == NULL || worker_idx >= kv->num_workers)
		return NVME_SC_NS_NOT_READY;
	kvw = &kv->workers[worker_idx];

	/* The conditional-store bits have no counterpart in the initiator store task, so a command
	 * carrying them would silently become an unconditional store. Reject rather than mislead;
	 * libdoca_kv only sets them when a caller asks for MUST_EXIST / DO_NOT_OVERWRITE. */
	if (opts != 0) {
		DOCA_LOG_WARN("KV Store with options 0x%x is not supported against a memos target", opts);
		return NVME_SC_INVALID_FIELD;
	}

	result = doca_memos_initiator_dev_io_task_store_alloc_init(kvw->io, kv->volume, task_user_data, &task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate store task: %s", doca_error_get_name(result));
		return kv_host_status(result, nvme_status, "store");
	}

	doca_memos_initiator_dev_io_task_store_set_key_value_conf(task, key, key_len, value_buf, value_list_len, len);
	result = kv_submit_and_wait(kvw, doca_memos_initiator_dev_io_task_store_as_task(task), "store");
	if (kvw->done)
		nvme_status = doca_memos_initiator_dev_io_task_store_get_result_nvme_status(task);
	doca_task_free(doca_memos_initiator_dev_io_task_store_as_task(task));

	return kv_host_status(result, nvme_status, "store");
}

uint16_t memos_initiator_ref_app_kv_retrieve(struct memos_initiator_ref_app_resources *app,
					     uint32_t worker_idx,
					     const uint8_t *key,
					     uint8_t key_len,
					     struct doca_buf *value_buf,
					     uint32_t value_list_len,
					     uint32_t out_cap,
					     uint32_t *out_len)
{
	struct memos_initiator_ref_app_kv *kv = app->kv;
	struct memos_initiator_ref_app_kv_worker *kvw;
	struct doca_memos_initiator_dev_io_task_retrieve *task;
	union doca_data task_user_data = {0};
	uint16_t nvme_status = NVME_SC_SUCCESS;
	doca_error_t result;

	if (kv == NULL || kv->volume == NULL || worker_idx >= kv->num_workers)
		return NVME_SC_NS_NOT_READY;
	kvw = &kv->workers[worker_idx];

	result = doca_memos_initiator_dev_io_task_retrieve_alloc_init(kvw->io, kv->volume, task_user_data, &task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate retrieve task: %s", doca_error_get_name(result));
		return kv_host_status(result, nvme_status, "retrieve");
	}

	doca_memos_initiator_dev_io_task_retrieve_set_key_value_conf(task,
								     key,
								     key_len,
								     value_buf,
								     value_list_len,
								     out_cap);

	result = kv_submit_and_wait_retrieve(kvw, task, out_len, &nvme_status);

	return kv_host_status(result, nvme_status, "retrieve");
}

uint16_t memos_initiator_ref_app_kv_exist(struct memos_initiator_ref_app_resources *app,
					  uint32_t worker_idx,
					  const uint8_t *key,
					  uint8_t key_len)
{
	struct memos_initiator_ref_app_kv *kv = app->kv;
	struct memos_initiator_ref_app_kv_worker *kvw;
	struct doca_memos_initiator_dev_io_task_exist *task;
	union doca_data task_user_data = {0};
	uint16_t nvme_status = NVME_SC_SUCCESS;
	doca_error_t result;

	if (kv == NULL || kv->volume == NULL || worker_idx >= kv->num_workers)
		return NVME_SC_NS_NOT_READY;
	kvw = &kv->workers[worker_idx];

	result = doca_memos_initiator_dev_io_task_exist_alloc_init(kvw->io, kv->volume, task_user_data, &task);
	if (result == DOCA_SUCCESS) {
		doca_memos_initiator_dev_io_task_exist_set_key_conf(task, key, key_len);
		result = kv_submit_and_wait(kvw, doca_memos_initiator_dev_io_task_exist_as_task(task), "exist");
		if (kvw->done)
			nvme_status = doca_memos_initiator_dev_io_task_exist_get_result_nvme_status(task);
		doca_task_free(doca_memos_initiator_dev_io_task_exist_as_task(task));
	} else {
		DOCA_LOG_ERR("Failed to allocate exist task: %s", doca_error_get_name(result));
	}

	return kv_host_status(result, nvme_status, "exist");
}

int memos_initiator_ref_app_kv_progress(struct memos_initiator_ref_app_resources *app)
{
	struct memos_initiator_ref_app_kv *kv = app->kv;
	int worked;

	if (kv == NULL || kv->pe == NULL)
		return 0;

	/*
	 * The control engine only. It carries the device, target and volume, and the main thread is
	 * its sole progressor - which is where the initiator's keep-alive messages come from. A worker engine
	 * belongs to its worker thread; touching one from here would put two threads in the same
	 * engine, which is what the per-worker split exists to avoid.
	 */
	worked = doca_pe_progress(kv->pe);

	return worked;
}

doca_error_t memos_initiator_ref_app_kv_get_geometry(const struct memos_initiator_ref_app_resources *app,
						     uint16_t *max_key_len,
						     uint32_t *max_value_len,
						     uint64_t *ns_size,
						     uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN])
{
	const struct memos_initiator_ref_app_kv *kv = app->kv;

	if (kv == NULL || kv->volume == NULL)
		return DOCA_ERROR_BAD_STATE;

	*max_key_len = kv->max_key_len;
	*max_value_len = kv->max_value_len;
	*ns_size = kv->ns_size;
	memcpy(nguid, kv->nguid, MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN);

	return DOCA_SUCCESS;
}
