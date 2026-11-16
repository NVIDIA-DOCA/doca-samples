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
#include <doca_gpunetio_eth_def.h>
#include <doca_eth_txq_gpu_data_path.h>

#include "common.h"

#define ETHER_ADDR_LEN 6
#define MAX_SQ_DESCR_NUM 8192
#define NUM_BURST_SEND 8
#define NUM_PACKETS_X_BURST DOCA_GPUNETIO_ETH_WARP_SIZE
#define PACKET_SIZE 1024
#define DELTA_NS 50000000 /* 50ms of delta before sending the first burst */

/* Application configuration structure */
struct sample_send_wait_cfg {
	char gpu_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* GPU PCIe address */
	char nic_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Network card PCIe address */
	uint32_t time_interval_ns;			/* Nanoseconds between bursts */
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
	uint32_t pkt_num;		 /* Number of packets in the packet buffer */
};

/* Wait on time value of every burst, passed by value to the CUDA kernel */
struct burst_wait_times {
	uint64_t wait_on_time[NUM_BURST_SEND]; /* Values returned by doca_eth_txq_calculate_timestamp() */
};

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

/*
 * Launch GPUNetIO send wait on time sample
 *
 * @sample_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_send_wait_time(struct sample_send_wait_cfg *sample_cfg);

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel to send packets with wait on time feature. The kernel queues all the bursts at once, the NIC
 * holds each one until its time, then the kernel waits for the completion of every burst.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @txq [in]: DOCA Eth Tx queue to use to send packets
 * @wait_times [in]: at which time each burst of packet must be sent
 * @bursts_done_gpu [in/out]: GPU pointer to the number of bursts sent without error. The caller sets it to 0, and
 * the CUDA kernel updates it after each burst
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_send_wait_on_time(cudaStream_t stream,
				      struct txq_queue *txq,
				      const struct burst_wait_times *wait_times,
				      uint32_t *bursts_done_gpu);

#ifdef __cplusplus
}
#endif
#endif
