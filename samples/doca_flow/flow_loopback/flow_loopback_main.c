/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_LOOPBACK::MAIN);

#define NB_PORTS 2
#define MAC_ADDR_LEN 6

struct flow_loopback_ctx {
	struct flow_dev_ctx flow_dev_ctx;	       /* Basic flow device context*/
	uint8_t mac_addresses[NB_PORTS][MAC_ADDR_LEN]; /* MAC addresses for the ports */
};

/* Sample's Logic */
doca_error_t flow_loopback(int nb_queues, uint8_t mac_addresses[2][6]);

/*
 * ARGP Callback - Handle MAC addresses parameter
 *
 * @param [in]: Input parameter
 * @opaque [in/out]: MAC address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mac_addresses_callback(void *param, void *opaque)
{
	char *mac_addresses_param = (char *)param;
	struct flow_loopback_ctx *ctx = (struct flow_loopback_ctx *)opaque;
	char mac1[18];
	char mac2[18];

	/* Split the input into two MAC addresses */
	if (sscanf(mac_addresses_param, "%17s %17s", mac1, mac2) != 2) {
		DOCA_LOG_ERR("Invalid input format");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Parse the first MAC address */
	if (sscanf(mac1,
		   "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   &ctx->mac_addresses[0][0],
		   &ctx->mac_addresses[0][1],
		   &ctx->mac_addresses[0][2],
		   &ctx->mac_addresses[0][3],
		   &ctx->mac_addresses[0][4],
		   &ctx->mac_addresses[0][5]) != MAC_ADDR_LEN) {
		DOCA_LOG_ERR("Invalid MAC address format: %s", mac1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*  Parse the second MAC address */
	if (sscanf(mac2,
		   "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   &ctx->mac_addresses[1][0],
		   &ctx->mac_addresses[1][1],
		   &ctx->mac_addresses[1][2],
		   &ctx->mac_addresses[1][3],
		   &ctx->mac_addresses[1][4],
		   &ctx->mac_addresses[1][5]) != MAC_ADDR_LEN) {
		DOCA_LOG_ERR("Invalid MAC address format: %s", mac2);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Conversion function from a user context to the flow_dev_ctx struct
 *
 * @param [in]: User context
 * @return: Pointer to the flow_dev_ctx struct
 */
static struct flow_dev_ctx *flow_dev_ctx_from_user_ctx(void *user_ctx)
{
	return &((struct flow_loopback_ctx *)user_ctx)->flow_dev_ctx;
}

/*
 * Register the command line parameter for the sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_flow_loopback_params(void)
{
	doca_error_t result;
	struct doca_argp_param *mac_addresses_param;

	result = register_flow_device_params(flow_dev_ctx_from_user_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&mac_addresses_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(mac_addresses_param, "m");
	doca_argp_param_set_long_name(mac_addresses_param, "mac-addresses");
	doca_argp_param_set_description(mac_addresses_param, "The MAC addresses of the ports, used for encapsulation");
	doca_argp_param_set_callback(mac_addresses_param, mac_addresses_callback);
	doca_argp_param_set_type(mac_addresses_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(mac_addresses_param);
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
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct flow_loopback_ctx flow_loopback_ctx = {};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = NB_PORTS,
		.port_config.nb_queues = 1,
		.port_config.enable_mbuf_metadata = 1,
		.port_config.lpbk_support = 1,
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

	result = doca_argp_init(NULL, &flow_loopback_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_flow_loopback_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register samples params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	doca_argp_set_dpdk_program(flow_init_dpdk);
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&flow_loopback_ctx.flow_dev_ctx);
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
	result = flow_loopback(dpdk_config.port_config.nb_queues, flow_loopback_ctx.mac_addresses);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_loopback() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini_with_devs(NB_PORTS);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
