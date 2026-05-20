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
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_mgmt_icm_quota.h>

DOCA_LOG_REGISTER(MGMT_ICM_QUOTA::MAIN);

#define ICM_QUOTA_LIMIT_UNLIMITED_STR "unlimited"
#define ICM_QUOTA_LIMIT_UNLIMITED UINT64_MAX
#define ICM_QUOTA_LIMIT_MAX ((1ULL << 44) - (1 << 13)) /* 16TB-8KB which is 2^32-2 in granularity of 4KB */

/* Configuration struct */
enum doca_mgmt_quota_icm_cmds {
	DOCA_MGMT_ICM_QUOTA_CMD_NONE,
	DOCA_MGMT_ICM_QUOTA_CMD_GET,
	DOCA_MGMT_ICM_QUOTA_CMD_SET,
	DOCA_MGMT_ICM_QUOTA_CMD_CAPS,
};

struct mgmt_icm_quota_config {
	/* Common configuration */
	enum doca_mgmt_quota_icm_cmds cmd; /* Command to execute */
	struct doca_dev *dev;		   /* Device */
	struct doca_dev_rep *dev_rep;	   /* Device representor */
	bool dev_set;			   /* Whether device is set */
	bool rep_set;			   /* Whether representor is set */

	/* Get command configuration */
	struct {
		bool limit;	  /* Get limit attribute */
		bool cur_alloc;	  /* Get current allocation attribute */
		bool max_reached; /* Get max reached attribute */
	} get_params;

	/* Set command configuration */
	struct {
		bool limit_set;		/* Whether limit is set */
		uint64_t limit;		/* Limit value */
		bool reset_max_reached; /* Whether to reset max reached */
	} set_params;
};

/* Sample's Logic */
doca_error_t mgmt_icm_quota_get(struct doca_dev *dev,
				struct doca_dev_rep *dev_rep,
				bool get_limit,
				bool get_cur_alloc,
				bool get_max_reached);
doca_error_t mgmt_icm_quota_set(struct doca_dev *dev,
				struct doca_dev_rep *dev_rep,
				bool limit_set,
				uint64_t limit,
				bool reset_max_reached);
doca_error_t mgmt_icm_quota_caps(struct doca_dev *dev);

/*
 * Parse size string with unit suffix (K, M, G, T)
 *
 * @param [in]: Size string (e.g., "4096", "4K", "1M", "unlimited")
 * @value [out]: Parsed value in bytes
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_limit_string(const char *str, uint64_t *value)
{
	char *endptr;
	unsigned long long num;
	uint64_t multiplier = 1;

	/* Check for unlimited */
	if (strcasecmp(str, ICM_QUOTA_LIMIT_UNLIMITED_STR) == 0) {
		*value = ICM_QUOTA_LIMIT_UNLIMITED;
		return DOCA_SUCCESS;
	}

	/* Parse the numeric part */
	errno = 0;
	num = strtoull(str, &endptr, 10);
	if (errno != 0) {
		fprintf(stderr, "Failed to parse limit string: %s. errno %d (%s)\n", str, errno, strerror(errno));
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (endptr == str) {
		fprintf(stderr, "Invalid limit string: %s\n", str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check for unit suffix */
	if (*endptr != '\0') {
		char unit = toupper(*endptr);

		switch (unit) {
		case 'K':
			multiplier = 1024ULL;
			break;
		case 'M':
			multiplier = 1024ULL * 1024ULL;
			break;
		case 'G':
			multiplier = 1024ULL * 1024ULL * 1024ULL;
			break;
		case 'T':
			multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
			break;
		default:
			fprintf(stderr, "Invalid size unit: %c (valid units: K, M, G, T)\n", unit);
			return DOCA_ERROR_INVALID_VALUE;
		}

		endptr++;
		if (*endptr != '\0') {
			fprintf(stderr, "Invalid characters after size unit: %s\n", endptr);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	/* Check for overflow */
	if (num > UINT64_MAX / multiplier) {
		fprintf(stderr, "Limit value would overflow\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	*value = num * multiplier;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle device parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	struct doca_argp_device_ctx *dev_ctx = (struct doca_argp_device_ctx *)param;

	if (conf->dev_set) {
		fprintf(stderr, "Only one device is allowed to be specified\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (conf->rep_set) {
		fprintf(stderr, "Device and representor cannot be specified together\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->dev = dev_ctx->dev;
	conf->dev_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle representor parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rep_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	struct doca_argp_device_rep_ctx *dev_rep_ctx = (struct doca_argp_device_rep_ctx *)param;

	if (conf->rep_set) {
		fprintf(stderr, "Only one representor is allowed to be specified\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (conf->dev_set) {
		fprintf(stderr, "Device and representor cannot be specified together\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->dev_rep = dev_rep_ctx->dev_rep;
	conf->dev = dev_rep_ctx->dev_ctx.dev;
	conf->rep_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle limit parameter (get command)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_limit_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	bool limit = *(bool *)param;

	conf->get_params.limit = limit;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cur-alloc parameter (get command)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_cur_alloc_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	bool cur_alloc = *(bool *)param;

	conf->get_params.cur_alloc = cur_alloc;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle max-reached parameter (get command)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_max_reached_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	bool max_reached = *(bool *)param;

	conf->get_params.max_reached = max_reached;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle limit parameter (set command)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_limit_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	const char *limit_str = (const char *)param;
	doca_error_t result;

	result = parse_limit_string(limit_str, &conf->set_params.limit);
	if (result != DOCA_SUCCESS)
		return result;

	if (conf->set_params.limit != ICM_QUOTA_LIMIT_UNLIMITED) {
		if (conf->set_params.limit > ICM_QUOTA_LIMIT_MAX) {
			fprintf(stderr, "Limit value exceeds the maximum limit of %llu bytes\n", ICM_QUOTA_LIMIT_MAX);
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (conf->set_params.limit % 4096 != 0) {
			fprintf(stderr, "Limit value must be aligned to 4K\n");
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	conf->set_params.limit_set = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle reset-max-reached parameter (set command)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_reset_max_reached_callback(void *param, void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;
	bool reset_max_reached = *(bool *)param;

	conf->set_params.reset_max_reached = reset_max_reached;

	return DOCA_SUCCESS;
}

/**
 * Register common device and representor parameters
 *
 * @cmd [in]: Command to register parameters for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_device_rep_params(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *device_param, *rep_param;
	doca_error_t result;

	/* Create and register device param */
	result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_description(
		device_param,
		"DOCA device (e.g., pci/0000:08:00.0), mutually exclusive with 'rep' parameter");
	doca_argp_param_set_callback(device_param, device_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_DEVICE);
	result = doca_argp_cmd_register_param(cmd, device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register representor param */
	result = doca_argp_param_create(&rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rep_param, "r");
	doca_argp_param_set_long_name(rep_param, "rep");
	doca_argp_param_set_description(
		rep_param,
		"Device representor (e.g., pci/0000:08:00.0,pf0vf0), mutually exclusive with 'device' parameter");
	doca_argp_param_set_callback(rep_param, rep_callback);
	doca_argp_param_set_type(rep_param, DOCA_ARGP_TYPE_DEVICE_REP);
	result = doca_argp_cmd_register_param(cmd, rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Register get command parameters
 *
 * @cmd [in]: Command to register parameters for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_icm_quota_get_params(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *limit_param, *cur_alloc_param, *max_reached_param;
	doca_error_t result;

	/* Register common device and representor params */
	result = register_device_rep_params(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	/* Create and register limit param */
	result = doca_argp_param_create(&limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(limit_param, "limit");
	doca_argp_param_set_description(limit_param, "Get ICM quota limit");
	doca_argp_param_set_callback(limit_param, get_limit_callback);
	doca_argp_param_set_type(limit_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_cmd_register_param(cmd, limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cur-alloc param */
	result = doca_argp_param_create(&cur_alloc_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(cur_alloc_param, "cur-alloc");
	doca_argp_param_set_description(cur_alloc_param, "Get currently allocated ICM quota");
	doca_argp_param_set_callback(cur_alloc_param, get_cur_alloc_callback);
	doca_argp_param_set_type(cur_alloc_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_cmd_register_param(cmd, cur_alloc_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register max-reached param */
	result = doca_argp_param_create(&max_reached_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(max_reached_param, "max-reached");
	doca_argp_param_set_description(max_reached_param, "Get maximum reached ICM quota");
	doca_argp_param_set_callback(max_reached_param, get_max_reached_callback);
	doca_argp_param_set_type(max_reached_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_cmd_register_param(cmd, max_reached_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Register set command parameters
 *
 * @cmd [in]: Command to register parameters for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_icm_quota_set_params(struct doca_argp_cmd *cmd)
{
	struct doca_argp_param *limit_param, *reset_max_reached_param;
	doca_error_t result;

	/* Register common device and representor params */
	result = register_device_rep_params(cmd);
	if (result != DOCA_SUCCESS)
		return result;

	/* Create and register limit param */
	result = doca_argp_param_create(&limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(limit_param, "L");
	doca_argp_param_set_long_name(limit_param, "limit");
	doca_argp_param_set_description(
		limit_param,
		"Set the ICM quota limit for the device (e.g., 4096, 4K, 1M, 1G, 1T, unlimited). The value must be aligned to 4K and less than or equal to both: a) 16TB-8KB; b) the maximum limit reported by `caps` command. Value of 'unlimited' indicates no limit.");
	doca_argp_param_set_callback(limit_param, set_limit_callback);
	doca_argp_param_set_type(limit_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_cmd_register_param(cmd, limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register reset-max-reached param */
	result = doca_argp_param_create(&reset_max_reached_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(reset_max_reached_param, "reset-max-reached");
	doca_argp_param_set_description(reset_max_reached_param, "Reset maximum reached ICM quota");
	doca_argp_param_set_callback(reset_max_reached_param, set_reset_max_reached_callback);
	doca_argp_param_set_type(reset_max_reached_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_cmd_register_param(cmd, reset_max_reached_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
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
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;

	conf->cmd = DOCA_MGMT_ICM_QUOTA_CMD_GET;

	return DOCA_SUCCESS;
}

/**
 * Register get command
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_icm_quota_cmd_get(void)
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
	doca_argp_cmd_set_description(cmd_get, "Get ICM quota configuration");
	doca_argp_cmd_set_callback(cmd_get, get_callback);

	/* Register get command params */
	result = register_mgmt_icm_quota_get_params(cmd_get);
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
 * Set command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_callback(void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;

	conf->cmd = DOCA_MGMT_ICM_QUOTA_CMD_SET;

	return DOCA_SUCCESS;
}

/**
 * Register set command
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_mgmt_icm_quota_cmd_set(void)
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
	doca_argp_cmd_set_description(cmd_set, "Set ICM quota configuration");
	doca_argp_cmd_set_callback(cmd_set, set_callback);

	/* Register set command params */
	result = register_mgmt_icm_quota_set_params(cmd_set);
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
 * Caps command callback
 *
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t caps_callback(void *config)
{
	struct mgmt_icm_quota_config *conf = (struct mgmt_icm_quota_config *)config;

	conf->cmd = DOCA_MGMT_ICM_QUOTA_CMD_CAPS;

	return DOCA_SUCCESS;
}

static doca_error_t register_mgmt_icm_quota_cmd_caps(void)
{
	struct doca_argp_cmd *cmd_caps;
	doca_error_t result;

	/* Create caps command */
	result = doca_argp_cmd_create(&cmd_caps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP command: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd_caps, "caps");
	doca_argp_cmd_set_description(cmd_caps, "Get ICM quota capabilities");
	doca_argp_cmd_set_callback(cmd_caps, caps_callback);

	/* Register common device and representor params */
	result = register_device_rep_params(cmd_caps);
	if (result != DOCA_SUCCESS)
		return result;

	/* Register caps command */
	result = doca_argp_register_cmd(cmd_caps);
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
static doca_error_t register_mgmt_icm_quota_cmds(void)
{
	doca_error_t result;

	result = register_mgmt_icm_quota_cmd_get();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register get command: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_mgmt_icm_quota_cmd_set();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register set command: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_mgmt_icm_quota_cmd_caps();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register caps command: %s", doca_error_get_descr(result));
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
static doca_error_t validate_params(struct mgmt_icm_quota_config *conf)
{
	if (conf->cmd == DOCA_MGMT_ICM_QUOTA_CMD_NONE) {
		fprintf(stderr, "Either 'get', 'set' or 'caps' command must be specified\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!conf->dev_set && !conf->rep_set) {
		fprintf(stderr, "Either 'device' or 'rep' parameter must be specified\n");
		doca_argp_usage();
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (conf->cmd == DOCA_MGMT_ICM_QUOTA_CMD_SET) {
		if (!conf->set_params.limit_set && !conf->set_params.reset_max_reached) {
			fprintf(stderr,
				"At least one of 'limit' or 'reset-max-reached' parameters must be specified for 'set' command\n");
			doca_argp_usage();
			return DOCA_ERROR_INVALID_VALUE;
		}
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
	struct mgmt_icm_quota_config conf = {};
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
	conf.cmd = DOCA_MGMT_ICM_QUOTA_CMD_NONE;
	conf.dev = NULL;
	conf.dev_rep = NULL;
	conf.dev_set = false;
	conf.rep_set = false;
	conf.get_params.limit = false;
	conf.get_params.cur_alloc = false;
	conf.get_params.max_reached = false;
	conf.set_params.limit_set = false;
	conf.set_params.limit = 0;
	conf.set_params.reset_max_reached = false;

	result = doca_argp_init(NULL, &conf);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to init ARGP resources: %s\n", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_mgmt_icm_quota_cmds();
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

	/* Validate parameters */
	result = validate_params(&conf);
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	if (conf.cmd == DOCA_MGMT_ICM_QUOTA_CMD_GET) {
		/* If no specific get params are set, get all */
		bool get_all = !conf.get_params.limit && !conf.get_params.cur_alloc && !conf.get_params.max_reached;

		result = mgmt_icm_quota_get(conf.dev,
					    conf.dev_rep,
					    get_all || conf.get_params.limit,
					    get_all || conf.get_params.cur_alloc,
					    get_all || conf.get_params.max_reached);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to get ICM quota configuration: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_ICM_QUOTA_CMD_SET) {
		result = mgmt_icm_quota_set(conf.dev,
					    conf.dev_rep,
					    conf.set_params.limit_set,
					    conf.set_params.limit,
					    conf.set_params.reset_max_reached);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to set ICM quota configuration: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	} else if (conf.cmd == DOCA_MGMT_ICM_QUOTA_CMD_CAPS) {
		result = mgmt_icm_quota_caps(conf.dev);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to get ICM quota capabilities: %s\n", doca_error_get_descr(result));
			goto argp_cleanup;
		}
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	if (conf.dev_rep != NULL)
		if (doca_dev_rep_close(conf.dev_rep) != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to close DOCA device representor");
	/* Only close device if it was opened directly (not through rep) */
	if (conf.dev != NULL && conf.dev_set)
		if (doca_dev_close(conf.dev) != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to close DOCA device");

	doca_argp_destroy();

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
