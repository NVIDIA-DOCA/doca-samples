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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include "eth_common.h"

DOCA_LOG_REGISTER(ETH_RXQ_BATCH_SHARED_MEMPOOL_RECEIVE::MAIN);

#define DEFAULT_QUEUES_NUM 1 /* Default number of ETH RXQs sharing the memory pool */

/* Configuration struct */
struct eth_rxq_cfg {
	char ib_dev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* DOCA IB device name */
	bool timestamp_enable;				/* timestamp enable */
	uint16_t headroom_size;				/* headroom size */
	uint16_t tailroom_size;				/* tailroom size */
	uint16_t num_queues;				/* number of ETH RXQs sharing the memory pool */
};

/* Sample's Logic */
doca_error_t eth_rxq_batch_shared_mempool_receive(const char *ib_dev_name,
						  bool timestamp_enable,
						  uint16_t headroom_size,
						  uint16_t tailroom_size,
						  uint16_t num_queues);

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_address_callback(void *param, void *config)
{
	struct eth_rxq_cfg *eth_rxq_cfg = (struct eth_rxq_cfg *)config;

	return extract_ibdev_name((char *)param, eth_rxq_cfg->ib_dev_name);
}

/*
 * ARGP Callback - timestamp enable parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t timestamp_callback(void *param, void *config)
{
	(void)param;
	struct eth_rxq_cfg *eth_rxq_cfg = (struct eth_rxq_cfg *)config;

	eth_rxq_cfg->timestamp_enable = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - headroom size parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t headroom_callback(void *param, void *config)
{
	uint16_t headroom_size = strtoul((char *)param, NULL, 10);
	struct eth_rxq_cfg *eth_rxq_cfg = (struct eth_rxq_cfg *)config;

	eth_rxq_cfg->headroom_size = headroom_size;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - tailroom size parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t tailroom_callback(void *param, void *config)
{
	uint16_t tailroom_size = strtoul((char *)param, NULL, 10);
	struct eth_rxq_cfg *eth_rxq_cfg = (struct eth_rxq_cfg *)config;

	eth_rxq_cfg->tailroom_size = tailroom_size;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - number of queues parameter
 *
 * @param [in]: Input parameter
 * @config [out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_queues_callback(void *param, void *config)
{
	uint16_t num_queues = strtoul((char *)param, NULL, 10);
	struct eth_rxq_cfg *eth_rxq_cfg = (struct eth_rxq_cfg *)config;

	if (num_queues == 0 || num_queues > MAX_QUEUES_NUM) {
		DOCA_LOG_ERR("Invalid number of queues %u, must be in the range [1, %d]", num_queues, MAX_QUEUES_NUM);
		return DOCA_ERROR_INVALID_VALUE;
	}

	eth_rxq_cfg->num_queues = num_queues;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_eth_rxq_params(void)
{
	doca_error_t result;
	struct doca_argp_param *dev_ib_name_param;
	struct doca_argp_param *enable_timestamp_param;
	struct doca_argp_param *headroom_param;
	struct doca_argp_param *tailroom_param;
	struct doca_argp_param *num_queues_param;

	result = doca_argp_param_create(&dev_ib_name_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(dev_ib_name_param, "d");
	doca_argp_param_set_long_name(dev_ib_name_param, "device");
	doca_argp_param_set_description(dev_ib_name_param, "IB device name - default: mlx5_0");
	doca_argp_param_set_callback(dev_ib_name_param, device_address_callback);
	doca_argp_param_set_type(dev_ib_name_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dev_ib_name_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&enable_timestamp_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(enable_timestamp_param, "ts");
	doca_argp_param_set_long_name(enable_timestamp_param, "timestamp");
	doca_argp_param_set_description(enable_timestamp_param, "Enable timestamp retrieval - default: disabled");
	doca_argp_param_set_callback(enable_timestamp_param, timestamp_callback);
	doca_argp_param_set_type(enable_timestamp_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(enable_timestamp_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&headroom_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(headroom_param, "hr");
	doca_argp_param_set_long_name(headroom_param, "headroom");
	doca_argp_param_set_description(headroom_param, "Packet headroom size - default: 0");
	doca_argp_param_set_callback(headroom_param, headroom_callback);
	doca_argp_param_set_type(headroom_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(headroom_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&tailroom_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(tailroom_param, "tr");
	doca_argp_param_set_long_name(tailroom_param, "tailroom");
	doca_argp_param_set_description(tailroom_param, "Packet tailroom size - default: 0");
	doca_argp_param_set_callback(tailroom_param, tailroom_callback);
	doca_argp_param_set_type(tailroom_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(tailroom_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&num_queues_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(num_queues_param, "nq");
	doca_argp_param_set_long_name(num_queues_param, "num-queues");
	doca_argp_param_set_description(num_queues_param, "Number of ETH RXQs sharing the memory pool - default: 1");
	doca_argp_param_set_callback(num_queues_param, num_queues_callback);
	doca_argp_param_set_type(num_queues_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(num_queues_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
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
	struct eth_rxq_cfg eth_rxq_cfg;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

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

	strcpy(eth_rxq_cfg.ib_dev_name, "mlx5_0");
	eth_rxq_cfg.timestamp_enable = false;
	eth_rxq_cfg.headroom_size = 0;
	eth_rxq_cfg.tailroom_size = 0;
	eth_rxq_cfg.num_queues = DEFAULT_QUEUES_NUM;

	result = doca_argp_init(NULL, &eth_rxq_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_eth_rxq_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = eth_rxq_batch_shared_mempool_receive(eth_rxq_cfg.ib_dev_name,
						      eth_rxq_cfg.timestamp_enable,
						      eth_rxq_cfg.headroom_size,
						      eth_rxq_cfg.tailroom_size,
						      eth_rxq_cfg.num_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("eth_rxq_batch_shared_mempool_receive() encountered an error: %s",
			     doca_error_get_descr(result));
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
