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
#include <doca_flow.h>
#include <doca_flow_external_action_memory.h>
#include <doca_log.h>

#include <flow_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_MEMORY_EXTERNAL_ACTION::MAIN);

/* Sample's Logic */
doca_error_t flow_memory_external_action(int nb_queues,
					 bool use_access_index,
					 enum doca_flow_external_action_memory_operation operation);

static bool use_access_index;
static enum doca_flow_external_action_memory_operation operation = DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_ADD;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

/* Name lookup table for the --operation argument. */
struct memory_operation_name {
	const char *name;
	enum doca_flow_external_action_memory_operation op;
};

static const struct memory_operation_name memory_operation_names[] = {
	{"load", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_LOAD},
	{"store", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_STORE},
	{"add", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_ADD},
	{"xor", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_BITWISE_XOR},
	{"or", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_BITWISE_OR},
	{"and", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_BITWISE_AND},
	{"ffs-id-msb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_ID_MSB},
	{"ffs-bitmap-msb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_SET_BITMAP_MSB},
	{"ffs-id-lsb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_ID_LSB},
	{"ffs-bitmap-lsb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_SET_BITMAP_LSB},
	{"ffs-reset-id-msb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_ID_MSB},
	{"ffs-reset-bitmap-msb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_BITMAP_MSB},
	{"ffs-reset-id-lsb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_ID_LSB},
	{"ffs-reset-bitmap-lsb", DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_BITMAP_LSB},
};

#define NB_MEMORY_OPERATION_NAMES ARRAY_SIZE(memory_operation_names)

/*
 * ARGP callback for the --use-access-index flag
 *
 * @param [in]: the parsed boolean value
 * @config [in]: unused sample configuration
 * @return: DOCA_SUCCESS
 */
static doca_error_t use_access_index_callback(void *param, void *config)
{
	(void)config;
	use_access_index = *(bool *)param;
	DOCA_LOG_INFO("Memory offset source: %s",
		      use_access_index ? "access_index register (per-packet from the L4 source port)" :
					 "static resource offset");
	return DOCA_SUCCESS;
}

/*
 * ARGP callback for the --operation flag
 *
 * @param [in]: the parsed string value
 * @config [in]: unused sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t operation_callback(void *param, void *config)
{
	const char *name = (const char *)param;
	size_t i;

	(void)config;
	for (i = 0; i < NB_MEMORY_OPERATION_NAMES; i++) {
		if (strcmp(name, memory_operation_names[i].name) == 0) {
			operation = memory_operation_names[i].op;
			DOCA_LOG_INFO("Memory operation: %s", name);
			return DOCA_SUCCESS;
		}
	}
	DOCA_LOG_ERR("Unknown memory operation: %s", name);
	return DOCA_ERROR_INVALID_VALUE;
}

/*
 * Register the sample's command line parameters
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t flow_memory_external_action_register_params(void)
{
	struct doca_argp_param *use_access_index_param;
	struct doca_argp_param *operation_param;
	doca_error_t result;

	result = doca_argp_param_create(&use_access_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create use-access-index ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(use_access_index_param, "use-access-index");
	doca_argp_param_set_description(
		use_access_index_param,
		"Select the memory slot per packet from the access_index register (L4 source port) instead of a static offset.");
	doca_argp_param_set_callback(use_access_index_param, use_access_index_callback);
	doca_argp_param_set_type(use_access_index_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(use_access_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register use-access-index param: %s", doca_error_get_descr(result));
		(void)doca_argp_param_destroy(use_access_index_param);
		return result;
	}

	result = doca_argp_param_create(&operation_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create op ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(operation_param, "op");
	doca_argp_param_set_long_name(operation_param, "operation");
	doca_argp_param_set_arguments(
		operation_param,
		"<load|store|add|xor|or|and|ffs-id-msb|ffs-bitmap-msb|ffs-id-lsb|ffs-bitmap-lsb|ffs-reset-id-msb|ffs-reset-bitmap-msb|ffs-reset-id-lsb|ffs-reset-bitmap-lsb>");
	doca_argp_param_set_description(operation_param, "Memory operation to perform. Default: add.");
	doca_argp_param_set_callback(operation_param, operation_callback);
	doca_argp_param_set_type(operation_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(operation_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register op param: %s", doca_error_get_descr(result));
		(void)doca_argp_param_destroy(operation_param);
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
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct flow_dev_ctx flow_dev_ctx = {};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 2,
		.port_config.nb_queues = 1,
	};

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

	result = doca_argp_init(NULL, &flow_dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_flow_device_params(NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Register common flow statistics parameters */
	result = register_flow_stats_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register stats parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = flow_memory_external_action_register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Register tune profile parameters */
	result = register_flow_tune_profile_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register tune parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&flow_dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues");
		goto dpdk_cleanup;
	}

	/* run sample */
	result = flow_memory_external_action(dpdk_config.port_config.nb_queues, use_access_index, operation);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_memory_external_action() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini_with_devs(dpdk_config.port_config.nb_ports);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
