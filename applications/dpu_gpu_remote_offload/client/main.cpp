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

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_version.h>

#include <remote_offload_common/os_utils.hpp>
#include <remote_offload_common/runtime_error.hpp>

#include <client/configuration.hpp>
#include <client/application.hpp>
#include <client/stats.hpp>

DOCA_LOG_REGISTER(client::main);

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
			DOCA_LOG_ERR("Failed to create register server IP address param: %s",
				     doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "s");
		doca_argp_param_set_long_name(param, "server-ip-address");
		doca_argp_param_set_arguments(param, "<IP ADDR>");
		doca_argp_param_set_description(param, "Server IP address (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::client::configuration *>(config)->server_ip_address =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register server IP address param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create register server IP port param: %s",
				     doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "p");
		doca_argp_param_set_long_name(param, "server-ip-port");
		doca_argp_param_set_arguments(param, "<IP PORT>");
		doca_argp_param_set_description(param, "Server IP port (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const port = *static_cast<int *>(param);
			if (port < 0 || port > 0xFFFF) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::client::configuration *>(config)->server_ip_port = port;
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register server IP port param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create register thread count param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "t");
		doca_argp_param_set_long_name(param, "thread-count");
		doca_argp_param_set_arguments(param, "<THREAD_COUNT>");
		doca_argp_param_set_description(param, "Thread count (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const thread_count = *static_cast<int *>(param);
			if (thread_count < 0) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::client::configuration *>(config)->thread_count = thread_count;
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register thread count param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create register iteration count param: %s",
				     doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "i");
		doca_argp_param_set_long_name(param, "iteration-count");
		doca_argp_param_set_arguments(param, "<ITERATION_COUNT>");
		doca_argp_param_set_description(param, "Iteration count (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			auto const iteration_count = *static_cast<int *>(param);
			if (iteration_count < 0) {
				return DOCA_ERROR_INVALID_VALUE;
			}

			static_cast<remote_offload::client::configuration *>(config)->iteration_count = iteration_count;
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register iteration count param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create register message string param: %s",
				     doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "m");
		doca_argp_param_set_long_name(param, "message-string");
		doca_argp_param_set_arguments(param, "<MESSAGE>");
		doca_argp_param_set_description(param, "message string (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::client::configuration *>(config)->message_string =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register message string param: %s", doca_error_get_descr(result));
			return result;
		}
	}

	{
		struct doca_argp_param *param;

		result = doca_argp_param_create(&param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create register expected response param: %s",
				     doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(param, "e");
		doca_argp_param_set_long_name(param, "expected-response");
		doca_argp_param_set_arguments(param, "<EXPECTED RESPONSE>");
		doca_argp_param_set_description(param, "Expected response (mandatory).");
		doca_argp_param_set_callback(param, [](void *param, void *config) {
			static_cast<remote_offload::client::configuration *>(config)->expected_response =
				static_cast<char const *>(param);
			return DOCA_SUCCESS;
		});
		doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(param);
		result = doca_argp_register_param(param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register expected response param: %s", doca_error_get_descr(result));
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

			static_cast<remote_offload::client::configuration *>(config)->max_concurrent_messages =
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

	return DOCA_SUCCESS;
}

remote_offload::client::configuration parse_cli_args(int argc, char **argv)
{
	remote_offload::client::configuration cfg{};
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

void display_configuration(remote_offload::client::configuration const &cfg)
{
	DOCA_LOG_INFO("Configuration: {");
	DOCA_LOG_INFO("\tserver_ip_address: \"%s\"", cfg.server_ip_address.c_str());
	DOCA_LOG_INFO("\tserver_ip_port: %u", cfg.server_ip_port);
	DOCA_LOG_INFO("\tmessage_string: \"%s\"", cfg.message_string.c_str());
	DOCA_LOG_INFO("\texpected_response: \"%s\"", cfg.expected_response.c_str());
	DOCA_LOG_INFO("\titeration_count: %u", cfg.iteration_count);
	DOCA_LOG_INFO("\tthread_count: %u", cfg.thread_count);
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

		remote_offload::client::application app{cfg};

		DOCA_LOG_INFO("Start running client threads...");
		app.start();
		DOCA_LOG_INFO("\tstarted!");

		// Main loop: Run until the user aborts the applications or receives an abort from the remote side
		while (g_ctrl_c_cond == false && app.is_running()) {
			std::this_thread::sleep_for(std::chrono::milliseconds{100});
		}

		DOCA_LOG_INFO("Stopping client threads...");
		app.stop();
		DOCA_LOG_INFO("\tStopped");

		if (app.failed()) {
			DOCA_LOG_ERR("Execution encountered one or more errors, will not display stats");
			fflush(stdout);
			fflush(stderr);
			return EXIT_FAILURE;
		}

		auto const stats = app.collect_stats();

		auto const duration_secs_float = static_cast<double>(stats.execution_time.count()) / 1'000.;
		auto const GiBs_tx = static_cast<double>(stats.total_request_byte_count) / (1024. * 1024. * 1024.);
		auto const GiBs_rx = static_cast<double>(stats.total_response_byte_count) / (1024. * 1024. * 1024.);
		auto const miops = (static_cast<double>(stats.total_messages) / 1'000'000.) / duration_secs_float;

		DOCA_LOG_INFO("+================================================+");
		DOCA_LOG_INFO("| Stats");
		DOCA_LOG_INFO("+================================================+");
		DOCA_LOG_INFO("| Duration (seconds): %2.06lf", duration_secs_float);
		DOCA_LOG_INFO("| Operation count: %lu", stats.total_messages);
		DOCA_LOG_INFO("| Tx Data rate: %.03lf GiB/s", GiBs_tx / duration_secs_float);
		DOCA_LOG_INFO("| Rx Data rate: %.03lf GiB/s", GiBs_rx / duration_secs_float);
		DOCA_LOG_INFO("| IO rate: %.03lf MIOP/s", miops);
		DOCA_LOG_INFO("+================================================+");

	} catch (remote_offload::runtime_error const &ex) {
		DOCA_LOG_ERR("%s : %s", doca_error_get_name(ex.get_doca_error()), ex.what());
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("UNEXPECTED ERROR : %s", ex.what());
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
