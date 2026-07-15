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

#include <unistd.h>
#include <time.h>

#include <doca_log.h>
#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include "common_common.h"

DOCA_LOG_REGISTER(SYNC_EVENT::COMMON);

#define SLEEP_IN_NANOS (10 * 1000)	  /* Sample the task every 10 microseconds */
#define TIMEOUT_IN_NANOS (1 * 1000000000) /* Poll the task for maximum of 1 second */

/**
 * Common DOCA Sync Event task callback for successful completion.
 *
 * @se_task [in]: the successfully completed sync event task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_completion_cb(struct doca_task *se_task, union doca_data task_user_data, union doca_data ctx_user_data)
{
	(void)se_task;
	(void)task_user_data;

	struct sync_event_runtime_objects *rt_objs = (struct sync_event_runtime_objects *)ctx_user_data.ptr;

	rt_objs->se_task_result = DOCA_SUCCESS;
}

/**
 * Common DOCA Sync Event task callback for completion with error.
 *
 * @se_task [in]: the successfully completed sync event task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_error_cb(struct doca_task *se_task, union doca_data task_user_data, union doca_data ctx_user_data)
{
	(void)se_task;
	(void)task_user_data;

	struct sync_event_runtime_objects *rt_objs = (struct sync_event_runtime_objects *)ctx_user_data.ptr;

	rt_objs->se_task_result = DOCA_ERROR_UNKNOWN;
}

/**
 * DOCA Sync Event get task callback for successful completion.
 *
 * See doca_sync_event_task_get_completion_cb_t doc.
 *
 * @task [in]: the successfully completed sync event get task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_get_completion_cb(struct doca_sync_event_task_get *task,
				   union doca_data task_user_data,
				   union doca_data ctx_user_data)
{
	task_completion_cb(doca_sync_event_task_get_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event get task callback for completion with error.
 *
 * See doca_sync_event_task_get_completion_cb_t doc.
 *
 * @task [in]: the completed-with-error sync event get task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_get_error_cb(struct doca_sync_event_task_get *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	task_error_cb(doca_sync_event_task_get_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event notify-set task callback for successful completion.
 *
 * See doca_sync_event_task_notify_set_completion_cb_t doc.
 *
 * @task [in]: the successfully completed sync event notify-set task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_notify_set_completion_cb(struct doca_sync_event_task_notify_set *task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	task_completion_cb(doca_sync_event_task_notify_set_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event notify-set task callback for completion with error.
 *
 * See doca_sync_event_task_notify_set_completion_cb_t doc.
 *
 * @task [in]: the completed-with-error sync event notify-set task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_notify_set_error_cb(struct doca_sync_event_task_notify_set *task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	task_error_cb(doca_sync_event_task_notify_set_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event notify-add task callback for successful completion.
 *
 * See doca_sync_event_task_notify_add_completion_cb_t doc.
 *
 * @task [in]: the successfully completed sync event notify-add task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_notify_add_completion_cb(struct doca_sync_event_task_notify_add *task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	task_completion_cb(doca_sync_event_task_notify_add_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event notify-add task callback for completion with error.
 *
 * See doca_sync_event_task_notify_add_completion_cb_t doc.
 *
 * @task [in]: the completed-with-error sync event notify-add task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_notify_add_error_cb(struct doca_sync_event_task_notify_add *task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	task_error_cb(doca_sync_event_task_notify_add_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event wait-eq task callback for successful completion.
 *
 * See doca_sync_event_task_wait_eq_completion_cb_t doc.
 *
 * @task [in]: The successfully completed sync event wait_eq task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_wait_eq_completion_cb(struct doca_sync_event_task_wait_eq *task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	task_completion_cb(doca_sync_event_task_wait_eq_as_doca_task(task), task_user_data, ctx_user_data);
}

/**
 * DOCA Sync Event wait-eq task callback for completion with error.
 *
 * See doca_sync_event_task_wait_eq_completion_cb_t doc.
 *
 * @task [in]: The completed-with-error sync event wait-eq task. Implementation can assume it is not NULL.
 * @task_user_data [in]: task's user data which was previously set.
 * @ctx_user_data [in]: context's user data which was previously set.
 */
static void task_wait_eq_error_cb(struct doca_sync_event_task_wait_eq *task,
				  union doca_data task_user_data,
				  union doca_data ctx_user_data)
{
	task_error_cb(doca_sync_event_task_wait_eq_as_doca_task(task), task_user_data, ctx_user_data);
}

/*
 * common helper for copying PCI address user input
 *
 * @pci_addr_src [in]: input PCI address string
 * @pci_addr_dest [out]: destination PCI address string buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t pci_addr_cb(const char *pci_addr_src, char pci_addr_dest[DOCA_DEVINFO_PCI_ADDR_SIZE])
{
	int len = strnlen(pci_addr_src, DOCA_DEVINFO_PCI_ADDR_SIZE);

	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("PCI address exceeding the maximum size of %d", DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(pci_addr_dest, pci_addr_src, len + 1);

	return DOCA_SUCCESS;
}

/*
 * argp callback - handle local device PCI address parameter
 *
 * @param [in]: input parameter
 * @config [in/out]: program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t dev_pci_addr_cb(void *param, void *config)
{
	return pci_addr_cb((char *)param, ((struct sync_event_config *)config)->dev_pci_addr);
}

/*
 * argp callback - handle DPU representor PCI address parameter
 *
 * @param [in]: input parameter
 * @config [in/out]: program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t rep_pci_addr_cb(void *param, void *config)
{
	return pci_addr_cb((char *)param, ((struct sync_event_config *)config)->rep_pci_addr);
}

/*
 * argp callback - handle sync event asynchronous mode parameter
 *
 * @param [in]: input parameter
 * @config [in/out]: program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t is_async_mode_cb(void *param, void *config)
{
	(void)(param);

	((struct sync_event_config *)config)->is_async_mode = true;

	return DOCA_SUCCESS;
}

/*
 * argp callback - handle sync event asynchronous number of tasks
 *
 * @param [in]: input parameter
 * @config [in/out]: program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t async_num_tasks_cb(void *param, void *config)
{
	((struct sync_event_config *)config)->async_num_tasks = *(int *)param;

	return DOCA_SUCCESS;
}

/*
 * argp callback - handle sync event atomic parameter
 *
 * @param [in]: input parameter
 * @config [in/out]: program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t is_update_atomic_cb(void *param, void *config)
{
	(void)(param);

	((struct sync_event_config *)config)->is_update_atomic = true;

	return DOCA_SUCCESS;
}

/*
 * Register command line parameters for DOCA Sync Event sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_params_register(void)
{
	doca_error_t result;
	struct doca_argp_param *dev_pci_addr_param = NULL, *is_async_mode_param = NULL, *async_num_tasks = NULL,
			       *is_update_atomic = NULL;

	result = doca_argp_param_create(&dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create dev-pci-addr param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(dev_pci_addr_param, "d");
	doca_argp_param_set_long_name(dev_pci_addr_param, "pci-addr");
	doca_argp_param_set_description(dev_pci_addr_param, "Device PCI address");
	doca_argp_param_set_mandatory(dev_pci_addr_param);
	doca_argp_param_set_callback(dev_pci_addr_param, dev_pci_addr_cb);
	doca_argp_param_set_type(dev_pci_addr_param, DOCA_ARGP_TYPE_STRING);

	result = doca_argp_register_param(dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register pci-addr param: %s", doca_error_get_descr(result));
		return result;
	}

#ifdef DOCA_ARCH_DPU
	struct doca_argp_param *rep_pci_addr_param = NULL;

	result = doca_argp_param_create(&rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rep-pci-addr param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(rep_pci_addr_param, "r");
	doca_argp_param_set_long_name(rep_pci_addr_param, "rep-pci");
	doca_argp_param_set_description(rep_pci_addr_param, "DPU representor PCI address");
	doca_argp_param_set_mandatory(rep_pci_addr_param);
	doca_argp_param_set_callback(rep_pci_addr_param, rep_pci_addr_cb);
	doca_argp_param_set_type(rep_pci_addr_param, DOCA_ARGP_TYPE_STRING);

	result = doca_argp_register_param(rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register rep-pci param: %s", doca_error_get_descr(result));
		return result;
	}
#endif

	result = doca_argp_param_create(&is_async_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create async param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(is_async_mode_param, "async");
	doca_argp_param_set_description(is_async_mode_param,
					"Start DOCA Sync Event in asynchronous mode (synchronous mode by default)");
	doca_argp_param_set_callback(is_async_mode_param, is_async_mode_cb);
	doca_argp_param_set_type(is_async_mode_param, DOCA_ARGP_TYPE_BOOLEAN);

	result = doca_argp_register_param(is_async_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register async param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&async_num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create async num tasks param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(async_num_tasks, "async-num-tasks");
	doca_argp_param_set_description(async_num_tasks, "Async num tasks for asynchronous mode");
	doca_argp_param_set_callback(async_num_tasks, async_num_tasks_cb);
	doca_argp_param_set_type(async_num_tasks, DOCA_ARGP_TYPE_INT);

	result = doca_argp_register_param(async_num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register async num tasks param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&is_update_atomic);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create atomic param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(is_update_atomic, "atomic");
	doca_argp_param_set_description(is_update_atomic,
					"Update DOCA Sync Event using Add operation (Set operation by default)");
	doca_argp_param_set_callback(is_update_atomic, is_update_atomic_cb);
	doca_argp_param_set_type(is_update_atomic, DOCA_ARGP_TYPE_BOOLEAN);

	result = doca_argp_register_param(is_update_atomic);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register atomic param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Validate configured flow by user input
 *
 * @se_cfg [in]: user configuration represents command line arguments
 * @se_rt_objs [in]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_config_validate(const struct sync_event_config *se_cfg,
					const struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;

	if (!se_cfg->is_async_mode)
		return DOCA_SUCCESS;

	result = doca_sync_event_cap_task_wait_eq_is_supported(doca_dev_as_devinfo(se_rt_objs->dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DOCA Sync Event asynchronous wait is not supported (%s) on the given device",
			     doca_error_get_descr(result));
		return result;
	}

	if (((int)(se_cfg->async_num_tasks) < 0) || (se_cfg->async_num_tasks > UINT32_MAX)) {
		DOCA_LOG_ERR("Please specify num async tasksin the range [0, 4294967295] (asynchronous mode)");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Start Sample's DOCA Sync Event in asynchronous operation mode
 *
 * @se_cfg [in]: user configuration represents command line arguments
 * @se_rt_objs [in]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_start_async(const struct sync_event_config *se_cfg,
				    struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;

	se_rt_objs->se_ctx = doca_sync_event_as_ctx(se_rt_objs->se);
	if (se_rt_objs->se_ctx == NULL) {
		DOCA_LOG_ERR("Failed to convert sync event to ctx");
		return result;
	}

	result = doca_pe_create(&se_rt_objs->se_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca pe: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(se_rt_objs->se_pe, se_rt_objs->se_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind pe with sync event: %s", doca_error_get_descr(result));
		return result;
	}

	union doca_data ctx_user_data = {.ptr = (void *)se_rt_objs};

	result = doca_ctx_set_user_data(se_rt_objs->se_ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set user data for se ctx: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_task_get_set_conf(se_rt_objs->se,
						   task_get_completion_cb,
						   task_get_error_cb,
						   se_cfg->async_num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set get task configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_task_notify_set_set_conf(se_rt_objs->se,
							  task_notify_set_completion_cb,
							  task_notify_set_error_cb,
							  se_cfg->async_num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set notify set task configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_task_notify_add_set_conf(se_rt_objs->se,
							  task_notify_add_completion_cb,
							  task_notify_add_error_cb,
							  se_cfg->async_num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add notify set task configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_task_wait_eq_set_conf(se_rt_objs->se,
						       task_wait_eq_completion_cb,
						       task_wait_eq_error_cb,
						       se_cfg->async_num_tasks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set wait-grater-than task configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(se_rt_objs->se_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start sync event ctx: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Callback for send completions
 *
 * Frees the allocated task that was used for the send
 *
 * @task [in]: send task that has completed
 * @task_user_data [in]: user data of task
 * @ctx_user_data [in]: user data of doca context
 */
static void comch_send_completion_cb(struct doca_comch_task_send *task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	doca_task_free(doca_comch_task_send_as_task(task));
}

/*
 * Callback for send completion error
 *
 * Frees the allocated task that was used for the send
 * Unexpected code path so logs an error
 *
 * @task [in]: send task that has completed
 * @task_user_data [in]: user data of task
 * @ctx_user_data [in]: user data of doca context
 */
static void comch_send_completion_err_cb(struct doca_comch_task_send *task,
					 union doca_data task_user_data,
					 union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	doca_task_free(doca_comch_task_send_as_task(task));
	DOCA_LOG_ERR("Send Task got a completion error");
}

#ifdef DOCA_ARCH_DPU
/*
 * Callback for new server connection
 *
 * @event [in]: connection event
 * @comch_connection [in]: doca connection that triggered the event
 * @change_successful [in]: indicator of change success
 */
static void comch_server_connection_cb(struct doca_comch_event_connection_status_changed *event,
				       struct doca_comch_connection *comch_connection,
				       uint8_t change_successful)
{
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(comch_connection);
	struct sync_event_runtime_objects *se_rt_objs;
	union doca_data ctx_user_data;
	doca_error_t result;

	(void)event;
	(void)change_successful;

	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from server context: %s", doca_error_get_descr(result));
		return;
	}

	se_rt_objs = (struct sync_event_runtime_objects *)ctx_user_data.ptr;
	se_rt_objs->comch_connection = comch_connection;

	DOCA_LOG_INFO("Server received a new connection");
}

/*
 * Callback for server disconnection
 *
 * @event [in]: connection event
 * @comch_connection [in]: doca connection that triggered the event
 * @change_successful [in]: indicator of change success
 */
static void comch_server_disconnection_cb(struct doca_comch_event_connection_status_changed *event,
					  struct doca_comch_connection *comch_connection,
					  uint8_t change_successful)
{
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(comch_connection);
	struct sync_event_runtime_objects *se_rt_objs;
	union doca_data ctx_user_data;
	doca_error_t result;

	(void)event;
	(void)change_successful;

	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from server context: %s", doca_error_get_descr(result));
		return;
	}

	se_rt_objs = (struct sync_event_runtime_objects *)ctx_user_data.ptr;
	se_rt_objs->comch_connection = NULL;

	DOCA_LOG_INFO("Client has disconnected from server");
}

/*
 * Initialize Sample's DOCA comch
 *
 * @se_rt_objs [in/out]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sync_event_cc_init(struct sync_event_runtime_objects *se_rt_objs)
{
	union doca_data user_data = {0};
	doca_error_t result = DOCA_SUCCESS;

	result = doca_pe_create(&se_rt_objs->comch_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to progress engine for comch: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_create(se_rt_objs->dev,
					  se_rt_objs->rep,
					  SYNC_EVENT_CC_SERVICE_NAME,
					  &se_rt_objs->server);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create comch server: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_set_max_msg_size(se_rt_objs->server, SYNC_EVENT_CC_MAX_MSG_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed set max message size of server: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_set_recv_queue_size(se_rt_objs->server, SYNC_EVENT_CC_MAX_QUEUE_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed set server recv queue size: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(se_rt_objs->comch_pe, doca_comch_server_as_ctx(se_rt_objs->server));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect comch server context to progress engine: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_task_send_set_conf(se_rt_objs->server,
						      comch_send_completion_cb,
						      comch_send_completion_err_cb,
						      SYNC_EVENT_CC_MAX_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure server task pool: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_event_msg_recv_register(se_rt_objs->server, se_rt_objs->comch_recv_event_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register comch server receive event callback: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_server_event_connection_status_changed_register(se_rt_objs->server,
									    comch_server_connection_cb,
									    comch_server_disconnection_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register comch server event callback: %s", doca_error_get_descr(result));
		return result;
	}

	user_data.ptr = se_rt_objs;
	doca_ctx_set_user_data(doca_comch_server_as_ctx(se_rt_objs->server), user_data);

	return DOCA_SUCCESS;
}

/*
 * Establish a connection on DOCA comch
 *
 * @se_rt_objs [in/out]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_cc_handshake(struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;
	int timeout = SYNC_EVENT_CC_TIMEOUT_SEC;

	result = sync_event_cc_init(se_rt_objs);
	if (result != DOCA_SUCCESS)
		return result;

	se_rt_objs->comch_connection = NULL;

	result = doca_ctx_start(doca_comch_server_as_ctx(se_rt_objs->server));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start comch server context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Connection will be populated by the cb when a single client connects */
	while (se_rt_objs->comch_connection == NULL) {
		sleep(1);
		timeout--;

		if (timeout == 0) {
			DOCA_LOG_ERR("Failed to receive connection from host: timeout");
			return DOCA_ERROR_TIME_OUT;
		}
		(void)doca_pe_progress(se_rt_objs->comch_pe);
	}

	DOCA_LOG_INFO("Received connection from host");

	return DOCA_SUCCESS;
}
#else /* host */

/*
 * Initialize Sample's DOCA comch
 *
 * @se_rt_objs [in/out]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sync_event_cc_init(struct sync_event_runtime_objects *se_rt_objs)
{
	union doca_data user_data = {0};
	doca_error_t result = DOCA_SUCCESS;

	result = doca_pe_create(&se_rt_objs->comch_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to progress engine for comch: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_client_create(se_rt_objs->dev, SYNC_EVENT_CC_SERVICE_NAME, &se_rt_objs->client);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create comch server: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_client_set_max_msg_size(se_rt_objs->client, SYNC_EVENT_CC_MAX_MSG_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed set max message size of client: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_client_set_recv_queue_size(se_rt_objs->client, SYNC_EVENT_CC_MAX_QUEUE_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed set client recv queue size: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(se_rt_objs->comch_pe, doca_comch_client_as_ctx(se_rt_objs->client));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect comch client context to progress engine: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_client_task_send_set_conf(se_rt_objs->client,
						      comch_send_completion_cb,
						      comch_send_completion_err_cb,
						      SYNC_EVENT_CC_MAX_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure client task pool: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_comch_client_event_msg_recv_register(se_rt_objs->client, se_rt_objs->comch_recv_event_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register comch client receive event callback: %s",
			     doca_error_get_descr(result));
		return result;
	}

	user_data.ptr = se_rt_objs;
	doca_ctx_set_user_data(doca_comch_client_as_ctx(se_rt_objs->client), user_data);

	return DOCA_SUCCESS;
}

/*
 * Establish a connection on DOCA comch
 *
 * @se_rt_objs [in/out]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_cc_handshake(struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t result = DOCA_SUCCESS;
	int timeout = SYNC_EVENT_CC_TIMEOUT_SEC;
	enum doca_ctx_states state;

	result = sync_event_cc_init(se_rt_objs);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_ctx_start(doca_comch_client_as_ctx(se_rt_objs->client));
	if (result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start comch client: %s", doca_error_get_descr(result));
		return result;
	}

	/* Wait for client/server handshake to complete */
	(void)doca_ctx_get_state(doca_comch_client_as_ctx(se_rt_objs->client), &state);
	while (state != DOCA_CTX_STATE_RUNNING) {
		sleep(1);
		timeout--;

		if (timeout == 0) {
			DOCA_LOG_ERR("Failed to connect to server: timeout");
			return DOCA_ERROR_TIME_OUT;
		}
		(void)doca_pe_progress(se_rt_objs->comch_pe);
		(void)doca_ctx_get_state(doca_comch_client_as_ctx(se_rt_objs->client), &state);
	}

	result = doca_comch_client_get_connection(se_rt_objs->client, &se_rt_objs->comch_connection);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set connection from client: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Connection to DPU has been established");

	return DOCA_SUCCESS;
}
#endif

/*
 * Submit asynchronous DOCA task on sample's DOCA Sync Event (DOCA) Context
 *
 * @se_rt_objs [in]: sample's runtime resources
 * @se_task [in]: DOCA task to submit
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t sync_event_async_task_submit(struct sync_event_runtime_objects *se_rt_objs, struct doca_task *se_task)
{
	doca_error_t result = DOCA_SUCCESS;
	int timeout = TIMEOUT_IN_NANOS;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	result = doca_task_submit(se_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit set task for sync event: %s", doca_error_get_descr(result));
		return result;
	}

	while (doca_pe_progress(se_rt_objs->se_pe) == 0) {
		if (timeout == 0) {
			DOCA_LOG_ERR("Failed to retrieve set task progress: timeout");
			return DOCA_ERROR_TIME_OUT;
		}

		nanosleep(&ts, &ts);
		timeout -= SLEEP_IN_NANOS;
	}

	if (se_rt_objs->se_task_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to execute set task for sync event: %s",
			     doca_error_get_descr(se_rt_objs->se_task_result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Sample's tear down flow
 *
 * @se_cfg [in]: user configuration represents command line arguments
 * @se_rt_objs [in]: sample's runtime resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
void sync_event_tear_down(struct sync_event_runtime_objects *se_rt_objs)
{
	doca_error_t status = DOCA_ERROR_UNKNOWN;
	enum doca_ctx_states state;
	int timeout;

#ifdef DOCA_ARCH_DPU
	timeout = SYNC_EVENT_CC_TIMEOUT_SEC;

	/* Wait for the client to disconnect before destroying server */
	while (se_rt_objs->comch_connection != NULL) {
		sleep(1);
		timeout--;
		if (timeout == 0) {
			DOCA_LOG_ERR("Server did not get client disconnection: timeout");
			break;
		}
		(void)doca_pe_progress(se_rt_objs->comch_pe);
	}

	status = doca_ctx_stop(doca_comch_server_as_ctx(se_rt_objs->server));
	if (status != DOCA_ERROR_IN_PROGRESS && status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop comch server: %s", doca_error_get_descr(status));

	timeout = SYNC_EVENT_CC_TIMEOUT_SEC;
	(void)doca_ctx_get_state(doca_comch_server_as_ctx(se_rt_objs->server), &state);
	while (state != DOCA_CTX_STATE_IDLE) {
		sleep(1);
		timeout--;
		if (timeout == 0) {
			DOCA_LOG_ERR("Server could not move to idle state: timeout");
			break;
		}
		(void)doca_pe_progress(se_rt_objs->comch_pe);
		(void)doca_ctx_get_state(doca_comch_server_as_ctx(se_rt_objs->server), &state);
	}

	status = doca_comch_server_destroy(se_rt_objs->server);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy server: %s", doca_error_get_descr(status));
	se_rt_objs->server = NULL;
#else
	status = doca_ctx_stop(doca_comch_client_as_ctx(se_rt_objs->client));
	if (status != DOCA_ERROR_IN_PROGRESS && status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop comch client: %s", doca_error_get_descr(status));

	timeout = SYNC_EVENT_CC_TIMEOUT_SEC;
	(void)doca_ctx_get_state(doca_comch_client_as_ctx(se_rt_objs->client), &state);
	while (state != DOCA_CTX_STATE_IDLE) {
		sleep(1);
		timeout--;
		if (timeout == 0) {
			DOCA_LOG_ERR("Client could not move to idle state: timeout");
			break;
		}
		(void)doca_pe_progress(se_rt_objs->comch_pe);
		(void)doca_ctx_get_state(doca_comch_client_as_ctx(se_rt_objs->client), &state);
	}

	status = doca_comch_client_destroy(se_rt_objs->client);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy client: %s", doca_error_get_descr(status));
	se_rt_objs->client = NULL;
#endif

	status = doca_pe_destroy(se_rt_objs->comch_pe);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(status));
	se_rt_objs->comch_pe = NULL;

	if (se_rt_objs->se_ctx != NULL) {
		status = doca_ctx_stop(se_rt_objs->se_ctx);
		if (status == DOCA_ERROR_IN_PROGRESS) {
			size_t inflight_tasks;
			enum doca_ctx_states ctx_state;
			int timeout = TIMEOUT_IN_NANOS;
			struct timespec ts = {
				.tv_sec = 0,
				.tv_nsec = SLEEP_IN_NANOS,
			};
			do {
				(void)doca_pe_progress(se_rt_objs->se_pe);
				status = doca_ctx_get_num_inflight_tasks(se_rt_objs->se_ctx, &inflight_tasks);
				if (status != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to get num inflight tasks: %s",
						     doca_error_get_descr(status));
					return;
				}
				nanosleep(&ts, &ts);
				timeout -= SLEEP_IN_NANOS;
			} while (inflight_tasks != 0 && timeout > 0);

			status = doca_ctx_get_state(se_rt_objs->se_ctx, &ctx_state);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed get status of context, err: %s", doca_error_get_name(status));
				return;
			}
			status = ctx_state == DOCA_CTX_STATE_IDLE ? DOCA_SUCCESS : DOCA_ERROR_BAD_STATE;
		}

		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA context, err: %s", doca_error_get_name(status));
			return;
		}
	}

	if (se_rt_objs->se != NULL) {
		if (se_rt_objs->se_ctx == NULL) {
			status = doca_sync_event_stop(se_rt_objs->se);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to stop se: %s", doca_error_get_descr(status));
		}
		status = doca_sync_event_destroy(se_rt_objs->se);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy se: %s", doca_error_get_descr(status));
	}

	if (se_rt_objs->se_pe != NULL) {
		status = doca_pe_destroy(se_rt_objs->se_pe);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy ep: %s", doca_error_get_descr(status));
	}

	if (se_rt_objs->rep != NULL) {
		status = doca_dev_rep_close(se_rt_objs->rep);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close rep: %s", doca_error_get_descr(status));
	}

	if (se_rt_objs->dev != NULL) {
		status = doca_dev_close(se_rt_objs->dev);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close dev: %s", doca_error_get_descr(status));
	}
}
