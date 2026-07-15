/*
 * Copyright (c) 2021 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_net.h>

#include <doca_log.h>
#include <doca_flow.h>

#include "simple_fwd_port.h"

DOCA_LOG_REGISTER(SIMPLE_FWD_PORT);

#define NS_PER_SEC 1E9	   /* Nano-seconds per second */
#ifdef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */

/* Defining the CLOCK ID type used for displaying stats of a port */
#define CLOCK_TYPE_ID CLOCK_MONOTONIC_RAW
#else
#define CLOCK_TYPE_ID CLOCK_MONOTONIC
#endif

/*
 * Dump port stats
 *
 * @port [in]: Port identifier of the port to dump the stats for
 * @f [in]: file to dump or buffering the stats into
 * @return: 0 on success and non-zero value on failure
 */
static int simple_fwd_port_stats_display(uint16_t port, FILE *f)
{
	uint32_t i;
	int result;
	static uint64_t prev_pkts_rx[RTE_MAX_ETHPORTS];
	static uint64_t prev_pkts_tx[RTE_MAX_ETHPORTS];
	static uint64_t prev_bytes_rx[RTE_MAX_ETHPORTS];
	static uint64_t prev_bytes_tx[RTE_MAX_ETHPORTS];
	static uint64_t prev_ns[RTE_MAX_ETHPORTS];
	struct timespec cur_time;
	uint64_t diff_pkts_rx, diff_pkts_tx, diff_bytes_rx, diff_bytes_tx, diff_ns;
	uint64_t mpps_rx, mpps_tx, mbps_rx, mbps_tx;
	struct rte_eth_stats ethernet_stats;
	struct rte_eth_dev_info dev_info;
	static const char *nic_stats_border = "########################";
	uint32_t max_rx_queues, max_tx_queues;

	result = rte_eth_stats_get(port, &ethernet_stats);
	if (result != 0)
		return result;
	result = rte_eth_dev_info_get(port, &dev_info);
	if (result != 0)
		return result;

	max_rx_queues = dev_info.nb_rx_queues < RTE_ETHDEV_QUEUE_STAT_CNTRS ? dev_info.nb_rx_queues :
									      RTE_ETHDEV_QUEUE_STAT_CNTRS;
	max_tx_queues = dev_info.nb_tx_queues < RTE_ETHDEV_QUEUE_STAT_CNTRS ? dev_info.nb_tx_queues :
									      RTE_ETHDEV_QUEUE_STAT_CNTRS;
	fprintf(f, "\n  %s NIC statistics for port %-2d %s\n", nic_stats_border, port, nic_stats_border);

	fprintf(f,
		"  RX-packets: %-10" PRIu64 " RX-missed: %-10" PRIu64 " RX-bytes:  %-" PRIu64 "\n",
		ethernet_stats.ipackets,
		ethernet_stats.imissed,
		ethernet_stats.ibytes);
	fprintf(f, "  RX-errors: %-" PRIu64 "\n", ethernet_stats.ierrors);
	fprintf(f, "  RX-nombuf:  %-10" PRIu64 "\n", ethernet_stats.rx_nombuf);
	fprintf(f,
		"  TX-packets: %-10" PRIu64 " TX-errors: %-10" PRIu64 " TX-bytes:  %-" PRIu64 "\n",
		ethernet_stats.opackets,
		ethernet_stats.oerrors,
		ethernet_stats.obytes);

	fprintf(f, "\n");

	for (i = 0; i < max_rx_queues; i++) {
		printf("  ethernet_stats reg %2d RX-packets: %-10" PRIu64 "  RX-errors: %-10" PRIu64
		       "  RX-bytes: %-10" PRIu64 "\n",
		       i,
		       ethernet_stats.q_ipackets[i],
		       ethernet_stats.q_errors[i],
		       ethernet_stats.q_ibytes[i]);
	}

	fprintf(f, "\n");
	for (i = 0; i < max_tx_queues; i++) {
		fprintf(stdout,
			"  ethernet_stats reg %2d TX-packets: %-10" PRIu64 "  TX-bytes: %-10" PRIu64 "\n",
			i,
			ethernet_stats.q_opackets[i],
			ethernet_stats.q_obytes[i]);
	}

	diff_ns = 0;
	if (clock_gettime(CLOCK_TYPE_ID, &cur_time) == 0) {
		uint64_t ns;

		ns = cur_time.tv_sec * NS_PER_SEC;
		ns += cur_time.tv_nsec;

		if (prev_ns[port] != 0)
			diff_ns = ns - prev_ns[port];
		prev_ns[port] = ns;
	}

	diff_pkts_rx = (ethernet_stats.ipackets > prev_pkts_rx[port]) ? (ethernet_stats.ipackets - prev_pkts_rx[port]) :
									0;
	diff_pkts_tx = (ethernet_stats.opackets > prev_pkts_tx[port]) ? (ethernet_stats.opackets - prev_pkts_tx[port]) :
									0;
	prev_pkts_rx[port] = ethernet_stats.ipackets;
	prev_pkts_tx[port] = ethernet_stats.opackets;
	mpps_rx = diff_ns > 0 ? (double)diff_pkts_rx / diff_ns * NS_PER_SEC : 0;
	mpps_tx = diff_ns > 0 ? (double)diff_pkts_tx / diff_ns * NS_PER_SEC : 0;

	diff_bytes_rx = (ethernet_stats.ibytes > prev_bytes_rx[port]) ? (ethernet_stats.ibytes - prev_bytes_rx[port]) :
									0;
	diff_bytes_tx = (ethernet_stats.obytes > prev_bytes_tx[port]) ? (ethernet_stats.obytes - prev_bytes_tx[port]) :
									0;
	prev_bytes_rx[port] = ethernet_stats.ibytes;
	prev_bytes_tx[port] = ethernet_stats.obytes;
	mbps_rx = diff_ns > 0 ? (double)diff_bytes_rx / diff_ns * NS_PER_SEC : 0;
	mbps_tx = diff_ns > 0 ? (double)diff_bytes_tx / diff_ns * NS_PER_SEC : 0;

	fprintf(f, "\n  Throughput (since last show)\n");
	fprintf(f,
		"  Rx-pps: %12" PRIu64 "          Rx-bps: %12" PRIu64 "\n  Tx-pps: %12" PRIu64
		"          Tx-bps: %12" PRIu64 "\n",
		mpps_rx,
		mbps_rx * 8,
		mpps_tx,
		mbps_tx * 8);

	fprintf(f, "  %s############################%s\n", nic_stats_border, nic_stats_border);
	return 0;
}

int simple_fwd_dump_port_stats(uint16_t port_id)
{
	int result = 0;
	const char clr[] = {27, '[', '2', 'J', '\0'};
	const char topLeft[] = {27, '[', '1', ';', '1', 'H', '\0'};

	fprintf(stdout, "%s%s", clr, topLeft);

	result = simple_fwd_port_stats_display(port_id, stdout);
	fflush(stdout);
	return result;
}
