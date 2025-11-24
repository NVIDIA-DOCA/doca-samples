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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_ORCHESTRATOR_APPLICATION_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_ORCHESTRATOR_APPLICATION_HPP_

#include <vector>

#include <doca_dev.h>
#include <doca_gpunetio.h>

#include <remote_offload_common/control_message.hpp>

#include <orchestrator/comch_control_channel.hpp>
#include <orchestrator/comch_datapath.hpp>
#include <orchestrator/configuration.hpp>

namespace remote_offload {
namespace orchestrator {

/*
 * Host application class. This class manages the resources which are shared across threads such as the comch connection
 * and owns each of the thread objects.
 */
class application {
public:
	~application();
	application() = delete;
	explicit application(orchestrator::configuration const &cfg);
	application(application const &) = delete;
	application(application &&) noexcept = delete;
	application &operator=(application const &) = delete;
	application &operator=(application &&) noexcept = delete;

	void init();

	void poll_control() noexcept;

	bool is_comch_client_connected() noexcept;

	bool is_datapath_connected() noexcept;

	void launch_gpu_processing();

	bool is_running() noexcept;

	void stop() noexcept;

	doca_error_t disconnect_from_comch_server() noexcept;

	void prepare_producers_consumers();

private:
	remote_offload::control::message_id process_control_message(std::vector<uint8_t> const &message) noexcept;

	void handle_remote_shutdown_request() noexcept;

	/* Application configuration */
	orchestrator::configuration m_cfg;
	/* The doca_dev to use. */
	doca_dev *m_dev;
	/* The doca_gpu device to use. */
	doca_gpu *m_gpu;
	/* Comch control path connection to the server */
	remote_offload::orchestrator::comch_control_channel *m_control_channel;
	/* Comch datapath objects*/
	remote_offload::orchestrator::comch_datapath *m_comch_datapath;
	/* Control flags - Do we need to issue a remote shutdown */
	bool m_remote_needs_to_shutdown;
	/* GPU control flag */
	bool *m_gpu_control_flag_gpu;
	bool *m_gpu_control_flag_cpu;
};

} /* namespace orchestrator */
} /* namespace remote_offload */

#endif // APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_ORCHESTRATOR_APPLICATION_HPP_
