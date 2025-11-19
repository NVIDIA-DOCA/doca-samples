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

#include <arpa/inet.h>
#include <doca_flow.h>

#include "common.h"
#include "tcp_cpu/tcp_session_table.h"

DOCA_LOG_REGISTER(GPU_PACKET_PROCESSING_FLOW);

static uint64_t default_flow_timeout_usec;

struct doca_flow_port *init_doca_flow(struct doca_dev *dev, uint16_t port_id, uint8_t rxq_num)
{
	doca_error_t result;
	struct doca_flow_port_cfg *port_cfg;
	struct doca_flow_port *df_port;
	struct doca_flow_cfg *rxq_flow_cfg;
	int ret;

	/* Initialize doca flow framework */
	ret = doca_flow_cfg_create(&rxq_flow_cfg);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(ret));
		return NULL;
	}

	ret = doca_flow_cfg_set_pipe_queues(rxq_flow_cfg, rxq_num);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(ret));
		doca_flow_cfg_destroy(rxq_flow_cfg);
		return NULL;
	}

	ret = doca_flow_cfg_set_mode_args(rxq_flow_cfg, "vnf,hws,isolated");
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(ret));
		doca_flow_cfg_destroy(rxq_flow_cfg);
		return NULL;
	}

	ret = doca_flow_cfg_set_nr_counters(rxq_flow_cfg, FLOW_NB_COUNTERS);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(ret));
		doca_flow_cfg_destroy(rxq_flow_cfg);
		return NULL;
	}

	ret = doca_flow_init(rxq_flow_cfg);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca flow with: %s", doca_error_get_descr(ret));
		doca_flow_cfg_destroy(rxq_flow_cfg);
		return NULL;
	}
	doca_flow_cfg_destroy(rxq_flow_cfg);

	/* Start doca flow port */
	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return NULL;
	}

	result = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port_id: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return NULL;
	}

	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg DOCA device: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return NULL;
	}

	result = doca_flow_port_start(port_cfg, &df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca flow port with: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return NULL;
	}
	doca_flow_port_cfg_destroy(port_cfg);

	default_flow_timeout_usec = 0;

	return df_port;
}

doca_error_t create_udp_pipe(struct rxq_udp_queues *udp_queues, struct doca_flow_port *port)
{
	doca_error_t result;
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd miss_fwd = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	uint16_t rss_queues[MAX_QUEUES];
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};
	const char *pipe_name = "GPU_RXQ_UDP_PIPE";

	if (udp_queues == NULL || port == NULL || udp_queues->numq > MAX_QUEUES)
		return DOCA_ERROR_INVALID_VALUE;

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	for (int idx = 0; idx < udp_queues->numq; idx++) {
		rss_queues[idx] = QUEUE_ID_UDP_0 + idx;
		doca_eth_rxq_apply_queue_id(udp_queues->eth_rxq_cpu[idx], rss_queues[idx]);
	}

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = udp_queues->numq;

	miss_fwd.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, &(udp_queues->rxq_pipe));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	/* Add HW offload */
	result = doca_flow_pipe_add_entry(0,
					  udp_queues->rxq_pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  NULL,
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, default_flow_timeout_usec, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

#include <rte_eal.h>

doca_error_t create_tcp_cpu_pipe(struct rxq_tcp_queues *tcp_queues, struct doca_flow_port *port)
{
	doca_error_t result;
	uint16_t rss_queues[MAX_QUEUES];
	struct doca_flow_match match = {0};
	char *eal_param[3] = {"", "-a", "00:00.0"};

	/*
	 * Setup the TCP pipe 'rxq_pipe_cpu' which forwards unrecognized flows and
	 * TCP SYN/ACK/FIN flags to the CPU - in other words, any TCP packets not
	 * recognized by the GPU TCP pipe.
	 */

	if (tcp_queues == NULL || port == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* DPDK initialization is needed to use DPDK rte_hash */
	int ret = rte_eal_init(3, eal_param);
	if (ret < 0) {
		DOCA_LOG_ERR("DPDK init failed: %d", ret);
		return DOCA_ERROR_DRIVER;
	}

	/* Init TCP session table */
	tcp_session_table = rte_hash_create(&tcp_session_ht_params);

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;

	for (int idx = 0; idx < tcp_queues->numq_cpu_rss; idx++) {
		rss_queues[idx] = QUEUE_ID_TCP_CPU_0 + idx;
		doca_eth_rxq_apply_queue_id(tcp_queues->eth_rxq_cpu_rss[idx], rss_queues[idx]);
	}

	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_RSS,
		.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
		.rss =
			{
				.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP,
				.queues_array = rss_queues,
				.nr_queues = tcp_queues->numq_cpu_rss,
			},
	};

	struct doca_flow_fwd miss_fwd = {
		.type = DOCA_FLOW_FWD_DROP,
	};

	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};

	struct doca_flow_pipe_cfg *pipe_cfg;
	const char *pipe_name = "CPU_RXQ_TCP_PIPE";

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, &tcp_queues->rxq_pipe_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_add_entry(0,
					  tcp_queues->rxq_pipe_cpu,
					  NULL,
					  0,
					  NULL,
					  NULL,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  NULL,
					  &tcp_queues->cpu_rss_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, default_flow_timeout_usec, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

doca_error_t create_tcp_gpu_pipe(struct rxq_tcp_queues *tcp_queues,
				 struct doca_flow_port *port,
				 bool connection_based_flows)
{
	uint16_t rss_queues[MAX_QUEUES];
	doca_error_t result;
	struct doca_flow_pipe_entry *dummy_entry = NULL;

	/* The GPU TCP pipe should only forward known flows to the GPU. Others will be dropped */

	if (tcp_queues == NULL || port == NULL || tcp_queues->numq > MAX_QUEUES)
		return DOCA_ERROR_INVALID_VALUE;

	struct doca_flow_match match = {
		.outer =
			{
				.l3_type = DOCA_FLOW_L3_TYPE_IP4,
				.ip4.next_proto = IPPROTO_TCP,
			},
	};

	if (connection_based_flows) {
		match.outer.ip4.src_ip = 0xffffffff;
		match.outer.ip4.dst_ip = 0xffffffff;
		match.outer.tcp.l4_port.src_port = 0xffff;
		match.outer.tcp.l4_port.dst_port = 0xffff;
		match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	};

	for (int idx = 0; idx < tcp_queues->numq; idx++) {
		rss_queues[idx] = QUEUE_ID_TCP_0 + idx;
		doca_eth_rxq_apply_queue_id(tcp_queues->eth_rxq_cpu[idx], rss_queues[idx]);
	}

	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_RSS,
		.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
		.rss =
			{
				.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP,
				.queues_array = rss_queues,
				.nr_queues = tcp_queues->numq,
			},
	};

	struct doca_flow_fwd miss_fwd = {
		.type = DOCA_FLOW_FWD_DROP,
	};

	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};

	struct doca_flow_pipe_cfg *pipe_cfg;
	const char *pipe_name = "GPU_RXQ_TCP_PIPE";

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, &tcp_queues->rxq_pipe_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	if (!connection_based_flows) {
		// For the non-connection-based configuration, create a dummy flow entry which will enable
		// any TCP packets to be forwarded.
		result = doca_flow_pipe_add_entry(0,
						  tcp_queues->rxq_pipe_gpu,
						  NULL,
						  0,
						  NULL,
						  NULL,
						  NULL,
						  0,
						  NULL,
						  &dummy_entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("RxQ pipe-entry creation failed with: %s", doca_error_get_descr(result));
			DOCA_GPUNETIO_VOLATILE(force_quit) = true;
			return result;
		}

		result = doca_flow_entries_process(port, 0, default_flow_timeout_usec, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
			return result;
		}
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

doca_error_t create_icmp_gpu_pipe(struct rxq_icmp_queues *icmp_queues, struct doca_flow_port *port)
{
	doca_error_t result;
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd miss_fwd = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	uint16_t rss_queues[MAX_QUEUES];
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};
	const char *pipe_name = "GPU_RXQ_ICMP_PIPE";

	if (icmp_queues == NULL || port == NULL || icmp_queues->numq > MAX_QUEUES)
		return DOCA_ERROR_INVALID_VALUE;

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ICMP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	for (int idx = 0; idx < icmp_queues->numq; idx++) {
		rss_queues[idx] = QUEUE_ID_ICMP_0 + idx;
		doca_eth_rxq_apply_queue_id(icmp_queues->eth_rxq_cpu[idx], rss_queues[idx]);
	}

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	fwd.rss.nr_queues = icmp_queues->numq;

	miss_fwd.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, &(icmp_queues->rxq_pipe));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	/* Add HW offload */
	result = doca_flow_pipe_add_entry(0,
					  icmp_queues->rxq_pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  NULL,
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, default_flow_timeout_usec, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

doca_error_t create_root_pipe(struct rxq_udp_queues *udp_queues,
			      struct rxq_tcp_queues *tcp_queues,
			      struct rxq_icmp_queues *icmp_queues,
			      struct doca_flow_port *port)
{
	uint32_t priority_high = 1;
	uint32_t priority_low = 3;
	doca_error_t result;
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};

	if (udp_queues == NULL || tcp_queues == NULL || port == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	struct doca_flow_pipe_cfg *pipe_cfg;
	const char *pipe_name = "ROOT_PIPE";

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &udp_queues->root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe creation failed with: %s", doca_error_get_descr(result));
		doca_flow_pipe_cfg_destroy(pipe_cfg);
		return result;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	struct doca_flow_match udp_match = {
		.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
		.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
		.outer.ip4.next_proto = IPPROTO_UDP,
	};

	struct doca_flow_fwd udp_fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = udp_queues->rxq_pipe,
	};

	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  udp_queues->root_pipe,
						  &udp_match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &udp_fwd,
						  NULL,
						  &udp_queues->root_udp_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe UDP entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	if (icmp_queues->rxq_pipe) {
		struct doca_flow_match icmp_match_gpu = {
			.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
			.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
			.outer.ip4.next_proto = IPPROTO_ICMP,
		};

		struct doca_flow_fwd icmp_fwd_gpu = {
			.type = DOCA_FLOW_FWD_PIPE,
			.next_pipe = icmp_queues->rxq_pipe,
		};

		result = doca_flow_pipe_control_add_entry(0,
							  priority_low,
							  udp_queues->root_pipe,
							  &icmp_match_gpu,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &icmp_fwd_gpu,
							  NULL,
							  &udp_queues->root_icmp_entry_gpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Root pipe ICMP entry creation failed with: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (tcp_queues->rxq_pipe_gpu) {
		struct doca_flow_match tcp_match_gpu = {
			.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
			.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
			.outer.ip4.next_proto = IPPROTO_TCP,
		};

		struct doca_flow_fwd tcp_fwd_gpu = {
			.type = DOCA_FLOW_FWD_PIPE,
			.next_pipe = tcp_queues->rxq_pipe_gpu,
		};

		result = doca_flow_pipe_control_add_entry(0,
							  priority_low,
							  udp_queues->root_pipe,
							  &tcp_match_gpu,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &tcp_fwd_gpu,
							  NULL,
							  &udp_queues->root_tcp_entry_gpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Root pipe TCP GPU entry creation failed with: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (tcp_queues->rxq_pipe_cpu) {
		struct doca_flow_match tcp_match_cpu = {
			.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
			.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
			.outer.ip4.next_proto = IPPROTO_TCP,
			.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP,
		};

		struct doca_flow_fwd tcp_fwd_cpu = {
			.type = DOCA_FLOW_FWD_PIPE,
			.next_pipe = tcp_queues->rxq_pipe_cpu,
		};

		uint8_t individual_tcp_flags[] = {
			DOCA_FLOW_MATCH_TCP_FLAG_SYN,
			DOCA_FLOW_MATCH_TCP_FLAG_RST,
			DOCA_FLOW_MATCH_TCP_FLAG_FIN,
		};

		/* Note: not strictly necessary */
		struct doca_flow_match tcp_match_cpu_mask = {
			.outer.eth.type = 0xFFFF,
			.outer.ip4.next_proto = 0xFF,
		};

		for (int i = 0; i < 3; i++) {
			tcp_match_cpu.outer.tcp.flags = individual_tcp_flags[i];
			tcp_match_cpu_mask.outer.tcp.flags = individual_tcp_flags[i];
			result = doca_flow_pipe_control_add_entry(0,
								  priority_high,
								  udp_queues->root_pipe,
								  &tcp_match_cpu,
								  &tcp_match_cpu_mask,
								  NULL,
								  NULL,
								  NULL,
								  NULL,
								  NULL,
								  &tcp_fwd_cpu,
								  NULL,
								  &udp_queues->root_tcp_entry_cpu[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Root pipe TCP CPU %d entry creation failed with: %s",
					     i,
					     doca_error_get_descr(result));
				return result;
			}
		}
	}

	result = doca_flow_entries_process(port, 0, default_flow_timeout_usec, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;
}

doca_error_t enable_tcp_gpu_offload(struct doca_flow_port *port,
				    uint16_t queue_id,
				    struct doca_flow_pipe *gpu_rss_pipe,
				    struct tcp_session_entry *session_entry)
{
	doca_error_t result;
	char src_addr[INET_ADDRSTRLEN];
	char dst_addr[INET_ADDRSTRLEN];

	struct doca_flow_match match = {
		.outer =
			{
				.l3_type = DOCA_FLOW_L3_TYPE_IP4,
				.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP,
				.tcp.flags = 0,
				.ip4.src_ip = session_entry->key.src_addr,
				.ip4.dst_ip = session_entry->key.dst_addr,
				.tcp.l4_port.src_port = session_entry->key.src_port,
				.tcp.l4_port.dst_port = session_entry->key.dst_port,
			},
	};

	result = doca_flow_pipe_add_entry(queue_id,
					  gpu_rss_pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  NULL,
					  &session_entry->flow);
	if (result != DOCA_SUCCESS) {
		inet_ntop(AF_INET, &session_entry->key.src_addr, src_addr, INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &session_entry->key.dst_addr, dst_addr, INET_ADDRSTRLEN);
		DOCA_LOG_ERR("Failed to create TCP offload session; error = %s, session = %s:%d>%s:%d",
			     doca_error_get_descr(result),
			     src_addr,
			     htons(session_entry->key.src_port),
			     dst_addr,
			     htons(session_entry->key.dst_port));
		return result;
	}

	inet_ntop(AF_INET, &session_entry->key.src_addr, src_addr, INET_ADDRSTRLEN);
	inet_ntop(AF_INET, &session_entry->key.dst_addr, dst_addr, INET_ADDRSTRLEN);
	DOCA_LOG_INFO("Created TCP offload session %s:%d>%s:%d",
		      src_addr,
		      htons(session_entry->key.src_port),
		      dst_addr,
		      htons(session_entry->key.dst_port));

	result = doca_flow_entries_process(port, queue_id, default_flow_timeout_usec, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t disable_tcp_gpu_offload(struct doca_flow_port *port,
				     uint16_t queue_id,
				     struct tcp_session_entry *session_entry)
{
	doca_error_t result;
	char src_addr[INET_ADDRSTRLEN];
	char dst_addr[INET_ADDRSTRLEN];

	/*
	 * Because those flows tend to be extremely short-lived,
	 * process the queue once more to ensure the newly created
	 * flows have been able to reach a deletable state.
	 */
	result = doca_flow_entries_process(port, queue_id, default_flow_timeout_usec, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_remove_entry(queue_id, DOCA_FLOW_NO_WAIT, session_entry->flow);

	if (result != DOCA_SUCCESS) {
		inet_ntop(AF_INET, &session_entry->key.src_addr, src_addr, INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &session_entry->key.dst_addr, dst_addr, INET_ADDRSTRLEN);
		DOCA_LOG_ERR("Failed to destroy TCP offload session; error = %s, session = %s:%d>%s:%d",
			     doca_error_get_descr(result),
			     src_addr,
			     htons(session_entry->key.src_port),
			     dst_addr,
			     htons(session_entry->key.dst_port));
	} else {
		inet_ntop(AF_INET, &session_entry->key.src_addr, src_addr, INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &session_entry->key.dst_addr, dst_addr, INET_ADDRSTRLEN);
		DOCA_LOG_INFO("Destroyed TCP offload session %s:%d>%s:%d",
			      src_addr,
			      htons(session_entry->key.src_port),
			      dst_addr,
			      htons(session_entry->key.dst_port));
	}

	return result;
}

doca_error_t destroy_flow_queue(struct doca_flow_port *port_df,
				struct rxq_icmp_queues *icmp_queues,
				struct rxq_udp_queues *udp_queues,
				struct rxq_tcp_queues *tcp_queues,
				bool http_server,
				struct txq_http_queues *http_queues)
{
	doca_flow_port_stop(port_df);
	doca_flow_destroy();

	destroy_icmp_queues(icmp_queues);
	destroy_udp_queues(udp_queues);
	destroy_tcp_queues(tcp_queues, http_server, http_queues);

	return DOCA_SUCCESS;
}
