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
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_bitfield.h>

#include "../gpunetio_common.h"

#define FLOW_NB_COUNTERS 524228 /* 1024 x 512 */
#define MBUF_NUM 8192
#define MBUF_SIZE 2048
#define QUEUE_ID 0
#define CPU_TO_BE16(val) __builtin_bswap16(val)

struct doca_flow_port *df_port;

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME : SAMPLE);

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
doca_error_t init_doca_device(char *nic_pcie_addr, struct doca_dev **ddev)
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
 * Destroy DOCA Ethernet Tx queue for GPU
 *
 * @txq [in]: DOCA Eth Tx queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_txq(struct txq_queue *txq)
{
	doca_error_t result;

	if (txq == NULL) {
		DOCA_LOG_ERR("Can't destroy UDP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Destroying Txq");

	if (txq->eth_txq_ctx != NULL) {
		result = doca_ctx_stop(txq->eth_txq_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (txq->eth_txq_cpu != NULL) {
		result = doca_eth_txq_destroy(txq->eth_txq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_destroy: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (df_port != NULL) {
		result = doca_flow_port_stop(df_port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (txq->pkt_buff_mmap != NULL) {
		result = doca_mmap_destroy(txq->pkt_buff_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (txq->pkt_buff_addr != NULL) {
		result = doca_gpu_mem_free(txq->gpu_dev, txq->pkt_buff_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	result = doca_dev_close(txq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Eth dev: %s", doca_error_get_descr(result));
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
	doca_error_t result;
	cudaError_t res_cuda;
	uint32_t buffer_size = 0;
	char *cpu_pkt_addr;
	enum doca_eth_wait_on_time_type wait_on_time_mode;
	const char *payload = "Sent from DOCA GPUNetIO";
	char *pkt;

	if (txq == NULL || gpu_dev == NULL || ddev == NULL || pkt_size == 0 || pkt_num == 0) {
		DOCA_LOG_ERR("Can't create TXQ queue, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	txq->gpu_dev = gpu_dev;
	txq->ddev = ddev;
	txq->port = df_port;
	txq->pkt_size = pkt_size;
	txq->cuda_threads = pkt_num;
	buffer_size = txq->cuda_threads * pkt_size;

	DOCA_LOG_INFO("Creating Sample Eth Txq");

	result = doca_eth_txq_create(txq->ddev, MAX_SQ_DESCR_NUM, &(txq->eth_txq_cpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_create: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_txq_set_l3_chksum_offload(txq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_txq_set_l4_chksum_offload(txq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set eth_txq l4 offloads: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Application can check Txq completions on the GPU. By default, it can be done by CPU. */
	result = doca_eth_txq_gpu_set_completion_on_gpu(txq->eth_txq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_gpu_set_completion_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	txq->eth_txq_ctx = doca_eth_txq_as_doca_ctx(txq->eth_txq_cpu);
	if (txq->eth_txq_ctx == NULL) {
		DOCA_LOG_ERR("Failed doca_eth_txq_as_doca_ctx: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_ctx_set_datapath_on_gpu(txq->eth_txq_ctx, txq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_txq_cap_get_wait_on_time_offload_supported(doca_dev_as_devinfo(ddev), &wait_on_time_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Wait on time offload error, returned %s", doca_error_get_descr(result));
		goto exit_error;
	}
	/*
	 * Feature is now only supported from CX7 and newer NICs. CX6Dx support is deprecated.
	 */
	DOCA_LOG_INFO("Wait on time supported mode: %s",
		      (wait_on_time_mode == DOCA_ETH_WAIT_ON_TIME_TYPE_NONE) ? "None" : "Native");

	if (wait_on_time_mode == DOCA_ETH_WAIT_ON_TIME_TYPE_NATIVE) {
		result = doca_eth_txq_set_wait_on_time_offload(txq->eth_txq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_set_wait_on_time_offload: %s", doca_error_get_descr(result));
			goto exit_error;
		}
	} else {
		DOCA_LOG_ERR(
			"Wait on time feature not supported by the network card or attributes ACCURATE_TX_SCHEDULER and REAL_TIME_CLOCK_ENABLE are not enabled.");
		goto exit_error;
	}

	result = doca_ctx_start(txq->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
		goto exit_error;
	}

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
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit_error;
	}

	cpu_pkt_addr = (char *)calloc(txq->cuda_threads * pkt_size, sizeof(char));
	if (cpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Error in tx buf preparation, failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto exit_error;
	}

	struct ether_hdr *eth;

	for (uint32_t idx = 0; idx < txq->cuda_threads; idx++) {
		pkt = cpu_pkt_addr + (idx * pkt_size);
		eth = (struct ether_hdr *)pkt;
		eth->d_addr_bytes[0] = 0x10;
		eth->d_addr_bytes[1] = 0x11;
		eth->d_addr_bytes[2] = 0x12;
		eth->d_addr_bytes[3] = 0x13;
		eth->d_addr_bytes[4] = 0x14;
		eth->d_addr_bytes[5] = 0x15;

		eth->s_addr_bytes[0] = 0x20;
		eth->s_addr_bytes[1] = 0x21;
		eth->s_addr_bytes[2] = 0x22;
		eth->s_addr_bytes[3] = 0x23;
		eth->s_addr_bytes[4] = 0x24;
		eth->s_addr_bytes[5] = 0x25;

		eth->ether_type = CPU_TO_BE16(0x0800);

		/* Assuming no TCP flags needed */
		pkt = pkt + sizeof(struct ether_hdr);
		sprintf(pkt, "%s", payload);
		pkt = pkt + strlen(payload);
		sprintf(pkt, " %d", idx);
	}

	res_cuda = cudaMemcpy(txq->pkt_buff_addr, cpu_pkt_addr, buffer_size, cudaMemcpyDefault);
	free(cpu_pkt_addr);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Function CUDA Memcpy cqe_addr failed with %s", cudaGetErrorString(res_cuda));
		result = DOCA_ERROR_DRIVER;
		goto exit_error;
	}

	/* Map GPU memory buffer used to send packets with DMABuf */
	result = doca_gpu_dmabuf_fd(txq->gpu_dev, txq->pkt_buff_addr, buffer_size, &(txq->dmabuf_fd));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Mapping send queue buffer (0x%p size %dB) with nvidia-peermem mode",
			      txq->pkt_buff_addr,
			      buffer_size);

		/* If failed, use nvidia-peermem legacy method */
		result = doca_mmap_set_memrange(txq->pkt_buff_mmap, txq->pkt_buff_addr, buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	} else {
		DOCA_LOG_INFO("Mapping send queue buffer (0x%p size %dB dmabuf fd %d) with dmabuf mode",
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
	destroy_txq(txq);
	return DOCA_ERROR_BAD_STATE;
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
	cudaStream_t stream;
	cudaError_t res_rt = cudaSuccess;

	result = init_doca_device(sample_cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_device returned %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	/* Init and start port for eth */
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

	res_rt = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", res_rt);
		return DOCA_ERROR_DRIVER;
	}

	result = doca_gpu_mem_alloc(gpu_dev,
				    sizeof(uint64_t) * NUM_BURST_SEND,
				    get_host_page_size(),
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

	result = destroy_txq(&txq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function destoy_txq returned %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Sample finished successfully");

	return DOCA_SUCCESS;
}
