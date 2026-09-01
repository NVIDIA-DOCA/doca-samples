/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

#include <inttypes.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#include <doca_buf.h>
#include <doca_common_defines.h>
#include <doca_ctx.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_eth_rxq_shared_mempool.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <samples/common.h>
#include <samples/doca_eth/eth_flow_common.h>

#include "eth_l2_fwd_core.h"

#define CACHELINE_SIZE 64	  /* Cacheline size */
#define NS_PER_SEC 1E9		  /* Nano-seconds per second */
#define STATS_MAX_BUFF_SIZE 10240 /* Max buffer size to hold statistics string */
#define NUM_OF_DEVICES 2	  /* Number of devices */

DOCA_LOG_REGISTER(ETH_L2_FWD::CORE);

/* Flag for forcing the application to stop and terminate gracefully */
static bool force_app_stop = false;

/* Previous time in nanoseconds */
uint64_t prev_time_ns;

/* Number of cores */
uint16_t true_nb_cores;

/* Global variable per thread for thread number */
__thread int thread_num;

/* Ethernet L2 Forwarding application device statistics */
struct eth_l2_fwd_dev_stats {
	uint64_t rx_pkts;	     /* Number of RX packets that were handled without being dropped */
	uint64_t prev_rx_pkts;	     /* Previous number of RX packets */
	uint64_t rx_dropped;	     /* Number of RX packets that were dropped (by SW) */
	uint64_t prev_rx_dropped;    /* Previous number of RX packets that were dropped (by SW) */
	uint64_t total_tx_pkts;	     /* Total number of TX packets */
	uint64_t prev_total_tx_pkts; /* Previous total number of TX packets */
};

/* Ethernet L2 Forwarding application statistics */
struct eth_l2_fwd_stats {
	struct eth_l2_fwd_dev_stats dev1_stats; /* Device 1 statistics */
	struct eth_l2_fwd_dev_stats dev2_stats; /* Device 2 statistics */
};

static struct eth_l2_fwd_stats stats[ETH_L2_FWD_MAX_NB_CORES] = {0};
static struct doca_eth_rxq_shared_mempool *shared_mempool = NULL;
static struct doca_dev *devices_array[NUM_OF_DEVICES];

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
	if (value % alignment == 0)
		return value;
	return value + alignment - (value % alignment);
}

/* Reset the application's statistics' reference point */
void reset_stats(void)
{
	for (uint16_t i = 0; i < true_nb_cores; i++) {
		stats[i].dev1_stats.prev_rx_pkts = stats[i].dev1_stats.rx_pkts;
		stats[i].dev1_stats.prev_rx_dropped = stats[i].dev1_stats.rx_dropped;
		stats[i].dev1_stats.prev_total_tx_pkts = stats[i].dev1_stats.total_tx_pkts;
	}

	struct timespec t;
	if (clock_gettime(CLOCK_REALTIME, &t) != 0) {
		DOCA_LOG_ERR("Failed to reset statistics: Failed to get time specification with clock_gettime()");
		return;
	}

	prev_time_ns = (uint64_t)t.tv_nsec + (uint64_t)t.tv_sec * NS_PER_SEC;
}

/* Print the application's statistics */
void print_stats(void)
{
	struct timespec t;
	size_t buff_size = STATS_MAX_BUFF_SIZE;
	char buff[buff_size];
	char *buff_cursor = buff;
	int curr_buff_offset = 0;

	if (clock_gettime(CLOCK_REALTIME, &t) != 0) {
		DOCA_LOG_ERR("Failed to print statistics: Failed to get time specification with clock_gettime()");
		return;
	}

	uint64_t curr_time_ns = (uint64_t)t.tv_nsec + (uint64_t)t.tv_sec * NS_PER_SEC;
	uint64_t diff_ns = curr_time_ns - prev_time_ns;

	curr_buff_offset += snprintf(
		buff_cursor,
		buff_size,
		"\n**************************** Printing statistics since last print/reset *****************************\n");

	curr_buff_offset += snprintf(
		buff_cursor + curr_buff_offset,
		buff_size - curr_buff_offset,
		"\n**************************** Forward statistics for device 1 *****************************\n");
	for (uint16_t i = 0; i < true_nb_cores; i++) {
		uint64_t diff_rx_pkts = stats[i].dev1_stats.rx_pkts - stats[i].dev1_stats.prev_rx_pkts;
		uint64_t diff_rx_dropped = stats[i].dev1_stats.rx_dropped - stats[i].dev1_stats.prev_rx_dropped;
		uint64_t diff_total_rx_pkts = diff_rx_pkts + diff_rx_dropped;
		uint64_t diff_tx_pkts = stats[i].dev1_stats.total_tx_pkts - stats[i].dev1_stats.prev_total_tx_pkts;
		uint64_t rx_pps = ((double)(diff_total_rx_pkts) / diff_ns) * NS_PER_SEC;
		uint64_t tx_pps = ((double)(diff_tx_pkts) / diff_ns) * NS_PER_SEC;

		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "\n**************************** Core %d *****************************\n",
					     i);
		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "RX-packets: %-17" PRIu64 " RX-SW-dropped: %-17" PRIu64
					     " RX-total: %-17" PRIu64 " TX-packets: %-17" PRIu64 "\n",
					     diff_rx_pkts,
					     diff_rx_dropped,
					     diff_total_rx_pkts,
					     diff_tx_pkts);
		curr_buff_offset +=
			snprintf(buff_cursor + curr_buff_offset, buff_size - curr_buff_offset, "\nThroughput\n");
		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "RX-pps: %-" PRIu64 "\n",
					     rx_pps);
		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "TX-pps: %-" PRIu64 "\n",
					     tx_pps);
	}
	curr_buff_offset += snprintf(
		buff_cursor + curr_buff_offset,
		buff_size - curr_buff_offset,
		"******************************************************************************************\n");

	curr_buff_offset += snprintf(
		buff_cursor + curr_buff_offset,
		buff_size - curr_buff_offset,
		"\n**************************** Forward statistics for device 2 *****************************\n");
	for (uint16_t i = 0; i < true_nb_cores; i++) {
		uint64_t diff_rx_pkts = stats[i].dev2_stats.rx_pkts - stats[i].dev2_stats.prev_rx_pkts;
		uint64_t diff_rx_dropped = stats[i].dev2_stats.rx_dropped - stats[i].dev2_stats.prev_rx_dropped;
		uint64_t diff_total_rx_pkts = diff_rx_pkts + diff_rx_dropped;
		uint64_t diff_tx_pkts = stats[i].dev2_stats.total_tx_pkts - stats[i].dev2_stats.prev_total_tx_pkts;
		uint64_t rx_pps = ((double)(diff_total_rx_pkts) / diff_ns) * NS_PER_SEC;
		uint64_t tx_pps = ((double)(diff_tx_pkts) / diff_ns) * NS_PER_SEC;

		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "\n**************************** Core %d *****************************\n",
					     i);
		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "RX-packets: %-17" PRIu64 " RX-SW-dropped: %-17" PRIu64
					     " RX-total: %-17" PRIu64 " TX-packets: %-17" PRIu64 "\n",
					     diff_rx_pkts,
					     diff_rx_dropped,
					     diff_total_rx_pkts,
					     diff_tx_pkts);
		curr_buff_offset +=
			snprintf(buff_cursor + curr_buff_offset, buff_size - curr_buff_offset, "\nThroughput\n");
		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "RX-pps: %-" PRIu64 "\n",
					     rx_pps);
		curr_buff_offset += snprintf(buff_cursor + curr_buff_offset,
					     buff_size - curr_buff_offset,
					     "TX-pps: %-" PRIu64 "\n",
					     tx_pps);
	}
	snprintf(buff_cursor + curr_buff_offset,
		 buff_size - curr_buff_offset,
		 "******************************************************************************************");

	DOCA_LOG_INFO("%s", buff);

	reset_stats();
}

void signal_handler(int signum)
{
	if (signum == SIGUSR1) {
		reset_stats();
	}
	if (signum == SIGUSR2) {
		print_stats();
	}
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		eth_l2_fwd_force_stop();
	}
}

/*
 * Function that checks if a device have the required TXQ/RXQ capabilities
 * to be used by open_doca_device_with_ibdev_name()
 *
 * @devinfo [in]: DOCA device info to query for capabilities
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t check_device_caps(struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result =
		doca_eth_txq_cap_is_type_supported(devinfo, DOCA_ETH_TXQ_TYPE_REGULAR, DOCA_ETH_TXQ_DATA_PATH_TYPE_CPU);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_eth_rxq_cap_is_type_supported(devinfo,
						    DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
						    DOCA_ETH_RXQ_DATA_PATH_TYPE_CPU);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/**
 * Create a DOCA mmap, initialize and start it
 *
 * @dev1 [in]: DOCA device to set to mmap
 * @dev2 [in]: DOCA device to set to mmap
 * @mmap_res [in/out]: DOCA mmap resources to use for mmap creation
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t create_mmap(struct doca_dev *dev1, struct doca_dev *dev2, struct mmap_resources *mmap_res)
{
	doca_error_t result;

	mmap_res->mmap_buffer = (uint8_t *)malloc(mmap_res->mmap_size + CACHELINE_SIZE);
	if (mmap_res->mmap_buffer == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for mmap's buffer");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_create(&mmap_res->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_permissions(mmap_res->mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memory permissions: %s", doca_error_get_descr(result));
		return result;
	}
	void *aligned_addr = (void *)align_up((uint64_t)mmap_res->mmap_buffer, CACHELINE_SIZE);
	result = doca_mmap_set_memrange(mmap_res->mmap, aligned_addr, mmap_res->mmap_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memory range: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_max_num_devices(mmap_res->mmap, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap max number of devices: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_add_dev(mmap_res->mmap, dev1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device 1 to mmap: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_mmap_add_dev(mmap_res->mmap, dev2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device 2 to mmap: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(mmap_res->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Destroy a DOCA mmap
 *
 * @mmap_res [in/out]: DOCA mmap resources to use for mmap destruction
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t destroy_mmap(struct mmap_resources *mmap_res)
{
	doca_error_t result;

	result = doca_mmap_destroy(mmap_res->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("MMAP destroyed successfully");
	free(mmap_res->mmap_buffer);

	return DOCA_SUCCESS;
}

/*
 * ETH RXQ managed receive event batch successful completion callback for device 1
 *
 * @event_batch_managed_recv [in]: The managed receive event batch
 * @events_number [in]: Number of retrieved events, each representing a single received packet
 * @event_batch_user_data [in]: User provided data, holding a pointer to the ETH TXQ context to forward the packets
 * with
 * @status [in]: Status of retrieved event batch
 * @pkt_array [in]: Array of doca_bufs containing the received packets
 */
static void rx_success_cb1(struct doca_eth_rxq_event_batch_managed_recv *event_batch_managed_recv,
			   uint16_t events_number,
			   union doca_data event_batch_user_data,
			   doca_error_t status,
			   struct doca_buf **pkt_array)
{
	/* Unused parameters */
	(void)status;
	(void)event_batch_managed_recv;

	doca_error_t result;
	struct doca_eth_txq *eth_txq = (struct doca_eth_txq *)(event_batch_user_data.ptr);
	union doca_data send_task_data;
	struct doca_buf **batch_pkt_array;
	union doca_data *task_user_data_array;
	struct doca_task_batch *task_batch_send;

	send_task_data.ptr = pkt_array; // This "trick" allows freeing the array when tx_success_cb is called

	result = doca_eth_txq_task_batch_send_allocate(eth_txq,
						       events_number,
						       send_task_data,
						       &batch_pkt_array,
						       &task_user_data_array,
						       &task_batch_send);
	if (doca_unlikely(result != DOCA_SUCCESS)) {
		stats[thread_num].dev1_stats.rx_dropped += events_number;
		doca_eth_rxq_event_batch_managed_recv_pkt_array_free(pkt_array);
		return;
	}

	memcpy(batch_pkt_array, pkt_array, sizeof(struct doca_buf *) * events_number);

	/* Return value is not checked since this is data-path (previous call result check prevents incorrect behavior)
	   When debugging, the return value should be checked */
	(void)doca_task_batch_submit(task_batch_send);

	stats[thread_num].dev1_stats.rx_pkts += events_number;
}

/*
 * ETH RXQ managed receive event batch successful completion callback for device 2
 *
 * @event_batch_managed_recv [in]: The managed receive event batch
 * @events_number [in]: Number of retrieved events, each representing a single received packet
 * @event_batch_user_data [in]: User provided data, holding a pointer to the ETH TXQ context to forward the packets
 * with
 * @status [in]: Status of retrieved event batch
 * @pkt_array [in]: Array of doca_bufs containing the received packets
 */
static void rx_success_cb2(struct doca_eth_rxq_event_batch_managed_recv *event_batch_managed_recv,
			   uint16_t events_number,
			   union doca_data event_batch_user_data,
			   doca_error_t status,
			   struct doca_buf **pkt_array)
{
	/* Unused parameters */
	(void)status;
	(void)event_batch_managed_recv;

	doca_error_t result;
	struct doca_eth_txq *eth_txq = (struct doca_eth_txq *)(event_batch_user_data.ptr);
	union doca_data send_task_data;
	struct doca_buf **batch_pkt_array;
	union doca_data *task_user_data_array;
	struct doca_task_batch *task_batch_send;

	send_task_data.ptr = pkt_array; // This "trick" allows freeing the array when tx_success_cb is called

	result = doca_eth_txq_task_batch_send_allocate(eth_txq,
						       events_number,
						       send_task_data,
						       &batch_pkt_array,
						       &task_user_data_array,
						       &task_batch_send);
	if (doca_unlikely(result != DOCA_SUCCESS)) {
		stats[thread_num].dev1_stats.rx_dropped += events_number;
		doca_eth_rxq_event_batch_managed_recv_pkt_array_free(pkt_array);
		return;
	}

	memcpy(batch_pkt_array, pkt_array, sizeof(struct doca_buf *) * events_number);

	/* Return value is not checked since this is data-path (previous call result check prevents incorrect behavior)
	   When debugging, the return value should be checked */
	(void)doca_task_batch_submit(task_batch_send);

	stats[thread_num].dev2_stats.rx_pkts += events_number;
}

/*
 * ETH RXQ shared mempool receive event batch successful completion callback for device 1
 *
 * @event_batch_shared_mempool_recv [in]: The shared mempool receive event batch
 * @packets_number [in]: Number of received packets
 * @status [in]: Status of event batch
 * @buf_array [in]: Array of doca_buf containing the received packets
 * @event_batch_shared_mempool_user_data [in]: User provided data, holding a pointer to the shared mempool and the ETH
 * TXQ context
 */
static void rx_shared_mempool_success_cb1(
	struct doca_eth_rxq_event_batch_shared_mempool_recv *event_batch_shared_mempool_recv,
	uint16_t packets_number,
	doca_error_t status,
	struct doca_buf **buf_array,
	union doca_data event_batch_shared_mempool_user_data)
{
	/* Unused parameters */
	(void)status;
	(void)event_batch_shared_mempool_recv;

	doca_error_t result;
	struct rxq_cb_shared_mempool_data *data =
		(struct rxq_cb_shared_mempool_data *)(event_batch_shared_mempool_user_data.ptr);
	struct doca_eth_txq *eth_txq = data->eth_txq;
	union doca_data send_task_data;
	struct doca_buf **batch_pkt_array;
	union doca_data *task_user_data_array;
	struct doca_task_batch *task_batch_send;

	send_task_data.ptr = data->shared_mempool;

	result = doca_eth_txq_task_batch_send_allocate(eth_txq,
						       packets_number,
						       send_task_data,
						       &batch_pkt_array,
						       &task_user_data_array,
						       &task_batch_send);
	if (doca_unlikely(result != DOCA_SUCCESS)) {
		stats[thread_num].dev1_stats.rx_dropped += packets_number;
		doca_eth_rxq_shared_mempool_free_buf_bulk(data->shared_mempool, buf_array, packets_number);
		return;
	}

	memcpy(batch_pkt_array, buf_array, sizeof(struct doca_buf *) * packets_number);

	/* Return value is not checked since this is data-path (previous call result check prevents incorrect behavior)
	When debugging, the return value should be checked */
	(void)doca_task_batch_submit(task_batch_send);

	stats[thread_num].dev1_stats.rx_pkts += packets_number;
}

/*
 * ETH RXQ shared mempool receive event batch successful completion callback for device 2
 *
 * @event_batch_shared_mempool_recv [in]: The shared mempool receive event batch
 * @packets_number [in]: Number of received packets
 * @status [in]: Status of event batch
 * @buf_array [in]: Array of doca_buf containing the received packets
 * @event_batch_shared_mempool_user_data [in]: User provided data, holding a pointer to the shared mempool and the ETH
 * TXQ context
 */
static void rx_shared_mempool_success_cb2(
	struct doca_eth_rxq_event_batch_shared_mempool_recv *event_batch_shared_mempool_recv,
	uint16_t packets_number,
	doca_error_t status,
	struct doca_buf **buf_array,
	union doca_data event_batch_shared_mempool_user_data)
{
	/* Unused parameters */
	(void)status;
	(void)event_batch_shared_mempool_recv;

	doca_error_t result;
	struct rxq_cb_shared_mempool_data *data =
		(struct rxq_cb_shared_mempool_data *)(event_batch_shared_mempool_user_data.ptr);
	struct doca_eth_txq *eth_txq = data->eth_txq;
	union doca_data send_task_data;
	struct doca_buf **batch_pkt_array;
	union doca_data *task_user_data_array;
	struct doca_task_batch *task_batch_send;

	send_task_data.ptr = data->shared_mempool;

	result = doca_eth_txq_task_batch_send_allocate(eth_txq,
						       packets_number,
						       send_task_data,
						       &batch_pkt_array,
						       &task_user_data_array,
						       &task_batch_send);
	if (doca_unlikely(result != DOCA_SUCCESS)) {
		stats[thread_num].dev2_stats.rx_dropped += packets_number;
		doca_eth_rxq_shared_mempool_free_buf_bulk(data->shared_mempool, buf_array, packets_number);
		return;
	}

	memcpy(batch_pkt_array, buf_array, sizeof(struct doca_buf *) * packets_number);

	/* Return value is not checked since this is data-path (previous call result check prevents incorrect behavior)
	When debugging, the return value should be checked */
	(void)doca_task_batch_submit(task_batch_send);

	stats[thread_num].dev2_stats.rx_pkts += packets_number;
}

/*
 * A common ETH RXQ managed receive event batch error completion callback
 *
 * @event_batch_managed_recv [in]: The managed receive event batch
 * @events_number [in]: Number of retrieved events, each representing a single received packet
 * @event_batch_user_data [in]: User provided data, holding a pointer to the ETH TXQ context to forward the packets
 * with
 * @status [in]: Status of retrieved event batch
 * @pkt_array [in]: Array of doca_bufs containing the received packets
 */
static void rx_error_cb(struct doca_eth_rxq_event_batch_managed_recv *event_batch_managed_recv,
			uint16_t events_number,
			union doca_data event_batch_user_data,
			doca_error_t status,
			struct doca_buf **pkt_array)
{
	/* Unused parameters */
	(void)events_number;
	(void)pkt_array;
	(void)event_batch_managed_recv;

	struct doca_eth_txq *eth_txq = (struct doca_eth_txq *)(event_batch_user_data.ptr);
	struct doca_ctx *eth_txq_ctx = doca_eth_txq_as_doca_ctx(eth_txq);
	enum doca_ctx_states ctx_state;
	(void)doca_ctx_get_state(eth_txq_ctx, &ctx_state);

	/* If the context is stopping then this callback was expectedly called during a draining process */
	if (ctx_state != DOCA_CTX_STATE_STOPPING)
		DOCA_LOG_ERR("Failed to receive packets: %s", doca_error_get_name(status));

	force_app_stop = true;
}

/*
 * A common ETH RXQ shared mempool receive event batch error completion callback
 *
 * @event_batch_shared_mempool_recv [in]: The shared mempool receive event batch
 * @packets_number [in]: Number of received packets
 * @status [in]: Status of event batch
 * @buf_array [in]: Array of doca_buf containing the received packets
 * @event_batch_shared_mempool_user_data [in]: User provided data, holding a pointer to the shared mempool and the ETH
 * TXQ context
 */
static void rx_shared_mempool_error_cb(
	struct doca_eth_rxq_event_batch_shared_mempool_recv *event_batch_shared_mempool_recv,
	uint16_t packets_number,
	doca_error_t status,
	struct doca_buf **buf_array,
	union doca_data event_batch_shared_mempool_user_data)
{
	/* Unused parameters */
	(void)packets_number;
	(void)buf_array;
	(void)event_batch_shared_mempool_recv;

	struct rxq_cb_shared_mempool_data *data =
		(struct rxq_cb_shared_mempool_data *)(event_batch_shared_mempool_user_data.ptr);
	struct doca_eth_txq *eth_txq = data->eth_txq;
	struct doca_ctx *eth_txq_ctx = doca_eth_txq_as_doca_ctx(eth_txq);
	enum doca_ctx_states ctx_state;
	(void)doca_ctx_get_state(eth_txq_ctx, &ctx_state);

	/* If the context is stopping then this callback was expectedly called during a draining process */
	if (ctx_state != DOCA_CTX_STATE_STOPPING)
		DOCA_LOG_ERR("Failed to receive packets: %s", doca_error_get_name(status));

	force_app_stop = true;
}

/*
 * ETH TXQ send task batch successful completion callback for device 1
 *
 * @task_batch [in]: Completed task batch
 * @tasks_num [in]: Task number associated with task batch
 * @ctx_user_data [in]: Context's user provided data
 * @task_batch_user_data [in]: Task batch user provided data, holding the packets array from the RX success callback
 * @task_user_data_array [in]: Array of user provided data, each used for identifying each task behind task batch
 * @pkt_array [in]: Array of packets, each associated to one send task that's part of the send task batch
 * @status_array [in]: Array of status, each associated to one send task that's part of the send task batch
 */
static void tx_success_cb1(struct doca_task_batch *task_batch,
			   uint16_t tasks_num,
			   union doca_data ctx_user_data,
			   union doca_data task_batch_user_data,
			   union doca_data *task_user_data_array,
			   struct doca_buf **pkt_array,
			   doca_error_t *status_array)
{
	/* Unused parameters */
	(void)tasks_num;
	(void)ctx_user_data;
	(void)task_user_data_array;
	(void)pkt_array;
	(void)status_array;

	struct doca_buf **pkt_array_handle = (struct doca_buf **)task_batch_user_data.ptr;
	doca_eth_rxq_event_batch_managed_recv_pkt_array_free(pkt_array_handle);

	doca_task_batch_free(task_batch);

	stats[thread_num].dev1_stats.total_tx_pkts += tasks_num;
}

/*
 * ETH TXQ send task batch successful completion callback for device 2
 *
 * @task_batch [in]: Completed task batch
 * @tasks_num [in]: Task number associated with task batch
 * @ctx_user_data [in]: Context's user provided data
 * @task_batch_user_data [in]: Task batch user provided data, holding the packets array from the RX success callback
 * @task_user_data_array [in]: Array of user provided data, each used for identifying each task behind task batch
 * @pkt_array [in]: Array of packets, each associated to one send task that's part of the send task batch
 * @status_array [in]: Array of status, each associated to one send task that's part of the send task batch
 */
static void tx_success_cb2(struct doca_task_batch *task_batch,
			   uint16_t tasks_num,
			   union doca_data ctx_user_data,
			   union doca_data task_batch_user_data,
			   union doca_data *task_user_data_array,
			   struct doca_buf **pkt_array,
			   doca_error_t *status_array)
{
	/* Unused parameters */
	(void)tasks_num;
	(void)ctx_user_data;
	(void)task_user_data_array;
	(void)pkt_array;
	(void)status_array;

	struct doca_buf **pkt_array_handle = (struct doca_buf **)task_batch_user_data.ptr;
	doca_eth_rxq_event_batch_managed_recv_pkt_array_free(pkt_array_handle);

	doca_task_batch_free(task_batch);

	stats[thread_num].dev2_stats.total_tx_pkts += tasks_num;
}

/*
 * ETH TXQ send task batch successful completion callback for device 1 (when using shared mempool mode for ETH RXQ)
 *
 * @task_batch [in]: Completed task batch
 * @tasks_num [in]: Task number associated with task batch
 * @ctx_user_data [in]: Context's user provided data
 * @task_batch_user_data [in]: Task batch user provided data, holding the packets array from the RX success callback
 * @task_user_data_array [in]: Array of user provided data, each used for identifying each task behind task batch
 * @pkt_array [in]: Array of packets, each associated to one send task that's part of the send task batch
 * @status_array [in]: Array of status, each associated to one send task that's part of the send task batch
 */
static void tx_shared_mempool_success_cb1(struct doca_task_batch *task_batch,
					  uint16_t tasks_num,
					  union doca_data ctx_user_data,
					  union doca_data task_batch_user_data,
					  union doca_data *task_user_data_array,
					  struct doca_buf **pkt_array,
					  doca_error_t *status_array)
{
	/* Unused parameters */
	(void)tasks_num;
	(void)ctx_user_data;
	(void)task_user_data_array;
	(void)pkt_array;
	(void)status_array;

	struct doca_eth_rxq_shared_mempool *shared_mempool =
		(struct doca_eth_rxq_shared_mempool *)task_batch_user_data.ptr;
	doca_eth_rxq_shared_mempool_free_buf_bulk(shared_mempool, (struct doca_buf **)pkt_array, tasks_num);

	doca_task_batch_free(task_batch);

	stats[thread_num].dev1_stats.total_tx_pkts += tasks_num;
}

/*
 * ETH TXQ send task batch successful completion callback for device 2 (when using shared mempool mode for ETH RXQ)
 *
 * @task_batch [in]: Completed task batch
 * @tasks_num [in]: Task number associated with task batch
 * @ctx_user_data [in]: Context's user provided data
 * @task_batch_user_data [in]: Task batch user provided data, holding the packets array from the RX success callback
 * @task_user_data_array [in]: Array of user provided data, each used for identifying each task behind task batch
 * @pkt_array [in]: Array of packets, each associated to one send task that's part of the send task batch
 * @status_array [in]: Array of status, each associated to one send task that's part of the send task batch
 */
static void tx_shared_mempool_success_cb2(struct doca_task_batch *task_batch,
					  uint16_t tasks_num,
					  union doca_data ctx_user_data,
					  union doca_data task_batch_user_data,
					  union doca_data *task_user_data_array,
					  struct doca_buf **pkt_array,
					  doca_error_t *status_array)
{
	/* Unused parameters */
	(void)tasks_num;
	(void)ctx_user_data;
	(void)task_user_data_array;
	(void)pkt_array;
	(void)status_array;

	struct doca_eth_rxq_shared_mempool *shared_mempool =
		(struct doca_eth_rxq_shared_mempool *)task_batch_user_data.ptr;
	doca_eth_rxq_shared_mempool_free_buf_bulk(shared_mempool, (struct doca_buf **)pkt_array, tasks_num);

	doca_task_batch_free(task_batch);

	stats[thread_num].dev2_stats.total_tx_pkts += tasks_num;
}

/*
 * A common ETH TXQ send task batch error completion callback
 *
 * @task_batch [in]: Completed task batch
 * @tasks_num [in]: Task number associated with task batch
 * @ctx_user_data [in]: Context's user provided data
 * @task_batch_user_data [in]: Task batch user provided data, holding the packets array from the RX success callback
 * @task_user_data_array [in]: Array of user provided data, each used for identifying each task behind task batch
 * @pkt_array [in]: Array of packets, each associated to one send task that's part of the send task batch
 * @status_array [in]: Array of status, each associated to one send task that's part of the send task batch
 */
static void tx_error_cb(struct doca_task_batch *task_batch,
			uint16_t tasks_num,
			union doca_data ctx_user_data,
			union doca_data task_batch_user_data,
			union doca_data *task_user_data_array,
			struct doca_buf **pkt_array,
			doca_error_t *status_array)
{
	/* Unused parameters */
	(void)tasks_num;
	(void)ctx_user_data;
	(void)task_user_data_array;
	(void)pkt_array;
	(void)status_array;

	struct doca_ctx *eth_txq_ctx = doca_task_batch_get_ctx(task_batch);
	enum doca_ctx_states ctx_state;
	(void)doca_ctx_get_state(eth_txq_ctx, &ctx_state);

	/* If the context is stopping then this callback was expectedly called during a draining process */
	if (ctx_state != DOCA_CTX_STATE_STOPPING)
		DOCA_LOG_ERR("Failed to send packets");

	struct doca_buf **pkt_array_handle = (struct doca_buf **)task_batch_user_data.ptr;
	doca_eth_rxq_event_batch_managed_recv_pkt_array_free(pkt_array_handle);

	doca_task_batch_free(task_batch);

	force_app_stop = true;
}

/*
 * A common ETH TXQ send task batch error completion callback (when using shared mempool mode for ETH RXQ)
 *
 * @task_batch [in]: Completed task batch
 * @tasks_num [in]: Task number associated with task batch
 * @ctx_user_data [in]: Context's user provided data
 * @task_batch_user_data [in]: Task batch user provided data, holding the packets array from the RX success callback
 * @task_user_data_array [in]: Array of user provided data, each used for identifying each task behind task batch
 * @pkt_array [in]: Array of packets, each associated to one send task that's part of the send task batch
 * @status_array [in]: Array of status, each associated to one send task that's part of the send task batch
 */
static void tx_shared_mempool_error_cb(struct doca_task_batch *task_batch,
				       uint16_t tasks_num,
				       union doca_data ctx_user_data,
				       union doca_data task_batch_user_data,
				       union doca_data *task_user_data_array,
				       struct doca_buf **pkt_array,
				       doca_error_t *status_array)
{
	/* Unused parameters */
	(void)tasks_num;
	(void)ctx_user_data;
	(void)task_user_data_array;
	(void)pkt_array;
	(void)status_array;

	struct doca_ctx *eth_txq_ctx = doca_task_batch_get_ctx(task_batch);
	enum doca_ctx_states ctx_state;
	(void)doca_ctx_get_state(eth_txq_ctx, &ctx_state);

	/* If the context is stopping then this callback was expectedly called during a draining process */
	if (ctx_state != DOCA_CTX_STATE_STOPPING)
		DOCA_LOG_ERR("Failed to send packets");

	struct doca_eth_rxq_shared_mempool *shared_mempool =
		(struct doca_eth_rxq_shared_mempool *)task_batch_user_data.ptr;
	doca_eth_rxq_shared_mempool_free_buf_bulk(shared_mempool, (struct doca_buf **)pkt_array, tasks_num);

	doca_task_batch_free(task_batch);

	force_app_stop = true;
}

static doca_error_t get_rx_batch_size(uint16_t batch_size, enum doca_event_batch_events_number *batch_events_num)
{
	switch (batch_size) {
	case 4:
		(*batch_events_num) = DOCA_EVENT_BATCH_EVENTS_NUMBER_4;
		return DOCA_SUCCESS;
	case 8:
		(*batch_events_num) = DOCA_EVENT_BATCH_EVENTS_NUMBER_8;
		return DOCA_SUCCESS;
	case 16:
		(*batch_events_num) = DOCA_EVENT_BATCH_EVENTS_NUMBER_16;
		return DOCA_SUCCESS;
	case 32:
		(*batch_events_num) = DOCA_EVENT_BATCH_EVENTS_NUMBER_32;
		return DOCA_SUCCESS;
	case 64:
		(*batch_events_num) = DOCA_EVENT_BATCH_EVENTS_NUMBER_64;
		return DOCA_SUCCESS;
	case 128:
		(*batch_events_num) = DOCA_EVENT_BATCH_EVENTS_NUMBER_128;
		return DOCA_SUCCESS;
	default:
		DOCA_LOG_ERR("Failed to set batch size, must be power of two and a max of 128");
		return DOCA_ERROR_INVALID_VALUE;
	}
}

/*
 * Initialize (create and start) an ETH RXQ context
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration to use for context creation
 * @dev_resrc [in]: Resources of the device to create the ETH RXQ context with
 * @pe [in]: DOCA progress engine to which the ETH RXQ context will be connected
 * @rx_success_cb [in]: RXQ event batch successful completion callback to set in registration
 * @queue_id [in]: Queue ID for the ETH RXQ context
 * @data [in]: Pointer to data to save as user_data
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t init_eth_rxq_ctx(struct eth_l2_fwd_cfg *cfg,
				     struct eth_l2_fwd_dev_resources *dev_resrc,
				     struct doca_pe *pe,
				     void *rx_success_cb,
				     uint16_t queue_id,
				     void *data)
{
	union doca_data user_data;
	doca_error_t result;
	uint32_t max_burst_size, max_pkt_size;

	result = doca_eth_rxq_cap_get_max_burst_size(doca_dev_as_devinfo(dev_resrc->mlxdev), &max_burst_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get RXQ max burst size capability: %s", doca_error_get_descr(result));
		return result;
	}

	if (max_burst_size < cfg->rx_depth) {
		DOCA_LOG_ERR("Failed to create ETH RXQ context: RX depth to set is to big");
		return DOCA_ERROR_TOO_BIG;
	}

	result = doca_eth_rxq_cap_get_max_packet_size(doca_dev_as_devinfo(dev_resrc->mlxdev), &max_pkt_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get RXQ max packet size capability: %s", doca_error_get_descr(result));
		return result;
	}

	if (max_pkt_size < cfg->max_pkt_size) {
		DOCA_LOG_ERR("Failed to create ETH RXQ context: max packet size to set is to big");
		return DOCA_ERROR_TOO_BIG;
	}

	result = doca_eth_rxq_create(dev_resrc->mlxdev, cfg->rx_depth, cfg->max_pkt_size, &dev_resrc->eth_rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ETH RXQ context: %s", doca_error_get_descr(result));
		return result;
	}

	enum doca_eth_rxq_type type = cfg->use_shared_mempool ? DOCA_ETH_RXQ_TYPE_SHARED_MEMPOOL :
								DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL;
	result = doca_eth_rxq_set_type(dev_resrc->eth_rxq, type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ETH RXQ type: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_rxq_set_cqe_format(dev_resrc->eth_rxq, cfg->cqe_format);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CQE format: %s", doca_error_get_descr(result));
		return result;
	}

	if (cfg->use_shared_mempool) {
		result = doca_eth_rxq_set_shared_mempool(dev_resrc->eth_rxq, shared_mempool);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set shared mempool: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_eth_rxq_set_pkt_buf(dev_resrc->eth_rxq,
						  dev_resrc->mmap_resrc.mmap,
						  0,
						  dev_resrc->mmap_resrc.mmap_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set packet buffer: %s", doca_error_get_descr(result));
			return result;
		}
	}

	user_data.ptr = data;

	enum doca_event_batch_events_number batch_size;
	result = get_rx_batch_size(cfg->batch_size, &batch_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get batch size: %s", doca_error_get_descr(result));
		return result;
	}

	if (cfg->use_shared_mempool) {
		result = doca_eth_rxq_event_batch_shared_mempool_recv_register(
			dev_resrc->eth_rxq,
			batch_size,
			DOCA_EVENT_BATCH_EVENTS_NUMBER_1,
			user_data,
			(doca_eth_rxq_event_batch_shared_mempool_recv_handler_cb_t)rx_success_cb,
			rx_shared_mempool_error_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register shared mempool receive event batch: %s",
				     doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_eth_rxq_event_batch_managed_recv_register(
			dev_resrc->eth_rxq,
			batch_size,
			DOCA_EVENT_BATCH_EVENTS_NUMBER_1,
			user_data,
			(doca_eth_rxq_event_batch_managed_recv_handler_cb_t)rx_success_cb,
			rx_error_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register managed receive event batch: %s",
				     doca_error_get_descr(result));
			return result;
		}
	}

	dev_resrc->eth_rxq_ctx = doca_eth_rxq_as_doca_ctx(dev_resrc->eth_rxq);
	if (dev_resrc->eth_rxq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH RXQ context as DOCA context: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(pe, dev_resrc->eth_rxq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PE for ETH RXQ context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(dev_resrc->eth_rxq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_rxq_apply_queue_id(dev_resrc->eth_rxq, queue_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply queue ID of RXQ: %s", doca_error_get_descr(result));
		return result;
	}
	dev_resrc->rxq_queue_id = queue_id;

	return DOCA_SUCCESS;
}

static doca_error_t get_tx_batch_size(uint16_t batch_size, enum doca_task_batch_max_tasks_number *max_tasks_num)
{
	switch (batch_size) {
	case 16:
		(*max_tasks_num) = DOCA_TASK_BATCH_MAX_TASKS_NUMBER_16;
		return DOCA_SUCCESS;
	case 32:
		(*max_tasks_num) = DOCA_TASK_BATCH_MAX_TASKS_NUMBER_32;
		return DOCA_SUCCESS;
	case 64:
		(*max_tasks_num) = DOCA_TASK_BATCH_MAX_TASKS_NUMBER_64;
		return DOCA_SUCCESS;
	case 128:
		(*max_tasks_num) = DOCA_TASK_BATCH_MAX_TASKS_NUMBER_128;
		return DOCA_SUCCESS;
	default:
		DOCA_LOG_ERR("Failed to set batch size, must be power of two and a max of 128");
		return DOCA_ERROR_INVALID_VALUE;
	}
}

/*
 * Initialize (create and start) an ETH TXQ context
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration to use for context creation and configuration
 * @dev_resrc [in]: Resources of the device to create the ETH TXQ context with
 * @pe [in]: DOCA progress engine to which the ETH TXQ context will be connected
 * @tx_success_cb [in]: TXQ task batch send successful completion callback to set in configuration
 * @queue_id [in]: Queue ID for the ETH TXQ context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t init_eth_txq_ctx(struct eth_l2_fwd_cfg *cfg,
				     struct eth_l2_fwd_dev_resources *dev_resrc,
				     struct doca_pe *pe,
				     doca_eth_txq_task_batch_send_completion_cb_t tx_success_cb,
				     uint16_t queue_id)
{
	doca_error_t result;
	uint32_t max_burst_size;

	result = doca_eth_txq_cap_get_max_burst_size(doca_dev_as_devinfo(dev_resrc->mlxdev), 1, 0, &max_burst_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get TXQ max burst size capability: %s", doca_error_get_descr(result));
		return result;
	}

	if (max_burst_size < cfg->tx_depth) {
		DOCA_LOG_ERR("Failed to create ETH TXQ context: TX depth to set is to big");
		return DOCA_ERROR_TOO_BIG;
	}

	result = doca_eth_txq_create(dev_resrc->mlxdev, cfg->tx_depth, &dev_resrc->eth_txq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ETH TXQ context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_txq_set_type(dev_resrc->eth_txq, DOCA_ETH_TXQ_TYPE_REGULAR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ETH TXQ type: %s", doca_error_get_descr(result));
		return result;
	}

	if (cfg->txq_inline_max > 0) {
		uint16_t hw_max;
		uint16_t effective_max = 0;

		result = doca_eth_txq_cap_get_max_inline_data_size(doca_dev_as_devinfo(dev_resrc->mlxdev), &hw_max);
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_WARN("Inline data not supported by device, disabling inline");
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get max inline data size capability: %s", doca_error_get_descr(result));
			return result;
		} else {
			effective_max = cfg->txq_inline_max < hw_max ? cfg->txq_inline_max : hw_max;

			/* Apply the cores_num_min_inline gate: when the core count is below the
			 * threshold, disable inline. Computed from CLI-only cfg fields, so it is
			 * consistent across threads.
			 */
			if (cfg->number_cores < cfg->cores_num_min_inline) {
				DOCA_LOG_INFO(
					"Inline disabled: number_cores=%u < cores_num_min_inline=%u (cap=%u, requested max=%u)",
					cfg->number_cores,
					cfg->cores_num_min_inline,
					hw_max,
					cfg->txq_inline_max);
				effective_max = 0;
			}
		}

		result = doca_eth_txq_set_max_inline_data_size(dev_resrc->eth_txq, effective_max);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set max inline data size: %s", doca_error_get_descr(result));
			return result;
		}
	}

	enum doca_task_batch_max_tasks_number batch_size;
	result = get_tx_batch_size(cfg->batch_size, &batch_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get batch size: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_txq_task_batch_send_set_conf(dev_resrc->eth_txq,
						       batch_size,
						       ((cfg->tx_depth / cfg->batch_size) * 4),
						       tx_success_cb,
						       cfg->use_shared_mempool ? tx_shared_mempool_error_cb :
										 tx_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure send task: %s", doca_error_get_descr(result));
		return result;
	}

	dev_resrc->eth_txq_ctx = doca_eth_txq_as_doca_ctx(dev_resrc->eth_txq);
	if (dev_resrc->eth_txq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH TXQ context as DOCA context: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(pe, dev_resrc->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PE for ETH TXQ context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(dev_resrc->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_txq_apply_queue_id(dev_resrc->eth_txq, queue_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply queue ID of TXQ: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Forward packets function per thread
 *
 * @args [in]: Pointer to app resources
 * @return: NULL
 */
void *start_forwarding(void *args)
{
	DOCA_LOG_INFO("Starting packets forwarding");
	struct eth_l2_fwd_resources *state = (struct eth_l2_fwd_resources *)args;
	thread_num = state->thread_num;
	struct doca_pe *pe = state->pe;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(thread_num + 1, &cpuset);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
		DOCA_LOG_ERR("Failed to set thread core affinity for thread %d", thread_num);
		force_app_stop = true;
		return NULL;
	}

	while (!force_app_stop) {
		(void)doca_pe_progress(pe);
	}

	return NULL;
}

doca_error_t eth_l2_fwd_general_init(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state)
{
	doca_error_t result, clean_result;
	true_nb_cores = cfg->number_cores;
	uint32_t recommended_size = 0;

	result = open_doca_device_with_ibdev_name((uint8_t *)cfg->mlxdev_name1,
						  strlen(cfg->mlxdev_name1),
						  check_device_caps,
						  &(state[0].dev_resrc1.mlxdev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open the first device based on IB device name: %s", cfg->mlxdev_name1);
		return result;
	}

	result = open_doca_device_with_ibdev_name((uint8_t *)cfg->mlxdev_name2,
						  strlen(cfg->mlxdev_name2),
						  check_device_caps,
						  &(state[0].dev_resrc2.mlxdev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open the second device based on IB device name: %s", cfg->mlxdev_name2);
		goto close_dev1;
	}

	result = doca_eth_rxq_cap_is_cqe_format_supported(doca_dev_as_devinfo(state[0].dev_resrc1.mlxdev),
							  cfg->cqe_format);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Device %s does not support CQE format %d", cfg->mlxdev_name1, (int)cfg->cqe_format);
		goto close_dev2;
	}

	result = doca_eth_rxq_cap_is_cqe_format_supported(doca_dev_as_devinfo(state[0].dev_resrc2.mlxdev),
							  cfg->cqe_format);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Device %s does not support CQE format %d", cfg->mlxdev_name2, (int)cfg->cqe_format);
		goto close_dev2;
	}

	if (cfg->use_shared_mempool) {
		devices_array[0] = state[0].dev_resrc1.mlxdev;
		devices_array[1] = state[0].dev_resrc2.mlxdev;
		result = doca_eth_rxq_shared_mempool_create(cfg->rx_depth,
							    NUM_OF_DEVICES * cfg->number_cores,
							    cfg->max_pkt_size,
							    devices_array,
							    NUM_OF_DEVICES,
							    &shared_mempool);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create shared mempool, err: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_eth_rxq_shared_mempool_start(shared_mempool);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start shared mempool, err: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
							       cfg->pkts_recv_rate,
							       cfg->pkt_max_process_time,
							       cfg->max_pkt_size,
							       cfg->rx_depth,
							       ETH_L2_FWD_LOG_MAX_LRO_DEFAULT,
							       0,
							       0,
							       &recommended_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to estimate packet buffer size: %s", doca_error_get_descr(result));
			goto close_dev2;
		}
	}

	result = eth_flow_common_init_flow();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow, err: %s", doca_error_get_descr(result));
		goto close_dev2;
	}

	for (int i = 0; i < true_nb_cores; i++) {
		state[i].dev_resrc1.mlxdev = state[0].dev_resrc1.mlxdev;
		state[i].dev_resrc1.mmap_resrc.mmap_size = recommended_size;
		state[i].dev_resrc1.rxq_cb_data.shared_mempool = shared_mempool;
		state[i].dev_resrc2.mlxdev = state[0].dev_resrc2.mlxdev;
		state[i].dev_resrc2.mmap_resrc.mmap_size = recommended_size;
		state[i].dev_resrc2.rxq_cb_data.shared_mempool = shared_mempool;
	}

	return DOCA_SUCCESS;

close_dev2:
	clean_result = doca_dev_close(state[0].dev_resrc2.mlxdev);
	if (clean_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device 2: %s", doca_error_get_descr(clean_result));
		return result;
	}
close_dev1:
	clean_result = doca_dev_close(state[0].dev_resrc1.mlxdev);
	if (clean_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device 1: %s", doca_error_get_descr(clean_result));
		return result;
	}

	return result;
}

doca_error_t eth_l2_fwd_general_cleanup(struct eth_l2_fwd_resources *state)
{
	doca_error_t result;

	eth_flow_common_cleanup_flow();

	if (shared_mempool != NULL) {
		result = doca_eth_rxq_shared_mempool_stop(shared_mempool);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop shared mempool, err: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_eth_rxq_shared_mempool_destroy(shared_mempool);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy shared mempool, err: %s", doca_error_get_descr(result));
			return result;
		}
	}

	result = doca_dev_close(state[0].dev_resrc2.mlxdev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device 2: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_dev_close(state[0].dev_resrc1.mlxdev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device 1: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t eth_l2_fwd_flow_init(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state)
{
	doca_error_t result, clean_result;
	struct eth_flow_common_config flow_cfg;

	flow_cfg.rxq_queue_ids = (uint16_t *)malloc(true_nb_cores * sizeof(uint16_t));
	if (flow_cfg.rxq_queue_ids == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for flow queue IDs");
		return DOCA_ERROR_NO_MEMORY;
	}

	flow_cfg.nb_queues = true_nb_cores;

	result = eth_flow_common_create_flow_port(state[0].dev_resrc1.mlxdev, 0, &(state[0].dev_resrc1.flow_resrc));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow port for device 1: %s", doca_error_get_descr(result));
		goto destroy_queue_ids;
	}

	result = eth_flow_common_create_flow_port(state[0].dev_resrc2.mlxdev, 1, &(state[0].dev_resrc2.flow_resrc));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow port for device 2: %s", doca_error_get_descr(result));
		goto destroy_flow_port1;
	}

	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		for (int i = 0; i < true_nb_cores; i++) {
			flow_cfg.rxq_queue_ids[i] = state[i].dev_resrc1.rxq_queue_id;
		}
		flow_cfg.dev = state[0].dev_resrc1.mlxdev;

		result = eth_flow_common_create_flow_pipe(&flow_cfg, &(state[0].dev_resrc1.flow_resrc));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create flow pipe for device 1: %s", doca_error_get_descr(result));
			goto destroy_flow_port2;
		}
	}
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 2) {
		for (int i = 0; i < true_nb_cores; i++) {
			flow_cfg.rxq_queue_ids[i] = state[i].dev_resrc2.rxq_queue_id;
		}
		flow_cfg.dev = state[0].dev_resrc2.mlxdev;

		result = eth_flow_common_create_flow_pipe(&flow_cfg, &(state[0].dev_resrc2.flow_resrc));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create flow pipe for device 2: %s", doca_error_get_descr(result));
			goto destroy_flow_pipe1;
		}
	}

	goto destroy_queue_ids;

destroy_flow_pipe1:
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		eth_flow_common_destroy_flow_pipe(&(state[0].dev_resrc1.flow_resrc));
	}
destroy_flow_port2:
	clean_result = eth_flow_common_destroy_flow_port(&(state[0].dev_resrc2.flow_resrc));
	if (clean_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy flow port for device 2: %s", doca_error_get_descr(clean_result));
		return result;
	}
destroy_flow_port1:
	clean_result = eth_flow_common_destroy_flow_port(&(state[0].dev_resrc1.flow_resrc));
	if (clean_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy flow port for device 1: %s", doca_error_get_descr(clean_result));
		return result;
	}
destroy_queue_ids:
	free(flow_cfg.rxq_queue_ids);
	flow_cfg.rxq_queue_ids = NULL;
	return result;
}

doca_error_t eth_l2_fwd_flow_cleanup(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state)
{
	doca_error_t result;

	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		eth_flow_common_destroy_flow_pipe(&(state[0].dev_resrc1.flow_resrc));
	}

	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 2) {
		eth_flow_common_destroy_flow_pipe(&(state[0].dev_resrc2.flow_resrc));
	}

	result = eth_flow_common_destroy_flow_port(&(state[0].dev_resrc2.flow_resrc));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy flow port for device 2: %s", doca_error_get_descr(result));
		return result;
	}

	result = eth_flow_common_destroy_flow_port(&(state[0].dev_resrc1.flow_resrc));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy flow port for device 1: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t eth_l2_fwd_per_thread_init(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state, uint16_t t_num)
{
	doca_error_t result, clean_result;

	result = doca_pe_create(&state->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		if (!cfg->use_shared_mempool) {
			result = create_mmap(state->dev_resrc1.mlxdev,
					     state->dev_resrc2.mlxdev,
					     &state->dev_resrc1.mmap_resrc);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create mmap for device 1: %s", doca_error_get_descr(result));
				goto destroy_pe;
			}
		}

		result = init_eth_txq_ctx(cfg,
					  &state->dev_resrc2,
					  state->pe,
					  cfg->use_shared_mempool ? tx_shared_mempool_success_cb2 : tx_success_cb2,
					  t_num * 2);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize ETH TXQ context for device 2: %s",
				     doca_error_get_descr(result));
			goto destroy_mmap1;
		}

		state->dev_resrc2.rxq_cb_data.eth_txq = state->dev_resrc2.eth_txq;

		result = init_eth_rxq_ctx(cfg,
					  &state->dev_resrc1,
					  state->pe,
					  cfg->use_shared_mempool ? (void *)rx_shared_mempool_success_cb1 :
								    (void *)rx_success_cb1,
					  t_num * 2,
					  cfg->use_shared_mempool ? (void *)&state->dev_resrc2.rxq_cb_data :
								    (void *)state->dev_resrc2.eth_txq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize ETH RXQ context for device 1: %s",
				     doca_error_get_descr(result));
			goto destroy_txq2;
		}
	}

	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 2) {
		if (!cfg->use_shared_mempool) {
			result = create_mmap(state->dev_resrc1.mlxdev,
					     state->dev_resrc2.mlxdev,
					     &state->dev_resrc2.mmap_resrc);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create mmap for device 2: %s", doca_error_get_descr(result));
				goto destroy_rxq1;
			}
		}

		result = init_eth_txq_ctx(cfg,
					  &state->dev_resrc1,
					  state->pe,
					  cfg->use_shared_mempool ? tx_shared_mempool_success_cb1 : tx_success_cb1,
					  t_num * 2 + 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize ETH TXQ context for device 1: %s",
				     doca_error_get_descr(result));
			goto destroy_mmap2;
		}

		state->dev_resrc1.rxq_cb_data.eth_txq = state->dev_resrc1.eth_txq;

		result = init_eth_rxq_ctx(cfg,
					  &state->dev_resrc2,
					  state->pe,
					  cfg->use_shared_mempool ? (void *)rx_shared_mempool_success_cb2 :
								    (void *)rx_success_cb2,
					  t_num * 2 + 1,
					  cfg->use_shared_mempool ? (void *)&state->dev_resrc1.rxq_cb_data :
								    (void *)state->dev_resrc1.eth_txq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize ETH RXQ context for device 2: %s",
				     doca_error_get_descr(result));
			goto destroy_txq1;
		}
	}

	return DOCA_SUCCESS;
destroy_txq1:
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 2) {
		clean_result = doca_ctx_stop(state->dev_resrc1.eth_txq_ctx);
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop ETH TXQ for device 1: %s", doca_error_get_descr(clean_result));
			return result;
		}
		clean_result = doca_eth_txq_destroy(state->dev_resrc1.eth_txq);
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy ETH TXQ for device 1: %s", doca_error_get_descr(clean_result));
			return result;
		}
	}
destroy_mmap2:
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 2) {
		clean_result = destroy_mmap(&(state->dev_resrc2.mmap_resrc));
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap for device 2: %s", doca_error_get_descr(clean_result));
			return result;
		}
	}
destroy_rxq1:
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		clean_result = doca_ctx_stop(state->dev_resrc1.eth_rxq_ctx);
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop ETH RXQ for device 1: %s", doca_error_get_descr(clean_result));
			return result;
		}
		clean_result = doca_eth_rxq_destroy(state->dev_resrc1.eth_rxq);
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy ETH RXQ for device 1: %s", doca_error_get_descr(clean_result));
			return result;
		}
	}
destroy_txq2:
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		clean_result = doca_ctx_stop(state->dev_resrc2.eth_txq_ctx);
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop ETH TXQ for device 2: %s", doca_error_get_descr(clean_result));
			return result;
		}
		clean_result = doca_eth_txq_destroy(state->dev_resrc2.eth_txq);
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy ETH TXQ for device 2: %s", doca_error_get_descr(clean_result));
			return result;
		}
	}
destroy_mmap1:
	if (cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) {
		clean_result = destroy_mmap(&(state->dev_resrc1.mmap_resrc));
		if (clean_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap for device 1: %s", doca_error_get_descr(clean_result));
			return result;
		}
	}
destroy_pe:
	clean_result = doca_pe_destroy(state->pe);
	if (clean_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(clean_result));
		return result;
	}

	return result;
}

doca_error_t eth_l2_fwd_per_thread_cleanup(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state)
{
	doca_error_t result;
	enum doca_ctx_states ctx_state;
	uint8_t num_ctx_in_progress = 0;

	if (state->dev_resrc1.eth_rxq_ctx != NULL) {
		result = doca_ctx_stop(state->dev_resrc1.eth_rxq_ctx);
		if (result == DOCA_ERROR_IN_PROGRESS)
			++num_ctx_in_progress;
		else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop device 1 ETH RXQ DOCA CTX: %s", doca_error_get_descr(result));
			return result;
		} else /* DOCA_SUCCESS */
			state->dev_resrc1.eth_rxq_ctx = NULL;
	}

	if (state->dev_resrc2.eth_rxq_ctx != NULL) {
		result = doca_ctx_stop(state->dev_resrc2.eth_rxq_ctx);
		if (result == DOCA_ERROR_IN_PROGRESS)
			++num_ctx_in_progress;
		else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop device 2 ETH RXQ DOCA CTX: %s", doca_error_get_descr(result));
			return result;
		} else /* DOCA_SUCCESS */
			state->dev_resrc2.eth_rxq_ctx = NULL;
	}

	if (state->dev_resrc1.eth_txq_ctx != NULL) {
		result = doca_ctx_stop(state->dev_resrc1.eth_txq_ctx);
		if (result == DOCA_ERROR_IN_PROGRESS)
			++num_ctx_in_progress;
		else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop device 1 ETH TXQ DOCA CTX: %s", doca_error_get_descr(result));
			return result;
		} else /* DOCA_SUCCESS */
			state->dev_resrc1.eth_txq_ctx = NULL;
	}

	if (state->dev_resrc2.eth_txq_ctx != NULL) {
		result = doca_ctx_stop(state->dev_resrc2.eth_txq_ctx);
		if (result == DOCA_ERROR_IN_PROGRESS)
			++num_ctx_in_progress;
		else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop device 2 ETH TXQ DOCA CTX: %s", doca_error_get_descr(result));
			return result;
		} else /* DOCA_SUCCESS */
			state->dev_resrc2.eth_txq_ctx = NULL;
	}

	/* Draining till all contexts states are IDLE */
	while (num_ctx_in_progress > 0) {
		(void)doca_pe_progress(state->pe);

		if (state->dev_resrc1.eth_rxq_ctx != NULL) {
			result = doca_ctx_get_state(state->dev_resrc1.eth_rxq_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed get status of ETH RXQ DOCA CTX for device 1: %s",
					     doca_error_get_name(result));
				return result;
			}

			if (ctx_state == DOCA_CTX_STATE_IDLE) {
				state->dev_resrc1.eth_rxq_ctx = NULL;
				--num_ctx_in_progress;
			}
		}

		if (state->dev_resrc2.eth_rxq_ctx != NULL) {
			result = doca_ctx_get_state(state->dev_resrc2.eth_rxq_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed get status of ETH RXQ DOCA CTX for device 2: %s",
					     doca_error_get_name(result));
				return result;
			}

			if (ctx_state == DOCA_CTX_STATE_IDLE) {
				state->dev_resrc2.eth_rxq_ctx = NULL;
				--num_ctx_in_progress;
			}
		}

		if (state->dev_resrc1.eth_txq_ctx != NULL) {
			result = doca_ctx_get_state(state->dev_resrc1.eth_txq_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed get status of ETH TXQ DOCA CTX for device 1: %s",
					     doca_error_get_name(result));
				return result;
			}

			if (ctx_state == DOCA_CTX_STATE_IDLE) {
				state->dev_resrc1.eth_txq_ctx = NULL;
				--num_ctx_in_progress;
			}
		}

		if (state->dev_resrc2.eth_txq_ctx != NULL) {
			result = doca_ctx_get_state(state->dev_resrc2.eth_txq_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed get status of ETH TXQ DOCA CTX for device 2: %s",
					     doca_error_get_name(result));
				return result;
			}

			if (ctx_state == DOCA_CTX_STATE_IDLE) {
				state->dev_resrc2.eth_txq_ctx = NULL;
				--num_ctx_in_progress;
			}
		}
	}

	if (state->dev_resrc1.eth_txq != NULL) {
		result = doca_eth_txq_destroy(state->dev_resrc1.eth_txq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy device 1 ETH TXQ CTX: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (state->dev_resrc2.eth_txq != NULL) {
		result = doca_eth_txq_destroy(state->dev_resrc2.eth_txq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy device 2 ETH TXQ CTX: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (state->dev_resrc1.eth_rxq != NULL) {
		result = doca_eth_rxq_destroy(state->dev_resrc1.eth_rxq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy device 1 ETH RXQ CTX: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (state->dev_resrc2.eth_rxq != NULL) {
		result = doca_eth_rxq_destroy(state->dev_resrc2.eth_rxq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy device 2 ETH RXQ CTX: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if ((cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 1) && !cfg->use_shared_mempool) {
		result = destroy_mmap(&(state->dev_resrc1.mmap_resrc));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap for device 1: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if ((cfg->one_sided_fwd == 0 || cfg->one_sided_fwd == 2) && !cfg->use_shared_mempool) {
		result = destroy_mmap(&(state->dev_resrc2.mmap_resrc));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap for device 2: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (state->pe != NULL) {
		result = doca_pe_destroy(state->pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

void eth_l2_fwd_force_stop(void)
{
	force_app_stop = true;
}
