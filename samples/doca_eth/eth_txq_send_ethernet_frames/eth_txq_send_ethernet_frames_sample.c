/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include "eth_common.h"
#include "eth_flow_common.h"

DOCA_LOG_REGISTER(ETH_TXQ_SEND_ETHERNET_FRAMES);

#define SLEEP_IN_NANOS (10 * 1000)     /* sample the task every 10 microseconds  */
#define MAX_BURST_SIZE 256	       /* Max burst size to set for eth_txq */
#define MAX_LIST_LENGTH 1	       /* Max number of elements in a doca_buf */
#define BUFS_NUM 1		       /* Number of DOCA buffers */
#define TASKS_NUM 1		       /* Tasks number */
#define REGULAR_PKT_SIZE 1500	       /* Size of the packet in doca_eth_txq_task_send task */
#define SEND_TASK_USER_DATA 0x43210    /* User data for send task */
#define UNKNOWN_ETHER_TYPE_IPV4 0x88b5 /* Unknown IPV4 type */

struct eth_txq_sample_objects {
	struct eth_core_resources core_resources;	  /* A struct to hold ETH core resources */
	struct eth_flow_common_resources flow_resources;  /* A struct to hold flow resources */
	struct doca_eth_txq *eth_txq;			  /* DOCA ETH TXQ context */
	struct doca_buf *eth_frame_buf;			  /* DOCA buffer to contain regular ethernet frame */
	struct doca_eth_txq_task_send *send_task;	  /* Regular send task */
	uint8_t src_mac_addr[DOCA_DEVINFO_MAC_ADDR_SIZE]; /* Device MAC address */
	uint32_t inflight_tasks;			  /* In flight tasks */
};

/*
 * ETH TXQ send task common callback
 *
 * @task_send [in]: Completed task
 * @task_user_data [in]: User provided data, used for identifying the task
 * @ctx_user_data [in]: User provided data, used to store sample state
 */
static void task_send_common_cb(struct doca_eth_txq_task_send *task_send,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	doca_error_t status, task_status;
	struct doca_buf *pkt;
	size_t packet_size;
	uint32_t *inflight_tasks;

	inflight_tasks = ctx_user_data.ptr;
	(*inflight_tasks)--;
	DOCA_LOG_INFO("Send task user data is 0x%lx", task_user_data.u64);

	status = doca_eth_txq_task_send_get_pkt(task_send, &pkt);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get packet of a send task, err: %s", doca_error_get_name(status));
		doca_task_free(doca_eth_txq_task_send_as_doca_task(task_send));
		return;
	}

	task_status = doca_task_get_status(doca_eth_txq_task_send_as_doca_task(task_send));

	status = doca_buf_get_data_len(pkt, &packet_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get send packet size, err: %s", doca_error_get_name(status));
	} else {
		if (task_status == DOCA_SUCCESS)
			DOCA_LOG_INFO("Sent a regular packet of size %lu successfully", packet_size);
		else
			DOCA_LOG_ERR("Failed to send a regular packet of size %lu, err: %s",
				     packet_size,
				     doca_error_get_name(task_status));
	}

	status = doca_buf_dec_refcount(pkt, NULL);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to free packet buf, err: %s", doca_error_get_name(status));

	doca_task_free(doca_eth_txq_task_send_as_doca_task(task_send));
}

/*
 * Destroy ETH TXQ context related resources
 *
 * @state [in]: eth_txq_sample_objects struct to destroy its ETH TXQ context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_txq_ctx(struct eth_txq_sample_objects *state)
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

	status = doca_eth_txq_destroy(state->eth_txq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA ETH TXQ context, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy DOCA buffers for the packets
 *
 * @state [in]: eth_txq_sample_objects struct to destroy its packet DOCA buffers
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_txq_packet_buffers(struct eth_txq_sample_objects *state)
{
	doca_error_t status;

	status = doca_buf_dec_refcount(state->eth_frame_buf, NULL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy eth_frame_buf buffer, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy ETH TXQ tasks
 *
 * @state [in]: eth_txq_sample_objects struct to destroy its tasks
 */
static void destroy_eth_txq_tasks(struct eth_txq_sample_objects *state)
{
	doca_task_free(doca_eth_txq_task_send_as_doca_task(state->send_task));
}

/*
 * Retrieve ETH TXQ tasks
 *
 * @state [in]: eth_txq_sample_objects struct to retrieve its tasks
 */
static void retrieve_eth_txq_tasks(struct eth_txq_sample_objects *state)
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
 * Submit ETH TXQ tasks
 *
 * @state [in]: eth_txq_sample_objects struct to submit its tasks
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t submit_eth_txq_tasks(struct eth_txq_sample_objects *state)
{
	doca_error_t status;

	status = doca_task_submit(doca_eth_txq_task_send_as_doca_task(state->send_task));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit send task, err: %s", doca_error_get_name(status));
		return status;
	}

	state->inflight_tasks++;

	return DOCA_SUCCESS;
}

/*
 * Create ETH TXQ tasks
 *
 * @state [in/out]: eth_txq_sample_objects struct to create tasks with its ETH TXQ context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_eth_txq_tasks(struct eth_txq_sample_objects *state)
{
	doca_error_t status;
	union doca_data user_data;

	user_data.u64 = SEND_TASK_USER_DATA;
	status = doca_eth_txq_task_send_allocate_init(state->eth_txq,
						      state->eth_frame_buf,
						      user_data,
						      &(state->send_task));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate send task, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA buffers for the packets
 *
 * @dest_mac_addr [in]: Destination MAC address to set in ethernet header
 * @state [in/out]: eth_txq_sample_objects struct to create its packet DOCA buffers
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_eth_txq_packet_buffers(uint8_t *dest_mac_addr, struct eth_txq_sample_objects *state)
{
	doca_error_t status;
	struct ether_hdr *eth_hdr;
	uint8_t *payload;

	status = doca_buf_inventory_buf_get_by_data(state->core_resources.core_objs.buf_inv,
						    state->core_resources.core_objs.src_mmap,
						    state->core_resources.mmap_addr,
						    REGULAR_PKT_SIZE,
						    &(state->eth_frame_buf));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA buffer for regular ethernet frame, err: %s",
			     doca_error_get_name(status));
		return status;
	}

	/* Create regular packet header + payload */
	eth_hdr = (struct ether_hdr *)state->core_resources.mmap_addr;
	payload = (uint8_t *)(eth_hdr + 1);
	memcpy(&(eth_hdr->src_addr), state->src_mac_addr, DOCA_DEVINFO_MAC_ADDR_SIZE);
	memcpy(&(eth_hdr->dst_addr), dest_mac_addr, DOCA_DEVINFO_MAC_ADDR_SIZE);
	/* Set unknown ether type to allow a packet with only ethernet header */
	eth_hdr->ether_type = htobe16(UNKNOWN_ETHER_TYPE_IPV4);
	memset(payload, 0x11, REGULAR_PKT_SIZE - sizeof(struct ether_hdr));

	return DOCA_SUCCESS;
}

/*
 * Create ETH TXQ context related resources
 *
 * @state [in/out]: eth_txq_sample_objects struct to create its ETH TXQ context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_eth_txq_ctx(struct eth_txq_sample_objects *state)
{
	doca_error_t status, clean_status;
	union doca_data user_data;

	status = doca_eth_txq_create(state->core_resources.core_objs.dev, MAX_BURST_SIZE, &(state->eth_txq));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ETH TXQ context, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_eth_txq_set_type(state->eth_txq, DOCA_ETH_TXQ_TYPE_REGULAR);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set type, err: %s", doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	status = doca_eth_txq_task_send_set_conf(state->eth_txq, task_send_common_cb, task_send_common_cb, TASKS_NUM);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure task_send, err: %s", doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	state->core_resources.core_objs.ctx = doca_eth_txq_as_doca_ctx(state->eth_txq);
	if (state->core_resources.core_objs.ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH TXQ context as DOCA context, err: %s",
			     doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	status = doca_pe_connect_ctx(state->core_resources.core_objs.pe, state->core_resources.core_objs.ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect PE, err: %s", doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	user_data.ptr = &(state->inflight_tasks);
	status = doca_ctx_set_user_data(state->core_resources.core_objs.ctx, user_data);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set user data for DOCA context, err: %s", doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	status = doca_ctx_start(state->core_resources.core_objs.ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context, err: %s", doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	status = doca_eth_txq_apply_queue_id(state->eth_txq, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply queue ID, err: %s", doca_error_get_name(status));
		goto destroy_eth_txq;
	}

	return DOCA_SUCCESS;
destroy_eth_txq:
	clean_status = doca_eth_txq_destroy(state->eth_txq);
	state->eth_txq = NULL;

	if (clean_status != DOCA_SUCCESS)
		return clean_status;

	return status;
}

/*
 * Clean sample resources
 *
 * @state [in]: eth_txq_sample_objects struct to clean
 */
static void eth_txq_cleanup(struct eth_txq_sample_objects *state)
{
	doca_error_t status;

	if (state->flow_resources.df_port != NULL) {
		eth_flow_common_destroy_flow_port(&(state->flow_resources));
	}

	eth_flow_common_cleanup_flow();

	if (state->eth_txq != NULL) {
		status = destroy_eth_txq_ctx(state);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy eth_txq_ctx, err: %s", doca_error_get_name(status));
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

	status = doca_eth_txq_cap_get_max_burst_size(devinfo, MAX_LIST_LENGTH, 0, &max_supported_burst_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get supported max burst size, err: %s", doca_error_get_name(status));
		return status;
	}

	if (max_supported_burst_size < MAX_BURST_SIZE)
		return DOCA_ERROR_NOT_SUPPORTED;

	status =
		doca_eth_txq_cap_is_type_supported(devinfo, DOCA_ETH_TXQ_TYPE_REGULAR, DOCA_ETH_TXQ_DATA_PATH_TYPE_CPU);
	if (status != DOCA_SUCCESS && status != DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to check supported type, err: %s", doca_error_get_name(status));
		return status;
	}

	return status;
}

/*
 * Run ETH TXQ send ethernet frames
 *
 * @ib_dev_name [in]: IB device name of a doca device
 * @dest_mac_addr [in]: destination MAC address to associate with the ethernet frames
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t eth_txq_send_ethernet_frames(const char *ib_dev_name, uint8_t *dest_mac_addr)
{
	doca_error_t status, clean_status;
	struct eth_txq_sample_objects state;
	struct eth_core_config cfg = {.mmap_size = REGULAR_PKT_SIZE * BUFS_NUM,
				      .inventory_num_elements = BUFS_NUM,
				      .check_device = check_device,
				      .ibdev_name = ib_dev_name};

	memset(&state, 0, sizeof(struct eth_txq_sample_objects));
	status = allocate_eth_core_resources(&cfg, &(state.core_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed allocate core resources, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_devinfo_get_mac_addr(doca_dev_as_devinfo(state.core_resources.core_objs.dev),
					   state.src_mac_addr,
					   DOCA_DEVINFO_MAC_ADDR_SIZE);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get device MAC address, err: %s", doca_error_get_name(status));
		goto txq_cleanup;
	}

	status = create_eth_txq_ctx(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create/start ETH TXQ context, err: %s", doca_error_get_name(status));
		goto txq_cleanup;
	}

	status = eth_flow_common_init_flow();
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow, err: %s", doca_error_get_name(status));
		goto txq_cleanup;
	}

	status = eth_flow_common_create_flow_port(state.core_resources.core_objs.dev, 0, &(state.flow_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow port, err: %s", doca_error_get_name(status));
		goto txq_cleanup;
	}

	status = create_eth_txq_packet_buffers(dest_mac_addr, &state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create packet buffers, err: %s", doca_error_get_name(status));
		goto txq_cleanup;
	}

	status = create_eth_txq_tasks(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tasks, err: %s", doca_error_get_name(status));
		goto destroy_packet_buffers;
	}

	status = submit_eth_txq_tasks(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit tasks, err: %s", doca_error_get_name(status));
		goto destroy_txq_tasks;
	}

	retrieve_eth_txq_tasks(&state);

	goto txq_cleanup;

destroy_txq_tasks:
	destroy_eth_txq_tasks(&state);
destroy_packet_buffers:
	clean_status = destroy_eth_txq_packet_buffers(&state);
	if (clean_status != DOCA_SUCCESS)
		return clean_status;
txq_cleanup:
	eth_txq_cleanup(&state);

	return status;
}
