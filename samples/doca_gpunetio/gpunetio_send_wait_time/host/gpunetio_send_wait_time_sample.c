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
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <doca_clock.h>
#include <doca_flow.h>
#include <doca_log.h>

#include "../gpunetio_common.h"

#define QUEUE_ID 0
#define NS_PER_SEC 1000000000LL
#define NS_PER_MSEC 1000000
#define CLOCK_OFFSET_MAX_NS (DELTA_NS / 2) /* Max NIC to host clock offset, within the margin of the first burst */
#define ETHERTYPE_IPV4 0x0800
#define IPV4_VERSION_IHL 0x45	  /* IPv4, header of 5 x 32-bit words (no options) */
#define IPV4_DONT_FRAGMENT 0x4000 /* DF flag in the fragment offset field */
#define IPV4_DEFAULT_TTL 64
#define IPV4_SRC_ADDR 0xC0A80101 /* 192.168.1.1, in host byte order */
#define IPV4_DST_ADDR 0xC0A80102 /* 192.168.1.2, in host byte order */
#define UDP_SRC_PORT 10000
#define UDP_DST_PORT 5000

static struct doca_flow_port *df_port;

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME::SAMPLE);

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
 * Get timestamp in nanoseconds
 *
 * @ns [out]: UTC timestamp
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_ns(uint64_t *ns)
{
	struct timespec t;

	if (clock_gettime(CLOCK_REALTIME, &t) != 0) {
		DOCA_LOG_ERR("Failed to read the real-time clock: %s", strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	*ns = (uint64_t)t.tv_nsec + (uint64_t)t.tv_sec * NS_PER_SEC;
	return DOCA_SUCCESS;
}

/*
 * Check that the NIC real-time clock, which runs the wait on time, is synchronized with the host real-time clock, used
 * to compute the timestamps. The README explains how to synchronize them with phc2sys.
 *
 * @ddev [in]: DOCA device
 * @return: DOCA_SUCCESS if the clocks are synchronized or the offset can't be measured, DOCA_ERROR otherwise
 */
static doca_error_t check_clock_offset(struct doca_dev *ddev)
{
	union doca_clock_timespec_t host_ts = {0}, nic_ts = {0};
	struct doca_clock *clock;
	uint64_t error_margin = 0;
	int64_t offset_ns;
	doca_error_t result, tmp_result;

	result = doca_clock_create(ddev, &clock);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Can't check the NIC clock synchronization, doca_clock_create failed: %s",
			      doca_error_get_descr(result));
		return DOCA_SUCCESS;
	}

	result = doca_clock_get_crosstimestamp(clock,
					       DOCA_CLOCK_HOST_REAL_TIME,
					       DOCA_CLOCK_NIC_REAL_TIME,
					       &host_ts,
					       &nic_ts,
					       &error_margin);
	tmp_result = doca_clock_destroy(clock);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy the DOCA clock: %s", doca_error_get_descr(tmp_result));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Can't check the NIC clock synchronization, doca_clock_get_crosstimestamp failed: %s",
			      doca_error_get_descr(result));
		return DOCA_SUCCESS;
	}

	offset_ns = ((int64_t)nic_ts.ts.tv_sec - (int64_t)host_ts.ts.tv_sec) * NS_PER_SEC +
		    ((int64_t)nic_ts.ts.tv_nsec - (int64_t)host_ts.ts.tv_nsec);
	DOCA_LOG_INFO("NIC clock - host clock offset: %" PRId64 " ns (error margin %" PRIu64 " ns)",
		      offset_ns,
		      error_margin);

	if (offset_ns > CLOCK_OFFSET_MAX_NS || offset_ns < -CLOCK_OFFSET_MAX_NS) {
		DOCA_LOG_ERR(
			"The NIC clock is %" PRId64
			" ns away from the host clock, the bursts would not leave at their time. Synchronize the clocks with phc2sys as described in the README",
			offset_ns);
		return DOCA_ERROR_BAD_STATE;
	}

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

	/* A context that was never started can't be stopped */
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
		txq->eth_txq_ctx = NULL;
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
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_txq(struct txq_queue *txq,
			       struct doca_gpu *gpu_dev,
			       struct doca_dev *ddev,
			       size_t pkt_size,
			       uint32_t pkt_num)
{
	enum doca_eth_wait_on_time_type wait_on_time_mode = DOCA_ETH_WAIT_ON_TIME_TYPE_NONE;
	uint8_t src_mac[DOCA_DEVINFO_MAC_ADDR_SIZE];
	struct eth_ip_udp_hdr *hdr;
	uint8_t *cpu_pkt_addr;
	size_t buffer_size;
	cudaError_t res_cuda;
	doca_error_t result;

	if (txq == NULL || gpu_dev == NULL || ddev == NULL || pkt_size == 0 || pkt_num == 0) {
		DOCA_LOG_ERR("Can't create TXQ queue, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (pkt_size < sizeof(struct eth_ip_udp_hdr)) {
		DOCA_LOG_ERR("Can't create TXQ queue, packet size %zu smaller than Ethernet, IPv4 and UDP headers",
			     pkt_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * Check the feature before creating anything. Wait on time needs a ConnectX-7 or newer NIC whose real-time
	 * clock is enabled: otherwise the query returns DOCA_ERROR_NOT_SUPPORTED.
	 */
	result =
		doca_eth_txq_cap_get_wait_on_time_offload_supported_mode(doca_dev_as_devinfo(ddev), &wait_on_time_mode);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to query the wait on time offload mode: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Wait on time supported mode: %s",
		      (wait_on_time_mode == DOCA_ETH_WAIT_ON_TIME_TYPE_NATIVE) ? "Native" : "None");

	if (wait_on_time_mode != DOCA_ETH_WAIT_ON_TIME_TYPE_NATIVE) {
		DOCA_LOG_ERR(
			"Wait on time is not available. It requires a ConnectX-7 or newer NIC with the REAL_TIME_CLOCK_ENABLE and ACCURATE_TX_SCHEDULER firmware attributes enabled: mlxconfig -d <NIC PCIe address> s REAL_TIME_CLOCK_ENABLE=1 ACCURATE_TX_SCHEDULER=1, then reset the NIC firmware");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	txq->gpu_dev = gpu_dev;
	txq->ddev = ddev;
	txq->pkt_size = pkt_size;
	txq->pkt_num = pkt_num;
	buffer_size = (size_t)txq->pkt_num * pkt_size;

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

	result = doca_eth_txq_set_wait_on_time_offload(txq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_set_wait_on_time_offload: %s", doca_error_get_descr(result));
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

	cpu_pkt_addr = (uint8_t *)calloc(buffer_size, sizeof(uint8_t));
	if (cpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Error in tx buf preparation, failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto exit_error;
	}

	/*
	 * Every packet holds an Ethernet, IPv4 and UDP header, followed by a text payload with the packet number. The
	 * IPv4 and UDP checksums are left to 0: the NIC computes them as the L3 and L4 checksum offloads are enabled.
	 */
	for (uint32_t idx = 0; idx < txq->pkt_num; idx++) {
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
		hdr->l3_hdr.src_addr = htonl(IPV4_SRC_ADDR);
		hdr->l3_hdr.dst_addr = htonl(IPV4_DST_ADDR);

		hdr->l4_hdr.src_port = htons(UDP_SRC_PORT);
		hdr->l4_hdr.dst_port = htons(UDP_DST_PORT);
		hdr->l4_hdr.dgram_len =
			htons((uint16_t)(pkt_size - sizeof(struct ether_hdr) - sizeof(struct ipv4_hdr)));
		hdr->l4_hdr.dgram_cksum = 0;

		snprintf((char *)(hdr + 1), pkt_size - sizeof(struct eth_ip_udp_hdr), "Sent from DOCA GPUNetIO %u", idx);
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
 * Launch GPUNetIO send wait on time sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_send_wait_time(struct sample_send_wait_cfg *sample_cfg)
{
	doca_error_t result, tmp_result;
	struct burst_wait_times wait_times = {0};
	uint32_t *bursts_done_gpu = NULL;
	uint32_t *bursts_done_cpu = NULL;
	uint64_t now_ns, time_seed, burst_time_ns;
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	struct txq_queue txq = {0};
	cudaStream_t stream = NULL;
	cudaError_t res_rt = cudaSuccess;
	bool flow_init = false;

	txq.dmabuf_fd = -1;

	result = init_doca_device(sample_cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_device returned %s", doca_error_get_descr(result));
		return result;
	}

	/* Init and start port for eth */
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

	result = create_txq(&txq, gpu_dev, ddev, PACKET_SIZE, NUM_PACKETS_X_BURST * NUM_BURST_SEND);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_txq returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = check_clock_offset(ddev);
	if (result != DOCA_SUCCESS)
		goto exit;

	res_rt = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %s", cudaGetErrorString(res_rt));
		result = DOCA_ERROR_DRIVER;
		goto exit;
	}

	/* The CUDA kernel reports how many bursts the NIC sent, the CPU reads it once the kernel is done */
	result = doca_gpu_mem_alloc(gpu_dev,
				    sizeof(uint32_t),
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_CPU_GPU,
				    (void **)&bursts_done_gpu,
				    (void **)&bursts_done_cpu);
	if (result != DOCA_SUCCESS || bursts_done_gpu == NULL || bursts_done_cpu == NULL) {
		if (result == DOCA_SUCCESS)
			result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit;
	}
	DOCA_GPUNETIO_VOLATILE(*bursts_done_cpu) = 0;

	result = get_ns(&now_ns);
	if (result != DOCA_SUCCESS)
		goto exit;

	time_seed = now_ns + DELTA_NS;
	for (uint32_t idx = 0; idx < NUM_BURST_SEND; idx++) {
		burst_time_ns = time_seed + (uint64_t)sample_cfg->time_interval_ns * idx;
		result =
			doca_eth_txq_calculate_timestamp(txq.eth_txq_cpu, burst_time_ns, &wait_times.wait_on_time[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get wait on time value for timestamp %" PRIu64 ", error %s",
				     burst_time_ns,
				     doca_error_get_descr(result));
			goto exit;
		}
	}

	DOCA_LOG_INFO(
		"Launching CUDA kernel to send %d bursts of %d packets, the first burst in %d ms (fixed delay introduced by the sample) and the last burst in %" PRIu64
		" ms",
		NUM_BURST_SEND,
		NUM_PACKETS_X_BURST,
		DELTA_NS / NS_PER_MSEC,
		(DELTA_NS + (uint64_t)sample_cfg->time_interval_ns * (NUM_BURST_SEND - 1)) / NS_PER_MSEC);
	result = kernel_send_wait_on_time(stream, &txq, &wait_times, bursts_done_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function kernel_send_wait_on_time returned %s", doca_error_get_descr(result));
		goto exit;
	}

	/* The kernel returns once the NIC completed the last burst, or after a send error */
	res_rt = cudaStreamSynchronize(stream);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamSynchronize error %s", cudaGetErrorString(res_rt));
		result = DOCA_ERROR_DRIVER;
		goto exit;
	}

	if (DOCA_GPUNETIO_VOLATILE(*bursts_done_cpu) != NUM_BURST_SEND) {
		DOCA_LOG_ERR("Send completion error: %u of %d bursts sent",
			     DOCA_GPUNETIO_VOLATILE(*bursts_done_cpu),
			     NUM_BURST_SEND);
		result = DOCA_ERROR_IO_FAILED;
		goto exit;
	}

	DOCA_LOG_INFO("All the %d packets have been sent", NUM_BURST_SEND * NUM_PACKETS_X_BURST);

exit:
	if (stream)
		cudaStreamDestroy(stream);
	if (bursts_done_gpu)
		doca_gpu_mem_free(gpu_dev, bursts_done_gpu);

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
