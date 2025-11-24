/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef DPA_PING_PONG_COMMON_DEFS_H_
#define DPA_PING_PONG_COMMON_DEFS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Number of expected receive completions on each DPA Thread
 */
#define EXPECTED_NUM_RECEIVES (100)

/**
 * @brief DPA Thread local storage struct
 */
struct dpa_ping_pong_tls {
	uint64_t received_values_arr_ptr; /**< Device memory pointer for receive values array */
	uint32_t num_receives;		  /**< Number of receive completions */
	uint32_t is_ping_thread;	  /**< Set if it's ping thread, zero if it's pong thread */
} __attribute__((__packed__, aligned(8)));

/**
 * @brief DPA Thread device argument struct
 */
struct dpa_thread_arg {
	doca_dpa_dev_t dpa_ctx_handle;	 /**< Extended DPA context handle */
	uint64_t dpa_comp_handle;	 /**< Handle of DPA Completion Context which is attached to DPA Thread */
	uint64_t rdma_handle;		 /**< Handle of RDMA context which is attached to  DPA Completion Context */
	uint64_t recv_addr;		 /**< Receive buffer address (DPA heap memory) */
	uint64_t send_addr;		 /**< Send buffer address (DPA heap memory) */
	uint32_t recv_addr_mmap_handle;	 /**< Receive buffer DOCA Mmap handle */
	uint32_t send_addr_mmap_handle;	 /**< Send buffer DOCA Mmap handle */
	uint64_t recv_addr_length;	 /**< Receive buffer length */
	uint64_t send_addr_length;	 /**< Send buffer length */
	uint64_t comp_sync_event_handle; /**< Completion DOCA Sync Event handle */
	uint64_t comp_sync_event_val;	 /**< Completion DOCA Sync Event value to be set when application is finished */
} __dpa_global__;

#ifdef __cplusplus
}
#endif

#endif /* DPA_PING_PONG_COMMON_DEFS_H_ */
