/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef DPA_VERBS_INITIATOR_TARGET_COMMON_DEFS_H_
#define DPA_VERBS_INITIATOR_TARGET_COMMON_DEFS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Verbs Sample Buffer Start Value
 */
#define VERBS_SAMPLE_LOCAL_BUF_START_VALUE (0x9)
/**
 * Verbs Sample Buffer End Value
 */
#define VERBS_SAMPLE_LOCAL_BUF_END_VALUE (0xF)

#if __cplusplus
extern "C" {
#endif

/**
 * Sample's DPA Thread arguments struct
 */
struct dpa_thread_arg {
	doca_dpa_dev_t dpa_ctx_handle;
	doca_dpa_dev_completion_t dpa_comp_handle;
	doca_dpa_dev_verbs_qp_t dpa_verbs_qp_handle;
	doca_dpa_dev_sync_event_t comp_sync_event_handle;
	int comp_sync_event_val;
	volatile doca_dpa_dev_uintptr_t local_dpa_buff_addr;
	doca_dpa_dev_uintptr_t remote_dpa_buff_addr;
	doca_dpa_dev_mmap_t local_dpa_buff_addr_mmap_handle;
	doca_dpa_dev_mmap_t remote_dpa_buff_addr_mmap_handle;
	uint32_t local_dpa_buff_addr_length;
	int return_status;
} __dpa_global__;

#if __cplusplus
}
#endif

#endif /* DPA_VERBS_INITIATOR_TARGET_COMMON_DEFS_H_ */
