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

#ifndef FLOW_ETH_COMMON_H_
#define FLOW_ETH_COMMON_H_

#include <doca_eth_rxq_cpu_data_path.h>

#ifdef __cplusplus
extern "C" {
#endif

struct doca_dev;
struct doca_pe;
struct flow_eth_common_rxq;
struct flow_eth_common_dev_context;

struct flow_eth_common_rx_cfg {
	uint16_t nb_rxqs;	      /* Number of RX queues */
	bool enable_metadata;	      /* Enable metadata */
	union doca_data rx_user_data; /* User data to pass to the callback, default is set to queue index */
	doca_eth_rxq_event_batch_managed_recv_handler_cb_t rx_success_cb; /* Success callback */
	doca_eth_rxq_event_batch_managed_recv_handler_cb_t rx_error_cb;	  /* Error callback */
};

/*
 * Set DOCA ETH common device configuration.
 *
 * @nb_queues [in]: Number of RX queues
 * @enable_metadata [in]: Enable metadata
 * @rx_user_data [in]: User data
 * @rx_success_cb [in]: Success callback
 * @rx_error_cb [in]: Error callback
 * @cfg [out]: Configuration to set
 */
void flow_eth_common_set_dev_cfg(uint16_t nb_queues,
				 bool enable_metadata,
				 union doca_data rx_user_data,
				 doca_eth_rxq_event_batch_managed_recv_handler_cb_t rx_success_cb,
				 doca_eth_rxq_event_batch_managed_recv_handler_cb_t rx_error_cb,
				 struct flow_eth_common_rx_cfg *cfg);

/*
 * Create DOCA ETH device resources.
 *
 * @dev [in]: DOCA device
 * @pe [in]: Progress engine
 * @cfg [in]: Configuration parameters
 * @dev_ctx [out]: Allocated device context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_eth_common_create_dev_resources(struct doca_dev *dev,
						  struct doca_pe *pe,
						  struct flow_eth_common_rx_cfg *cfg,
						  struct flow_eth_common_dev_context **dev_ctx);

/*
 * Destroy DOCA ETH device resources.
 *
 * @dev_ctx [in]: Device context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_eth_common_destroy_dev_resources(struct flow_eth_common_dev_context *dev_ctx);

/*
 * Handle received traffic
 *
 * @pe [in]: Progress engine (shared across all ports)
 * @wait_secs [in]: Number of seconds to wait/process
 */
void flow_eth_common_handle_pkts(struct doca_pe *pe, uint32_t wait_secs);

#ifdef __cplusplus
}
#endif

#endif /* FLOW_ETH_COMMON_H_ */
