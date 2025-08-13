/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

/*
 * This application is designed to verify the switch RSS feature using shared, immediate,
 * and internal RSS configurations.
 * Shared and immediate RSS will be applied to both basic and control pipes, with combinations
 * of changeable and constant settings in switch N2H, H2N, and bi-directional modes.
 * Internal RSS will be tested by sending wire traffic from ingress to egress.
 * Three ports are required, covering MPESW wire0, wire1, and VF.
 *
 * The test traffic is as follows:
 *
 * switch N2H:
 * 1.2.3.4, 8.8.8.8 -> basic constant immediate rss
 * 1.2.3.5, 8.8.8.8 -> basic dynamic immediate rss
 * 1.2.3.6, 8.8.8.8 -> basic changeable shared rss
 * 1.2.3.7, 8.8.8.8 -> control immediate rss
 * 1.2.3.8, 8.8.8.9 -> control shared rss
 *
 * switch H2N/bi-direction:
 * 1.2.3.9, 8.8.8.8 -> basic constant immediate rss
 * 1.2.3.10, 8.8.8.8 -> basic dynamic immediate rss
 * 1.2.3.11, 8.8.8.8 -> basic changeable shared rss
 * 1.2.3.12, 8.8.8.8 -> control immediate rss
 * 1.2.3.13, 8.8.8.9 -> control shared rss
 *
 * Wire to egress pipe vport:
 * 1.2.3.14, 8.8.8.8 -> vport 0
 * 1.2.3.14, 8.8.8.9 -> vport 1
 * 1.2.3.14, 8.8.8.10 -> vport 2
 */

#include <stdlib.h>

#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_ctx.h>

#include <flow_common.h>
#include <flow_switch_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_SWITCH_RSS::MAIN);

#define SWITCH_RSS_PORTS 3

/* Sample's Logic */
doca_error_t flow_switch_rss(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx);

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
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = SWITCH_RSS_PORTS,
		.port_config.nb_queues = 10,
		.port_config.switch_mode = 1,
	};
	struct flow_switch_ctx ctx = {0};
	uint16_t nr_ports;

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

	result = doca_argp_init(NULL, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_doca_flow_switch_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow param: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&ctx.devs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow switch common: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	nr_ports = rte_eth_dev_count_avail();
	if (nr_ports < SWITCH_RSS_PORTS) {
		DOCA_LOG_ERR("Failed to init - lack of ports, probed:%d, needed:%d", nr_ports, SWITCH_RSS_PORTS);
		goto sample_exit;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues");
		goto dpdk_cleanup;
	}

	/* run sample */
	result = flow_switch_rss(dpdk_config.port_config.nb_queues, SWITCH_RSS_PORTS, &ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_switch_rss() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini();
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	destroy_doca_flow_devs(&ctx.devs_ctx);
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
