/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdint.h>
#include <signal.h>

#include <rte_cycles.h>
#include <rte_launch.h>
#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_flow_tune_server.h>
#include <doca_dpdk.h>
#include <doca_dev.h>
#include <doca_log.h>

#include <dpdk_utils.h>
#include <utils.h>

#include "simple_fwd.h"
#include "simple_fwd_port.h"
#include "simple_fwd_vnf_core.h"

DOCA_LOG_REGISTER(SIMPLE_FWD_VNF);

#define DEFAULT_NB_METERS (1 << 13) /* Maximum number of meters used */

/*
 * Signal handler
 *
 * @signum [in]: The signal received to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		simple_fwd_process_pkts_stop();
	}
}

/*
 * Close DOCA devices for all ports
 *
 * @nb_devs [in]: number of devices to close
 */
static void simple_fwd_close_doca_devs(int nb_devs)
{
	struct doca_dev *dev;
	int i;

	for (i = 0; i < nb_devs; i++)
		if (doca_dpdk_port_as_dev(i, &dev) == DOCA_SUCCESS)
			doca_dev_close(dev);
}

/*
 * Simple forward VNF application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	int exit_status = EXIT_SUCCESS;
	struct doca_log_backend *logger;
	struct doca_log_backend *sdk_log;
	struct simple_fwd_port_cfg port_cfg = {0};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 2,
		.port_config.nb_queues = 4,
		.port_config.enable_mbuf_metadata = 1,
		.reserve_main_thread = true,
	};
	struct simple_fwd_config app_cfg = {
		.dpdk_cfg = &dpdk_config,
		.rx_only = 0,
		.hw_offload = 0,
		.stats_timer = 100000,
		.age_thread = false,
		.is_hairpin = false,
	};
	struct app_vnf *vnf;
	struct simple_fwd_process_pkts_params process_pkts_params = {.cfg = &app_cfg};
	struct doca_flow_tune_server_cfg *server_cfg;

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
	doca_argp_set_dpdk_program(flow_init_dpdk);
	result = register_simple_fwd_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application params: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = init_doca_flow_devs(&app_cfg.flow_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = doca_log_backend_create_with_syslog("doca_core", &logger);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate the logger");
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update application ports and queues: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto dpdk_destroy;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* convert to number of cycles */
	app_cfg.stats_timer *= rte_get_timer_hz();

	vnf = simple_fwd_get_vnf();
	port_cfg.nb_queues = dpdk_config.port_config.nb_queues;
	port_cfg.is_hairpin = app_cfg.is_hairpin;
	port_cfg.nb_meters = DEFAULT_NB_METERS;
	port_cfg.nb_counters = (1 << 13);
	port_cfg.nb_rss = 1;
	port_cfg.nb_encap = 1;
	port_cfg.nb_decap = 3;
	port_cfg.age_thread = app_cfg.age_thread;
	if (vnf->vnf_init(&port_cfg) != 0) {
		DOCA_LOG_ERR("VNF application init error");
		exit_status = EXIT_FAILURE;
		goto exit_app;
	}

	/* Init DOCA Flow Tune Server */
	result = doca_flow_tune_server_cfg_create(&server_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create flow tune server configuration");
		return result;
	}
	result = doca_flow_tune_server_init(server_cfg);
	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_DBG("DOCA Flow Tune Server isn't supported in this runtime version");
		} else {
			DOCA_LOG_ERR("Failed to initialize the flow tune server");
			doca_flow_tune_server_cfg_destroy(server_cfg);
			return result;
		}
	}
	doca_flow_tune_server_cfg_destroy(server_cfg);

	simple_fwd_map_queue(dpdk_config.port_config.nb_queues);
	process_pkts_params.vnf = vnf;
	rte_eal_mp_remote_launch(simple_fwd_process_pkts, &process_pkts_params, CALL_MAIN);
	rte_eal_mp_wait_lcore();
exit_app:
	/* cleanup app resources */
	simple_fwd_destroy(vnf);
	doca_flow_tune_server_destroy();

	/* DPDK cleanup resources */
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_destroy:
	simple_fwd_close_doca_devs(dpdk_config.port_config.nb_ports);
	dpdk_fini();

	/* ARGP cleanup */
	doca_argp_destroy();

	return exit_status;
}
