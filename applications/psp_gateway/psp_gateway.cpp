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

// system headers
#include <fcntl.h>
#include <memory>
#include <signal.h>

// DPDK
#include <rte_ethdev.h>

// gRPC
#include <google/protobuf/util/json_util.h>
#include <grpc/support/log.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

// DOCA
#include <dpdk_utils.h>
#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_dpdk.h>
#include <samples/common.h>

// application
#include <psp_gw_config.h>
#include <psp_gw_flows.h>
#include <psp_gw_svc_impl.h>
#include <psp_gw_params.h>
#include <psp_gw_pkt_rss.h>
#include <psp_gw_utils.h>

DOCA_LOG_REGISTER(PSP_GATEWAY);

volatile bool force_quit; // Set when signal is received

/**
 * @brief Signal handler function (SIGINT and SIGTERM signals)
 *
 * @signum [in]: signal number
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/*
 * @brief PSP Gateway application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	int nb_ports = 2;
	int exit_status = EXIT_SUCCESS;

	struct psp_gw_app_config app_config = {};
	app_config.dpdk_config.port_config.nb_ports = nb_ports;
	app_config.dpdk_config.port_config.nb_queues = 2;
	app_config.dpdk_config.port_config.switch_mode = true;
	app_config.dpdk_config.port_config.enable_mbuf_metadata = true;
	app_config.dpdk_config.reserve_main_thread = true;
	app_config.core_mask = "0x3";
	app_config.max_tunnels = 256;
	app_config.net_config.vc_enabled = false;
	app_config.net_config.crypt_offset = UINT32_MAX;
	app_config.net_config.default_psp_proto_ver = UINT32_MAX;
	app_config.log2_sample_rate = 0;
	app_config.ingress_sample_meta_indicator = 0x65656565; // arbitrary pkt_meta flag value
	app_config.egress_sample_meta_indicator = 0x43434343;
	app_config.return_to_vf_indicator = 0x78787878;
	app_config.show_sampled_packets = true;
	app_config.show_rss_rx_packets = false;
	app_config.show_rss_durations = false;
	app_config.outer = DOCA_FLOW_L3_TYPE_IP6;
	app_config.inner = DOCA_FLOW_L3_TYPE_IP4;
	app_config.mode = PSP_GW_MODE_TUNNEL;

	struct psp_pf_dev pf_dev = {};
	struct doca_dev_rep *vf_dev;
	uint16_t vf_port_id;
	std::string dev_probe_str;

	struct doca_log_backend *sdk_log;

	// Register a logger backend
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	// Register a logger backend for internal SDK errors and warnings
	result = doca_log_backend_create_with_file_sdk(stdout, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	result = psp_gw_argp_exec(argc, argv, &app_config);
	if (result != DOCA_SUCCESS) {
		return EXIT_FAILURE;
	}

	// init DPDK
	std::string pid_str = "pid_" + std::to_string(getpid());
	const char *eal_args[] = {"", "-a00:00.0", "-c", app_config.core_mask.c_str(), "--file-prefix", pid_str.c_str()};
	int n_eal_args = sizeof(eal_args) / sizeof(eal_args[0]);
	int rc = rte_eal_init(n_eal_args, (char **)eal_args);
	if (rc < 0) {
		DOCA_LOG_ERR("EAL initialization failed");
		return DOCA_ERROR_DRIVER;
	}

	result = psp_gw_parse_config_file(&app_config);
	if (result != DOCA_SUCCESS) {
		exit_status = EXIT_FAILURE;
		goto dpdk_destroy;
	}

	if (app_config.net_config.crypt_offset == UINT32_MAX) {
		// If not specified by argp, select a default crypt_offset
		if (app_config.inner == DOCA_FLOW_L3_TYPE_IP4)
			app_config.net_config.crypt_offset = app_config.net_config.vc_enabled ?
								     DEFAULT_CRYPT_OFFSET_VC_ENABLED_IPV4 :
								     DEFAULT_CRYPT_OFFSET_IPV4;
		else
			app_config.net_config.crypt_offset = app_config.net_config.vc_enabled ?
								     DEFAULT_CRYPT_OFFSET_VC_ENABLED_IPV6 :
								     DEFAULT_CRYPT_OFFSET_IPV6;
		DOCA_LOG_INFO("Selected crypt_offset of %d", app_config.net_config.crypt_offset);
	}

	if (app_config.net_config.default_psp_proto_ver == UINT32_MAX) {
		// If not specified by argp, select a default PSP protocol version
		app_config.net_config.default_psp_proto_ver = DEFAULT_PSP_VERSION;
		DOCA_LOG_INFO("Selected psp_ver %d", app_config.net_config.default_psp_proto_ver);
	}

	// init devices
	dev_probe_str = std::string("dv_flow_en=2,"	 // hardware steering
				    "dv_xmeta_en=4,"	 // extended flow metadata support
				    "fdb_def_rule_en=0," // disable default root flow table rule
				    "vport_match=1,"
				    "repr_matching_en=0");

	result = doca_dpdk_port_probe_with_representors(app_config.pf_dev,
							dev_probe_str.c_str(),
							&app_config.vf_dev_rep,
							1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to probe dpdk port for secured port: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto dev_close;
	}

	pf_dev.dev = app_config.pf_dev;
	pf_dev.port_id = 0;
	vf_dev = app_config.vf_dev_rep;
	vf_port_id = pf_dev.port_id + 1;

	app_config.dpdk_config.port_config.nb_ports = rte_eth_dev_count_avail();

	rte_eth_macaddr_get(pf_dev.port_id, &pf_dev.src_mac);
	pf_dev.src_mac_str = mac_to_string(pf_dev.src_mac);

	if (app_config.outer == DOCA_FLOW_L3_TYPE_IP4) {
		pf_dev.src_pip.type = DOCA_FLOW_L3_TYPE_IP4;
		result = doca_devinfo_get_ipv4_addr(doca_dev_as_devinfo(pf_dev.dev),
						    (uint8_t *)&pf_dev.src_pip.ipv4_addr,
						    DOCA_DEVINFO_IPV4_ADDR_SIZE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to find IPv4 addr for PF: %s", doca_error_get_descr(result));
			exit_status = EXIT_FAILURE;
			goto dev_close;
		}
	} else {
		pf_dev.src_pip.type = DOCA_FLOW_L3_TYPE_IP6;
		result = doca_devinfo_get_ipv6_addr(doca_dev_as_devinfo(pf_dev.dev),
						    (uint8_t *)pf_dev.src_pip.ipv6_addr,
						    DOCA_DEVINFO_IPV6_ADDR_SIZE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to find IPv6 addr for PF: %s", doca_error_get_descr(result));
			exit_status = EXIT_FAILURE;
			goto dev_close;
		}
	}
	pf_dev.src_pip_str = ip_to_string(pf_dev.src_pip);

	DOCA_LOG_INFO("Port %d: Detected PF MAC addr: %s, IP addr: %s, total ports: %d",
		      pf_dev.port_id,
		      pf_dev.src_mac_str.c_str(),
		      pf_dev.src_pip_str.c_str(),
		      app_config.dpdk_config.port_config.nb_ports);

	// Update queues and ports
	result = dpdk_queues_and_ports_init(&app_config.dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update application ports and queues: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto dev_rep_close;
	}

	/* add one more queue for the grpc requests */
	app_config.grpc_queue_id = app_config.dpdk_config.port_config.nb_queues;
	app_config.dpdk_config.port_config.nb_queues++;

	{
		PSP_GatewayFlows psp_flows(&pf_dev, vf_dev, vf_port_id, &app_config);
		std::vector<psp_gw_peer> remotes_to_connect;
		uint32_t lcore_id;

		result = psp_flows.init();
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create flow pipes");
			exit_status = EXIT_FAILURE;
			goto dpdk_cleanup;
		}

		PSP_GatewayImpl psp_svc(&app_config, &psp_flows);

		struct lcore_params lcore_params = {
			&force_quit,
			&app_config,
			&pf_dev,
			&psp_flows,
			&psp_svc,
		};

		RTE_LCORE_FOREACH_WORKER(lcore_id)
		{
			rte_eal_remote_launch(lcore_pkt_proc_func, &lcore_params, lcore_id);
		}

		std::string server_address = app_config.local_svc_addr;
		if (server_address.empty()) {
			server_address = "0.0.0.0";
		}
		if (server_address.find(":") == std::string::npos) {
			server_address += ":" + std::to_string(PSP_GatewayImpl::DEFAULT_HTTP_PORT_NUM);
		}
		grpc::ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&psp_svc);
		auto server_instance = builder.BuildAndStart();

		if (!server_instance) {
			DOCA_LOG_ERR("Failed to initialize gRPC server (%s)", server_address.c_str());
			exit_status = EXIT_FAILURE;
			goto workers_cleanup;
		} else {
			DOCA_LOG_WARN("Server listening on %s", server_address.c_str());
		}

		// If configured to create all tunnels at startup, create a list of
		// pending tunnels here. Each invocation of try_connect will
		// remove entries from the list as tunnels are created.
		// Otherwise, this list will be left empty and tunnels will be created
		// on demand via the miss path.

		if (app_config.create_tunnels_at_startup) {
			remotes_to_connect = app_config.net_config.peers;
		}

		while (!force_quit) {
			psp_svc.try_connect(remotes_to_connect);
			sleep(1);

			if (app_config.print_stats) {
				psp_flows.show_static_flow_counts();
				psp_svc.show_flow_counts();
			}
		}

		DOCA_LOG_INFO("Shutting down");

		if (server_instance) {
			server_instance->Shutdown();
			server_instance.reset();
		}

workers_cleanup:
		force_quit = true;
		RTE_LCORE_FOREACH_WORKER(lcore_id)
		{
			DOCA_LOG_INFO("Stopping L-Core %d", lcore_id);
			rte_eal_wait_lcore(lcore_id);
		}
	}

dpdk_cleanup:
	dpdk_queues_and_ports_fini(&app_config.dpdk_config);
dev_rep_close:
	doca_dev_rep_close(vf_dev);
dev_close:
	doca_dev_close(pf_dev.dev);
dpdk_destroy:
	dpdk_fini();
	doca_argp_destroy();

	return exit_status;
}
