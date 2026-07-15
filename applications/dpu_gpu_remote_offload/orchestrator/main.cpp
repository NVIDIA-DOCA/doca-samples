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

#include <chrono>
#include <cstdio>
#include <thread>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_version.h>

#include <remote_offload_common/runtime_error.hpp>
#include <remote_offload_common/os_utils.hpp>

#include <orchestrator/configuration.hpp>
#include <orchestrator/application.hpp>
#include <orchestrator/stats.hpp>

DOCA_LOG_REGISTER(orchestrator::main);

namespace {

bool g_ctrl_c_cond = false;
void ctrl_c_signal_handler(void)
{
	g_ctrl_c_cond = true;
}

doca_error_t register_params(void) noexcept
{
	doca_error_t result;

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create device ID param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "d");
		doca_argp_param_set_long_name(param, "device-id");
		doca_argp_param_set_arguments(param, "<DEV ID>");
		doca_argp_param_set_description(param, "Device ID (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::orchestrator::configuration *>(config)->device_id =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register device ID param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create GPU PCI Address param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "g");
		doca_argp_param_set_long_name(param, "gpu-pci-addr");
		doca_argp_param_set_arguments(param, "<PCI ADDRESS>");
		doca_argp_param_set_description(param, "GPU PCIe Address (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::orchestrator::configuration *>(config)->gpu_pci_addr =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register GPU PCI Address param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create Comch channel name param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "c");
		doca_argp_param_set_long_name(param, "comch-channel-name");
		doca_argp_param_set_arguments(param, "<DEV ID>");
		doca_argp_param_set_description(param, "Comch channel name (optional).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::orchestrator::configuration *>(config)->device_id =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register Comch channel name param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create gpu threads param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_long_name(param, "num-gpu-threads");
		doca_argp_param_set_short_name(param, "t");
		doca_argp_param_set_arguments(param, "<NUM>");
		doca_argp_param_set_description(
			param,
			"Number of GPU threads to use when executing data path operations (mandatory). Must be the same as core count on server.");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const value = *static_cast<int *>(param);
			if (value < 1) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::orchestrator::configuration *>(config)->num_gpu_threads =
				static_cast<uint32_t>(value);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_mandatory(param);
		doca_argp_param_set_multiplicity(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register num-gpu-threads param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create max-concurrent-messages param: %s",
				     doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_long_name(param, "max-concurrent-messages");
		doca_argp_param_set_arguments(param, "<NUM_MESSAGES>");
		doca_argp_param_set_description(
			param,
			"Set the maximum number of concurrent messages that can be processed per thread (optional).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const value = *static_cast<int *>(param);
			if (value < 0) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::orchestrator::configuration *>(config)->max_concurrent_messages =
				static_cast<uint32_t>(value);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_multiplicity(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register max-concurrent-messages param: %s",
				     doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create max-message-length param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_long_name(param, "max-message-length");
		doca_argp_param_set_arguments(param, "<LENGTH>");
		doca_argp_param_set_description(
			param,
			"Set the maximum length of a message that can be processed (exclusive of header) (optional).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const value = *static_cast<int *>(param);
			if (value < 0) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::orchestrator::configuration *>(config)->max_message_length =
				static_cast<uint32_t>(value);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_multiplicity(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register max-message-length param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

remote_offload::orchestrator::configuration parse_cli_args(int argc, char **argv)
{
	remote_offload::orchestrator::configuration cfg{};
	doca_error_t result;

	/* Parse cmdline/json arguments */
	result = doca_argp_init(NULL, &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	static_cast<void>(doca_argp_destroy());
	return cfg;

destroy_argp:
	doca_argp_destroy();
exit_error:
	throw remote_offload::runtime_error{result, "Failed to parse args"};
}

void display_configuration(remote_offload::orchestrator::configuration const &cfg)
{
	DOCA_LOG_INFO("Configuration: {");
	DOCA_LOG_INFO("\tdevice_id: \"%s\"", cfg.device_id.c_str());
	DOCA_LOG_INFO("\tgpu_pci_addr: \"%s\"", cfg.gpu_pci_addr.c_str());
	DOCA_LOG_INFO("\tgpu_threads: \"%u\"", cfg.num_gpu_threads);
	DOCA_LOG_INFO("\tcomch_channel_name: \"%s\"", cfg.comch_channel_name.c_str());
	DOCA_LOG_INFO("\tmax_concurrent_messages: %u", cfg.max_concurrent_messages);
	DOCA_LOG_INFO("\tmax_message_length: %u", cfg.max_message_length);
	DOCA_LOG_INFO("}");
}

} /* namespace */

int main(int argc, char **argv)
{
	try {
		doca_error_t result;
		doca_log_backend *stdout_logger = nullptr;

		/* Register a logger backend */
		result = doca_log_backend_create_standard();
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "doca_log_backend_create_standard() failed"};
		}

		result = doca_log_backend_create_with_file_sdk(stdout, &stdout_logger);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "doca_log_backend_create_with_file_sdk() failed"};
		}

		result = doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_WARNING);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "doca_log_backend_set_sdk_level() failed"};
		}

		DOCA_LOG_INFO("%s: %s", argv[0], doca_version_runtime());

		remote_offload::install_ctrl_c_handler(ctrl_c_signal_handler);

		auto const cfg = parse_cli_args(argc, argv);
		display_configuration(cfg);

		auto app = remote_offload::orchestrator::application{cfg};
		app.init();
		DOCA_LOG_INFO("Connecting to DPU server via doca_comch...");
		while (!app.is_comch_client_connected())
			app.poll_control();
		DOCA_LOG_INFO("\tConnected to DPU server!");

		DOCA_LOG_INFO("Waiting for producers/consumers to be exchange...");
		while (!app.is_datapath_connected())
			app.poll_control();

		app.launch_gpu_processing();

		// Main loop: Run until the user aborts the applications or receives an abort from the remote side
		while (g_ctrl_c_cond == false && app.is_running()) {
			std::this_thread::sleep_for(std::chrono::milliseconds{100});
			app.poll_control();
		}

		DOCA_LOG_INFO("Shutting down data path...");
		app.stop();
		DOCA_LOG_INFO("\tData path shutdown complete!");

		DOCA_LOG_INFO("Disconnecting DPU Server Comch connection...");
		app.disconnect_from_comch_server();
		DOCA_LOG_INFO("\tDisconnected!");

	} catch (remote_offload::runtime_error const &ex) {
		DOCA_LOG_ERR("Exception: %s : %s", doca_error_get_name(ex.get_doca_error()), ex.what());
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("UNEXPECTED ERROR: %s", ex.what());
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}