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
#include <doca_dpdk.h>
#include <doca_log.h>
#include <rte_ethdev.h>

#include "gpunetio_common.h"

#include "common.h"

#define FLOW_NB_COUNTERS 524228 /* 1024 x 512 */
#define MBUF_NUM 8192
#define MBUF_SIZE 2048
#define CPU_TO_BE16(val) __builtin_bswap16(val)

struct doca_flow_port *df_port;
bool force_quit;

DOCA_LOG_REGISTER(SIMPLE_SEND : SAMPLE);

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
 * @dpdk_port_id [in]: DPDK Port id of the DOCA device
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
 * Destroy DOCA Ethernet Tx queue for GPU
 *
 * @txq [in]: DOCA Eth Rx queue handler
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

	if (txq->gpu_pkt_addr != NULL) {
		result = doca_gpu_mem_free(txq->gpu_dev, txq->gpu_pkt_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(result));
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
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_txq(struct txq_queue *txq,
			       struct doca_gpu *gpu_dev,
			       struct doca_dev *ddev,
			       uint32_t pkt_size,
			       uint32_t pkt_num)
{
	doca_error_t result;
	cudaError_t res_cuda;

	uint32_t buffer_size = 0;
	uint8_t *cpu_pkt_addr;

	if (txq == NULL || gpu_dev == NULL || ddev == NULL) {
		DOCA_LOG_ERR("Can't create UDP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	txq->gpu_dev = gpu_dev;
	txq->ddev = ddev;
	txq->port = df_port;
	txq->pkt_size = pkt_size;
	txq->cuda_threads = pkt_num;
	txq->inflight_sends = MAX_SQ_DESCR_NUM / 2;
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
		DOCA_LOG_ERR("Failed to set eth_txq l3 offloads: %s", doca_error_get_descr(result));
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

	result = doca_ctx_start(txq->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
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
				    GPU_PAGE_SIZE,
				    DOCA_GPU_MEM_TYPE_GPU,
				    &txq->gpu_pkt_addr,
				    NULL);
	if (result != DOCA_SUCCESS || txq->gpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit_error;
	}

	cpu_pkt_addr = (uint8_t *)calloc(txq->cuda_threads * pkt_size, sizeof(uint8_t));
	if (cpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Error in tx buf preparation, failed to allocate memory");
		goto exit_error;
	}

	struct ether_hdr *eth;

	for (uint32_t idx = 0; idx < txq->cuda_threads; idx++) {
		eth = (struct ether_hdr *)(cpu_pkt_addr + (idx * pkt_size));
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
	}

	res_cuda = cudaMemcpy(txq->gpu_pkt_addr, cpu_pkt_addr, buffer_size, cudaMemcpyDefault);
	free(cpu_pkt_addr);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Function CUDA Memcpy cqe_addr failed with %s", cudaGetErrorString(res_cuda));
		return DOCA_ERROR_DRIVER;
	}

	/* Map GPU memory buffer used to receive packets with DMABuf */
	result = doca_gpu_dmabuf_fd(txq->gpu_dev, txq->gpu_pkt_addr, buffer_size, &(txq->dmabuf_fd));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB) with nvidia-peermem mode",
			      txq->gpu_pkt_addr,
			      buffer_size);

		/* If failed, use nvidia-peermem legacy method */
		result = doca_mmap_set_memrange(txq->pkt_buff_mmap, txq->gpu_pkt_addr, buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	} else {
		DOCA_LOG_INFO("Mapping receive queue buffer (0x%p size %dB dmabuf fd %d) with dmabuf mode",
			      txq->gpu_pkt_addr,
			      buffer_size,
			      txq->dmabuf_fd);

		result = doca_mmap_set_dmabuf_memrange(txq->pkt_buff_mmap,
						       txq->dmabuf_fd,
						       txq->gpu_pkt_addr,
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

	result = doca_buf_arr_create(txq->cuda_threads, &txq->buf_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		goto exit_error;
	}

	result = doca_buf_arr_set_target_gpu(txq->buf_arr, txq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		goto exit_error;
	}

	result = doca_buf_arr_set_params(txq->buf_arr, txq->pkt_buff_mmap, txq->pkt_size, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		goto exit_error;
	}

	result = doca_buf_arr_start(txq->buf_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start buf: doca buf_arr internal error");
		goto exit_error;
	}

	result = doca_buf_arr_get_gpu_handle(txq->buf_arr, &(txq->buf_arr_gpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get buff_arr GPU handle: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	return DOCA_SUCCESS;

exit_error:
	destroy_txq(txq);
	return DOCA_ERROR_BAD_STATE;
}

/*
 * Launch GPUNetIO simple receive sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_simple_send(struct sample_simple_send_cfg *sample_cfg)
{
	doca_error_t result;
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	struct txq_queue txq = {0};
	cudaStream_t stream;
	cudaError_t res_rt = cudaSuccess;
	uint32_t *cpu_exit_condition;
	uint32_t *gpu_exit_condition;

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

	result = create_txq(&txq, gpu_dev, ddev, sample_cfg->pkt_size, sample_cfg->cuda_threads);
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
				    sizeof(uint32_t),
				    GPU_PAGE_SIZE,
				    DOCA_GPU_MEM_TYPE_GPU_CPU,
				    (void **)&gpu_exit_condition,
				    (void **)&cpu_exit_condition);
	if (result != DOCA_SUCCESS || gpu_exit_condition == NULL || cpu_exit_condition == NULL) {
		DOCA_LOG_ERR("Function doca_gpu_mem_alloc returned %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}
	cpu_exit_condition[0] = 0;

	DOCA_LOG_INFO("Launching CUDA kernel to receive packets");

	kernel_send_packets(stream, &txq, gpu_exit_condition);

	DOCA_LOG_INFO("Waiting for termination");
	/* This loop keeps busy main thread until force_quit is set to 1 (e.g. typing ctrl+c) */
	while (DOCA_GPUNETIO_VOLATILE(force_quit) == false)
		;
	DOCA_GPUNETIO_VOLATILE(*cpu_exit_condition) = 1;

	DOCA_LOG_INFO("Exiting from sample");

	cudaStreamSynchronize(stream);
exit:

	result = destroy_txq(&txq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function destroy_txq returned %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Sample finished successfully");

	return DOCA_SUCCESS;
}
