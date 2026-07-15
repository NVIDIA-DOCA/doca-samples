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

#include <client/thread.hpp>

#include <doca_log.h>

#include <remote_offload_common/control_message.hpp>
#include <remote_offload_common/runtime_error.hpp>

DOCA_LOG_REGISTER(client::thread);

namespace remote_offload {
namespace client {

thread::~thread()
{
	join();
}

thread::thread()
	: m_expected_response{nullptr},
	  m_request_message{},
	  m_response_buffer{},
	  m_shared_thread_control{nullptr},
	  m_socket{},
	  m_rx_bytes_so_far{0},
	  m_sent_request_count{0},
	  m_stats{},
	  m_running{false},
	  m_thread{}
{
}

void thread::launch(uint32_t max_concurrent_messages,
		    uint32_t num_iterations,
		    std::string const &server_addr,
		    uint16_t server_port,
		    std::string const &request_message,
		    std::string const *expected_response,
		    remote_offload::thread_control *shared_thread_control)
{
	m_running = true;

	m_expected_response = expected_response;

	control::message_header request_hdr{
		static_cast<uint32_t>(sizeof(control::message_header) + sizeof(uint32_t) + request_message.size()),
		control::message_id::client_request};
	control::client_data request_data{request_message};

	m_request_message.resize(request_hdr.wire_size);
	control::encode(control::encode(m_request_message.data(), request_hdr), request_data);

	m_response_buffer.resize(m_request_message.size());
	m_shared_thread_control = shared_thread_control;

	m_stats.total_request_byte_count = request_message.size();
	m_stats.total_response_byte_count = expected_response->size();

	m_thread = std::thread{thread_proc_wrapper,
			       this,
			       max_concurrent_messages,
			       num_iterations,
			       server_addr,
			       server_port};
}

bool thread::is_running() const noexcept
{
	return m_running;
}

void thread::join() noexcept
{
	if (m_shared_thread_control != nullptr) {
		m_shared_thread_control->quit_flag = true;
	}

	if (m_thread.joinable()) {
		try {
			m_thread.join();
		} catch (std::exception const &ex) {
			DOCA_LOG_ERR("Failed to join thread. Error: %s", ex.what());
		}
	}
}

client::stats const &thread::get_stats() const noexcept
{
	return m_stats;
}

void thread::thread_proc_wrapper(thread *self,
				 uint32_t max_concurrent_messages,
				 uint32_t num_iterations,
				 std::string server_addr,
				 uint16_t server_port) noexcept
{
	doca_error_t result;
	try {
		result = self->thread_proc(max_concurrent_messages, num_iterations, server_addr, server_port);
	} catch (remote_offload::runtime_error const &ex) {
		result = ex.get_doca_error();
		DOCA_LOG_ERR("Thread %p Failed. Error: %s : %s",
			     self,
			     doca_error_get_name(ex.get_doca_error()),
			     ex.what());
	} catch (std::exception const &ex) {
		result = DOCA_ERROR_UNEXPECTED;
		DOCA_LOG_ERR("Thread %p Failed. Unexpected exception: %s", self, ex.what());
	}

	if (result == DOCA_SUCCESS) {
		DOCA_LOG_INFO("Thread %p completed successfully", self);
	} else {
		DOCA_LOG_INFO("Thread %p completed with error: %s", self, doca_error_get_name(result));
		self->m_shared_thread_control->quit_flag = true;
		self->m_shared_thread_control->error_flag = true;
	}
	self->m_running = false;
}

doca_error_t thread::thread_proc(uint32_t max_concurrent_messages,
				 uint32_t num_iterations,
				 std::string server_addr,
				 uint16_t server_port)
{
	connect_to_server(server_addr, server_port);

	DOCA_LOG_INFO("Thread %p connected to: %s:%u", this, server_addr.c_str(), server_port);

	doca_error_t result;

	auto const start_time = std::chrono::steady_clock::now();
	for (uint32_t ii = 0; ii != std::min(max_concurrent_messages, num_iterations); ++ii) {
		result = submit_request();
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Thread %p failed to submit initial request: %s",
				     this,
				     doca_error_get_name(result));
			return result;
		}
	}

	result = tcp_thread_proc(num_iterations);
	if (result != DOCA_SUCCESS) {
		return result;
	}

	auto const end_time = std::chrono::steady_clock::now();
	m_stats.execution_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
	m_stats.total_request_byte_count *= m_stats.total_messages;
	m_stats.total_response_byte_count *= m_stats.total_messages;

	return DOCA_SUCCESS;
}

void thread::connect_to_server(std::string const &server_addr, uint16_t server_port)
{
start_connecting:
	DOCA_LOG_INFO("Connecting to server: %s:%u", server_addr.c_str(), server_port);
	m_socket.connect(server_addr, server_port);

	for (;;) {
		if (m_shared_thread_control->quit_flag)
			throw remote_offload::runtime_error{DOCA_ERROR_CONNECTION_ABORTED, "Connect to server aborted"};

		std::this_thread::yield();

		m_socket.poll();
		auto const status = m_socket.get_connection_status();

		if (status == tcp_socket::connection_status::connected) {
			DOCA_LOG_INFO("Connected to %s:%u", server_addr.c_str(), server_port);
			return;
		}

		if (status == tcp_socket::connection_status::refused) {
			DOCA_LOG_INFO("Connection refused - trying again");
			m_socket = {};
			std::this_thread::sleep_for(std::chrono::milliseconds{500});
			goto start_connecting;
		}

		if (status == tcp_socket::connection_status::failed) {
			throw remote_offload::runtime_error{DOCA_ERROR_CONNECTION_ABORTED,
							    "Connection to server failed"};
		}
	}
}

doca_error_t thread::submit_request() noexcept
{
	size_t written = 0;
	do {
		m_socket.poll();
		if (!m_socket.can_write()) {
			DOCA_LOG_ERR("Thread %p Unable to write to socket", this);
			return DOCA_ERROR_IO_FAILED;
		}

		auto const bytes_this_write = m_socket.write(m_request_message.data(), m_request_message.size());
		if (bytes_this_write < 0) {
			DOCA_LOG_ERR("Thread %p Failed to write to socket", this);
			return DOCA_ERROR_IO_FAILED;
		}

		written += bytes_this_write;

		if (m_shared_thread_control->quit_flag) {
			DOCA_LOG_ERR("Thread %p Aborted write after sending only %zu of %zu bytes",
				     this,
				     written,
				     m_request_message.size());
			return DOCA_ERROR_IO_FAILED;
		}
	} while (written != m_request_message.size());

	++m_sent_request_count;

	return DOCA_SUCCESS;
}

doca_error_t thread::read_and_process_response() noexcept
{
	m_socket.poll();
	if (!m_socket.can_read()) {
		return DOCA_ERROR_AGAIN;
	}

	auto const read = m_socket.read(m_response_buffer.data() + m_rx_bytes_so_far,
					m_response_buffer.size() - m_rx_bytes_so_far);
	if (read == 0) {
		DOCA_LOG_ERR("Thread %p Server unexpectedly closed socket", this);
		return DOCA_ERROR_IO_FAILED;
	}

	if (read < 0) {
		DOCA_LOG_ERR("Thread %p Failed to read from socket", this);
		return DOCA_ERROR_IO_FAILED;
	}

	m_rx_bytes_so_far += read;
	if (m_rx_bytes_so_far < sizeof(control::message_header)) {
		return DOCA_ERROR_AGAIN;
	}

	control::message_header hdr;
	control::decode(m_response_buffer.data(), hdr);

	if (m_rx_bytes_so_far < hdr.wire_size) {
		return DOCA_ERROR_AGAIN;
	}

	m_rx_bytes_so_far -= hdr.wire_size;

	auto constexpr response_offset = sizeof(control::message_header) + sizeof(uint32_t);
	auto const response_len = hdr.wire_size - response_offset;

	if (response_len != m_expected_response->size()) {
		DOCA_LOG_ERR("Thread %p Received invalid response: expected %zu bytes, but got %zu bytes",
			     this,
			     m_expected_response->size(),
			     response_len);
		return DOCA_ERROR_INVALID_VALUE;
	}

	size_t diff_position =
		::memcmp(m_response_buffer.data() + response_offset, m_expected_response->data(), response_len);
	if (diff_position != 0) {
		DOCA_LOG_ERR("Thread %p Received invalid response: different at offset: %zu. Expected[%c] got [%c]",
			     this,
			     diff_position,
			     (*m_expected_response)[diff_position],
			     m_response_buffer[response_offset + diff_position]);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

doca_error_t thread::tcp_thread_proc(uint32_t num_iterations) noexcept
{
	while (m_stats.total_messages != num_iterations) {
		auto const result = read_and_process_response();
		if (result == DOCA_SUCCESS) {
			++(m_stats.total_messages);
			if (m_sent_request_count != num_iterations) {
				submit_request();
			}
		} else if (result != DOCA_ERROR_AGAIN) {
			return result;
		}
	}

	return DOCA_SUCCESS;
}

} /* namespace client */
} /* namespace remote_offload */
