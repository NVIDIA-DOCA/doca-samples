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

#ifndef ETH_FLOW_COMMON_H_
#define ETH_FLOW_COMMON_H_

#include <unistd.h>

#include <doca_flow.h>
#include <doca_dev.h>
#include <doca_error.h>

struct eth_flow_common_resources {
	struct doca_flow_port *df_port;		 /* DOCA flow port */
	struct doca_flow_pipe *root_pipe;	 /* DOCA flow root pipe*/
	struct doca_flow_pipe_entry *root_entry; /* DOCA flow root pipe entry*/
};

struct eth_flow_common_config {
	struct doca_dev *dev;	 /* DOCA device */
	uint16_t *rxq_queue_ids; /* DOCA ETH RXQ's queue IDs array */
	uint16_t nb_queues;	 /* Number of queues IDs in the array */
};

/*
 * Initialize flow for ETH sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t eth_flow_common_init_flow(void);

/*
 * Cleanup flow for ETH sample
 *
 */
void eth_flow_common_cleanup_flow(void);

/*
 * Initialize Flow port for ETH sample
 *
 * @dev [in]: Doca device to use for doca_flow_port
 * @port_id [in]: doca_flow_port id to assign
 * @resources [out]: flow resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t eth_flow_common_create_flow_port(struct doca_dev *dev,
					      uint16_t port_id,
					      struct eth_flow_common_resources *resources);

/*
 * Clean up Flow port for ETH sample
 *
 * @resources [in]: flow resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t eth_flow_common_destroy_flow_port(struct eth_flow_common_resources *resources);

/*
 * Create flow pipe for ETH sample
 *
 * @cfg [in]: Configuration parameters
 * @resources [out]: flow resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t eth_flow_common_create_flow_pipe(struct eth_flow_common_config *cfg,
					      struct eth_flow_common_resources *resources);

/*
 * Destroy flow pipe for ETH sample
 *
 * @resources [in]: flow resources
 */
void eth_flow_common_destroy_flow_pipe(struct eth_flow_common_resources *resources);

#endif /* ETH_FLOW_COMMON_H_ */
