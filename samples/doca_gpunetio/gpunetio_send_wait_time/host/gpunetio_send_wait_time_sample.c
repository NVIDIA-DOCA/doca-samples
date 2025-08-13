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

#include <time.h>
#include <rte_ethdev.h>

#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_bitfield.h>

#include "../gpunetio_common.h"

#define MAC_ADDR_BYTE_SZ 6
#define MAX_PORT_STR_LEN 128
struct doca_flow_port *df_port;

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME : SAMPLE);

/*
 * Initialize a DOCA network device.
 *
 * @nic_pcie_addr [in]: Network card PCIe address
 * @ddev [out]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_doca_device(char *nic_pcie_addr, struct doca_dev **ddev)
{
	doca_error_t result;
	int ret;
	char *eal_param[3] = {"", "-a", "00:00.0"};

	if (nic_pcie_addr == NULL || ddev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	if (strlen(nic_pcie_addr) >= DOCA_DEVINFO_PCI_ADDR_SIZE)
		return DOCA_ERROR_INVALID_VALUE;

	result = open_doca_device_with_pci(nic_pcie_addr, NULL, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open NIC device based on PCI address");
		return result;
	}

	ret = rte_eal_init(3, eal_param);
	if (ret < 0) {
		DOCA_LOG_ERR("DPDK init failed: %d", ret);
		return DOCA_ERROR_DRIVER;
	}

	// /* Enable DOCA Flow HWS mode */
	result = doca_dpdk_port_probe(*ddev, "dv_flow_en=2");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpdk_port_probe returned %s", doca_error_get_descr(result));
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
 * Get timestamp in nanoseconds
 *
 * @return: UTC timestamp
 */
uint64_t get_ns(void)
{
	struct timespec t;
	int ret;

	ret = clock_gettime(CLOCK_REALTIME, &t);
	if (ret != 0)
		exit(EXIT_FAILURE);

	return (uint64_t)t.tv_nsec + (uint64_t)t.tv_sec * 1000 * 1000 * 1000;
}

/*
 * Create TX buf to send dummy packets to Ethernet broadcast address
 *
 * @txq [in]: DOCA Eth Tx queue with Tx buf
 * @num_packets [in]: Number of packets in the doca_buf_arr of the txbuf
 * @max_pkt_sz [in]: Max packet size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_tx_buf(struct txq_queue *txq, uint32_t num_packets, uint32_t max_pkt_sz)
{
	doca_error_t status;
	struct tx_buf *buf;

	if (txq == NULL || num_packets == 0 || max_pkt_sz == 0) {
		DOCA_LOG_ERR("Invalid input arguments");
		return DOCA_ERROR_INVALID_VALUE;
	}

	buf = &(txq->txbuf);
	buf->num_packets = num_packets;
	buf->max_pkt_sz = max_pkt_sz;
	buf->gpu_dev = txq->gpu_dev;

	status = doca_mmap_create(&(buf->mmap));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create doca_buf: failed to create mmap");
		return status;
	}

	status = doca_mmap_add_dev(buf->mmap, txq->ddev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add dev to buf: doca mmap internal error");
		return status;
	}

	status = doca_gpu_mem_alloc(buf->gpu_dev,
				    buf->num_packets * buf->max_pkt_sz,
				    GPU_PAGE_SIZE,
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&(buf->gpu_pkt_addr),
				    NULL);
	if ((status != DOCA_SUCCESS) || (buf->gpu_pkt_addr == NULL)) {
		DOCA_LOG_ERR("Unable to alloc txbuf: failed to allocate gpu memory");
		return status;
	}

	/* Map GPU memory buffer used to send packets with DMABuf */
	status = doca_gpu_dmabuf_fd(buf->gpu_dev,
				    buf->gpu_pkt_addr,
				    buf->num_packets * buf->max_pkt_sz,
				    &(buf->dmabuf_fd));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Mapping send queue buffer (0x%p size %dB) with legacy nvidia-peermem mode",
			      buf->gpu_pkt_addr,
			      buf->num_packets * buf->max_pkt_sz);

		/* If failed, use nvidia-peermem legacy method */
		status = doca_mmap_set_memrange(buf->mmap, buf->gpu_pkt_addr, (buf->num_packets * buf->max_pkt_sz));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca mmap internal error");
			return status;
		}
	} else {
		DOCA_LOG_INFO("Mapping send queue buffer (0x%p size %dB dmabuf fd %d) with dmabuf mode",
			      buf->gpu_pkt_addr,
			      (buf->num_packets * buf->max_pkt_sz),
			      buf->dmabuf_fd);

		status = doca_mmap_set_dmabuf_memrange(buf->mmap,
						       buf->dmabuf_fd,
						       buf->gpu_pkt_addr,
						       0,
						       (buf->num_packets * buf->max_pkt_sz));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set dmabuf memrange for mmap %s", doca_error_get_descr(status));
			return status;
		}
	}

	status = doca_mmap_set_permissions(buf->mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca mmap internal error");
		return status;
	}

	status = doca_mmap_start(buf->mmap);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca mmap internal error");
		return status;
	}

	status = doca_buf_arr_create(buf->num_packets, &buf->buf_arr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		return status;
	}

	status = doca_buf_arr_set_target_gpu(buf->buf_arr, buf->gpu_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		return status;
	}

	status = doca_buf_arr_set_params(buf->buf_arr, buf->mmap, buf->max_pkt_sz, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		return status;
	}

	status = doca_buf_arr_start(buf->buf_arr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		return status;
	}

	status = doca_buf_arr_get_gpu_handle(buf->buf_arr, &(buf->buf_arr_gpu));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get buff_arr GPU handle: %s", doca_error_get_descr(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Pre-prepare TX buf filling default values in GPU memory
 *
 * @txq [in]: DOCA Eth Tx queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_tx_buf(struct txq_queue *txq, struct doca_dev *ddev)
{
	uint8_t *cpu_pkt_addr;
	uint8_t *pkt;
	struct ether_hdr *hdr;
	cudaError_t res_cuda;
	doca_error_t status;
	struct tx_buf *buf;
	uint8_t mac_addr[MAC_ADDR_BYTE_SZ];
	uint32_t idx;
	const char *payload = "Sent from DOCA GPUNetIO";

	if (txq == NULL) {
		DOCA_LOG_ERR("Invalid input arguments");
		return DOCA_ERROR_INVALID_VALUE;
	}

	buf = &(txq->txbuf);
	buf->pkt_nbytes = strlen(payload);

	status = doca_devinfo_get_mac_addr(doca_dev_as_devinfo(ddev), mac_addr, MAC_ADDR_BYTE_SZ);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get interface MAC address: %s", doca_error_get_descr(status));
		return status;
	}

	cpu_pkt_addr = (uint8_t *)calloc(buf->num_packets * buf->max_pkt_sz, sizeof(uint8_t));
	if (cpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Error in txbuf preparation, failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	for (idx = 0; idx < buf->num_packets; idx++) {
		pkt = cpu_pkt_addr + (idx * buf->max_pkt_sz);
		hdr = (struct ether_hdr *)pkt;

		hdr->s_addr_bytes[0] = mac_addr[0];
		hdr->s_addr_bytes[1] = mac_addr[1];
		hdr->s_addr_bytes[2] = mac_addr[2];
		hdr->s_addr_bytes[3] = mac_addr[3];
		hdr->s_addr_bytes[4] = mac_addr[4];
		hdr->s_addr_bytes[5] = mac_addr[5];

		hdr->d_addr_bytes[0] = 0x10;
		hdr->d_addr_bytes[1] = 0x11;
		hdr->d_addr_bytes[2] = 0x12;
		hdr->d_addr_bytes[3] = 0x13;
		hdr->d_addr_bytes[4] = 0x14;
		hdr->d_addr_bytes[5] = 0x15;

		hdr->ether_type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);

		/* Assuming no TCP flags needed */
		pkt = pkt + sizeof(struct ether_hdr);

		memcpy(pkt, payload, buf->pkt_nbytes);
	}

	/* Copy the whole list of packets into GPU memory buffer */
	res_cuda = cudaMemcpy(buf->gpu_pkt_addr, cpu_pkt_addr, buf->num_packets * buf->max_pkt_sz, cudaMemcpyDefault);
	free(cpu_pkt_addr);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Function CUDA Memcpy cqe_addr failed with %s", cudaGetErrorString(res_cuda));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy TX buf
 *
 * @txq [in]: DOCA Eth Tx queue with Tx buf
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_tx_buf(struct txq_queue *txq)
{
	doca_error_t status;
	struct tx_buf *buf;

	if (txq == NULL) {
		DOCA_LOG_ERR("Invalid input arguments");
		return DOCA_ERROR_INVALID_VALUE;
	}

	buf = &(txq->txbuf);

	/* Tx buf may not be created yet */
	if (buf == NULL)
		return DOCA_SUCCESS;

	if (buf->mmap) {
		status = doca_mmap_destroy(buf->mmap);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to destroy doca_buf: failed to destroy mmap");
			return status;
		}
	}

	if (buf->gpu_pkt_addr) {
		status = doca_gpu_mem_free(txq->gpu_dev, buf->gpu_pkt_addr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to stop buf: failed to free gpu memory");
			return status;
		}
	}

	if (buf->buf_arr) {
		status = doca_buf_arr_stop(buf->buf_arr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to stop buf: failed to destroy doca_buf_arr");
			return status;
		}

		status = doca_buf_arr_destroy(buf->buf_arr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to stop buf: failed to destroy doca_buf_arr");
			return status;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy DOCA Ethernet Tx queue for GPU
 *
 * @txq [in]: DOCA Eth Tx queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_txq(struct txq_queue *txq)
{
	doca_error_t result;

	if (txq == NULL) {
		DOCA_LOG_ERR("Can't destroy Tx queue, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_ctx_stop(txq->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_txq_destroy(txq->eth_txq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	doca_flow_port_stop(df_port);

	result = doca_dev_close(txq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_dev_close: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Ethernet Tx queue for GPU
 *
 * @txq [in]: DOCA Eth Tx queue handler
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_txq(struct txq_queue *txq, struct doca_gpu *gpu_dev, struct doca_dev *ddev)
{
	doca_error_t result;

	if (txq == NULL || gpu_dev == NULL || ddev == NULL) {
		DOCA_LOG_ERR("Can't create DOCA Eth Tx queue, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	txq->gpu_dev = gpu_dev;
	txq->ddev = ddev;

	result = doca_eth_txq_create(txq->ddev, MAX_SQ_DESCR_NUM, &(txq->eth_txq_cpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_create: %s", doca_error_get_descr(result));
		destroy_txq(txq);
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_txq_set_wait_on_time_offload(txq->eth_txq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
		destroy_txq(txq);
		return DOCA_ERROR_BAD_STATE;
	}

	txq->eth_txq_ctx = doca_eth_txq_as_doca_ctx(txq->eth_txq_cpu);
	if (txq->eth_txq_ctx == NULL) {
		DOCA_LOG_ERR("Failed doca_eth_txq_as_doca_ctx: %s", doca_error_get_descr(result));
		destroy_txq(txq);
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_ctx_set_datapath_on_gpu(txq->eth_txq_ctx, txq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
		destroy_txq(txq);
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_ctx_start(txq->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
		destroy_txq(txq);
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_txq_get_gpu_handle(txq->eth_txq_cpu, &(txq->eth_txq_gpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_get_gpu_handle: %s", doca_error_get_descr(result));
		destroy_txq(txq);
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

/*
 * Launch GPUNetIO send wait on time sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_send_wait_time(struct sample_send_wait_cfg *sample_cfg)
{
	doca_error_t result;
	uint64_t *intervals_cpu = NULL;
	uint64_t *intervals_gpu = NULL;
	uint64_t time_seed;
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	struct txq_queue txq = {0};
	enum doca_eth_wait_on_time_type wait_on_time_mode;
	cudaStream_t stream;
	cudaError_t res_rt = cudaSuccess;

	result = init_doca_device(sample_cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_device returned %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = doca_eth_txq_cap_get_wait_on_time_offload_supported(doca_dev_as_devinfo(ddev), &wait_on_time_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Wait on time offload error, returned %s", doca_error_get_descr(result));
		goto exit;
	}

	/* Init and start port for eth */
	result = init_doca_flow();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_txq returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = start_doca_flow(ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function start_doca_flow returned %s", doca_error_get_descr(result));
		goto exit;
	}

	if (wait_on_time_mode == DOCA_ETH_WAIT_ON_TIME_TYPE_DPDK) {
		/*
		 * From CX7, tx_pp is not needed anymore.
		 */
		result = doca_dpdk_port_probe(ddev, "tx_pp=500,txq_inline_max=0,dv_flow_en=2");
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dpdk_port_probe returned %s", doca_error_get_descr(result));
			return result;
		}
	}

	DOCA_LOG_INFO("Wait on time supported mode: %s",
		      (wait_on_time_mode == DOCA_ETH_WAIT_ON_TIME_TYPE_DPDK) ? "DPDK" : "Native");

	result = doca_gpu_create(sample_cfg->gpu_pcie_addr, &gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = create_txq(&txq, gpu_dev, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_txq returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = create_tx_buf(&txq, NUM_PACKETS_X_BURST * NUM_BURST_SEND, PACKET_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_tx_buf returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = prepare_tx_buf(&txq, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function prepare_tx_buf returned %s", doca_error_get_descr(result));
		goto exit;
	}

	res_rt = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", res_rt);
		return DOCA_ERROR_DRIVER;
	}

	result = doca_gpu_mem_alloc(gpu_dev,
				    sizeof(uint64_t) * NUM_BURST_SEND,
				    GPU_PAGE_SIZE,
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&intervals_gpu,
				    (void **)&intervals_cpu);
	if (result != DOCA_SUCCESS || intervals_gpu == NULL || intervals_cpu == NULL) {
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit;
	}

	time_seed = get_ns() + DELTA_NS;
	for (int idx = 0; idx < NUM_BURST_SEND; idx++) {
		result = doca_eth_txq_calculate_timestamp(txq.eth_txq_cpu,
							  time_seed + (sample_cfg->time_interval_ns * idx),
							  &intervals_cpu[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get wait on time value for timestamp %ld, error %s",
				     time_seed + (sample_cfg->time_interval_ns * idx),
				     doca_error_get_descr(result));
			goto exit;
		}
	}

	DOCA_LOG_INFO("Launching CUDA kernel to send packets");
	kernel_send_wait_on_time(stream, &txq, intervals_gpu);
	cudaStreamSynchronize(stream);
	/*
	 * This is needed only because it's a synthetic example.
	 * Typical application works in a continuous loop so there is no need to wait.
	 */
	DOCA_LOG_INFO("Waiting 10 sec for %d packets to be sent", NUM_BURST_SEND * NUM_PACKETS_X_BURST);
	sleep(10);

exit:
	if (intervals_gpu)
		doca_gpu_mem_free(gpu_dev, intervals_gpu);

	result = destroy_tx_buf(&txq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_txq returned %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	result = destroy_txq(&txq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_txq returned %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Sample finished successfully");

	return DOCA_SUCCESS;
}
