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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_SERVER_APPLICATION_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_SERVER_APPLICATION_HPP_

#include <vector>

#include <doca_dev.h>

#include <remote_offload_common/tcp_socket.hpp>
#include <remote_offload_common/thread_control.hpp>

#include <server/comch_control_channel.hpp>
#include <server/configuration.hpp>
#include <server/thread.hpp>

namespace remote_offload {
namespace server {

/*
 * Server application class. This class manages the resources which are shared across threads such as the TCP listen
 * socket and the comch connection and owns each of the thread objects.
 */
class application {
public:
	~application();
	application() = delete;
	explicit application(server::configuration const &cfg);
	application(application const &) = delete;
	application(application &&) noexcept = delete;
	application &operator=(application const &) = delete;
	application &operator=(application &&) noexcept = delete;

	void poll_control() noexcept;

	bool is_comch_client_connected() noexcept;

	void prepare_threads();

	void start_tcp_server();

	bool is_running() noexcept;

	void stop() noexcept;

	doca_error_t disconnect_from_comch_host() noexcept;

private:
	void cleanup();

	void process_control_response(std::vector<uint8_t> const &response) noexcept;

	void handle_remote_shutdown_request() noexcept;

	/* Application configuration */
	server::configuration m_cfg;
	/* The doca_dev to use. */
	doca_dev *m_dev;
	/* Comch control path connection to the host */
	remote_offload::server::comch_control_channel *m_control_channel;
	/* TCP listener socket */
	remote_offload::tcp_socket m_listen_socket;
	/* shared thread control */
	remote_offload::thread_control m_thread_control;
	/* Thread objects */
	std::vector<server::thread> m_threads;
	/* Control flags - Do we need to issue a remote shutdown */
	bool m_remote_needs_to_shutdown;
};

} /* namespace server */
} /* namespace remote_offload */

#endif // APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_SERVER_APPLICATION_HPP_
