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

#include <time.h>
#include <unistd.h>

#include <doca_flow.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include "eth_common.h"
#include "eth_rxq_common.h"

DOCA_LOG_REGISTER(ETH_RXQ_REGULAR_RECEIVE);

#define SLEEP_IN_NANOS (10 * 1000)  /* sample the task every 10 microseconds  */
#define MAX_BURST_SIZE 256	    /* Max burst size to set for eth_rxq */
#define MAX_PKT_SIZE 1600	    /* Max packet size to set for eth_rxq */
#define BUFS_NUM 1		    /* Number of DOCA buffers */
#define TASKS_NUM 1		    /* Tasks number */
#define RECV_TASK_USER_DATA 0x43210 /* User data for receive task */

struct eth_rxq_sample_objects {
	struct eth_core_resources core_resources;     /* A struct to hold ETH core resources */
	struct eth_rxq_flow_resources flow_resources; /* A struct to hold DOCA flow resources */
	struct doca_eth_rxq *eth_rxq;		      /* DOCA ETH RXQ context */
	struct doca_buf *packet_buf;		      /* DOCA buffer to contain received packet */
	struct doca_eth_rxq_task_recv *recv_task;     /* Receive task */
	uint32_t inflight_tasks;		      /* Inflight tasks count */
	uint16_t rxq_queue_id;			      /* DOCA ETH RXQ's queue ID */
	bool timestamp_enable;			      /* timestamp enable */
};

/*
 * ETH RXQ receive task common callback
 *
 * @task_recv [in]: Completed task
 * @task_user_data [in]: User provided data, used for identifying the task
 * @ctx_user_data [in]: User provided data, used to store sample state
 */
static void task_recv_common_cb(struct doca_eth_rxq_task_recv *task_recv,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	doca_error_t status, task_status;
	struct eth_rxq_sample_objects *state;
	struct doca_buf *pkt;
	size_t packet_size;
	uint32_t rx_hash;
	const uint32_t *metadata_array;
	uint64_t timestamp;

	state = ctx_user_data.ptr;
	state->inflight_tasks--;
	DOCA_LOG_INFO("Receive task user data is 0x%lx", task_user_data.u64);

	status = doca_eth_rxq_task_recv_get_pkt(task_recv, &pkt);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get packet of a receive task, err: %s", doca_error_get_name(status));
		doca_task_free(doca_eth_rxq_task_recv_as_doca_task(task_recv));
		return;
	}

	task_status = doca_task_get_status(doca_eth_rxq_task_recv_as_doca_task(task_recv));

	if (task_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive a packet, err: %s", doca_error_get_name(task_status));
	} else {
		status = doca_eth_rxq_task_recv_get_metadata_array(task_recv, &metadata_array);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get metadata_array, err: %s", doca_error_get_name(status));
		else
			DOCA_LOG_INFO("Received a packet with metadata %u", metadata_array[0]);

		status = doca_eth_rxq_task_recv_get_rx_hash(task_recv, &rx_hash);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get rx_hash, err: %s", doca_error_get_name(status));
		else
			DOCA_LOG_INFO("Received a packet with rx_hash %u", rx_hash);

		if (state->timestamp_enable) {
			status = doca_eth_rxq_task_recv_get_timestamp(task_recv, &timestamp);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to get timestamp, err: %s", doca_error_get_name(status));
			else
				DOCA_LOG_INFO("Received a packet with timestamp %lu", timestamp);
		}

		status = doca_buf_get_data_len(pkt, &packet_size);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get receive packet size, err: %s", doca_error_get_name(status));
		else
			DOCA_LOG_INFO("Received a packet of size %lu successfully", packet_size);
	}

	status = doca_buf_dec_refcount(pkt, NULL);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to free packet buf, err: %s", doca_error_get_name(status));

	doca_task_free(doca_eth_rxq_task_recv_as_doca_task(task_recv));
}

/*
 * Destroy ETH RXQ context related resources
 *
 * @state [in]: eth_rxq_sample_objects struct to destroy its ETH RXQ context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_rxq_ctx(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;
	enum doca_ctx_states ctx_state;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	status = doca_ctx_stop(state->core_resources.core_objs.ctx);
	if (status == DOCA_ERROR_IN_PROGRESS) {
		while (state->inflight_tasks != 0) {
			(void)doca_pe_progress(state->core_resources.core_objs.pe);
			nanosleep(&ts, &ts);
		}

		status = doca_ctx_get_state(state->core_resources.core_objs.ctx, &ctx_state);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed get status of context, err: %s", doca_error_get_name(status));
			return status;
		}

		status = ctx_state == DOCA_CTX_STATE_IDLE ? DOCA_SUCCESS : DOCA_ERROR_BAD_STATE;
	}

	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA context, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_eth_rxq_destroy(state->eth_rxq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA ETH RXQ context, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy DOCA buffers for the packets
 *
 * @state [in]: eth_rxq_sample_objects struct to destroy its packet DOCA buffers
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_rxq_packet_buffers(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	status = doca_buf_dec_refcount(state->packet_buf, NULL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy packet_buf buffer, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy ETH RXQ tasks
 *
 * @state [in]: eth_rxq_sample_objects struct to destroy its tasks
 */
static void destroy_eth_rxq_tasks(struct eth_rxq_sample_objects *state)
{
	doca_task_free(doca_eth_rxq_task_recv_as_doca_task(state->recv_task));
}

/*
 * Retrieve ETH RXQ tasks
 *
 * @state [in]: eth_rxq_sample_objects struct to retrieve tasks from
 */
static void retrieve_rxq_recv_tasks(struct eth_rxq_sample_objects *state)
{
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	while (state->inflight_tasks != 0) {
		(void)doca_pe_progress(state->core_resources.core_objs.pe);
		nanosleep(&ts, &ts);
	}
}

/*
 * Create ETH RXQ context related resources
 *
 * @state [in/out]: eth_rxq_sample_objects struct to create its ETH RXQ context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_eth_rxq_ctx(struct eth_rxq_sample_objects *state)
{
	doca_error_t status, clean_status;
	union doca_data user_data;

	status = doca_eth_rxq_create(state->core_resources.core_objs.dev,
				     MAX_BURST_SIZE,
				     MAX_PKT_SIZE,
				     &(state->eth_rxq));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ETH RXQ context, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_eth_rxq_set_type(state->eth_rxq, DOCA_ETH_RXQ_TYPE_REGULAR);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set type, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_task_recv_set_conf(state->eth_rxq, task_recv_common_cb, task_recv_common_cb, TASKS_NUM);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set receive task configuration, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_metadata_num(state->eth_rxq, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable metadata, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_rx_hash(state->eth_rxq, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable rx_hash, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_timestamp(state->eth_rxq, state->timestamp_enable);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set enable timestamp, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	state->core_resources.core_objs.ctx = doca_eth_rxq_as_doca_ctx(state->eth_rxq);
	if (state->core_resources.core_objs.ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH RXQ context as DOCA context, err: %s",
			     doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_pe_connect_ctx(state->core_resources.core_objs.pe, state->core_resources.core_objs.ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect PE, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	user_data.ptr = state;
	status = doca_ctx_set_user_data(state->core_resources.core_objs.ctx, user_data);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set user data for DOCA context, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_ctx_start(state->core_resources.core_objs.ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_apply_queue_id(state->eth_rxq, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply queue ID of RXQ, err: %s", doca_error_get_name(status));
		goto stop_ctx;
	}
	state->rxq_queue_id = 0;

	return DOCA_SUCCESS;
stop_ctx:
	clean_status = doca_ctx_stop(state->core_resources.core_objs.ctx);
	state->core_resources.core_objs.ctx = NULL;

	if (clean_status != DOCA_SUCCESS)
		return status;
destroy_eth_rxq:
	clean_status = doca_eth_rxq_destroy(state->eth_rxq);
	state->eth_rxq = NULL;

	if (clean_status != DOCA_SUCCESS)
		return status;

	return status;
}

/*
 * Submit ETH RXQ tasks
 *
 * @state [in/out]: eth_rxq_sample_objects struct to submit its tasks
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t submit_eth_rxq_tasks(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	status = doca_task_submit(doca_eth_rxq_task_recv_as_doca_task(state->recv_task));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit receive task, err: %s", doca_error_get_name(status));
		return status;
	}

	state->inflight_tasks++;

	return DOCA_SUCCESS;
}

/*
 * Create ETH RXQ tasks
 *
 * @state [in/out]: eth_rxq_sample_objects struct to create tasks with its ETH RXQ context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_eth_rxq_tasks(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;
	union doca_data user_data;

	user_data.u64 = RECV_TASK_USER_DATA;
	status =
		doca_eth_rxq_task_recv_allocate_init(state->eth_rxq, state->packet_buf, user_data, &(state->recv_task));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate receive task, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA buffers for the packet
 *
 * @state [in/out]: eth_rxq_sample_objects struct to create its packet DOCA buffers
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_eth_rxq_packet_buffer(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	status = doca_buf_inventory_buf_get_by_addr(state->core_resources.core_objs.buf_inv,
						    state->core_resources.core_objs.src_mmap,
						    state->core_resources.mmap_addr,
						    MAX_PKT_SIZE,
						    &(state->packet_buf));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA buffer for ethernet frame, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Clean sample resources
 *
 * @state [in]: eth_rxq_sample_objects struct to clean
 */
static void eth_rxq_cleanup(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	if (state->flow_resources.root_pipe != NULL)
		doca_flow_pipe_destroy(state->flow_resources.root_pipe);

	if (state->flow_resources.df_port != NULL) {
		status = doca_flow_port_stop(state->flow_resources.df_port);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(status));
			return;
		}
	}

	if (state->eth_rxq != NULL) {
		status = destroy_eth_rxq_ctx(state);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy eth_rxq_ctx, err: %s", doca_error_get_name(status));
			return;
		}
	}

	if (state->core_resources.core_objs.dev != NULL) {
		status = destroy_eth_core_resources(&(state->core_resources));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy core_resources, err: %s", doca_error_get_name(status));
			return;
		}
	}

	if (state->flow_resources.df_port != NULL)
		doca_flow_destroy();
}

/*
 * Check if device supports needed capabilities
 *
 * @devinfo [in]: Device info for device to check
 * @return: DOCA_SUCCESS in case the device supports needed capabilities and DOCA_ERROR otherwise
 */
static doca_error_t check_device(struct doca_devinfo *devinfo)
{
	doca_error_t status;
	uint32_t max_supported_burst_size;
	uint32_t max_supported_packet_size;

	status = doca_eth_rxq_cap_get_max_burst_size(devinfo, &max_supported_burst_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get supported max burst size, err: %s", doca_error_get_name(status));
		return status;
	}

	if (max_supported_burst_size < MAX_BURST_SIZE)
		return DOCA_ERROR_NOT_SUPPORTED;

	status = doca_eth_rxq_cap_get_max_packet_size(devinfo, &max_supported_packet_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get supported max packet size, err: %s", doca_error_get_name(status));
		return status;
	}

	if (max_supported_packet_size < MAX_PKT_SIZE)
		return DOCA_ERROR_NOT_SUPPORTED;

	status =
		doca_eth_rxq_cap_is_type_supported(devinfo, DOCA_ETH_RXQ_TYPE_REGULAR, DOCA_ETH_RXQ_DATA_PATH_TYPE_CPU);
	if (status != DOCA_SUCCESS && status != DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to check supported type, err: %s", doca_error_get_name(status));
		return status;
	}

	return status;
}

/*
 * Run ETH RXQ regular mode receive
 *
 * @ib_dev_name [in]: IB device name of a doca device
 * @timestamp_enable [in]: timestamp enable
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t eth_rxq_regular_receive(const char *ib_dev_name, bool timestamp_enable)
{
	doca_error_t status, clean_status;
	struct eth_rxq_sample_objects state = {.timestamp_enable = timestamp_enable};
	struct eth_core_config cfg = {.mmap_size = MAX_PKT_SIZE * BUFS_NUM,
				      .inventory_num_elements = BUFS_NUM,
				      .check_device = check_device,
				      .ibdev_name = ib_dev_name};
	struct eth_rxq_flow_config flow_cfg = {};

	status = allocate_eth_core_resources(&cfg, &(state.core_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed allocate core resources, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	flow_cfg.dev = state.core_resources.core_objs.dev;

	status = rxq_common_init_doca_flow(flow_cfg.dev, 0, &(state.flow_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca flow, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	status = create_eth_rxq_ctx(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create/start ETH RXQ context, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	flow_cfg.rxq_queue_ids = &(state.rxq_queue_id);
	flow_cfg.nb_queues = 1;

	status = allocate_eth_rxq_flow_resources(&flow_cfg, &(state.flow_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate flow resources, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	status = create_eth_rxq_packet_buffer(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create packer buffer, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	status = create_eth_rxq_tasks(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tasks, err: %s", doca_error_get_name(status));
		goto destroy_packet_buffers;
	}

	status = submit_eth_rxq_tasks(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit tasks, err: %s", doca_error_get_name(status));
		goto destroy_rxq_tasks;
	}

	retrieve_rxq_recv_tasks(&state);

	goto rxq_cleanup;

destroy_rxq_tasks:
	destroy_eth_rxq_tasks(&state);
destroy_packet_buffers:
	clean_status = destroy_eth_rxq_packet_buffers(&state);
	if (clean_status != DOCA_SUCCESS)
		return status;
rxq_cleanup:
	eth_rxq_cleanup(&state);

	return status;
}
