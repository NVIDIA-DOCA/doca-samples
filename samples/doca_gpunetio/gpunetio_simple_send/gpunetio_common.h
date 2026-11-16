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

#ifndef GPUNETIO_SIMPLE_SEND_COMMON_H_
#define GPUNETIO_SIMPLE_SEND_COMMON_H_

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_error.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_gpunetio.h>
#include <doca_gpunetio_eth_def.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_gpu_data_path.h>
#include <doca_buf_array.h>

#include "common.h"

#define MAX_PCI_ADDRESS_LEN 32U
#define ETHER_ADDR_LEN 6
#define MAX_SQ_DESCR_NUM 8192
#define SHARED_QP_BLOCKS 2
#define MAX_CUDA_BLOCK_THREADS 1024 /* CUDA limit of threads per block */
#define DEFAULT_CUDA_THREADS 512    /* Valid with and without shared QP */
#define DEFAULT_EXEC_SCOPE 2	    /* DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK, numeric to be printed in the help */
#define DEFAULT_CPU_PROXY 0	    /* CPU proxy mode disabled: the GPU rings the doorbell */
#define DEFAULT_SHARED_QP 0	    /* Shared QP mode disabled */
#define MAX_PKT_SIZE 9216	    /* Jumbo frame, the port MTU must allow it */
#define DEFAULT_SRC_IP "192.168.1.1"
#define DEFAULT_DST_IP "192.168.1.2"
#define UDP_SRC_PORT 10000
#define UDP_DST_PORT 5000

#ifndef ACCESS_ONCE_64b
#define ACCESS_ONCE_64b(x) (*(volatile uint64_t *)&(x))
#endif

#ifndef WRITE_ONCE_64b
#define WRITE_ONCE_64b(x, v) (ACCESS_ONCE_64b(x) = (v))
#endif

#define ALIGN_SIZE(size, align) size = ((size + (align)-1) / (align)) * (align);

struct ether_hdr {
	uint8_t d_addr_bytes[ETHER_ADDR_LEN]; /* Destination addr bytes in tx order */
	uint8_t s_addr_bytes[ETHER_ADDR_LEN]; /* Source addr bytes in tx order */
	uint16_t ether_type;		      /* Frame type */
} __attribute__((__packed__));

struct ipv4_hdr {
	uint8_t version_ihl;	  /* Version and header length */
	uint8_t type_of_service;  /* Type of service */
	uint16_t total_length;	  /* Length of packet */
	uint16_t packet_id;	  /* Packet ID */
	uint16_t fragment_offset; /* Fragmentation offset */
	uint8_t time_to_live;	  /* Time to live */
	uint8_t next_proto_id;	  /* Protocol ID */
	uint16_t hdr_checksum;	  /* Header checksum */
	uint32_t src_addr;	  /* Source address */
	uint32_t dst_addr;	  /* Destination address */
} __attribute__((__packed__));

struct udp_hdr {
	uint16_t src_port;    /* UDP source port */
	uint16_t dst_port;    /* UDP destination port */
	uint16_t dgram_len;   /* UDP datagram length */
	uint16_t dgram_cksum; /* UDP datagram checksum */
} __attribute__((__packed__));

/* Ethernet, IPv4 and UDP headers at the beginning of every packet */
struct eth_ip_udp_hdr {
	struct ether_hdr l2_hdr;
	struct ipv4_hdr l3_hdr;
	struct udp_hdr l4_hdr;
} __attribute__((__packed__));

#define MIN_PKT_SIZE ((int)sizeof(struct eth_ip_udp_hdr)) /* 42 bytes, the NIC pads to the Ethernet minimum */

/* Application configuration structure */
struct sample_simple_send_cfg {
	char gpu_pcie_addr[MAX_PCI_ADDRESS_LEN];       /* GPU PCIe address */
	char nic_pcie_addr[MAX_PCI_ADDRESS_LEN];       /* Network card PCIe address */
	size_t pkt_size;			       /* Packet size to send */
	uint32_t src_ip;			       /* IPv4 source address, network byte order */
	uint32_t dst_ip;			       /* IPv4 destination address, network byte order */
	uint32_t cuda_threads;			       /* Number of CUDA threads in CUDA send kernel */
	bool shared_qp;				       /* Shared QP mode enabled/disabled */
	uint32_t exec_scope;			       /* If shared QP mode is enabled, define the exec scope */
	enum doca_gpu_dev_eth_nic_handler nic_handler; /* Regular GPU DB ring or via CPU proxy */
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
	int dmabuf_fd;			 /* GPU memory dmabuf descriptor, -1 if not used */
	bool ctx_started;		 /* DOCA Ethernet send queue context started */
	size_t pkt_size;		 /* Packet size to send */
	uint32_t cuda_threads;		 /* Number of CUDA threads in CUDA send kernel */
	uint32_t src_ip;		 /* IPv4 source address, network byte order */
	uint32_t dst_ip;		 /* IPv4 destination address, network byte order */
};

/* CPU Proxy thread arguments */
struct cpu_proxy_args {
	struct doca_eth_txq *txq; /* DOCA Eth Txq handler to progress */
	uint64_t exit_flag;	  /* CPU thread exit flag */
};

/*
 * Launch GPUNetIO simple send sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_simple_send(struct sample_simple_send_cfg *sample_cfg);

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel to send packets, optionally in shared QP mode.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @txq [in]: DOCA Eth Tx queue to use to send packets
 * @gpu_exit_condition [in]: exit from CUDA kernel
 * @shared_qp [in]: enable shared qp mode
 * @exec_scope [in]: if shared qp is enabled, define the exec scope
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_send_packets(cudaStream_t stream,
				 struct txq_queue *txq,
				 uint32_t *gpu_exit_condition,
				 bool shared_qp,
				 enum doca_gpu_dev_eth_exec_scope exec_scope);

#ifdef __cplusplus
}
#endif
#endif
