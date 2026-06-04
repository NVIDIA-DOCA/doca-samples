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

#include "opentelemetry_core.hpp"

#include <chrono>
#include <cinttypes>
#include <thread>
#include <signal.h>
#include <stdio.h>

#include <doca_log.h>

/* OpenTelemetry Metric and Exporter headers */
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h"

namespace resource = opentelemetry::sdk::resource;
namespace otlp = opentelemetry::exporter::otlp;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace metrics_api = opentelemetry::metrics;

DOCA_LOG_REGISTER(OPENTELEMETRY_CORE);

#define DIAG_FORCE_OWNERSHIP 1 /* Flag to give app's diag context ownership of the firmware diag buffers */

#define EXPORT_TIMEOUT 1000			 /* Set the timeout value to 1 second */
#define OPENTELEMETRY_EXPORT_DEFAULT_PORT "4318" /* Default port number to use for OpenTelemetry exports */

/* Control value to exit poll loop - triggered by ctrl+c interrupt */
static volatile int end_poll = 0;

/*
 * Signal function to catch ctrl+c and break polling loop
 *
 * @unused [in]: Ignored parameter
 */
static void sigint_handler(int unused)
{
	(void)unused;
	end_poll = 1;
}

static void opentel_metrics_init(const struct opentelemetry_cfg *app_cfg, struct opentelemetry_ctx *opentel_ctx)
{
	/* Set attributes and create a description of the resource generating the telemetry */
	auto resource_attributes = resource::ResourceAttributes{{"service.name", "DOCA-OpenTelemetry-ref-app"}};
	auto resource_ptr = resource::Resource::Create(resource_attributes);

	/* Define input options and configure a OpenTelemetry protocol HTTP metric exporter */
	otlp::OtlpHttpMetricExporterOptions opts;
	opts.url = "http://" + app_cfg->export_ip + ":" + OPENTELEMETRY_EXPORT_DEFAULT_PORT + "/v1/metrics";
	auto exporter = std::make_unique<otlp::OtlpHttpMetricExporter>(opts);

	/* Create a reader to periodically export telemetry and link with the exporter */
	metrics_sdk::PeriodicExportingMetricReaderOptions reader_opts;
	reader_opts.export_interval_millis = std::chrono::milliseconds(app_cfg->export_time);
	reader_opts.export_timeout_millis = std::chrono::milliseconds(EXPORT_TIMEOUT);
	auto reader = std::make_unique<metrics_sdk::PeriodicExportingMetricReader>(std::move(exporter), reader_opts);

	/* Create a meter provider to handle meter instances in the app */
	opentel_ctx->meter_provider = std::shared_ptr<metrics_sdk::MeterProvider>(
		new metrics_sdk::MeterProvider(std::make_unique<metrics_sdk::ViewRegistry>(), resource_ptr));

	/* Add the reader to the provider */
	opentel_ctx->meter_provider->AddMetricReader(std::move(reader));

	/* Register the provider as global for the metrics API */
	metrics_api::Provider::SetMeterProvider(
		opentelemetry::nostd::shared_ptr<metrics_api::MeterProvider>(opentel_ctx->meter_provider));
}

static void opentel_counters_init(const struct opentelemetry_cfg *app_cfg, struct opentelemetry_ctx *opentel_ctx)
{
	/* Get the global provider and create a custom meter - meter is a shared_ptr so cleaned automatically */
	auto meter_provider = metrics_api::Provider::GetMeterProvider();
	auto meter = meter_provider->GetMeter("DOCA_Ref_App");
	const struct diag_data_id *diag_id;

	/* Add a counter for each input data_id */
	auto num_counters = app_cfg->diag_data.size();
	for (size_t i = 0; i < num_counters; i++) {
		diag_id = static_cast<const struct diag_data_id *>(&app_cfg->diag_data[i]);

		/* Create a new OpenTelemetry Counter based on the input information */
		auto counter = meter->CreateUInt64Counter(diag_id->name, diag_id->desc, diag_id->unit);

		opentel_ctx->counters.push_back(std::move(counter));

		/* Create labels for the counter */
		std::map<std::string, std::string> label;

		/* Add host and device PCIe address as standard */
		label.emplace("host.name", std::string(app_cfg->host_name));
		label.emplace("device.pcie", std::string(app_cfg->dev_pci_addr));

		if (diag_id->port != DATA_ID_NO_PORT)
			label.emplace("hw.network.io.physical.port", std::to_string(diag_id->port));
		if (diag_id->priority != DATA_ID_NO_PRIORITY)
			label.emplace("hw.network.io.priority", std::to_string(diag_id->priority));

		label.emplace("hw.network.io.direction", diag_id->dir);

		opentel_ctx->labels.push_back(std::move(label));
	}
}

static doca_error_t open_doca_dev(const char *dev_pci, struct doca_dev **dev) noexcept
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0, i;
	uint8_t is_equal = 0;
	doca_error_t result, ret;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	ret = DOCA_ERROR_NOT_FOUND;

	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_is_equal_pci_addr(dev_list[i], dev_pci, &is_equal);
		if (result != DOCA_SUCCESS)
			continue;

		if (is_equal == 0)
			continue;

		result = doca_dev_open(dev_list[i], dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open doca_device: %s", doca_error_get_descr(result));
			ret = result;
			break;
		}

		/* Device is found and open so stop checking */
		ret = DOCA_SUCCESS;
		break;
	}

	doca_devinfo_destroy_list(dev_list);

	return ret;
}

doca_error_t opentelemetry_init(const struct opentelemetry_cfg *app_cfg, struct opentelemetry_ctx *opentel_ctx) noexcept
{
	auto result = open_doca_dev(app_cfg->dev_pci_addr, &opentel_ctx->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open doca device %s: %s", app_cfg->dev_pci_addr, doca_error_get_descr(result));
		return result;
	}

	/* Create and configure a DOCA telemetry Diag context */
	auto num_data_ids = app_cfg->diag_data.size();
	uint32_t max_supported_data_ids = 0;
	uint64_t counter_id_failure = 0;
	std::vector<uint64_t> data_ids(num_data_ids);
	uint8_t sample_mode_sup = 0;

	/* Verify the device caps meet the app configuration requirements */
	result = doca_telemetry_diag_cap_is_supported(doca_dev_as_devinfo(opentel_ctx->dev));
	if (result == DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Device does not support DOCA Telemetry Diag");
		goto close_dev;
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check support for DOCA Telemetry Diag: %s", doca_error_get_descr(result));
		goto close_dev;
	}

	result = doca_telemetry_diag_cap_get_max_num_data_ids(doca_dev_as_devinfo(opentel_ctx->dev),
							      &max_supported_data_ids);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get maximum number of supported data_ids: %s", doca_error_get_descr(result));
		goto close_dev;
	}

	if (num_data_ids > max_supported_data_ids) {
		DOCA_LOG_ERR("Requested data ids (%lu) is > max supported on device (%u)",
			     num_data_ids,
			     max_supported_data_ids);
		result = DOCA_ERROR_NOT_SUPPORTED;
		goto close_dev;
	}

	result = doca_telemetry_diag_cap_is_sample_mode_supported(doca_dev_as_devinfo(opentel_ctx->dev),
								  DOCA_TELEMETRY_DIAG_SAMPLE_MODE_ON_DEMAND,
								  &sample_mode_sup);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check sample mode support: %s", doca_error_get_descr(result));
		goto close_dev;
	}

	if (sample_mode_sup == 0) {
		DOCA_LOG_ERR("Device does not support ON_DEMAND sample mode");
		result = DOCA_ERROR_NOT_SUPPORTED;
		goto close_dev;
	}

	/* Device has necessary caps so move ahead with context creation */
	result = doca_telemetry_diag_create(opentel_ctx->dev, DIAG_FORCE_OWNERSHIP, &opentel_ctx->diag);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create telemetry diag context: %s", doca_error_get_descr(result));
		goto close_dev;
	}

	/* Configure the telemetry Diag context to on-demand mode */
	result = doca_telemetry_diag_set_sample_mode(opentel_ctx->diag, DOCA_TELEMETRY_DIAG_SAMPLE_MODE_ON_DEMAND);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diag sample_mode: %s", doca_error_get_descr(result));
		goto destroy_diag;
	}

	result = doca_telemetry_diag_set_output_format(opentel_ctx->diag, DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diag output_format: %s", doca_error_get_descr(result));
		goto destroy_diag;
	}

	/* No sync allows access to individual port stats */
	result = doca_telemetry_diag_set_sync_mode(opentel_ctx->diag, DOCA_TELEMETRY_DIAG_SYNC_MODE_NO_SYNC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diag sync_mode: %s", doca_error_get_descr(result));
		goto destroy_diag;
	}

	/* No sample period is required for on demand mode */
	result = doca_telemetry_diag_set_sample_period(opentel_ctx->diag, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diag sample_period: %s", doca_error_get_descr(result));
		goto destroy_diag;
	}

	/* On demand mode only returns 1 sample at a time */
	result = doca_telemetry_diag_set_log_max_num_samples(opentel_ctx->diag, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diag log_max_num_samples: %s", doca_error_get_descr(result));
		goto destroy_diag;
	}

	result = doca_telemetry_diag_set_max_num_data_ids(opentel_ctx->diag, num_data_ids);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set diag max_num_data_ids: %s", doca_error_get_descr(result));
		goto destroy_diag;
	}

	/* Apply the configuration to the diag context */
	result = doca_telemetry_diag_apply_config(opentel_ctx->diag);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply diag config: %ss", doca_error_get_descr(result));
		goto destroy_diag;
	}

	/* Once the context is configured, the data_ids can be applied */
	for (size_t i = 0; i < num_data_ids; i++)
		data_ids[i] = app_cfg->diag_data[i].data_id;

	result = doca_telemetry_diag_apply_counters_list_by_id(opentel_ctx->diag,
							       data_ids.data(),
							       num_data_ids,
							       &counter_id_failure);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply diag counters list: %s, counter_id_failure=0x%016lx",
			     doca_error_get_descr(result),
			     counter_id_failure);
		goto destroy_diag;
	}

	try {
		opentel_metrics_init(app_cfg, opentel_ctx);
	} catch (const std::bad_alloc &) {
		DOCA_LOG_ERR("Memory failure when initializing OpenTelemetry Metrics");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_diag;
	} catch (...) {
		DOCA_LOG_ERR("Failed to initialize OpenTelemetry Metrics");
		result = DOCA_ERROR_INITIALIZATION;
		goto destroy_diag;
	}

	try {
		opentel_counters_init(app_cfg, opentel_ctx);
	} catch (const std::bad_alloc &) {
		DOCA_LOG_ERR("Memory failure when initializing OpenTelemetry Counters");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_diag;
	} catch (...) {
		DOCA_LOG_ERR("Failed to initialize OpenTelemetry Counters");
		result = DOCA_ERROR_INITIALIZATION;
		goto destroy_diag;
	}

	return result;

destroy_diag:
	(void)doca_telemetry_diag_destroy(opentel_ctx->diag);
close_dev:
	(void)doca_dev_close(opentel_ctx->dev);

	return result;
}

doca_error_t opentelemetry_destroy(struct opentelemetry_ctx *opentel_ctx) noexcept
{
	doca_error_t result;

	if (opentel_ctx->diag != nullptr) {
		result = doca_telemetry_diag_destroy(opentel_ctx->diag);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca diag context. err=%s", doca_error_get_descr(result));
			return result;
		}
		opentel_ctx->diag = nullptr;
	}

	if (opentel_ctx->dev != nullptr) {
		result = doca_dev_close(opentel_ctx->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca device: %s", doca_error_get_descr(result));
			return result;
		}
		opentel_ctx->dev = nullptr;
	}

	return DOCA_SUCCESS;
}

doca_error_t opentelemetry_run(const struct opentelemetry_cfg *app_cfg, struct opentelemetry_ctx *opentel_ctx) noexcept
{
	signal(SIGINT, sigint_handler);

	/* Set sleep time to half the export time - counters should be updated twice per export period */
	auto sleep_time = app_cfg->export_time / 2;

	/* Start the diag context */
	auto result = doca_telemetry_diag_start(opentel_ctx->diag);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA telemetry diag with error=%s", doca_error_get_name(result));
		return result;
	}

	/* Determine the size of each sample */
	uint32_t size_of_sample;
	result = doca_telemetry_diag_get_sample_size(opentel_ctx->diag, &size_of_sample);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get diag sample size with error=%s", doca_error_get_name(result));
		(void)doca_telemetry_diag_stop(opentel_ctx->diag);
		return result;
	}

	/* Allocate a buffer to receive sample - in 'on demand' mode only one sample is retrieved at a time */
	uint8_t *diag_buf;
	diag_buf = (uint8_t *)malloc(size_of_sample);
	if (diag_buf == nullptr) {
		DOCA_LOG_ERR("Failed to allocate memory for diag buffer");
		(void)doca_telemetry_diag_stop(opentel_ctx->diag);
		return DOCA_ERROR_NO_MEMORY;
	}

	auto num_counters = opentel_ctx->counters.size();
	std::vector<uint64_t> last_counter(num_counters, 0);
	uint32_t samples_read = 0;
	auto verbose = app_cfg->verbose;
	uint64_t poll_count = 0;

	while (end_poll == 0) {
		result = doca_telemetry_diag_query_counters(opentel_ctx->diag,
							    diag_buf,
							    1 /* Max samples per read */,
							    &samples_read);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query diag counters with error=%s", doca_error_get_name(result));
			break;
		}

		if (samples_read == 0)
			continue;

		poll_count++;

		struct doca_telemetry_diag_data_sample_format_0 *sample =
			(struct doca_telemetry_diag_data_sample_format_0 *)diag_buf;

		/* Read the data from the sample and update the counters */
		for (size_t i = 0; i < num_counters; i++) {
			/* Get the label associated with the counter */
			auto label = opentel_ctx->labels[i];
			auto label_kv = opentelemetry::common::KeyValueIterableView<decltype(label)>{label};

			/* Diag returns full counter but OpenTelemetry expects the delta */
			opentel_ctx->counters[i]->Add(sample->value[i].data_value - last_counter[i], label_kv);
			last_counter[i] = sample->value[i].data_value;

			if (verbose == 1) {
				DOCA_LOG_INFO("Poll %" PRIu64 ", 0x%" PRIx64 ",%s: %" PRIu64,
					      poll_count,
					      app_cfg->diag_data[i].data_id,
					      app_cfg->diag_data[i].name.c_str(),
					      sample->value[i].data_value);
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
	}

	auto tmp_result = doca_telemetry_diag_stop(opentel_ctx->diag);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA telemetry diag with error=%s", doca_error_get_name(tmp_result));
	}

	free(diag_buf);

	return result;
}
