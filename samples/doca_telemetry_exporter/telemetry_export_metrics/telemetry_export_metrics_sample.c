/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include <doca_log.h>
#include <doca_telemetry_exporter.h>

#define NB_ITERATIONS 10       /* Number of monitoring iterations */
#define NB_INTERFACES 2	       /* Number of network interfaces to monitor */
#define FLUSH_INTERVAL_MS 1000 /* Automatic flush interval in milliseconds */

DOCA_LOG_REGISTER(TELEMETRY_EXPORTER::METRICS);

/* Interface names for demonstration */
static const char *interface_names[NB_INTERFACES] = {"eth0", "eth1"};

/*
 * Generates synthetic network metrics for demonstration purposes.
 *
 * @iteration [in]: Current iteration number
 * @interface_idx [in]: Interface index (0 or 1)
 * @packets_sent [out]: Simulated packets sent
 * @packets_received [out]: Simulated packets received
 * @bandwidth_mbps [out]: Simulated bandwidth in Mbps
 */
static void generate_synthetic_metrics(int32_t iteration,
				       int interface_idx,
				       uint64_t *packets_sent,
				       uint64_t *packets_received,
				       double *bandwidth_mbps)
{
	/* Generate synthetic data that varies with iteration and interface */
	*packets_sent = (1000 + iteration * 100) * (interface_idx + 1);
	*packets_received = (900 + iteration * 95) * (interface_idx + 1);
	*bandwidth_mbps = (50.0 + iteration * 5.5) * (interface_idx + 1);
}

/*
 * Main sample function demonstrating DOCA Telemetry Metrics API.
 * Creates DOCA Telemetry schema and source, then demonstrates:
 * - Metrics context creation
 * - Constant labels
 * - Dynamic label sets
 * - Counter metrics (absolute and incremental)
 * - Gauge metrics
 * - Automatic flush interval
 *
 * Exporter Configuration:
 * - File exporter: Enabled by default (file_write_enable = true)
 * - IPC exporter: Disabled by default (ipc_enabled = false)
 * - Prometheus exporter: Disabled by default (prometheus_enabled = false)
 *   Set prometheus_enabled = true to enable
 *   Metrics exposed at: http://localhost:9090/metrics (or configured endpoint)
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t telemetry_export_metrics(void)
{
	bool file_write_enable = true;	 /* Enables writing to local machine as file */
	bool ipc_enabled = false;	 /* Enables sending to DTS through ipc sockets */
	bool prometheus_enabled = false; /* Enables Prometheus exporter - set endpoint below if enabled */
	const char *prometheus_endpoint = "localhost:9090"; /* Prometheus endpoint (host:port or URL) */
	doca_error_t result;
	int32_t iteration = 0;
	int interface_idx = 0;
	struct doca_telemetry_exporter_schema *doca_schema = NULL;
	struct doca_telemetry_exporter_source *doca_source = NULL;
	doca_telemetry_exporter_label_set_id_t interface_label_set_id = 0;
	uint64_t timestamp = 0;
	uint64_t packets_sent = 0, packets_received = 0;
	uint64_t prev_packets_sent[NB_INTERFACES] = {0};
	double bandwidth_mbps = 0;

	/* Label names for dynamic metrics */
	const char *label_names[] = {"interface"};
	const char *label_values[NB_INTERFACES][1] = {{interface_names[0]}, {interface_names[1]}};

	DOCA_LOG_INFO("Starting DOCA Telemetry Exporter Metrics sample");

	/* Configure Prometheus exporter via environment variable if enabled */
	if (prometheus_enabled) {
		if (setenv("PROMETHEUS_ENDPOINT", prometheus_endpoint, 1) != 0) {
			DOCA_LOG_ERR("Failed to set PROMETHEUS_ENDPOINT environment variable");
			return DOCA_ERROR_INITIALIZATION;
		}
		DOCA_LOG_INFO("Prometheus exporter enabled with endpoint: %s", prometheus_endpoint);
		DOCA_LOG_INFO("Metrics will be exposed at: http://%s/metrics", prometheus_endpoint);
	}

	/* Initialize DOCA Telemetry schema */
	result = doca_telemetry_exporter_schema_init("metrics_example_schema", &doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot init doca schema");
		return result;
	}

	/* Configure file write if enabled */
	if (file_write_enable)
		doca_telemetry_exporter_schema_set_file_write_enabled(doca_schema);

	/* Configure IPC if enabled */
	if (ipc_enabled)
		doca_telemetry_exporter_schema_set_ipc_enabled(doca_schema);

	/*
	 * NOTE: Prometheus exporter is configured via PROMETHEUS_ENDPOINT environment variable
	 * (set above before schema_init if prometheus_enabled is true).
	 *
	 * Configured exporters in this sample:
	 * - file_write_enable: File exporter (writes binary telemetry to disk)
	 * - ipc_enabled: IPC exporter (sends to DOCA Telemetry Service via IPC sockets)
	 * - prometheus_enabled: Prometheus exporter (exposes metrics endpoint for scraping)
	 *
	 * To use Prometheus exporter, set prometheus_enabled = true above,
	 * then metrics will be available at: http://<prometheus_endpoint>/metrics
	 */

	/* Start DOCA Telemetry schema */
	result = doca_telemetry_exporter_schema_start(doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_schema");
		goto err_schema;
	}

	/* Create DOCA Telemetry source */
	result = doca_telemetry_exporter_source_create(doca_schema, &doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create doca_source");
		goto err_schema;
	}

	doca_telemetry_exporter_source_set_id(doca_source, "metrics_source_1");
	doca_telemetry_exporter_source_set_tag(doca_source, "");

	/* Start DOCA Telemetry source */
	result = doca_telemetry_exporter_source_start(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_source");
		goto err_source;
	}

	/* Create metrics context - must be done AFTER starting the source */
	result = doca_telemetry_exporter_metrics_create_context(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create metrics context");
		goto err_source;
	}
	DOCA_LOG_INFO("Metrics context created successfully");

	/* Set automatic flush interval - metrics will be flushed automatically every N milliseconds */
	result = doca_telemetry_exporter_metrics_set_flush_interval_ms(doca_source, FLUSH_INTERVAL_MS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set metrics flush interval");
		goto err_metrics;
	}
	DOCA_LOG_INFO("Automatic flush interval set to %d ms", FLUSH_INTERVAL_MS);

	/* Add constant labels that apply to all metrics */
	result = doca_telemetry_exporter_metrics_add_constant_label(doca_source, "hostname", "doca-server-01");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add constant label 'hostname'");
		goto err_metrics;
	}

	result = doca_telemetry_exporter_metrics_add_constant_label(doca_source, "application", "network_monitor");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add constant label 'application'");
		goto err_metrics;
	}
	DOCA_LOG_INFO("Constant labels added successfully");

	/* Add dynamic label set for interface-specific metrics */
	result = doca_telemetry_exporter_metrics_add_label_names(doca_source, label_names, 1, &interface_label_set_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add label names");
		goto err_metrics;
	}
	DOCA_LOG_INFO("Dynamic label set created with ID: %zu", interface_label_set_id);

	/* Main monitoring loop */
	for (iteration = 0; iteration < NB_ITERATIONS; iteration++) {
		DOCA_LOG_INFO("Iteration %d/%d", iteration + 1, NB_ITERATIONS);

		/* Get current timestamp */
		result = doca_telemetry_exporter_get_timestamp(&timestamp);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get timestamp");
			goto err_metrics;
		}

		/* Report metrics for each interface */
		for (interface_idx = 0; interface_idx < NB_INTERFACES; interface_idx++) {
			/* Generate synthetic metrics */
			generate_synthetic_metrics(iteration,
						   interface_idx,
						   &packets_sent,
						   &packets_received,
						   &bandwidth_mbps);

			DOCA_LOG_INFO("  %s: packets_sent=%" PRIu64 ", packets_received=%" PRIu64
				      ", bandwidth=%.2f Mbps",
				      interface_names[interface_idx],
				      packets_sent,
				      packets_received,
				      bandwidth_mbps);

			/* Report absolute counter for total packets sent */
			result = doca_telemetry_exporter_metrics_add_counter(doca_source,
									     timestamp,
									     "packets_sent_total",
									     packets_sent,
									     interface_label_set_id,
									     label_values[interface_idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add counter 'packets_sent_total'");
				goto err_metrics;
			}

			/* Report absolute counter for total packets received */
			result = doca_telemetry_exporter_metrics_add_counter(doca_source,
									     timestamp,
									     "packets_received_total",
									     packets_received,
									     interface_label_set_id,
									     label_values[interface_idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add counter 'packets_received_total'");
				goto err_metrics;
			}

			/* Report incremental counter showing change in packets sent since last iteration */
			if (iteration > 0) {
				uint64_t increment = packets_sent - prev_packets_sent[interface_idx];
				result = doca_telemetry_exporter_metrics_add_counter_increment(
					doca_source,
					timestamp,
					"packets_sent_increment",
					increment,
					interface_label_set_id,
					label_values[interface_idx]);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to add counter increment 'packets_sent_increment'");
					goto err_metrics;
				}
			}
			prev_packets_sent[interface_idx] = packets_sent;

			/* Report gauge for current bandwidth */
			result = doca_telemetry_exporter_metrics_add_gauge(doca_source,
									   timestamp,
									   "bandwidth_mbps",
									   bandwidth_mbps,
									   interface_label_set_id,
									   label_values[interface_idx]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add gauge 'bandwidth_mbps'");
				goto err_metrics;
			}
		}

		/* Small delay between iterations */
		usleep(1000); /* 1ms */
	}

	/* Final flush to ensure all metrics are sent */
	result = doca_telemetry_exporter_metrics_flush(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to perform final metrics flush");
		goto err_metrics;
	}
	DOCA_LOG_INFO("Final metrics flush completed");

	/* Clean up metrics context */
	result = doca_telemetry_exporter_metrics_destroy_context(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy metrics context");
		goto err_source;
	}
	DOCA_LOG_INFO("Metrics context destroyed successfully");

	/* Destroy source and schema */
	doca_telemetry_exporter_source_destroy(doca_source);
	doca_telemetry_exporter_schema_destroy(doca_schema);

	DOCA_LOG_INFO("DOCA Telemetry Exporter Metrics sample completed successfully");
	return DOCA_SUCCESS;

err_metrics:
	doca_telemetry_exporter_metrics_destroy_context(doca_source);
err_source:
	doca_telemetry_exporter_source_destroy(doca_source);
err_schema:
	doca_telemetry_exporter_schema_destroy(doca_schema);
	return result;
}
