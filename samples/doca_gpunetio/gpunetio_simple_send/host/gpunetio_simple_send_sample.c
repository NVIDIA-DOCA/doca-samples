/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <time.h>
#include <doca_flow.h>
#include <doca_log.h>

#include "gpunetio_common.h"

#include "common.h"

#define FLOW_NB_COUNTERS 524288 /* 1024 x 512 */
#define QUEUE_ID 0
#define ETHERTYPE_IPV4 0x0800
#define IPV4_VERSION_IHL 0x45	  /* IPv4, header of 5 x 32-bit words (no options) */
#define IPV4_DONT_FRAGMENT 0x4000 /* DF flag in the fragment offset field */
#define IPV4_DEFAULT_TTL 64

static struct doca_flow_port *df_port;
static volatile sig_atomic_t force_quit;

DOCA_LOG_REGISTER(SIMPLE_SEND::SAMPLE);

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
	/* Logging is not async-signal-safe: the main loop reports the exit */
	if (signum == SIGINT || signum == SIGTERM)
		force_quit = 1;
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

	if (strlen(nic_pcie_addr) >= DOCA_DEVINFO_PCI_ADDR_SIZE)
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

	result = doca_flow_cfg_set_mode_args(queue_flow_cfg, "vnf");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_cfg_set_resource_mode(queue_flow_cfg, DOCA_FLOW_RESOURCE_MODE_PORT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg resource mode: %s", doca_error_get_descr(result));
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

	result = doca_flow_port_cfg_set_nr_resources(port_cfg, DOCA_FLOW_RESOURCE_COUNTER, FLOW_NB_COUNTERS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg nr_resources: %s", doca_error_get_name(result));
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

	doca_flow_port_cfg_destroy(port_cfg);

	return DOCA_SUCCESS;
}

/*
 * Destroy DOCA Ethernet Tx queue for GPU.
 * Every resource is released even if a previous step fails, the first error is returned.
 *
 * @txq [in]: DOCA Eth Tx queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_txq(struct txq_queue *txq)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	if (txq == NULL) {
		DOCA_LOG_ERR("Can't destroy Txq, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Destroying Txq");

	if (txq->ctx_started) {
		tmp_result = doca_ctx_stop(txq->eth_txq_ctx);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		txq->ctx_started = false;
	}

	if (txq->eth_txq_cpu != NULL) {
		tmp_result = doca_eth_txq_destroy(txq->eth_txq_cpu);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_destroy: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		txq->eth_txq_cpu = NULL;
	}

	/* The mmap registers the packet buffer with the NIC: destroy it before releasing the memory */
	if (txq->pkt_buff_mmap != NULL) {
		tmp_result = doca_mmap_destroy(txq->pkt_buff_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		txq->pkt_buff_mmap = NULL;
	}

	if (txq->dmabuf_fd >= 0) {
		close(txq->dmabuf_fd);
		txq->dmabuf_fd = -1;
	}

	if (txq->pkt_buff_addr != NULL) {
		tmp_result = doca_gpu_mem_free(txq->gpu_dev, txq->pkt_buff_addr);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		txq->pkt_buff_addr = NULL;
	}

	return result;
}

/*
 * Create DOCA Ethernet Tx queue for GPU
 *
 * @txq [in]: DOCA Eth Tx queue handler
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA device
 * @pkt_size [in]: Packet max size
 * @pkt_num [in]: Packet number in buffer
 * @cpu_proxy [in]: Enable CPU proxy mode
 * @src_ip [in]: IPv4 source address, network byte order
 * @dst_ip [in]: IPv4 destination address, network byte order
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_txq(struct txq_queue *txq,
			       struct doca_gpu *gpu_dev,
			       struct doca_dev *ddev,
			       size_t pkt_size,
			       uint32_t pkt_num,
			       bool cpu_proxy,
			       uint32_t src_ip,
			       uint32_t dst_ip)
{
	doca_error_t result;
	cudaError_t res_cuda;

	size_t buffer_size = 0;
	uint8_t *cpu_pkt_addr;
	struct eth_ip_udp_hdr *hdr;
	uint8_t src_mac[DOCA_DEVINFO_MAC_ADDR_SIZE];

	if (txq == NULL || gpu_dev == NULL || ddev == NULL || pkt_size == 0 || pkt_num == 0) {
		DOCA_LOG_ERR("Can't create TXQ queue, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (pkt_size < sizeof(struct eth_ip_udp_hdr)) {
		DOCA_LOG_ERR("Can't create TXQ queue, packet size %zu smaller than Ethernet, IPv4 and UDP headers",
			     pkt_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	txq->gpu_dev = gpu_dev;
	txq->ddev = ddev;
	txq->pkt_size = pkt_size;
	txq->cuda_threads = pkt_num;
	txq->src_ip = src_ip;
	txq->dst_ip = dst_ip;
	buffer_size = (size_t)txq->cuda_threads * pkt_size;

	DOCA_LOG_INFO("Creating Sample Eth Txq");

	result = doca_eth_txq_create(txq->ddev, MAX_SQ_DESCR_NUM, &(txq->eth_txq_cpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_create: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_txq_set_l3_checksum_offload(txq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_txq_set_l4_checksum_offload(txq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set eth_txq l4 offloads: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Application can check Txq completions on the GPU. By default, it can be done by CPU. */
	result = doca_eth_txq_gpu_set_completion_on_gpu(txq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_gpu_set_completion_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	if (cpu_proxy) {
		result = doca_eth_txq_gpu_set_uar_on_cpu(txq->eth_txq_cpu, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_gpu_set_uar_on_cpu: %s", doca_error_get_descr(result));
			goto exit_error;
		}
	}

	txq->eth_txq_ctx = doca_eth_txq_as_ctx(txq->eth_txq_cpu);
	if (txq->eth_txq_ctx == NULL) {
		DOCA_LOG_ERR("Failed doca_eth_txq_as_ctx");
		result = DOCA_ERROR_BAD_STATE;
		goto exit_error;
	}

	result = doca_ctx_set_datapath_on_gpu(txq->eth_txq_ctx, txq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_ctx_start(txq->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	txq->ctx_started = true;

	result = doca_eth_txq_apply_queue_id(txq->eth_txq_cpu, QUEUE_ID);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_apply_queue_id: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_txq_get_gpu_handle(txq->eth_txq_cpu, &(txq->eth_txq_gpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_get_gpu_handle: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_create(&txq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_add_dev(txq->pkt_buff_mmap, txq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add dev to mmap: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	ALIGN_SIZE(buffer_size, get_host_page_size());

	result = doca_gpu_mem_alloc(txq->gpu_dev,
				    buffer_size,
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&txq->pkt_buff_addr,
				    NULL);
	if (result != DOCA_SUCCESS || txq->pkt_buff_addr == NULL) {
		if (result == DOCA_SUCCESS)
			result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Packets are sent with the MAC address of the NIC port as source */
	result = doca_devinfo_get_mac_addr(doca_dev_as_devinfo(txq->ddev), src_mac, DOCA_DEVINFO_MAC_ADDR_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get NIC MAC address: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	DOCA_LOG_INFO("Source MAC address %02x:%02x:%02x:%02x:%02x:%02x",
		      src_mac[0],
		      src_mac[1],
		      src_mac[2],
		      src_mac[3],
		      src_mac[4],
		      src_mac[5]);

	/* Same size as the GPU buffer, aligned to the page size, as it is copied entirely */
	cpu_pkt_addr = (uint8_t *)calloc(buffer_size, sizeof(uint8_t));
	if (cpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Error in tx buf preparation, failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto exit_error;
	}

	/*
	 * Every packet buffer holds an Ethernet, IPv4 and UDP header followed by a zero payload. All the packets
	 * belong to the same UDP flow. The IPv4 and UDP checksums are left to 0: the NIC computes them as the L3 and
	 * L4 checksum offloads are enabled.
	 */
	for (uint32_t idx = 0; idx < txq->cuda_threads; idx++) {
		hdr = (struct eth_ip_udp_hdr *)(cpu_pkt_addr + (idx * pkt_size));

		hdr->l2_hdr.d_addr_bytes[0] = 0x10;
		hdr->l2_hdr.d_addr_bytes[1] = 0x11;
		hdr->l2_hdr.d_addr_bytes[2] = 0x12;
		hdr->l2_hdr.d_addr_bytes[3] = 0x13;
		hdr->l2_hdr.d_addr_bytes[4] = 0x14;
		hdr->l2_hdr.d_addr_bytes[5] = 0x15;

		memcpy(hdr->l2_hdr.s_addr_bytes, src_mac, ETHER_ADDR_LEN);

		hdr->l2_hdr.ether_type = htons(ETHERTYPE_IPV4);

		hdr->l3_hdr.version_ihl = IPV4_VERSION_IHL;
		hdr->l3_hdr.type_of_service = 0;
		hdr->l3_hdr.total_length = htons((uint16_t)(pkt_size - sizeof(struct ether_hdr)));
		hdr->l3_hdr.packet_id = 0;
		hdr->l3_hdr.fragment_offset = htons(IPV4_DONT_FRAGMENT);
		hdr->l3_hdr.time_to_live = IPV4_DEFAULT_TTL;
		hdr->l3_hdr.next_proto_id = IPPROTO_UDP;
		hdr->l3_hdr.hdr_checksum = 0;
		hdr->l3_hdr.src_addr = txq->src_ip;
		hdr->l3_hdr.dst_addr = txq->dst_ip;

		hdr->l4_hdr.src_port = htons(UDP_SRC_PORT);
		hdr->l4_hdr.dst_port = htons(UDP_DST_PORT);
		hdr->l4_hdr.dgram_len =
			htons((uint16_t)(pkt_size - sizeof(struct ether_hdr) - sizeof(struct ipv4_hdr)));
		hdr->l4_hdr.dgram_cksum = 0;
	}

	res_cuda = cudaMemcpy(txq->pkt_buff_addr, cpu_pkt_addr, buffer_size, cudaMemcpyDefault);
	free(cpu_pkt_addr);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Function CUDA Memcpy packet buffer failed with %s", cudaGetErrorString(res_cuda));
		result = DOCA_ERROR_DRIVER;
		goto exit_error;
	}

	/* Map GPU memory buffer used to send packets with DMABuf */
	result = doca_gpu_dmabuf_fd(txq->gpu_dev, txq->pkt_buff_addr, buffer_size, &(txq->dmabuf_fd));
	if (result != DOCA_SUCCESS) {
		txq->dmabuf_fd = -1;
		DOCA_LOG_INFO("Mapping send queue buffer (%p size %zuB) with nvidia-peermem mode",
			      txq->pkt_buff_addr,
			      buffer_size);

		/* If failed, use nvidia-peermem legacy method */
		result = doca_mmap_set_memrange(txq->pkt_buff_mmap, txq->pkt_buff_addr, buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	} else {
		DOCA_LOG_INFO("Mapping send queue buffer (%p size %zuB dmabuf fd %d) with dmabuf mode",
			      txq->pkt_buff_addr,
			      buffer_size,
			      txq->dmabuf_fd);

		result = doca_mmap_set_dmabuf_memrange(txq->pkt_buff_mmap,
						       txq->dmabuf_fd,
						       txq->pkt_buff_addr,
						       0,
						       buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set dmabuf memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	}

	result = doca_mmap_set_permissions(txq->pkt_buff_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions for mmap %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_start(txq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_get_mkey(txq->pkt_buff_mmap, txq->ddev, &txq->pkt_buff_mkey);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get mmap mkey %s", doca_error_get_descr(result));
		goto exit_error;
	}
	// N.B. mkey must be in network byte order
	txq->pkt_buff_mkey = htobe32(txq->pkt_buff_mkey);

	return DOCA_SUCCESS;

exit_error:
	/* Caller owns the cleanup: it always invokes destroy_txq() on its exit path */
	return result;
}

/*
 * CPU proxy thread: wait in a continuous loop an update from the GPU.
 * Once the GPU requires to ring the doorbell, it notifies this thread via internal
 * shared memory. This CPU thread rings the Txq DB in place of the GPU.
 *
 * @args_ [in]: Thread args
 */
static void *progress_cpu_proxy(void *args_)
{
	struct cpu_proxy_args *args = (struct cpu_proxy_args *)args_;

	DOCA_LOG_INFO("CPU proxy progress thread is running");

	while (ACCESS_ONCE_64b(args->exit_flag) == 0)
		doca_eth_txq_gpu_cpu_proxy_progress(args->txq);

	return NULL;
}

/*
 * Launch GPUNetIO simple send sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_simple_send(struct sample_simple_send_cfg *sample_cfg)
{
	doca_error_t result, tmp_result;
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	struct txq_queue txq = {0};
	cudaStream_t stream = NULL;
	cudaError_t res_rt = cudaSuccess;
	uint32_t *cpu_exit_condition = NULL;
	uint32_t *gpu_exit_condition = NULL;
	struct cpu_proxy_args args = {0};
	pthread_t thread_id;
	bool flow_init = false, proxy_started = false, kernel_launched = false;
	const struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1 ms */

	txq.dmabuf_fd = -1;

	/* Gracefully terminate sample if ctrlc, also during the initialization */
	force_quit = 0;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	result = init_doca_device(sample_cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_device returned %s", doca_error_get_descr(result));
		return result;
	}

	result = init_doca_flow();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_flow returned %s", doca_error_get_descr(result));
		goto exit;
	}
	flow_init = true;

	result = start_doca_flow(ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function start_doca_flow returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = doca_gpu_create(sample_cfg->gpu_pcie_addr, &gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = create_txq(&txq,
			    gpu_dev,
			    ddev,
			    sample_cfg->pkt_size,
			    sample_cfg->cuda_threads,
			    (sample_cfg->nic_handler == DOCA_GPUNETIO_ETH_NIC_HANDLER_CPU_PROXY),
			    sample_cfg->src_ip,
			    sample_cfg->dst_ip);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_txq returned %s", doca_error_get_descr(result));
		goto exit;
	}

	res_rt = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %s", cudaGetErrorString(res_rt));
		result = DOCA_ERROR_DRIVER;
		goto exit;
	}

	result = doca_gpu_mem_alloc(gpu_dev,
				    sizeof(uint32_t),
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&gpu_exit_condition,
				    (void **)&cpu_exit_condition);
	if (result != DOCA_SUCCESS || gpu_exit_condition == NULL || cpu_exit_condition == NULL) {
		if (result == DOCA_SUCCESS)
			result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc returned %s", doca_error_get_descr(result));
		goto exit;
	}
	DOCA_GPUNETIO_VOLATILE(*cpu_exit_condition) = 0;

	if (sample_cfg->nic_handler == DOCA_GPUNETIO_ETH_NIC_HANDLER_CPU_PROXY) {
		args.txq = txq.eth_txq_cpu;
		args.exit_flag = 0;

		int ret = pthread_create(&thread_id, NULL, progress_cpu_proxy, (void *)&args);
		if (ret != 0) {
			DOCA_LOG_ERR("Failed to create CPU proxy thread: %s", strerror(ret));
			result = DOCA_ERROR_OPERATING_SYSTEM;
			goto exit;
		}
		proxy_started = true;
	}

	DOCA_LOG_INFO("Launching CUDA kernel to send packets.");

	result = kernel_send_packets(stream, &txq, gpu_exit_condition, sample_cfg->shared_qp, sample_cfg->exec_scope);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function kernel_send_packets returned %s", doca_error_get_descr(result));
		goto exit;
	}
	kernel_launched = true;

	DOCA_LOG_INFO("Waiting for ctrl+c termination");
	/* The kernel sets the exit condition on its own only in case of send error */
	while (force_quit == 0 && DOCA_GPUNETIO_VOLATILE(*cpu_exit_condition) == 0)
		nanosleep(&poll_interval, NULL);

	if (force_quit == 0) {
		DOCA_LOG_ERR("CUDA kernel stopped sending because of a send completion error");
		result = DOCA_ERROR_IO_FAILED;
	} else {
		DOCA_LOG_INFO("Signal received, exiting from sample");
		DOCA_GPUNETIO_VOLATILE(*cpu_exit_condition) = 1;
	}

exit:
	if (kernel_launched) {
		res_rt = cudaStreamSynchronize(stream);
		if (res_rt != cudaSuccess) {
			DOCA_LOG_ERR("Function cudaStreamSynchronize error %s", cudaGetErrorString(res_rt));
			DOCA_ERROR_PROPAGATE(result, DOCA_ERROR_DRIVER);
		}
	}

	if (proxy_started) {
		WRITE_ONCE_64b(args.exit_flag, 1);
		pthread_join(thread_id, NULL);
	}

	if (stream)
		cudaStreamDestroy(stream);
	if (gpu_exit_condition)
		doca_gpu_mem_free(gpu_dev, gpu_exit_condition);

	tmp_result = destroy_txq(&txq);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function destroy_txq returned %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	if (df_port != NULL) {
		tmp_result = doca_flow_port_stop(df_port);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		df_port = NULL;
	}

	if (flow_init)
		doca_flow_destroy();

	if (gpu_dev) {
		tmp_result = doca_gpu_destroy(gpu_dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_gpu_destroy returned %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	if (ddev != NULL) {
		tmp_result = doca_dev_close(ddev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dev_close returned %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	return result;
}
