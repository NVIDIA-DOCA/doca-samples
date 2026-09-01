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

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_mgmt.h>

#include "mgmt_common.h"

DOCA_LOG_REGISTER(MGMT_RAW::MAIN);

/* Configuration struct */
enum doca_mgmt_raw_cmds {
	DOCA_MGMT_RAW_CMD_NONE,
	DOCA_MGMT_RAW_CMD_LIST_TARGETS,
	DOCA_MGMT_RAW_CMD_REGISTER_LIST,
	DOCA_MGMT_RAW_CMD_REGISTER_DESCRIBE,
	DOCA_MGMT_RAW_CMD_REGISTER_GET,
	DOCA_MGMT_RAW_CMD_REGISTER_SET,
	DOCA_MGMT_RAW_CMD_HCA_CAP_LIST,
	DOCA_MGMT_RAW_CMD_HCA_CAP_DESCRIBE,
	DOCA_MGMT_RAW_CMD_HCA_CAP_GET,
	DOCA_MGMT_RAW_CMD_HCA_CAP_SET,
};

struct mgmt_raw_register_config {
	/* Describe command configuration */
	struct {
		char *register_name; /* Register name for fields info */
	} describe_params;

	/* Get command configuration */
	struct {
		char *register_name; /* Register name */
		char *input_fields;  /* Input fields string */
		char *output_fields; /* Output fields string */
	} get_params;

	/* Set command configuration */
	struct {
		char *register_name; /* Register name */
		char *input_fields;  /* Input fields string */
	} set_params;
};

struct mgmt_raw_hca_cap_config {
	/* Describe command configuration */
	struct {
		char *cap_type; /* HCA cap type name for fields info */
	} describe_params;

	/* Get command configuration */
	struct {
		char *cap_type;	     /* HCA cap type name */
		char *cap_mode;	     /* Cap mode */
		char *output_fields; /* Output fields string */
		bool has_vhca_id;    /* If true, target is OTHER_VHCA_ID */
		uint16_t vhca_id;
		bool has_function_id; /* If true, target is OTHER_FUNCTION_ID */
		uint16_t function_id;
	} get_params;

	/* Set command configuration */
	struct {
		char *cap_type;	    /* HCA cap type name */
		char *input_fields; /* Input fields string */
		bool has_vhca_id;   /* If true, target is OTHER_VHCA_ID */
		uint16_t vhca_id;
		bool has_function_id; /* If true, target is OTHER_FUNCTION_ID */
		uint16_t function_id;
	} set_params;
};

struct mgmt_raw_config {
	/* Common configuration */
	enum doca_mgmt_raw_cmds cmd;		 /* Command to execute */
	char target[MGMT_COMMON_TARGET_STR_LEN]; /* Device management target string */

	/* Sub-commands configuration */
	union {
		struct mgmt_raw_register_config register_config;
		struct mgmt_raw_hca_cap_config hca_cap_config;
	};
};

/* Sample's Logic */
doca_error_t mgmt_raw_register_list(const char *target_string);
doca_error_t mgmt_raw_register_describe(const char *target_string, const char *register_name);
doca_error_t mgmt_raw_register_get(const char *target_string,
				   const char *register_name,
				   const char *input_fields_str,
				   const char *output_fields_str);
doca_error_t mgmt_raw_register_get_all(const char *target_string,
				       const char *register_name,
				       const char *input_fields_str);
doca_error_t mgmt_raw_register_set(const char *target_string, const char *register_name, const char *input_fields_str);

doca_error_t mgmt_raw_hca_cap_list(const char *target_string);
doca_error_t mgmt_raw_hca_cap_describe(const char *target_string, const char *cap_type);
doca_error_t mgmt_raw_hca_cap_get(const char *target_string,
				  const char *cap_type,
				  const char *cap_mode,
				  uint16_t function_id,
				  bool has_function_id,
				  uint16_t vhca_id,
				  bool has_vhca_id,
				  const char *output_fields_str);
doca_error_t mgmt_raw_hca_cap_get_all(const char *target_string,
				      const char *cap_type,
				      const char *cap_mode,
				      uint16_t function_id,
				      bool has_function_id,
				      uint16_t vhca_id,
				      bool has_vhca_id);
doca_error_t mgmt_raw_hca_cap_set(const char *target_string,
				  const char *cap_type,
				  uint16_t function_id,
				  bool has_function_id,
				  uint16_t vhca_id,
				  bool has_vhca_id,
				  const char *input_fields_str);

/*
 * ARGP Callback - Handle target parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t target_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	return mgmt_common_target_string_copy((const char *)param, conf->target);
}

/*
 * ARGP Callback - Handle list-targets command
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t list_targets_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_LIST_TARGETS;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle register register-name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_name_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *register_name = (const char *)param;
	char *register_name_dup = NULL;

	register_name_dup = strdup(register_name);
	if (register_name_dup == NULL) {
		fprintf(stderr, "Failed to allocate memory for register name\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	if (conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_DESCRIBE) {
		conf->register_config.describe_params.register_name = register_name_dup;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_GET) {
		conf->register_config.get_params.register_name = register_name_dup;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_SET) {
		conf->register_config.set_params.register_name = register_name_dup;
	} else {
		free(register_name_dup);
		fprintf(stderr, "Invalid command: %d\n", conf->cmd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle register input-fields parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_input_fields_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *input_fields = (const char *)param;
	char *input_fields_dup = NULL;

	input_fields_dup = strdup(input_fields);
	if (input_fields_dup == NULL) {
		fprintf(stderr, "Failed to allocate memory for input fields\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	if (conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_GET) {
		conf->register_config.get_params.input_fields = input_fields_dup;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_SET) {
		conf->register_config.set_params.input_fields = input_fields_dup;
	} else {
		free(input_fields_dup);
		fprintf(stderr, "Invalid command: %d\n", conf->cmd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle register output-fields parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_output_fields_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *output_fields = (const char *)param;

	conf->register_config.get_params.output_fields = strdup(output_fields);
	if (conf->register_config.get_params.output_fields == NULL) {
		fprintf(stderr, "Failed to allocate memory for output fields\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle hca-cap cap-type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_cap_type_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *cap_type = (const char *)param;
	char *cap_type_dup;

	cap_type_dup = strdup(cap_type);
	if (cap_type_dup == NULL) {
		fprintf(stderr, "Failed to allocate memory for cap type\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_DESCRIBE) {
		conf->hca_cap_config.describe_params.cap_type = cap_type_dup;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_GET) {
		conf->hca_cap_config.get_params.cap_type = cap_type_dup;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_SET) {
		conf->hca_cap_config.set_params.cap_type = cap_type_dup;
	} else {
		free(cap_type_dup);
		fprintf(stderr, "Invalid command: %d\n", conf->cmd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle hca-cap cap-mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_cap_mode_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *cap_mode = (const char *)param;

	if (strcasecmp(cap_mode, "current") != 0 && strcasecmp(cap_mode, "max") != 0) {
		fprintf(stderr, "Invalid cap mode '%s'; expected 'current' or 'max'\n", cap_mode);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->hca_cap_config.get_params.cap_mode = strdup(cap_mode);
	if (conf->hca_cap_config.get_params.cap_mode == NULL) {
		fprintf(stderr, "Failed to allocate memory for cap mode\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle hca-cap function-id parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_function_id_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	int function_id = *(int *)param;

	if (function_id < 0 || function_id > UINT16_MAX) {
		fprintf(stderr, "Function ID must be in range 0-%u\n", UINT16_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_SET) {
		conf->hca_cap_config.set_params.function_id = function_id;
		conf->hca_cap_config.set_params.has_function_id = true;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_GET) {
		conf->hca_cap_config.get_params.function_id = function_id;
		conf->hca_cap_config.get_params.has_function_id = true;
	} else {
		fprintf(stderr, "Invalid command: %d\n", conf->cmd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle hca-cap vhca-id parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_vhca_id_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	int vhca_id = *(int *)param;

	if (vhca_id < 0 || vhca_id > UINT16_MAX) {
		fprintf(stderr, "VHCA ID must be in range 0-%u\n", UINT16_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_SET) {
		conf->hca_cap_config.set_params.vhca_id = vhca_id;
		conf->hca_cap_config.set_params.has_vhca_id = true;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_GET) {
		conf->hca_cap_config.get_params.vhca_id = vhca_id;
		conf->hca_cap_config.get_params.has_vhca_id = true;
	} else {
		fprintf(stderr, "Invalid command: %d\n", conf->cmd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle hca-cap output-fields parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_output_fields_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *output_fields = (const char *)param;

	conf->hca_cap_config.get_params.output_fields = strdup(output_fields);
	if (conf->hca_cap_config.get_params.output_fields == NULL) {
		fprintf(stderr, "Failed to allocate memory for output fields\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle hca-cap input-fields parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_input_fields_callback(void *param, void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;
	const char *input_fields = (const char *)param;

	conf->hca_cap_config.set_params.input_fields = strdup(input_fields);
	if (conf->hca_cap_config.set_params.input_fields == NULL) {
		fprintf(stderr, "Failed to allocate memory for input fields\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * Register device parameter
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_device_param(struct doca_argp_cmd *cmd)
{
	return mgmt_common_register_target_dev_param(cmd, target_callback, true, NULL);
}

/*
 * Register register-name parameter
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_name_param(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *register_name_param;
	doca_error_t result;

	result = doca_argp_param_create(&register_name_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(register_name_param, "r");
	doca_argp_param_set_long_name(register_name_param, "register-name");
	doca_argp_param_set_arguments(register_name_param, "<register>");
	doca_argp_param_set_description(register_name_param, "Name of the register");
	doca_argp_param_set_callback(register_name_param, register_register_name_callback);
	doca_argp_param_set_type(register_name_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(register_name_param);
	result = doca_argp_cmd_register_param(cmd, register_name_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register register input-fields parameter
 *
 * @cmd [in]: ARGP command
 * @is_optional [in]: Whether the parameter is optional
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_input_fields_param(struct doca_argp_cmd *cmd, bool is_optional)
{
	struct doca_argp_param *input_fields_param;
	char *input_fields_description;
	doca_error_t result;

	input_fields_description =
		is_optional ? "Optional input fields in format 'field_name1=value1,field_name2=value2,...'" :
			      "Input fields in format 'field_name1=value1,field_name2=value2,...'";

	result = doca_argp_param_create(&input_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(input_fields_param, "i");
	doca_argp_param_set_long_name(input_fields_param, "input-fields");
	doca_argp_param_set_arguments(input_fields_param, "<field_name1=value1,field_name2=value2,...>");
	doca_argp_param_set_description(input_fields_param, input_fields_description);
	if (!is_optional)
		doca_argp_param_set_mandatory(input_fields_param);
	doca_argp_param_set_callback(input_fields_param, register_input_fields_callback);
	doca_argp_param_set_type(input_fields_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, input_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register register list command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_list_params(struct doca_argp_cmd *cmd)
{
	return register_device_param(cmd);
}

/*
 * Register register describe command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_describe_params(struct doca_argp_cmd *cmd)
{
	doca_error_t result;

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_register_name_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Register register get command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_get_params(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *output_fields_param;
	doca_error_t result;

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_register_name_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_register_input_fields_param(cmd, true);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&output_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(output_fields_param, "o");
	doca_argp_param_set_long_name(output_fields_param, "output-fields");
	doca_argp_param_set_arguments(output_fields_param, "<field_name1,field_name2,...>");
	doca_argp_param_set_description(
		output_fields_param,
		"Optional output fields in format 'field_name1,field_name2,...'. If not specified, all fields will be returned.");
	doca_argp_param_set_callback(output_fields_param, register_output_fields_callback);
	doca_argp_param_set_type(output_fields_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, output_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register register set command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_set_params(struct doca_argp_cmd *cmd)
{
	doca_error_t result;

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_register_name_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_register_input_fields_param(cmd, false);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Register list command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_list_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_REGISTER_LIST;
	return DOCA_SUCCESS;
}

/*
 * Register describe command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_describe_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_REGISTER_DESCRIBE;
	return DOCA_SUCCESS;
}

/*
 * Register get command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_get_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_REGISTER_GET;
	return DOCA_SUCCESS;
}

/*
 * Register set command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_set_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_REGISTER_SET;
	return DOCA_SUCCESS;
}

/*
 * Register register command and its sub-commands
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_register_cmd(void)
{
	struct doca_argp_cmd *cmd_register = NULL;
	struct doca_argp_cmd *cmd_list = NULL;
	struct doca_argp_cmd *cmd_describe = NULL;
	struct doca_argp_cmd *cmd_get = NULL;
	struct doca_argp_cmd *cmd_set = NULL;
	doca_error_t result;

	result = doca_argp_cmd_create(&cmd_register);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_register, "register");
	doca_argp_cmd_set_description(cmd_register, "Set and get registers of a device");

	/* Register list sub-command */
	result = doca_argp_cmd_create(&cmd_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_list, "list");
	doca_argp_cmd_set_description(cmd_list, "List available registers of a device");
	doca_argp_cmd_set_callback(cmd_list, register_list_callback);
	result = register_register_list_params(cmd_list);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_register, cmd_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register register list command: %s", doca_error_get_descr(result));
		cmd_list = NULL;
		goto cleanup;
	}
	cmd_list = NULL;

	/* Register describe sub-command */
	result = doca_argp_cmd_create(&cmd_describe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_describe, "describe");
	doca_argp_cmd_set_description(cmd_describe, "List available fields of a register");
	doca_argp_cmd_set_callback(cmd_describe, register_describe_callback);
	result = register_register_describe_params(cmd_describe);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_register, cmd_describe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register register describe command: %s", doca_error_get_descr(result));
		cmd_describe = NULL;
		goto cleanup;
	}
	cmd_describe = NULL;

	/* Register get sub-command */
	result = doca_argp_cmd_create(&cmd_get);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_get, "get");
	doca_argp_cmd_set_description(cmd_get, "Query a register of a device");
	doca_argp_cmd_set_callback(cmd_get, register_get_callback);
	result = register_register_get_params(cmd_get);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_register, cmd_get);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register register get command: %s", doca_error_get_descr(result));
		cmd_get = NULL;
		goto cleanup;
	}
	cmd_get = NULL;

	/* Register set sub-command */
	result = doca_argp_cmd_create(&cmd_set);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_set, "set");
	doca_argp_cmd_set_description(cmd_set, "Modify a register of a device");
	doca_argp_cmd_set_callback(cmd_set, register_set_callback);
	result = register_register_set_params(cmd_set);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_register, cmd_set);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register register set command: %s", doca_error_get_descr(result));
		cmd_set = NULL;
		goto cleanup;
	}
	cmd_set = NULL;

	result = doca_argp_register_cmd(cmd_register);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register register command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;

cleanup:
	if (cmd_set != NULL)
		(void)doca_argp_cmd_destroy(cmd_set);
	if (cmd_get != NULL)
		(void)doca_argp_cmd_destroy(cmd_get);
	if (cmd_describe != NULL)
		(void)doca_argp_cmd_destroy(cmd_describe);
	if (cmd_list != NULL)
		(void)doca_argp_cmd_destroy(cmd_list);
	if (cmd_register != NULL)
		(void)doca_argp_cmd_destroy(cmd_register);

	return result;
}

/*
 * Register hca-cap cap type parameter
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_cap_type_param(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *cap_type_param;
	doca_error_t result;

	result = doca_argp_param_create(&cap_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cap_type_param, "c");
	doca_argp_param_set_long_name(cap_type_param, "cap-type");
	doca_argp_param_set_arguments(cap_type_param, "<type>");
	doca_argp_param_set_description(cap_type_param, "Name of the HCA capability type");
	doca_argp_param_set_callback(cap_type_param, hca_cap_cap_type_callback);
	doca_argp_param_set_type(cap_type_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(cap_type_param);
	result = doca_argp_cmd_register_param(cmd, cap_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register hca-cap target parameters
 *
 * @cmd [in]: ARGP command
 * @is_optional [in]: Whether the parameter is optional
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_target_params(struct doca_argp_cmd *cmd, bool is_optional)
{
	struct doca_argp_param *function_id_param, *vhca_id_param;
	char *function_id_description, *vhca_id_description;
	doca_error_t result;

	if (is_optional) {
		function_id_description =
			"Optional function ID of the target function (mutually exclusive with -v/--vhca-id)";
		vhca_id_description =
			"Optional VHCA ID of the target function (mutually exclusive with -f/--function-id)";
	} else {
		function_id_description = "Function ID of the target function (mutually exclusive with -v/--vhca-id)";
		vhca_id_description = "VHCA ID of the target function (mutually exclusive with -f/--function-id)";
	}

	result = doca_argp_param_create(&function_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(function_id_param, "f");
	doca_argp_param_set_long_name(function_id_param, "function-id");
	doca_argp_param_set_arguments(function_id_param, "<id>");
	doca_argp_param_set_description(function_id_param, function_id_description);
	doca_argp_param_set_callback(function_id_param, hca_cap_function_id_callback);
	doca_argp_param_set_type(function_id_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_cmd_register_param(cmd, function_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&vhca_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(vhca_id_param, "v");
	doca_argp_param_set_long_name(vhca_id_param, "vhca-id");
	doca_argp_param_set_arguments(vhca_id_param, "<id>");
	doca_argp_param_set_description(vhca_id_param, vhca_id_description);
	doca_argp_param_set_callback(vhca_id_param, hca_cap_vhca_id_callback);
	doca_argp_param_set_type(vhca_id_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_cmd_register_param(cmd, vhca_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register hca-cap list command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_list_params(struct doca_argp_cmd *cmd)
{
	return register_device_param(cmd);
}

/*
 * Register hca-cap describe command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_describe_params(struct doca_argp_cmd *cmd)
{
	doca_error_t result;

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_hca_cap_cap_type_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Register hca-cap get command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_get_params(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *cap_mode_param, *output_fields_param;
	doca_error_t result;

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_hca_cap_cap_type_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&cap_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cap_mode_param, "m");
	doca_argp_param_set_long_name(cap_mode_param, "cap-mode");
	doca_argp_param_set_arguments(cap_mode_param, "<mode>");
	doca_argp_param_set_description(cap_mode_param, "Capability mode. Valid values are 'current' or 'max'.");
	doca_argp_param_set_callback(cap_mode_param, hca_cap_cap_mode_callback);
	doca_argp_param_set_type(cap_mode_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(cap_mode_param);
	result = doca_argp_cmd_register_param(cmd, cap_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_hca_cap_target_params(cmd, true);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&output_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(output_fields_param, "o");
	doca_argp_param_set_long_name(output_fields_param, "output-fields");
	doca_argp_param_set_arguments(output_fields_param, "<field_name1,field_name2,...>");
	doca_argp_param_set_description(
		output_fields_param,
		"Optional output fields in format 'field_name1,field_name2,...'. If not specified, all fields will be returned.");
	doca_argp_param_set_callback(output_fields_param, hca_cap_output_fields_callback);
	doca_argp_param_set_type(output_fields_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, output_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register hca-cap set command parameters
 *
 * @cmd [in]: ARGP command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_set_params(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *input_fields_param;
	doca_error_t result;

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_hca_cap_cap_type_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_hca_cap_target_params(cmd, false);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&input_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(input_fields_param, "i");
	doca_argp_param_set_long_name(input_fields_param, "input-fields");
	doca_argp_param_set_arguments(input_fields_param, "<field_name1=value1,field_name2=value2,...>");
	doca_argp_param_set_description(input_fields_param,
					"Input fields in format 'field_name1=value1,field_name2=value2,...'");
	doca_argp_param_set_callback(input_fields_param, hca_cap_input_fields_callback);
	doca_argp_param_set_type(input_fields_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(input_fields_param);
	result = doca_argp_cmd_register_param(cmd, input_fields_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Hca-cap list command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_list_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_HCA_CAP_LIST;
	return DOCA_SUCCESS;
}

/*
 * Hca-cap describe command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_describe_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_HCA_CAP_DESCRIBE;
	return DOCA_SUCCESS;
}

/*
 * Hca-cap get command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_get_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_HCA_CAP_GET;
	return DOCA_SUCCESS;
}

/*
 * Hca-cap set command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t hca_cap_set_callback(void *config)
{
	struct mgmt_raw_config *conf = (struct mgmt_raw_config *)config;

	conf->cmd = DOCA_MGMT_RAW_CMD_HCA_CAP_SET;
	return DOCA_SUCCESS;
}

/*
 * Register hca-cap command and its sub-commands
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_hca_cap_cmd(void)
{
	struct doca_argp_cmd *cmd_hca_cap = NULL;
	struct doca_argp_cmd *cmd_list = NULL;
	struct doca_argp_cmd *cmd_describe = NULL;
	struct doca_argp_cmd *cmd_get = NULL;
	struct doca_argp_cmd *cmd_set = NULL;
	doca_error_t result;

	result = doca_argp_cmd_create(&cmd_hca_cap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_hca_cap, "hca-cap");
	doca_argp_cmd_set_description(cmd_hca_cap, "List, describe, get and set HCA capabilities of a device");

	result = doca_argp_cmd_create(&cmd_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_list, "list");
	doca_argp_cmd_set_description(cmd_list, "List available HCA capability types of a device");
	doca_argp_cmd_set_callback(cmd_list, hca_cap_list_callback);
	result = register_hca_cap_list_params(cmd_list);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_hca_cap, cmd_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register hca-cap list command: %s", doca_error_get_descr(result));
		cmd_list = NULL;
		goto cleanup;
	}
	cmd_list = NULL;

	result = doca_argp_cmd_create(&cmd_describe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_describe, "describe");
	doca_argp_cmd_set_description(cmd_describe, "List available fields of a HCA capability type");
	doca_argp_cmd_set_callback(cmd_describe, hca_cap_describe_callback);
	result = register_hca_cap_describe_params(cmd_describe);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_hca_cap, cmd_describe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register hca-cap describe command: %s", doca_error_get_descr(result));
		cmd_describe = NULL;
		goto cleanup;
	}
	cmd_describe = NULL;

	result = doca_argp_cmd_create(&cmd_get);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_get, "get");
	doca_argp_cmd_set_description(cmd_get, "Query HCA capabilities of a device");
	doca_argp_cmd_set_callback(cmd_get, hca_cap_get_callback);
	result = register_hca_cap_get_params(cmd_get);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_hca_cap, cmd_get);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register hca-cap get command: %s", doca_error_get_descr(result));
		cmd_get = NULL;
		goto cleanup;
	}
	cmd_get = NULL;

	result = doca_argp_cmd_create(&cmd_set);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	doca_argp_cmd_set_name(cmd_set, "set");
	doca_argp_cmd_set_description(cmd_set, "Modify HCA capabilities of a device");
	doca_argp_cmd_set_callback(cmd_set, hca_cap_set_callback);
	result = register_hca_cap_set_params(cmd_set);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = doca_argp_cmd_register_cmd(cmd_hca_cap, cmd_set);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register hca-cap set command: %s", doca_error_get_descr(result));
		cmd_set = NULL;
		goto cleanup;
	}
	cmd_set = NULL;

	result = doca_argp_register_cmd(cmd_hca_cap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register hca-cap command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;

cleanup:
	if (cmd_set != NULL)
		(void)doca_argp_cmd_destroy(cmd_set);
	if (cmd_get != NULL)
		(void)doca_argp_cmd_destroy(cmd_get);
	if (cmd_describe != NULL)
		(void)doca_argp_cmd_destroy(cmd_describe);
	if (cmd_list != NULL)
		(void)doca_argp_cmd_destroy(cmd_list);
	if (cmd_hca_cap != NULL)
		(void)doca_argp_cmd_destroy(cmd_hca_cap);

	return result;
}

/*
 * Register the sample commands
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_raw_cmds(void)
{
	doca_error_t result;

	result = mgmt_common_register_list_targets_cmd(list_targets_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_register_cmd();
	if (result != DOCA_SUCCESS)
		return result;

	result = register_hca_cap_cmd();
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Validate the sample parameters
 *
 * @conf [in]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t validate_params(struct mgmt_raw_config *conf)
{
	if (conf->cmd == DOCA_MGMT_RAW_CMD_NONE) {
		fprintf(stderr,
			"A command must be specified (e.g., list-targets, register list, register get, hca-cap get, etc.)\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_GET) {
		bool has_vhca_id = conf->hca_cap_config.get_params.has_vhca_id;
		bool has_function_id = conf->hca_cap_config.get_params.has_function_id;
		bool both_targets_specified = has_vhca_id && has_function_id;

		if (both_targets_specified) {
			fprintf(stderr,
				"-f/--function-id and -v/--vhca-id parameters are mutually exclusive; specify at most one\n");
			doca_argp_usage();
			return DOCA_ERROR_INVALID_VALUE;
		}
	} else if (conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_SET) {
		bool has_vhca_id = conf->hca_cap_config.set_params.has_vhca_id;
		bool has_function_id = conf->hca_cap_config.set_params.has_function_id;
		bool neither_target_specified = !has_vhca_id && !has_function_id;
		bool both_targets_specified = has_vhca_id && has_function_id;

		if (neither_target_specified) {
			fprintf(stderr, "Either -f/--function-id or -v/--vhca-id parameter must be specified\n");
			doca_argp_usage();
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (both_targets_specified) {
			fprintf(stderr,
				"-f/--function-id and -v/--vhca-id parameters are mutually exclusive; specify exactly one\n");
			doca_argp_usage();
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Execute the selected command
 *
 * @conf [in]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t execute_command(struct mgmt_raw_config *conf)
{
	doca_error_t result;

	switch (conf->cmd) {
	case DOCA_MGMT_RAW_CMD_LIST_TARGETS:
		result = mgmt_common_list_targets(true);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to list management targets: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_REGISTER_LIST:
		result = mgmt_raw_register_list(conf->target);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to list raw registers: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_REGISTER_DESCRIBE:
		result = mgmt_raw_register_describe(conf->target, conf->register_config.describe_params.register_name);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to describe raw register: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_REGISTER_GET:
		if (conf->register_config.get_params.output_fields != NULL)
			result = mgmt_raw_register_get(conf->target,
						       conf->register_config.get_params.register_name,
						       conf->register_config.get_params.input_fields,
						       conf->register_config.get_params.output_fields);
		else
			result = mgmt_raw_register_get_all(conf->target,
							   conf->register_config.get_params.register_name,
							   conf->register_config.get_params.input_fields);

		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to query raw register: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_REGISTER_SET:
		result = mgmt_raw_register_set(conf->target,
					       conf->register_config.set_params.register_name,
					       conf->register_config.set_params.input_fields);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to modify raw register: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_HCA_CAP_LIST:
		result = mgmt_raw_hca_cap_list(conf->target);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to list HCA capabilities: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_HCA_CAP_DESCRIBE:
		result = mgmt_raw_hca_cap_describe(conf->target, conf->hca_cap_config.describe_params.cap_type);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to describe HCA capabilities: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_HCA_CAP_GET:
		if (conf->hca_cap_config.get_params.output_fields != NULL)
			result = mgmt_raw_hca_cap_get(conf->target,
						      conf->hca_cap_config.get_params.cap_type,
						      conf->hca_cap_config.get_params.cap_mode,
						      conf->hca_cap_config.get_params.function_id,
						      conf->hca_cap_config.get_params.has_function_id,
						      conf->hca_cap_config.get_params.vhca_id,
						      conf->hca_cap_config.get_params.has_vhca_id,
						      conf->hca_cap_config.get_params.output_fields);
		else
			result = mgmt_raw_hca_cap_get_all(conf->target,
							  conf->hca_cap_config.get_params.cap_type,
							  conf->hca_cap_config.get_params.cap_mode,
							  conf->hca_cap_config.get_params.function_id,
							  conf->hca_cap_config.get_params.has_function_id,
							  conf->hca_cap_config.get_params.vhca_id,
							  conf->hca_cap_config.get_params.has_vhca_id);

		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to query HCA capabilities: %s\n", doca_error_get_descr(result));

		break;
	case DOCA_MGMT_RAW_CMD_HCA_CAP_SET:
		result = mgmt_raw_hca_cap_set(conf->target,
					      conf->hca_cap_config.set_params.cap_type,
					      conf->hca_cap_config.set_params.function_id,
					      conf->hca_cap_config.set_params.has_function_id,
					      conf->hca_cap_config.set_params.vhca_id,
					      conf->hca_cap_config.set_params.has_vhca_id,
					      conf->hca_cap_config.set_params.input_fields);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "Failed to modify HCA capabilities: %s\n", doca_error_get_descr(result));

		break;
	default:
		result = DOCA_ERROR_INVALID_VALUE;
		fprintf(stderr, "Invalid command: %d\n", conf->cmd);

		break;
	}

	return result;
}

/*
 * Free configuration resources
 *
 * @conf [in]: Program configuration context
 */
static void free_config(struct mgmt_raw_config *conf)
{
	bool is_register_cmd =
		conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_LIST || conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_DESCRIBE ||
		conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_GET || conf->cmd == DOCA_MGMT_RAW_CMD_REGISTER_SET;
	bool is_hca_cap_cmd = conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_LIST ||
			      conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_DESCRIBE ||
			      conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_GET || conf->cmd == DOCA_MGMT_RAW_CMD_HCA_CAP_SET;

	if (is_register_cmd) {
		if (conf->register_config.describe_params.register_name != NULL)
			free(conf->register_config.describe_params.register_name);
		if (conf->register_config.get_params.register_name != NULL)
			free(conf->register_config.get_params.register_name);
		if (conf->register_config.get_params.input_fields != NULL)
			free(conf->register_config.get_params.input_fields);
		if (conf->register_config.get_params.output_fields != NULL)
			free(conf->register_config.get_params.output_fields);
		if (conf->register_config.set_params.register_name != NULL)
			free(conf->register_config.set_params.register_name);
		if (conf->register_config.set_params.input_fields != NULL)
			free(conf->register_config.set_params.input_fields);
	} else if (is_hca_cap_cmd) {
		if (conf->hca_cap_config.describe_params.cap_type != NULL)
			free(conf->hca_cap_config.describe_params.cap_type);
		if (conf->hca_cap_config.get_params.cap_type != NULL)
			free(conf->hca_cap_config.get_params.cap_type);
		if (conf->hca_cap_config.get_params.cap_mode != NULL)
			free(conf->hca_cap_config.get_params.cap_mode);
		if (conf->hca_cap_config.get_params.output_fields != NULL)
			free(conf->hca_cap_config.get_params.output_fields);
		if (conf->hca_cap_config.set_params.cap_type != NULL)
			free(conf->hca_cap_config.set_params.cap_type);
		if (conf->hca_cap_config.set_params.input_fields != NULL)
			free(conf->hca_cap_config.set_params.input_fields);
	}
}

/*
 * Sample main function
 *
 * @argc [in]: Command line arguments size
 * @argv [in]: Command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;
	struct mgmt_raw_config conf = {};
	int exit_status = EXIT_FAILURE;
	doca_error_t result;

	/* Disable all log messages unless explicitly enabled by user */
	result = doca_log_level_set_global_lower_limit(DOCA_LOG_LEVEL_DISABLE);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr,
			"Failed to set global lower limit for log messages: %s\n",
			doca_error_get_descr(result));
		goto sample_exit;
	}

	result = doca_log_level_set_global_sdk_limit(DOCA_LOG_LEVEL_ERROR);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set global limit for SDK log messages: %s\n", doca_error_get_descr(result));
		goto sample_exit;
	}

	/* Register logger backends */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create standard logger backend: %s\n", doca_error_get_descr(result));
		goto sample_exit;
	}

	/* Register a logger backend for internal SDK */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create SDK logger backend: %s\n", doca_error_get_descr(result));
		goto sample_exit;
	}

	/* Initialize configuration */
	memset(&conf, 0, sizeof(conf));

	result = doca_argp_init(NULL, &conf);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to init ARGP resources: %s\n", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_mgmt_raw_cmds();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register sample commands: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to parse sample input: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	DOCA_LOG_INFO("Starting the sample");

	result = validate_params(&conf);
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	result = execute_command(&conf);
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	free_config(&conf);
	doca_argp_destroy();

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
