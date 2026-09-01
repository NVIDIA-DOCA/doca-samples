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

#ifndef ETH_L2_FWD_CORE_H_
#define ETH_L2_FWD_CORE_H_

#include <doca_dev.h>
#include <doca_eth_rxq.h>
#include <doca_mmap.h>

#include <samples/doca_eth/eth_flow_common.h>

#define ETH_L2_FWD_MAX_PKT_SIZE_DEFAULT 1600
#define ETH_L2_FWD_PKTS_RECV_RATE_DEFAULT 12500
#define ETH_L2_FWD_PKT_MAX_PROCESS_TIME_DEFAULT 1
#define ETH_L2_FWD_LOG_MAX_LRO_DEFAULT 15
#define ETH_L2_FWD_BATCH_SIZE 128
#define ETH_L2_FWD_TX_DEPTH_DEFAULT 4096
#define ETH_L2_FWD_RX_DEPTH_DEFAULT 2048
#define ETH_L2_FWD_MAX_NB_CORES 32
#define ETH_L2_FWD_NUM_CORES_DEFAULT 1
#define ETH_L2_FWD_TXQ_INLINE_MAX_DEFAULT 0 /* Disabled by default */
#define ETH_L2_FWD_TXQ_INLINE_MAX_LIMIT 512
#define ETH_L2_FWD_CORES_NUM_MIN_INLINE_DEFAULT 8 /* Min cores to enable inline by default */

/* Ethernet L2 Forwarding application configuration */
struct eth_l2_fwd_cfg {
	char mlxdev_name1[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* First IB device name */
	char mlxdev_name2[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* Second IB device name */
	uint32_t pkts_recv_rate; /* Packets receive rate for ETH RXQ memory buffer size estimation */
	uint32_t max_pkt_size;	 /* Max packet size for ETH RXQ context creation and memory buffer size estimation */
	uint16_t pkt_max_process_time;		 /* Packet max process time for ETH RXQ memory buffer size estimation */
	uint16_t number_cores;			 /* Number of cores used for forwarding */
	uint16_t batch_size;			 /* Batch size for tasks/events */
	uint16_t tx_depth;			 /*  Max burst size for DOCA ETH TXQ contexts creation */
	uint16_t rx_depth;			 /* Max burst size for DOCA ETH RXQ contexts creation */
	uint8_t one_sided_fwd;			 /* One-sided forwarding flag:
						  * 0 - two-sided forwarding
						  * 1 - device 1 -> device 2
						  * 2 - device 2 -> device 1
						  */
	uint8_t use_shared_mempool;		 /* Use shared mempool flag */
	enum doca_eth_rxq_cqe_format cqe_format; /* CQE format for the RXQ.
						  * DOCA_ETH_RXQ_CQE_FORMAT_REGULAR (default) disables CQE
						  * compression; any compressed variant enables it. */
	uint16_t txq_inline_max;		 /* Max packet size to inline into WQE (0 = disabled) */
	uint16_t cores_num_min_inline;		 /* Min number of cores to enable inline (0 = always enable) */
};

/* DOCA mmap resources */
struct mmap_resources {
	struct doca_mmap *mmap; /* DOCA mmap */
	uint8_t *mmap_buffer;	/* Memory buffer for mmap */
	uint32_t mmap_size;	/* Size of mmap's memory buffer */
};

/* data struct for shared mempool RXQ CB's */
struct rxq_cb_shared_mempool_data {
	struct doca_eth_rxq_shared_mempool *shared_mempool;
	struct doca_eth_txq *eth_txq;
};

/* Ethernet L2 Forwarding application single device resources */
struct eth_l2_fwd_dev_resources {
	struct doca_dev *mlxdev; /* DOCA device */

	struct doca_eth_rxq *eth_rxq; /* DOCA Ethernet RXQ context */
	struct doca_ctx *eth_rxq_ctx; /* DOCA Ethernet RXQ context as DOCA context */

	struct doca_eth_txq *eth_txq; /* DOCA Ethernet TXQ context */
	struct doca_ctx *eth_txq_ctx; /* DOCA Ethernet TXQ context as DOCA context */

	struct eth_flow_common_resources flow_resrc; /* Flow resources for mlxdev */

	struct mmap_resources mmap_resrc; /* Memory resources to set for the ETH RXQ context */

	struct rxq_cb_shared_mempool_data rxq_cb_data; /* Data struct for shared mempool RXQ CB's */

	uint16_t rxq_queue_id; /* Queue ID for the ETH RXQ context */
};

/* Ethernet L2 Forwarding application resources */
struct eth_l2_fwd_resources {
	struct doca_pe *pe; /* DOCA progress engine */

	struct eth_l2_fwd_dev_resources dev_resrc1; /* First IB device resources */
	struct eth_l2_fwd_dev_resources dev_resrc2; /* Second IB device resources */
	int thread_num;				    /* Running thread identifier */
};

/*
 * Initializes general application's logic/objects
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration
 * @state [in]: Ethernet L2 Forwarding application resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_general_init(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state);

/*
 * Initializes each application thread's logic/objects
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration
 * @state [in]: Ethernet L2 Forwarding application resources
 * @t_num [in]: Ethernet L2 Forwarding application thread identifier
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_per_thread_init(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state, uint16_t t_num);

/*
 * Initializes the application's flow logic/objects
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration
 * @state [in]: Ethernet L2 Forwarding application resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_flow_init(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state);

/*
 * Stops the application forcefully during execution
 */
void eth_l2_fwd_force_stop(void);

/*
 * Clean up the application's general resources
 *
 * @state [in]: Ethernet L2 Forwarding application resources to clean up
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_general_cleanup(struct eth_l2_fwd_resources *state);

/*
 * Clean up each application thread's resources
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration
 * @state [in]: Ethernet L2 Forwarding application resources to clean up
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_per_thread_cleanup(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state);

/*
 * Clean up the application's flow resources
 *
 * @cfg [in]: Ethernet L2 Forwarding application configuration
 * @state [in]: Ethernet L2 Forwarding application resources to clean up
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
doca_error_t eth_l2_fwd_flow_cleanup(struct eth_l2_fwd_cfg *cfg, struct eth_l2_fwd_resources *state);

/*
 * Signal handler
 *
 * @signum [in]: The signal received to handle
 */
void signal_handler(int signum);

/*
 * Start application's forwarding logic for each thread
 *
 * @args [in]: Ethernet L2 Forwarding application thread PE
 * @return: NULL
 */
void *start_forwarding(void *args);

/*
 * Reset the application's statistics
 */
void reset_stats(void);

/*
 * Print the application's statistics
 */
void print_stats(void);

#endif /* ETH_L2_FWD_CORE_H_ */
