/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <stdlib.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <samples/common.h>

#include <utils.h>

#include "yara_inspection_core.h"

DOCA_LOG_REGISTER(YARA_APP::Core);

/* This value is guaranteed to be 253 on Linux, and 16 bytes on Windows */
#define MAX_HOSTNAME_LEN 253

/*
 * ARGP Callback - Handle mem_regions.json path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t memr_callback(void *param, void *config)
{
	struct yara_config *conf = (struct yara_config *)config;
	size_t size = sizeof(conf->system_mem_region_path);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("System memory regions map argument too long, must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->system_mem_region_path, param);

	if (access(conf->system_mem_region_path, F_OK) == -1) {
		DOCA_LOG_ERR("System memory regions map json file not found %s", conf->system_mem_region_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle VUID parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vuid_callback(void *param, void *config)
{
	struct yara_config *conf = (struct yara_config *)config;
	size_t size = sizeof(conf->system_vuid);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("System VUID argument too long, must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->system_vuid, param);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DMA device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dma_callback(void *param, void *config)
{
	struct yara_config *conf = (struct yara_config *)config;
	size_t size = sizeof(conf->dma_dev_name);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("DMA device name argument too long, must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->dma_dev_name, param);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle os_symbols.json path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t os_syms_callback(void *param, void *config)
{
	struct yara_config *conf = (struct yara_config *)config;
	size_t size = sizeof(conf->system_os_symbol_map_path);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("System os symbols map argument too long, must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->system_os_symbol_map_path, param);

	if (access(conf->system_os_symbol_map_path, F_OK) == -1) {
		DOCA_LOG_ERR("System os symbols map json file not found %s", conf->system_os_symbol_map_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle time between attestations parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t time_callback(void *param, void *config)
{
	struct yara_config *conf = (struct yara_config *)config;

	conf->time_interval = *(int *)param;
	return DOCA_SUCCESS;
}

doca_error_t register_yara_params(void)
{
	doca_error_t result;
	struct doca_argp_param *memr_param, *vuid_param, *dma_param, *os_syms_param;
	struct doca_argp_param *time_param;

	/* Create and register system memory map param */
	result = doca_argp_param_create(&memr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(memr_param, "m");
	doca_argp_param_set_long_name(memr_param, "memr");
	doca_argp_param_set_arguments(memr_param, "<path>");
	doca_argp_param_set_description(memr_param, "System memory regions map");
	doca_argp_param_set_callback(memr_param, memr_callback);
	doca_argp_param_set_type(memr_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(memr_param);
	result = doca_argp_register_param(memr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register VUID param */
	result = doca_argp_param_create(&vuid_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(vuid_param, "f");
	doca_argp_param_set_long_name(vuid_param, "vuid");
	doca_argp_param_set_description(vuid_param, "VUID of the System device");
	doca_argp_param_set_callback(vuid_param, vuid_callback);
	doca_argp_param_set_type(vuid_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(vuid_param);
	result = doca_argp_register_param(vuid_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DMA param */
	result = doca_argp_param_create(&dma_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dma_param, "d");
	doca_argp_param_set_long_name(dma_param, "dma");
	doca_argp_param_set_description(dma_param, "DMA device name");
	doca_argp_param_set_callback(dma_param, dma_callback);
	doca_argp_param_set_type(dma_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(dma_param);
	result = doca_argp_register_param(dma_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register system OS map param */
	result = doca_argp_param_create(&os_syms_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(os_syms_param, "o");
	doca_argp_param_set_long_name(os_syms_param, "osym");
	doca_argp_param_set_arguments(os_syms_param, "<path>");
	doca_argp_param_set_description(os_syms_param, "System OS symbol map path");
	doca_argp_param_set_callback(os_syms_param, os_syms_callback);
	doca_argp_param_set_type(os_syms_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(os_syms_param);
	result = doca_argp_register_param(os_syms_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register time interval param */
	result = doca_argp_param_create(&time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(time_param, "t");
	doca_argp_param_set_long_name(time_param, "time");
	doca_argp_param_set_arguments(time_param, "<seconds>");
	doca_argp_param_set_description(time_param, "Scan time interval in seconds");
	doca_argp_param_set_callback(time_param, time_callback);
	doca_argp_param_set_type(time_param, DOCA_ARGP_TYPE_INT);
	doca_argp_param_set_mandatory(time_param);
	result = doca_argp_register_param(time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_register_version_callback(sdk_version_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register version callback: %s", doca_error_get_descr(result));
		return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Creates and starts a DOCA Apsh context, in order to make the library usable.
 *
 * @conf [in]: Configuration used for init process
 * @resources [out]: Memory storage for the context pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 *
 * @NOTE: On failure all lib Apsh resources are freed
 */
static doca_error_t yara_ctx_init(struct yara_config *conf, struct yara_resources *resources)
{
	doca_error_t result;

	/* Create a new yara context */
	resources->ctx = doca_apsh_create();
	if (resources->ctx == NULL) {
		DOCA_LOG_ERR("Create lib APSH context failed");
		return DOCA_ERROR_INITIALIZATION;
	}

	/* Get dma device */
	result = open_doca_device_with_ibdev_name((uint8_t *)conf->dma_dev_name,
						  strlen(conf->dma_dev_name),
						  NULL,
						  &resources->dma_device);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Failed to open dma device");
		return result;
	}

	/* Start apsh context */
	/* set the DMA device */
	result = doca_apsh_dma_dev_set(resources->ctx, resources->dma_device);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Set dma device failed");
		return result;
	}

	/* Start apsh handler and init connection to devices */
	result = doca_apsh_start(resources->ctx);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Start APSH failed");
		return result;
	}

	/* return value */
	return DOCA_SUCCESS;
}

/*
 * Creates and starts a DOCA Apsh System context, in order to apply the library on a specific target system.
 *
 * @conf [in]: Configuration used for init process
 * @resources [out]: Memory storage for the system context pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 *
 * @NOTE: On failure all lib Apsh resources are freed
 */
static doca_error_t yara_system_init(struct yara_config *conf, struct yara_resources *resources)
{
	doca_error_t result;

	result = open_doca_device_rep_with_vuid(resources->dma_device,
						DOCA_DEVINFO_REP_FILTER_NET,
						(uint8_t *)conf->system_vuid,
						strlen(conf->system_vuid),
						&resources->system_device);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Failed to open representor device");
		return result;
	}

	/* Create a new system handler to introspect */
	resources->sys = doca_apsh_system_create(resources->ctx);
	if (resources->sys == NULL) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Create system context failed");
		return result;
	}

	/* Start system context - bare-metal */
	/* Set the system os symbol map */
	result = doca_apsh_sys_os_symbol_map_set(resources->sys, conf->system_os_symbol_map_path);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Set os symbols map failed");
		return result;
	}

	/* Set the system memory region the apsh handler is allowed to access */
	result = doca_apsh_sys_mem_region_set(resources->sys, conf->system_mem_region_path);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Set mem regions map failed");
		return result;
	}

	/* Set the system device for the apsh handler to use */
	result = doca_apsh_sys_dev_set(resources->sys, resources->system_device);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Set system device failed");
		return result;
	}

	/* Set the system os type - linux/widows */
	result = doca_apsh_sys_os_type_set(resources->sys, DOCA_APSH_SYSTEM_WINDOWS);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Set system os type failed");
		return result;
	}

	/* Start system handler and init connection to the system and the devices */
	result = doca_apsh_system_start(resources->sys);
	if (result != DOCA_SUCCESS) {
		yara_inspection_cleanup(resources);
		DOCA_LOG_ERR("Start system failed");
		return result;
	}

	/* return value */
	return DOCA_SUCCESS;
}

doca_error_t yara_inspection_init(struct yara_config *conf, struct yara_resources *resources)
{
	doca_error_t result;

	/* Init basic apsh handlers */
	memset(resources, 0, sizeof(*resources));
	result = yara_ctx_init(conf, resources);
	if (result != DOCA_SUCCESS)
		return result;
	return yara_system_init(conf, resources);
}

void yara_inspection_cleanup(struct yara_resources *resources)
{
	/* free the system handler and disconnect from the devices */
	if (resources->sys != NULL) {
		doca_apsh_system_destroy(resources->sys);
		resources->sys = NULL;
	}

	/* free the apsh handler and disconnect from the devices */
	if (resources->ctx != NULL) {
		doca_apsh_destroy(resources->ctx);
		resources->ctx = NULL;
	}

	/* Close the devices */
	if (resources->dma_device != NULL) {
		doca_dev_close(resources->dma_device);
		resources->dma_device = NULL;
	}
	if (resources->system_device != NULL) {
		doca_dev_rep_close(resources->system_device);
		resources->system_device = NULL;
	}
}

/*
 * Register a yara event to the Telemetry schema
 *
 * @schema [in]: Created DOCA Telemetry schema
 * @type_index [out]: Memory storage for the Telemetry index created for the event
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t telemetry_register_yara_event(struct doca_telemetry_exporter_schema *schema,
						  doca_telemetry_exporter_type_index_t *type_index)
{
	doca_error_t result;
	/* Event type for schema. Should be consistent with event struct */
	struct doca_telemetry_exporter_type *type;
	struct doca_telemetry_exporter_field *field;
	const int nb_fields = 5;
	int idx = 0;
	struct {
		const char *name;
		const char *desc;
		const char *type_name;
		uint16_t len;
	} fields_info[] = {
		{"timestamp", "Event timestamp", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_TIMESTAMP, 1},
		{"pid", "Pid", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_INT32, 1},
		{"process_name", "Process name", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_CHAR, MAX_PROCESS_NAME_LEN + 1},
		{"rule_name", "Rule name", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_CHAR, MAX_PATH_LEN + 1},
		{"vad", "Path", DOCA_TELEMETRY_EXPORTER_FIELD_TYPE_INT64, 1},
	};

	result = doca_telemetry_exporter_type_create(&type);
	if (result != DOCA_SUCCESS)
		return result;

	for (idx = 0; idx < nb_fields; idx++) {
		result = doca_telemetry_exporter_field_create(&field);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create field");
			doca_telemetry_exporter_type_destroy(type);
			return result;
		}
		doca_telemetry_exporter_field_set_name(field, fields_info[idx].name);
		doca_telemetry_exporter_field_set_description(field, fields_info[idx].desc);
		doca_telemetry_exporter_field_set_type_name(field, fields_info[idx].type_name);
		doca_telemetry_exporter_field_set_array_len(field, fields_info[idx].len);

		result = doca_telemetry_exporter_type_add_field(type, field);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add field to type");
			doca_telemetry_exporter_field_destroy(field);
			doca_telemetry_exporter_type_destroy(type);
			return result;
		}
	}

	/* Register type */
	result = doca_telemetry_exporter_schema_add_type(schema, "yara_event", type, type_index);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add type to schema");
		doca_telemetry_exporter_type_destroy(type);
	}
	return result;
}

doca_error_t telemetry_start(struct doca_telemetry_exporter_schema **telemetry_schema,
			     struct doca_telemetry_exporter_source **telemetry_source,
			     doca_telemetry_exporter_type_index_t *index)
{
	doca_error_t result;
	struct doca_telemetry_exporter_schema *schema = NULL;
	struct doca_telemetry_exporter_source *source = NULL;
	char source_id_buf[MAX_HOSTNAME_LEN + 1], source_tag_buf[MAX_HOSTNAME_LEN + strlen("_tag") + 1];

	/* Creating telemetry schema */
	result = doca_telemetry_exporter_schema_init("yara_inspection_telemetry", &schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init the doca telemetry schema");
		return result;
	}

	/* Register all currently supported events */
	result = telemetry_register_yara_event(schema, index);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register attestation event in the telemetry schema");
		goto schema_error;
	}

	/* Enable file write during the app development.
	 * Check written files under data root to make sure that data format is correct.
	 * Default max_file_size is 1 Mb, default max_file_age is 1 hour.
	 */
	// doca_telemetry_exporter_schema_set_file_write_enabled(schema);
	// doca_telemetry_exporter_schema_set_file_write_max_size(schema, 1 * 1024 * 1024);
	// doca_telemetry_exporter_schema_set_file_write_max_age(schema, 60 * 60 * 1000000L);

	/* Activate the schema */
	result = doca_telemetry_exporter_schema_start(schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start the doca telemetry schema");
		goto schema_error;
	}

	/* Open a telemetry connection with custom source id and tag */
	result = doca_telemetry_exporter_source_create(schema, &source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create a source end point to the telemetry");
		goto schema_error;
	}

	/* Creating a unique tag and id per host */
	if (gethostname(source_id_buf, sizeof(source_id_buf)) < 0) {
		DOCA_LOG_ERR("Gethostname failed, can't create a unique source tag and id");
		result = DOCA_ERROR_OPERATING_SYSTEM;
		goto source_error;
	}

	strlcpy(source_tag_buf, source_id_buf, sizeof(source_tag_buf));
	strlcat(source_tag_buf, "_tag", sizeof(source_tag_buf));
	doca_telemetry_exporter_source_set_id(source, source_id_buf);
	doca_telemetry_exporter_source_set_tag(source, source_tag_buf);

	/* Initiate the DOCA telemetry source */
	result = doca_telemetry_exporter_source_start(source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to establish a source connection to the telemetry");
		goto source_error;
	}

	/* Success init, return handlers */
	*telemetry_schema = schema;
	*telemetry_source = source;
	return DOCA_SUCCESS;

source_error:
	doca_telemetry_exporter_source_destroy(source);
schema_error:
	doca_telemetry_exporter_schema_destroy(schema);
	return result;
}

void telemetry_destroy(struct doca_telemetry_exporter_schema *telemetry_schema,
		       struct doca_telemetry_exporter_source *telemetry_source)
{
	doca_telemetry_exporter_source_destroy(telemetry_source);
	doca_telemetry_exporter_schema_destroy(telemetry_schema);
}
