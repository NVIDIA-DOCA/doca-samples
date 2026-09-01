/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <samples/common.h>

#include <time.h>

#include "comch_utils.h"

DOCA_LOG_REGISTER(COMCH_UTILS);

#define COMCH_NUM_TASKS 1024 /* Tasks for sending comch messages */
#define SLEEP_IN_NANOS (10 * 1000)

struct comch_cfg {
	void *app_user_data;  /* User-data supplied by the app */
	struct doca_pe *pe;   /* Progress engine for comch */
	struct doca_ctx *ctx; /* Doca context of the client/server */
	union {
		struct doca_comch_server *server; /* Server context (DPU only) */
		struct doca_comch_client *client; /* Client context (x86 host only) */
	};
	struct doca_comch_connection *active_connection; /* Single connection active on the channel */
	struct doca_dev *dev;				 /* Device in use */
	struct doca_dev_rep *dev_rep;			 /* Representor in use (DPU only) */
	uint32_t max_buf_size;				 /* Maximum size of message on channel */
	uint8_t is_server;				 /* Indicator of client or server */
};

/*
 * Callback for send completions
 *
 * Frees the allocated task that was used for the send
 *
 * @task [in]: send task that has completed
 * @task_user_data [in]: user data of task
 * @ctx_user_data [in]: user data of doca context
 */
static void comch_send_completion(struct doca_comch_task_send *task,
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
static void comch_send_completion_err(struct doca_comch_task_send *task,
				      union doca_data task_user_data,
				      union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	doca_task_free(doca_comch_task_send_as_task(task));
	DOCA_LOG_ERR("Send Task got a completion error");
}

/*
 * Callback for new server connection
 *
 * @event [in]: connection event
 * @comch_connection [in]: doca connection that triggered the event
 * @change_successful [in]: indicator of change success
 */
static void server_connection_cb(struct doca_comch_event_connection_status_changed *event,
				 struct doca_comch_connection *comch_connection,
				 uint8_t change_successful)
{
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(comch_connection);
	union doca_data ctx_user_data = {0};
	struct comch_cfg *comch_cfg;
	doca_error_t result;

	(void)event;

	if (change_successful == 0) {
		DOCA_LOG_ERR("Connection event unsuccessful");
		return;
	}

	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from server context: %s", doca_error_get_descr(result));
		return;
	}

	comch_cfg = (struct comch_cfg *)ctx_user_data.ptr;
	if (comch_cfg == NULL) {
		DOCA_LOG_ERR("Failed to get configuration from server context");
		return;
	}

	if (comch_cfg->active_connection != NULL) {
		DOCA_LOG_ERR("A connection already exists on the server - rejecting new attempt");
		result = doca_comch_server_disconnect(server, comch_connection);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to properly reject connection");
		return;
	}

	result = doca_comch_connection_set_user_data(comch_connection, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set user data on connection: %s", doca_error_get_descr(result));
		return;
	}

	comch_cfg->active_connection = comch_connection;

	DOCA_LOG_TRC("Server received a new connection");
}

/*
 * Callback for server disconnection
 *
 * @event [in]: connection event
 * @comch_connection [in]: doca connection that triggered the event
 * @change_successful [in]: indicator of change success
 */
static void server_disconnection_cb(struct doca_comch_event_connection_status_changed *event,
				    struct doca_comch_connection *comch_connection,
				    uint8_t change_successful)
{
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(comch_connection);
	union doca_data ctx_user_data = {0};
	struct comch_cfg *comch_cfg;
	doca_error_t result;

	(void)event;
	(void)change_successful;

	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from server context: %s", doca_error_get_descr(result));
		return;
	}

	comch_cfg = (struct comch_cfg *)ctx_user_data.ptr;
	if (comch_cfg == NULL) {
		DOCA_LOG_ERR("Failed to get configuration from server context");
		return;
	}

	comch_cfg->active_connection = NULL;

	DOCA_LOG_TRC("Server received a client disconnection");
}

/*
 * Extract the comch_cfg data from a connection
 *
 * @connection [in]: connection to extract comch_cfg from
 * @return: pointer to the comch_cfg object
 */
static inline struct comch_cfg *get_comch_cfg_from_connection(struct doca_comch_connection *connection)
{
	union doca_data connection_user_data;
	struct comch_cfg *comch_cfg;

	if (connection == NULL) {
		DOCA_LOG_ERR("Connection is NULL");
		return NULL;
	}

	connection_user_data = doca_comch_connection_get_user_data(connection);
	comch_cfg = connection_user_data.ptr;

	if (comch_cfg == NULL)
		DOCA_LOG_ERR("Failed to get user data from connection");

	return comch_cfg;
}

doca_error_t comch_utils_send(struct doca_comch_connection *connection, const void *msg, uint32_t len)
{
	struct comch_cfg *comch_cfg = get_comch_cfg_from_connection(connection);
	struct doca_comch_task_send *task;
	doca_error_t result;

	if (comch_cfg == NULL)
		return DOCA_ERROR_NOT_FOUND;

	if (len > comch_cfg->max_buf_size) {
		DOCA_LOG_ERR("Message length of %u larger than max comch length of %u", len, comch_cfg->max_buf_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (comch_cfg->is_server)
		result = doca_comch_server_task_send_alloc_init(comch_cfg->server, connection, msg, len, &task);
	else
		result = doca_comch_client_task_send_alloc_init(comch_cfg->client, connection, msg, len, &task);

	/*
	 * QP depths match the number of tasks so if a task can be allocated then there must be a space to send.
	 * Return AGAIN if a task can not be allocated telling the application to progress and retry.
	 * Assuming a task can be allocated it will only fail to send if there is a more serious error.
	 */
	if (result == DOCA_ERROR_NO_MEMORY)
		return DOCA_ERROR_AGAIN;

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate send task: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_task_submit(doca_comch_task_send_as_task(task));
	if (result != DOCA_SUCCESS) {
		doca_task_free(doca_comch_task_send_as_task(task));
		DOCA_LOG_ERR("Failed to submit send task: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

void *comch_utils_get_user_data(struct doca_comch_connection *connection)
{
	struct comch_cfg *comch_cfg = get_comch_cfg_from_connection(connection);

	if (comch_cfg == NULL)
		return NULL;

	return comch_cfg->app_user_data;
}

doca_error_t comch_utils_progress_connection(struct doca_comch_connection *connection)
{
	struct comch_cfg *comch_cfg = get_comch_cfg_from_connection(connection);

	if (comch_cfg == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	(void)doca_pe_progress(comch_cfg->pe);

	return DOCA_SUCCESS;
}

struct doca_comch_connection *comch_util_get_connection(struct comch_cfg *comch_cfg)
{
	if (comch_cfg == NULL) {
		DOCA_LOG_ERR("Configuration object is NULL");
		return NULL;
	}

	return comch_cfg->active_connection;
}

uint32_t comch_utils_get_max_buffer_size(struct comch_cfg *comch_cfg)
{
	if (comch_cfg == NULL) {
		DOCA_LOG_ERR("Configuration object is NULL");
		return 0;
	}

	return comch_cfg->max_buf_size;
}

doca_error_t comch_utils_fast_path_init(const char *server_name,
					const char *pci_addr,
					const char *rep_pci_addr,
					void *user_data,
					doca_comch_event_msg_recv_cb_t client_recv_event_cb,
					doca_comch_event_msg_recv_cb_t server_recv_event_cb,
					doca_comch_event_consumer_cb_t new_consumer_event_cb,
					doca_comch_event_consumer_cb_t expired_consumer_event_cb,
					struct comch_cfg **comch_cfg)
{
	enum doca_ctx_states state;
	union doca_data comch_user_data = {0};
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};
	struct comch_cfg *cfg;
	doca_error_t result;

	if (server_name == NULL) {
		DOCA_LOG_ERR("Init: server name is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (pci_addr == NULL) {
		DOCA_LOG_ERR("Init: PCIe address is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (comch_cfg == NULL) {
		DOCA_LOG_ERR("Init: configuration object is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

#ifdef DOCA_ARCH_DPU
	if (rep_pci_addr == NULL) {
		DOCA_LOG_ERR("Init: repr PCIe is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (server_recv_event_cb == NULL) {
		DOCA_LOG_ERR("Init: server recv event callback is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}
#else
	if (client_recv_event_cb == NULL) {
		DOCA_LOG_ERR("Init: client recv event callback is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}
#endif

	cfg = (struct comch_cfg *)calloc(1, sizeof(struct comch_cfg));
	if (cfg == NULL) {
		DOCA_LOG_ERR("Failed to comch configuration data");
		return DOCA_ERROR_NO_MEMORY;
	}

#ifdef DOCA_ARCH_DPU
	cfg->is_server = 1;
#endif

	cfg->app_user_data = user_data;
	comch_user_data.ptr = cfg;

	result = doca_pe_create(&cfg->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create comch progress engine: %s", doca_error_get_descr(result));
		goto destroy_comch_cfg;
	}

	result = open_doca_device_with_pci(pci_addr, NULL, &cfg->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open Comm Channel DOCA device based on PCI address: %s",
			     doca_error_get_descr(result));
		goto destroy_pe;
	}

	result = doca_comch_cap_get_max_msg_size(doca_dev_as_devinfo(cfg->dev), &cfg->max_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get comch max buffer size: %s", doca_error_get_descr(result));
		goto close_device;
	}

	if (cfg->is_server) {
		result = doca_comch_cap_server_is_supported(doca_dev_as_devinfo(cfg->dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Doca device does not support comch server: %s", doca_error_get_descr(result));
			goto close_device;
		}

		result = open_doca_device_rep_with_pci(cfg->dev,
						       DOCA_DEVINFO_REP_FILTER_NET,
						       rep_pci_addr,
						       &cfg->dev_rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open Comm Channel DOCA device representor based on PCI address: %s",
				     doca_error_get_descr(result));
			goto close_device;
		}

		result = doca_comch_server_create(cfg->dev, cfg->dev_rep, server_name, &cfg->server);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create comch server: %s", doca_error_get_descr(result));
			goto close_rep_device;
		}

		result = doca_comch_server_set_max_msg_size(cfg->server, cfg->max_buf_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed set max message size of server: %s", doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		cfg->ctx = doca_comch_server_as_ctx(cfg->server);

		result = doca_pe_connect_ctx(cfg->pe, cfg->ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to connect comch server context to progress engine: %s",
				     doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		result = doca_comch_server_task_send_set_conf(cfg->server,
							      comch_send_completion,
							      comch_send_completion_err,
							      COMCH_NUM_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure server task pool: %s", doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		result = doca_comch_server_event_msg_recv_register(cfg->server, server_recv_event_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register comch server receive event callback: %s",
				     doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		result = doca_comch_server_event_connection_status_changed_register(cfg->server,
										    server_connection_cb,
										    server_disconnection_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register comch server event callback: %s",
				     doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		/* Only set consumer events if app is interesting in fast path */
		if (new_consumer_event_cb != NULL || expired_consumer_event_cb != NULL) {
			/* Both callbacks must be non NULL for function to succeed */
			result = doca_comch_server_event_consumer_register(cfg->server,
									   new_consumer_event_cb,
									   expired_consumer_event_cb);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to register comch server consumer callback: %s",
					     doca_error_get_descr(result));
				goto destroy_comch_ep;
			}
		}

		doca_ctx_set_user_data(cfg->ctx, comch_user_data);

		cfg->active_connection = NULL;

		result = doca_ctx_start(cfg->ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start comch server context: %s", doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		DOCA_LOG_INFO("Server waiting on a client to connect");

		/* Connection will be populated when a single client connects */
		while (cfg->active_connection == NULL) {
			(void)doca_pe_progress(cfg->pe);
			nanosleep(&ts, &ts);
		}

		DOCA_LOG_INFO("Server connection established");

	} else {
		result = doca_comch_cap_client_is_supported(doca_dev_as_devinfo(cfg->dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Doca device does not support comch client: %s", doca_error_get_descr(result));
			goto close_device;
		}

		result = doca_comch_client_create(cfg->dev, server_name, &cfg->client);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create comch client: %s", doca_error_get_descr(result));
			goto close_device;
		}

		result = doca_comch_client_set_max_msg_size(cfg->client, cfg->max_buf_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed set max message size of client: %s", doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		cfg->ctx = doca_comch_client_as_ctx(cfg->client);

		result = doca_pe_connect_ctx(cfg->pe, cfg->ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to connect comch client context to progress engine: %s",
				     doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		result = doca_comch_client_task_send_set_conf(cfg->client,
							      comch_send_completion,
							      comch_send_completion_err,
							      COMCH_NUM_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure client task pool: %s", doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		result = doca_comch_client_event_msg_recv_register(cfg->client, client_recv_event_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register comch client receive event callback: %s",
				     doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		/* Only set consumer events if app is interesting in fast path */
		if (new_consumer_event_cb != NULL || expired_consumer_event_cb != NULL) {
			/* Both callbacks must be non NULL for function to succeed */
			result = doca_comch_client_event_consumer_register(cfg->client,
									   new_consumer_event_cb,
									   expired_consumer_event_cb);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to register comch client consumer callback: %s",
					     doca_error_get_descr(result));
				goto destroy_comch_ep;
			}
		}

		doca_ctx_set_user_data(cfg->ctx, comch_user_data);

		result = doca_ctx_start(cfg->ctx);
		if (result != DOCA_ERROR_IN_PROGRESS) {
			DOCA_LOG_ERR("Failed to start comch client context: %s", doca_error_get_descr(result));
			goto destroy_comch_ep;
		}

		/* Wait for client/server handshake to complete */
		(void)doca_ctx_get_state(cfg->ctx, &state);
		while (state != DOCA_CTX_STATE_RUNNING) {
			(void)doca_pe_progress(cfg->pe);
			nanosleep(&ts, &ts);
			(void)doca_ctx_get_state(cfg->ctx, &state);
		}

		(void)doca_comch_client_get_connection(cfg->client, &cfg->active_connection);
		doca_comch_connection_set_user_data(cfg->active_connection, comch_user_data);
	}

	*comch_cfg = cfg;

	return DOCA_SUCCESS;

destroy_comch_ep:
	if (cfg->is_server)
		doca_comch_server_destroy(cfg->server);
	else
		doca_comch_client_destroy(cfg->client);
close_rep_device:
	if (cfg->is_server)
		doca_dev_rep_close(cfg->dev_rep);
close_device:
	doca_dev_close(cfg->dev);
destroy_pe:
	doca_pe_destroy(cfg->pe);
destroy_comch_cfg:
	free(cfg);

	return result;
}

doca_error_t comch_utils_init(const char *server_name,
			      const char *pci_addr,
			      const char *rep_pci_addr,
			      void *user_data,
			      doca_comch_event_msg_recv_cb_t client_recv_event_cb,
			      doca_comch_event_msg_recv_cb_t server_recv_event_cb,
			      struct comch_cfg **comch_cfg)
{
	return comch_utils_fast_path_init(server_name,
					  pci_addr,
					  rep_pci_addr,
					  user_data,
					  client_recv_event_cb,
					  server_recv_event_cb,
					  NULL,
					  NULL,
					  comch_cfg);
}

doca_error_t comch_utils_destroy(struct comch_cfg *comch_cfg)
{
	enum doca_ctx_states state;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;

	if (comch_cfg == NULL) {
		DOCA_LOG_ERR("Destroy: configuration object is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (comch_cfg->is_server) {
		/* Wait until the client has closed the connection to end gracefully */
		while (comch_cfg->active_connection != NULL) {
			(void)doca_pe_progress(comch_cfg->pe);
			nanosleep(&ts, &ts);
		}
	}

	result = doca_ctx_stop(comch_cfg->ctx);
	if (result != DOCA_ERROR_IN_PROGRESS && result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop comch ctx: %s", doca_error_get_descr(result));
		return result;
	}

	(void)doca_ctx_get_state(comch_cfg->ctx, &state);
	while (state != DOCA_CTX_STATE_IDLE) {
		(void)doca_pe_progress(comch_cfg->pe);
		nanosleep(&ts, &ts);
		(void)doca_ctx_get_state(comch_cfg->ctx, &state);
	}

	if (comch_cfg->is_server) {
		result = doca_comch_server_destroy(comch_cfg->server);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy server: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_dev_rep_close(comch_cfg->dev_rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device representor: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_comch_client_destroy(comch_cfg->client);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy client: %s", doca_error_get_descr(result));
			return result;
		}
	}

	result = doca_dev_close(comch_cfg->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_destroy(comch_cfg->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	free(comch_cfg);

	return DOCA_SUCCESS;
}
