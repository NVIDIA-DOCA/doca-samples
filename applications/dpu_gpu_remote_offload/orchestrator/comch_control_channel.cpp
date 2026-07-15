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

#include <orchestrator/comch_control_channel.hpp>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

#include <remote_offload_common/doca_utils.hpp>
#include <remote_offload_common/runtime_error.hpp>

DOCA_LOG_REGISTER(orchestrator::comch_control);

namespace {

uint32_t constexpr max_concurrent_comch_control_messages = 4;

}

namespace remote_offload {
namespace orchestrator {

comch_control_channel::~comch_control_channel()
{
	cleanup();
}

comch_control_channel::comch_control_channel(doca_dev *dev, std::string const &comch_channel_name, doca_gpu *gpu)
	: m_pe{nullptr},
	  m_comch_client{nullptr},
	  m_comch_connection{nullptr},
	  m_rx_messages{}
{
	doca_error_t result;

	try {
		result = doca_comch_cap_client_is_supported(doca_dev_as_devinfo(dev));
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "DOCA device does not support comch server"};
		}

		result = doca_pe_create(&m_pe);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to create progress engine"};
		}

		result = doca_comch_client_create(dev, comch_channel_name.c_str(), &m_comch_client);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to create doca_comch_client"};
		}

		result = doca_pe_connect_ctx(m_pe, doca_comch_client_as_ctx(m_comch_client));
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to connect doca_comch_client with doca_pe"};
		}

		result = doca_ctx_set_datapath_on_gpu(doca_comch_client_as_ctx(m_comch_client), gpu);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed adding setting context datapath to gpu"};
		}

		result = doca_comch_client_task_send_set_conf(m_comch_client,
							      doca_comch_task_send_completion_cb,
							      doca_comch_task_send_error_cb,
							      max_concurrent_comch_control_messages);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result,
							    "Failed to configure doca_comch_client send task pool"};
		}

		result = doca_comch_client_event_msg_recv_register(m_comch_client, doca_comch_event_msg_recv_cb);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{
				result,
				"Failed to configure doca_comch_client receive task callback"};
		}

		result = doca_ctx_set_user_data(doca_comch_client_as_ctx(m_comch_client), doca_data{.ptr = this});
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to set doca_comch_client user data"};
		}

		result = doca_ctx_start(doca_comch_client_as_ctx(m_comch_client));
		if (result != DOCA_ERROR_IN_PROGRESS && result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to start doca_comch_client"};
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
	if (m_comch_connection != nullptr)
		return true;

	doca_ctx_states state = DOCA_CTX_STATE_IDLE;
	static_cast<void>(doca_ctx_get_state(doca_comch_client_as_ctx(m_comch_client), &state));
	if (state == DOCA_CTX_STATE_RUNNING) {
		static_cast<void>(doca_comch_client_get_connection(m_comch_client, &m_comch_connection));
		static_cast<void>(doca_comch_connection_set_user_data(m_comch_connection, doca_data{.ptr = this}));
		return true;
	}

	return false;
}

doca_comch_connection *comch_control_channel::get_connection() noexcept
{
	return m_comch_connection;
}

doca_error_t comch_control_channel::send_control_message(void *bytes, uint32_t byte_count) noexcept
{
	doca_error_t result;
	doca_comch_task_send *task;

	result = doca_comch_client_task_send_alloc_init(m_comch_client, m_comch_connection, bytes, byte_count, &task);
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

void comch_control_channel::cleanup() noexcept
{
	doca_error_t result;

	if (m_comch_client != nullptr) {
		result = doca_ctx_stop(doca_comch_client_as_ctx(m_comch_client));
		if (result == DOCA_ERROR_IN_PROGRESS) {
			doca_ctx_states state;
			do {
				static_cast<void>(doca_pe_progress(m_pe));
				static_cast<void>(doca_ctx_get_state(doca_comch_client_as_ctx(m_comch_client), &state));
			} while (state != DOCA_CTX_STATE_IDLE);
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop comch server: %s", doca_error_get_name(result));
		}
		result = doca_comch_client_destroy(m_comch_client);
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
}

} // namespace orchestrator
} // namespace remote_offload
