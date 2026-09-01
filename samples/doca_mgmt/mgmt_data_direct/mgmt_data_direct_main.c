/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_log.h>

#include "mgmt_common.h"

/* Configuration struct */
enum doca_mgmt_data_direct_cmds {
	DOCA_MGMT_DATA_DIRECT_CMD_NONE,
	DOCA_MGMT_DATA_DIRECT_CMD_LIST_TARGETS,
	DOCA_MGMT_DATA_DIRECT_CMD_GET,
	DOCA_MGMT_DATA_DIRECT_CMD_SET,
};

struct mgmt_data_direct_config {
	/* Common configuration */
	enum doca_mgmt_data_direct_cmds cmd;	      /* Command to execute */
	char rep_target[MGMT_COMMON_TARGET_STR_LEN];  /* Representor management target string */
	bool rep_target_set;			      /* Whether representor target is set */
	char vf_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* VF PCI address */
	bool vf_pci_addr_set;			      /* Whether VF PCI address is set */

	/* Set command configuration */
	struct {
		bool enabled; /* Enable or disable data direct */
	} set_params;
};

/* Sample's Logic */
doca_error_t mgmt_data_direct_get(const char *rep_target_string, const char *vf_pci_addr);
doca_error_t mgmt_data_direct_set(const char *rep_target_string, const char *vf_pci_addr, bool enabled);

/*
 * ARGP Callback - Handle rep target parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rep_target_callback(void *param, void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;
	doca_error_t result;

	result = mgmt_common_target_string_copy((const char *)param, conf->rep_target);
	if (result != DOCA_SUCCESS)
		return result;

	conf->rep_target_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle list-targets command
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t list_targets_callback(void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;

	conf->cmd = DOCA_MGMT_DATA_DIRECT_CMD_LIST_TARGETS;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle VF PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vf_pci_addr_callback(void *param, void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;
	const char *vf_pci_addr = (char *)param;
	int vf_pci_addr_len = strlen(vf_pci_addr);

	if (!(vf_pci_addr_len == (DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1) && isxdigit((unsigned char)vf_pci_addr[0]) &&
	      isxdigit((unsigned char)vf_pci_addr[1]) && isxdigit((unsigned char)vf_pci_addr[2]) &&
	      isxdigit((unsigned char)vf_pci_addr[3]) && vf_pci_addr[4] == ':' &&
	      isxdigit((unsigned char)vf_pci_addr[5]) && isxdigit((unsigned char)vf_pci_addr[6]) &&
	      vf_pci_addr[7] == ':' && isxdigit((unsigned char)vf_pci_addr[8]) &&
	      isxdigit((unsigned char)vf_pci_addr[9]) && vf_pci_addr[10] == '.' &&
	      isdigit((unsigned char)vf_pci_addr[11]))) {
		fprintf(stderr, "Entered VF PCI address is invalid\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	strcpy(conf->vf_pci_addr, vf_pci_addr);
	conf->vf_pci_addr_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle enabled parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t enabled_callback(void *param, void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;
	const char *enabled = (char *)param;

	if (strcasecmp(enabled, "true") == 0) {
		conf->set_params.enabled = true;
	} else if (strcasecmp(enabled, "false") == 0) {
		conf->set_params.enabled = false;
	} else {
		fprintf(stderr, "Entered enabled value %s is invalid, valid values are 'true' or 'false'\n", enabled);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/**
 * Register commands parameters
 *
 * @cmd [in]: Command to register parameters for
 * @is_set [in]: Flag that indicates whether the command is set or get
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_data_direct_params(struct doca_argp_cmd *cmd, bool is_set)
{
	struct doca_argp_param *vf_pci_addr_param;
	struct doca_argp_param *enabled_param;
	doca_error_t result;

	/* Register the representor management target param */
	result = mgmt_common_register_target_dev_rep_param(
		cmd,
		rep_target_callback,
		false,
		"DOCA device representor management target string (e.g., pci/0000:08:00.0,c0pf0vf0 for VF), mutually exclusive with 'vf-pci-addr'. Use 'list-targets' to discover available targets");
	if (result != DOCA_SUCCESS)
		return result;

	/* Create and register VF param */
	result = doca_argp_param_create(&vf_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(vf_pci_addr_param, "v");
	doca_argp_param_set_long_name(vf_pci_addr_param, "vf-pci-addr");
	doca_argp_param_set_description(vf_pci_addr_param,
					"VF PCI address (e.g., 0000:08:00.2), mutually exclusive with 'rep' parameter");
	doca_argp_param_set_callback(vf_pci_addr_param, vf_pci_addr_callback);
	doca_argp_param_set_type(vf_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, vf_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	if (is_set) {
		/* Create and register enabled param */
		result = doca_argp_param_create(&enabled_param);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to create ARGP param: %s\n", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(enabled_param, "e");
		doca_argp_param_set_long_name(enabled_param, "enabled");
		doca_argp_param_set_description(enabled_param,
						"Enable or disable data direct (valid values: true or false)");
		doca_argp_param_set_callback(enabled_param, enabled_callback);
		doca_argp_param_set_type(enabled_param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(enabled_param);
		result = doca_argp_cmd_register_param(cmd, enabled_param);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to register program param: %s\n", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/**
 * Set command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_callback(void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;

	conf->cmd = DOCA_MGMT_DATA_DIRECT_CMD_SET;

	return DOCA_SUCCESS;
}

/**
 * Register set command
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_data_direct_cmd_set(void)
{
	struct doca_argp_cmd *cmd_set;
	doca_error_t result;

	/* Create set command */
	result = doca_argp_cmd_create(&cmd_set);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create ARGP command: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_set, "set");
	doca_argp_cmd_set_description(cmd_set, "Set data direct capability");
	doca_argp_cmd_set_callback(cmd_set, set_callback);

	/* Register set command params */
	result = register_mgmt_data_direct_params(cmd_set, true);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register set command params: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Register set command */
	result = doca_argp_register_cmd(cmd_set);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register ARGP command: %s\n", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Get command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_callback(void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;

	conf->cmd = DOCA_MGMT_DATA_DIRECT_CMD_GET;

	return DOCA_SUCCESS;
}

/**
 * Register get command
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_data_direct_cmd_get(void)
{
	struct doca_argp_cmd *cmd_get;
	doca_error_t result;

	/* Create get command */
	result = doca_argp_cmd_create(&cmd_get);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create ARGP command: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_get, "get");
	doca_argp_cmd_set_description(cmd_get, "Get data direct capability");
	doca_argp_cmd_set_callback(cmd_get, get_callback);

	/* Register get command params */
	result = register_mgmt_data_direct_params(cmd_get, false);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register get command params: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Register get command */
	result = doca_argp_register_cmd(cmd_get);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register ARGP command: %s\n", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Register the sample commands
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_data_direct_cmds(void)
{
	doca_error_t result;

	result = mgmt_common_register_list_targets_cmd(list_targets_callback);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register list-targets command: %s\n", doca_error_get_descr(result));
		return result;
	}

	result = register_mgmt_data_direct_cmd_set();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register set command: %s\n", doca_error_get_descr(result));
		return result;
	}

	result = register_mgmt_data_direct_cmd_get();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register get command: %s\n", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Validate the sample parameters
 *
 * @param [in] conf: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t validate_params(struct mgmt_data_direct_config *conf)
{
	if (conf->cmd == DOCA_MGMT_DATA_DIRECT_CMD_NONE) {
		fprintf(stderr, "A command must be specified: 'list-targets', 'get' or 'set'\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* list-targets does not operate on a specific target, skip following checks */
	if (conf->cmd == DOCA_MGMT_DATA_DIRECT_CMD_LIST_TARGETS)
		return DOCA_SUCCESS;

	if (!conf->rep_target_set && !conf->vf_pci_addr_set) {
		fprintf(stderr, "Either 'rep' or 'vf-pci-addr' parameters must be specified\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	} else if (conf->rep_target_set && conf->vf_pci_addr_set) {
		fprintf(stderr,
			"'rep' and 'vf-pci-addr' parameters are mutually exclusive and must not be specified together\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;
	struct mgmt_data_direct_config conf = {};
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
	result = register_mgmt_data_direct_cmds();
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register sample commands: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to parse sample input: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Validate parameters */
	result = validate_params(&conf);
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	if (conf.cmd == DOCA_MGMT_DATA_DIRECT_CMD_LIST_TARGETS) {
		result = mgmt_common_list_targets(true);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to list management targets: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_DATA_DIRECT_CMD_SET) {
		result = mgmt_data_direct_set(conf.rep_target_set ? conf.rep_target : NULL,
					      conf.vf_pci_addr_set ? conf.vf_pci_addr : NULL,
					      conf.set_params.enabled);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to set data direct: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_DATA_DIRECT_CMD_GET) {
		result = mgmt_data_direct_get(conf.rep_target_set ? conf.rep_target : NULL,
					      conf.vf_pci_addr_set ? conf.vf_pci_addr : NULL);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to get data direct: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

sample_exit:
	return exit_status;
}
