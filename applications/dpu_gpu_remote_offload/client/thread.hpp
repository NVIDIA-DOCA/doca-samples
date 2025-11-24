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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_CLIENT_THREAD_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_CLIENT_THREAD_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <doca_error.h>

#include <remote_offload_common/thread_control.hpp>
#include <remote_offload_common/tcp_socket.hpp>

#include <client/configuration.hpp>
#include <client/stats.hpp>

namespace remote_offload {
namespace client {

class thread {
public:
	~thread();
	thread();
	thread(thread const &) = delete;
	thread(thread &&) noexcept = delete;
	thread &operator=(thread const &) = delete;
	thread &operator=(thread &&) noexcept = delete;

	void launch(uint32_t max_concurrent_messages,
		    uint32_t num_iterations,
		    std::string const &server_addr,
		    uint16_t server_port,
		    std::string const &request_message,
		    std::string const *expected_response,
		    remote_offload::thread_control *shared_thread_control);

	bool is_running() const noexcept;
	void join() noexcept;

	client::stats const &get_stats() const noexcept;

private:
	static void thread_proc_wrapper(thread *self,
					uint32_t max_concurrent_messages,
					uint32_t num_iterations,
					std::string server_addr,
					uint16_t server_port) noexcept;

	doca_error_t thread_proc(uint32_t max_concurrent_messages,
				 uint32_t num_iterations,
				 std::string server_addr,
				 uint16_t server_port);

	void connect_to_server(std::string const &server_addr, uint16_t server_portr);
	doca_error_t submit_request() noexcept;
	doca_error_t read_and_process_response() noexcept;
	doca_error_t tcp_thread_proc(uint32_t num_iterations) noexcept;

	std::string const *m_expected_response;
	std::vector<uint8_t> m_request_message;
	std::vector<uint8_t> m_response_buffer;
	remote_offload::thread_control *m_shared_thread_control;
	remote_offload::tcp_socket m_socket;
	size_t m_rx_bytes_so_far;
	uint32_t m_sent_request_count;
	client::stats m_stats;
	std::atomic_bool m_running;
	std::thread m_thread;
};

} /* namespace client */
} /* namespace remote_offload */

#endif // APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_CLIENT_THREAD_HPP_
