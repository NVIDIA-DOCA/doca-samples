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

#ifndef ETH_L2_FWD_CORE_H_
#define ETH_L2_FWD_CORE_H_

#include <doca_dev.h>
#include <doca_mmap.h>

#include <samples/doca_eth/eth_rxq_common.h>

#define ETH_L2_FWD_MAX_PKT_SIZE_DEFAULT 1600
#define ETH_L2_FWD_PKTS_RECV_RATE_DEFAULT 12500
#define ETH_L2_FWD_PKT_MAX_PROCESS_TIME_DEFAULT 1
#define ETH_L2_FWD_LOG_MAX_LRO_DEFAULT 15
#define ETH_L2_FWD_NUM_TASK_BATCHES_DEFAULT 32
#define ETH_L2_FWD_NUM_TASKS_PER_BATCH 128

/* Ethernet L2 Forwarding application configuration */
struct eth_l2_fwd_cfg {
	char mlxdev_name1[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* First IB device name */
	char mlxdev_name2[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* Second IB device name */
	uint32_t max_burst_size; /* Max burst size for DOCA ETH contexts creation and memory buffer size estimation */
	uint32_t pkts_recv_rate; /* Packets receive rate for ETH RXQ memory buffer size estimation */
	uint32_t max_pkt_size;	 /* Max packet size for ETH RXQ context creation and memory buffer size estimation */
	uint16_t pkt_max_process_time; /* Packet max process time for ETH RXQ memory buffer size estimation */
	uint16_t num_task_batches;     /* Number of task batches */
	uint8_t one_sided_fwd;	       /* One-sided forwarding flag:
					* 0 - two-sided forwarding
					* 1 - device 1 -> device 2
					* 2 - device 2 -> device 1
					*/
};

/* DOCA mmap resources */
struct mmap_resources {
	struct doca_mmap *mmap; /* DOCA mmap */
	uint8_t *mmap_buffer;	/* Memory buffer for mmap */
	uint32_t mmap_size;	/* Size of mmap's memory buffer */
};

/* Ethernet L2 Forwarding application single device resources */
struct eth_l2_fwd_dev_resources {
	struct doca_dev *mlxdev; /* DOCA device */

	struct doca_eth_rxq *eth_rxq; /* DOCA Ethernet RXQ context */
	struct doca_ctx *eth_rxq_ctx; /* DOCA Ethernet RXQ context as DOCA context */

	struct doca_eth_txq *eth_txq; /* DOCA Ethernet TXQ context */
	struct doca_ctx *eth_txq_ctx; /* DOCA Ethernet TXQ context as DOCA context */

	struct eth_rxq_flow_resources flow_resrc; /* DOCA flow resources for mlxdev */

	struct mmap_resources mmap_resrc; /* Memory resources to set for the ETH RXQ context */

	uint16_t rxq_queue_id; /* Queue ID for the ETH RXQ context */
};

/* Ethernet L2 Forwarding application resources */
struct eth_l2_fwd_resources {
	struct doca_pe *pe; /* DOCA progress engine */

	struct eth_l2_fwd_dev_resources dev_resrc1; /* First IB device resources */
	struct eth_l2_fwd_dev_resources dev_resrc2; /* Second IB device resources */
};

/*
 * Executes the application's logic
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration
 * @state [in]: Ethernet L2 Forwarding application resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_execute(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state);

/*
 * Stops the application forcefully during execution
 */
void eth_l2_fwd_force_stop(void);

/*
 * Clean up the application's resources
 *
 * @state [in]: Ethernet L2 Forwarding application resources to clean up
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_cleanup(struct eth_l2_fwd_resources *state);

#endif /* ETH_L2_FWD_CORE_H_ */
