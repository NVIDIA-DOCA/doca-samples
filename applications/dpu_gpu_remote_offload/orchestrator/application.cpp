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

#include <orchestrator/application.hpp>

#include <array>
#include <thread>
#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

#include <orchestrator/gpu_processing.h>
#include <remote_offload_common/control_message.hpp>
#include <remote_offload_common/doca_utils.hpp>
#include <remote_offload_common/runtime_error.hpp>

DOCA_LOG_REGISTER(orchestrator::application);

namespace {

uint32_t constexpr max_concurrent_comch_control_messages = 4;

doca_gpu *open_gpu(std::string const &pci_addr)
{
	cudaFree(0);
	doca_gpu *gpu;
	auto result = doca_gpu_create(pci_addr.c_str(), &gpu);

	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to open GPU device"};
	}
	return gpu;
}

} // namespace

namespace remote_offload {
namespace orchestrator {

application::~application()
{
	doca_error_t result;

	stop();

	result = disconnect_from_comch_server();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to perform clean disconnect from orchestrator: %s", doca_error_get_name(result));
	}

	if (m_comch_datapath != nullptr) {
		delete m_comch_datapath;
	}

	if (m_control_channel != nullptr) {
		delete m_control_channel;
	}

	if (m_gpu_control_flag_gpu != nullptr) {
		result = doca_gpu_mem_free(m_gpu, m_gpu_control_flag_gpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free GPU memory: %s", doca_error_get_name(result));
		}
	}

	if (m_gpu != nullptr) {
		result = doca_gpu_destroy(m_gpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close GPU: %s", doca_error_get_name(result));
		}
	}

	if (m_dev != nullptr) {
		result = doca_dev_close(m_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_name(result));
		}
	}
}

application::application(orchestrator::configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_gpu{nullptr},
	  m_control_channel{nullptr},
	  m_comch_datapath{nullptr},
	  m_remote_needs_to_shutdown{false},
	  m_gpu_control_flag_gpu{nullptr},
	  m_gpu_control_flag_cpu{nullptr}
{
}

void application::init()
{
	m_dev = remote_offload::open_device(m_cfg.device_id);
	m_gpu = open_gpu(m_cfg.gpu_pci_addr);
	m_control_channel = new orchestrator::comch_control_channel{m_dev, m_cfg.comch_channel_name, m_gpu};
	m_comch_datapath = new orchestrator::comch_datapath(m_dev, m_gpu, m_cfg.num_gpu_threads);
}

void application::poll_control() noexcept
{
	m_control_channel->poll_pe();
	auto message = m_control_channel->get_pending_control_message();
	if (!message.empty()) {
		static_cast<void>(process_control_message(message));
	}
	m_comch_datapath->poll_pe();
}

bool application::is_comch_client_connected() noexcept
{
	if (m_control_channel->is_connected()) {
		m_remote_needs_to_shutdown = true;
		return true;
	}

	return false;
}

bool application::is_datapath_connected() noexcept
{
	return m_comch_datapath->are_all_contexts_running();
}

void application::launch_gpu_processing()
{
	gpu_thread_data *gpu_data = m_comch_datapath->get_gpu_thread_data();

	doca_error_t result = doca_gpu_mem_alloc(m_gpu,
						 sizeof(bool),
						 0,
						 DOCA_GPU_MEM_TYPE_GPU_CPU,
						 (void **)&m_gpu_control_flag_gpu,
						 (void **)&m_gpu_control_flag_cpu);
	if (result != DOCA_SUCCESS || m_gpu_control_flag_gpu == nullptr || m_gpu_control_flag_cpu == nullptr) {
		throw remote_offload::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate GPU flag buffer"};
	}
	*m_gpu_control_flag_cpu = false;

	DOCA_LOG_INFO("Starting GPU Processing with %u threads", m_cfg.num_gpu_threads);
	start_gpu_processing(m_cfg.num_gpu_threads,
			     m_gpu_control_flag_gpu,
			     gpu_data,
			     m_cfg.max_message_length + sizeof(control::message_header),
			     m_cfg.max_concurrent_messages);
}

bool application::is_running() noexcept
{
	return !*m_gpu_control_flag_cpu;
}

void application::stop() noexcept
{
	if (m_gpu_control_flag_gpu != nullptr) {
		*m_gpu_control_flag_cpu = true;
		cudaDeviceSynchronize();
		m_comch_datapath->cleanup();
	}
}

doca_error_t application::disconnect_from_comch_server() noexcept
{
	doca_error_t result;

	if (m_control_channel == nullptr || !m_control_channel->is_connected()) {
		return DOCA_SUCCESS;
	}

	if (m_remote_needs_to_shutdown) {
		/* If consumers and producers are active these need to be destroyed before the comch control connection
		 * can be closed. Ask the other side to tear down the consumers and producers they have so that this can
		 * happen
		 */
		DOCA_LOG_ERR("Sending shutdown start request to server");
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
				static_cast<void>(process_control_message(response));
			}

		} while (m_remote_needs_to_shutdown);
	}

	return DOCA_SUCCESS;
}

remote_offload::control::message_id application::process_control_message(std::vector<uint8_t> const &message) noexcept
{
	control::message_header hdr;
	control::decode(message.data(), hdr);

	DOCA_LOG_INFO("Receive control message: %u...", static_cast<uint32_t>(hdr.msg_id));

	auto const recv_message_id = hdr.msg_id;
	switch (recv_message_id) {
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
	case control::message_id::exchange_consumer_id_request: {
		control::exchange_consumer_id_data payload;
		control::decode(message.data() + sizeof(control::message_header), payload);

		try {
			payload.consumer_id =
				m_comch_datapath->register_remote_consumer(payload.consumer_id,
									   m_cfg.max_concurrent_messages,
									   m_cfg.max_message_length,
									   m_control_channel->get_connection());
		} catch (remote_offload::runtime_error const &ex) {
			DOCA_LOG_ERR("Failed to to prepare comch consumer and producer: %s : %s",
				     doca_error_get_name(ex.get_doca_error()),
				     ex.what());
			return recv_message_id;
		}

		hdr.msg_id = control::message_id::exchange_consumer_id_response;
		std::array<uint8_t, sizeof(control::message_header) + sizeof(control::exchange_consumer_id_data)>
			response;
		control::encode(control::encode(response.data(), hdr), payload);

		auto const result = m_control_channel->send_control_message(response.data(), response.size());
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send exchange consumer id response: %s", doca_error_get_name(result));
		}
	} break;
	default: {
		DOCA_LOG_ERR("Received unexpected message :%u from orchestrator comch connection",
			     static_cast<uint32_t>(hdr.msg_id));
	}
	}

	return recv_message_id;
}

void application::handle_remote_shutdown_request() noexcept
{
	stop();
}

} /* namespace orchestrator */
} /* namespace remote_offload */
