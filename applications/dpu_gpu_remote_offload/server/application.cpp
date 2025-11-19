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

#include <server/application.hpp>

#include <algorithm>
#include <array>
#include <thread>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

#include <remote_offload_common/control_message.hpp>
#include <remote_offload_common/doca_utils.hpp>
#include <remote_offload_common/runtime_error.hpp>

DOCA_LOG_REGISTER(server::application);

namespace remote_offload {
namespace server {

application::~application()
{
	cleanup();
}

application::application(server::configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_control_channel{nullptr},
	  m_listen_socket{},
	  m_thread_control{false, false},
	  m_threads{cfg.core_list.size()},
	  m_remote_needs_to_shutdown{false}
{
	try {
		m_dev = remote_offload::open_device(m_cfg.device_id);
		m_control_channel =
			new server::comch_control_channel{m_dev, m_cfg.representor_id, m_cfg.comch_channel_name};
	} catch (...) {
		cleanup();
		throw;
	}
}

void application::poll_control() noexcept
{
	if (m_control_channel == nullptr)
		return;

	m_control_channel->poll_pe();
	auto response = m_control_channel->get_pending_control_message();
	if (!response.empty()) {
		process_control_response(response);
	}

	if (!m_listen_socket.is_valid())
		return;

	remote_offload::tcp_socket client_socket{};
	try {
		client_socket = m_listen_socket.accept();
	} catch (remote_offload::runtime_error const &ex) {
		DOCA_LOG_ERR("Failed to accept new socket: %s:%s", doca_error_get_name(ex.get_doca_error()), ex.what());
	}

	if (!client_socket.is_valid())
		return;

	DOCA_LOG_INFO("Client connected");
	/* wait for the socket to become writable before using it */
	do {
		std::this_thread::yield();
		client_socket.poll();
		if (m_thread_control.quit_flag)
			return;
	} while (!client_socket.can_write());

	/* find a free thread */
	auto available_thread = std::find_if(std::begin(m_threads), std::end(m_threads), [](auto &thread) {
		return !thread.is_running();
	});
	if (available_thread == std::end(m_threads)) {
		DOCA_LOG_ERR("New client rejected, no available threads");
		client_socket.close();
	} else {
		available_thread->launch(std::move(client_socket));
	}
}

bool application::is_comch_client_connected() noexcept
{
	if (m_control_channel != nullptr && m_control_channel->is_connected()) {
		m_remote_needs_to_shutdown = true;
		return true;
	}

	return false;
}

void application::prepare_threads()
{
	for (uint32_t ii = 0; ii != m_threads.size(); ++ii) {
		DOCA_LOG_INFO("Prepare thread using CPU: %u", m_cfg.core_list[ii]);
		m_threads[ii].init(m_dev,
				   m_cfg.core_list[ii],
				   m_cfg.max_concurrent_messages,
				   m_cfg.max_message_length,
				   m_control_channel,
				   &m_thread_control);
	}
}

void application::start_tcp_server()
{
	m_listen_socket.listen(m_cfg.server_listen_port);
}

bool application::is_running() noexcept
{
	return m_thread_control.quit_flag == false;
}

void application::stop() noexcept
{
	m_listen_socket.close();

	m_thread_control.quit_flag = true;
	for (auto &thread : m_threads) {
		thread.join();
	}
}

doca_error_t application::disconnect_from_comch_host() noexcept
{
	doca_error_t result;

	if (m_control_channel == nullptr || m_control_channel->is_connected() == false) {
		return DOCA_SUCCESS;
	}

	if (m_remote_needs_to_shutdown) {
		/* If consumers and producers are active these need to be destroyed before the comch control connection
		 * can be closed. Ask the other side to tear down the consumers and producers they have so that this can
		 * happen
		 */
		DOCA_LOG_ERR("Sending shutdown start request to host");
		control::message_header hdr{sizeof(control::message_header), control::message_id::start_shutdown};
		std::array<uint8_t, sizeof(control::message_header)> message_buf;
		control::encode(message_buf.data(), hdr);
		result = m_control_channel->send_control_message(message_buf.data(), message_buf.size());
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send start remote shutdown process request");
			return result;
		}

		do {
			m_control_channel->poll_pe();
			auto response = m_control_channel->get_pending_control_message();
			if (response.empty()) {
				std::this_thread::yield();
			} else {
				process_control_response(response);
			}

		} while (m_remote_needs_to_shutdown);
	}

	/* Continue waiting for the remote client to disconnect */
	while (m_control_channel->is_connected()) {
		m_control_channel->poll_pe();
	}

	return DOCA_SUCCESS;
}

void application::cleanup()
{
	doca_error_t result;

	stop();

	m_threads.clear();

	result = disconnect_from_comch_host();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to perform clean disconnect from host: %s", doca_error_get_name(result));
	}

	if (m_control_channel != nullptr) {
		delete m_control_channel;
	}

	if (m_dev != nullptr) {
		result = doca_dev_close(m_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_name(result));
		}
	}
}

void application::process_control_response(std::vector<uint8_t> const &response) noexcept
{
	control::message_header hdr;
	control::decode(response.data(), hdr);

	DOCA_LOG_INFO("Receive control message: %u...", static_cast<uint32_t>(hdr.msg_id));

	switch (hdr.msg_id) {
	case control::message_id::start_shutdown: {
		DOCA_LOG_INFO("Host Comch notification: start shutdown");
		/* If remote is telling us to shutdown we should not send it a start shutdown when we disconnect */
		m_remote_needs_to_shutdown = false;

		handle_remote_shutdown_request();

		hdr.msg_id = control::message_id::shutdown_complete;
		std::array<uint8_t, sizeof(control::message_header)> message_buf;
		control::encode(message_buf.data(), hdr);
		auto const result = m_control_channel->send_control_message(message_buf.data(), message_buf.size());
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to ack start shutdown: %s", doca_error_get_name(result));
		}
	} break;
	case control::message_id::shutdown_complete: {
		DOCA_LOG_INFO("Host Comch client notification: shutdown complete");
		/* Remote has acked our shutdown */
		m_remote_needs_to_shutdown = false;
	} break;
	default: {
		DOCA_LOG_ERR("Received unexpected message :%u from host comch connection",
			     static_cast<uint32_t>(hdr.msg_id));
	}
	}
}

void application::handle_remote_shutdown_request() noexcept
{
	stop();
}

} /* namespace server */
} /* namespace remote_offload */