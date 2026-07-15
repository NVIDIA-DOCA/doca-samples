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

#include <stdlib.h>
#include <string.h>

#include "common.h"

/* Since DPDK and ETH queues should have different IDs, we define a range for DPDK queues */
#define MAX_DPDK_QUEUE_NUM 128

DOCA_LOG_REGISTER(GPU_PACKET_PROCESSING_UDP);

/*
 * Retrieve host page size
 *
 * @return: host page size
 */
static size_t get_host_page_size(void)
{
	long ret = sysconf(_SC_PAGESIZE);
	if (ret == -1)
		return 4096; // 4KB, default Linux page size
	return (size_t)ret;
}

doca_error_t create_icmp_queues(struct rxq_icmp_queues *icmp_queues,
				struct doca_flow_port *df_port,
				struct doca_gpu *gpu_dev,
				struct doca_dev *ddev,
				uint32_t queue_num,
				struct doca_pe *pe,
				doca_eth_txq_gpu_event_error_send_packet_cb_t event_error_send_packet_cb,
				doca_eth_txq_gpu_event_notify_send_packet_cb_t event_notify_send_packet_cb)
{
	uint32_t cyclic_buffer_size = 0;
	doca_error_t result;
	union doca_data event_user_data[MAX_QUEUES_ICMP] = {0};

	if (icmp_queues == NULL || df_port == NULL || gpu_dev == NULL || ddev == NULL || queue_num == 0) {
		DOCA_LOG_ERR("Can't create ICMP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	icmp_queues->gpu_dev = gpu_dev;
	icmp_queues->ddev = ddev;
	icmp_queues->port = df_port;
	icmp_queues->numq = queue_num;

	for (uint32_t idx = 0; idx < queue_num; idx++) {
		DOCA_LOG_INFO("Creating ICMP Eth Rxq %d", idx);

		result = doca_eth_rxq_create(icmp_queues->ddev,
					     MAX_PKT_NUM_ICMP,
					     MAX_PKT_SIZE_ICMP,
					     &(icmp_queues->eth_rxq_cpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_create: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_set_type(icmp_queues->eth_rxq_cpu[idx], DOCA_ETH_RXQ_TYPE_CYCLIC);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_set_type: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_CYCLIC,
							       0,
							       0,
							       MAX_PKT_SIZE_ICMP,
							       MAX_PKT_NUM_ICMP,
							       0,
							       0,
							       0,
							       &cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get eth_rxq cyclic buffer size: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_create(&icmp_queues->pkt_buff_mmap[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_add_dev(icmp_queues->pkt_buff_mmap[idx], icmp_queues->ddev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add dev to mmap: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		ALIGN_SIZE(cyclic_buffer_size, get_host_page_size());
		result = doca_gpu_mem_alloc(icmp_queues->gpu_dev,
					    cyclic_buffer_size,
					    get_host_page_size(),
					    DOCA_GPU_MEM_TYPE_GPU,
					    &icmp_queues->gpu_pkt_addr[idx],
					    NULL);
		if (result != DOCA_SUCCESS || icmp_queues->gpu_pkt_addr[idx] == NULL) {
			DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		/* Map GPU memory buffer used to receive packets with DMABuf */
		result = doca_gpu_dmabuf_fd(icmp_queues->gpu_dev,
					    icmp_queues->gpu_pkt_addr[idx],
					    cyclic_buffer_size,
					    &(icmp_queues->dmabuf_fd[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB) with nvidia-peermem mode",
				      icmp_queues->gpu_pkt_addr[idx],
				      cyclic_buffer_size);

			/* If failed, use nvidia-peermem method */
			result = doca_mmap_set_memrange(icmp_queues->pkt_buff_mmap[idx],
							icmp_queues->gpu_pkt_addr[idx],
							cyclic_buffer_size);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
				destroy_icmp_queues(icmp_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		} else {
			DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB dmabuf fd %d) with dmabuf mode",
				      icmp_queues->gpu_pkt_addr[idx],
				      cyclic_buffer_size,
				      icmp_queues->dmabuf_fd[idx]);

			result = doca_mmap_set_dmabuf_memrange(icmp_queues->pkt_buff_mmap[idx],
							       icmp_queues->dmabuf_fd[idx],
							       icmp_queues->gpu_pkt_addr[idx],
							       0,
							       cyclic_buffer_size);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set dmabuf memrange for mmap %s", doca_error_get_descr(result));
				destroy_icmp_queues(icmp_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		}

		result = doca_mmap_set_permissions(icmp_queues->pkt_buff_mmap[idx],
						   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
							   DOCA_ACCESS_FLAG_PCI_RELAXED_ORDERING);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set permissions for mmap %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_start(icmp_queues->pkt_buff_mmap[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start mmap %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_set_pkt_buf(icmp_queues->eth_rxq_cpu[idx],
						  icmp_queues->pkt_buff_mmap[idx],
						  0,
						  cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set cyclic buffer  %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		icmp_queues->eth_rxq_ctx[idx] = doca_eth_rxq_as_doca_ctx(icmp_queues->eth_rxq_cpu[idx]);
		if (icmp_queues->eth_rxq_ctx[idx] == NULL) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_as_doca_ctx: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_ctx_set_datapath_on_gpu(icmp_queues->eth_rxq_ctx[idx], icmp_queues->gpu_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_ctx_start(icmp_queues->eth_rxq_ctx[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_get_gpu_handle(icmp_queues->eth_rxq_cpu[idx], &(icmp_queues->eth_rxq_gpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_get_gpu_handle: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_txq_create(icmp_queues->ddev, MAX_SQ_DESCR_NUM, &(icmp_queues->eth_txq_cpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_create: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_txq_set_l3_chksum_offload(icmp_queues->eth_txq_cpu[idx], 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		icmp_queues->eth_txq_ctx[idx] = doca_eth_txq_as_doca_ctx(icmp_queues->eth_txq_cpu[idx]);
		if (icmp_queues->eth_txq_ctx[idx] == NULL) {
			DOCA_LOG_ERR("Failed doca_eth_txq_as_doca_ctx: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_ctx_set_datapath_on_gpu(icmp_queues->eth_txq_ctx[idx], icmp_queues->gpu_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		if (pe != NULL) {
			event_user_data[idx].u64 = idx;
			result = doca_eth_txq_gpu_event_error_send_packet_register(icmp_queues->eth_txq_cpu[idx],
										   event_error_send_packet_cb,
										   event_user_data[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to set DOCA progress engine callback: %s",
					     doca_error_get_descr(result));
				destroy_icmp_queues(icmp_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_eth_txq_gpu_event_notify_send_packet_register(icmp_queues->eth_txq_cpu[idx],
										    event_notify_send_packet_cb,
										    event_user_data[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to set DOCA progress engine callback: %s",
					     doca_error_get_descr(result));
				destroy_icmp_queues(icmp_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_pe_connect_ctx(pe, icmp_queues->eth_txq_ctx[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to set DOCA progress engine to DOCA Eth Txq: %s",
					     doca_error_get_descr(result));
				destroy_icmp_queues(icmp_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		}

		result = doca_ctx_start(icmp_queues->eth_txq_ctx[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_txq_apply_queue_id(icmp_queues->eth_txq_cpu[idx], QUEUE_ID_ICMP_0 + idx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_apply_queue_id: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_txq_get_gpu_handle(icmp_queues->eth_txq_cpu[idx], &(icmp_queues->eth_txq_gpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_get_gpu_handle: %s", doca_error_get_descr(result));
			destroy_icmp_queues(icmp_queues);
			return DOCA_ERROR_BAD_STATE;
		}
	}

	/* Create UDP based flow pipe */
	result = create_icmp_gpu_pipe(icmp_queues, df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function build_rxq_pipe returned %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

doca_error_t destroy_icmp_queues(struct rxq_icmp_queues *icmp_queues)
{
	doca_error_t result;

	if (icmp_queues == NULL) {
		DOCA_LOG_ERR("Can't destroy ICMP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (int idx = 0; idx < icmp_queues->numq; idx++) {
		DOCA_LOG_INFO("Destroying ICMP queue %d", idx);

		result = doca_ctx_stop(icmp_queues->eth_rxq_ctx[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_destroy(icmp_queues->eth_rxq_cpu[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_destroy(icmp_queues->pkt_buff_mmap[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_gpu_mem_free(icmp_queues->gpu_dev, icmp_queues->gpu_pkt_addr[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_ctx_stop(icmp_queues->eth_txq_ctx[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_txq_destroy(icmp_queues->eth_txq_cpu[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	return DOCA_SUCCESS;
}
