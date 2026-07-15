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

#include <stdlib.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include "verbs_server_client_common.h"

DOCA_LOG_REGISTER(VERBS_SERVER_CLIENT::MAIN);

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_name_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(verbs_cfg->device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set GID index
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t gid_index_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int gid_index = *(uint32_t *)param;

	if (gid_index < 0) {
		DOCA_LOG_ERR("GID index for DOCA RDMA Verbs must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->gid_index = (uint32_t)gid_index;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle client parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t client_param_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	char *server_ip_addr = (char *)param;
	int len;

	len = strnlen(server_ip_addr, MAX_IP_ADDRESS_LEN + 1);
	if (len == MAX_IP_ADDRESS_LEN) {
		DOCA_LOG_ERR("Entered server address exceeded the maximum size of %d", MAX_IP_ADDRESS_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(verbs_cfg->server_ip_addr, server_ip_addr, len + 1);
	verbs_cfg->is_server = false;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle rdma_oper_type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_oper_type_param_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const char *rdma_oper_type = (char *)param;
	int type_len = strnlen(rdma_oper_type, RDMA_OPER_TYPE_LEN + 1);

	if (type_len == RDMA_OPER_TYPE_LEN) {
		DOCA_LOG_ERR("Entered RDMA operation type exceeded the maximum size of: %d", RDMA_OPER_TYPE_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcasecmp(rdma_oper_type, "SEND") == 0)
		verbs_cfg->rdma_oper_type = VERBS_SAMPLE_OPERATION_SEND_RECEIVE;
	else if (strcasecmp(rdma_oper_type, "WRITE") == 0)
		verbs_cfg->rdma_oper_type = VERBS_SAMPLE_OPERATION_WRITE;
	else if (strcasecmp(rdma_oper_type, "READ") == 0)
		verbs_cfg->rdma_oper_type = VERBS_SAMPLE_OPERATION_READ;
	else {
		DOCA_LOG_ERR("Entered wrong RDMA operation type, the accepted RDMA operation types are: "
			     "Send, SEND, send, "
			     "Write, WRITE, write, "
			     "Read, READ, read");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle buf_size parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t buffer_size_param_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int buf_size = *(uint32_t *)param;

	if (buf_size < 0) {
		DOCA_LOG_ERR("RDMA operation for DOCA RDMA Verbs must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->buf_size = (uint32_t)buf_size;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle qp_type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t qp_type_param_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const char *type = (char *)param;
	int type_len = strnlen(type, QP_TYPE_LEN + 1);

	if (type_len == QP_TYPE_LEN) {
		DOCA_LOG_ERR("Entered QP type exceeded buffer the maximum size of %d", QP_TYPE_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcasecmp(type, "RC") == 0)
		verbs_cfg->qp_type = DOCA_VERBS_QP_TYPE_RC;
	else {
		DOCA_LOG_ERR("Entered wrong QP type, the accepted QP types are: "
			     "Rc, RC, rc");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set Number of iterations
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t iter_num_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;
	const int iter_num = *(uint32_t *)param;

	if (iter_num < 1 || iter_num > 10000) {
		DOCA_LOG_ERR("Number of iterations for DOCA RDMA Verbs Sample must be in range of 1 - 10000");
		return DOCA_ERROR_INVALID_VALUE;
	}

	verbs_cfg->iter_num = (uint32_t)iter_num;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Set Send Burst flag
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_burst_callback(void *param, void *config)
{
	struct verbs_config *verbs_cfg = (struct verbs_config *)config;

	verbs_cfg->send_burst = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * Register sample argp params
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_rdma_params(void)
{
	doca_error_t status;
	struct doca_argp_param *client_param;
	struct doca_argp_param *device_param;
	struct doca_argp_param *gid_index_param;
	struct doca_argp_param *rdma_oper_type_param;
	struct doca_argp_param *buffer_size_param;
	struct doca_argp_param *qp_type_param;
	struct doca_argp_param *iter_num_param;
	struct doca_argp_param *send_burst_param;

	/* Create and register client param */
	status = doca_argp_param_create(&client_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(client_param, "c");
	doca_argp_param_set_long_name(client_param, "client");
	doca_argp_param_set_arguments(client_param, "<Sample is client, requires server OOB IP>");
	doca_argp_param_set_description(client_param, "Sample is client, requires server OOB IP");
	doca_argp_param_set_callback(client_param, client_param_callback);
	doca_argp_param_set_type(client_param, DOCA_ARGP_TYPE_STRING);
	status = doca_argp_register_param(client_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register device param */
	status = doca_argp_param_create(&device_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<IB device name>");
	doca_argp_param_set_description(device_param, "IB device name");
	doca_argp_param_set_callback(device_param, device_name_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	status = doca_argp_register_param(device_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register gid_index param */
	status = doca_argp_param_create(&gid_index_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(gid_index_param, "gid");
	doca_argp_param_set_long_name(gid_index_param, "gid-index");
	doca_argp_param_set_description(gid_index_param, "GID index for DOCA RDMA. Default value 0");
	doca_argp_param_set_callback(gid_index_param, gid_index_callback);
	doca_argp_param_set_type(gid_index_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(gid_index_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register rdma_oper_type param */
	status = doca_argp_param_create(&rdma_oper_type_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(rdma_oper_type_param, "ro");
	doca_argp_param_set_long_name(rdma_oper_type_param, "rdma-oper-type");
	doca_argp_param_set_description(rdma_oper_type_param,
					"RDMA operation type: Send, Write, Read. Default type Send");
	doca_argp_param_set_callback(rdma_oper_type_param, rdma_oper_type_param_callback);
	doca_argp_param_set_type(rdma_oper_type_param, DOCA_ARGP_TYPE_STRING);
	status = doca_argp_register_param(rdma_oper_type_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register buf_size param */
	status = doca_argp_param_create(&buffer_size_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(buffer_size_param, "s");
	doca_argp_param_set_long_name(buffer_size_param, "buffer-size");
	doca_argp_param_set_description(buffer_size_param, "Buffer size. Default value 64");
	doca_argp_param_set_callback(buffer_size_param, buffer_size_param_callback);
	doca_argp_param_set_type(buffer_size_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(buffer_size_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register qp_type param */
	status = doca_argp_param_create(&qp_type_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(qp_type_param, "qt");
	doca_argp_param_set_long_name(qp_type_param, "qp-type");
	doca_argp_param_set_description(qp_type_param, "QP Type: RC. Default type RC");
	doca_argp_param_set_callback(qp_type_param, qp_type_param_callback);
	doca_argp_param_set_type(qp_type_param, DOCA_ARGP_TYPE_STRING);
	status = doca_argp_register_param(qp_type_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register gid_index param */
	status = doca_argp_param_create(&iter_num_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(iter_num_param, "n");
	doca_argp_param_set_long_name(iter_num_param, "iters");
	doca_argp_param_set_description(iter_num_param, "Number of iterations. Default value 1");
	doca_argp_param_set_callback(iter_num_param, iter_num_callback);
	doca_argp_param_set_type(iter_num_param, DOCA_ARGP_TYPE_INT);
	status = doca_argp_register_param(iter_num_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create and register send_burst param */
	status = doca_argp_param_create(&send_burst_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(status));
		return status;
	}
	doca_argp_param_set_short_name(send_burst_param, "b");
	doca_argp_param_set_long_name(send_burst_param, "send-burst");
	doca_argp_param_set_description(send_burst_param, "Send all messages at once in a burst");
	doca_argp_param_set_callback(send_burst_param, send_burst_callback);
	doca_argp_param_set_type(send_burst_param, DOCA_ARGP_TYPE_BOOLEAN);
	status = doca_argp_register_param(send_burst_param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(status));
		return status;
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
	struct verbs_config cfg = {0};
	doca_error_t status;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values */
	cfg.is_server = true;
	cfg.gid_index = DEFAULT_GID_INDEX;
	cfg.buf_size = DEFAULT_BUFFER_SIZE;
	cfg.rdma_oper_type = VERBS_SAMPLE_OPERATION_SEND_RECEIVE;
	cfg.qp_type = DOCA_VERBS_QP_TYPE_RC;
	cfg.iter_num = DEFAULT_NUM_OF_INTERATIONS;
	cfg.send_burst = false;

	/* Register a logger backend */
	status = doca_log_backend_create_standard();
	if (status != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK */
	status = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (status != DOCA_SUCCESS)
		goto sample_exit;
	status = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_INFO);
	if (status != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the Sample");

	/* Initialize argparser */
	status = doca_argp_init("doca_verbs_server_client", &cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(status));
		goto sample_exit;
	}

	/* Register RDMA Verbs common params */
	status = register_rdma_params();
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample parameters: %s", doca_error_get_descr(status));
		goto argp_cleanup;
	}

	/* Start argparser */
	status = doca_argp_start(argc, argv);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(status));
		goto argp_cleanup;
	}

	/* Start sample */
	if (cfg.is_server) {
		status = verbs_server(&cfg);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("verbs_server() failed: %s", doca_error_get_descr(status));
			goto argp_cleanup;
		}
	} else {
		status = verbs_client(&cfg);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("verbs_client() failed: %s", doca_error_get_descr(status));
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
