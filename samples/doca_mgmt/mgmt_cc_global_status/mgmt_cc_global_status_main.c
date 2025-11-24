/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <doca_dev.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(MGMT_CC_GLOBAL_STATUS::MAIN);

#define CC_GLOBAL_STATUS_PROTOCOL_MAX_SIZE 3
#define CC_GLOBAL_STATUS_PROTOCOL_NP "np"
#define CC_GLOBAL_STATUS_PROTOCOL_RP "rp"

/* Configuration struct */
enum doca_mgmt_cc_global_status_cmds {
	DOCA_MGMT_CC_GLOBAL_STATUS_CMD_NONE,
	DOCA_MGMT_CC_GLOBAL_STATUS_CMD_GET,
	DOCA_MGMT_CC_GLOBAL_STATUS_CMD_SET,
};

struct mgmt_cc_global_status_config {
	/* Common configuration */
	enum doca_mgmt_cc_global_status_cmds cmd;	   /* Command to execute */
	char dev_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];	   /* Device PCI address */
	uint8_t priority;				   /* Priority of the congestion control */
	char protocol[CC_GLOBAL_STATUS_PROTOCOL_MAX_SIZE]; /* Protocol of the congestion control */

	/* Set command configuration */
	struct {
		bool enabled; /* Enable or disable the congestion control */
	} set_params;
};

/* Sample's Logic */
doca_error_t mgmt_cc_global_status_get(const char *dev_pci_addr, uint8_t priority, char *protocol);
doca_error_t mgmt_cc_global_status_set(const char *dev_pci_addr, uint8_t priority, char *protocol, bool enabled);

/*
 * ARGP Callback - Handle device parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dev_pci_addr_callback(void *param, void *config)
{
	struct mgmt_cc_global_status_config *conf = (struct mgmt_cc_global_status_config *)config;
	const char *dev_pci_addr = (char *)param;
	int dev_pci_addr_len = strnlen(dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);

	if (dev_pci_addr_len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeds the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(conf->dev_pci_addr, dev_pci_addr, dev_pci_addr_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle priority parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t priority_callback(void *param, void *config)
{
	struct mgmt_cc_global_status_config *conf = (struct mgmt_cc_global_status_config *)config;
	const int *priority = (int *)param;

	if (*priority < 0 || *priority > 7) {
		DOCA_LOG_ERR("Entered priority: %d, valid value must be between 0 and 7", *priority);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->priority = *priority;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle protocol parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t protocol_callback(void *param, void *config)
{
	struct mgmt_cc_global_status_config *conf = (struct mgmt_cc_global_status_config *)config;
	const char *protocol = (char *)param;
	int protocol_len = strnlen(protocol, CC_GLOBAL_STATUS_PROTOCOL_MAX_SIZE);

	if (protocol_len >= CC_GLOBAL_STATUS_PROTOCOL_MAX_SIZE) {
		DOCA_LOG_ERR("Entered protocol exceeds the maximum size of %d", CC_GLOBAL_STATUS_PROTOCOL_MAX_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcmp(protocol, CC_GLOBAL_STATUS_PROTOCOL_RP) != 0 &&
	    strcmp(protocol, CC_GLOBAL_STATUS_PROTOCOL_NP) != 0) {
		DOCA_LOG_ERR("Entered protocol: %s, valid values are %s or %s",
			     protocol,
			     CC_GLOBAL_STATUS_PROTOCOL_RP,
			     CC_GLOBAL_STATUS_PROTOCOL_NP);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(conf->protocol, protocol, protocol_len + 1);

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
	struct mgmt_cc_global_status_config *conf = (struct mgmt_cc_global_status_config *)config;
	const char *enabled = (char *)param;

	if (strcasecmp(enabled, "true") == 0) {
		conf->set_params.enabled = true;
	} else if (strcasecmp(enabled, "false") == 0) {
		conf->set_params.enabled = false;
	} else {
		DOCA_LOG_ERR("Entered enabled value %s is invalid, valid values are 'true' or 'false'", enabled);
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
static doca_error_t register_mgmt_cc_global_status_cmd_params(struct doca_argp_cmd *cmd, bool is_set)
{
	struct doca_argp_param *dev_pci_addr_param;
	struct doca_argp_param *priority_param;
	struct doca_argp_param *protocol_param;
	struct doca_argp_param *enabled_param;
	doca_error_t result;

	/* Create and register PCI address param */
	result = doca_argp_param_create(&dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dev_pci_addr_param, "d");
	doca_argp_param_set_long_name(dev_pci_addr_param, "dev-pci-addr");
	doca_argp_param_set_description(dev_pci_addr_param, "DOCA device PCI device address - default: 08:00.0");
	doca_argp_param_set_callback(dev_pci_addr_param, dev_pci_addr_callback);
	doca_argp_param_set_type(dev_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register priority param */
	result = doca_argp_param_create(&priority_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(priority_param, "p");
	doca_argp_param_set_long_name(priority_param, "priority");
	doca_argp_param_set_description(priority_param, "Priority - default: 0");
	doca_argp_param_set_callback(priority_param, priority_callback);
	doca_argp_param_set_type(priority_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_cmd_register_param(cmd, priority_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register protocol param */
	result = doca_argp_param_create(&protocol_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(protocol_param, "t");
	doca_argp_param_set_long_name(protocol_param, "protocol");
	doca_argp_param_set_description(protocol_param, "Protocol - default: rp");
	doca_argp_param_set_callback(protocol_param, protocol_callback);
	doca_argp_param_set_type(protocol_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, protocol_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	if (is_set) {
		/* Create and register enabled param */
		result = doca_argp_param_create(&enabled_param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
			return result;
		}
		doca_argp_param_set_short_name(enabled_param, "e");
		doca_argp_param_set_long_name(enabled_param, "enabled");
		doca_argp_param_set_description(
			enabled_param,
			"Enable or disable congestion control (valid values: true or false) - default: false");
		doca_argp_param_set_callback(enabled_param, enabled_callback);
		doca_argp_param_set_type(enabled_param, DOCA_ARGP_TYPE_STRING);
		result = doca_argp_cmd_register_param(cmd, enabled_param);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
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
static doca_error_t set_global_status_callback(void *config)
{
	struct mgmt_cc_global_status_config *conf = (struct mgmt_cc_global_status_config *)config;

	conf->cmd = DOCA_MGMT_CC_GLOBAL_STATUS_CMD_SET;

	return DOCA_SUCCESS;
}

/**
 * Register set command
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_cc_global_status_cmd_set(void)
{
	struct doca_argp_cmd *cmd_set;
	doca_error_t result;

	/* Create set command */
	result = doca_argp_cmd_create(&cmd_set);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_set, "set");
	doca_argp_cmd_set_description(cmd_set, "Set congestion control global status");
	doca_argp_cmd_set_callback(cmd_set, set_global_status_callback);

	/* Register set command params */
	result = register_mgmt_cc_global_status_cmd_params(cmd_set, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register set command params: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register set command */
	result = doca_argp_register_cmd(cmd_set);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP command: %s", doca_error_get_descr(result));
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
static doca_error_t get_global_status_callback(void *config)
{
	struct mgmt_cc_global_status_config *conf = (struct mgmt_cc_global_status_config *)config;

	conf->cmd = DOCA_MGMT_CC_GLOBAL_STATUS_CMD_GET;

	return DOCA_SUCCESS;
}

/**
 * Register get command
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_cc_global_status_cmd_get(void)
{
	struct doca_argp_cmd *cmd_get;
	doca_error_t result;

	/* Create get command */
	result = doca_argp_cmd_create(&cmd_get);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_get, "get");
	doca_argp_cmd_set_description(cmd_get, "Get congestion control global status");
	doca_argp_cmd_set_callback(cmd_get, get_global_status_callback);

	/* Register get command params */
	result = register_mgmt_cc_global_status_cmd_params(cmd_get, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register get command params: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register get command */
	result = doca_argp_register_cmd(cmd_get);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Register the sample commands
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_cc_global_status_cmds(void)
{
	doca_error_t result;

	result = register_mgmt_cc_global_status_cmd_set();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register set command: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_mgmt_cc_global_status_cmd_get();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register get command: %s", doca_error_get_descr(result));
		return result;
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
	struct mgmt_cc_global_status_config conf = {};
	int exit_status = EXIT_FAILURE;
	doca_error_t result;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	/* Set the default configuration values (Example values) */
	conf.cmd = DOCA_MGMT_CC_GLOBAL_STATUS_CMD_NONE;
	strcpy(conf.dev_pci_addr, "08:00.0");
	conf.priority = 0;
	strcpy(conf.protocol, CC_GLOBAL_STATUS_PROTOCOL_RP);
	conf.set_params.enabled = false;

	result = doca_argp_init(NULL, &conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_mgmt_cc_global_status_cmds();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample commands: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	if (conf.cmd == DOCA_MGMT_CC_GLOBAL_STATUS_CMD_SET) {
		result = mgmt_cc_global_status_set(conf.dev_pci_addr,
						   conf.priority,
						   conf.protocol,
						   conf.set_params.enabled);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to run mgmt_cc_global_status_set: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_CC_GLOBAL_STATUS_CMD_GET) {
		result = mgmt_cc_global_status_get(conf.dev_pci_addr, conf.priority, conf.protocol);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to run mgmt_cc_global_status_get: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else {
		DOCA_LOG_ERR("Either 'get' or 'set' command must be specified");
		doca_argp_usage();
		goto argp_cleanup;
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
