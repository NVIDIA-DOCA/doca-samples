/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <signal.h>
#include <stdint.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <utils.h>

#include "eth_l2_fwd_core.h"

DOCA_LOG_REGISTER(ETH_L2_FWD);

extern uint32_t max_forwardings;

/*
 * Signal handler
 *
 * @signum [in]: The signal received to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		eth_l2_fwd_force_stop();
	}
}

/*
 * ARGP Callback - Handle IB devices names parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t mlxdevs_names_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	char *mlxdevs_names = (char *)param;
	char *current_dev_name;
	int len;

	/* Split the devices names */
	/* First device name */
	current_dev_name = strtok(mlxdevs_names, ",");
	if (current_dev_name == NULL) {
		DOCA_LOG_ERR("Expected two IB devices names, but none entered");
		return DOCA_ERROR_INVALID_VALUE;
	}

	len = strnlen(current_dev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("First entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->mlxdev_name1, current_dev_name, len + 1);

	/* Second device name */
	current_dev_name = strtok(NULL, ",");
	if (current_dev_name == NULL) {
		DOCA_LOG_ERR("Expected two IB devices names, but only one entered");
		return DOCA_ERROR_INVALID_VALUE;
	}

	len = strnlen(current_dev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Second entered IB device name exceeding the maximum size of %d",
			     DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->mlxdev_name2, current_dev_name, len + 1);

	/* Check that no more names were entered */
	current_dev_name = strtok(NULL, ",");
	if (current_dev_name != NULL) {
		DOCA_LOG_ERR("Entered more than two IB devices names");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strncmp(app_cfg->mlxdev_name1, app_cfg->mlxdev_name2, DOCA_DEVINFO_IBDEV_NAME_SIZE) == 0) {
		DOCA_LOG_ERR("Identical devices names were provided, please enter different names");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle packets receive rate parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t pkts_recv_rate_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *pkts_recv_rate = (int *)param;

	if (*pkts_recv_rate <= 0) {
		DOCA_LOG_ERR("Packets receive rate parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->pkts_recv_rate = *pkts_recv_rate;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle max packet size parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t max_pkt_size_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *max_pkt_size = (int *)param;

	if (*max_pkt_size <= 0) {
		DOCA_LOG_ERR("Max packet size parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->max_pkt_size = *max_pkt_size;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle packet max process time parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t pkt_max_process_time_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *pkt_max_process_time = (int *)param;

	if (*pkt_max_process_time <= 0) {
		DOCA_LOG_ERR("Packet max process time parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (*pkt_max_process_time > UINT16_MAX) {
		DOCA_LOG_ERR("Packet max process time parameter can not be larger than %d", UINT16_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->pkt_max_process_time = *pkt_max_process_time;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle number of task batches parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t num_task_batches_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *num_task_batches = (int *)param;

	if (*num_task_batches <= 0) {
		DOCA_LOG_ERR("Number of task batches parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (*num_task_batches > UINT16_MAX) {
		DOCA_LOG_ERR("Number of task batches parameter can not be larger than %d", UINT16_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->num_task_batches = *num_task_batches;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle one sided forwarding parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t one_sided_fwd_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *one_sided_fwd = (int *)param;

	if (*one_sided_fwd < 0 || *one_sided_fwd > 2) {
		DOCA_LOG_ERR("One-sided forwarding parameter must be 0, 1 or 2");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->one_sided_fwd = *one_sided_fwd;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle max forwardings parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t max_forwardings_callback(void *param, void *config)
{
	(void)config;
	int *max_fwds = (int *)param;

	if (*max_fwds < 0) {
		DOCA_LOG_ERR("Max forwardings parameter must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	max_forwardings = *max_fwds;

	return DOCA_SUCCESS;
}

/*
 * Registers all flags used by the application for DOCA argument parser, so that when parsing
 * it can be parsed accordingly
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t register_eth_l2_fwd_params(void)
{
	doca_error_t result;
	struct doca_argp_param *mlxdevs_names, *pkts_recv_rate, *max_pkt_size, *pkt_max_process_time, *num_task_batches,
		*one_sided_fwd, *max_fwds;

	/* Create and register IB devices names param */
	result = doca_argp_param_create(&mlxdevs_names);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(mlxdevs_names, "d");
	doca_argp_param_set_long_name(mlxdevs_names, "devs-names");
	doca_argp_param_set_arguments(mlxdevs_names, "<name1,name2>");
	doca_argp_param_set_description(mlxdevs_names,
					"Set two IB devices names separated by a comma, without spaces.");
	doca_argp_param_set_callback(mlxdevs_names, mlxdevs_names_callback);
	doca_argp_param_set_type(mlxdevs_names, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(mlxdevs_names);
	result = doca_argp_register_param(mlxdevs_names);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register packets receive rate param */
	result = doca_argp_param_create(&pkts_recv_rate);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pkts_recv_rate, "r");
	doca_argp_param_set_long_name(pkts_recv_rate, "rate");
	doca_argp_param_set_arguments(pkts_recv_rate, "<rate>");
	doca_argp_param_set_description(pkts_recv_rate, "Set packets receive rate (in [MB/s]), default is 12500.");
	doca_argp_param_set_callback(pkts_recv_rate, pkts_recv_rate_callback);
	doca_argp_param_set_type(pkts_recv_rate, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(pkts_recv_rate);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register max packet size param */
	result = doca_argp_param_create(&max_pkt_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(max_pkt_size, "ps");
	doca_argp_param_set_long_name(max_pkt_size, "pkt-size");
	doca_argp_param_set_arguments(max_pkt_size, "<size>");
	doca_argp_param_set_description(max_pkt_size, "Set max packet size (in [B]), default is 1600.");
	doca_argp_param_set_callback(max_pkt_size, max_pkt_size_callback);
	doca_argp_param_set_type(max_pkt_size, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(max_pkt_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register packet max process time param */
	result = doca_argp_param_create(&pkt_max_process_time);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pkt_max_process_time, "t");
	doca_argp_param_set_long_name(pkt_max_process_time, "time");
	doca_argp_param_set_arguments(pkt_max_process_time, "<time>");
	doca_argp_param_set_description(pkt_max_process_time, "Set packet max process time (in [μs]), default is 1.");
	doca_argp_param_set_callback(pkt_max_process_time, pkt_max_process_time_callback);
	doca_argp_param_set_type(pkt_max_process_time, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(pkt_max_process_time);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of task batches param */
	result = doca_argp_param_create(&num_task_batches);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(num_task_batches, "nb");
	doca_argp_param_set_long_name(num_task_batches, "num-batches");
	doca_argp_param_set_arguments(num_task_batches, "<num>");
	doca_argp_param_set_description(num_task_batches, "Set number of task batches, default is 32.");
	doca_argp_param_set_callback(num_task_batches, num_task_batches_callback);
	doca_argp_param_set_type(num_task_batches, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_task_batches);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register one-sided forwarding param */
	result = doca_argp_param_create(&one_sided_fwd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(one_sided_fwd, "o");
	doca_argp_param_set_long_name(one_sided_fwd, "one-sided-forwarding");
	doca_argp_param_set_arguments(one_sided_fwd, "<num>");
	doca_argp_param_set_description(
		one_sided_fwd,
		"Set one-sided forwarding: 0 - two-sided forwarding, 1 - device 1 -> device 2, 2 - device 2 -> device 1. default is 0.");
	doca_argp_param_set_callback(one_sided_fwd, one_sided_fwd_callback);
	doca_argp_param_set_type(one_sided_fwd, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(one_sided_fwd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register max forwardings param */
	result = doca_argp_param_create(&max_fwds);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(max_fwds, "f");
	doca_argp_param_set_long_name(max_fwds, "max-forwardings");
	doca_argp_param_set_arguments(max_fwds, "<num>");
	doca_argp_param_set_description(
		max_fwds,
		"Set max forwarded packet batches limit after which the application run will end, default is 0, meaning no limit.");
	doca_argp_param_set_callback(max_fwds, max_forwardings_callback);
	doca_argp_param_set_type(max_fwds, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(max_fwds);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Ethernet L2 Forwarding application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct eth_l2_fwd_resources app_resources = {0};
	struct eth_l2_fwd_cfg app_cfg = {.pkts_recv_rate = ETH_L2_FWD_PKTS_RECV_RATE_DEFAULT,
					 .max_pkt_size = ETH_L2_FWD_MAX_PKT_SIZE_DEFAULT,
					 .pkt_max_process_time = ETH_L2_FWD_PKT_MAX_PROCESS_TIME_DEFAULT,
					 .num_task_batches = ETH_L2_FWD_NUM_TASK_BATCHES_DEFAULT,
					 .one_sided_fwd = 0};
	struct doca_log_backend *sdk_log;
	doca_error_t result;
	int exit_status = EXIT_SUCCESS;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Parse cmdline/json arguments */
	result = doca_argp_init(NULL, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	/* Register application parameters */
	result = register_eth_l2_fwd_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application parameters: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto destroy_argp;
	}

	/* Start Arg Parser */
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto destroy_argp;
	}

	/* Setting the max burst size separately after all the set parameters became permanent */
	app_cfg.max_burst_size = app_cfg.num_task_batches * ETH_L2_FWD_NUM_TASKS_PER_BATCH;

	/* Signal handlers for graceful termination */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Execute Ethernet L2 Forwarding Application logic */
	result = eth_l2_fwd_execute(&app_cfg, &app_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to execute Ethernet L2 Forwarding Application: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
	}

	/* Ethernet L2 Forwarding Application resources cleanup */
	result = eth_l2_fwd_cleanup(&app_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to clean up Ethernet L2 Forwarding Application resources: %s",
			     doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
	}

destroy_argp:
	/* Arg Parser cleanup */
	doca_argp_destroy();

	return exit_status;
}
