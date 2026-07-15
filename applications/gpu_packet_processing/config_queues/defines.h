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

#ifndef DOCA_GPU_PACKET_PROCESSING_DEF_H
#define DOCA_GPU_PACKET_PROCESSING_DEF_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <utils.h>
#include <signal.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_version.h>
#include <doca_log.h>
#include <doca_error.h>
#include <doca_gpunetio.h>
#include <doca_dev.h>
#include <doca_eth_rxq.h>
#include <doca_eth_txq.h>
#include <doca_mmap.h>
#include <doca_argp.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_pe.h>
#include <doca_gpunetio_eth_def.h>

/* GPU page size */
#define WARP_SIZE 32
#define WARP_FULL_MASK 0xFFFFFFFF
#define MAX_PORT_STR_LEN 128 /* Maximal length of port name */
#define MAX_QUEUES 4
#define MAX_QUEUES_ICMP 1
#define QUEUE_ID_UDP_0 (MAX_QUEUES * 0)
#define QUEUE_ID_TCP_0 (MAX_QUEUES * 1)
#define QUEUE_ID_HTTP_0 (MAX_QUEUES * 2)
#define QUEUE_ID_TCP_CPU_0 (MAX_QUEUES * 3)
#define QUEUE_ID_ICMP_0 (MAX_QUEUES * 4)
#define MAX_PKT_NUM 65536
#define MAX_PKT_SIZE 8192
#define MAX_RX_NUM_PKTS 4096
#define MAX_RX_TIMEOUT_NS 1000000 /* 1ms */
#define MAX_PKT_NUM_ICMP 16384
#define MAX_PKT_SIZE_ICMP 512
#define MAX_RX_NUM_PKTS_ICMP 64
#define MAX_RX_TIMEOUT_NS_ICMP 50000 /* 50us */
#define MAX_RX_NUM_PKTS_HTTP 64
#define MAX_RX_TIMEOUT_NS_HTTP 50000 /* 50us */
#define MAX_SQ_DESCR_NUM 4096
#define SEMAPHORES_PER_QUEUE 1024
#define CUDA_THREADS 512
#define ETHER_ADDR_LEN 6
#define BYTE_SWAP16(v) ((((uint16_t)(v)&UINT16_C(0x00ff)) << 8) | (((uint16_t)(v)&UINT16_C(0xff00)) >> 8))

#define BYTE_SWAP32(x) \
	((((x)&0xff000000) >> 24) | (((x)&0x00ff0000) >> 8) | (((x)&0x0000ff00) << 8) | (((x)&0x000000ff) << 24))

/* Each thread in the HTTP server CUDA kernel warp has it's own subset of 32 buffers */
#define TX_BUF_NUM 1024 /* 32 x 32 */
#define TX_BUF_MAX_SZ 512
#define FLOW_NB_COUNTERS 524228 /* 1024 x 512 */
/* HTTP page type to send as response to HTTP GET request */
enum http_page_get {
	HTTP_GET_INDEX = 0, /* HTML index page */
	HTTP_GET_CONTACTS,  /* HTML contact page */
	HTTP_GET_NOT_FOUND  /* HTML not found page */
};

#define ALIGN_SIZE(size, align) size = ((size + (align)-1) / (align)) * (align);

#endif /* DOCA_GPU_PACKET_PROCESSING_DEF_H */
