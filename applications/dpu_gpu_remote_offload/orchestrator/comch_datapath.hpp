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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_HOST_COMCH_DATAPATH_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_HOST_COMCH_DATAPATH_HPP_

#include <vector>

#include <doca_buf_array.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <orchestrator/configuration.hpp>
#include <orchestrator/gpu_processing.h>

struct doca_gpu;

namespace remote_offload {
namespace orchestrator {

class comch_datapath {
public:
	~comch_datapath();
	comch_datapath(doca_dev *dev, doca_gpu *gpu, uint32_t num_gpu_consumers);
	comch_datapath(comch_datapath const &) = delete;
	comch_datapath(comch_datapath &&) noexcept = delete;
	comch_datapath &operator=(comch_datapath const &) = delete;
	comch_datapath &operator=(comch_datapath &&) noexcept = delete;

	uint32_t register_remote_consumer(uint32_t remote_consumer_id,
					  uint32_t max_concurrent_messages,
					  uint32_t max_message_length,
					  doca_comch_connection *connection);

	void poll_pe() noexcept;

	bool are_all_contexts_running() noexcept;

	gpu_thread_data *get_gpu_thread_data();

	void cleanup() noexcept;

private:
	struct thread_data {
		~thread_data();
		thread_data(doca_dev *dev,
			    doca_gpu *gpu,
			    doca_pe *pe,
			    uint32_t max_concurrent_messages,
			    uint32_t max_message_length,
			    uint32_t remote_consumer_id,
			    uint32_t local_consumer_id,
			    doca_comch_connection *connection);
		thread_data(thread_data const &) = delete;
		thread_data(thread_data &&) noexcept = delete;
		thread_data &operator=(thread_data const &) = delete;
		thread_data &operator=(thread_data &&) noexcept = delete;

		doca_comch_consumer *m_consumer;
		doca_comch_gpu_consumer *m_gpu_consumer;

		doca_comch_producer *m_producer;
		doca_comch_gpu_producer *m_gpu_producer;

		uint8_t *m_io_memory;
		doca_mmap *m_io_mmap;

		doca_buf_arr *m_io_buf_arr;
		doca_gpu_buf_arr *m_gpu_io_buf_arr;

		inflight_msg_data *m_inflight_messages;
		uint32_t m_inflight_messages_mask;

		uint32_t m_remote_consumer_id;
		uint32_t m_local_consumer_id;

		doca_gpu *m_gpu;
	};

	bool all_producers_consumers_idle() noexcept;

	doca_pe *m_pe;

	doca_dev *m_dev;
	doca_gpu *m_gpu;

	uint32_t m_next_local_consumer_id;
	uint32_t m_num_gpu_consumers;

	std::vector<thread_data *> m_thread_data;
	gpu_thread_data *m_gpu_thread_data;
};

} // namespace orchestrator
} // namespace remote_offload

#endif // APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_HOST_COMCH_DATAPATH_HPP_
