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

#ifndef GPUNETIO_SEND_WAIT_TIME_COMMON_H_
#define GPUNETIO_SEND_WAIT_TIME_COMMON_H_

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_error.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_gpunetio.h>
#include <doca_eth_txq.h>
#include <doca_buf_array.h>
#include <doca_gpunetio_eth_def.h>
#include <doca_eth_txq_gpu_data_path.h>

#include "common.h"

#define MAX_PCI_ADDRESS_LEN 32U
#define ETHER_ADDR_LEN 6
#define MAX_SQ_DESCR_NUM 8192
#define SHARED_QP_BLOCKS 2
#define NUM_BURST_SEND 8
#define NUM_PACKETS_X_BURST DOCA_GPUNETIO_ETH_WARP_SIZE
#define PACKET_SIZE 1024
#define DELTA_NS 50000000 /* 50ms of delta before sending the first burst */

/* Application configuration structure */
struct sample_send_wait_cfg {
	char gpu_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* GPU PCIe address */
	char nic_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Network card PCIe address */
	uint32_t time_interval_ns;			/* Nanoseconds between sends */
	uint32_t exec_scope;				/* If shared QP mode is enabled, define the exec scope */
};

/* Send queues objects */
struct txq_queue {
	struct doca_gpu *gpu_dev; /* GPUNetio handler associated to queues*/
	struct doca_dev *ddev;	  /* DOCA device handler associated to queues */

	struct doca_ctx *eth_txq_ctx;	      /* DOCA Ethernet send queue context */
	struct doca_eth_txq *eth_txq_cpu;     /* DOCA Ethernet send queue CPU handler */
	struct doca_gpu_eth_txq *eth_txq_gpu; /* DOCA Ethernet send queue GPU handler */

	struct doca_mmap *pkt_buff_mmap; /* DOCA mmap to send packet with DOCA Ethernet queue */
	uint32_t pkt_buff_mkey;		 /* DOCA mmap memory key */
	uint8_t *pkt_buff_addr;		 /* DOCA mmap GPU memory address */
	int dmabuf_fd;			 /* GPU memory dmabuf descriptor */
	struct doca_flow_port *port;	 /* DOCA Flow port */
	size_t pkt_size;		 /* Packet size to send */
	uint32_t cuda_threads;		 /* Number of CUDA threads in CUDA send kernel */
};

struct ether_hdr {
	uint8_t d_addr_bytes[ETHER_ADDR_LEN]; /* Destination addr bytes in tx order */
	uint8_t s_addr_bytes[ETHER_ADDR_LEN]; /* Source addr bytes in tx order */
	uint16_t ether_type;		      /* Frame type */
} __attribute__((__packed__));

/*
 * Launch GPUNetIO send wait on time sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_send_wait_time(struct sample_send_wait_cfg *sample_cfg);

#if __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel to send packets with wait on time feature.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @txq [in]: DOCA Eth Tx queue to use to send packets
 * @intervals_gpu [in]: at which time each burst of packet must be sent
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_send_wait_on_time(cudaStream_t stream, struct txq_queue *txq, uint64_t *intervals_gpu);

#if __cplusplus
}
#endif
#endif
