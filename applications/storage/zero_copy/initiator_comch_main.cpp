/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <csignal>
#include <cstdio>
#include <memory>
#include <stdexcept>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_version.h>

#include <storage_common/os_utils.hpp>
#include <storage_common/doca_utils.hpp>
#include <zero_copy/initiator_comch_application.hpp>

using namespace std::string_literals;

namespace {

/*
 * Print the parsed configuration
 *
 * @cfg [in]: Configuration to display
 */
void print_config(storage::zero_copy::initiator_comch_application::configuration const &cfg) noexcept
{
	printf("configuration: {\n");
	printf("\tcpu_set : [");
	bool first = true;
	for (auto cpu : cfg.cpu_set) {
		if (first)
			first = false;
		else
			printf(", ");
		printf("%u", cpu);
	}
	printf("]\n");
	printf("\tdevice : \"%s\",\n", cfg.device_id.c_str());
	printf("\tcommand_channel_name : \"%s\",\n", cfg.command_channel_name.c_str());
	printf("\tbuffer_size : %u,\n", cfg.buffer_size);
	printf("\tbuffer_count : %u,\n", cfg.buffer_count);
	printf("\tbatch_size : %u,\n", cfg.batch_size);
	printf("\trun_limit_operation_count : %u,\n", cfg.run_limit_operation_count);
	printf("\tvalidate_writes : %s,\n", (cfg.validate_writes == 0 ? "no" : "yes"));
	printf("\tcontrol_timeout : %u,\n", static_cast<uint32_t>(cfg.control_timeout.count()));
	printf("}\n");
}

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed configuration
 *
 * @throws: std::runtime_error If the configuration cannot pe parsed or contains invalid values
 */
storage::zero_copy::initiator_comch_application::configuration parse_cli_args(int argc, char **argv)
{
	storage::zero_copy::initiator_comch_application::configuration config{};
	config.buffer_count = 64;
	config.buffer_size = 4096;
	config.batch_size = 0;
	config.validate_writes = false;
	config.command_channel_name = "storage_zero_copy_comch";
	config.control_timeout = std::chrono::seconds{10};

	doca_error_t ret;

	ret = doca_argp_init(NULL, &config);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to parse CLI args: "s + doca_error_get_name(ret)};
	}

	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		"d",
		"device",
		"Device identifier",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)->device_id =
				static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"operation",
		"Operation to perform. One of: read|write",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->operation_type = static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"run-limit-operation-count",
		"Run N operations then stop",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->run_limit_operation_count = *static_cast<int *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"cpu",
		"CPU core to which the process affinity can be set",
		storage::required_value,
		storage::multiple_values,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->cpu_set.push_back(*static_cast<int *>(value));
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"per-cpu-buffer-count",
		"Number of memory buffers to create. Default: 64",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->buffer_count = *static_cast<int *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"buffer-size",
		"Size of each created buffer. Default: 4096",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)->buffer_size =
				*static_cast<int *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_BOOLEAN,
		nullptr,
		"validate-writes",
		"Enable validation of writes operations by reading them back afterwards. Default: false",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->validate_writes = *static_cast<uint8_t *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"command-channel-name",
		"Name of the channel used by the doca_comch_client. Default: storage_zero_copy_comch",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->command_channel_name = static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"control-timeout",
		"Time (in seconds) to wait while performing control operations. Default: 10",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)
				->control_timeout = std::chrono::seconds{*static_cast<int *>(value)};
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"batch-size",
		"Batch size: Default: ${per-cpu-buffer-count} / 2",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<storage::zero_copy::initiator_comch_application::configuration *>(cfg)->batch_size =
				*static_cast<int *>(value);
			return DOCA_SUCCESS;
		});

	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to parse CLI args: "s + doca_error_get_name(ret)};
	}

	static_cast<void>(doca_argp_destroy());

	if (config.batch_size == 0) {
		config.batch_size = std::max(
			config.buffer_count / std::max(uint32_t{1}, static_cast<uint32_t>(config.cpu_set.size() * 2)),
			uint32_t{1});
	} else if (config.batch_size > 128) {
		config.batch_size = 128;
	}

	return config;
}

/*
 * Signal handler
 *
 * @cfg [in]: Configuration
 */
void validate_configuration(storage::zero_copy::initiator_comch_application::configuration const &cfg)
{
	bool valid_configuration = true;

	if (cfg.buffer_size == 0) {
		valid_configuration = false;
		printf("Invalid configuration: buffer-size must not be zero\n");
	}

	if ((cfg.buffer_size % storage::cache_line_size) != 0) {
		valid_configuration = false;
		printf("Invalid configuration: buffer-size(%u) must be a multiple of the cache line size(%u) to avoid false sharing\n",
		       cfg.buffer_size,
		       storage::cache_line_size);
	}

	if (cfg.buffer_count == 0) {
		valid_configuration = false;
		printf("Invalid configuration: per-cpu-buffer-count must not be zero\n");
	}

	if (cfg.run_limit_operation_count == 0) {
		valid_configuration = false;
		printf("Invalid configuration: run-limit-operation-count must not be zero\n");
	}

	if (cfg.control_timeout.count() == 0) {
		valid_configuration = false;
		printf("Invalid configuration: control-timeout must not be zero\n");
	}

	if (!valid_configuration) {
		throw std::runtime_error{"Invalid configuration detected"};
	}
}

std::unique_ptr<storage::zero_copy::initiator_comch_application> g_app{};

/*
 * Signal handler
 *
 * @signal [in]: Received signal number
 */
void signal_handler(int signal)
{
	static_cast<void>(signal);

	if (g_app)
		g_app->abort("User requested abort");
}

/*
 * Register signal handlers
 */
bool register_signal_handlers() noexcept
{
	struct sigaction new_sigaction {};
	new_sigaction.sa_handler = signal_handler;
	new_sigaction.sa_flags = 0;

	sigemptyset(&new_sigaction.sa_mask);

	if (sigaction(SIGINT, &new_sigaction, nullptr) != 0) {
		printf("failed to set SIGINT signal handler: %s\n", storage::strerror_r(errno).c_str());
		return false;
	}

	return true;
}

} /* namespace */

/*
 * Main
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	int rc = EXIT_SUCCESS;
	storage::create_doca_logger_backend();
	if (!register_signal_handlers()) {
		return EXIT_FAILURE;
	}

	printf("doca_storage_zero_copy_initiator_comch: v%s\n", doca_version());

	try {
		auto const cfg = parse_cli_args(argc, argv);
		print_config(cfg);
		validate_configuration(cfg);
		g_app.reset(storage::zero_copy::make_initiator_comch_application(cfg));
		if (g_app->run()) {
			auto const stats = g_app->get_stats();
			auto duration_secs_float = static_cast<double>(stats.duration.count()) /
						   std::chrono::microseconds{std::chrono::seconds{1}}.count();
			auto const bytes = uint64_t{stats.operation_count} * cfg.buffer_size;
			auto const GiBs = static_cast<double>(bytes) / (1024. * 1024. * 1024.);
			auto const miops =
				(static_cast<double>(stats.operation_count) / 1'000'000.) / duration_secs_float;
			auto const pe_hit_rate_pct =
				(static_cast<double>(stats.pe_hit_count) /
				 (static_cast<double>(stats.pe_hit_count) + static_cast<double>(stats.pe_miss_count))) *
				100.;

			printf("+================================================+\n");
			printf("| Stats\n");
			printf("+================================================+\n");
			printf("| Duration (seconds): %2.06lf\n", duration_secs_float);
			printf("| Operation count: %u\n", stats.operation_count);
			printf("| Data rate: %.03lf GiB/s\n", GiBs / duration_secs_float);
			printf("| IO rate: %.03lf MIOP/s\n", miops);
			printf("| PE hit rate: %2.03lf%% (%lu:%lu)\n",
			       pe_hit_rate_pct,
			       stats.pe_hit_count,
			       stats.pe_miss_count);
			printf("| Latency:\n");
			printf("| \tMin: %uus\n", stats.latency_min);
			printf("| \tMax: %uus\n", stats.latency_max);
			printf("| \tMean: %uus\n", stats.latency_mean);
			printf("+================================================+\n");
		}
	} catch (std::exception const &ex) {
		if (g_app)
			g_app->abort("Exception occurred");
		fprintf(stderr, "EXCEPTION: %s\n", ex.what());

		rc = EXIT_FAILURE;
	}

	g_app.reset();
	return rc;
}
