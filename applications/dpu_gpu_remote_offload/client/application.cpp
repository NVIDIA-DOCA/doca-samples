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

#include <client/application.hpp>

#include <doca_error.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(client::application);

namespace remote_offload {
namespace client {

application::~application()
{
	stop();
}

application::application(configuration const &cfg) : m_cfg{cfg}, m_thread_control{}, m_threads{m_cfg.thread_count}
{
}

void application::start()
{
	for (auto &thread : m_threads) {
		thread.launch(m_cfg.max_concurrent_messages,
			      m_cfg.iteration_count,
			      m_cfg.server_ip_address,
			      m_cfg.server_ip_port,
			      m_cfg.message_string,
			      &m_cfg.expected_response,
			      &m_thread_control);
	}
}

bool application::is_running() noexcept
{
	for (auto &thread : m_threads) {
		if (thread.is_running())
			return true;
	}

	return false;
}

void application::stop() noexcept
{
	m_thread_control.quit_flag = true;
	for (auto &thread : m_threads) {
		thread.join();
	}
}

bool application::failed() noexcept
{
	return m_thread_control.error_flag;
}

client::stats application::collect_stats() noexcept
{
	client::stats total_stats{};

	for (auto &thread : m_threads) {
		auto const &thread_stats = thread.get_stats();
		total_stats.execution_time += thread_stats.execution_time;
		total_stats.total_request_byte_count += thread_stats.total_request_byte_count;
		total_stats.total_response_byte_count += thread_stats.total_response_byte_count;
		total_stats.total_messages += thread_stats.total_messages;
	}

	if (m_threads.size() != 0) {
		total_stats.execution_time /= m_threads.size();
	}

	return total_stats;
}

} /* namespace client */
} /* namespace remote_offload */
