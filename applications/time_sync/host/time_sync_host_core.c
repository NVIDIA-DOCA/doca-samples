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

#include <endian.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <doca_ctx.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "time_sync_host_core.h"

DOCA_LOG_REGISTER(TIME_SYNC::TIME_SYNC_HOST_CORE);

#define NSECS_IN_SEC 1000000000 /* Number of nanoseconds in a second */

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
	struct doca_comch_client *client;
	struct time_sync_response *resp;
	struct time_sync_cfg *ts_cfg;
	union doca_data user_data;
	doca_error_t result;

	(void)event;

	client = doca_comch_client_get_client_ctx(comch_connection);

	result = doca_ctx_get_user_data(doca_comch_client_as_ctx(client), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from ctx: %s", doca_error_get_name(result));
		return;
	}

	ts_cfg = (struct time_sync_cfg *)user_data.ptr;

	/* Received message is expected to be of type time_sync_response */
	if (msg_len != sizeof(struct time_sync_response)) {
		DOCA_LOG_ERR("Received unexpected message - msg_len: %u, expected: %lu",
			     msg_len,
			     sizeof(struct time_sync_response));
		ts_cfg->error = DOCA_ERROR_UNEXPECTED;
		return;
	}

	/* Copy received message from network byte order to local */
	resp = (struct time_sync_response *)recv_buffer;
	ts_cfg->dpu_resp.dpa_event_time.ts.tv_sec = be64toh(resp->dpa_event_time.ts.tv_sec);
	ts_cfg->dpu_resp.dpa_event_time.ts.tv_nsec = be64toh(resp->dpa_event_time.ts.tv_nsec);
	ts_cfg->dpu_resp.dpu_event_time.ts.tv_sec = be64toh(resp->dpu_event_time.ts.tv_sec);
	ts_cfg->dpu_resp.dpu_event_time.ts.tv_nsec = be64toh(resp->dpu_event_time.ts.tv_nsec);
	ts_cfg->dpu_resp.dpu_event_time_nic.ts.tv_sec = be64toh(resp->dpu_event_time_nic.ts.tv_sec);
	ts_cfg->dpu_resp.dpu_event_time_nic.ts.tv_nsec = be64toh(resp->dpu_event_time_nic.ts.tv_nsec);
	ts_cfg->dpu_resp.dpu_event_error_margin = be64toh(resp->dpu_event_error_margin);

	DOCA_LOG_INFO("Received and processed response from DPU");

	/* Host only expects to receive one message so mark it as finished */
	ts_cfg->finished = 1;
}

doca_error_t time_sync_host_init_comch_client(struct time_sync_cfg *ts_cfg)
{
	union doca_data user_data;
	doca_error_t result;

	result = doca_pe_create(&ts_cfg->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_client_create(ts_cfg->doca_dev, COMCH_NAME, &ts_cfg->client);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create comch client context: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	ts_cfg->comch_ctx = doca_comch_client_as_ctx(ts_cfg->client);

	result = doca_pe_connect_ctx(ts_cfg->pe, ts_cfg->comch_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect comch client to progress engine: %s", doca_error_get_descr(result));
		goto destroy_client;
	}

	result = doca_comch_client_task_send_set_conf(ts_cfg->client,
						      send_task_comp_cb,
						      send_task_comp_err_cb,
						      COMCH_NUM_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed configuring comch tasks: %s", doca_error_get_name(result));
		goto destroy_client;
	}

	result = doca_comch_client_event_msg_recv_register(ts_cfg->client, msg_recv_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed configuring comch recv event callback: %s", doca_error_get_name(result));
		goto destroy_client;
	}

	user_data.ptr = ts_cfg;
	result = doca_ctx_set_user_data(ts_cfg->comch_ctx, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed add user data to context: %s", doca_error_get_name(result));
		goto destroy_client;
	}

	result = doca_ctx_start(ts_cfg->comch_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed start comch client: %s", doca_error_get_name(result));
		goto destroy_client;
	}

	return DOCA_SUCCESS;

destroy_client:
	(void)doca_comch_client_destroy(ts_cfg->client);
	ts_cfg->client = NULL;
	ts_cfg->comch_ctx = NULL;
destroy_pe:
	(void)doca_pe_destroy(ts_cfg->pe);
	ts_cfg->pe = NULL;

	return result;
}

doca_error_t time_sync_host_close_comch_client(struct time_sync_cfg *ts_cfg)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_STOPPING;
	doca_error_t result;

	/* It is assumed that this function is called when the context is ready to be stopped */
	result = doca_ctx_stop(ts_cfg->comch_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to stop comch client context: %s", doca_error_get_name(result));
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

	result = doca_comch_client_destroy(ts_cfg->client);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy comch client: %s", doca_error_get_name(result));
		return result;
	}

	ts_cfg->client = NULL;
	ts_cfg->comch_ctx = NULL;

	result = doca_pe_destroy(ts_cfg->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_name(result));
		return result;
	}

	ts_cfg->pe = NULL;

	return DOCA_SUCCESS;
}

/*
 * Helper function to get the difference in 2 timespecs
 *
 * It is assumed that 'end_time' will be more recent than 'start_time'
 *
 * @end_time [in]: Most recent clock time for difference calculation
 * @start_time [in]: Earlier clock time for difference calculation
 * @diff [out]: Difference between 2 input clocks (end_time - start_time)
 */
static inline void time_sync_subtract_timespecs(struct timespec *end_time,
						struct timespec *start_time,
						struct timespec *diff)
{
	if (start_time->tv_nsec > end_time->tv_nsec) {
		end_time->tv_nsec += NSECS_IN_SEC;
		end_time->tv_sec--;
	}

	diff->tv_sec = end_time->tv_sec - start_time->tv_sec;
	diff->tv_nsec = end_time->tv_nsec - start_time->tv_nsec;
}

/*
 * Helper function to get the NIC time that a dpa event occurred
 *
 * @clock [in]: Initialised doca clock context
 * @dpa_event_time [in]: Time on the DPA clock that an event occurred
 * @nic_event_time [out]: Calculated NIC time that the DPA event occurred
 * @accuracy [out]: Margin of error in the conversion (nanosecs)
 */
static inline doca_error_t time_sync_dpa_to_nic(struct doca_clock *clock,
						union doca_clock_timespec_t dpa_event_time,
						union doca_clock_timespec_t *nic_event_time,
						uint64_t *accuracy)
{
	union doca_clock_timespec_t current_nic, current_dpa;
	struct timespec dpa_time_diff;
	doca_error_t result;

	/* Get a current cross timestamp reading of NIC and DPA clocks */
	result = doca_clock_get_crosstimestamp(clock,
					       DOCA_CLOCK_NIC_REAL_TIME,
					       DOCA_CLOCK_NIC_DPA_TIMER,
					       &current_nic,
					       &current_dpa,
					       accuracy);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cross timestamp NIC and DPA: %s", doca_error_get_name(result));
		return result;
	}

	/* Get the difference between DPA time just read and DPA time when event occurred */
	time_sync_subtract_timespecs(&current_dpa.ts, &dpa_event_time.ts, &dpa_time_diff);

	/*
	 * Difference between the current time and event time on DPA timer will be the same as on the NIC clock.
	 * Event time on the NIC clock is therefore current NIC time minus difference in DPA timestamps.
	 */
	time_sync_subtract_timespecs(&current_nic.ts, &dpa_time_diff, &nic_event_time->ts);

	return DOCA_SUCCESS;
}

static inline void time_sync_add_log_entry(FILE *log,
					   union doca_clock_timespec_t *sync_time,
					   union doca_clock_timespec_t *local_time,
					   uint64_t accuracy,
					   char *description)
{
	fprintf(log,
		"%lu.%09lu, %lu.%09lu, %lu, %s\n",
		sync_time->ts.tv_sec,
		sync_time->ts.tv_nsec,
		local_time->ts.tv_sec,
		local_time->ts.tv_nsec,
		accuracy,
		description);
}

doca_error_t time_sync_host_main_loop(struct time_sync_cfg *ts_cfg)
{
	union doca_clock_timespec_t start_host, start_nic, end_host, end_nic, dpa_nic_time;
	uint64_t start_accuracy_nsec, end_accuracy_nsec, dpa_accuracy;
	enum doca_ctx_states state = DOCA_CTX_STATE_STARTING;
	struct doca_comch_task_send *task;
	struct time_sync_request request;
	FILE *log;
	doca_error_t result;

	/* Progress until the connection attempt issued by doca_ctx_start() is established */
	while (state != DOCA_CTX_STATE_RUNNING) {
		(void)doca_pe_progress(ts_cfg->pe);
		result = doca_ctx_get_state(ts_cfg->comch_ctx, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get connection state: %s", doca_error_get_name(result));
			return result;
		}
	}

	/* Connection is established so extract it for future use */
	result = doca_comch_client_get_connection(ts_cfg->client, &ts_cfg->conn);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get connection from client: %s", doca_error_get_name(result));
		return result;
	}

	/* Populate request message with defined delay time between events - in network order */
	request.delay = htobe32(ts_cfg->delay);

	/* Take time before sending the first message, cross timestamping host and NIC clock */
	result = doca_clock_get_crosstimestamp(ts_cfg->clock,
					       DOCA_CLOCK_HOST_REAL_TIME,
					       DOCA_CLOCK_NIC_REAL_TIME,
					       &start_host,
					       &start_nic,
					       &start_accuracy_nsec);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to do cross-timestamp on events start: %s", doca_error_get_name(result));
		return result;
	}

	/* Allocate task for request message */
	result = doca_comch_client_task_send_alloc_init(ts_cfg->client,
							ts_cfg->conn,
							&request,
							sizeof(struct time_sync_request),
							&task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate task for request message: %s", doca_error_get_name(result));
		return result;
	}

	DOCA_LOG_INFO("Sending request to DPU");

	/* Send request message task */
	result = doca_task_submit(doca_comch_task_send_as_task(task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit send task: %s", doca_error_get_name(result));
		doca_task_free(doca_comch_task_send_as_task(task));
		return result;
	}

	/* Progress until a response is received */
	while (ts_cfg->finished == 0 && ts_cfg->error == DOCA_SUCCESS)
		(void)doca_pe_progress(ts_cfg->pe);

	/* Check progress loop has completed successfully */
	if (ts_cfg->error != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Main loop failed: %s", doca_error_get_name(ts_cfg->error));
		return ts_cfg->error;
	}

	/* Take time after response message has been processed, cross timestamping host and NIC clock */
	result = doca_clock_get_crosstimestamp(ts_cfg->clock,
					       DOCA_CLOCK_HOST_REAL_TIME,
					       DOCA_CLOCK_NIC_REAL_TIME,
					       &end_host,
					       &end_nic,
					       &end_accuracy_nsec);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to do cross-timestamp on events end: %s", doca_error_get_name(result));
		return result;
	}

	/* DPU response contains local DPA event time only - sync it with NIC clock */
	result = time_sync_dpa_to_nic(ts_cfg->clock, ts_cfg->dpu_resp.dpa_event_time, &dpa_nic_time, &dpa_accuracy);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to sync DPA and NIC timestamps: %s", doca_error_get_name(result));
		return result;
	}

	/* Open log file */
	log = fopen("time_sync.log", "w");
	if (log == NULL) {
		DOCA_LOG_ERR("Failed to open log file.");
		return DOCA_ERROR_IO_FAILED;
	}

	/* Add header to log file */
	fprintf(log, "Sync Time - NIC Real Time (secs), Local Time (secs), Sync Margin of Error (nsec), Event\n");

	/* Print the App events in expected order along with time values (all events are sync'd to NIC time) */
	time_sync_add_log_entry(log,
				&start_nic,
				&start_host,
				start_accuracy_nsec,
				"Host sent msg to DPU (Host Real Time)");
	time_sync_add_log_entry(log,
				&ts_cfg->dpu_resp.dpu_event_time_nic,
				&ts_cfg->dpu_resp.dpu_event_time,
				ts_cfg->dpu_resp.dpu_event_error_margin,
				"DPU processed msg (DPU Real Time)");
	time_sync_add_log_entry(log,
				&dpa_nic_time,
				&ts_cfg->dpu_resp.dpa_event_time,
				dpa_accuracy,
				"DPA processed msg (DPA Timer)");
	time_sync_add_log_entry(log,
				&end_nic,
				&end_host,
				end_accuracy_nsec,
				"Host processed DPU response (Host Real Time)");

	fclose(log);

	return DOCA_SUCCESS;
}
