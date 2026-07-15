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

#ifndef DOCA_GPU_PACKET_PROCESSING_TCP_RSS_H
#define DOCA_GPU_PACKET_PROCESSING_TCP_RSS_H

#include <stdbool.h>
#include <arpa/inet.h>

#include <doca_flow.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_buf.h>
#include <doca_buf_pool.h>
#include <doca_mmap.h>

#include <common.h>

#define IPV4_HDR_DF_SHIFT 14
#define IPV4_HDR_DF_FLAG (1 << IPV4_HDR_DF_SHIFT)

enum tcp_opt {
	TCP_OPT_END = 0,
	TCP_OPT_NOP = 1,
	TCP_OPT_MSS = 2,
	TCP_OPT_WND_SCALE = 3,
	TCP_OPT_SACK_PERMITTED = 4,
	TCP_OPT_SACK = 5,
	TCP_OPT_TIMESTAMP = 8,
};
/*
 * Launch CPU thread to manage TCP 3way handshake
 *
 * @args [in]: thread input args
 */
void *tcp_cpu_rss_func(void *args);

/*
 * Create DOCA PE for a specific CPU RSS queue
 *
 * @args [in]: TCP queues structure
 * @args [in]: Queue ID to create PE for
 * @return: DOCA error
 */
doca_error_t create_pe_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id);

/*
 * Destroy DOCA PE for a specific CPU RSS queue
 *
 * @args [in]: TCP queues structure
 * @args [in]: Queue ID to destroy PE for
 * @return: DOCA error
 */
doca_error_t destroy_pe_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id);

/*
 * Create DOCA TXQ for a specific CPU RSS queue
 *
 * @args [in]: TCP queues structure
 * @args [in]: Queue ID to create TXQ for
 * @return: DOCA error
 */
doca_error_t create_txq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id);

/*
 * Destroy DOCA TXQ for a specific CPU RSS queue
 *
 * @args [in]: TCP queues structure
 * @args [in]: Queue ID to destroy TXQ for
 * @return: DOCA error
 */
doca_error_t destroy_txq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id);

/*
 * Create DOCA RXQ for a specific CPU RSS queue
 *
 * @args [in]: TCP queues structure
 * @args [in]: Queue ID to create RXQ for
 * @return: DOCA error
 */
doca_error_t create_rxq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id);

/*
 * Destroy DOCA RXQ for a specific CPU RSS queue
 *
 * @args [in]: TCP queues structure
 * @args [in]: Queue ID to destroy RXQ for
 * @return: DOCA error
 */
doca_error_t destroy_rxq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id);

#endif
