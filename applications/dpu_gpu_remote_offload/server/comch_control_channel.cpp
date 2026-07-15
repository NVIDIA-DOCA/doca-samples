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

#include <server/comch_control_channel.hpp>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

#include <remote_offload_common/doca_utils.hpp>
#include <remote_offload_common/runtime_error.hpp>

DOCA_LOG_REGISTER(server::comch_control);

namespace {

uint32_t constexpr max_concurrent_comch_control_messages = 4;

}

namespace remote_offload {
namespace server {

comch_control_channel::~comch_control_channel()
{
	cleanup();
}

comch_control_channel::comch_control_channel(doca_dev *dev,
					     std::string const &representor_id,
					     std::string const &comch_channel_name)
	: m_dev_rep{nullptr},
	  m_pe{nullptr},
	  m_comch_server{nullptr},
	  m_comch_connection{nullptr},
	  m_rx_messages{}
{
	doca_error_t result;

	try {
		result = doca_comch_cap_server_is_supported(doca_dev_as_devinfo(dev));
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "DOCA device does not support comch server"};
		}

		m_dev_rep = remote_offload::open_representor(dev, representor_id);

		result = doca_pe_create(&m_pe);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to create progress engine"};
		}

		result = doca_comch_server_create(dev, m_dev_rep, comch_channel_name.c_str(), &m_comch_server);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to create doca_comch_server"};
		}

		result = doca_pe_connect_ctx(m_pe, doca_comch_server_as_ctx(m_comch_server));
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to connect doca_comch_server with doca_pe"};
		}

		result = doca_comch_server_task_send_set_conf(m_comch_server,
							      doca_comch_task_send_completion_cb,
							      doca_comch_task_send_error_cb,
							      max_concurrent_comch_control_messages);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result,
							    "Failed to configure doca_comch_server send task pool"};
		}

		result = doca_comch_server_event_msg_recv_register(m_comch_server, doca_comch_event_msg_recv_cb);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{
				result,
				"Failed to configure doca_comch_server receive task callback"};
		}

		result = doca_comch_server_event_connection_status_changed_register(m_comch_server,
										    doca_comch_event_connection_cb,
										    doca_comch_event_disconnection_cb);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{
				result,
				"Failed to configure doca_comch_server connection callbacks"};
		}

		result = doca_ctx_set_user_data(doca_comch_server_as_ctx(m_comch_server), doca_data{.ptr = this});
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to set doca_comch_server user data"};
		}

		result = doca_ctx_start(doca_comch_server_as_ctx(m_comch_server));
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to start doca_comch_server"};
		}
	} catch (...) {
		cleanup();
		throw;
	}
}

void comch_control_channel::poll_pe() noexcept
{
	static_cast<void>(doca_pe_progress(m_pe));
}

bool comch_control_channel::is_connected() noexcept
{
	return m_comch_connection != nullptr;
}

doca_comch_connection *comch_control_channel::get_connection() noexcept
{
	return m_comch_connection;
}

doca_error_t comch_control_channel::send_control_message(void *bytes, uint32_t byte_count) noexcept
{
	doca_error_t result;
	doca_comch_task_send *task;

	result = doca_comch_server_task_send_alloc_init(m_comch_server, m_comch_connection, bytes, byte_count, &task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate doca_comch_task_send");
		return result;
	}

	result = doca_task_submit(doca_comch_task_send_as_task(task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send doca_comch_task_send");
		doca_task_free(doca_comch_task_send_as_task(task));
	}

	return result;
}

std::vector<uint8_t> comch_control_channel::get_pending_control_message() noexcept
{
	std::vector<uint8_t> message;

	if (!m_rx_messages.empty()) {
		message = std::move(m_rx_messages[0]);
		m_rx_messages.erase(m_rx_messages.begin());
	}

	return message;
}

void comch_control_channel::doca_comch_task_send_completion_cb(doca_comch_task_send *task,
							       doca_data task_user_data,
							       doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);
	static_cast<void>(ctx_user_data);

	doca_task_free(doca_comch_task_send_as_task(task));
}

void comch_control_channel::doca_comch_task_send_error_cb(doca_comch_task_send *task,
							  doca_data task_user_data,
							  doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);
	static_cast<void>(ctx_user_data);

	DOCA_LOG_ERR("doca_comch_task_send: %p failed: %s",
		     task,
		     doca_error_get_name(doca_task_get_status(doca_comch_task_send_as_task(task))));

	doca_task_free(doca_comch_task_send_as_task(task));
}

void comch_control_channel::doca_comch_event_msg_recv_cb(doca_comch_event_msg_recv *event,
							 uint8_t *recv_buffer,
							 uint32_t msg_len,
							 doca_comch_connection *comch_connection) noexcept
{
	static_cast<void>(event);

	auto *self = static_cast<comch_control_channel *>(doca_comch_connection_get_user_data(comch_connection).ptr);

	self->m_rx_messages.emplace_back(recv_buffer, recv_buffer + msg_len);
}

void comch_control_channel::doca_comch_event_connection_cb(doca_comch_event_connection_status_changed *event,
							   doca_comch_connection *comch_connection,
							   uint8_t change_successful) noexcept
{
	static_cast<void>(event);
	DOCA_LOG_DBG("Connection %p %s", comch_connection, (change_successful ? "connected" : "refused"));

	if (change_successful == 0) {
		DOCA_LOG_ERR("Failed to accept new client connection");
		return;
	}

	doca_data user_data{.ptr = nullptr};
	static_cast<void>(
		doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(comch_connection)),
				       &user_data));
	if (user_data.ptr == nullptr) {
		// Will never happen but coverity complains if we don't check it.
		DOCA_LOG_ERR("[BUG] Failed to accept new client connection, user_data is null");
		return;
	}

	auto *self = static_cast<comch_control_channel *>(user_data.ptr);
	static_cast<void>(doca_comch_connection_set_user_data(comch_connection, doca_data{.ptr = self}));
	self->m_comch_connection = comch_connection;
}

void comch_control_channel::doca_comch_event_disconnection_cb(doca_comch_event_connection_status_changed *event,
							      doca_comch_connection *comch_connection,
							      uint8_t change_successful) noexcept
{
	static_cast<void>(event);
	static_cast<void>(change_successful);

	DOCA_LOG_DBG("Connection %p disconnected", comch_connection);

	doca_data user_data{.ptr = nullptr};
	static_cast<void>(
		doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(comch_connection)),
				       &user_data));

	if (user_data.ptr == nullptr) {
		// Will never happen but coverity complains if we don't check it.
		DOCA_LOG_ERR("[BUG] Failed to process disconnect, user_data is null");
		return;
	}

	auto *self = static_cast<comch_control_channel *>(user_data.ptr);
	if (self->m_comch_connection == comch_connection) {
		self->m_comch_connection = nullptr;
	} else {
		DOCA_LOG_WARN("Ignoring disconnect of unknown connection");
	}
}

void comch_control_channel::cleanup() noexcept
{
	doca_error_t result;

	if (m_comch_server != nullptr) {
		result = doca_ctx_stop(doca_comch_server_as_ctx(m_comch_server));
		if (result == DOCA_ERROR_IN_PROGRESS) {
			doca_ctx_states state;
			do {
				static_cast<void>(doca_pe_progress(m_pe));
				static_cast<void>(doca_ctx_get_state(doca_comch_server_as_ctx(m_comch_server), &state));
			} while (state != DOCA_CTX_STATE_IDLE);
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop comch server: %s", doca_error_get_name(result));
		}
		result = doca_comch_server_destroy(m_comch_server);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop comch server: %s", doca_error_get_name(result));
		}
	}

	if (m_pe != nullptr) {
		result = doca_pe_destroy(m_pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy progress engine: %s", doca_error_get_name(result));
		}
	}

	if (m_dev_rep != nullptr) {
		result = doca_dev_rep_close(m_dev_rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device representor: %s", doca_error_get_name(result));
		}
	}
}

} // namespace server
} // namespace remote_offload
