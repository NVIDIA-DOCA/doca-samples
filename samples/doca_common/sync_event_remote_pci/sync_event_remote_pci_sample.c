/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>

#include <doca_log.h>
#include <doca_comch.h>
#include <doca_sync_event.h>

#include <common.h>

#include "common_common.h"

DOCA_LOG_REGISTER(SYNC_EVENT::SAMPLE);

/* Information exported from local sample via comch message */
struct export_blob {
	char se_blob[SYNC_EVENT_CC_MAX_MSG_SIZE]; /* Data received via comch */
	size_t se_blob_sz;			  /* Length of data received */
};

/*
 * DOCA device with create-doca-sync-event-from-export capability filter callback
 *
 * @devinfo [in]: doca_devinfo
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t sync_event_get_create_from_export_supported(struct doca_devinfo *devinfo)
{
	return doca_sync_event_cap_is_create_from_export_supported(devinfo);
}

/*
 * Initialize sample's DOCA Sync Event
 *
 * @export_data [in]: data to be populated by comch message callback
 * @se_rt_objs [in/out]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t se_init(struct export_blob *export_data, struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;
	int timeout = SYNC_EVENT_CC_TIMEOUT_SEC;

	DOCA_LOG_INFO("Listening");
	/* Wait for comch callback to populate the export data fields */
	while (export_data->se_blob_sz == 0) {
		if (timeout == 0) {
			DOCA_LOG_ERR("Failed to retrieve set task progress: timeout");
			return DOCA_ERROR_TIME_OUT;
		}
		sleep(1);
		timeout--;
		(void)doca_pe_progress(se_rt_objs->comch_pe);
	}

	DOCA_LOG_INFO("Received blob from remote side");

	result = doca_sync_event_create_from_export(se_rt_objs->dev,
						    (const uint8_t *)export_data->se_blob,
						    export_data->se_blob_sz,
						    &se_rt_objs->se);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA Sync Event from export: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Communicate with remote side through DOCA Sync Event in synchronous mode
 *
 * @se_cfg [in]: user configuration represents command line arguments
 * @se_rt_objs [in]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t se_communicate_sync(const struct sync_event_config *se_cfg,
					struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;
	uint64_t fetched = 0;

	uint64_t se_value = 1;

	DOCA_LOG_INFO("Waiting for sync event to be signaled from remote side");
	result = doca_sync_event_wait_eq(se_rt_objs->se, se_value, UINT64_MAX);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to wait for sync event: %s", doca_error_get_descr(result));
		return result;
	}

	se_value++;

	DOCA_LOG_INFO("Signaling sync event for remote side");
	if (se_cfg->is_update_atomic)
		result = doca_sync_event_update_add(se_rt_objs->se, 1, &fetched);
	else
		result = doca_sync_event_update_set(se_rt_objs->se, se_value);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to signal sync event: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Done");

	return DOCA_SUCCESS;
}

/*
 * Communicate with remote side through DOCA Sync Event in asynchronous mode
 *
 * @se_cfg [in]: user configuration represents command line arguments
 * @se_rt_objs [in]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t se_communicate_async(const struct sync_event_config *se_cfg,
					 struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;
	struct doca_sync_event_task_wait_eq *wait_eq_task;
	struct doca_sync_event_task_notify_add *notify_add_task;
	struct doca_sync_event_task_notify_set *notify_set_task;
	uint64_t fetched = 0;
	union doca_data user_data;

	user_data.u64 = 0;

	uint64_t se_value = 1;

	result =
		doca_sync_event_task_wait_eq_alloc_init(se_rt_objs->se, se_value, UINT64_MAX, user_data, &wait_eq_task);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("Waiting for sync event to be signaled from remote side");
	result = sync_event_async_task_submit(se_rt_objs, doca_sync_event_task_wait_eq_as_doca_task(wait_eq_task));
	if (result != DOCA_SUCCESS)
		goto free_wait_eq_task;

	se_value++;

	DOCA_LOG_INFO("Signaling sync event for remote side");
	if (se_cfg->is_update_atomic) {
		result = doca_sync_event_task_notify_add_alloc_init(se_rt_objs->se,
								    1,
								    &fetched,
								    user_data,
								    &notify_add_task);
		if (result != DOCA_SUCCESS)
			goto free_wait_eq_task;

		result = sync_event_async_task_submit(se_rt_objs,
						      doca_sync_event_task_notify_add_as_doca_task(notify_add_task));

		doca_task_free(doca_sync_event_task_notify_add_as_doca_task(notify_add_task));

		if (result != DOCA_SUCCESS)
			goto free_wait_eq_task;
	} else {
		result = doca_sync_event_task_notify_set_alloc_init(se_rt_objs->se,
								    se_value,
								    user_data,
								    &notify_set_task);
		if (result != DOCA_SUCCESS)
			goto free_wait_eq_task;

		result = sync_event_async_task_submit(se_rt_objs,
						      doca_sync_event_task_notify_set_as_doca_task(notify_set_task));

		doca_task_free(doca_sync_event_task_notify_set_as_doca_task(notify_set_task));

		if (result != DOCA_SUCCESS)
			goto free_wait_eq_task;
	}

	DOCA_LOG_INFO("Done");

	result = DOCA_SUCCESS;

free_wait_eq_task:
	doca_task_free(doca_sync_event_task_wait_eq_as_doca_task(wait_eq_task));

	return result;
}

/*
 * Callback event comch messages
 *
 * @event [in]: message receive event
 * @recv_buffer [in]: array of bytes containing the message data
 * @msg_len [in]: number of bytes in the recv_buffer
 * @comch_connection [in]: comm channel connection over which the event occurred
 */
static void comch_recv_event_cb(struct doca_comch_event_msg_recv *event,
				uint8_t *recv_buffer,
				uint32_t msg_len,
				struct doca_comch_connection *comch_connection)
{
	struct sync_event_runtime_objects *se_rt_objs;
	struct export_blob *recv_data;
	union doca_data ctx_user_data;
	struct doca_ctx *ctx;
	doca_error_t result;
#ifdef DOCA_ARCH_DPU
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(comch_connection);

	ctx = doca_comch_server_as_ctx(server);
#else
	struct doca_comch_client *client = doca_comch_client_get_client_ctx(comch_connection);

	ctx = doca_comch_client_as_ctx(client);
#endif

	(void)event;

	result = doca_ctx_get_user_data(ctx, &ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from doca context: %s", doca_error_get_descr(result));
		return;
	}

	se_rt_objs = (struct sync_event_runtime_objects *)ctx_user_data.ptr;
	recv_data = (struct export_blob *)se_rt_objs->user_data;

	memcpy(recv_data->se_blob, recv_buffer, msg_len);
	recv_data->se_blob_sz = msg_len;
}

/*
 * Sample's logic
 *
 * @se_cfg [in]: user configuration represents command line arguments
 * @se_rt_objs [in/out]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_run(const struct sync_event_config *se_cfg, struct sync_event_runtime_objects *se_rt_objs)
{
	struct export_blob received_data = {0};
	doca_error_t result = DOCA_SUCCESS;

	result = open_doca_device_with_pci(se_cfg->dev_pci_addr,
					   sync_event_get_create_from_export_supported,
					   &se_rt_objs->dev);
	if (result != DOCA_SUCCESS) {
		sync_event_tear_down(se_rt_objs);
		return result;
	}

#ifdef DOCA_ARCH_DPU
	result = open_doca_device_rep_with_pci(se_rt_objs->dev,
					       DOCA_DEVINFO_REP_FILTER_NET,
					       se_cfg->rep_pci_addr,
					       &se_rt_objs->rep);
	if (result != DOCA_SUCCESS) {
		sync_event_tear_down(se_rt_objs);
		return result;
	}
#endif

	result = sync_event_config_validate(se_cfg, se_rt_objs);
	if (result != DOCA_SUCCESS) {
		sync_event_tear_down(se_rt_objs);
		return result;
	}

	/* Set sample specific cb message recv callback even for comch */
	se_rt_objs->comch_recv_event_cb = comch_recv_event_cb;

	/* Set user data to populate in callback on message recv */
	se_rt_objs->user_data = &received_data;

	result = sync_event_cc_handshake(se_rt_objs);
	if (result != DOCA_SUCCESS) {
		sync_event_tear_down(se_rt_objs);
		return result;
	}

	result = se_init(&received_data, se_rt_objs);
	if (result != DOCA_SUCCESS) {
		sync_event_tear_down(se_rt_objs);
		return result;
	}

	if (se_cfg->is_async_mode)
		result = sync_event_start_async(se_cfg, se_rt_objs);
	else
		result = doca_sync_event_start(se_rt_objs->se);

	if (result != DOCA_SUCCESS) {
		sync_event_tear_down(se_rt_objs);
		return result;
	}

	if (se_cfg->is_async_mode)
		result = se_communicate_async(se_cfg, se_rt_objs);
	else
		result = se_communicate_sync(se_cfg, se_rt_objs);

	sync_event_tear_down(se_rt_objs);

	return result;
}
