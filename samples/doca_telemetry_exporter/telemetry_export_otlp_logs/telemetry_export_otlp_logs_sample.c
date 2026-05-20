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

#include <doca_log.h>
#include <doca_telemetry_exporter.h>

DOCA_LOG_REGISTER(TELEMETRY_EXPORTER::OTLP_LOGS);

doca_error_t telemetry_export_otlp_logs(void)
{
	bool file_write_enable;
	bool ipc_enabled;
	const char *env_ipc;
	const char *env_file_write;
	doca_error_t result;
	struct doca_telemetry_exporter_schema *doca_schema = NULL;
	struct doca_telemetry_exporter_source *doca_source = NULL;
	doca_telemetry_exporter_otlp_logs_resource_id_t resource_id = 0;
	doca_telemetry_exporter_otlp_logs_scope_id_t scope_id = 0;
	/* Event schema attribute types and keys - declared here to avoid branch past initialization */
	doca_telemetry_exporter_otlp_attribute_type_t attr_types[3];
	const char *attr_keys[3];

	DOCA_LOG_INFO("Starting DOCA Telemetry Exporter OTLP Logs sample");

	/* Configure exporters via environment variables */
	env_file_write = getenv("ENABLE_FILE_WRITE");
	file_write_enable = (env_file_write != NULL && strcmp(env_file_write, "1") == 0);
	env_ipc = getenv("ENABLE_IPC");
	ipc_enabled = (env_ipc != NULL && strcmp(env_ipc, "1") == 0);

	if (ipc_enabled)
		DOCA_LOG_INFO("IPC exporter enabled - will send to DTS via IPC sockets");
	else
		DOCA_LOG_INFO("Using direct OTLP HTTP export (no IPC)");

	/* Initialize DOCA Telemetry schema */
	result = doca_telemetry_exporter_schema_init("otlp_logs_example_schema", &doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot init doca schema");
		return result;
	}

	/* Set writable buffer path (required for internal buffers, not for file export) */
	doca_telemetry_exporter_schema_set_buf_data_root(doca_schema, "/tmp/doca_telemetry_buffer");

	/* Enable opaque events - REQUIRED for OTLP logs */
	doca_telemetry_exporter_schema_set_opaque_events_enabled(doca_schema);
	DOCA_LOG_INFO("Opaque events enabled (required for OTLP logs)");

	/* Configure file write if enabled */
	if (file_write_enable)
		doca_telemetry_exporter_schema_set_file_write_enabled(doca_schema);

	/* Configure IPC if enabled */
	if (ipc_enabled)
		doca_telemetry_exporter_schema_set_ipc_enabled(doca_schema);

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

	doca_telemetry_exporter_source_set_id(doca_source, "otlp_logs_source_1");
	doca_telemetry_exporter_source_set_tag(doca_source, "");

	/* Start DOCA Telemetry source */
	result = doca_telemetry_exporter_source_start(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_source");
		goto err_source;
	}

	/* Create OTLP logs context - must be done AFTER starting the source */
	result = doca_telemetry_exporter_otlp_logs_create_context(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create OTLP logs context");
		goto err_source;
	}
	DOCA_LOG_INFO("OTLP logs context created successfully");

	/* Create resource representing the device/node */
	result = doca_telemetry_exporter_otlp_logs_add_resource(doca_source, &resource_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create OTLP logs resource");
		goto err_otlp;
	}
	DOCA_LOG_INFO("Resource created with ID: %zu", resource_id);

	/* Add resource attributes (metadata about the device/service) */
	result = doca_telemetry_exporter_otlp_logs_add_resource_attribute_string(doca_source,
										 resource_id,
										 "service.name",
										 "doca_network_service");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add resource attribute 'service.name'");
		goto err_otlp;
	}

	result = doca_telemetry_exporter_otlp_logs_add_resource_attribute_string(doca_source,
										 resource_id,
										 "host.name",
										 "doca-server-01");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add resource attribute 'host.name'");
		goto err_otlp;
	}

	result = doca_telemetry_exporter_otlp_logs_add_resource_attribute_string(doca_source,
										 resource_id,
										 "device.name",
										 "mlx5_0");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add resource attribute 'device.name'");
		goto err_otlp;
	}

	result = doca_telemetry_exporter_otlp_logs_add_resource_attribute_int(doca_source,
									      resource_id,
									      "device.port",
									      1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add resource attribute 'device.port'");
		goto err_otlp;
	}

	DOCA_LOG_INFO("Resource attributes added successfully");

	/* Define event schema - 3 attributes: message (STRING), error_code (INT), severity (STRING) */
	attr_types[0] = DOCA_TELEMETRY_EXPORTER_OTLP_ATTRIBUTE_TYPE_STRING;
	attr_types[1] = DOCA_TELEMETRY_EXPORTER_OTLP_ATTRIBUTE_TYPE_INT;
	attr_types[2] = DOCA_TELEMETRY_EXPORTER_OTLP_ATTRIBUTE_TYPE_STRING;
	attr_keys[0] = "message";
	attr_keys[1] = "error_code";
	attr_keys[2] = "severity";

	/* Create scope (instrumentation library) */
	result = doca_telemetry_exporter_otlp_logs_add_scope(doca_source,
							     "doca.telemetry.example",
							     "1.0.0",
							     "network_event",
							     "INFO",
							     attr_keys,
							     attr_types,
							     3,
							     &scope_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create scope");
		goto err_otlp;
	}
	DOCA_LOG_INFO("Scope created with ID: %zu", scope_id);

	/* Write first log event */
	doca_telemetry_exporter_otlp_attribute_value_t event1_values[3];
	event1_values[0].string_value = "Connection established successfully";
	event1_values[1].int_value = 0;
	event1_values[2].string_value = "INFO";

	result = doca_telemetry_exporter_otlp_logs_write_event(doca_source, resource_id, scope_id, event1_values);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to write first log event");
		goto err_otlp;
	}
	DOCA_LOG_INFO("First log event written: Connection established");

	/* Write second log event */
	doca_telemetry_exporter_otlp_attribute_value_t event2_values[3];
	event2_values[0].string_value = "Packet loss detected on interface eth0";
	event2_values[1].int_value = 2001;
	event2_values[2].string_value = "WARN";

	result = doca_telemetry_exporter_otlp_logs_write_event_with_severity(doca_source,
									     resource_id,
									     scope_id,
									     event2_values,
									     "WARN");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to write second log event");
		goto err_otlp;
	}
	DOCA_LOG_INFO("Second log event written: Packet loss warning");

	/* Final flush to ensure all logs are sent */
	result = doca_telemetry_exporter_otlp_logs_flush(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to perform final OTLP logs flush");
		goto err_otlp;
	}
	DOCA_LOG_INFO("Final OTLP logs flush completed (written to page buffer)");

	/* Wait for async OTLP export to complete */
	DOCA_LOG_INFO("Waiting for async OTLP export to complete...");
	sleep(5);

	/* Clean up OTLP logs context */
	result = doca_telemetry_exporter_otlp_logs_destroy_context(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy OTLP logs context");
		goto err_source;
	}
	DOCA_LOG_INFO("OTLP logs context destroyed successfully");

	/* Destroy source and schema */
	doca_telemetry_exporter_source_destroy(doca_source);
	doca_telemetry_exporter_schema_destroy(doca_schema);

	DOCA_LOG_INFO("DOCA Telemetry Exporter OTLP Logs sample completed successfully");
	DOCA_LOG_INFO("Sent 1 resource, 1 scope, 2 log events via OTLP HTTP");
	return DOCA_SUCCESS;

err_otlp:
	doca_telemetry_exporter_otlp_logs_destroy_context(doca_source);
err_source:
	doca_telemetry_exporter_source_destroy(doca_source);
err_schema:
	doca_telemetry_exporter_schema_destroy(doca_schema);
	return result;
}
