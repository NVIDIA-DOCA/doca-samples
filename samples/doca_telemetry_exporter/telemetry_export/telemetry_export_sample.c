/*
 * Copyright (c) 2021-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include <doca_log.h>
#include <doca_telemetry_exporter.h>

#define NB_EXAMPLE_STRINGS 5	    /* Amount of example strings */
#define MAX_EXAMPLE_STRING_SIZE 256 /* Indicates the max length of string */
#define SINGLE_FIELD_VALUE 1	    /* Indicates the field contains one value */

DOCA_LOG_REGISTER(TELEMETRY_EXPORTER);

static char *example_strings[NB_EXAMPLE_STRINGS] = {"example_str_1",
						    "example_str_2",
						    "example_str_3",
						    "example_str_4",
						    "example_str_5"};

/* Event struct from which report will be serialized */
struct test_event_type {
	doca_telemetry_exporter_timestamp_t timestamp;
	int32_t event_number;
	int32_t iter_number;
	uint64_t string_number;
	char example_string[MAX_EXAMPLE_STRING_SIZE];
} __attribute__((packed));

/*
 * This function fills up event buffer with the example string of specified number.
 * It also saves number of iteration, number of string and overall number of events.
 *
 * @iter_number [in]: Iteration number to insert to event
 * @string_number [in]: String number to insert to event
 * @event [out]: The event getting filled
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_example_event(int32_t iter_number, uint64_t string_number, struct test_event_type *event)
{
	static int collected_example_events_count; /* Initialized to 0 by default, only in first call to function */
	doca_telemetry_exporter_timestamp_t timestamp;
	doca_error_t result = DOCA_SUCCESS;

	result = doca_telemetry_exporter_get_timestamp(&timestamp);
	if (result != DOCA_SUCCESS)
		return result;

	event->timestamp = timestamp;
	event->event_number = collected_example_events_count++;
	event->iter_number = iter_number;
	event->string_number = string_number;
	if (strnlen(example_strings[string_number], MAX_EXAMPLE_STRING_SIZE) >= MAX_EXAMPLE_STRING_SIZE)
		return DOCA_ERROR_INVALID_VALUE;
	strcpy(event->example_string, example_strings[string_number]);
	return result;
}

/*
 * Registers the example fields to the doca_telemetry_exporter_type.
 *
 * @type [out]: The doca_telemetry_exporter_type whose getting registered
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t telemetry_register_fields(struct doca_telemetry_exporter_type *type)
{
	doca_error_t result;
	struct doca_telemetry_exporter_field *field;
	const int nb_fields = 5;
	int idx = 0;
	struct {
		const char *name;
		const char *desc;
		const char *type_name;
		uint16_t len;
	} fields_info[] = {
		{"timestamp", "Event timestamp", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_TIMESTAMP, SINGLE_FIELD_VALUE},
		{"event_number", "Event number", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_INT32, SINGLE_FIELD_VALUE},
		{"iter_num", "Iteration number", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_INT32, SINGLE_FIELD_VALUE},
		{"string_number", "String number", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_UINT64, SINGLE_FIELD_VALUE},
		{"example_string", "String example", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_CHAR, MAX_EXAMPLE_STRING_SIZE},
	};

	for (idx = 0; idx < nb_fields; idx++) {
		result = doca_telemetry_exporter_field_create(&field);
		if (result != DOCA_SUCCESS)
			return result;

		doca_telemetry_exporter_field_set_name(field, fields_info[idx].name);
		doca_telemetry_exporter_field_set_description(field, fields_info[idx].desc);
		doca_telemetry_exporter_field_set_type_name(field, fields_info[idx].type_name);
		doca_telemetry_exporter_field_set_array_len(field, fields_info[idx].len);

		result = doca_telemetry_exporter_type_add_field(type, field);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return result;
}

/*
 * Main sample function.
 * Creates DOCA Telemetry schema and DOCA Telemetry source, prepares events,
 * and sends events through DOCA Telemetry API.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t telemetry_export(void)
{
	bool file_write_enable = true; /* Enables writing to local machine as file */
	bool ipc_enabled = true;       /* Enables sending to DTS through ipc sockets */
	int repetition = 10;	       /* Repetition amount of exporting telemetry */
	doca_error_t result;
	int32_t iteration = 0;
	uint64_t string_number = 0;
	struct doca_telemetry_exporter_schema *doca_schema = NULL;
	struct doca_telemetry_exporter_source *doca_source = NULL;
	struct test_event_type test_event;
	doca_telemetry_exporter_type_index_t example_index;

	/* Event type for DOCA Telemetry schema. Should be consistent with event struct */
	struct doca_telemetry_exporter_type *example_type;

	/* DOCA Telemetry schema initialization and attributes configuration */

	/* Init DOCA schema */
	result = doca_telemetry_exporter_schema_init("example_doca_schema_name", &doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot init doca schema");
		return result;
	}

	/*
	 * Set buffer size in bytes to fit 5 example events. By default it is set to 60K.
	 * Data root should be set to keep data schemas and binary data if file_write
	 * is enabled.
	 */
	doca_telemetry_exporter_schema_set_buf_size(doca_schema, sizeof(test_event) * 5);

	result = doca_telemetry_exporter_type_create(&example_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create type");
		goto err_schema;
	}

	result = telemetry_register_fields(example_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot register fields");
		doca_telemetry_exporter_type_destroy(example_type);
		goto err_schema;
	}

	/*
	 * Enable file write during the app development.
	 * Check written files under data root to make sure that data format is correct.
	 * Default max_file_size is 1 Mb, default max_file_age is 1 hour.
	 */
	if (file_write_enable)
		doca_telemetry_exporter_schema_set_file_write_enabled(doca_schema);

	/*
	 * If IPC is enabled, DOCA Telemetry will try to find DOCA Telemetry Service (DTS) socket
	 * under ipc_sockets_dir. IPC is disabled by default.
	 * Optionally change parameters for IPC connection/reconnection tries
	 * and IPC socket timeout. Default values are 100 msec, 3 tries, and 500 ms accordingly.
	 * see doca_telemetry_exporter_schema_set_ipc_* functions for details.
	 */
	if (ipc_enabled)
		doca_telemetry_exporter_schema_set_ipc_enabled(doca_schema);

	/* Add DOCA Telemetry schema types */
	result = doca_telemetry_exporter_schema_add_type(doca_schema, "example_event", example_type, &example_index);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add type to doca_schema!");
		doca_telemetry_exporter_type_destroy(example_type);
		goto err_schema;
	}

	/* "apply" DOCA Telemetry schema */
	result = doca_telemetry_exporter_schema_start(doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_schema!");
		goto err_schema;
	}

	/* DOCA Telemetry source initialization */

	/* Create DOCA Telemetry Source context from DOCA Telemetry schema */
	result = doca_telemetry_exporter_source_create(doca_schema, &doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create doca_source!");
		goto err_schema;
	}

	doca_telemetry_exporter_source_set_id(doca_source, "source_1");
	doca_telemetry_exporter_source_set_tag(doca_source, "source_1_tag");

	/* Start DOCA Telemetry source to apply attributes and start services */
	result = doca_telemetry_exporter_source_start(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_source!");
		goto err_source;
	}
	/* Create more DOCA Telemetry sources if needed */

	/* Prepare events and report them via DOCA Telemetry */
	for (iteration = 0; iteration < repetition; iteration++) {
		for (string_number = 0; string_number < NB_EXAMPLE_STRINGS; string_number++) {
			DOCA_LOG_INFO("Progressing: k=%" PRId32 " \t i=%" PRIu64, iteration, string_number);
			result = prepare_example_event(iteration, string_number, &test_event);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create event");
				goto err_source;
			}
			result = doca_telemetry_exporter_source_report(doca_source, example_index, &test_event, 1);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Cannot report to doca_source!");
				goto err_source;
			}
		}
		if (iteration % 2 == 0) {
			/*
			 * Optionally force DOCA Telemetry source buffer to flush.
			 * Handy for bursty events or specific event types.
			 */
			result = doca_telemetry_exporter_source_flush(doca_source);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Cannot flush doca_source!");
				goto err_source;
			}
		}
	}

	/* Destroy all DOCA Telemetry sources and DOCA Telemetry schema to clean up */
	doca_telemetry_exporter_source_destroy(doca_source);
	doca_telemetry_exporter_schema_destroy(doca_schema);

	return DOCA_SUCCESS;
err_source:
	doca_telemetry_exporter_source_destroy(doca_source);
err_schema:
	doca_telemetry_exporter_schema_destroy(doca_schema);
	return result;
}
