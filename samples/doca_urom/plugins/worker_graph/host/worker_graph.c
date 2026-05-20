/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <string.h>

#include <doca_buf.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "urom_graph.h"
#include "worker_graph.h"

DOCA_LOG_REGISTER(UROM::WORKER::GRAPH);

static uint64_t graph_id;	      /* Graph plugin id, will be set in init function */
static uint64_t graph_version = 0x01; /* Graph plugin version */

/* Graph task metadata */
struct doca_graph_task_data {
	union doca_data cookie;		      /* User cookie */
	urom_graph_loopback_finished user_cb; /* User callback */
};

/*
 * Packs graph commands
 *
 * @graph_cmd [in]: internal graph command to pack
 * @packed_cmd_len [in/out]: packed buffer size, at the end will store packed command size
 * @packed_cmd [out]: Store graph packed command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t urom_worker_graph_cmd_pack(struct urom_worker_graph_cmd *graph_cmd,
					       size_t *packed_cmd_len,
					       void *packed_cmd)
{
	size_t pack_len;
	void *pack_head = packed_cmd;

	pack_len = sizeof(struct urom_worker_graph_cmd);
	if (pack_len > *packed_cmd_len)
		return DOCA_ERROR_INITIALIZATION;

	/* pack base command */
	memcpy(pack_head, graph_cmd, pack_len);
	*packed_cmd_len = pack_len;
	return DOCA_SUCCESS;
}

/*
 * Graph commands completion callback function
 *
 * @task [in]: UROM worker task
 * @task_user_data [in]: task user data
 * @ctx_user_data [in]: worker context user data
 */
static void urom_graph_completed(struct doca_urom_worker_cmd_task *task,
				 union doca_data task_user_data,
				 union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	size_t data_len;
	doca_error_t result;
	struct doca_buf *response;
	struct urom_worker_notify_graph notify_error = {0};
	struct urom_worker_notify_graph *graph_notify = &notify_error;
	struct doca_graph_task_data *task_data;

	task_data = (struct doca_graph_task_data *)doca_urom_worker_cmd_task_get_user_data(task);
	if (task_data == NULL) {
		DOCA_LOG_ERR("Failed to get task data buffer");
		result = DOCA_ERROR_INVALID_VALUE;
		goto error_exit;
	}

	response = doca_urom_worker_cmd_task_get_response(task);
	if (response == NULL) {
		DOCA_LOG_ERR("Failed to get task response buffer");
		result = DOCA_ERROR_INVALID_VALUE;
		goto error_exit;
	}

	result = doca_buf_get_data_len(response, &data_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get response data length");
		goto error_exit;
	}

	if (data_len != sizeof(*graph_notify)) {
		DOCA_LOG_ERR("Task response data length is different from notification expected length");
		result = DOCA_ERROR_INVALID_VALUE;
		goto error_exit;
	}

	result = doca_buf_get_data(response, (void **)&graph_notify);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get response data");
		goto error_exit;
	}

	result = doca_task_get_status(doca_urom_worker_cmd_task_as_task(task));
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Bad worker command task status %s", doca_error_get_descr(result));

error_exit:
	(task_data->user_cb)(result, task_data->cookie, graph_notify->loopback.data);
	result = doca_urom_worker_cmd_task_release(task);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to release worker command task %s", doca_error_get_descr(result));
}

doca_error_t urom_graph_task_loopback(struct doca_urom_worker *worker_ctx,
				      union doca_data cookie,
				      uint64_t data,
				      urom_graph_loopback_finished cb)
{
	size_t cmd_len = 0;
	doca_error_t tmp_result, result;
	struct doca_buf *payload;
	struct doca_urom_worker_cmd_task *task;
	struct doca_graph_task_data *task_data;
	struct urom_worker_graph_cmd *graph_cmd;

	/* Allocate task */
	result = doca_urom_worker_cmd_task_allocate_init(worker_ctx, graph_id, &task);
	if (result != DOCA_SUCCESS)
		return result;

	payload = doca_urom_worker_cmd_task_get_payload(task);
	result = doca_buf_get_data(payload, (void **)&graph_cmd);
	if (result != DOCA_SUCCESS)
		goto task_destroy;

	result = doca_buf_get_data_len(payload, &cmd_len);
	if (result != DOCA_SUCCESS)
		goto task_destroy;

	/* Populate commands attributes */
	graph_cmd->type = UROM_WORKER_CMD_GRAPH_LOOPBACK;
	graph_cmd->loopback.data = data;

	result = urom_worker_graph_cmd_pack(graph_cmd, &cmd_len, (void *)graph_cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to pack graph command");
		goto task_destroy;
	}

	result = doca_buf_set_data(payload, graph_cmd, cmd_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set task command data");
		goto task_destroy;
	}

	task_data = (struct doca_graph_task_data *)doca_urom_worker_cmd_task_get_user_data(task);
	task_data->user_cb = cb;
	task_data->cookie = cookie;

	doca_urom_worker_cmd_task_set_cb(task, urom_graph_completed);
	result = doca_task_submit(doca_urom_worker_cmd_task_as_task(task));
	if (result != DOCA_SUCCESS)
		goto task_destroy;

	return DOCA_SUCCESS;
task_destroy:
	tmp_result = doca_urom_worker_cmd_task_release(task);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to release worker command task, error [%s]", doca_error_get_descr(tmp_result));
	return result;
}

doca_error_t urom_graph_init(uint64_t plugin_id, uint64_t version)
{
	if (version != graph_version)
		return DOCA_ERROR_UNSUPPORTED_VERSION;

	graph_id = plugin_id;
	return DOCA_SUCCESS;
}
