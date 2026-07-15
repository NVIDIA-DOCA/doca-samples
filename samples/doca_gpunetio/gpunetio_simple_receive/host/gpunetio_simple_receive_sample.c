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

#include <arpa/inet.h>
#include <doca_flow.h>
#include "gpunetio_common.h"
#include "common.h"

#define FLOW_NB_COUNTERS 524228 /* 1024 x 512 */
#define MBUF_NUM 8192
#define MBUF_SIZE 2048

struct doca_flow_port *df_port;
bool force_quit;

DOCA_LOG_REGISTER(GPUNETIO_SIMPLE_RECEIVE);

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

/*
 * Signal handler to quit application gracefully
 *
 * @signum [in]: signal received
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit!", signum);
		DOCA_GPUNETIO_VOLATILE(force_quit) = true;
	}
}

/*
 * Initialize a DOCA network device.
 *
 * @nic_pcie_addr [in]: Network card PCIe address
 * @ddev [out]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_doca_device(char *nic_pcie_addr, struct doca_dev **ddev)
{
	doca_error_t result;

	if (nic_pcie_addr == NULL || ddev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	if (strnlen(nic_pcie_addr, DOCA_DEVINFO_PCI_ADDR_SIZE) >= DOCA_DEVINFO_PCI_ADDR_SIZE)
		return DOCA_ERROR_INVALID_VALUE;

	result = open_doca_device_with_pci(nic_pcie_addr, NULL, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open NIC device based on PCI address");
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Init doca flow.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_doca_flow(void)
{
	struct doca_flow_cfg *queue_flow_cfg;
	doca_error_t result;

	/* Initialize doca flow framework */
	result = doca_flow_cfg_create(&queue_flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_cfg_set_pipe_queues(queue_flow_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_cfg_set_mode_args(queue_flow_cfg, "vnf,isolated");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_cfg_set_nr_counters(queue_flow_cfg, FLOW_NB_COUNTERS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_init(queue_flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca flow with: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}
	doca_flow_cfg_destroy(queue_flow_cfg);

	return DOCA_SUCCESS;
}

/*
 * Start doca flow.
 *
 * @dev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t start_doca_flow(struct doca_dev *dev)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result;

	/* Start doca flow port */
	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_port_id(port_cfg, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port ID: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg dev: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	result = doca_flow_port_start(port_cfg, &df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca flow port with: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow UDP pipeline
 *
 * @rxq [in]: Receive queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_udp_pipe(struct rxq_queue *rxq)
{
	doca_error_t result;
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd miss_fwd = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	uint16_t rss_queues[1];
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};
	const char *pipe_name = "GPU_RXQ_UDP_PIPE";

	if (rxq == NULL || df_port == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	doca_eth_rxq_apply_queue_id(rxq->eth_rxq_cpu, 0);
	rss_queues[0] = 0;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = 1;

	miss_fwd.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, &(rxq->rxq_pipe));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	/* Add HW offload */
	result =
		doca_flow_pipe_add_entry(0, rxq->rxq_pipe, &match, 0, NULL, NULL, NULL, DOCA_FLOW_NO_WAIT, NULL, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(df_port, 0, 0, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow root pipeline
 *
 * @rxq [in]: Receive queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_root_pipe(struct rxq_queue *rxq)
{
	doca_error_t result;
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};

	struct doca_flow_match udp_match = {
		.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
		.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
		.outer.ip4.next_proto = IPPROTO_UDP,
	};

	struct doca_flow_fwd udp_fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
	};

	struct doca_flow_pipe_cfg *pipe_cfg;
	const char *pipe_name = "ROOT_PIPE";

	if (rxq == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	udp_fwd.next_pipe = rxq->rxq_pipe;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &rxq->root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  rxq->root_pipe,
						  &udp_match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &udp_fwd,
						  NULL,
						  &rxq->root_udp_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe UDP entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(df_port, 0, 0, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Destroy DOCA Ethernet Tx queue for GPU
 *
 * @rxq [in]: DOCA Eth Rx queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_rxq(struct rxq_queue *rxq)
{
	doca_error_t result;

	if (rxq == NULL) {
		DOCA_LOG_ERR("Can't destroy UDP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Destroying Rxq");

	if (rxq->root_pipe != NULL) {
		doca_flow_pipe_destroy(rxq->root_pipe);
	}
	if (rxq->rxq_pipe != NULL) {
		doca_flow_pipe_destroy(rxq->rxq_pipe);
	}

	if (df_port != NULL) {
		result = doca_flow_port_stop(df_port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->eth_rxq_ctx != NULL) {
		result = doca_ctx_stop(rxq->eth_rxq_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->gpu_pkt_addr != NULL) {
		result = doca_gpu_mem_free(rxq->gpu_dev, rxq->gpu_pkt_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->eth_rxq_cpu != NULL) {
		result = doca_eth_rxq_destroy(rxq->eth_rxq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->pkt_buff_mmap != NULL) {
		result = doca_mmap_destroy(rxq->pkt_buff_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	result = doca_dev_close(rxq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Eth dev: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (df_port != NULL)
		doca_flow_destroy();

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Ethernet Tx queue for GPU
 *
 * @rxq [in]: DOCA Eth Tx queue handler
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_rxq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, int cuda_id, struct doca_dev *ddev)
{
	doca_error_t result;
	uint32_t cyclic_buffer_size = 0;
	struct cudaDeviceProp prop;

	if (rxq == NULL || gpu_dev == NULL || ddev == NULL) {
		DOCA_LOG_ERR("Can't create UDP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rxq->gpu_dev = gpu_dev;
	rxq->ddev = ddev;
	rxq->port = df_port;

	DOCA_LOG_INFO("Creating Sample Eth Rxq");

	result = doca_eth_rxq_create(rxq->ddev, MAX_PKT_NUM, MAX_PKT_SIZE, &(rxq->eth_rxq_cpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_create: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_rxq_set_type(rxq->eth_rxq_cpu, DOCA_ETH_RXQ_TYPE_CYCLIC);
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
		goto exit_error;
	}

	result = doca_mmap_create(&rxq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_add_dev(rxq->pkt_buff_mmap, rxq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add dev to mmap: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	ALIGN_SIZE(cyclic_buffer_size, get_host_page_size());

	result = doca_gpu_mem_alloc(rxq->gpu_dev,
				    cyclic_buffer_size,
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&rxq->gpu_pkt_addr,
				    NULL);
	if (result != DOCA_SUCCESS || rxq->gpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Map GPU memory buffer used to receive packets with DMABuf */
	result = doca_gpu_dmabuf_fd(rxq->gpu_dev, rxq->gpu_pkt_addr, cyclic_buffer_size, &(rxq->dmabuf_fd));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB) with nvidia-peermem mode",
			      rxq->gpu_pkt_addr,
			      cyclic_buffer_size);

		/* If failed, use nvidia-peermem legacy method */
		result = doca_mmap_set_memrange(rxq->pkt_buff_mmap, rxq->gpu_pkt_addr, cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	} else {
		DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB dmabuf fd %d) with dmabuf mode",
			      rxq->gpu_pkt_addr,
			      cyclic_buffer_size,
			      rxq->dmabuf_fd);

		result = doca_mmap_set_dmabuf_memrange(rxq->pkt_buff_mmap,
						       rxq->dmabuf_fd,
						       rxq->gpu_pkt_addr,
						       0,
						       cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set dmabuf memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	}

	result = doca_mmap_set_permissions(rxq->pkt_buff_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions for mmap %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_start(rxq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_rxq_set_pkt_buf(rxq->eth_rxq_cpu, rxq->pkt_buff_mmap, 0, cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cyclic buffer  %s", doca_error_get_descr(result));
		goto exit_error;
	}

	cudaGetDeviceProperties(&prop, cuda_id);
	// If pre-Hopper GPU with __CUDA_ARCH__ < 900
	if (prop.major < 9) {
		result = doca_eth_rxq_gpu_enable_mcst_qp(rxq->eth_rxq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set GPU dump qp  %s", doca_error_get_descr(result));
			goto exit_error;
		}
	}

	rxq->eth_rxq_ctx = doca_eth_rxq_as_doca_ctx(rxq->eth_rxq_cpu);
	if (rxq->eth_rxq_ctx == NULL) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_as_doca_ctx: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_ctx_set_datapath_on_gpu(rxq->eth_rxq_ctx, rxq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_ctx_start(rxq->eth_rxq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_rxq_get_gpu_handle(rxq->eth_rxq_cpu, &(rxq->eth_rxq_gpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_get_gpu_handle: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Create UDP based flow pipe */
	result = create_udp_pipe(rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_udp_pipe returned %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Create root pipe with UDP pipe as unique entry */
	result = create_root_pipe(rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_root_pipe returned %s", doca_error_get_descr(result));
		goto exit_error;
	}

	return DOCA_SUCCESS;

exit_error:
	destroy_rxq(rxq);
	return DOCA_ERROR_BAD_STATE;
}

/*
 * Launch GPUNetIO simple receive sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_simple_receive(struct sample_simple_recv_cfg *sample_cfg)
{
	doca_error_t result;
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	struct rxq_queue rxq = {0};
	cudaStream_t stream;
	cudaError_t res_rt = cudaSuccess;
	uint32_t *cpu_exit_condition;
	uint32_t *gpu_exit_condition;
	uint64_t *cpu_tot_pkts;
	uint64_t *gpu_tot_pkts;

	result = init_doca_device(sample_cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_device returned %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = init_doca_flow();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_flow returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = start_doca_flow(ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function start_doca_flow returned %s", doca_error_get_descr(result));
		goto exit;
	}

	/* Gracefully terminate sample if ctrlc */
	DOCA_GPUNETIO_VOLATILE(force_quit) = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	result = doca_gpu_create(sample_cfg->gpu_pcie_addr, &gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = create_rxq(&rxq, gpu_dev, sample_cfg->cuda_id, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_rxq returned %s", doca_error_get_descr(result));
		goto exit;
	}

	res_rt = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", res_rt);
		return DOCA_ERROR_DRIVER;
	}

	result = doca_gpu_mem_alloc(gpu_dev,
				    sizeof(uint32_t),
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&gpu_exit_condition,
				    (void **)&cpu_exit_condition);
	if (result != DOCA_SUCCESS || gpu_exit_condition == NULL || cpu_exit_condition == NULL) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc returned %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}
	cpu_exit_condition[0] = 0;

	result = doca_gpu_mem_alloc(gpu_dev,
				    sizeof(uint64_t),
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_CPU_GPU,
				    (void **)&gpu_tot_pkts,
				    (void **)&cpu_tot_pkts);
	if (result != DOCA_SUCCESS || gpu_tot_pkts == NULL || cpu_tot_pkts == NULL) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc returned %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}
	cpu_tot_pkts[0] = 0;

	DOCA_LOG_INFO("Launching CUDA kernel to receive packets");

	kernel_receive_packets(stream, &rxq, sample_cfg->exec_scope, gpu_exit_condition, gpu_tot_pkts);

	DOCA_LOG_INFO("Waiting for termination");
	/* This loop keeps busy main thread until force_quit is set to 1 (e.g. typing ctrl+c) */
	while (DOCA_GPUNETIO_VOLATILE(force_quit) == false)
		;
	DOCA_GPUNETIO_VOLATILE(*cpu_exit_condition) = 1;

	cudaStreamSynchronize(stream);

	printf("Exiting from simple receive sample. Total number of received packets: %ld\n", cpu_tot_pkts[0]);
exit:

	result = destroy_rxq(&rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function destroy_rxq returned %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Sample finished successfully");

	return DOCA_SUCCESS;
}
