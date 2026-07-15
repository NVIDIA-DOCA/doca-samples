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
#include "tcp_cpu/tcp_session_table.h"
#include "tcp_cpu/tcp_cpu_rss_func.h"

/* Since DPDK and ETH queues should have different IDs, we define a range for DPDK queues */
#define MAX_DPDK_QUEUE_NUM 128

DOCA_LOG_REGISTER(GPU_PACKET_PROCESSING_TCP);

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

doca_error_t create_tcp_queues(struct rxq_tcp_queues *tcp_queues,
			       struct doca_flow_port *df_port,
			       struct doca_gpu *gpu_dev,
			       struct doca_dev *ddev,
			       uint32_t queue_num,
			       uint32_t sem_num,
			       bool http_server,
			       struct txq_http_queues *http_queues,
			       struct doca_pe *pe,
			       doca_eth_txq_gpu_event_error_send_packet_cb_t event_error_send_packet_cb)
{
	doca_error_t result;
	uint32_t cyclic_buffer_size = 0;
	union doca_data event_user_data[MAX_QUEUES] = {0};

	if (tcp_queues == NULL || df_port == NULL || gpu_dev == NULL || ddev == NULL || queue_num == 0 ||
	    sem_num == 0 || (http_server && http_queues == NULL)) {
		DOCA_LOG_ERR("Can't create TCP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	tcp_queues->ddev = ddev;
	tcp_queues->gpu_dev = gpu_dev;
	tcp_queues->port = df_port;
	tcp_queues->numq = queue_num;
	tcp_queues->numq_cpu_rss = queue_num;
	tcp_queues->nums = sem_num;

	for (uint32_t idx = 0; idx < queue_num; idx++) {
		DOCA_LOG_INFO("Creating TCP Eth Rxq %d", idx);

		result = doca_eth_rxq_create(tcp_queues->ddev,
					     MAX_PKT_NUM,
					     MAX_PKT_SIZE,
					     &(tcp_queues->eth_rxq_cpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_create: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_set_type(tcp_queues->eth_rxq_cpu[idx], DOCA_ETH_RXQ_TYPE_CYCLIC);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_set_type: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_CYCLIC,
							       0,
							       0,
							       MAX_PKT_SIZE,
							       MAX_PKT_NUM,
							       0,
							       0,
							       0,
							       &cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get eth_rxq cyclic buffer size: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_create(&tcp_queues->pkt_buff_mmap[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_add_dev(tcp_queues->pkt_buff_mmap[idx], tcp_queues->ddev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add dev to mmap: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		ALIGN_SIZE(cyclic_buffer_size, get_host_page_size());

		result = doca_gpu_mem_alloc(tcp_queues->gpu_dev,
					    cyclic_buffer_size,
					    get_host_page_size(),
					    DOCA_GPU_MEM_TYPE_GPU,
					    &tcp_queues->gpu_pkt_addr[idx],
					    NULL);
		if (result != DOCA_SUCCESS || tcp_queues->gpu_pkt_addr[idx] == NULL) {
			DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		/* Map GPU memory buffer used to receive packets with DMABuf */
		result = doca_gpu_dmabuf_fd(tcp_queues->gpu_dev,
					    tcp_queues->gpu_pkt_addr[idx],
					    cyclic_buffer_size,
					    &(tcp_queues->dmabuf_fd[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB) with nvidia-peermem mode",
				      tcp_queues->gpu_pkt_addr[idx],
				      cyclic_buffer_size);

			/* If failed, use nvidia-peermem method */
			result = doca_mmap_set_memrange(tcp_queues->pkt_buff_mmap[idx],
							tcp_queues->gpu_pkt_addr[idx],
							cyclic_buffer_size);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		} else {
			DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB dmabuf fd %d) with dmabuf mode",
				      tcp_queues->gpu_pkt_addr[idx],
				      cyclic_buffer_size,
				      tcp_queues->dmabuf_fd[idx]);

			result = doca_mmap_set_dmabuf_memrange(tcp_queues->pkt_buff_mmap[idx],
							       tcp_queues->dmabuf_fd[idx],
							       tcp_queues->gpu_pkt_addr[idx],
							       0,
							       cyclic_buffer_size);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set dmabuf memrange for mmap %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		}

		result = doca_mmap_set_permissions(tcp_queues->pkt_buff_mmap[idx],
						   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
							   DOCA_ACCESS_FLAG_PCI_RELAXED_ORDERING);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set permissions for mmap %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_mmap_start(tcp_queues->pkt_buff_mmap[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start mmap %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_set_pkt_buf(tcp_queues->eth_rxq_cpu[idx],
						  tcp_queues->pkt_buff_mmap[idx],
						  0,
						  cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set cyclic buffer  %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		tcp_queues->eth_rxq_ctx[idx] = doca_eth_rxq_as_doca_ctx(tcp_queues->eth_rxq_cpu[idx]);
		if (tcp_queues->eth_rxq_ctx[idx] == NULL) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_as_doca_ctx: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_ctx_set_datapath_on_gpu(tcp_queues->eth_rxq_ctx[idx], tcp_queues->gpu_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_ctx_start(tcp_queues->eth_rxq_ctx[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_eth_rxq_get_gpu_handle(tcp_queues->eth_rxq_cpu[idx], &(tcp_queues->eth_rxq_gpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_get_gpu_handle: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_gpu_semaphore_create(tcp_queues->gpu_dev, &(tcp_queues->sem_cpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_gpu_semaphore_create: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		/*
		 * Semaphore memory reside on CPU visible from GPU.
		 * CPU will poll in busy wait on this semaphore (multiple reads)
		 * while GPU access each item only once to update values.
		 */
		result = doca_gpu_semaphore_set_memory_type(tcp_queues->sem_cpu[idx], DOCA_GPU_MEM_TYPE_CPU_GPU);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_gpu_semaphore_set_memory_type: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_gpu_semaphore_set_items_num(tcp_queues->sem_cpu[idx], tcp_queues->nums);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_gpu_semaphore_set_items_num: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		/*
		 * Semaphore memory reside on CPU visible from GPU.
		 * The CPU reads packets info from this structure.
		 * The GPU access each item only once to update values.
		 */
		result = doca_gpu_semaphore_set_custom_info(tcp_queues->sem_cpu[idx],
							    sizeof(struct stats_tcp),
							    DOCA_GPU_MEM_TYPE_CPU_GPU);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_gpu_semaphore_set_custom_info: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_gpu_semaphore_start(tcp_queues->sem_cpu[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_gpu_semaphore_start: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = doca_gpu_semaphore_get_gpu_handle(tcp_queues->sem_cpu[idx], &(tcp_queues->sem_gpu[idx]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_gpu_semaphore_get_gpu_handle: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		if (http_server) {
			http_queues->gpu_dev = gpu_dev;
			http_queues->ddev = ddev;

			result = doca_gpu_semaphore_create(tcp_queues->gpu_dev, &(tcp_queues->sem_http_cpu[idx]));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_create: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			/*
			 * Semaphore memory reside on CPU visible from GPU.
			 * CPU will poll in busy wait on this semaphore (multiple reads)
			 * while GPU access each item only once to update values.
			 */
			result = doca_gpu_semaphore_set_memory_type(tcp_queues->sem_http_cpu[idx],
								    DOCA_GPU_MEM_TYPE_GPU);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_set_memory_type: %s",
					     doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_gpu_semaphore_set_items_num(tcp_queues->sem_http_cpu[idx], tcp_queues->nums);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_set_items_num: %s",
					     doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			/*
			 * Semaphore memory reside on GPU, not visible from CPU.
			 * This semaphore is needed only across CUDA kernels to exchange HTTP GET info.
			 */
			result = doca_gpu_semaphore_set_custom_info(tcp_queues->sem_http_cpu[idx],
								    sizeof(struct info_http),
								    DOCA_GPU_MEM_TYPE_GPU);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_set_custom_info: %s",
					     doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_gpu_semaphore_start(tcp_queues->sem_http_cpu[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_start: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_gpu_semaphore_get_gpu_handle(tcp_queues->sem_http_cpu[idx],
								   &(tcp_queues->sem_http_gpu[idx]));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_get_gpu_handle: %s",
					     doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_eth_txq_create(http_queues->ddev,
						     MAX_SQ_DESCR_NUM,
						     &(http_queues->eth_txq_cpu[idx]));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_eth_txq_create: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_eth_txq_set_l3_chksum_offload(http_queues->eth_txq_cpu[idx], 1);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_eth_txq_set_l4_chksum_offload(http_queues->eth_txq_cpu[idx], 1);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			http_queues->eth_txq_ctx[idx] = doca_eth_txq_as_doca_ctx(http_queues->eth_txq_cpu[idx]);
			if (http_queues->eth_txq_ctx[idx] == NULL) {
				DOCA_LOG_ERR("Failed doca_eth_txq_as_doca_ctx: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_ctx_set_datapath_on_gpu(http_queues->eth_txq_ctx[idx], http_queues->gpu_dev);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			if (pe != NULL) {
				event_user_data[idx].u64 = idx;
				result =
					doca_eth_txq_gpu_event_error_send_packet_register(http_queues->eth_txq_cpu[idx],
											  event_error_send_packet_cb,
											  event_user_data[idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Unable to set DOCA progress engine callback: %s",
						     doca_error_get_descr(result));
					destroy_tcp_queues(tcp_queues, http_server, http_queues);
					return DOCA_ERROR_BAD_STATE;
				}

				result = doca_pe_connect_ctx(pe, http_queues->eth_txq_ctx[idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Unable to set DOCA progress engine to DOCA Eth Txq: %s",
						     doca_error_get_descr(result));
					destroy_tcp_queues(tcp_queues, http_server, http_queues);
					return DOCA_ERROR_BAD_STATE;
				}
			}

			result = doca_ctx_start(http_queues->eth_txq_ctx[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_eth_txq_apply_queue_id(http_queues->eth_txq_cpu[idx], QUEUE_ID_HTTP_0 + idx);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_eth_txq_apply_queue_id: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_eth_txq_get_gpu_handle(http_queues->eth_txq_cpu[idx],
							     &(http_queues->eth_txq_gpu[idx]));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_eth_txq_get_gpu_handle: %s", doca_error_get_descr(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		}
	}

	if (http_server) {
		/* Prepare packets for HTTP response to GET INDEX */
		result = create_tx_buf(&http_queues->buf_page_index,
				       http_queues->gpu_dev,
				       http_queues->ddev,
				       TX_BUF_NUM,
				       TX_BUF_MAX_SZ);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create buf_page_index: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = prepare_tx_buf(&http_queues->buf_page_index, HTTP_GET_INDEX);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed prepare buf_page_index: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		/* Prepare packets for HTTP response to GET CONTACTS */
		result = create_tx_buf(&http_queues->buf_page_contacts,
				       http_queues->gpu_dev,
				       http_queues->ddev,
				       TX_BUF_NUM,
				       TX_BUF_MAX_SZ);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create buf_page_contacts: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = prepare_tx_buf(&http_queues->buf_page_contacts, HTTP_GET_CONTACTS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed prepare buf_page_contacts: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		/* Prepare packets for HTTP response to any other GET request */
		result = create_tx_buf(&http_queues->buf_page_not_found,
				       http_queues->gpu_dev,
				       http_queues->ddev,
				       TX_BUF_NUM,
				       TX_BUF_MAX_SZ);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create buf_page_not_found: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		result = prepare_tx_buf(&http_queues->buf_page_not_found, HTTP_GET_NOT_FOUND);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed prepare buf_page_not_found: %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}

		for (int i = 0; i < tcp_queues->numq_cpu_rss; i++) {
			result = create_pe_cpu_rss(tcp_queues, i);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create PE CPU RSS on queue %u (err=%s)",
					     i,
					     doca_error_get_name(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = create_txq_cpu_rss(tcp_queues, i);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create TXQ CPU RSS on queue %u (err=%s)",
					     i,
					     doca_error_get_name(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}

			result = create_rxq_cpu_rss(tcp_queues, i);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create RXQ CPU RSS on queue %u (err=%s)",
					     i,
					     doca_error_get_name(result));
				destroy_tcp_queues(tcp_queues, http_server, http_queues);
				return DOCA_ERROR_BAD_STATE;
			}
		}

		/* Create TCP based flow pipes */
		result = create_tcp_cpu_pipe(tcp_queues, df_port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function create_tcp_cpu_pipe returned %s", doca_error_get_descr(result));
			destroy_tcp_queues(tcp_queues, http_server, http_queues);
			return DOCA_ERROR_BAD_STATE;
		}
	}

	/* Create UDP based flow pipe */
	result = create_tcp_gpu_pipe(tcp_queues, df_port, http_server);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_tcp_gpu_pipe returned %s", doca_error_get_descr(result));
		destroy_tcp_queues(tcp_queues, http_server, http_queues);
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

doca_error_t destroy_tcp_queues(struct rxq_tcp_queues *tcp_queues,
				bool http_server,
				struct txq_http_queues *http_queues)
{
	doca_error_t result;

	if (tcp_queues == NULL || (http_server && http_queues == NULL)) {
		DOCA_LOG_ERR("Can't destroy TCP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (int idx = 0; idx < tcp_queues->numq; idx++) {
		DOCA_LOG_INFO("Destroying TCP queue %d", idx);

		if (tcp_queues->sem_cpu[idx]) {
			result = doca_gpu_semaphore_stop(tcp_queues->sem_cpu[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_start: %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_gpu_semaphore_destroy(tcp_queues->sem_cpu[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_gpu_semaphore_destroy: %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}
		}

		if (tcp_queues->eth_rxq_ctx[idx]) {
			result = doca_ctx_stop(tcp_queues->eth_rxq_ctx[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}
		}

		if (tcp_queues->pkt_buff_mmap[idx]) {
			result = doca_mmap_stop(tcp_queues->pkt_buff_mmap[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to start mmap %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}

			result = doca_mmap_destroy(tcp_queues->pkt_buff_mmap[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}
		}

		if (tcp_queues->gpu_pkt_addr[idx]) {
			result = doca_gpu_mem_free(tcp_queues->gpu_dev, tcp_queues->gpu_pkt_addr[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}
		}

		if (tcp_queues->eth_rxq_cpu[idx]) {
			result = doca_eth_rxq_destroy(tcp_queues->eth_rxq_cpu[idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
				return DOCA_ERROR_BAD_STATE;
			}
		}

		if (http_server) {
			DOCA_LOG_INFO("Destroying HTTP queue %d", idx);

			result = destroy_rxq_cpu_rss(tcp_queues, idx);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy RXQ CPU RSS on queue %u (err=%s)",
					     idx,
					     doca_error_get_name(result));
				return DOCA_ERROR_BAD_STATE;
			}

			result = destroy_txq_cpu_rss(tcp_queues, idx);
			destroy_pe_cpu_rss(tcp_queues, idx);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy TXQ CPU RSS on queue %u (err=%s)",
					     idx,
					     doca_error_get_name(result));
				return DOCA_ERROR_BAD_STATE;
			}

			result = destroy_pe_cpu_rss(tcp_queues, idx);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy PE CPU RSS on queue %u (err=%s)",
					     idx,
					     doca_error_get_name(result));
				return DOCA_ERROR_BAD_STATE;
			}

			if (tcp_queues->sem_http_cpu[idx]) {
				result = doca_gpu_semaphore_stop(tcp_queues->sem_http_cpu[idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed doca_gpu_semaphore_start: %s",
						     doca_error_get_descr(result));
					return DOCA_ERROR_BAD_STATE;
				}

				result = doca_gpu_semaphore_destroy(tcp_queues->sem_http_cpu[idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed doca_gpu_semaphore_destroy: %s",
						     doca_error_get_descr(result));
					return DOCA_ERROR_BAD_STATE;
				}
			}

			if (http_queues->eth_txq_ctx[idx]) {
				result = doca_ctx_stop(http_queues->eth_txq_ctx[idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
					return DOCA_ERROR_BAD_STATE;
				}
			}

			if (http_queues->eth_txq_cpu[idx]) {
				result = doca_eth_txq_destroy(http_queues->eth_txq_cpu[idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
					return DOCA_ERROR_BAD_STATE;
				}
			}
		}
	}

	if (http_server) {
		result = destroy_tx_buf(&http_queues->buf_page_index);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create buf_page_index: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = destroy_tx_buf(&http_queues->buf_page_contacts);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create buf_page_contacts: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}

		result = destroy_tx_buf(&http_queues->buf_page_not_found);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create buf_page_not_found: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	return DOCA_SUCCESS;
}
