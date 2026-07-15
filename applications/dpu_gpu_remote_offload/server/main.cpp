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

#include <server/configuration.hpp>
#include <server/application.hpp>
#include <server/stats.hpp>

DOCA_LOG_REGISTER(server::main);

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
			static_cast<remote_offload::server::configuration *>(config)->device_id =
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
			DOCA_LOG_ERR("Failed to create representor ID param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "r");
		doca_argp_param_set_long_name(param, "representor-id");
		doca_argp_param_set_arguments(param, "<REPRESENTOR ID>");
		doca_argp_param_set_description(param, "Representor ID (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::server::configuration *>(config)->representor_id =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register representor ID param: %s", doca_error_get_descr(result));
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
			static_cast<remote_offload::server::configuration *>(config)->comch_channel_name =
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
			DOCA_LOG_ERR("Failed to create server listen port param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "p");
		doca_argp_param_set_long_name(param, "server-listen-port");
		doca_argp_param_set_arguments(param, "<PORT>");
		doca_argp_param_set_description(param, "Server listen port (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const port = *static_cast<int *>(param);
			if (port < 0 || port > 0xFFFF) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::server::configuration *>(config)->server_listen_port = port;
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register server listen port param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create cpu param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_long_name(param, "cpu");
		doca_argp_param_set_arguments(param, "<CPU>");
		doca_argp_param_set_description(
			param,
			"CPU to use when executing data path operations (mandatory). May be repeated for more cores");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const cpu = *static_cast<int *>(param);
			if (cpu < 0) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::server::configuration *>(config)->core_list.push_back(
				static_cast<uint32_t>(cpu));
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_multiplicity(param);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register cpu param: %s", doca_error_get_descr(result));
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

			static_cast<remote_offload::server::configuration *>(config)->max_concurrent_messages =
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

			static_cast<remote_offload::server::configuration *>(config)->max_message_length =
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

remote_offload::server::configuration parse_cli_args(int argc, char **argv)
{
	remote_offload::server::configuration cfg{};
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

void display_configuration(remote_offload::server::configuration const &cfg)
{
	DOCA_LOG_INFO("Configuration: {");
	DOCA_LOG_INFO("\tcpus: [");
	if (!cfg.core_list.empty()) {
		DOCA_LOG_INFO("%u", cfg.core_list[0]);
		for (uint32_t ii = 1; ii != cfg.core_list.size(); ++ii) {
			DOCA_LOG_INFO(", %u", cfg.core_list[ii]);
		}
	}
	DOCA_LOG_INFO("]");
	DOCA_LOG_INFO("\tdevice_id: \"%s\"", cfg.device_id.c_str());
	DOCA_LOG_INFO("\trepresentor_id: \"%s\"", cfg.representor_id.c_str());
	DOCA_LOG_INFO("\tcomch_channel_name: \"%s\"", cfg.comch_channel_name.c_str());
	DOCA_LOG_INFO("\tmax_concurrent_messages: %u", cfg.max_concurrent_messages);
	DOCA_LOG_INFO("\tmax_message_length: %u", cfg.max_message_length);
	DOCA_LOG_INFO("\tserver_listen_port: %u", cfg.server_listen_port);
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

		remote_offload::install_ctrl_c_handler(ctrl_c_signal_handler);

		DOCA_LOG_INFO("%s: %s", argv[0], doca_version_runtime());

		auto const cfg = parse_cli_args(argc, argv);
		display_configuration(cfg);

		auto app = remote_offload::server::application{cfg};

		DOCA_LOG_INFO("Waiting for host to connect via doca_comch...");
		while (!app.is_comch_client_connected()) {
			app.poll_control();
			std::this_thread::sleep_for(std::chrono::milliseconds{100});
			if (g_ctrl_c_cond == true) {
				DOCA_LOG_INFO("\tAborted waiting to connect to host!");
				return EXIT_FAILURE;
			}
		}
		DOCA_LOG_INFO("\tConnected to host!");

		DOCA_LOG_INFO("Pre-initialise data path...");
		app.prepare_threads();
		DOCA_LOG_INFO("\tPre-initialise data path complete!");

		app.start_tcp_server();
		DOCA_LOG_INFO("Listening for clients on socket %u...", cfg.server_listen_port);

		// Main loop: Run until the user aborts the applications or receives an abort from the remote side
		while (g_ctrl_c_cond == false && app.is_running()) {
			std::this_thread::sleep_for(std::chrono::milliseconds{100});
			app.poll_control();
		}

		DOCA_LOG_INFO("Shutting down data path...");
		app.stop();
		DOCA_LOG_INFO("\tData path shutdown complete!");

		DOCA_LOG_INFO("Disconnecting host Comch connection...");
		app.disconnect_from_comch_host();
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