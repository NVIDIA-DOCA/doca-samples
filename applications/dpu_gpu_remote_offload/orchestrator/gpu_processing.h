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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_ORCHESTRATOR_GPU_PROCESSING_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_ORCHESTRATOR_GPU_PROCESSING_HPP_

#include <doca_buf_array.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_error.h>

struct inflight_msg_data {
	uint32_t mkey;
	uintptr_t addr;
};

struct gpu_thread_data {
	doca_comch_gpu_consumer *consumer;
	doca_comch_gpu_producer *producer;
	doca_gpu_buf_arr *buf_arr;
	inflight_msg_data *inflight_messages;
	uint32_t inflight_messages_mask;
	uint32_t remote_consumer_id;
	uint32_t local_consumer_id;
};

#ifdef __cplusplus
extern "C" {
#endif

doca_error_t start_gpu_processing(uint32_t num_threads,
				  bool *gpu_stop_flag,
				  gpu_thread_data *thread_data,
				  const uint32_t max_message_size,
				  const uint32_t max_concurrent_messages);

#ifdef __cplusplus
}
#endif

#endif // APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_ORCHESTRATOR_GPU_PROCESSING_HPP_
