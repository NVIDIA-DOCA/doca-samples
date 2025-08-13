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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

#include "rdma_common.h"

DOCA_LOG_REGISTER(RDMA::COMMON);

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_address_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(rdma_cfg->device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle send string parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_string_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *send_string = (char *)param;
	int len;

	len = strnlen(send_string, MAX_ARG_SIZE);
	if (len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered send string exceeded buffer size: %d", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->send_string, send_string, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle read string parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t read_string_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *read_string = (char *)param;
	int len;

	len = strnlen(read_string, MAX_ARG_SIZE);
	if (len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered read string exceeded buffer size: %d", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->read_string, read_string, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle write string parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t write_string_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	char *write_string = (char *)param;
	int len;

	len = strnlen(write_string, MAX_ARG_SIZE);
	if (len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered send string exceeded buffer size: %d", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->write_string, write_string, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle exported descriptor file path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t local_descriptor_path_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *path = (char *)param;
	int path_len;

	path_len = strnlen(path, MAX_ARG_SIZE);
	if (path_len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered path exceeded buffer size: %d", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->local_connection_desc_path, path, path_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle exported descriptor file path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t remote_descriptor_path_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *path = (char *)param;
	int path_len = strnlen(path, MAX_ARG_SIZE);

	if (path_len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered path exceeded buffer size: %d", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->remote_connection_desc_path, path, path_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle exported descriptor file path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mmap_descriptor_path_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *path = (char *)param;
	int path_len = strnlen(path, MAX_ARG_SIZE);

	if (path_len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered path exceeded buffer size: %d", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->remote_resource_desc_path, path, path_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle gid_index parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t gid_index_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const int gid_index = *(uint32_t *)param;

	if (gid_index < 0) {
		DOCA_LOG_ERR("GID index for DOCA RDMA must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rdma_cfg->is_gid_index_set = true;
	rdma_cfg->gid_index = (uint32_t)gid_index;

	return DOCA_SUCCESS;
}

doca_error_t register_rdma_send_string_param(void)
{
	struct doca_argp_param *send_string_param;
	doca_error_t result;

	/* Create and register send_string param */
	result = doca_argp_param_create(&send_string_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(send_string_param, "s");
	doca_argp_param_set_long_name(send_string_param, "send-string");
	doca_argp_param_set_arguments(send_string_param, "<Send string>");
	doca_argp_param_set_description(send_string_param,
					"String to send (optional). If not provided then \"" DEFAULT_STRING
					"\" will be chosen");
	doca_argp_param_set_callback(send_string_param, send_string_callback);
	doca_argp_param_set_type(send_string_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(send_string_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t register_rdma_read_string_param(void)
{
	struct doca_argp_param *read_string_param;
	doca_error_t result;

	/* Create and register read_string param */
	result = doca_argp_param_create(&read_string_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(read_string_param, "r");
	doca_argp_param_set_long_name(read_string_param, "read-string");
	doca_argp_param_set_arguments(read_string_param, "<Read string>");
	doca_argp_param_set_description(read_string_param,
					"String to read (optional). If not provided then \"" DEFAULT_STRING
					"\" will be chosen");
	doca_argp_param_set_callback(read_string_param, read_string_callback);
	doca_argp_param_set_type(read_string_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(read_string_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t register_rdma_write_string_param(void)
{
	struct doca_argp_param *write_string_param;
	doca_error_t result;

	/* Create and register write_string param */
	result = doca_argp_param_create(&write_string_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(write_string_param, "w");
	doca_argp_param_set_long_name(write_string_param, "write-string");
	doca_argp_param_set_arguments(write_string_param, "<Write string>");
	doca_argp_param_set_description(write_string_param,
					"String to write (optional). If not provided then \"" DEFAULT_STRING
					"\" will be chosen");
	doca_argp_param_set_callback(write_string_param, write_string_callback);
	doca_argp_param_set_type(write_string_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(write_string_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * ARGP Callback - Handle num_connections parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_connections_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const uint32_t num_connections = *(uint32_t *)param;

	if (num_connections > MAX_NUM_CONNECTIONS) {
		DOCA_LOG_ERR("Max number of connections must be <= [%d]", MAX_NUM_CONNECTIONS);
		return DOCA_ERROR_INVALID_VALUE;
	}

	rdma_cfg->num_connections = num_connections;

	return DOCA_SUCCESS;
}

doca_error_t register_rdma_num_connections_param(void)
{
	struct doca_argp_param *num_connections_param;
	doca_error_t result;
	static char param_desc[MAX_ARG_SIZE];

	/* Create and register num_connections param */
	result = doca_argp_param_create(&num_connections_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(num_connections_param, "nc");
	doca_argp_param_set_long_name(num_connections_param, "num-connections");
	snprintf(param_desc,
		 MAX_ARG_SIZE,
		 "%s%d%s",
		 "num_connections for DOCA RDMA (optional), max connections number must be <= ",
		 MAX_NUM_CONNECTIONS,
		 " in this sample");
	doca_argp_param_set_description(num_connections_param, param_desc);
	doca_argp_param_set_callback(num_connections_param, num_connections_param_callback);
	doca_argp_param_set_type(num_connections_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_connections_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}
	return result;
}

/*
 * ARGP Callback - Handle transport_type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t transport_type_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *type = (char *)param;

	if (strcasecmp(type, "RC") == 0)
		rdma_cfg->transport_type = DOCA_RDMA_TRANSPORT_TYPE_RC;
	else if (strcasecmp(type, "DC") == 0)
		rdma_cfg->transport_type = DOCA_RDMA_TRANSPORT_TYPE_DC;
	else {
		DOCA_LOG_ERR("Entered wrong RDMA transport_type, the accepted RDMA transport_type are: "
			     "RC, rc, "
			     "DC, dc");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle use_rdma_cm parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t use_rdma_cm_param_callback(void *param, void *config)
{
	(void)param;
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;

	rdma_cfg->use_rdma_cm = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cm_port parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cm_port_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const int cm_port = *(uint32_t *)param;

	if (cm_port < 0) {
		DOCA_LOG_ERR("Server listening port number for DOCA RDMA-CM must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rdma_cfg->cm_port = (uint32_t)cm_port;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cm server addr parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cm_addr_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *addr = (char *)param;
	int addr_len = strnlen(addr, SERVER_ADDR_LEN + 1);

	if (addr_len == SERVER_ADDR_LEN) {
		DOCA_LOG_ERR("Entered server address exceeded buffer size: %d", SERVER_ADDR_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(rdma_cfg->cm_addr, addr, addr_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cm server addr type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cm_addr_type_param_callback(void *param, void *config)
{
	struct rdma_config *rdma_cfg = (struct rdma_config *)config;
	const char *type = (char *)param;
	int type_len = strnlen(type, SERVER_ADDR_TYPE_LEN + 1);

	if (type_len == SERVER_ADDR_TYPE_LEN) {
		DOCA_LOG_ERR("Entered server address type exceeded buffer size: %d", SERVER_ADDR_TYPE_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcasecmp(type, "ip4") == 0 || strcasecmp(type, "ipv4") == 0)
		rdma_cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_IPv4;
	else if (strcasecmp(type, "ip6") == 0 || strcasecmp(type, "ipv6") == 0)
		rdma_cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_IPv6;
	else if (strcasecmp(type, "gid") == 0)
		rdma_cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_GID;
	else {
		DOCA_LOG_ERR("Entered wrong server address type, the accepted server address type are: "
			     "ip4, ipv4, IP4, IPv4, IPV4, "
			     "ip6, ipv6, IP6, IPv6, IPV6, "
			     "gid, GID");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * A wrapper for handling rdma_cm related cmdline parameters
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_rdma_cm_params(void)
{
	doca_error_t result;
	struct doca_argp_param *use_rdma_cm_param;
	struct doca_argp_param *cm_port_param;
	struct doca_argp_param *cm_addr_param;
	struct doca_argp_param *cm_addr_type_param;

	/* Create and register user_rdma_cm param */
	result = doca_argp_param_create(&use_rdma_cm_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(use_rdma_cm_param, "cm");
	doca_argp_param_set_long_name(use_rdma_cm_param, "use-rdma-cm");
	doca_argp_param_set_description(use_rdma_cm_param, "Whether to use rdma-cm or oob to setup connection");
	doca_argp_param_set_callback(use_rdma_cm_param, use_rdma_cm_param_callback);
	doca_argp_param_set_type(use_rdma_cm_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(use_rdma_cm_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cm_port_param */
	result = doca_argp_param_create(&cm_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cm_port_param, "lp");
	doca_argp_param_set_long_name(cm_port_param, "listen-port");
	doca_argp_param_set_arguments(cm_port_param, "<listen-port-num>");
	doca_argp_param_set_description(cm_port_param, "server listen port number");
	doca_argp_param_set_callback(cm_port_param, cm_port_param_callback);
	doca_argp_param_set_type(cm_port_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(cm_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cm_addr_param */
	result = doca_argp_param_create(&cm_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cm_addr_param, "sa");
	doca_argp_param_set_long_name(cm_addr_param, "server-addr");
	doca_argp_param_set_arguments(cm_addr_param, "<server address>");
	doca_argp_param_set_description(cm_addr_param, "Rdma cm server device address");
	doca_argp_param_set_callback(cm_addr_param, cm_addr_param_callback);
	doca_argp_param_set_type(cm_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(cm_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cm_addr_type_param */
	result = doca_argp_param_create(&cm_addr_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cm_addr_type_param, "sat");
	doca_argp_param_set_long_name(cm_addr_type_param, "server-addr-type");
	doca_argp_param_set_arguments(cm_addr_type_param, "<server address type>");
	doca_argp_param_set_description(cm_addr_type_param, "Rdma cm server device address type: IPv4, IPv6 or GID");
	doca_argp_param_set_callback(cm_addr_type_param, cm_addr_type_param_callback);
	doca_argp_param_set_type(cm_addr_type_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(cm_addr_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t register_rdma_common_params(void)
{
	doca_error_t result;
	struct doca_argp_param *device_param;
	struct doca_argp_param *local_desc_path_param;
	struct doca_argp_param *remote_desc_path_param;
	struct doca_argp_param *remote_resource_desc_path;
	struct doca_argp_param *gid_index_param;
	struct doca_argp_param *transport_type_param;

	/* Create and register device param */
	result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<IB device name>");
	doca_argp_param_set_description(device_param, "IB device name");
	doca_argp_param_set_callback(device_param, device_address_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	result = doca_argp_register_param(device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register local descriptor file path param */
	result = doca_argp_param_create(&local_desc_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(local_desc_path_param, "ld");
	doca_argp_param_set_long_name(local_desc_path_param, "local-descriptor-path");
	doca_argp_param_set_description(
		local_desc_path_param,
		"Local descriptor file path that includes the local connection descriptor, to be copied to the remote program, "
		"used only when not using the use-rdma-cm flag");
	doca_argp_param_set_callback(local_desc_path_param, local_descriptor_path_callback);
	doca_argp_param_set_type(local_desc_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(local_desc_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register remote descriptor file path param */
	result = doca_argp_param_create(&remote_desc_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(remote_desc_path_param, "re");
	doca_argp_param_set_long_name(remote_desc_path_param, "remote-descriptor-path");
	doca_argp_param_set_description(
		remote_desc_path_param,
		"Remote descriptor file path that includes the remote connection descriptor, to be copied from the remote program, "
		"used only when not using the use-rdma-cm flag");
	doca_argp_param_set_callback(remote_desc_path_param, remote_descriptor_path_callback);
	doca_argp_param_set_type(remote_desc_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(remote_desc_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register mmap descriptor file path param */
	result = doca_argp_param_create(&remote_resource_desc_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(remote_resource_desc_path, "m");
	doca_argp_param_set_long_name(remote_resource_desc_path, "remote-resource-descriptor-path");
	doca_argp_param_set_description(
		remote_resource_desc_path,
		"Remote descriptor file path that includes the remote mmap connection descriptor, to be copied from the remote program, "
		"used only when not using the use-rdma-cm flag");
	doca_argp_param_set_callback(remote_resource_desc_path, mmap_descriptor_path_callback);
	doca_argp_param_set_type(remote_resource_desc_path, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(remote_resource_desc_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register gid_index param */
	result = doca_argp_param_create(&gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gid_index_param, "g");
	doca_argp_param_set_long_name(gid_index_param, "gid-index");
	doca_argp_param_set_description(gid_index_param, "GID index for DOCA RDMA (optional)");
	doca_argp_param_set_callback(gid_index_param, gid_index_param_callback);
	doca_argp_param_set_type(gid_index_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register transport_type param */
	result = doca_argp_param_create(&transport_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(transport_type_param, "tt");
	doca_argp_param_set_long_name(transport_type_param, "transport-type");
	doca_argp_param_set_description(
		transport_type_param,
		"transport_type for DOCA RDMA (RC or DC, optional), only useful for single connection out-of-band RDMA for now");
	doca_argp_param_set_callback(transport_type_param, transport_type_param_callback);
	doca_argp_param_set_type(transport_type_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(transport_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return register_rdma_cm_params();
}

/*
 * Open DOCA device
 *
 * @device_name [in]: The name of the wanted IB device (could be empty string)
 * @func [in]: Function to check if a given device is capable of executing some task
 * @doca_device [out]: An allocated DOCA device on success and NULL otherwise
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_doca_device(const char *device_name, task_check func, struct doca_dev **doca_device)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	/* Search device with same dev name*/
	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS ||
		    (strlen(device_name) != 0 && strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) != 0))
			continue;
		/* If any special capabilities are needed */
		if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
			continue;
		result = doca_dev_open(dev_list[i], doca_device);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
			goto out;
		}
		break;
	}

out:
	doca_devinfo_destroy_list(dev_list);

	if (*doca_device == NULL) {
		DOCA_LOG_ERR("Couldn't get DOCA device");
		return DOCA_ERROR_NOT_FOUND;
	}

	return result;
}

doca_error_t allocate_rdma_resources(struct rdma_config *cfg,
				     const uint32_t mmap_permissions,
				     const uint32_t rdma_permissions,
				     task_check func,
				     struct rdma_resources *resources)
{
	doca_error_t result, tmp_result;

	resources->cfg = cfg;
	resources->first_encountered_error = DOCA_SUCCESS;
	resources->run_pe_progress = true;
	resources->num_remaining_tasks = 0;

	/* Check configuration correctness, for now, DC is only supported for out-of-band single connection sample */
	if (((cfg->num_connections > 1) || (cfg->use_rdma_cm == true)) &&
	    (cfg->transport_type == DOCA_RDMA_TRANSPORT_TYPE_DC)) {
		DOCA_LOG_ERR(
			"Failed to allocate RDMA resources: due to DOCA_RDMA_TRANSPORT_TYPE_DC is only supported for out-of-band single connection case for now");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Open DOCA device */
	result = open_doca_device(cfg->device_name, func, &(resources->doca_device));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
		return result;
	}

	/* Allocate memory for memory range */
	resources->mmap_memrange = calloc(MEM_RANGE_LEN, sizeof(*resources->mmap_memrange));
	if (resources->mmap_memrange == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for mmap_memrange: %s", doca_error_get_descr(result));
		result = DOCA_ERROR_NO_MEMORY;
		goto close_doca_dev;
	}

	/* Create mmap with allocated memory */
	result = create_local_mmap(&(resources->mmap),
				   mmap_permissions,
				   (void *)resources->mmap_memrange,
				   MEM_RANGE_LEN,
				   resources->doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA mmap: %s", doca_error_get_descr(result));
		goto free_memrange;
	}

	result = doca_pe_create(&(resources->pe));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions to DOCA RDMA: %s", doca_error_get_descr(result));
		goto destroy_doca_mmap;
	}

	/* Create DOCA RDMA instance */
	result = doca_rdma_create(resources->doca_device, &(resources->rdma));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA RDMA: %s", doca_error_get_descr(result));
		goto destroy_pe;
	}

	/* Convert DOCA RDMA to general DOCA context */
	resources->rdma_ctx = doca_rdma_as_ctx(resources->rdma);
	if (resources->rdma_ctx == NULL) {
		result = DOCA_ERROR_UNEXPECTED;
		DOCA_LOG_ERR("Failed to convert DOCA RDMA to DOCA context: %s", doca_error_get_descr(result));
		goto destroy_doca_rdma;
	}

	/* Set permissions to DOCA RDMA */
	result = doca_rdma_set_permissions(resources->rdma, rdma_permissions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions to DOCA RDMA: %s", doca_error_get_descr(result));
		goto destroy_doca_rdma;
	}

	/* Set gid_index to DOCA RDMA if it's provided */
	if (cfg->is_gid_index_set) {
		/* Set gid_index to DOCA RDMA */
		result = doca_rdma_set_gid_index(resources->rdma, cfg->gid_index);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set gid_index to DOCA RDMA: %s", doca_error_get_descr(result));
			goto destroy_doca_rdma;
		}
	}

	/* Set num_connections to DOCA RDMA */
	result = doca_rdma_set_max_num_connections(resources->rdma, cfg->num_connections);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max_num_connections to DOCA RDMA: %s", doca_error_get_descr(result));
		goto destroy_doca_rdma;
	}

	/* Set transport type */
	result = doca_rdma_set_transport_type(resources->rdma, cfg->transport_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RDMA transport type: %s", doca_error_get_descr(result));
		goto destroy_doca_rdma;
	}

	result = doca_pe_connect_ctx(resources->pe, resources->rdma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set progress engine for RDMA: %s", doca_error_get_descr(result));
		goto destroy_doca_rdma;
	}

	return result;

destroy_doca_rdma:
	/* Destroy DOCA RDMA */
	tmp_result = doca_rdma_destroy(resources->rdma);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_pe:
	/* Destroy DOCA progress engine */
	tmp_result = doca_pe_destroy(resources->pe);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_doca_mmap:
	/* Destroy DOCA mmap */
	tmp_result = doca_mmap_destroy(resources->mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_memrange:
	/* Free DOCA mmap memory range */
	free(resources->mmap_memrange);
close_doca_dev:
	/* Close DOCA device */
	tmp_result = doca_dev_close(resources->doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t delete_file(const char *file_path)
{
	FILE *fp;
	int res;
	doca_error_t result = DOCA_SUCCESS;

	/* Check if file exists before deleting it */
	fp = fopen(file_path, "r");
	if (fp) {
		/* Delete file by using unlink */
		res = unlink(file_path);
		if (res != 0) {
			result = DOCA_ERROR_IO_FAILED;
			DOCA_LOG_ERR("Failed to delete file %s: %s", file_path, doca_error_get_descr(result));
		}
		fclose(fp);
	}

	return result;
}

/*
 * Delete the description files that we created
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t clean_up_files(struct rdma_config *cfg)
{
	doca_error_t result = DOCA_SUCCESS;

	result = delete_file(cfg->local_connection_desc_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Deleting file %s failed: %s",
			     cfg->local_connection_desc_path,
			     doca_error_get_descr(result));
		return result;
	}

	result = delete_file(cfg->remote_connection_desc_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Deleting file %s failed: %s",
			     cfg->remote_connection_desc_path,
			     doca_error_get_descr(result));
		return result;
	}

	result = delete_file(cfg->remote_resource_desc_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Deleting file %s failed: %s",
			     cfg->remote_resource_desc_path,
			     doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Destroy DOCA RDMA-CM related resources
 *
 * @resources [in]: DOCA RDMA-CM related resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_rdma_cm_resources(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	if (resources->cm_addr != NULL) {
		result = doca_rdma_addr_destroy(resources->cm_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA rdma cm address: %s", doca_error_get_descr(result));
			return result;
		}
	}
	if (resources->mmap_descriptor_mmap != NULL) {
		tmp_result = doca_mmap_destroy(resources->mmap_descriptor_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA local mmap descriptor mmap: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	if (resources->remote_mmap_descriptor_mmap != NULL) {
		tmp_result = doca_mmap_destroy(resources->remote_mmap_descriptor_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA remote mmap descriptor mmap: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	if (resources->sync_event_descriptor_mmap != NULL) {
		tmp_result = doca_mmap_destroy(resources->sync_event_descriptor_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA local sync_event descriptor mmap: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	return result;
}

doca_error_t destroy_rdma_resources(struct rdma_resources *resources, struct rdma_config *cfg)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	enum doca_ctx_states ctx_state = DOCA_CTX_STATE_IDLE;
	result = doca_ctx_get_state(doca_rdma_as_ctx(resources->rdma), &ctx_state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DOCA rdma ctx state: %s", doca_error_get_descr(result));
	}
	if (ctx_state != DOCA_CTX_STATE_IDLE) {
		(void)doca_ctx_stop(doca_rdma_as_ctx(resources->rdma));
	}

	/* Stop and destroy remote mmap if exists */
	if (resources->remote_mmap != NULL) {
		result = doca_mmap_stop(resources->remote_mmap);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA remote mmap: %s", doca_error_get_descr(result));

		tmp_result = doca_mmap_destroy(resources->remote_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA remote mmap: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	if (resources->cfg->use_rdma_cm == true) {
		tmp_result = destroy_rdma_cm_resources(resources);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy rdma cm resources: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	/* Destroy DOCA RDMA */
	tmp_result = doca_rdma_destroy(resources->rdma);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA RDMA: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Destroy DOCA progress engine */
	tmp_result = doca_pe_destroy(resources->pe);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Stop DOCA mmap */
	tmp_result = doca_mmap_stop(resources->mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Free DOCA mmap memory range */
	free(resources->mmap_memrange);

	/* Destroy DOCA mmap */
	tmp_result = doca_mmap_destroy(resources->mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Free remote connection descriptor */
	if (resources->remote_rdma_conn_descriptor)
		free(resources->remote_rdma_conn_descriptor);

	/* Free remote mmap descriptor */
	if (resources->remote_mmap_descriptor)
		free(resources->remote_mmap_descriptor);

	/* Free remote mmap descriptor */
	if (resources->sync_event_descriptor)
		free(resources->sync_event_descriptor);

	/* Close DOCA device */
	tmp_result = doca_dev_close(resources->doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Delete description files that we created */
	tmp_result = clean_up_files(cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to clean up files: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t write_file(const char *file_path, const char *string, size_t string_len)
{
	FILE *fp;
	doca_error_t result = DOCA_SUCCESS;

	/* Check if the file exists by opening it to read */
	fp = fopen(file_path, "r");
	if (fp) {
		DOCA_LOG_ERR("File %s already exists. Please delete it prior to running the sample", file_path);
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	fp = fopen(file_path, "wb");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to create file %s", file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Write the string */
	if (fwrite(string, 1, string_len, fp) != string_len) {
		DOCA_LOG_ERR("Failed to write on file %s", file_path);
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Close the file */
	fclose(fp);

	return result;
}

doca_error_t read_file(const char *file_path, char **string, size_t *string_len)
{
	FILE *fp;
	long file_size;
	doca_error_t result = DOCA_SUCCESS;

	/* Open the file for reading */
	fp = fopen(file_path, "r");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to open the file %s for reading", file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Calculate the size of the file */
	if (fseek(fp, 0, SEEK_END) != 0) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	file_size = ftell(fp);
	if (file_size == -1) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Rewind file to the start */
	if (fseek(fp, 0, SEEK_SET) != 0) {
		DOCA_LOG_ERR("Failed to rewind file");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	*string_len = file_size;
	*string = malloc(file_size);
	if (*string == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory of size %lu", file_size);
		fclose(fp);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Read the file to the string */
	if (fread(*string, 1, file_size, fp) != (size_t)file_size) {
		DOCA_LOG_ERR("Failed read from file %s", file_path);
		result = DOCA_ERROR_IO_FAILED;
		free(*string);
	}

	fclose(fp);
	return result;
}

doca_error_t rdma_cm_connect(struct rdma_resources *resources)
{
	union doca_data connection_data;
	doca_error_t result = DOCA_SUCCESS;
	struct rdma_config *cfg = resources->cfg;

	resources->self_name = SERVER_NAME;
	resources->is_client = false;
	if (cfg->use_rdma_cm == true) {
		DOCA_LOG_INFO("Using RDMA_CM to setup RDMA connection");
		/* If cmdline option has a cm_addr param, the test object is used as a client; otherwise it is a server
		 */
		if (cfg->cm_addr[0] != '\0') {
			resources->self_name = CLIENT_NAME;
			resources->is_client = true;
		}
		if ((resources->is_client == true) && (cfg->num_connections > 1)) {
			DOCA_LOG_ERR("Client only support single connection, but input num_connections is [%u]",
				     cfg->num_connections);
			return DOCA_ERROR_INVALID_VALUE;
		}
	} else
		DOCA_LOG_INFO("Using Out-Of-Band to setup RDMA connection");

	DOCA_LOG_INFO("-----------------------------------------------");
	DOCA_LOG_INFO("RDMA_CM connection params:");
	DOCA_LOG_INFO("-- Connection Role: %s", (resources->is_client == true) ? CLIENT_NAME : SERVER_NAME);
	DOCA_LOG_INFO("-- Addr_type : %d", cfg->cm_addr_type);
	DOCA_LOG_INFO("-- Addr: %s", (cfg->cm_addr[0] == '\0') ? "NULL" : cfg->cm_addr);
	DOCA_LOG_INFO("-- Port: %u", cfg->cm_port);
	DOCA_LOG_INFO("-- Num_connections: %u", cfg->num_connections);
	DOCA_LOG_INFO("-----------------------------------------------");

	resources->cm_addr = NULL;
	resources->num_connection_established = 0;

	if (resources->is_client == false) {
		DOCA_LOG_INFO("Server calling doca_rdma_start_listen_to_port");
		result = doca_rdma_start_listen_to_port(resources->rdma, cfg->cm_port);
		if (DOCA_IS_ERROR(result)) {
			DOCA_LOG_ERR("Server failed to call doca_rdma_start_listen_to_port");
			return result;
		}
	} else {
		result = doca_rdma_addr_create(cfg->cm_addr_type, cfg->cm_addr, cfg->cm_port, &resources->cm_addr);
		if (DOCA_IS_ERROR(result)) {
			DOCA_LOG_ERR("Failed to create rdma cm connection address");
			return result;
		}
		if (resources->cm_addr == NULL) {
			DOCA_LOG_ERR("RDMA_CM client must be given a valid server address (ipv4, ipv6 or gid)");
			return DOCA_ERROR_INVALID_VALUE;
		}
		DOCA_LOG_INFO("Client calling doca_rdma_connect_to_addr");
		connection_data.ptr = (void *)resources;
		result = doca_rdma_connect_to_addr(resources->rdma, resources->cm_addr, connection_data);
		if (DOCA_IS_ERROR(result)) {
			doca_rdma_addr_destroy(resources->cm_addr);
			resources->cm_addr = NULL;
			DOCA_LOG_ERR("Client failed to call doca_rdma_connect_to_addr");
			return result;
		}
	}

	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("[%s] failed to start connection: %s", resources->self_name, doca_error_get_descr(result));
	else
		DOCA_LOG_INFO("[%s] started connection successfully", resources->self_name);

	return result;
}

doca_error_t rdma_cm_disconnect(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;
	uint32_t i = 0;

	for (i = 0; i < resources->num_connection_established; i++) {
		result = doca_rdma_connection_disconnect(resources->connections[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("[%s] cannot disconnect rdma-cm connection: %s",
				     resources->self_name,
				     doca_error_get_descr(result));
			break;
		} else
			DOCA_LOG_INFO("[%s] successfully disconnect rdma-cm connection", resources->self_name);
	}
	return result;
}

doca_error_t send_msg(struct doca_rdma *rdma,
		      struct doca_rdma_connection *rdma_connection,
		      struct doca_mmap *mmap,
		      struct doca_buf_inventory *buf_inv,
		      void *msg,
		      uint32_t msg_len,
		      void *user_data)
{
	doca_error_t result;
	struct doca_buf *src_buf;
	struct doca_rdma_task_send *rdma_send_task;
	union doca_data task_user_data;
	task_user_data.ptr = user_data;

	result = doca_buf_inventory_buf_get_by_data(buf_inv, mmap, msg, msg_len, &src_buf);
	if (DOCA_IS_ERROR(result)) {
		DOCA_LOG_ERR("Failed to get a doca_buf, with error: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_rdma_task_send_allocate_init(rdma, rdma_connection, src_buf, task_user_data, &rdma_send_task);
	if (DOCA_IS_ERROR(result)) {
		DOCA_LOG_ERR("Failed to allocate send task, with error: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_task_submit(doca_rdma_task_send_as_task(rdma_send_task));
	if (DOCA_IS_ERROR(result)) {
		DOCA_LOG_ERR("Failed to submit a send task, with error: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t recv_msg(struct doca_rdma *rdma,
		      struct doca_mmap *mmap,
		      struct doca_buf_inventory *buf_inv,
		      void *msg,
		      uint32_t msg_len,
		      void *user_data)
{
	doca_error_t result;
	struct doca_buf *dst_buf;
	struct doca_rdma_task_receive *rdma_recv_task;
	union doca_data task_user_data;
	task_user_data.ptr = user_data;

	result = doca_buf_inventory_buf_get_by_addr(buf_inv, mmap, msg, msg_len, &dst_buf);
	if (DOCA_IS_ERROR(result)) {
		DOCA_LOG_ERR("Failed to get a doca_buf, with error: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_rdma_task_receive_allocate_init(rdma, dst_buf, task_user_data, &rdma_recv_task);
	if (DOCA_IS_ERROR(result)) {
		DOCA_LOG_ERR("Failed to allocate receive task, with error: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_task_submit(doca_rdma_task_receive_as_task(rdma_recv_task));
	if (DOCA_IS_ERROR(result)) {
		DOCA_LOG_ERR("Failed to submit a receive task, with error: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_INFO("Negotiation receive task submission completed\n");

	return result;
}

doca_error_t rdma_requester_recv_data_from_rdma_responder(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;
	struct doca_mmap *recv_descriptor_mmap = NULL;
	void *recv_descriptor = NULL;
	size_t recv_descriptor_size = MEM_RANGE_LEN;

	DOCA_LOG_INFO("Start to exchange data resource between client and server");

	/* Create receive descriptor buffer  */
	recv_descriptor = malloc(sizeof(uint8_t) * recv_descriptor_size);
	if (recv_descriptor == NULL) {
		DOCA_LOG_ERR("Failed to allocate buffer for receive descriptor: %s", doca_error_get_descr(result));
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Create receive descriptor's mmap  */
	result = create_local_mmap(&recv_descriptor_mmap,
				   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE,
				   recv_descriptor,
				   recv_descriptor_size,
				   resources->doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create receive descriptor's mmap: %s", doca_error_get_descr(result));
		goto free_recv_descriptor;
	}

	result = recv_msg(resources->rdma,
			  recv_descriptor_mmap,
			  resources->buf_inventory,
			  recv_descriptor,
			  recv_descriptor_size,
			  resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to recvd responder's data to requester: %s", doca_error_get_descr(result));
		goto destroy_recv_descriptor_mmap;
	}

	if (resources->recv_sync_event_desc == true) {
		resources->sync_event_descriptor_mmap = recv_descriptor_mmap;
		resources->sync_event_descriptor = recv_descriptor;
	} else {
		resources->remote_mmap_descriptor_mmap = recv_descriptor_mmap;
		resources->remote_mmap_descriptor = recv_descriptor;
	}

	return DOCA_SUCCESS;

destroy_recv_descriptor_mmap:
	doca_mmap_destroy(recv_descriptor_mmap);
free_recv_descriptor:
	free(recv_descriptor);
	return result;
}

doca_error_t rdma_responder_send_data_to_rdma_requester(struct rdma_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;
	struct doca_mmap *send_descriptor_mmap = NULL;
	void *send_descriptor = NULL;
	size_t send_descriptor_size = 0;

	DOCA_LOG_INFO("Start to exchange data resource between client and server");

	if (resources->recv_sync_event_desc == true) {
		/* Export RDMA sync event */
		result = doca_sync_event_export_to_remote_net(resources->sync_event,
							      (const uint8_t **)&(resources->sync_event_descriptor),
							      &(resources->sync_event_descriptor_size));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to export DOCA sync event for RDMA: %s", doca_error_get_descr(result));
			return result;
		}
		send_descriptor = (void *)(resources->sync_event_descriptor);
		send_descriptor_size = resources->sync_event_descriptor_size;
	} else {
		/* Export RDMA mmap */
		result = doca_mmap_export_rdma(resources->mmap,
					       resources->doca_device,
					       (const void **)&(resources->mmap_descriptor),
					       &(resources->mmap_descriptor_size));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to export DOCA mmap for RDMA: %s", doca_error_get_descr(result));
			return result;
		}
		send_descriptor = (void *)(resources->mmap_descriptor);
		send_descriptor_size = resources->mmap_descriptor_size;
	}

	/* Create DOCA buffer inventory */
	result = doca_buf_inventory_create(INVENTORY_NUM_INITIAL_ELEMENTS, &resources->buf_inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA buffer inventory: %s", doca_error_get_descr(result));
		return result;
	}

	/* Start DOCA buffer inventory */
	result = doca_buf_inventory_start(resources->buf_inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA buffer inventory: %s", doca_error_get_descr(result));
		goto destroy_buf_inv;
	}

	/* Create local data descriptor's mmap  */
	result = create_local_mmap(&send_descriptor_mmap,
				   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE,
				   send_descriptor,
				   send_descriptor_size,
				   resources->doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create send mmap for local descriptor: %s", doca_error_get_descr(result));
		goto destroy_buf_inv;
	}
	if (resources->recv_sync_event_desc == true)
		resources->sync_event_descriptor_mmap = send_descriptor_mmap;
	else
		resources->mmap_descriptor_mmap = send_descriptor_mmap;

	/* Wait for enter which means that the requester has finished posting receive */
	DOCA_LOG_INFO(
		"Wait till the requester has finished the submission of the receive task for negotiation and press enter");
	wait_for_enter();

	result = send_msg(resources->rdma,
			  resources->connections[0],
			  send_descriptor_mmap,
			  resources->buf_inventory,
			  send_descriptor,
			  send_descriptor_size,
			  resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send responder's data to requester: %s", doca_error_get_descr(result));
		goto destroy_send_descriptor_mmap;
	}

	return DOCA_SUCCESS;

destroy_send_descriptor_mmap:
	doca_mmap_destroy(send_descriptor_mmap);
destroy_buf_inv:
	doca_buf_inventory_destroy(resources->buf_inventory);
	resources->buf_inventory = NULL;
	return result;
}

void receive_task_completion_cb(struct doca_rdma_task_receive *task,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	(void)task_user_data;
	unsigned long int dst_buf_data_len = 0;
	doca_error_t result = DOCA_SUCCESS;
	struct doca_buf *dst_buf = doca_rdma_task_receive_get_dst_buf(task);
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;
	bool cm_error_occur = false;

	result = doca_buf_get_data_len(dst_buf, &dst_buf_data_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get data length from doca_buf with error: %s", doca_error_get_name(result));
		cm_error_occur = true;
	}

	doca_task_free(doca_rdma_task_receive_as_task(task));
	doca_buf_dec_refcount(dst_buf, NULL);
	if (cm_error_occur == false) {
		if (resource->recv_sync_event_desc == false)
			resource->remote_mmap_descriptor_size = dst_buf_data_len;
		else
			resource->sync_event_descriptor_size = dst_buf_data_len;

		result = resource->task_fn(resource);
		if (result != DOCA_SUCCESS)
			cm_error_occur = true;
	}
	if (cm_error_occur == true)
		(void)doca_ctx_stop(resource->rdma_ctx);
}

void receive_task_error_cb(struct doca_rdma_task_receive *rdma_recv_task,
			   union doca_data task_user_data,
			   union doca_data ctx_user_data)
{
	(void)task_user_data;
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_rdma_task_receive_as_task(rdma_recv_task);

	/* Get the result of the task */
	DOCA_LOG_ERR("RDMA negotiation receive task failed: %s", doca_error_get_descr(doca_task_get_status(task)));
	doca_task_free(task);
	doca_buf_dec_refcount(doca_rdma_task_receive_get_dst_buf(rdma_recv_task), NULL);
	(void)doca_ctx_stop(resource->rdma_ctx);
}

void send_task_completion_cb(struct doca_rdma_task_send *task,
			     union doca_data task_user_data,
			     union doca_data ctx_user_data)
{
	(void)task_user_data;
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;
	doca_error_t result;

	doca_task_free(doca_rdma_task_send_as_task(task));
	doca_buf_dec_refcount((struct doca_buf *)(doca_rdma_task_send_get_src_buf(task)), NULL);

	result = resource->task_fn(resource);
	if (result != DOCA_SUCCESS)
		(void)doca_ctx_stop(resource->rdma_ctx);
}

void send_task_error_cb(struct doca_rdma_task_send *rdma_send_task,
			union doca_data task_user_data,
			union doca_data ctx_user_data)
{
	(void)task_user_data;
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_rdma_task_send_as_task(rdma_send_task);

	/* Get the result of the task */
	DOCA_LOG_ERR("RDMA negotiation send task failed: %s", doca_error_get_descr(doca_task_get_status(task)));
	doca_task_free(task);
	doca_buf_dec_refcount((struct doca_buf *)(doca_rdma_task_send_get_src_buf(rdma_send_task)), NULL);
	(void)doca_ctx_stop(resource->rdma_ctx);
}

void rdma_cm_connect_request_cb(struct doca_rdma_connection *connection, union doca_data ctx_user_data)
{
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;
	doca_error_t result;
	union doca_data connection_user_data;

	result = doca_rdma_connection_accept(connection, NULL, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to accept rdma cm connection: %s", doca_error_get_descr(result));
		resource->run_pe_progress = false;
		return;
	}

	connection_user_data.ptr = ctx_user_data.ptr;
	result = doca_rdma_connection_set_user_data(connection, connection_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set server connection user data: %s", doca_error_get_descr(result));
		resource->run_pe_progress = false;
	}
}

void rdma_cm_connect_established_cb(struct doca_rdma_connection *connection,
				    union doca_data connection_user_data,
				    union doca_data ctx_user_data)
{
	(void)connection_user_data;
	union doca_data connection_data;
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;

	connection_data.u64 = resource->num_connection_established;
	(void)doca_rdma_connection_set_user_data(connection, connection_data);
	resource->connections[resource->num_connection_established] = connection;
	resource->connection_established[resource->num_connection_established++] = true;

	if (resource->require_remote_mmap == false) {
		if (resource->num_connection_established >= resource->cfg->num_connections) {
			if (resource->task_fn(resource) != DOCA_SUCCESS)
				(void)doca_ctx_stop(resource->rdma_ctx);
		}
		return;
	}

	if (resource->is_requester == true)
		rdma_requester_recv_data_from_rdma_responder(resource);
	else
		rdma_responder_send_data_to_rdma_requester(resource);
}

void rdma_cm_connect_failure_cb(struct doca_rdma_connection *connection,
				union doca_data connection_user_data,
				union doca_data ctx_user_data)
{
	uint16_t connection_index = (uint16_t)(connection_user_data.u64);
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;

	if (resource->num_connection_established > 0) {
		if ((resource->connections[connection_index] == connection) &&
		    (resource->connection_established[connection_index] == true)) {
			resource->connection_established[connection_index] = false;
			--resource->num_connection_established;
		}
	}
	DOCA_ERROR_PROPAGATE(resource->first_encountered_error, DOCA_ERROR_CONNECTION_ABORTED);

	resource->run_pe_progress = false;
}

void rdma_cm_disconnect_cb(struct doca_rdma_connection *connection,
			   union doca_data connection_user_data,
			   union doca_data ctx_user_data)
{
	(void)connection_user_data;
	struct rdma_resources *resource = (struct rdma_resources *)ctx_user_data.ptr;
	doca_error_t result;

	result = doca_rdma_connection_disconnect(connection);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to disconnect rdma cm connection: %s", doca_error_get_descr(result));
		(void)doca_ctx_stop(resource->rdma_ctx);
		return;
	}

	resource->connections[--resource->num_connection_established] = NULL;
}

doca_error_t set_default_config_value(struct rdma_config *cfg)
{
	if (cfg == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Set the default configuration values (Example values) */
	strcpy(cfg->send_string, DEFAULT_STRING);
	strcpy(cfg->read_string, DEFAULT_STRING);
	strcpy(cfg->write_string, DEFAULT_STRING);
	strcpy(cfg->local_connection_desc_path, DEFAULT_LOCAL_CONNECTION_DESC_PATH);
	strcpy(cfg->remote_connection_desc_path, DEFAULT_REMOTE_CONNECTION_DESC_PATH);
	strcpy(cfg->remote_resource_desc_path, DEFAULT_REMOTE_RESOURCE_CONNECTION_DESC_PATH);
	cfg->is_gid_index_set = false;
	cfg->num_connections = 1;
	cfg->transport_type = DOCA_RDMA_TRANSPORT_TYPE_RC;

	/* Only related rdma cm */
	cfg->use_rdma_cm = false;
	cfg->cm_port = DEFAULT_RDMA_CM_PORT;
	cfg->cm_addr_type = DOCA_RDMA_ADDR_TYPE_IPv4;
	memset(cfg->cm_addr, 0, SERVER_ADDR_LEN);

	return DOCA_SUCCESS;
}

doca_error_t create_local_mmap(struct doca_mmap **mmap,
			       const uint32_t mmap_permissions,
			       void *data_buffer,
			       size_t data_buffer_size,
			       struct doca_dev *dev)
{
	doca_error_t result, tmp_result;

	/* Create mmap with no user data */
	result = doca_mmap_create(mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap for source buffer, error: %s", doca_error_get_descr(result));
		return result;
	}

	/* Set permissions for DOCA mmap */
	result = doca_mmap_set_permissions(*mmap, mmap_permissions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap source buffer permissions, error: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Set memory range for DOCA mmap */
	result = doca_mmap_set_memrange(*mmap, data_buffer, data_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memory range, error: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Add device to mmap */
	result = doca_mmap_add_dev(*mmap, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to mmap, error: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Start DOCA mmap */
	result = doca_mmap_start(*mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap, error: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	return DOCA_SUCCESS;

destroy_mmap:
	tmp_result = doca_mmap_destroy(*mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	*mmap = NULL;
	return result;
}

doca_error_t config_rdma_cm_callback_and_negotiation_task(struct rdma_resources *resources,
							  bool need_send_task,
							  bool need_recv_task)
{
	doca_error_t result = DOCA_SUCCESS;

	if (resources == NULL) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid resource pointer found, error: %s", doca_error_get_descr(result));
		return result;
	}

	/**
	 * Set send&recv task configuration
	 * they are used for transferring the mmap desc between client and server for non-sync-event task
	 * also used for transferring the sync-event desc between client and server for syn-event task
	 */
	if (need_recv_task == true) {
		result = doca_rdma_task_receive_set_conf(resources->rdma,
							 receive_task_completion_cb,
							 receive_task_error_cb,
							 NUM_NEGOTIATION_RDMA_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set task recv configuration, error: %s", doca_error_get_descr(result));
			return result;
		}
	}
	if (need_send_task == true) {
		result = doca_rdma_task_send_set_conf(resources->rdma,
						      send_task_completion_cb,
						      send_task_error_cb,
						      NUM_NEGOTIATION_RDMA_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set task send configuration, error: %s", doca_error_get_descr(result));
			return result;
		}
	}
	/* Set rdma cm connection configuration callbacks */
	result = doca_rdma_set_connection_state_callbacks(resources->rdma,
							  rdma_cm_connect_request_cb,
							  rdma_cm_connect_established_cb,
							  rdma_cm_connect_failure_cb,
							  rdma_cm_disconnect_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set rdma cm callback configuration, error: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

void wait_for_enter(void)
{
	int enter = 0;

	/* Wait for enter */
	while (enter != '\r' && enter != '\n')
		enter = getchar();
}
