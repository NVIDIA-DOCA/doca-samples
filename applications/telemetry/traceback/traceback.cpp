/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <signal.h>

#include <traceback_core.hpp>

#include <doca_error.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(TRACEBACK);

namespace {

/* Shared signal to exit the application */
std::atomic_bool g_quit_flag = false;

/*
 * Signal function to catch ctrl+c and break polling loop
 *
 * @signum [in]: Signal number
 */
void sigint_handler(int signum)
{
	static_cast<void>(signum);
	g_quit_flag = true;
}

doca_error_t create_doca_logger_backend() noexcept
{
	doca_error_t status;

	doca_log_backend *stdout_logger = nullptr;

	/* Register a logger backend */
	status = doca_log_backend_create_standard();
	if (status != DOCA_SUCCESS) {
		return status;
	}

	status = doca_log_backend_create_with_file_sdk(stdout, &stdout_logger);
	if (status != DOCA_SUCCESS) {
		return status;
	}

	status = doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_WARNING);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_log_backend_set_sdk_level() failed: %s", doca_error_get_name(status));
		return status;
	}

	return DOCA_SUCCESS;
}

} /* namespace */

int main(int argc, char **argv)
{
	doca_error_t status;
	configuration cfg{};

	status = create_doca_logger_backend();
	if (status != DOCA_SUCCESS) {
		fprintf(stderr, "create_doca_logger_backend() failed: %s\n", doca_error_get_name(status));
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	}

	status = parse_configuration(argc, argv, cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse configuration: %s", doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	std::vector<char> file_data;

	status = load_file_into_buffer(cfg.rules_file_path, file_data);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load rules file: \"%s\". Error: %s",
			     cfg.rules_file_path.c_str(),
			     doca_error_get_name(status));
		return status;
	}

	status = parse_json_configuration(file_data.data(),
					  cfg.diag_sample_period_ns,
					  cfg.sample_history_depth,
					  cfg.data_id_definitions,
					  cfg.rule_definitions);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load json data: %s", doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	if (!validate_configuration(cfg)) {
		return EXIT_FAILURE;
	}

	/* Each Sample record contains: 64 bit timestamp followed by N 64 bit values, one per data-id */
	size_t const sample_history_record_size = sizeof(uint64_t) * (1 + cfg.data_id_definitions.size());

	/* The user defined how many samples they wish to see as previous history when a rule activates. To keep things
	 * simple the circular buffer stores twice that amount with the rule processing the most recent half of the data
	 * with the latter half available then as historic data.
	 */
	samples_circular_buffer sample_history_buffer;
	status = sample_history_buffer.init_data_aligned_ring(DESIRED_MEMORY_ALIGNMENT,
							      sample_history_record_size,
							      cfg.sample_history_depth * 2);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create diag history buffer: %s", doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	/* Prepare rule monitors */
	uint32_t const hysteresis_threshold = cfg.sample_history_depth;
	std::vector<rule> rules{};
	rules.reserve(cfg.rule_definitions.size());

	for (auto const &rule_def : cfg.rule_definitions) {
		rules.emplace_back();

		/* find the position of the data id used by the rule in the data ids collection */
		auto matching_data_id = std::find_if(std::begin(cfg.data_id_definitions),
						     std::end(cfg.data_id_definitions),
						     [&rule_def](auto const &data_id_def) {
							     return data_id_def.data_id == rule_def.data_id;
						     });

		/* Sample contains a time stamp followed by N counters. The counter values match the order of the data
		 * ids. So to get the correct index inside the sample for data_id[N] is N+1.
		 */
		auto const sample_record_offset =
			1 + std::distance(std::begin(cfg.data_id_definitions), matching_data_id);

		status = rules.rbegin()->configure(rule_def.comparison_type,
						   rule_def.comparison_value,
						   sample_record_offset,
						   hysteresis_threshold);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure rule: %s", doca_error_get_name(status));
			return EXIT_FAILURE;
		}
	}

	/* Create log file */
	std::ofstream events_log_file{cfg.event_log_file_path, std::ios_base::out | std::ios_base::trunc};
	if (!events_log_file) {
		DOCA_LOG_ERR("Failed to create events log file: \"%s\"", cfg.event_log_file_path.c_str());
		return EXIT_FAILURE;
	}

	/* Open DOCA dev */
	std::unique_ptr<doca_dev, void (*)(doca_dev *)> device{nullptr, close_doca_dev};
	status = open_doca_dev(cfg.device_id, device);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	/* check device support doca_telemetry_diag */
	status = check_dev_supports_doca_telemetry_diag(cfg, device.get());
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Diag is not supported or not able to perform the requested operations: %s",
			     doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	/* Create and configure diag context */
	std::unique_ptr<doca_telemetry_diag, void (*)(doca_telemetry_diag *)> diag{nullptr, stop_and_destroy_diag_ctx};
	status = create_and_prepare_diag_ctx(cfg, device.get(), diag);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare diag context: %s", doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	status = doca_telemetry_diag_start(diag.get());
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_diag: %s", doca_error_get_name(status));
		return EXIT_FAILURE;
	}

	/* Validate calculated sample size matches actual sample size */
	size_t const diag_sample_size =
		sizeof(doca_telemetry_diag_data_sample_format_1) + (sizeof(uint64_t) * cfg.data_id_definitions.size());
	{
		uint32_t actual_sample_size = 0;
		status = doca_telemetry_diag_get_sample_size(diag.get(), &actual_sample_size);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_telemetry_diag_get_sample_size failed: %s", doca_error_get_name(status));
			return EXIT_FAILURE;
		}

		if (actual_sample_size != diag_sample_size) {
			DOCA_LOG_ERR("[BUG] Expected sample size to be: %zu actual sample size: %u",
				     diag_sample_size,
				     actual_sample_size);
			return EXIT_FAILURE;
		}
	}

	/* Validate that the sampling period requested is achieved */
	{
		uint64_t actual_sample_period;
		status = doca_telemetry_diag_get_sample_period(diag.get(), &actual_sample_period);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_telemetry_diag_get_sample_period failed: %s", doca_error_get_name(status));
			return EXIT_FAILURE;
		}

		if (actual_sample_period != cfg.diag_sample_period_ns) {
			DOCA_LOG_ERR(
				"Device is not able to provide requested sampling period: %u. Actually providing: %lu",
				cfg.diag_sample_period_ns,
				actual_sample_period);
			return EXIT_FAILURE;
		}
	}

	uint32_t active_monitors_count = 0;
	uint32_t read_sample_count = 0;
	std::vector<uint8_t> sample_buffer;

	sample_buffer.resize(cfg.sample_history_depth * diag_sample_size);

	/* Diag sample is a timestamp followed by N values. The timestamp is presented as two uint32_ts which needs
	 * reconstructed then the values are copied from the diag memory into the circular buffer. The memcpy size
	 * can be calculated once and cached once early
	 */
	uint32_t const history_data_memcpy_len = diag_sample_size - sizeof(uint64_t);

	log_app_start_header(events_log_file, cfg);
	std::flush(events_log_file);

	if (signal(SIGINT, sigint_handler) == SIG_ERR) {
		DOCA_LOG_ERR("Failed to install signal handler");
		return EXIT_FAILURE;
	}

	while (!g_quit_flag) {
		status = doca_telemetry_diag_query_counters(diag.get(),
							    sample_buffer.data(),
							    cfg.sample_history_depth,
							    &read_sample_count);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query counters with error=%s", doca_error_get_name(status));
			return EXIT_FAILURE;
		}

		if (0 == read_sample_count) {
			std::this_thread::sleep_for(std::chrono::nanoseconds{cfg.diag_sample_period_ns});
			continue;
		}

		/* process samples */
		for (uint32_t sample_idx = 0; sample_idx != read_sample_count; ++sample_idx) {
			doca_telemetry_diag_data_sample_format_1 const *const sample =
				reinterpret_cast<doca_telemetry_diag_data_sample_format_1 const *>(
					sample_buffer.data() + (sample_idx * diag_sample_size));

			sample_history_buffer.advance_head_idx();
			uint64_t *const history_record =
				sample_history_buffer.get_sample(sample_history_buffer.get_head_idx());
			history_record[0] = (uint64_t{sample->earliest_data_timestamp_h} << 32) |
					    sample->latest_data_timestamp_l;
			::memcpy(history_record + 1, sample->data_value, history_data_memcpy_len);

			if (active_monitors_count != 0) {
				/* Log: all samples that were read until the rule was deactivated */
				log_event_samples(events_log_file,
						  sample_history_buffer,
						  static_cast<uint32_t>(cfg.data_id_definitions.size()),
						  1);
			}

			for (auto &monitor : rules) {
				if (monitor.process_sample(sample_history_buffer) == false)
					continue;

				auto const monitor_idx = std::distance(&*rules.begin(), &monitor);

				if (monitor.is_active()) {
					++active_monitors_count;

					if (active_monitors_count == 1) {
						/* Log: up to {sample_history_depth} * 2 samples before any event
						 * occurred
						 */
						log_data_id_names(events_log_file, cfg.data_id_definitions);
						log_event_samples(events_log_file,
								  sample_history_buffer,
								  static_cast<uint32_t>(cfg.data_id_definitions.size()),
								  sample_history_buffer.get_num_stored());
					}

					log_event_started(events_log_file, cfg.rule_definitions[monitor_idx]);
				} else {
					--active_monitors_count;

					log_event_completed(events_log_file, cfg.rule_definitions[monitor_idx]);
				}
			}

			std::flush(events_log_file);
		}
	}

	return EXIT_SUCCESS;
}
