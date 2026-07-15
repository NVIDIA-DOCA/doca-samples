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

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_log.h>

#include "common.h"

DOCA_LOG_REGISTER(MGMT_DATA_DIRECT::MAIN);

/* Configuration struct */
enum doca_mgmt_data_direct_cmds {
	DOCA_MGMT_DATA_DIRECT_CMD_NONE,
	DOCA_MGMT_DATA_DIRECT_CMD_GET,
	DOCA_MGMT_DATA_DIRECT_CMD_SET,
};

struct mgmt_data_direct_config {
	/* Common configuration */
	enum doca_mgmt_data_direct_cmds cmd;	      /* Command to execute */
	struct doca_dev *dev;			      /* Device */
	struct doca_dev_rep *dev_rep;		      /* Device representor */
	bool rep_set;				      /* Whether representor is set */
	char vf_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* VF PCI address */
	bool vf_pci_addr_set;			      /* Whether VF PCI address is set */

	/* Set command configuration */
	struct {
		bool enabled; /* Enable or disable data direct */
	} set_params;
};

/* Sample's Logic */
doca_error_t mgmt_data_direct_get(struct doca_dev *dev, struct doca_dev_rep *dev_rep);
doca_error_t mgmt_data_direct_set(struct doca_dev *dev, struct doca_dev_rep *dev_rep, bool enabled);

/*
 * ARGP Callback - Handle rep parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rep_callback(void *param, void *config)
{
	struct mgmt_data_direct_config *conf = (struct mgmt_data_direct_config *)config;
	struct doca_argp_device_rep_ctx *dev_rep_ctx = (struct doca_argp_device_rep_ctx *)param;

	if (conf->rep_set) {
		DOCA_LOG_ERR("Only one representor is allowed to be specified");
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->dev_rep = dev_rep_ctx->dev_rep;
	conf->dev = dev_rep_ctx->dev_ctx.dev;
	conf->rep_set = true;

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

	if (!(vf_pci_addr_len == (DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1) && isxdigit(vf_pci_addr[0]) &&
	      isxdigit(vf_pci_addr[1]) && isxdigit(vf_pci_addr[2]) && isxdigit(vf_pci_addr[3]) &&
	      vf_pci_addr[4] == ':' && isxdigit(vf_pci_addr[5]) && isxdigit(vf_pci_addr[6]) && vf_pci_addr[7] == ':' &&
	      isxdigit(vf_pci_addr[8]) && isxdigit(vf_pci_addr[9]) && vf_pci_addr[10] == '.' &&
	      isdigit(vf_pci_addr[11]))) {
		DOCA_LOG_ERR("Entered VF PCI address is invalid");
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
static doca_error_t register_mgmt_data_direct_params(struct doca_argp_cmd *cmd, bool is_set)
{
	struct doca_argp_param *rep_param;
	struct doca_argp_param *vf_pci_addr_param;
	struct doca_argp_param *enabled_param;
	doca_error_t result;

	/* Create and register device param */
	result = doca_argp_param_create(&rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rep_param, "r");
	doca_argp_param_set_long_name(rep_param, "rep");
	doca_argp_param_set_description(
		rep_param,
		"Device representor (e.g., pci/0000:08:00.0,pf0vf0 for VF), mutually exclusive with 'vf-pci-addr' parameter");
	doca_argp_param_set_callback(rep_param, rep_callback);
	doca_argp_param_set_type(rep_param, DOCA_ARGP_TYPE_DEVICE_REP);
	result = doca_argp_cmd_register_param(cmd, rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register VF param */
	result = doca_argp_param_create(&vf_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
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
		doca_argp_param_set_description(enabled_param,
						"Enable or disable data direct (valid values: true or false)");
		doca_argp_param_set_callback(enabled_param, enabled_callback);
		doca_argp_param_set_type(enabled_param, DOCA_ARGP_TYPE_STRING);
		doca_argp_param_set_mandatory(enabled_param);
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
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_set, "set");
	doca_argp_cmd_set_description(cmd_set, "Set data direct capability");
	doca_argp_cmd_set_callback(cmd_set, set_callback);

	/* Register set command params */
	result = register_mgmt_data_direct_params(cmd_set, true);
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
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_get, "get");
	doca_argp_cmd_set_description(cmd_get, "Get data direct capability");
	doca_argp_cmd_set_callback(cmd_get, get_callback);

	/* Register get command params */
	result = register_mgmt_data_direct_params(cmd_get, false);
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
static doca_error_t register_mgmt_data_direct_cmds(void)
{
	doca_error_t result;

	result = register_mgmt_data_direct_cmd_set();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register set command: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_mgmt_data_direct_cmd_get();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register get command: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Get the PF PCI address of the given VF over sysfs
 *
 * @param [in] vf_pci_addr: The PCI address of the VF
 * @param [out] pf_pci_addr: Buffer to store the PF PCI address. Must be at least DOCA_DEVINFO_PCI_ADDR_SIZE bytes long.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_pf_pci_addr(const char *vf_pci_addr, char *pf_pci_addr)
{
	char physfn_path[PATH_MAX];
	char physfn_link[PATH_MAX];
	char *pf_pci_addr_start;
	ssize_t rc;
	int ret;
	doca_error_t result;

	ret = snprintf(physfn_path, PATH_MAX, "/sys/bus/pci/devices/%s/physfn", vf_pci_addr);
	if (ret < 0 || ret >= PATH_MAX) {
		DOCA_LOG_ERR("Failed to get PF PCI address: failed to create physfn path");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rc = readlink(physfn_path, physfn_link, PATH_MAX);
	if (rc < 0 || rc == PATH_MAX) {
		if (rc == PATH_MAX) {
			ret = EINVAL;
			result = DOCA_ERROR_INVALID_VALUE;
		} else {
			ret = errno;
			result = DOCA_ERROR_OPERATING_SYSTEM;
		}
		DOCA_LOG_ERR("Failed to get PF PCI address: failed to read physfn link, errno: %s (%d)",
			     strerror(ret),
			     ret);
		return result;
	}
	physfn_link[rc] = '\0';

	pf_pci_addr_start = strrchr(physfn_link, '/');
	if (pf_pci_addr_start == NULL) {
		DOCA_LOG_ERR("Failed to get PF PCI address: failed to parse physfn link %s", physfn_link);
		return DOCA_ERROR_INVALID_VALUE;
	}
	pf_pci_addr_start++;
	strncpy(pf_pci_addr, pf_pci_addr_start, DOCA_DEVINFO_PCI_ADDR_SIZE);

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
		DOCA_LOG_ERR("Either 'get' or 'set' command must be specified");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!conf->rep_set && !conf->vf_pci_addr_set) {
		DOCA_LOG_ERR("Either 'rep' or 'vf-pci-addr' parameters must be specified");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	} else if (conf->rep_set && conf->vf_pci_addr_set) {
		DOCA_LOG_ERR(
			"'rep' and 'vf-pci-addr' parameters are mutually exclusive and must not be specified together");
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
	char pf_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];
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

	conf.cmd = DOCA_MGMT_DATA_DIRECT_CMD_NONE;
	conf.dev_rep = NULL;
	conf.dev = NULL;
	conf.rep_set = false;
	conf.vf_pci_addr[0] = '\0';
	conf.vf_pci_addr_set = false;
	conf.set_params.enabled = false;

	result = doca_argp_init(NULL, &conf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_mgmt_data_direct_cmds();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample commands: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Validate parameters */
	result = validate_params(&conf);
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	/* If VF PCI address parameter is set, open DOCA device and representor of VF */
	if (conf.vf_pci_addr_set) {
		result = get_pf_pci_addr(conf.vf_pci_addr, pf_pci_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get PF PCI address: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}

		result = open_doca_device_with_pci(pf_pci_addr, NULL, &conf.dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device of PF %s: %s",
				     pf_pci_addr,
				     doca_error_get_descr(result));
			goto argp_cleanup;
		}

		result = open_doca_device_rep_with_pci(conf.dev,
						       DOCA_DEVINFO_REP_FILTER_NET,
						       conf.vf_pci_addr,
						       &conf.dev_rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device representor of VF %s: %s",
				     conf.vf_pci_addr,
				     doca_error_get_descr(result));
			goto argp_cleanup;
		}
	}

	if (conf.cmd == DOCA_MGMT_DATA_DIRECT_CMD_SET) {
		result = mgmt_data_direct_set(conf.dev, conf.dev_rep, conf.set_params.enabled);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set data direct: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else {
		result = mgmt_data_direct_get(conf.dev, conf.dev_rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get data direct: %s", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	if (conf.dev_rep != NULL)
		if (doca_dev_rep_close(conf.dev_rep) != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to close DOCA device representor");
	/* Need to close doca_dev only in vf_pci_addr case, where its explicitly opened */
	if (conf.dev != NULL && conf.vf_pci_addr_set && !conf.rep_set)
		if (doca_dev_close(conf.dev) != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to close DOCA device");

	doca_argp_destroy();

sample_exit:
	return exit_status;
}