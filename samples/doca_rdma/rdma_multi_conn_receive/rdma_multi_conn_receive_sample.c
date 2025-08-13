/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_error.h>
#include <doca_log.h>
#include <doca_buf_inventory.h>
#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_argp.h>

#include "rdma_common.h"

#define MAX_BUFF_SIZE (256) /* Maximum DOCA buffer size */

DOCA_LOG_REGISTER(RDMA_MULTI_CONN_RECEIVE::SAMPLE);

/*
 * Write the connection details for the sender to read, and read the connection details of the sender
 * To differentiate each local and remote connection details, we append the connection_id so the file-name
 * follows the syntax <local/remote>_connection_desc_path.txt-<connection_id>
 *
 * @cfg [in]: Configuration parameters
 * @resources [in/out]: RDMA resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t write_read_connection(struct rdma_config *cfg,
					  struct rdma_resources *resources,
					  uint32_t connection_id)
{
	doca_error_t result = DOCA_SUCCESS;
	char tmp_file_path[MAX_ARG_SIZE * 2];

	/* Write the RDMA connection details */
	memset(tmp_file_path, 0, sizeof(tmp_file_path));
	sprintf(tmp_file_path, "%s-%04u", cfg->local_connection_desc_path, connection_id);
	result = write_file(tmp_file_path,
			    (char *)resources->rdma_conn_descriptor,
			    resources->rdma_conn_descriptor_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to write the RDMA connection details: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("You can now copy %s to the sender", tmp_file_path);

	memset(tmp_file_path, 0, sizeof(tmp_file_path));
	sprintf(tmp_file_path, "%s-%04u", cfg->remote_connection_desc_path, connection_id);
	DOCA_LOG_INFO("Please copy %s from the sender and then press enter", tmp_file_path);

	/* Wait for enter */
	wait_for_enter();

	/* Read the remote RDMA connection details */
	result = read_file(tmp_file_path,
			   (char **)&resources->remote_rdma_conn_descriptor,
			   &resources->remote_rdma_conn_descriptor_size);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to read the remote RDMA connection details: %s", doca_error_get_descr(result));

	return result;
}

/*
 * RDMA receive task completed callback
 *
 * @rdma_receive_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void rdma_multi_conn_receive_completed_callback(struct doca_rdma_task_receive *rdma_receive_task,
						       union doca_data task_user_data,
						       union doca_data ctx_user_data)
{
	struct rdma_resources *resources = (struct rdma_resources *)ctx_user_data.ptr;
	void *dst_buf_data = NULL;
	doca_error_t *first_encountered_error = (doca_error_t *)task_user_data.ptr;
	doca_error_t result = DOCA_SUCCESS, tmp_result;
	const struct doca_rdma_connection *rdma_connection;
	struct doca_buf *dst_buf = NULL;

	DOCA_LOG_INFO("RDMA receive task was done successfully");

	/* Read the data that was received */
	rdma_connection = doca_rdma_task_receive_get_result_rdma_connection(rdma_receive_task);
	dst_buf = doca_rdma_task_receive_get_dst_buf(rdma_receive_task);
	result = doca_buf_get_data(dst_buf, &dst_buf_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get destination buffer data: %s", doca_error_get_descr(result));
		goto free_task;
	}

	/* Check if dst_buf_data is null terminated and of legal size */
	if (strnlen(dst_buf_data, MAX_BUFF_SIZE) == MAX_BUFF_SIZE) {
		DOCA_LOG_ERR("The message that was received from sender exceeds buffer size %d", MAX_BUFF_SIZE);
		result = DOCA_ERROR_INVALID_VALUE;
		goto free_task;
	}

	DOCA_LOG_INFO("Got from sender: \"%s\", sender's rdma_connection address [%p]",
		      (char *)dst_buf_data,
		      rdma_connection);

free_task:
	tmp_result = doca_buf_dec_refcount(dst_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease dst_buf count: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	doca_task_free(doca_rdma_task_receive_as_task(rdma_receive_task));

	/* Update that an error was encountered, if any */
	DOCA_ERROR_PROPAGATE(*first_encountered_error, tmp_result);

	resources->num_remaining_tasks--;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->rdma_ctx);
}

/*
 * RDMA receive task error callback
 *
 * @rdma_receive_task [in]: failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void rdma_multi_conn_receive_error_callback(struct doca_rdma_task_receive *rdma_receive_task,
						   union doca_data task_user_data,
						   union doca_data ctx_user_data)
{
	struct rdma_resources *resources = (struct rdma_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_rdma_task_receive_as_task(rdma_receive_task);
	doca_error_t *first_encountered_error = (doca_error_t *)task_user_data.ptr;
	struct doca_buf *dst_buf = NULL;
	doca_error_t result;

	/* Update that an error was encountered */
	result = doca_task_get_status(task);
	DOCA_ERROR_PROPAGATE(*first_encountered_error, result);
	DOCA_LOG_ERR("RDMA receive task failed: %s", doca_error_get_descr(result));

	dst_buf = doca_rdma_task_receive_get_dst_buf(rdma_receive_task);
	result = doca_buf_dec_refcount(dst_buf, NULL);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to decrease dst_buf count: %s", doca_error_get_descr(result));

	doca_task_free(task);
	resources->num_remaining_tasks--;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->rdma_ctx);
}

/*
 * Export and receive connection details, and connect to the remote RDMA
 *
 * @resources [in]: RDMA resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_multi_conn_receive_export_and_connect(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;
	uint32_t i = 0;

	if (resources->cfg->use_rdma_cm == true)
		return rdma_cm_connect(resources);

	/* 1-by-1 to setup all the connections */
	for (i = 0; i < resources->cfg->num_connections; i++) {
		DOCA_LOG_INFO("Start to establish RDMA connection [%d]", i);
		/* Export RDMA connection details */
		result = doca_rdma_export(resources->rdma,
					  &(resources->rdma_conn_descriptor),
					  &(resources->rdma_conn_descriptor_size),
					  &(resources->connections[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to export RDMA: %s", doca_error_get_descr(result));
			return result;
		}

		/* write and read connection details to the sender */
		result = write_read_connection(resources->cfg, resources, i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to write and read connection details from sender: %s",
				     doca_error_get_descr(result));
			return result;
		}

		/* Connect RDMA */
		result = doca_rdma_connect(resources->rdma,
					   resources->remote_rdma_conn_descriptor,
					   resources->remote_rdma_conn_descriptor_size,
					   resources->connections[i]);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to connect the receiver's RDMA to the sender's RDMA: %s",
				     doca_error_get_descr(result));

		/* Free remote connection descriptor */
		free(resources->remote_rdma_conn_descriptor);
		resources->remote_rdma_conn_descriptor = NULL;

		DOCA_LOG_INFO("RDMA connection [%d] is establshed", i);
	}
	DOCA_LOG_INFO("All [%d] RDMA connections have been establshed", resources->cfg->num_connections);

	return result;
}

/*
 * Prepare and submit RDMA receive task
 *
 * @resources [in]: RDMA resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_multi_conn_receive_prepare_and_submit_task(struct rdma_resources *resources)
{
	struct doca_rdma_task_receive *rdma_receive_tasks[MAX_NUM_CONNECTIONS] = {0};
	struct doca_buf *dst_bufs[MAX_NUM_CONNECTIONS] = {0};
	union doca_data task_user_data = {0};
	doca_error_t result = DOCA_SUCCESS, tmp_result;
	uint32_t i = 0;

	for (i = 0; i < resources->cfg->num_connections; i++) {
		/* Add dst buffer to DOCA buffer inventory */
		result = doca_buf_inventory_buf_get_by_addr(resources->buf_inventory,
							    resources->mmap,
							    resources->mmap_memrange + i * MAX_BUFF_SIZE,
							    MAX_BUFF_SIZE,
							    &dst_bufs[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate DOCA buffer [%d] to DOCA buffer inventory: %s",
				     i,
				     doca_error_get_descr(result));
			return result;
		}

		/* Include first_encountered_error in user data of task to be used in the callbacks */
		task_user_data.ptr = &(resources->first_encountered_error);
		/* Allocate and construct RDMA receive task */
		result = doca_rdma_task_receive_allocate_init(resources->rdma,
							      dst_bufs[i],
							      task_user_data,
							      &rdma_receive_tasks[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate RDMA receive task [%d]: %s", i, doca_error_get_descr(result));
			goto destroy_dst_buf;
		}

		/* Submit RDMA receive task */
		DOCA_LOG_INFO("Submitting RDMA receive task [%d]", i);
		resources->num_remaining_tasks++;
		result = doca_task_submit(doca_rdma_task_receive_as_task(rdma_receive_tasks[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit RDMA receive task [%d]: %s", i, doca_error_get_descr(result));
			goto free_task;
		}
		DOCA_LOG_INFO("RDMA receive task [%d] successfully submitted", i);
	}
	DOCA_LOG_INFO("All RDMA receive tasks have been successfully submitted");

	return result;

free_task:
	doca_task_free(doca_rdma_task_receive_as_task(rdma_receive_tasks[i]));
destroy_dst_buf:
	tmp_result = doca_buf_dec_refcount(dst_bufs[i], NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease dst_buf count: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*
 * RDMA receive state change callback
 * This function represents the state machine for this RDMA program
 *
 * @user_data [in]: doca_data from the context
 * @ctx [in]: DOCA context
 * @prev_state [in]: Previous DOCA context state
 * @next_state [in]: Next DOCA context state
 */
static void rdma_multi_conn_receive_state_change_callback(const union doca_data user_data,
							  struct doca_ctx *ctx,
							  enum doca_ctx_states prev_state,
							  enum doca_ctx_states next_state)
{
	struct rdma_resources *resources = (struct rdma_resources *)user_data.ptr;
	struct rdma_config *cfg = resources->cfg;
	doca_error_t result = DOCA_SUCCESS;
	(void)prev_state;
	(void)ctx;

	switch (next_state) {
	case DOCA_CTX_STATE_STARTING:
		DOCA_LOG_INFO("RDMA context entered starting state");
		break;
	case DOCA_CTX_STATE_RUNNING:
		DOCA_LOG_INFO("RDMA context is running");

		result = rdma_multi_conn_receive_export_and_connect(resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("rdma_multi_conn_receive_export_and_connect() failed: %s",
				     doca_error_get_descr(result));
			break;
		} else
			DOCA_LOG_INFO("RDMA context finished initialization");

		if (cfg->use_rdma_cm == true)
			break;

		result = rdma_multi_conn_receive_prepare_and_submit_task(resources);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("rdma_multi_conn_receive_prepare_and_submit_task() failed: %s",
				     doca_error_get_descr(result));
		break;
	case DOCA_CTX_STATE_STOPPING:
		/**
		 * doca_ctx_stop() has been called.
		 * In this sample, this happens either due to a failure encountered, in which case doca_pe_progress()
		 * will cause any inflight task to be flushed, or due to the successful compilation of the sample flow.
		 * In both cases, in this sample, doca_pe_progress() will eventually transition the context to idle
		 * state.
		 */
		DOCA_LOG_INFO("RDMA context entered into stopping state. Any inflight tasks will be flushed");

		if (resources->cfg->use_rdma_cm == true)
			(void)rdma_cm_disconnect(resources);
		break;
	case DOCA_CTX_STATE_IDLE:
		DOCA_LOG_INFO("RDMA context has been stopped");

		/* We can stop progressing the PE */
		resources->run_pe_progress = false;
		break;
	default:
		break;
	}

	/* If something failed - update that an error was encountered and stop the ctx */
	if (result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(resources->first_encountered_error, result);
		(void)doca_ctx_stop(ctx);
	}
}

/*
 * Receive a message from the sender
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_multi_conn_receive(struct rdma_config *cfg)
{
	struct rdma_resources resources = {0};
	union doca_data ctx_user_data = {0};
	uint32_t mmap_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE;
	uint32_t rdma_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result, tmp_result;

	/* Allocating resources */
	result = allocate_rdma_resources(cfg,
					 mmap_permissions,
					 rdma_permissions,
					 doca_rdma_cap_task_receive_is_supported,
					 &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate RDMA Resources: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_rdma_task_receive_set_conf(resources.rdma,
						 rdma_multi_conn_receive_completed_callback,
						 rdma_multi_conn_receive_error_callback,
						 NUM_RDMA_TASKS * cfg->num_connections);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set configurations for RDMA receive task: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	result = doca_ctx_set_state_changed_cb(resources.rdma_ctx, rdma_multi_conn_receive_state_change_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set state change callback for RDMA context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	/* Include the program's resources in user data of context to be used in callbacks */
	ctx_user_data.ptr = &(resources);
	result = doca_ctx_set_user_data(resources.rdma_ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set context user data: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	/* Create DOCA buffer inventory */
	result = doca_buf_inventory_create(INVENTORY_NUM_INITIAL_ELEMENTS * cfg->num_connections,
					   &resources.buf_inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA buffer inventory: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	/* Start DOCA buffer inventory */
	result = doca_buf_inventory_start(resources.buf_inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA buffer inventory: %s", doca_error_get_descr(result));
		goto destroy_buf_inventory;
	}

	if (cfg->use_rdma_cm == true) {
		/* Set rdma cm connection configuration callbacks */
		resources.require_remote_mmap = false;
		resources.task_fn = rdma_multi_conn_receive_prepare_and_submit_task;
		result = config_rdma_cm_callback_and_negotiation_task(&resources,
								      /* need_send_mmap_info */ false,
								      /* need_recv_mmap_info */ false);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to config RDMA CM callbacks and negotiation functions: %s",
				     doca_error_get_descr(result));
			goto destroy_buf_inventory;
		}
	}

	/* Start RDMA context */
	result = doca_ctx_start(resources.rdma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start RDMA context: %s", doca_error_get_descr(result));
		goto stop_buf_inventory;
	}

	/*
	 * Run the progress engine which will run the state machine defined in rdma_receive_state_change_callback()
	 * When the context moves to idle, the context change callback call will signal to stop running the progress
	 * engine.
	 */
	while (resources.run_pe_progress) {
		if (doca_pe_progress(resources.pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Assign the result we update in the callbacks */
	result = resources.first_encountered_error;

stop_buf_inventory:
	tmp_result = doca_buf_inventory_stop(resources.buf_inventory);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA buffer inventory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_buf_inventory:
	tmp_result = doca_buf_inventory_destroy(resources.buf_inventory);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA buffer inventory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_resources:
	tmp_result = destroy_rdma_resources(&resources, cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA resources: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}
