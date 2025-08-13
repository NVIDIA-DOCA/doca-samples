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
#include <stdexcept>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_version.h>

#include <storage_common/os_utils.hpp>
#include <storage_common/doca_utils.hpp>
#include <zero_copy/target_rdma_application.hpp>

using namespace std::string_literals;

namespace {

/*
 * Print the parsed configuration
 *
 * @cfg [in]: Configuration to display
 */
void print_config(storage::zero_copy::target_rdma_application::configuration const &cfg) noexcept
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
	printf("\tlisten_port : %u,\n", cfg.listen_port);
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
storage::zero_copy::target_rdma_application::configuration parse_cli_args(int argc, char **argv)
{
	storage::zero_copy::target_rdma_application::configuration config{};
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
			static_cast<storage::zero_copy::target_rdma_application::configuration *>(cfg)->device_id =
				static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"listen-port",
		"TCP Port on which to listen for incoming connections",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			auto port = *static_cast<int *>(value);
			if (port > UINT16_MAX || port <= 0)
				return DOCA_ERROR_INVALID_VALUE;
			static_cast<storage::zero_copy::target_rdma_application::configuration *>(cfg)->listen_port =
				static_cast<uint16_t>(port);
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
			static_cast<storage::zero_copy::target_rdma_application::configuration *>(cfg)
				->cpu_set.push_back(*static_cast<int *>(value));
			return DOCA_SUCCESS;
		});

	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to parse CLI args: "s + doca_error_get_name(ret)};
	}

	static_cast<void>(doca_argp_destroy());

	return config;
}

std::unique_ptr<storage::zero_copy::target_rdma_application> g_app{};

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
		g_app.reset(storage::zero_copy::make_target_rdma_application(cfg));
		g_app->run();
		auto const &stats = g_app->get_stats();
		printf("+================================================+\n");
		printf("| Stats\n");
		printf("+================================================+\n");
		for (uint32_t ii = 0; ii != stats.size(); ++ii) {
			printf("| Thread[%u]\n", ii);
			auto const pe_hit_rate_pct = (static_cast<double>(stats[ii].pe_hit_count) /
						      (static_cast<double>(stats[ii].pe_hit_count) +
						       static_cast<double>(stats[ii].pe_miss_count))) *
						     100.;
			printf("| PE hit rate: %2.03lf%% (%lu:%lu)\n",
			       pe_hit_rate_pct,
			       stats[ii].pe_hit_count,
			       stats[ii].pe_miss_count);

			printf("+------------------------------------------------+\n");
		}
		printf("+================================================+\n");

	} catch (std::exception const &ex) {
		if (g_app)
			g_app->abort("Exception: "s + ex.what());
		fprintf(stderr, "EXCEPTION: %s\n", ex.what());

		rc = EXIT_FAILURE;
	}

	g_app.reset();
	return rc;
}
