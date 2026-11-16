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

/* CT Firewall entry point. */

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_byteorder.h>

#include <doca_argp.h>
#include <doca_dpdk.h>
#include <doca_log.h>

#include <dpdk_utils.h>
#include <flow_common.h>
#include <flow_ct_common.h>
#include <flow_switch_common.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::MAIN);

/* CT action-memory pool, in bytes. */
#define CT_FW_CT_ACTIONS_MEM_SIZE 131072
/* Fixed pipeline counters, plus 2 spare. */
#define CT_FW_FIXED_COUNTERS 15

static volatile sig_atomic_t force_quit;

/* Only a flag is safe from a signal handler. */
static void signal_handler(int signum)
{
	(void)signum;
	force_quit = 1;
}

/* Without IPv6 CT the pipeline drops IPv6. */
static doca_error_t query_ipv6_ct(struct ct_fw_ctx *app)
{
	const struct doca_flow_port_capabilities *caps = NULL;
	struct doca_dev *dev = app->config.switch_ctx.devs_ctx.devs_manager[0].doca_dev;
	doca_error_t result;

	app->has_ipv6_ct = false;
	if (dev == NULL)
		return DOCA_SUCCESS;

	result = doca_flow_port_capabilities_create(dev, &caps);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_flow_port_capabilities_get_ct_ipv6(caps, &app->has_ipv6_ct);
	doca_flow_port_capabilities_destroy(caps);
	return result;
}

static const char *nat_addr_text(const struct ct_fw_config *cfg, bool dst, char *buf, size_t len)
{
	int family;

	for (family = 0; family < CT_FW_NB_FAMILIES; family++) {
		const uint8_t *addr = dst ? cfg->nat[family].new_dst : cfg->nat[family].new_src;

		if (ct_fw_nat_addr_set(addr, family)) {
			const char *text = inet_ntop(family == CT_FW_FAMILY_IPV6 ? AF_INET6 : AF_INET, addr, buf, len);

			return text != NULL ? text : "invalid";
		}
	}
	return "none";
}

/* Set up workers before the pipeline reads them; no thread starts. */
static doca_error_t assign_workers(struct ct_fw_ctx *app)
{
	uint32_t per_worker = (app->config.nb_sessions + app->nb_workers - 1) / app->nb_workers;
	uint16_t worker_id = 0;
	uint16_t lcore;

	/* --sessions is per family. */
	if (app->has_ipv6_ct)
		per_worker *= CT_FW_NB_FAMILIES;
	if (per_worker == 0)
		per_worker = 1;
	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		struct ct_fw_worker *worker;

		if (worker_id >= app->nb_workers)
			break;
		worker = &app->workers[worker_id];
		memset(worker, 0, sizeof(*worker));
		worker->app = app;
		worker->worker_id = worker_id;
		worker->lcore = lcore;
		worker->rss_queue = worker_id;
		worker->ct_queue = app->nb_queues + worker_id;
		worker->submit = app->config.sync_mode == CT_FW_ASYNC ? ct_fw_submit_async : ct_fw_submit_sync;
		if (ct_fw_conn_table_init(&worker->table, per_worker, &app->config) != DOCA_SUCCESS)
			return DOCA_ERROR_NO_MEMORY;
		worker->table.worker_id = worker_id;
		worker_id++;
	}
	if (worker_id < app->nb_workers) {
		DOCA_LOG_ERR("Only %u lcores available for %u workers", worker_id, app->nb_workers);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/* On failure the caller stops the started workers. */
static doca_error_t launch_workers(struct ct_fw_ctx *app)
{
	uint16_t worker_id;

	for (worker_id = 0; worker_id < app->nb_workers; worker_id++) {
		struct ct_fw_worker *worker = &app->workers[worker_id];

		if (rte_eal_remote_launch(ct_fw_worker_loop, worker, worker->lcore) != 0) {
			DOCA_LOG_ERR("Failed to launch worker %u on lcore %u", worker_id, worker->lcore);
			return DOCA_ERROR_OPERATING_SYSTEM;
		}
	}
	return DOCA_SUCCESS;
}

static doca_error_t run(struct ct_fw_ctx *app, struct application_dpdk_config *dpdk_config)
{
	struct flow_switch_ctx *ctx = &app->config.switch_ctx;
	struct flow_resources resource;
	uint32_t nr_shared[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_meta zone_mask_o, zone_mask_r;
	struct doca_flow_ct_meta modify_o, modify_r;
	uint32_t actions_mem_size[CT_FW_MAX_PORTS];
	struct ct_init_cfg ct_init = {.ct_queues = app->nb_workers, .ct_ctrl_queues = 1};
	struct ct_port_cfg ct_port_cfg;
	int nb_ports = CT_FW_MAX_PORTS;
	int i;
	doca_error_t result;

	/* Zone in pkt_meta [31:7], freeing [6:4]. */
	memset(&zone_mask_o, 0, sizeof(zone_mask_o));
	memset(&zone_mask_r, 0, sizeof(zone_mask_r));
	memset(&modify_o, 0, sizeof(modify_o));
	memset(&modify_r, 0, sizeof(modify_r));
	zone_mask_o.pkt_meta = rte_cpu_to_be_32(CT_FW_META_ZONE_MASK);
	zone_mask_r.pkt_meta = rte_cpu_to_be_32(CT_FW_META_ZONE_MASK);

	memset(&ct_port_cfg, 0, sizeof(ct_port_cfg));
	/* Only NAT entries carry actions. */
	{
		enum ct_fw_family nat_family;

		if (ct_fw_nat_family(&app->config, &nat_family))
			ct_port_cfg.ct_actions_mem_size = CT_FW_CT_ACTIONS_MEM_SIZE;
	}
	ct_port_cfg.zone_match_mask[0] = &zone_mask_o;
	ct_port_cfg.zone_match_mask[1] = &zone_mask_r;
	ct_port_cfg.modify_mask[0] = &modify_o;
	ct_port_cfg.modify_mask[1] = &modify_r;
	/* The callback makes updates asynchronous; --sync leaves it unset. */
	if (app->config.sync_mode == CT_FW_ASYNC)
		ct_port_cfg.tcp_window_validation_comp_cb = ct_fw_async_wv_comp_cb;

	memset(&resource, 0, sizeof(resource));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	/* Plus two per port. */
	resource.nr_counters = CT_FW_FIXED_COUNTERS + 2 * CT_FW_MAX_PORTS;
	/* Two per session per family; IPv6 support is not known yet. */
	resource.nr_ct_counters = (uint32_t)((uint64_t)app->config.nb_sessions * 4);
	resource.nr_rss = 1;
	if (app->config.enable_window_validation)
		resource.nr_tcp_window_validation = app->config.pool_size;

	ct_fw_cb_init(app);
	result = init_doca_flow_cb(dpdk_config->port_config.nb_queues,
				   "switch,expert",
				   &resource,
				   nr_shared,
				   ct_fw_entry_cb,
				   NULL,
				   NULL,
				   &ct_init);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < nb_ports; i++)
		actions_mem_size[i] = ACTIONS_MEM_SIZE(FLOW_COMMON_PIPE_RULES);

	result = init_doca_flow_switch_ports_with_ct(ctx->devs_ctx.devs_manager,
						     ctx->devs_ctx.nb_devs,
						     app->ports,
						     nb_ports,
						     actions_mem_size,
						     &resource,
						     &ct_port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		DOCA_LOG_ERR(
			"Each connection uses two hardware flow counters per address family; if the device cannot provide them, lower --sessions");
		destroy_doca_flow();
		return result;
	}
	app->nb_ports = nb_ports;
	app->nb_queues = dpdk_config->port_config.nb_queues;

	result = query_ipv6_ct(app);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("IPv6 CT capability query failed: %s", doca_error_get_descr(result));
	if (!app->has_ipv6_ct) {
		enum ct_fw_family nat_family;

		if (ct_fw_nat_family(&app->config, &nat_family) && nat_family == CT_FW_FAMILY_IPV6) {
			DOCA_LOG_ERR(
				"IPv6 --dnat/--snat needs IPv6 connection tracking, which this device does not support");
			result = DOCA_ERROR_NOT_SUPPORTED;
			goto cleanup;
		}
		DOCA_LOG_WARN("IPv6 connection tracking is not supported on this device; IPv6 traffic is dropped");
	}

	result = assign_workers(app);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = ct_fw_pipeline_build(app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to build the pipeline: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = launch_workers(app);
	if (result != DOCA_SUCCESS) {
		/* Join the workers already running before tearing down. */
		*app->force_quit = 1;
		rte_eal_mp_wait_lcore();
		goto cleanup;
	}

	{
		char dnat[INET6_ADDRSTRLEN], snat[INET6_ADDRSTRLEN];

		DOCA_LOG_INFO(
			"CT Firewall ready: mode=%s window_validation=%s liberal=%s max_ack_window=%u offload_after=%u dnat=%s snat=%s workers=%u aging_timeout=%us ipv6_ct=%s",
			app->config.sync_mode == CT_FW_SYNC ? "sync" : "async",
			app->config.enable_window_validation ? "on" : "off",
			app->config.liberal_mode ? "on" : "off",
			app->config.max_ack_window,
			app->config.offload_after,
			nat_addr_text(&app->config, true, dnat, sizeof(dnat)),
			nat_addr_text(&app->config, false, snat, sizeof(snat)),
			app->nb_workers,
			app->config.aging_timeout_s,
			app->has_ipv6_ct ? "on" : "off");
	}
	DOCA_LOG_INFO("CT Firewall running with %u worker(s); Ctrl-C to stop", app->nb_workers);

	/* One-second slices keep Ctrl-C responsive. */
	if (app->config.stats_interval != 0) {
		struct timespec start, now;

		clock_gettime(CLOCK_MONOTONIC, &start);
		while (!*app->force_quit) {
			uint32_t left = app->config.stats_interval;

			while (left-- != 0 && !*app->force_quit)
				sleep(1);
			if (*app->force_quit)
				break;
			clock_gettime(CLOCK_MONOTONIC, &now);
			ct_fw_stats_print_periodic(app, (uint64_t)(now.tv_sec - start.tv_sec));
		}
	}

	rte_eal_mp_wait_lcore();
	if (__atomic_load_n(&app->failed, __ATOMIC_RELAXED))
		result = DOCA_ERROR_TIME_OUT;

	ct_fw_stats_dump(app);

cleanup:
	ct_fw_pipeline_destroy(app);
	stop_doca_flow_ports(app->nb_ports, app->ports);
	destroy_doca_flow();
	/* Freed only after the ports stop. */
	for (i = 0; i < app->nb_workers; i++) {
		ct_fw_conn_table_fini(&app->workers[i].table);
		ct_fw_frag_flush(&app->workers[i].frags);
	}
	return result;
}

int main(int argc, char **argv)
{
	static struct ct_fw_ctx app;
	struct doca_log_backend *sdk_log;
	struct application_dpdk_config dpdk_config;
	struct sigaction sa;
	int exit_status = EXIT_FAILURE;
	doca_error_t result;

	memset(&app, 0, sizeof(app));
	app.force_quit = &force_quit;

	/* Defaults; zero for the rest. */
	app.config.sync_mode = CT_FW_ASYNC;
	app.config.enable_window_validation = true;
	app.config.nb_ct_queues = CT_FW_DEFAULT_CT_QUEUES;
	app.config.nb_sessions = CT_FW_DEFAULT_SESSIONS;
	app.config.pool_size = CT_FW_DEFAULT_POOL;
	app.config.aging_timeout_s = CT_FW_DEFAULT_AGING_TIMEOUT_S;
	app.config.stats_interval = CT_FW_DEFAULT_STATS_INTERVAL;

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_argp_init(NULL, &app.config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}
	doca_argp_set_dpdk_program(flow_init_dpdk);
	app.config.switch_ctx.devs_ctx.default_dev_args = FLOW_SWITCH_DEV_ARGS;
	app.config.switch_ctx.devs_ctx.port_cap = flow_ct_capable;
	app.config.switch_ctx.is_expert = true; /* the pipeline needs switch expert mode */

	result = ct_fw_register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	app.nb_workers = app.config.nb_ct_queues;

	result = ct_fw_config_validate(&app.config);
	if (result != DOCA_SUCCESS)
		goto argp_cleanup;

	/* sleep() still wakes on the signal. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
		DOCA_LOG_ERR("Failed to install the signal handlers");
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&app.config.switch_ctx.devs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Both uplinks are required. */
	if (app.config.switch_ctx.devs_ctx.devs_manager[0].nb_reps == 0) {
		DOCA_LOG_ERR("No representor given: the second uplink (-r/--rep <PCI>) is required");
		goto devs_cleanup;
	}

	memset(&dpdk_config, 0, sizeof(dpdk_config));
	dpdk_config.port_config.nb_ports = CT_FW_MAX_PORTS;
	/* One queue for the main thread. */
	dpdk_config.port_config.nb_queues = app.nb_workers + 1;
	dpdk_config.port_config.switch_mode = 1;
	dpdk_config.port_config.enable_mbuf_metadata = 1;
	dpdk_config.port_config.rss_support = 1;
	dpdk_config.port_config.mbuf_size = CT_FW_MBUF_SIZE;
	dpdk_config.reserve_main_thread = true;

	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DPDK ports and queues: %s", doca_error_get_descr(result));
		goto devs_cleanup;
	}

	result = run(&app, &dpdk_config);
	if (result == DOCA_SUCCESS)
		exit_status = EXIT_SUCCESS;

	dpdk_queues_and_ports_fini(&dpdk_config);
devs_cleanup:
	destroy_doca_flow_devs(&app.config.switch_ctx.devs_ctx);
argp_cleanup:
	doca_argp_destroy();
	return exit_status;
}
