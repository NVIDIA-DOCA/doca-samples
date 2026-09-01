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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_log.h>

#include "mgmt_common.h"

DOCA_LOG_REGISTER(MGMT_DIAGNOSTICS_DATA::MAIN);

/* Configuration struct */
enum doca_mgmt_diagnostics_data_cmds {
	DOCA_MGMT_DIAGNOSTICS_DATA_CMD_NONE,
	DOCA_MGMT_DIAGNOSTICS_DATA_CMD_LIST_TARGETS,
	DOCA_MGMT_DIAGNOSTICS_DATA_CMD_CAPS,
	DOCA_MGMT_DIAGNOSTICS_DATA_CMD_GET,
	DOCA_MGMT_DIAGNOSTICS_DATA_CMD_SET,
};

struct mgmt_diagnostics_data_config {
	enum doca_mgmt_diagnostics_data_cmds cmd;
	char target[MGMT_COMMON_TARGET_STR_LEN];
	bool set_multi_domain_enable;
};

/* Sample's Logic */
doca_error_t mgmt_diagnostics_data_supported(const char *target_string);
doca_error_t mgmt_diagnostics_data_get(const char *target_string);
doca_error_t mgmt_diagnostics_data_set(const char *target_string, bool multi_domain);

/*
 * ARGP Callback - Handle target parameter
 */
static doca_error_t target_callback(void *param, void *config)
{
	struct mgmt_diagnostics_data_config *conf = (struct mgmt_diagnostics_data_config *)config;

	return mgmt_common_target_string_copy((const char *)param, conf->target);
}

/*
 * ARGP Callback - Handle list-targets command
 */
static doca_error_t list_targets_callback(void *config)
{
	struct mgmt_diagnostics_data_config *conf = (struct mgmt_diagnostics_data_config *)config;

	conf->cmd = DOCA_MGMT_DIAGNOSTICS_DATA_CMD_LIST_TARGETS;

	return DOCA_SUCCESS;
}

/*
 * Register device management target parameter for a command
 */
static doca_error_t register_device_param(struct doca_argp_cmd *cmd)
{
	return mgmt_common_register_target_dev_param(cmd, target_callback, true, NULL);
}

/*
 * ARGP Callback — caps command
 */
static doca_error_t caps_callback(void *config)
{
	struct mgmt_diagnostics_data_config *conf = (struct mgmt_diagnostics_data_config *)config;

	conf->cmd = DOCA_MGMT_DIAGNOSTICS_DATA_CMD_CAPS;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback — get command
 */
static doca_error_t get_callback(void *config)
{
	struct mgmt_diagnostics_data_config *conf = (struct mgmt_diagnostics_data_config *)config;

	conf->cmd = DOCA_MGMT_DIAGNOSTICS_DATA_CMD_GET;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback — set command
 */
static doca_error_t set_callback(void *config)
{
	struct mgmt_diagnostics_data_config *conf = (struct mgmt_diagnostics_data_config *)config;

	conf->cmd = DOCA_MGMT_DIAGNOSTICS_DATA_CMD_SET;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback — multi_domain value for set: true|false
 */
static doca_error_t set_multi_domain_callback(void *param, void *config)
{
	struct mgmt_diagnostics_data_config *conf = (struct mgmt_diagnostics_data_config *)config;
	const char *value = (const char *)param;

	if (strcasecmp(value, "false") == 0) {
		conf->set_multi_domain_enable = false;
	} else if (strcasecmp(value, "true") == 0) {
		conf->set_multi_domain_enable = true;
	} else {
		fprintf(stderr, "Invalid --multi-domain value '%s' (use true|false)\n", value);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the "supported" command
 */
static doca_error_t register_supported_cmd(void)
{
	struct doca_argp_cmd *cmd;
	doca_error_t result;

	result = doca_argp_cmd_create(&cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd, "caps");
	doca_argp_cmd_set_description(cmd, "Check if the device supports diagnostics data capabilities");
	doca_argp_cmd_set_callback(cmd, caps_callback);

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_register_cmd(cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the "get" command
 */
static doca_error_t register_get_cmd(void)
{
	struct doca_argp_cmd *cmd;
	doca_error_t result;

	result = doca_argp_cmd_create(&cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd, "get");
	doca_argp_cmd_set_description(cmd, "Get diagnostics data configuration.");
	doca_argp_cmd_set_callback(cmd, get_callback);

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_register_cmd(cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the "set" command
 */
static doca_error_t register_set_cmd(void)
{
	struct doca_argp_cmd *cmd;
	struct doca_argp_param *md_param;
	doca_error_t result;

	result = doca_argp_cmd_create(&cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd, "set");
	doca_argp_cmd_set_description(cmd, "Set diagnostics data configuration.");
	doca_argp_cmd_set_callback(cmd, set_callback);

	result = register_device_param(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&md_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(md_param, "multi-domain");
	doca_argp_param_set_description(md_param, "Enable or disable multi-domain (valid values: true or false)");
	doca_argp_param_set_callback(md_param, set_multi_domain_callback);
	doca_argp_param_set_type(md_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(md_param);
	result = doca_argp_cmd_register_param(cmd, md_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register set param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_register_cmd(cmd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Validate the sample parameters
 */
static doca_error_t validate_params(struct mgmt_diagnostics_data_config *conf)
{
	if (conf->cmd == DOCA_MGMT_DIAGNOSTICS_DATA_CMD_NONE) {
		fprintf(stderr, "A command must be specified: 'list-targets', 'caps', 'get' or 'set'\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;
	struct mgmt_diagnostics_data_config conf = {};
	int exit_status = EXIT_FAILURE;
	doca_error_t result;

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

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create standard logger backend: %s\n", doca_error_get_descr(result));
		goto sample_exit;
	}

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

	result = register_supported_cmd();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register sample commands: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = mgmt_common_register_list_targets_cmd(list_targets_callback);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register list-targets command: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = register_get_cmd();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register sample commands: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = register_set_cmd();
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

	if (conf.cmd == DOCA_MGMT_DIAGNOSTICS_DATA_CMD_LIST_TARGETS) {
		result = mgmt_common_list_targets(false);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to list management targets: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_DIAGNOSTICS_DATA_CMD_CAPS) {
		result = mgmt_diagnostics_data_supported(conf.target);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Diagnostics data supported check failed: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_DIAGNOSTICS_DATA_CMD_GET) {
		result = mgmt_diagnostics_data_get(conf.target);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Diagnostics data get failed: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_DIAGNOSTICS_DATA_CMD_SET) {
		result = mgmt_diagnostics_data_set(conf.target, conf.set_multi_domain_enable);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Diagnostics data set failed: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
