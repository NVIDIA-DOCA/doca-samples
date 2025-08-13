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

DOCA_LOG_REGISTER(ETH_RXQ_MANAGED_MEMPOOL_RECEIVE);

#define SLEEP_IN_NANOS (10 * 1000) /* sample the task every 10 microseconds  */
#define MAX_BURST_SIZE 256	   /* Max burst size to set for eth_rxq */
#define MAX_PKT_SIZE 1600	   /* Max packet size to set for eth_rxq */
#define RATE 10000		   /* Traffic max rate in [MB/s] */
#define LOG_MAX_LRO_PKT_SIZE 15	   /* Log of max LRO packet size */
#define PKT_MAX_TIME 10		   /* Max time in [μs] to process a packet */
#define COUNTERS_NUM (1 << 19)	   /* Number of counters to configure for DOCA flow*/
#define SAMPLE_RUNS_TIME 30	   /* Sample total run-time in [s] */

struct eth_rxq_sample_objects {
	struct eth_core_resources core_resources;     /* A struct to hold ETH core resources */
	struct eth_rxq_flow_resources flow_resources; /* A struct to hold DOCA flow resources */
	struct doca_eth_rxq *eth_rxq;		      /* DOCA ETH RXQ context */
	uint64_t total_cb_counter;		      /* Counter for all call-back calls */
	uint64_t success_cb_counter;		      /* Counter for successful call-back calls */
	uint16_t rxq_queue_id;			      /* DOCA ETH RXQ's queue ID */
	bool timestamp_enable;			      /* timestamp enable */
	uint16_t headroom_size;			      /* headroom size */
	uint16_t tailroom_size;			      /* tailroom size */
};

/*
 * ETH RXQ managed receive event completed callback
 *
 * @event_managed_recv [in]: Completed event
 * @pkt [in]: received packet
 * @event_user_data [in]: User provided data, used to associate with a specific type of events
 */
static void event_managed_rcv_success_cb(struct doca_eth_rxq_event_managed_recv *event_managed_recv,
					 struct doca_buf *pkt,
					 union doca_data event_user_data)
{
	doca_error_t status, event_status;
	struct eth_rxq_sample_objects *state;
	size_t packet_size;
	const uint32_t *metadata_array;
	uint32_t rx_hash;
	uint64_t timestamp;
	uint16_t headroom_size;
	uint16_t tailroom_size;

	state = event_user_data.ptr;
	state->total_cb_counter++;
	state->success_cb_counter++;

	event_status = doca_eth_rxq_event_managed_recv_get_status(event_managed_recv);
	if (event_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Event status is %s", doca_error_get_name(event_status));

	status = doca_eth_rxq_event_managed_recv_get_metadata_array(event_managed_recv, &metadata_array);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to get metadata_array, err: %s", doca_error_get_name(status));
	else
		DOCA_LOG_INFO("Received a packet with metadata %u", metadata_array[0]);

	status = doca_eth_rxq_event_managed_recv_get_rx_hash(event_managed_recv, &rx_hash);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to get rx_hash, err: %s", doca_error_get_name(status));
	else
		DOCA_LOG_INFO("Received a packet with rx_hash %u", rx_hash);

	if (state->timestamp_enable) {
		status = doca_eth_rxq_event_managed_recv_get_timestamp(event_managed_recv, &timestamp);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get timestamp, err: %s", doca_error_get_name(status));
		else
			DOCA_LOG_INFO("Received a packet with timestamp %lu", timestamp);
	}

	status = doca_buf_get_data_len(pkt, &packet_size);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to get received packet data size");
	else
		DOCA_LOG_INFO("Received a packet with data size %lu successfully", packet_size);

	if (state->headroom_size > 0) {
		status = get_pkt_headroom(pkt, &headroom_size);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get packet headroom size, err: %s", doca_error_get_name(status));
		else
			DOCA_LOG_INFO("Received a packet with headroom size %d, requested %d",
				      headroom_size,
				      state->headroom_size);
	}
	if (state->tailroom_size > 0) {
		status = get_pkt_tailroom(pkt, &tailroom_size);
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get packet tailroom size, err: %s", doca_error_get_name(status));
		else
			DOCA_LOG_INFO("Received a packet with tailroom size %d, requested %d",
				      tailroom_size,
				      state->tailroom_size);
	}

	status = doca_buf_dec_refcount(pkt, NULL);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to free packet buffer, err: %s", doca_error_get_name(status));
}

/*
 * ETH RXQ managed receive event completed callback
 *
 * @event_managed_recv [in]: Failed event
 * @pkt [in]: received packet (NULL in this case)
 * @event_user_data [in]: User provided data, used to associate with a specific type of events
 */
static void event_managed_rcv_error_cb(struct doca_eth_rxq_event_managed_recv *event_managed_recv,
				       struct doca_buf *pkt,
				       union doca_data event_user_data)
{
	doca_error_t status;
	struct eth_rxq_sample_objects *state;

	if (pkt != NULL)
		DOCA_LOG_ERR("Received a non NULL packet");

	state = event_user_data.ptr;
	state->total_cb_counter++;
	status = doca_eth_rxq_event_managed_recv_get_status(event_managed_recv);

	DOCA_LOG_ERR("Failed to receive a packet, err: %s", doca_error_get_name(status));
}

/*
 * Destroy ETH RXQ context related resources
 *
 * @state [in]: eth_rxq_sample_objects struct to destroy its ETH RXQ context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_rxq_ctx(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	status = doca_ctx_stop(state->core_resources.core_objs.ctx);
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
 * Retrieve ETH RXQ tasks
 *
 * @state [in]: eth_rxq_sample_objects struct to retrieve events from
 */
static void retrieve_rxq_managed_recv_event(struct eth_rxq_sample_objects *state)
{
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	time_t start_time, end_time;
	double elapsed_time = 0;

	start_time = time(NULL);
	while (elapsed_time <= SAMPLE_RUNS_TIME) {
		end_time = time(NULL);
		elapsed_time = difftime(end_time, start_time);

		(void)doca_pe_progress(state->core_resources.core_objs.pe);
		nanosleep(&ts, &ts);
	}

	DOCA_LOG_INFO("Total call-backs invoked: %lu, %lu out of them were successful",
		      state->total_cb_counter,
		      state->success_cb_counter);
}

/*
 * Create ETH RXQ context related resources
 *
 * @state [in/out]: eth_rxq_sample_objects struct to create its ETH RXQ context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
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

	status = doca_eth_rxq_set_type(state->eth_rxq, DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set type, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_pkt_buf(state->eth_rxq,
					  state->core_resources.core_objs.src_mmap,
					  0,
					  state->core_resources.mmap_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set packet buffer, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	user_data.ptr = state;
	status = doca_eth_rxq_event_managed_recv_register(state->eth_rxq,
							  user_data,
							  event_managed_rcv_success_cb,
							  event_managed_rcv_error_cb);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register managed receive event, err: %s", doca_error_get_name(status));
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

	status = doca_eth_rxq_set_packet_headroom(state->eth_rxq, state->headroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set packet headroom size, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_packet_tailroom(state->eth_rxq, state->tailroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set packet tailroom size, err: %s", doca_error_get_name(status));
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
			DOCA_LOG_ERR("Failed to destroy eth_rxq_ctx, err: %s", doca_error_get_name(status));
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

	status = doca_eth_rxq_cap_is_type_supported(devinfo,
						    DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
						    DOCA_ETH_RXQ_DATA_PATH_TYPE_CPU);
	if (status != DOCA_SUCCESS && status != DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to check supported type, err: %s", doca_error_get_name(status));
		return status;
	}

	return status;
}

/*
 * Run ETH RXQ managed mempool mode receive
 *
 * @ib_dev_name [in]: IB device name of a doca device
 * @timestamp_enable [in]: timestamp enable
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t eth_rxq_managed_mempool_receive(const char *ib_dev_name,
					     bool timestamp_enable,
					     uint16_t headroom_size,
					     uint16_t tailroom_size)
{
	doca_error_t status;
	struct eth_rxq_sample_objects state = {.total_cb_counter = 0,
					       .success_cb_counter = 0,
					       .timestamp_enable = timestamp_enable,
					       .headroom_size = headroom_size,
					       .tailroom_size = tailroom_size};
	struct eth_core_config cfg = {.mmap_size = 0,
				      .inventory_num_elements = 0,
				      .check_device = check_device,
				      .ibdev_name = ib_dev_name};
	struct eth_rxq_flow_config flow_cfg = {};

	status = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
						       RATE,
						       PKT_MAX_TIME,
						       MAX_PKT_SIZE,
						       MAX_BURST_SIZE,
						       LOG_MAX_LRO_PKT_SIZE,
						       headroom_size,
						       tailroom_size,
						       &(cfg.mmap_size));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to estimate mmap size for ETH RXQ, err: %s", doca_error_get_name(status));
		return status;
	}

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

	retrieve_rxq_managed_recv_event(&state);
rxq_cleanup:
	eth_rxq_cleanup(&state);

	return status;
}
