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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_HOST_COMCH_CONTROL_CHANNEL_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_HOST_COMCH_CONTROL_CHANNEL_HPP_

#include <string>
#include <vector>

#include <doca_comch.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <remote_offload_common/control_message.hpp>

struct doca_gpu;

namespace remote_offload {
namespace orchestrator {

class comch_control_channel {
public:
	~comch_control_channel();
	comch_control_channel(doca_dev *dev, std::string const &comch_channel_name, doca_gpu *gpu);
	comch_control_channel(comch_control_channel const &) = delete;
	comch_control_channel(comch_control_channel &&) noexcept = delete;
	comch_control_channel &operator=(comch_control_channel const &) = delete;
	comch_control_channel &operator=(comch_control_channel &&) noexcept = delete;

	void poll_pe() noexcept;
	bool is_connected() noexcept;
	doca_comch_connection *get_connection() noexcept;
	doca_error_t send_control_message(void *bytes, uint32_t byte_count) noexcept;
	std::vector<uint8_t> get_pending_control_message() noexcept;

private:
	static void doca_comch_task_send_completion_cb(doca_comch_task_send *task,
						       doca_data task_user_data,
						       doca_data ctx_user_data) noexcept;

	static void doca_comch_task_send_error_cb(doca_comch_task_send *task,
						  doca_data task_user_data,
						  doca_data ctx_user_data) noexcept;

	static void doca_comch_event_msg_recv_cb(doca_comch_event_msg_recv *event,
						 uint8_t *recv_buffer,
						 uint32_t msg_len,
						 doca_comch_connection *comch_connection) noexcept;

	void cleanup() noexcept;

	/* Progress engine */
	doca_pe *m_pe;
	/* The comch client instance */
	doca_comch_client *m_comch_client;
	/* Connection to the comch_client */
	doca_comch_connection *m_comch_connection;
	/* Received messages */
	std::vector<std::vector<uint8_t>> m_rx_messages;
};

} // namespace orchestrator
} // namespace remote_offload

#endif /* APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_HOST_COMCH_CONTROL_CHANNEL_HPP_ */