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

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <doca_argp.h>
#include <doca_log.h>
#include <utils.h>

#include "eth_l2_fwd_core.h"

DOCA_LOG_REGISTER(ETH_L2_FWD);

struct eth_l2_fwd_resources app_resources[ETH_L2_FWD_MAX_NB_CORES];

static bool is_power_of_two(uint64_t num)
{
	return (num & (num - 1)) == 0 ? true : false;
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
 * ARGP Callback - Handle number of cores parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t number_cores_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *number_cores = (int *)param;

	if (*number_cores <= 0) {
		DOCA_LOG_ERR("Number of cores parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (*number_cores > ETH_L2_FWD_MAX_NB_CORES) {
		DOCA_LOG_ERR("Number of cores parameter can not be larger than %d", ETH_L2_FWD_MAX_NB_CORES);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->number_cores = *number_cores;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle batch size parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t batch_size_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *batch_size = (int *)param;

	if (*batch_size < 16) {
		DOCA_LOG_ERR("Batch size parameter must be greater or equal to 16");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (*batch_size > 128) {
		DOCA_LOG_ERR("Burst size parameter must be smaller or equal to 128");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!is_power_of_two(*batch_size)) {
		DOCA_LOG_ERR("Burst size parameter must be a power of 2");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->batch_size = *batch_size;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle TX depth parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t tx_depth_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *tx_depth = (int *)param;

	if (*tx_depth <= 0) {
		DOCA_LOG_ERR("TX depth parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!is_power_of_two(*tx_depth)) {
		DOCA_LOG_ERR("TX depth parameter must be a power of 2");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->tx_depth = *tx_depth;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle RX depth parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t rx_depth_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *rx_depth = (int *)param;

	if (*rx_depth <= 0) {
		DOCA_LOG_ERR("RX depth parameter must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!is_power_of_two(*rx_depth)) {
		DOCA_LOG_ERR("RX depth parameter must be a power of 2");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->rx_depth = *rx_depth;

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
 * ARGP Callback - Handle enable shared mempool mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t enable_shared_mempool_mode_callback(void *param, void *config)
{
	(void)param;
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;

	app_cfg->use_shared_mempool = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle CQE format selection
 *
 * Accepts:
 *   "regular"  - uncompressed CQEs (default; same as not passing the flag)
 *   "rx_hash"  - compressed, per-mini rx_hash (best for RSS workloads)
 *   "flow_tag" - compressed, per-mini flow_tag (best when flow_tag varies per flow
 *                AND rx_hash is stable; otherwise underperforms regular)
 *
 * @param [in]: Input parameter (string)
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t cqe_format_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	const char *value = (const char *)param;

	if (value == NULL) {
		DOCA_LOG_ERR("--cqe-format requires a value (regular|rx_hash|flow_tag)");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcmp(value, "regular") == 0) {
		app_cfg->cqe_format = DOCA_ETH_RXQ_CQE_FORMAT_REGULAR;
	} else if (strcmp(value, "rx_hash") == 0) {
		app_cfg->cqe_format = DOCA_ETH_RXQ_CQE_FORMAT_COMPRESSED_RX_HASH;
	} else if (strcmp(value, "flow_tag") == 0) {
		app_cfg->cqe_format = DOCA_ETH_RXQ_CQE_FORMAT_COMPRESSED_FLOW_TAG;
	} else {
		DOCA_LOG_ERR("Invalid --cqe-format value '%s' (expected: regular | rx_hash | flow_tag)", value);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle TXQ inline max parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t txq_inline_max_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *txq_inline_max = (int *)param;

	if (*txq_inline_max < 0 || *txq_inline_max > ETH_L2_FWD_TXQ_INLINE_MAX_LIMIT) {
		DOCA_LOG_ERR("TXQ inline max must be between 0 and 512");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->txq_inline_max = *txq_inline_max;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle cores num min inline parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t cores_num_min_inline_callback(void *param, void *config)
{
	struct eth_l2_fwd_cfg *app_cfg = (struct eth_l2_fwd_cfg *)config;
	int *cores_num_min_inline = (int *)param;

	if (*cores_num_min_inline < 0) {
		DOCA_LOG_ERR("Cores num min inline must be >= 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->cores_num_min_inline = *cores_num_min_inline;

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
	struct doca_argp_param *mlxdevs_names, *pkts_recv_rate, *max_pkt_size, *pkt_max_process_time, *one_sided_fwd,
		*number_cores, *batch_size, *tx_depth, *rx_depth, *enable_shared_mempool_mode_param, *cqe_format_param,
		*txq_inline_max_param, *cores_num_min_inline_param;

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

	/* Create and register packet number of cores param */
	result = doca_argp_param_create(&number_cores);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(number_cores, "nbc");
	doca_argp_param_set_long_name(number_cores, "number-of-cores");
	doca_argp_param_set_arguments(number_cores, "<number-of-cores>");
	doca_argp_param_set_description(number_cores, "Set number of cores used, default is 1, maximum is 32.");
	doca_argp_param_set_callback(number_cores, number_cores_callback);
	doca_argp_param_set_type(number_cores, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(number_cores);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register burst size param */
	result = doca_argp_param_create(&batch_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(batch_size, "bs");
	doca_argp_param_set_long_name(batch_size, "batch-size");
	doca_argp_param_set_arguments(batch_size, "<batch-size>");
	doca_argp_param_set_description(
		batch_size,
		"Set batch size, default is 128, batch size refers to the size of batch for DOCA_ETH, must be power of 2, maximum of 128 and minimum of 16.");
	doca_argp_param_set_callback(batch_size, batch_size_callback);
	doca_argp_param_set_type(batch_size, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(batch_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register TX depth param */
	result = doca_argp_param_create(&tx_depth);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(tx_depth, "txd");
	doca_argp_param_set_long_name(tx_depth, "tx-depth");
	doca_argp_param_set_arguments(tx_depth, "<tx-depth>");
	doca_argp_param_set_description(
		tx_depth,
		"Set TX depth, default is 4096, TX depth refers to the burst_size argument used in ETH TXQ creation, must be power of 2.");
	doca_argp_param_set_callback(tx_depth, tx_depth_callback);
	doca_argp_param_set_type(tx_depth, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(tx_depth);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register RX depth param */
	result = doca_argp_param_create(&rx_depth);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rx_depth, "rxd");
	doca_argp_param_set_long_name(rx_depth, "rx-depth");
	doca_argp_param_set_arguments(rx_depth, "<rx-depth>");
	doca_argp_param_set_description(
		rx_depth,
		"Set RX depth, default is 2048, RX depth refers to the burst_size argument used in ETH RXQ creation, must be power of 2.");
	doca_argp_param_set_callback(rx_depth, rx_depth_callback);
	doca_argp_param_set_type(rx_depth, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(rx_depth);
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

	result = doca_argp_param_create(&enable_shared_mempool_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(enable_shared_mempool_mode_param, "smm");
	doca_argp_param_set_long_name(enable_shared_mempool_mode_param, "shared-mempool-mode");
	doca_argp_param_set_description(enable_shared_mempool_mode_param,
					"Enable shared mempool mode - default: disabled");
	doca_argp_param_set_callback(enable_shared_mempool_mode_param, enable_shared_mempool_mode_callback);
	doca_argp_param_set_type(enable_shared_mempool_mode_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(enable_shared_mempool_mode_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&cqe_format_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(cqe_format_param, "cqef");
	doca_argp_param_set_long_name(cqe_format_param, "cqe-format");
	doca_argp_param_set_arguments(cqe_format_param, "<regular|rx_hash|flow_tag>");
	doca_argp_param_set_description(cqe_format_param,
					"CQE format. 'regular' (default) disables compression; "
					"'rx_hash' / 'flow_tag' enable compression with the matching per-mini field");
	doca_argp_param_set_callback(cqe_format_param, cqe_format_callback);
	doca_argp_param_set_type(cqe_format_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(cqe_format_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register TXQ inline max param */
	result = doca_argp_param_create(&txq_inline_max_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(txq_inline_max_param, "tim");
	doca_argp_param_set_long_name(txq_inline_max_param, "txq-inline-max");
	doca_argp_param_set_arguments(txq_inline_max_param, "<size>");
	doca_argp_param_set_description(
		txq_inline_max_param,
		"Set max packet size to inline into WQE (in [B]), 0 to disable. Must be <= 512. Default is 0.");
	doca_argp_param_set_callback(txq_inline_max_param, txq_inline_max_callback);
	doca_argp_param_set_type(txq_inline_max_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(txq_inline_max_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register cores num min inline param */
	result = doca_argp_param_create(&cores_num_min_inline_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(cores_num_min_inline_param, "cmi");
	doca_argp_param_set_long_name(cores_num_min_inline_param, "cores-num-min-inline");
	doca_argp_param_set_arguments(cores_num_min_inline_param, "<count>");
	doca_argp_param_set_description(
		cores_num_min_inline_param,
		"Set minimum number of cores to enable inline. 0 = always enable when txq-inline-max > 0. Default is 8.");
	doca_argp_param_set_callback(cores_num_min_inline_param, cores_num_min_inline_callback);
	doca_argp_param_set_type(cores_num_min_inline_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(cores_num_min_inline_param);
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
	struct eth_l2_fwd_cfg app_cfg = {
		.pkts_recv_rate = ETH_L2_FWD_PKTS_RECV_RATE_DEFAULT,
		.max_pkt_size = ETH_L2_FWD_MAX_PKT_SIZE_DEFAULT,
		.pkt_max_process_time = ETH_L2_FWD_PKT_MAX_PROCESS_TIME_DEFAULT,
		.one_sided_fwd = 0,
		.number_cores = ETH_L2_FWD_NUM_CORES_DEFAULT,
		.batch_size = ETH_L2_FWD_BATCH_SIZE,
		.tx_depth = ETH_L2_FWD_TX_DEPTH_DEFAULT,
		.rx_depth = ETH_L2_FWD_RX_DEPTH_DEFAULT,
		.cqe_format = DOCA_ETH_RXQ_CQE_FORMAT_REGULAR,
		.txq_inline_max = ETH_L2_FWD_TXQ_INLINE_MAX_DEFAULT,
		.cores_num_min_inline = ETH_L2_FWD_CORES_NUM_MIN_INLINE_DEFAULT,
	};
	pthread_t tids[ETH_L2_FWD_MAX_NB_CORES];
	struct doca_log_backend *sdk_log;
	doca_error_t result;
	int exit_status = EXIT_SUCCESS;
	int err;
	int i;

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

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);

	memset(app_resources, 0, ETH_L2_FWD_MAX_NB_CORES * sizeof(struct eth_l2_fwd_resources));

	result = eth_l2_fwd_general_init(&app_cfg, app_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize general resources, err: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto destroy_argp;
	}

	if (app_cfg.txq_inline_max > 0)
		DOCA_LOG_INFO("Inline data policy: requested max=%u, number_cores=%u, cores_num_min_inline=%u",
			      app_cfg.txq_inline_max,
			      app_cfg.number_cores,
			      app_cfg.cores_num_min_inline);

	for (i = 0; i < app_cfg.number_cores; i++) {
		result = eth_l2_fwd_per_thread_init(&app_cfg, &(app_resources[i]), i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize thread number %d resources, err: %s",
				     i + 1,
				     doca_error_get_descr(result));
			exit_status = EXIT_FAILURE;
			goto destroy_thread_resources;
		}
	}

	result = eth_l2_fwd_flow_init(&app_cfg, app_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA FLOW resources, err: %s", doca_error_get_descr(result));
		goto destroy_thread_resources;
	}

	reset_stats();

	for (i = 0; i < app_cfg.number_cores; i++) {
		app_resources[i].thread_num = i;
		err = pthread_create(&(tids[i]), NULL, start_forwarding, (void *)(&(app_resources[i])));
		if (err != 0) {
			DOCA_LOG_ERR("Failed to create thread number %d, errno: %s", i + 1, strerror(err));
			goto destroy_doca_flow_port;
		}
	}

	for (i = 0; i < app_cfg.number_cores; i++) {
		err = pthread_join(tids[i], NULL);
		if (err != 0) {
			DOCA_LOG_ERR("Failed to join thread number %d, errno: %s", i + 1, strerror(err));
			goto destroy_doca_flow_port;
		}
	}

	print_stats();

destroy_doca_flow_port:
	eth_l2_fwd_flow_cleanup(&app_cfg, app_resources);

destroy_thread_resources:
	for (int j = 0; j < i; j++) {
		result = eth_l2_fwd_per_thread_cleanup(&app_cfg, &(app_resources[j]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to clean up Ethernet L2 Forwarding Application resources: %s",
				     doca_error_get_descr(result));
			exit_status = EXIT_FAILURE;
		}
	}

	result = eth_l2_fwd_general_cleanup(app_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to clean up Ethernet L2 Forwarding Application resources: %s",
			     doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
	}

	DOCA_LOG_INFO("Ethernet L2 Forwarding Application cleanup finished successfully");

destroy_argp:
	/* Arg Parser cleanup */
	doca_argp_destroy();

	return exit_status;
}
