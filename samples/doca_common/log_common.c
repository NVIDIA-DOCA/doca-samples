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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>

#include "log_common.h"

DOCA_LOG_REGISTER(LOG_COMMON);

#define LOG_LEVEL_MULTIPLIER 10

/*
 * ARGP Callback - Handle server IP address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t server_ip_callback(void *param, void *config)
{
	struct log_sample_config *cfg = (struct log_sample_config *)config;
	size_t len = strnlen((const char *)param, sizeof(cfg->server_ip) - 1);
	const char *server_ip_str = (const char *)param;
	const char *p;
	char *endptr;
	int i;
	unsigned long octet;

	if (len >= sizeof(cfg->server_ip) - 1) {
		DOCA_LOG_ERR("Server IP parameter is too long: %zu, maximum allowed length is %zu",
			     len,
			     sizeof(cfg->server_ip) - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (server_ip_str == NULL || server_ip_str[0] == '\0') {
		DOCA_LOG_ERR("Server IP parameter is empty");
		return DOCA_ERROR_INVALID_VALUE;
	}

	p = server_ip_str;
	for (i = 0; i < 4; i++) {
		if (*p == '\0' || *p == '.') {
			DOCA_LOG_ERR("Server IP is not a valid IPv4 address: expected 4 dot-separated numbers");
			return DOCA_ERROR_INVALID_VALUE;
		}

		errno = 0;
		octet = strtoul(p, &endptr, 10);
		if (endptr == p || *endptr != (i < 3 ? '.' : '\0')) {
			DOCA_LOG_ERR("Server IP is not a valid IPv4 address: '%s'", server_ip_str);
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (errno == ERANGE || octet > 255) {
			DOCA_LOG_ERR("Server IP octet out of range 0-255: '%s'", server_ip_str);
			return DOCA_ERROR_INVALID_VALUE;
		}

		p = endptr + (i < 3 ? 1 : 0);
	}

	memcpy(cfg->server_ip, server_ip_str, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle server port parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t server_port_callback(void *param, void *config)
{
	struct log_sample_config *cfg = (struct log_sample_config *)config;
	int port = *(int *)param;

	if (port < DEFAULT_MINIMUM_LOG_SERVER_PORT || port > UINT16_MAX) {
		DOCA_LOG_ERR("Server port out of range %d <--> %u: '%d'",
			     DEFAULT_MINIMUM_LOG_SERVER_PORT,
			     UINT16_MAX,
			     port);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->server_port = (uint16_t)port;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle get_limits command parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_limits_callback(void *param, void *config)
{
	struct log_sample_config *cfg = (struct log_sample_config *)config;
	(void)param;
	if (cfg->cmd_set == true) {
		DOCA_LOG_WARN("Only the first command can be processed, all the following commands are ignored");
		return DOCA_SUCCESS;
	}
	cfg->cmd.operation = LOG_COMMAND_GET_LIMITS;
	cfg->cmd.operation_name = log_command_to_string(cfg->cmd.operation);
	cfg->cmd_set = true;
	return DOCA_SUCCESS;
}

/*
 * Helper function to set the lower and upper log level limit
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_limit_callback(void *param, void *config, bool is_lower_limit)
{
	uint32_t limit;
	struct log_sample_config *cfg = (struct log_sample_config *)config;
	if (cfg->cmd_set == true) {
		DOCA_LOG_WARN("Only the first command can be processed, all the following commands are ignored");
		return DOCA_SUCCESS;
	}
	cfg->cmd.operation = is_lower_limit ? LOG_COMMAND_SET_LOWER_LIMIT : LOG_COMMAND_SET_UPPER_LIMIT;
	cfg->cmd.operation_name = log_command_to_string(cfg->cmd.operation);
	cfg->cmd_set = true;
	limit = *(uint32_t *)param;
	if (!is_log_level_valid(limit)) {
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->cmd_arg = limit;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle set_lower_limit command parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_lower_limit_callback(void *param, void *config)
{
	return set_limit_callback(param, config, true);
}

/*
 * ARGP Callback - Handle set_upper_limit command parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_upper_limit_callback(void *param, void *config)
{
	return set_limit_callback(param, config, false);
}

doca_error_t register_log_sample_params(bool is_client)
{
	doca_error_t result;
	struct doca_argp_param *server_ip_param, *server_port_param, *get_limits_param, *set_lower_limit_param,
		*set_upper_limit_param;

	/* Create and register server port param */
	result = doca_argp_param_create(&server_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(server_port_param, "p");
	doca_argp_param_set_long_name(server_port_param, "port");
	doca_argp_param_set_description(
		server_port_param,
		"Server UDP port (default: 9999), should be a port number > 1024 to avoid \"sudo\" requirement");
	doca_argp_param_set_callback(server_port_param, server_port_callback);
	doca_argp_param_set_type(server_port_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(server_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	if (is_client == false) {
		/* Server side does not need command line parameters for server IP address and command */
		return DOCA_SUCCESS;
	}

	/* Create and register server IP address param */
	result = doca_argp_param_create(&server_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(server_ip_param, "i");
	doca_argp_param_set_long_name(server_ip_param, "ip");
	doca_argp_param_set_description(
		server_ip_param,
		"Server IPv4 address (default: 127.0.0.1), only IPv4 is supported. A valid IPv4 address is expected to be 4 dot-separated numbers.");
	doca_argp_param_set_callback(server_ip_param, server_ip_callback);
	doca_argp_param_set_type(server_ip_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(server_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	/* Create and register get limits param */
	result = doca_argp_param_create(&get_limits_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(get_limits_param, "g");
	doca_argp_param_set_long_name(get_limits_param, "get-limits");
	doca_argp_param_set_description(get_limits_param, "Get both lower and upper log limits from the server");
	doca_argp_param_set_callback(get_limits_param, get_limits_callback);
	doca_argp_param_set_type(get_limits_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(get_limits_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	/* Create and register set lower limit param */
	result = doca_argp_param_create(&set_lower_limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(set_lower_limit_param, "sl");
	doca_argp_param_set_long_name(set_lower_limit_param, "set-lower-limit");
	doca_argp_param_set_description(
		set_lower_limit_param,
		"Set the lower log limit on the server, valid values must be >=10 and <=70 and a multiple of 10");
	doca_argp_param_set_callback(set_lower_limit_param, set_lower_limit_callback);
	doca_argp_param_set_type(set_lower_limit_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(set_lower_limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	/* Create and register set upper limit param */
	result = doca_argp_param_create(&set_upper_limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(result));
		return result;
	}
	doca_argp_param_set_short_name(set_upper_limit_param, "su");
	doca_argp_param_set_long_name(set_upper_limit_param, "set-upper-limit");
	doca_argp_param_set_description(
		set_upper_limit_param,
		"Set the upper log limit on the server, valid values must be >=10 and <=70 and a multiple of 10");
	doca_argp_param_set_callback(set_upper_limit_param, set_upper_limit_callback);
	doca_argp_param_set_type(set_upper_limit_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(set_upper_limit_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t set_default_log_sample_config(struct log_sample_config *cfg)
{
	if (cfg == NULL) {
		DOCA_LOG_ERR("Failed to set default log sample config: cfg is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	memset(cfg, 0, sizeof(*cfg));
	strcpy(cfg->server_ip, DEFAULT_LOG_SERVER_IP_ADDRESS);
	cfg->server_port = DEFAULT_LOG_SERVER_PORT;
	cfg->cmd.operation = LOG_COMMAND_NONE;
	cfg->cmd.operation_name = "none";
	cfg->cmd_set = false;
	cfg->cmd_arg = 0;

	return DOCA_SUCCESS;
}

const char *log_command_to_string(enum log_command cmd)
{
	switch (cmd) {
	case LOG_COMMAND_GET_LIMITS:
		return "get-limits";
	case LOG_COMMAND_SET_LOWER_LIMIT:
		return "set-lower-limit";
	case LOG_COMMAND_SET_UPPER_LIMIT:
		return "set-upper-limit";
	case LOG_COMMAND_NONE:
		return "none";
	default:
		return "unknown";
	}
}

const char *log_level_to_string(enum doca_log_level level)
{
	switch (level) {
	case DOCA_LOG_LEVEL_DISABLE:
		return "DISABLE";
	case DOCA_LOG_LEVEL_CRIT:
		return "CRIT";
	case DOCA_LOG_LEVEL_ERROR:
		return "ERROR";
	case DOCA_LOG_LEVEL_WARNING:
		return "WARNING";
	case DOCA_LOG_LEVEL_INFO:
		return "INFO";
	case DOCA_LOG_LEVEL_DEBUG:
		return "DEBUG";
	case DOCA_LOG_LEVEL_TRACE:
		return "TRACE";
	default:
		return "UNKNOWN";
	}
}

doca_error_t print_log_sample_config(const struct log_sample_config *cfg)
{
	if (cfg == NULL) {
		DOCA_LOG_ERR("Failed to print log sample config: cfg is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("DOCA_LOG sample configuration:");
	if (cfg->is_client == true) {
		DOCA_LOG_INFO("---- Sample is a client");
		DOCA_LOG_INFO("---- Server IP address: %s", cfg->server_ip);
		DOCA_LOG_INFO("---- UDP server port: %d", cfg->server_port);
		DOCA_LOG_INFO("---- Command: %s", cfg->cmd.operation_name);
		if (cfg->cmd.operation == LOG_COMMAND_SET_LOWER_LIMIT ||
		    cfg->cmd.operation == LOG_COMMAND_SET_UPPER_LIMIT) {
			DOCA_LOG_INFO("---- Command argument: %d", cfg->cmd_arg);
		}
	} else {
		DOCA_LOG_INFO("---- Sample is a server");
		DOCA_LOG_INFO("---- UDP server port: %d", cfg->server_port);
	}

	return DOCA_SUCCESS;
}

bool is_log_level_valid(uint32_t limit)
{
	if (limit % LOG_LEVEL_MULTIPLIER != 0) {
		DOCA_LOG_ERR("Log level limit is not a multiple of %d: '%u'", LOG_LEVEL_MULTIPLIER, limit);
		return false;
	}

	if (limit < DOCA_LOG_LEVEL_DISABLE || limit > DOCA_LOG_LEVEL_TRACE) {
		DOCA_LOG_ERR("Log level limit out of range %d-%d: '%u'",
			     DOCA_LOG_LEVEL_DISABLE,
			     DOCA_LOG_LEVEL_TRACE,
			     limit);
		return false;
	}

	return true;
}
