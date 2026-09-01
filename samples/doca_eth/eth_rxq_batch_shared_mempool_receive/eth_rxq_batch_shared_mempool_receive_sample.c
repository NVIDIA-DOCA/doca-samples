/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <stdint.h>
#include <unistd.h>

#include <doca_flow.h>
#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_dev.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_eth_rxq_shared_mempool.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include "eth_common.h"
#include "eth_rxq_common.h"
#include "eth_flow_common.h"

DOCA_LOG_REGISTER(ETH_RXQ_BATCH_SHARED_MEMPOOL_RECEIVE);

#define SLEEP_IN_NANOS (10 * 1000)   /* sample for event batch every 10 microseconds  */
#define MAX_BURST_SIZE 2048	     /* Max burst size to set for eth_rxq (also the shared mempool queue depth) */
#define MAX_PKT_SIZE 1600	     /* Max packet size to set for eth_rxq */
#define SAMPLE_RUNS_TIME 30	     /* Sample total run-time in [s] */
#define SHARED_MEMPOOL_DEVICES_NUM 1 /* Number of devices sharing the memory pool */

struct eth_rxq_sample_objects;

/* Per-queue resources for a single ETH RXQ context sharing the memory pool */
struct eth_rxq_queue_resources {
	struct eth_rxq_sample_objects *state; /* Back-pointer to the sample objects, used in the receive callback */
	struct doca_eth_rxq *eth_rxq;	      /* DOCA ETH RXQ context */
	struct doca_ctx *ctx;		      /* DOCA ETH RXQ context as DOCA context */
	uint16_t queue_id;		      /* Logical queue ID applied to the RXQ (used for DOCA Flow RSS) */
};

struct eth_rxq_sample_objects {
	struct eth_core_resources core_resources;		    /* A struct to hold ETH core resources */
	struct eth_flow_common_resources flow_resources;	    /* A struct to hold flow resources */
	struct doca_eth_rxq_shared_mempool *shared_mempool;	    /* DOCA ETH RXQ shared memory pool */
	struct doca_dev *devices_array[SHARED_MEMPOOL_DEVICES_NUM]; /* Devices sharing the memory pool */
	struct eth_rxq_queue_resources queues[MAX_QUEUES_NUM]; /* Per-queue resources of the RXQs sharing the pool */
	uint16_t rxq_queue_ids[MAX_QUEUES_NUM];		       /* RXQ queue IDs array (RSS targets for the flow pipe) */
	uint16_t num_queues;				       /* Number of ETH RXQs sharing the memory pool */
	uint64_t total_received_packets;		       /* Counter for received packets across all queues */
	bool timestamp_enable;				       /* timestamp enable */
	uint16_t headroom_size;				       /* headroom size */
	uint16_t tailroom_size;				       /* tailroom size */
};

/*
 * ETH RXQ shared mempool receive event batch successful callback
 *
 * @event_batch_shared_mempool_recv [in]: The shared mempool receive event batch
 * @packets_number [in]: Number of retrieved packets
 * @status [in]: Status of retrieved event batch (DOCA_SUCCESS in case of success callback)
 * @pkt_array [in]: Array of doca_bufs containing the received packets (NULL in case of error callback)
 * @event_batch_user_data [in]: User provided data, holds the per-queue resources of the receiving RXQ
 */
static void event_batch_shared_mempool_rcv_success_cb(
	struct doca_eth_rxq_event_batch_shared_mempool_recv *event_batch_shared_mempool_recv,
	uint16_t packets_number,
	doca_error_t status,
	struct doca_buf **pkt_array,
	union doca_data event_batch_user_data)
{
	doca_error_t ret;
	struct eth_rxq_sample_objects *state;
	const uint32_t *metadata_array = NULL, *rx_hash_array = NULL;
	const uint64_t *timestamp_array = NULL;
	size_t packet_size;
	uint16_t headroom_size;
	uint16_t tailroom_size;

	struct eth_rxq_queue_resources *queue_res = event_batch_user_data.ptr;
	state = queue_res->state;
	state->total_received_packets += packets_number;

	DOCA_LOG_INFO("Queue #%u: received %u packets successfully", queue_res->queue_id, packets_number);

	ret = doca_eth_rxq_event_batch_shared_mempool_recv_get_metadata_array(event_batch_shared_mempool_recv,
									      &metadata_array);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to get metadata_array, err: %s", doca_error_get_name(ret));

	ret = doca_eth_rxq_event_batch_shared_mempool_recv_get_rx_hash_array(event_batch_shared_mempool_recv,
									     &rx_hash_array);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to get rx_hash_array, err: %s", doca_error_get_name(ret));

	if (state->timestamp_enable) {
		ret = doca_eth_rxq_event_batch_shared_mempool_recv_get_timestamp_array(event_batch_shared_mempool_recv,
										       &timestamp_array);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get timestamp_array, err: %s", doca_error_get_name(ret));
	}

	for (uint16_t i = 0; i < packets_number; i++) {
		ret = doca_buf_get_data_len(pkt_array[i], &packet_size);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_ERR("Packet#%u: failed to get received packet data size, err: %s",
				     i,
				     doca_error_get_name(ret));
		else
			DOCA_LOG_INFO("Packet#%u: received a packet with data size %lu successfully", i, packet_size);

		if (state->headroom_size > 0) {
			status = get_pkt_headroom(pkt_array[i], &headroom_size);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Packet#%u: failed to get packet headroom size, err: %s",
					     i,
					     doca_error_get_name(status));
			else
				DOCA_LOG_INFO("Packet#%u: received a packet with headroom size %d, requested %d",
					      i,
					      headroom_size,
					      state->headroom_size);
		}
		if (state->tailroom_size > 0) {
			status = get_pkt_tailroom(pkt_array[i], &tailroom_size);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Packet#%u: failed to get packet tailroom size, err: %s",
					     i,
					     doca_error_get_name(status));
			else
				DOCA_LOG_INFO("Packet#%u: received a packet with tailroom size %d, requested %d",
					      i,
					      tailroom_size,
					      state->tailroom_size);
		}

		if (metadata_array != NULL)
			DOCA_LOG_INFO(
				"Packet#%u: metadata associated with the packet is %u",
				i,
				doca_eth_rxq_event_batch_recv_metadata_array_get_metadata(metadata_array, 1, i, 0));

		if (rx_hash_array != NULL)
			DOCA_LOG_INFO("Packet#%u: rx_hash associated with the packet is %u", i, rx_hash_array[i]);

		if (timestamp_array != NULL)
			DOCA_LOG_INFO("Packet#%u: timestamp associated with the packet is %lu", i, timestamp_array[i]);
	}

	status = doca_eth_rxq_shared_mempool_free_buf_bulk(state->shared_mempool, pkt_array, packets_number);
	if (status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to free received packets, err: %s", doca_error_get_name(status));
}

/*
 * ETH RXQ shared mempool receive event batch error callback
 *
 * @event_batch_shared_mempool_recv [in]: The shared mempool receive event batch
 * @packets_number [in]: Number of retrieved packets
 * @status [in]: Status of retrieved event batch (DOCA_SUCCESS in case of success callback)
 * @pkt_array [in]: Array of doca_bufs containing the received packets (NULL in case of error callback)
 * @event_batch_user_data [in]: User provided data, holds the per-queue resources of the receiving RXQ
 */
static void event_batch_shared_mempool_rcv_error_cb(
	struct doca_eth_rxq_event_batch_shared_mempool_recv *event_batch_shared_mempool_recv,
	uint16_t packets_number,
	doca_error_t status,
	struct doca_buf **pkt_array,
	union doca_data event_batch_user_data)
{
	struct eth_rxq_queue_resources *queue_res = event_batch_user_data.ptr;

	DOCA_LOG_ERR("Queue #%u: failed to receive packets, err: %s", queue_res->queue_id, doca_error_get_name(status));
}

/*
 * Destroy the ETH RXQ contexts sharing the memory pool
 *
 * @state [in]: eth_rxq_sample_objects struct to destroy its ETH RXQ contexts
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_rxq_ctxs(struct eth_rxq_sample_objects *state)
{
	doca_error_t status, ret = DOCA_SUCCESS;

	for (uint16_t i = 0; i < state->num_queues; i++) {
		if (state->queues[i].eth_rxq == NULL)
			continue;

		status = doca_ctx_stop(state->queues[i].ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA context of RXQ #%u, err: %s", i, doca_error_get_name(status));
			DOCA_ERROR_PROPAGATE(ret, status);
			continue;
		}

		status = doca_eth_rxq_destroy(state->queues[i].eth_rxq);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA ETH RXQ #%u, err: %s", i, doca_error_get_name(status));
			DOCA_ERROR_PROPAGATE(ret, status);
			continue;
		}
		state->queues[i].eth_rxq = NULL;
		state->queues[i].ctx = NULL;
	}

	return ret;
}

/*
 * Destroy ETH RXQ shared memory pool
 *
 * @state [in]: eth_rxq_sample_objects struct to destroy its shared memory pool
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_eth_rxq_shared_mempool(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	status = doca_eth_rxq_shared_mempool_stop(state->shared_mempool);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop ETH RXQ shared mempool, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_eth_rxq_shared_mempool_destroy(state->shared_mempool);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy ETH RXQ shared mempool, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Retrieve ETH RXQ event batches
 *
 * @state [in]: eth_rxq_sample_objects struct to retrieve event batches from
 */
static void retrieve_rxq_shared_mempool_recv_event_batches(struct eth_rxq_sample_objects *state)
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

	DOCA_LOG_INFO("Total packets received: %lu", state->total_received_packets);
}

/*
 * Create ETH RXQ shared memory pool
 *
 * @state [in/out]: eth_rxq_sample_objects struct to create its shared memory pool
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_eth_rxq_shared_mempool(struct eth_rxq_sample_objects *state)
{
	doca_error_t status, clean_status;

	state->devices_array[0] = state->core_resources.core_objs.dev;

	status = doca_eth_rxq_shared_mempool_create(MAX_BURST_SIZE,
						    state->num_queues,
						    MAX_PKT_SIZE,
						    state->devices_array,
						    SHARED_MEMPOOL_DEVICES_NUM,
						    &(state->shared_mempool));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ETH RXQ shared mempool, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_eth_rxq_shared_mempool_set_headroom_size(state->shared_mempool, state->headroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set shared mempool headroom size, err: %s", doca_error_get_name(status));
		goto destroy_shared_mempool;
	}

	status = doca_eth_rxq_shared_mempool_set_tailroom_size(state->shared_mempool, state->tailroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set shared mempool tailroom size, err: %s", doca_error_get_name(status));
		goto destroy_shared_mempool;
	}

	status = doca_eth_rxq_shared_mempool_start(state->shared_mempool);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start ETH RXQ shared mempool, err: %s", doca_error_get_name(status));
		goto destroy_shared_mempool;
	}

	return DOCA_SUCCESS;
destroy_shared_mempool:
	clean_status = doca_eth_rxq_shared_mempool_destroy(state->shared_mempool);
	state->shared_mempool = NULL;

	if (clean_status != DOCA_SUCCESS)
		return status;

	return status;
}

/*
 * Create a single ETH RXQ context sharing the memory pool
 *
 * @state [in/out]: eth_rxq_sample_objects struct holding the shared mempool and per-queue resources
 * @queue_index [in]: Index of the queue to create in the per-queue resources array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_eth_rxq_ctx(struct eth_rxq_sample_objects *state, uint16_t queue_index)
{
	doca_error_t status, clean_status;
	union doca_data user_data;
	struct doca_ctx *ctx;
	struct doca_eth_rxq *eth_rxq;

	state->queues[queue_index].state = state;
	state->queues[queue_index].queue_id = queue_index;

	status = doca_eth_rxq_create(state->core_resources.core_objs.dev, MAX_BURST_SIZE, MAX_PKT_SIZE, &eth_rxq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ETH RXQ context, err: %s", doca_error_get_name(status));
		return status;
	}
	state->queues[queue_index].eth_rxq = eth_rxq;

	status = doca_eth_rxq_set_type(eth_rxq, DOCA_ETH_RXQ_TYPE_SHARED_MEMPOOL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set type, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_shared_mempool(eth_rxq, state->shared_mempool);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set shared mempool, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	user_data.ptr = &(state->queues[queue_index]);
	status = doca_eth_rxq_event_batch_shared_mempool_recv_register(eth_rxq,
								       DOCA_EVENT_BATCH_EVENTS_NUMBER_64,
								       DOCA_EVENT_BATCH_EVENTS_NUMBER_32,
								       user_data,
								       event_batch_shared_mempool_rcv_success_cb,
								       event_batch_shared_mempool_rcv_error_cb);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register shared mempool receive event batch, err: %s",
			     doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_metadata_num(eth_rxq, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable metadata, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_rx_hash(eth_rxq, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable rx_hash, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_timestamp(eth_rxq, state->timestamp_enable);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set enable timestamp, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_packet_headroom(eth_rxq, state->headroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set packet headroom size, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_set_packet_tailroom(eth_rxq, state->tailroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set packet tailroom size, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	ctx = doca_eth_rxq_as_doca_ctx(eth_rxq);
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH RXQ context as DOCA context");
		status = DOCA_ERROR_UNEXPECTED;
		goto destroy_eth_rxq;
	}
	state->queues[queue_index].ctx = ctx;

	status = doca_pe_connect_ctx(state->core_resources.core_objs.pe, ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect PE, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_ctx_start(ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context, err: %s", doca_error_get_name(status));
		goto destroy_eth_rxq;
	}

	status = doca_eth_rxq_apply_queue_id(eth_rxq, queue_index);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply queue ID of RXQ, err: %s", doca_error_get_name(status));
		goto stop_ctx;
	}
	state->rxq_queue_ids[queue_index] = queue_index;

	return DOCA_SUCCESS;
stop_ctx:
	clean_status = doca_ctx_stop(ctx);
	if (clean_status != DOCA_SUCCESS)
		return status;
destroy_eth_rxq:
	clean_status = doca_eth_rxq_destroy(eth_rxq);
	state->queues[queue_index].eth_rxq = NULL;
	state->queues[queue_index].ctx = NULL;

	if (clean_status != DOCA_SUCCESS)
		return status;

	return status;
}

/*
 * Create the ETH RXQ contexts sharing the memory pool
 *
 * @state [in/out]: eth_rxq_sample_objects struct to create its ETH RXQ contexts
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_eth_rxq_ctxs(struct eth_rxq_sample_objects *state)
{
	doca_error_t status;

	for (uint16_t i = 0; i < state->num_queues; i++) {
		status = create_eth_rxq_ctx(state, i);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ETH RXQ context #%u, err: %s", i, doca_error_get_name(status));
			return status;
		}
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
		eth_flow_common_destroy_flow_pipe(&(state->flow_resources));

	if (state->flow_resources.df_port != NULL) {
		status = eth_flow_common_destroy_flow_port(&(state->flow_resources));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy flow port, err: %s", doca_error_get_name(status));
			return;
		}
	}

	eth_flow_common_cleanup_flow();

	status = destroy_eth_rxq_ctxs(state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy eth_rxq_ctxs, err: %s", doca_error_get_name(status));
		return;
	}

	if (state->shared_mempool != NULL) {
		status = destroy_eth_rxq_shared_mempool(state);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy shared mempool, err: %s", doca_error_get_name(status));
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
						    DOCA_ETH_RXQ_TYPE_SHARED_MEMPOOL,
						    DOCA_ETH_RXQ_DATA_PATH_TYPE_CPU);
	if (status != DOCA_SUCCESS && status != DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to check supported type, err: %s", doca_error_get_name(status));
		return status;
	}

	return status;
}

/*
 * Allocate the core resources (device + progress engine) needed by the shared mempool sample
 *
 * @state [in/out]: eth_rxq_sample_objects struct to populate with the device and progress engine
 * @ib_dev_name [in]: IB device name of a doca device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 *
 * @note In SHARED_MEMPOOL mode the packet buffers are managed by the shared memory pool, so unlike the managed mempool
 * sample there is no user-provided mmap to allocate here.
 */
static doca_error_t allocate_eth_core_resources_shared_mempool(struct eth_rxq_sample_objects *state,
							       const char *ib_dev_name)
{
	doca_error_t status;

	status = open_doca_device_with_ibdev_name((const uint8_t *)ib_dev_name,
						  DOCA_DEVINFO_IBDEV_NAME_SIZE,
						  check_device,
						  &(state->core_resources.core_objs.dev));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_pe_create(&(state->core_resources.core_objs.pe));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA progress engine, err: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Run ETH RXQ batch shared mempool receive
 *
 * @ib_dev_name [in]: IB device name of a doca device
 * @timestamp_enable [in]: timestamp enable
 * @headroom_size [in]: packet headroom size
 * @tailroom_size [in]: packet tailroom size
 * @num_queues [in]: number of ETH RXQs to share the memory pool between
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t eth_rxq_batch_shared_mempool_receive(const char *ib_dev_name,
						  bool timestamp_enable,
						  uint16_t headroom_size,
						  uint16_t tailroom_size,
						  uint16_t num_queues)
{
	doca_error_t status;
	struct eth_rxq_sample_objects state = {.total_received_packets = 0,
					       .num_queues = num_queues,
					       .timestamp_enable = timestamp_enable,
					       .headroom_size = headroom_size,
					       .tailroom_size = tailroom_size};
	struct eth_flow_common_config flow_cfg = {};

	status = allocate_eth_core_resources_shared_mempool(&state, ib_dev_name);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate core resources, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	status = create_eth_rxq_shared_mempool(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create/start ETH RXQ shared mempool, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	status = create_eth_rxq_ctxs(&state);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create/start ETH RXQ contexts, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	status = eth_flow_common_init_flow();
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	flow_cfg.dev = state.core_resources.core_objs.dev;

	status = eth_flow_common_create_flow_port(flow_cfg.dev, 0, &(state.flow_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow port, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	flow_cfg.rxq_queue_ids = state.rxq_queue_ids;
	flow_cfg.nb_queues = state.num_queues;

	status = eth_flow_common_create_flow_pipe(&flow_cfg, &(state.flow_resources));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow pipe, err: %s", doca_error_get_name(status));
		goto rxq_cleanup;
	}

	retrieve_rxq_shared_mempool_recv_event_batches(&state);
rxq_cleanup:
	eth_rxq_cleanup(&state);

	return status;
}
