/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "time_sync_dpu_core.h"

#include <endian.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <doca_comch.h>
#include <doca_ctx.h>
#include <doca_dpa.h>
#include <doca_log.h>
#include <doca_pe.h>

DOCA_LOG_REGISTER(TIME_SYNC::TIME_SYNC_DPU_CORE);

#define USEC_IN_SEC 1000000  /* Number of microseconds in a second */
#define NSEC_IN_USEC 1000    /* Number of nanoseconds in a microsecond */
#define MSEC_IN_SEC 1000     /* Number of milliseconds in a second */
#define NSEC_IN_MSEC 1000000 /* Number of nanoseconds in a millisecond */

/* External struct to link to compiled DPA app of given name */
extern struct doca_dpa_app *dpa_time_sync;

/* Function defined on DPA kernel */
doca_dpa_func_t time_sync_event_rpc;

/*
 * Helper function to check capabilities of selected device for use in app
 *
 * @devinfo [in]: device to check caps for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t time_sync_dpu_cap_check(struct doca_devinfo *devinfo)
{
	uint8_t filter_net_sup = 0;
	doca_error_t result;

	/* DPU device must have comch server capabilities */
	result = doca_comch_cap_server_is_supported(devinfo);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Comch server is not supported on device: %s", doca_error_get_descr(result));
		return result;
	}

	/* DPU device requires repr remote net dev support for setting up comch */
	result = doca_devinfo_rep_cap_is_filter_net_supported(devinfo, &filter_net_sup);
	if (result != DOCA_SUCCESS || filter_net_sup == 0) {
		DOCA_LOG_ERR("Representors with remove net devs are not supported on device: err: %s, filter sup: %u",
			     doca_error_get_descr(result),
			     filter_net_sup);
		return result;
	}

	/* DPU side interfaces a DPA app so verify support */
	result = doca_dpa_cap_is_supported(devinfo);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Doca DPA is not supported on device: %s", doca_error_get_descr(result));
		return result;
	}

	/* DPU side needs real time clock support enabled on NIC */
	result = doca_clock_cap_nic_real_time_is_supported(devinfo);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Real time clock is not supported on device: %s", doca_error_get_descr(result));
		return result;
	}

	/* DPU app requires DPA timer clock support */
	result = doca_clock_cap_nic_dpa_timer_is_supported(devinfo);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("DPA timer is not supported on device: %s", doca_error_get_descr(result));

	return result;
}

doca_error_t time_sync_dpu_open_devs(struct time_sync_cfg *ts_cfg)
{
	doca_error_t result;

	result = time_sync_common_open_dev_with_caps(ts_cfg, time_sync_dpu_cap_check);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device (%s): %s", ts_cfg->pci_addr, doca_error_get_descr(result));
		return result;
	}

	result = time_sync_common_open_repr(ts_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open repr device: %s", doca_error_get_descr(result));
		(void)doca_dev_close(ts_cfg->doca_dev);
		ts_cfg->doca_dev = NULL;
	}

	return result;
}

doca_error_t time_sync_dpu_load_dpa_app(struct time_sync_cfg *ts_cfg)
{
	doca_error_t result;

	result = doca_dpa_create(ts_cfg->doca_dev, &ts_cfg->dpa_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca dpa context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_dpa_set_app(ts_cfg->dpa_ctx, dpa_time_sync);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect app to doca dpa context: %s", doca_error_get_descr(result));
		doca_dpa_destroy(ts_cfg->dpa_ctx);
		return result;
	}

	result = doca_dpa_start(ts_cfg->dpa_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca dpa context: %s", doca_error_get_descr(result));
		doca_dpa_destroy(ts_cfg->dpa_ctx);
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t time_sync_dpu_unload_dpa_app(struct time_sync_cfg *ts_cfg)
{
	doca_error_t result;

	result = doca_dpa_stop(ts_cfg->dpa_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop doca dpa context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_dpa_destroy(ts_cfg->dpa_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca dpa context: %s", doca_error_get_descr(result));
		return result;
	}

	ts_cfg->dpa_ctx = NULL;

	return DOCA_SUCCESS;
}

/*
 * Callback for successful completion of a message send
 *
 * @task [in]: Config file for app containing initialised contexts
 * @task_user_data [in]: Metadata associated with the task
 * @ctx_user_data [in]: Metadata associated with the comch context
 */
static void send_task_comp_cb(struct doca_comch_task_send *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	doca_task_free(doca_comch_task_send_as_task(task));
}

/*
 * Callback for unsuccessful completion of a message send
 *
 * @task [in]: Config file for app containing initialised contexts
 * @task_user_data [in]: Metadata associated with the task
 * @ctx_user_data [in]: Metadata associated with the comch context
 */
static void send_task_comp_err_cb(struct doca_comch_task_send *task,
				  union doca_data task_user_data,
				  union doca_data ctx_user_data)
{
	struct time_sync_cfg *ts_cfg = (struct time_sync_cfg *)ctx_user_data.ptr;

	(void)task_user_data;

	ts_cfg->error = doca_task_get_status(doca_comch_task_send_as_task(task));
	doca_task_free(doca_comch_task_send_as_task(task));
}

/*
 * Callback event for a new message being received
 *
 * @event [in]: Message receive event that triggered the callback
 * @recv_buffer [in]: Pointer to buffer containing received message
 * @msg_len [in]: Length of the received message
 * @comch_connection [in]: Connection of which the message was received
 */
static void msg_recv_cb(struct doca_comch_event_msg_recv *event,
			uint8_t *recv_buffer,
			uint32_t msg_len,
			struct doca_comch_connection *comch_connection)
{
	struct doca_comch_task_send *task;
	struct doca_comch_server *server;
	struct time_sync_response resp;
	struct time_sync_request *req;
	uint64_t dpa_return, dpa_secs;
	struct time_sync_cfg *ts_cfg;
	union doca_data user_data;
	struct timespec ts;
	uint32_t delay;
	doca_error_t result;

	(void)event;

	server = doca_comch_server_get_server_ctx(comch_connection);

	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from ctx: %s", doca_error_get_name(result));
		return;
	}

	ts_cfg = (struct time_sync_cfg *)user_data.ptr;

	if (msg_len != sizeof(struct time_sync_request)) {
		DOCA_LOG_ERR("Received unexpected message - msg_len: %u, expected: %lu",
			     msg_len,
			     sizeof(struct time_sync_request));
		ts_cfg->error = DOCA_ERROR_UNEXPECTED;
		return;
	}

	req = (struct time_sync_request *)recv_buffer;
	/* Request is in network endianness (BE) so convert to host before using */
	delay = be32toh(req->delay);

	/* Delay value is in msec - convert to sec/nsec as per timespec */
	ts.tv_sec = delay / MSEC_IN_SEC;
	ts.tv_nsec = (delay - (ts.tv_sec * MSEC_IN_SEC)) * NSEC_IN_MSEC;

	DOCA_LOG_INFO("Received message from connected host");

	/* Add a delay before getting DPU time */
	(void)nanosleep(&ts, NULL);

	DOCA_LOG_INFO("Capturing local time");

	/* Populate response DPU host and NIC times for recv event */
	result = doca_clock_get_crosstimestamp(ts_cfg->clock,
					       DOCA_CLOCK_HOST_REAL_TIME,
					       DOCA_CLOCK_NIC_REAL_TIME,
					       &resp.dpu_event_time,
					       &resp.dpu_event_time_nic,
					       &resp.dpu_event_error_margin);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cross timestamp DPU and NIC: %s", doca_error_get_name(result));
		ts_cfg->error = result;
		return;
	}

	/* Add a delay before triggering DPA event */
	(void)nanosleep(&ts, NULL);

	DOCA_LOG_INFO("Sending request to DPA kernel");

	/* Do RPC on loaded DPA app to trigger time event on running kernel */
	result = doca_dpa_rpc(ts_cfg->dpa_ctx, &time_sync_event_rpc, &dpa_return);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to trigger time sync event on DPA kernel: %s", doca_error_get_name(result));
		ts_cfg->error = result;
		return;
	}

	/* The DPA RPC will give DPA clock time in microseconds - convert to sec/nsec timespec for response */
	dpa_secs = dpa_return / USEC_IN_SEC;
	resp.dpa_event_time.ts.tv_sec = dpa_secs;
	/* Get remainder (usec) and convert to nsec */
	resp.dpa_event_time.ts.tv_nsec = (dpa_return - (dpa_secs * USEC_IN_SEC)) * NSEC_IN_USEC;

	/* Response is fully populated so convert to network order (BE) for sending */
	resp.dpa_event_time.ts.tv_sec = htobe64(resp.dpa_event_time.ts.tv_sec);
	resp.dpa_event_time.ts.tv_nsec = htobe64(resp.dpa_event_time.ts.tv_nsec);
	resp.dpu_event_time.ts.tv_sec = htobe64(resp.dpu_event_time.ts.tv_sec);
	resp.dpu_event_time.ts.tv_nsec = htobe64(resp.dpu_event_time.ts.tv_nsec);
	resp.dpu_event_time_nic.ts.tv_sec = htobe64(resp.dpu_event_time_nic.ts.tv_sec);
	resp.dpu_event_time_nic.ts.tv_nsec = htobe64(resp.dpu_event_time_nic.ts.tv_nsec);
	resp.dpu_event_error_margin = htobe64(resp.dpu_event_error_margin);

	/* Add a delay before sending response */
	(void)nanosleep(&ts, NULL);

	/* Allocate a task for a response message */
	result = doca_comch_server_task_send_alloc_init(server,
							ts_cfg->conn,
							&resp,
							sizeof(struct time_sync_response),
							&task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate task for response message: %s", doca_error_get_name(result));
		ts_cfg->error = result;
		return;
	}

	DOCA_LOG_INFO("Sending response back to host");

	/* Send request message task */
	result = doca_task_submit(doca_comch_task_send_as_task(task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit send task: %s", doca_error_get_name(result));
		doca_task_free(doca_comch_task_send_as_task(task));
		ts_cfg->error = result;
		return;
	}
}

/*
 * Callback event for a new connection event
 *
 * @event [in]: Connection event that triggered the callback
 * @comch_conn [in]: New connection identifier
 * @change_success [in]: Indication of success of event
 */
static void server_connection_event_cb(struct doca_comch_event_connection_status_changed *event,
				       struct doca_comch_connection *comch_conn,
				       uint8_t change_success)
{
	struct doca_comch_server *server;
	struct time_sync_cfg *ts_cfg;
	union doca_data user_data;
	doca_error_t result;

	(void)event;

	/* Extract the comch server from the connection */
	server = doca_comch_server_get_server_ctx(comch_conn);

	/* Extract user data from server which will point to the app config struct */
	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from ctx: %s", doca_error_get_name(result));
		return;
	}

	ts_cfg = (struct time_sync_cfg *)user_data.ptr;
	if (change_success == 0) {
		ts_cfg->error = DOCA_ERROR_CONNECTION_ABORTED;
		DOCA_LOG_ERR("Connection attempt failed: %s", doca_error_get_name(ts_cfg->error));
		return;
	}

	ts_cfg->conn = comch_conn;
}

/*
 * Callback event for a new disconnection event
 *
 * @event [in]: Disconnection event that triggered the callback
 * @comch_conn [in]: Identifier of connection that is tearing down
 * @change_success [in]: Indication of success of event
 */
static void server_disconnection_event_cb(struct doca_comch_event_connection_status_changed *event,
					  struct doca_comch_connection *comch_conn,
					  uint8_t change_success)
{
	struct doca_comch_server *server;
	struct time_sync_cfg *ts_cfg;
	union doca_data user_data;
	doca_error_t result;

	(void)event;
	(void)change_success;

	/* Extract the comch server from the connection */
	server = doca_comch_server_get_server_ctx(comch_conn);

	/* Extract user data from server which will point to the app config struct */
	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from ctx: %s", doca_error_get_name(result));
		return;
	}

	ts_cfg = (struct time_sync_cfg *)user_data.ptr;
	ts_cfg->conn = NULL;
}

doca_error_t time_sync_dpu_init_comch_server(struct time_sync_cfg *ts_cfg)
{
	union doca_data user_data;
	doca_error_t result;

	result = doca_pe_create(&ts_cfg->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_create(ts_cfg->doca_dev, ts_cfg->repr_dev, COMCH_NAME, &ts_cfg->server);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create comch server context: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	ts_cfg->comch_ctx = doca_comch_server_as_ctx(ts_cfg->server);

	result = doca_pe_connect_ctx(ts_cfg->pe, ts_cfg->comch_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect comch server to progress engine: %s", doca_error_get_descr(result));
		goto destroy_server;
	}

	result = doca_comch_server_task_send_set_conf(ts_cfg->server,
						      send_task_comp_cb,
						      send_task_comp_err_cb,
						      COMCH_NUM_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed configuring comch tasks: %s", doca_error_get_name(result));
		goto destroy_server;
	}

	result = doca_comch_server_event_msg_recv_register(ts_cfg->server, msg_recv_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed configuring comch recv event callback: %s", doca_error_get_name(result));
		goto destroy_server;
	}

	result = doca_comch_server_event_connection_status_changed_register(ts_cfg->server,
									    server_connection_event_cb,
									    server_disconnection_event_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed configuring comch connection changed event callback: %s",
			     doca_error_get_name(result));
		goto destroy_server;
	}

	user_data.ptr = ts_cfg;
	result = doca_ctx_set_user_data(ts_cfg->comch_ctx, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed add user data to context: %s", doca_error_get_name(result));
		goto destroy_server;
	}

	result = doca_ctx_start(ts_cfg->comch_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed start comch server: %s", doca_error_get_name(result));
		goto destroy_server;
	}

	return DOCA_SUCCESS;

destroy_server:
	(void)doca_comch_server_destroy(ts_cfg->server);
	ts_cfg->server = NULL;
	ts_cfg->comch_ctx = NULL;
destroy_pe:
	(void)doca_pe_destroy(ts_cfg->pe);
	ts_cfg->pe = NULL;

	return result;
}

doca_error_t time_sync_dpu_close_comch_server(struct time_sync_cfg *ts_cfg)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_STOPPING;
	doca_error_t result;

	/* It is assumed that this function is called when the context is ready to be stopped */
	result = doca_ctx_stop(ts_cfg->comch_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to stop comch server context: %s", doca_error_get_name(result));
		return result;
	}

	/* Progress until stopping process has completed */
	while (state != DOCA_CTX_STATE_IDLE) {
		(void)doca_pe_progress(ts_cfg->pe);
		result = doca_ctx_get_state(ts_cfg->comch_ctx, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get connection state: %s", doca_error_get_name(result));
			return result;
		}
	}

	result = doca_comch_server_destroy(ts_cfg->server);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy comch server: %s", doca_error_get_name(result));
		return result;
	}

	ts_cfg->server = NULL;
	ts_cfg->comch_ctx = NULL;

	result = doca_pe_destroy(ts_cfg->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_name(result));
		return result;
	}

	ts_cfg->pe = NULL;

	return DOCA_SUCCESS;
}

doca_error_t time_sync_dpu_main_loop(struct time_sync_cfg *ts_cfg)
{
	/* Progress until a connection is established */
	while (ts_cfg->conn == NULL && ts_cfg->error == DOCA_SUCCESS) {
		(void)doca_pe_progress(ts_cfg->pe);
	}

	/* Check if connection attempt was made but failed */
	if (ts_cfg->error != DOCA_SUCCESS)
		return ts_cfg->error;

	/* Host app has established a connection to DPU */

	/* Progress until the connection is torn down */
	while (ts_cfg->conn != NULL && ts_cfg->error == DOCA_SUCCESS) {
		(void)doca_pe_progress(ts_cfg->pe);
	}

	return ts_cfg->error;
}
