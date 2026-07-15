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

#ifndef DOCA_GPU_PACKET_PROCESSING_TCP_SESSION_H
#define DOCA_GPU_PACKET_PROCESSING_TCP_SESSION_H

#include <stdint.h>
#include <linux/types.h>
#include <rte_hash.h>

#define TCP_SESSION_MAX_ENTRIES 4096

/* TCP session key */
struct tcp_session_key {
	__be32 src_addr; /* TCP session key src addr */
	__be32 dst_addr; /* TCP session key dst addr */
	__be16 src_port; /* TCP session key src port */
	__be16 dst_port; /* TCP session key dst port */
};

/* TCP session entry */
struct tcp_session_entry {
	struct tcp_session_key key;	   /* TCP session key */
	struct doca_flow_pipe_entry *flow; /* TCP session key DOCA flow entry */
};

/* TCP session params */
extern struct rte_hash_parameters tcp_session_ht_params;
/* TCP session table */
extern struct rte_hash *tcp_session_table;

/*
 * TCP session table CRC
 *
 * @data [in]: Network card PCIe address
 * @data_len [in]: DOCA device
 * @init_val [in]: DPDK port id associated with the DOCA device
 * @return: 0 on success and 1 otherwise
 */
uint32_t tcp_session_table_crc(const void *data, uint32_t data_len, uint32_t init_val);

/*
 * TCP session table CRC
 *
 * @key [in]: TCP session table key
 * @return: ptr on success and NULL otherwise
 */
struct tcp_session_entry *tcp_session_table_find(struct tcp_session_key *key);

/*
 * TCP session table CRC
 *
 * @entry [in]: TCP session table key
 */
void tcp_session_table_delete(struct tcp_session_entry *entry);

/*
 * Establish new TCP session
 *
 * @key [in]: TCP session table key
 * @return: ptr on success and NULL otherwise
 */
struct tcp_session_entry *tcp_session_table_new(struct tcp_session_key *key);

#endif
