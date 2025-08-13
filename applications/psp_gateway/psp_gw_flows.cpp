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

#include <string>
#include <vector>

#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <netinet/icmp6.h>

#include <doca_bitfield.h>
#include <doca_flow.h>
#include <doca_flow_crypto.h>
#include <doca_flow_tune_server.h>
#include <doca_log.h>

#include "psp_gw_config.h"
#include "psp_gw_flows.h"
#include "psp_gw_utils.h"

#define IF_SUCCESS(result, expr) \
	if (result == DOCA_SUCCESS) { \
		result = expr; \
		if (likely(result == DOCA_SUCCESS)) { \
			DOCA_LOG_DBG("Success: %s", #expr); \
		} else { \
			DOCA_LOG_ERR("Error: %s: %s", #expr, doca_error_get_descr(result)); \
		} \
	} else { /* skip this expr */ \
	}

#define NEXT_HEADER_IPV4 0x4
#define NEXT_HEADER_IPV6 0x29

DOCA_LOG_REGISTER(PSP_GATEWAY);

static const uint32_t DEFAULT_TIMEOUT_US = 10000; /* default timeout for processing entries */
static const uint32_t PSP_ICV_SIZE = 16;
static const uint32_t MAX_ACTIONS_MEM_SIZE = 8388608 * 64;

/**
 * @brief packet header structure to simplify populating the encap_data array for tunnel encap ipv6 data
 */
struct eth_ipv6_psp_tunnel_hdr {
	// encapped Ethernet header contents.
	rte_ether_hdr eth;

	// encapped IP header contents (extension header not supported)
	rte_ipv6_hdr ip;

	rte_udp_hdr udp;

	// encapped PSP header contents.
	rte_psp_base_hdr psp;
	rte_be64_t psp_virt_cookie;

} __rte_packed __rte_aligned(2);

/**
 * @brief packet header structure to simplify populating the encap_data array for tunnel encap ipv4 data
 */
struct eth_ipv4_psp_tunnel_hdr {
	// encapped Ethernet header contents.
	rte_ether_hdr eth;

	// encapped IP header contents (extension header not supported)
	rte_ipv4_hdr ip;

	rte_udp_hdr udp;

	// encapped PSP header contents.
	rte_psp_base_hdr psp;
	rte_be64_t psp_virt_cookie;

} __rte_packed __rte_aligned(2);

/**
 * @brief packet header structure to simplify populating the encap_data array for transport encap data
 */
struct udp_psp_transport_hdr {
	// encaped udp header
	rte_udp_hdr udp;

	// encapped PSP header contents.
	rte_psp_base_hdr psp;
	rte_be64_t psp_virt_cookie;

} __rte_packed __rte_aligned(2);

const uint8_t PSP_SAMPLE_ENABLE = 1 << 7;

PSP_GatewayFlows::PSP_GatewayFlows(psp_pf_dev *pf,
				   doca_dev_rep *vf_dev,
				   uint16_t vf_port_id,
				   psp_gw_app_config *app_config)
	: app_config(app_config),
	  pf_dev(pf),
	  vf_port_id(vf_port_id),
	  vf_dev_rep(vf_dev),
	  sampling_enabled(app_config->log2_sample_rate > 0)
{
	monitor_count.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	for (uint16_t i = 0; i < app_config->dpdk_config.port_config.nb_queues - 1; i++) {
		rss_queues.push_back(i);
	}
	fwd_changeable_rss.type = DOCA_FLOW_FWD_RSS;
	fwd_changeable_rss.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd_changeable_rss.rss.nr_queues = -1;

	fwd_ipv4_rss.type = DOCA_FLOW_FWD_RSS;
	fwd_ipv4_rss.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd_ipv4_rss.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	fwd_ipv4_rss.rss.queues_array = rss_queues.data();
	fwd_ipv4_rss.rss.nr_queues = (int)rss_queues.size();

	fwd_ipv6_rss.type = DOCA_FLOW_FWD_RSS;
	fwd_ipv6_rss.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd_ipv6_rss.rss.outer_flags = DOCA_FLOW_RSS_IPV6;
	fwd_ipv6_rss.rss.queues_array = rss_queues.data();
	fwd_ipv6_rss.rss.nr_queues = (int)rss_queues.size();
}

PSP_GatewayFlows::~PSP_GatewayFlows()
{
	if (vf_port)
		doca_flow_port_stop(vf_port);

	if (pf_dev->port_obj)
		doca_flow_port_stop(pf_dev->port_obj);

	doca_flow_tune_server_destroy();
	doca_flow_destroy();
}

doca_error_t PSP_GatewayFlows::init(void)
{
	doca_error_t result = DOCA_SUCCESS;

	IF_SUCCESS(result, init_doca_flow(app_config));
	IF_SUCCESS(result, start_port(pf_dev->port_id, pf_dev->dev, nullptr, &pf_dev->port_obj));
	IF_SUCCESS(result, start_port(vf_port_id, nullptr, vf_dev_rep, &vf_port));
	IF_SUCCESS(result, bind_shared_resources());
	init_status(app_config);
	IF_SUCCESS(result, rss_pipe_create());
	if (sampling_enabled) {
		IF_SUCCESS(result, fwd_to_wire_pipe_create());
		IF_SUCCESS(result, fwd_to_rss_pipe_create());
	}
	IF_SUCCESS(result, syndrome_stats_pipe_create());
	IF_SUCCESS(result, ingress_acl_pipe_create(true));
	IF_SUCCESS(result, ingress_acl_pipe_create(false));
	if (!app_config->disable_ingress_acl) {
		IF_SUCCESS(result, create_ingress_src_ip6_pipe());
	}
	IF_SUCCESS(result, ingress_inner_classifier_pipe_create());
	if (sampling_enabled) {
		IF_SUCCESS(result, configure_mirrors());
	}
	IF_SUCCESS(result, create_pipes());

	return result;
}

doca_error_t PSP_GatewayFlows::configure_mirrors(void)
{
	assert(rss_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_mirror_target mirr_tgt = {};
	mirr_tgt.fwd.type = DOCA_FLOW_FWD_PIPE;
	mirr_tgt.fwd.next_pipe = ingress_inner_ip_classifier_pipe;

	struct doca_flow_shared_resource_cfg res_cfg = {};
	res_cfg.mirror_cfg.nr_targets = 1;
	res_cfg.mirror_cfg.target = &mirr_tgt;
	res_cfg.mirror_cfg.fwd.type = DOCA_FLOW_FWD_PIPE;
	res_cfg.mirror_cfg.fwd.next_pipe = rss_pipe;

	IF_SUCCESS(
		result,
		doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_res_id_ingress, &res_cfg));

	IF_SUCCESS(result,
		   doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
						   &mirror_res_id_ingress,
						   1,
						   pf_dev->port_obj));

	doca_flow_mirror_target mirr_tgt_rss_pipe = {};
	mirr_tgt_rss_pipe.fwd.type = DOCA_FLOW_FWD_PIPE;
	mirr_tgt_rss_pipe.fwd.next_pipe = fwd_to_rss_pipe;

	res_cfg.mirror_cfg.target = &mirr_tgt_rss_pipe;
	res_cfg.mirror_cfg.fwd.type = DOCA_FLOW_FWD_NONE; /* defined in pipe creation */

	IF_SUCCESS(result,
		   doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_res_id_rss, &res_cfg));
	IF_SUCCESS(result,
		   doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
						   &mirror_res_id_rss,
						   1,
						   pf_dev->port_obj));

	if (app_config->maintain_order) {
		doca_flow_mirror_target mirr_tgt_drop = {};
		mirr_tgt_drop.fwd.type = DOCA_FLOW_FWD_DROP;

		res_cfg.mirror_cfg.target = &mirr_tgt_drop;

		IF_SUCCESS(result,
			   doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
							     mirror_res_id_drop,
							     &res_cfg));
		IF_SUCCESS(result,
			   doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
							   &mirror_res_id_drop,
							   1,
							   pf_dev->port_obj));
	}

	return result;
}

doca_error_t PSP_GatewayFlows::start_port(uint16_t port_id,
					  doca_dev *port_dev,
					  doca_dev_rep *port_rep,
					  doca_flow_port **port)
{
	doca_flow_port_cfg *port_cfg;
	doca_error_t result = DOCA_SUCCESS;

	IF_SUCCESS(result, doca_flow_port_cfg_create(&port_cfg));

	IF_SUCCESS(result, doca_flow_port_cfg_set_port_id(port_cfg, port_id));
	IF_SUCCESS(result, doca_flow_port_cfg_set_dev(port_cfg, port_dev));
	IF_SUCCESS(result, doca_flow_port_cfg_set_dev_rep(port_cfg, port_rep));
	IF_SUCCESS(result, doca_flow_port_cfg_set_actions_mem_size(port_cfg, rte_align32pow2(MAX_ACTIONS_MEM_SIZE)));
	IF_SUCCESS(result, doca_flow_port_start(port_cfg, port));

	if (result == DOCA_SUCCESS) {
		rte_ether_addr port_mac_addr;
		rte_eth_macaddr_get(port_id, &port_mac_addr);
		DOCA_LOG_INFO("Started port_id %d, mac-addr: %s", port_id, mac_to_string(port_mac_addr).c_str());
	}

	if (port_cfg) {
		doca_flow_port_cfg_destroy(port_cfg);
	}
	return result;
}

static doca_error_t configure_tune_server(struct doca_flow_cfg *flow_cfg, enum doca_flow_tune_profile profile)
{
	doca_error_t result = DOCA_SUCCESS;
	struct doca_flow_tune_cfg *tune_cfg;

	result = doca_flow_tune_cfg_create(&tune_cfg);
	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_INFO("DOCA Flow Tune Server isn't supported in this runtime version");
			DOCA_LOG_INFO("Program will continue execution without activating the DOCA Flow Tune Server");
			return DOCA_SUCCESS;
		}

		DOCA_LOG_ERR("Failed to create doca_flow_tune_cfg: %s", doca_error_get_descr(result));
		goto out;
	}

	result = doca_flow_tune_cfg_set_profile(tune_cfg, profile);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_tune_cfg profile: %s", doca_error_get_descr(result));
		goto cfg_destroy;
	}

	result = doca_flow_cfg_set_tune_cfg(flow_cfg, tune_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg tune_cfg: %s", doca_error_get_descr(result));
		goto cfg_destroy;
	}

cfg_destroy:
	doca_flow_tune_cfg_destroy(tune_cfg);
out:
	return result;
}

doca_error_t PSP_GatewayFlows::init_doca_flow(const psp_gw_app_config *app_cfg)
{
	struct doca_flow_tune_server_cfg *server_cfg;
	doca_error_t result = DOCA_SUCCESS;

	uint16_t nb_queues = app_cfg->dpdk_config.port_config.nb_queues;

	uint16_t rss_queues[nb_queues];
	for (int i = 0; i < nb_queues; i++)
		rss_queues[i] = i;
	doca_flow_resource_rss_cfg rss_config = {};
	rss_config.nr_queues = nb_queues;
	rss_config.queues_array = rss_queues;

	/* Init DOCA Flow with crypto shared resources */
	doca_flow_cfg *flow_cfg;
	IF_SUCCESS(result, doca_flow_cfg_create(&flow_cfg));
	IF_SUCCESS(result, doca_flow_cfg_set_pipe_queues(flow_cfg, nb_queues));
	IF_SUCCESS(result, doca_flow_cfg_set_nr_counters(flow_cfg, app_cfg->max_tunnels * NUM_OF_PSP_SYNDROMES + 10));
	IF_SUCCESS(result, doca_flow_cfg_set_mode_args(flow_cfg, "switch,hws,isolated,expert"));
	IF_SUCCESS(result, doca_flow_cfg_set_cb_entry_process(flow_cfg, PSP_GatewayFlows::check_for_valid_entry));
	IF_SUCCESS(result, doca_flow_cfg_set_default_rss(flow_cfg, &rss_config));
	IF_SUCCESS(result,
		   doca_flow_cfg_set_nr_shared_resource(flow_cfg,
							app_cfg->max_tunnels + 1,
							DOCA_FLOW_SHARED_RESOURCE_PSP));
	IF_SUCCESS(
		result,
		doca_flow_cfg_set_nr_shared_resource(flow_cfg, mirror_res_id_count, DOCA_FLOW_SHARED_RESOURCE_MIRROR));
	IF_SUCCESS(result, configure_tune_server(flow_cfg, DOCA_FLOW_TUNE_PROFILE_FULL));
	IF_SUCCESS(result, doca_flow_init(flow_cfg));
	if (result != DOCA_SUCCESS) {
		doca_flow_cfg_destroy(flow_cfg);
		return result;
	}
	DOCA_LOG_INFO("Initialized DOCA Flow for a max of %d tunnels", app_cfg->max_tunnels);
	doca_flow_cfg_destroy(flow_cfg);

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
			result = DOCA_SUCCESS;
		} else {
			DOCA_LOG_ERR("Failed to initialize the DOCA Flow Tune Server");
		}
	}

	doca_flow_tune_server_cfg_destroy(server_cfg);
	return result;
}

void PSP_GatewayFlows::init_status(psp_gw_app_config *app_config)
{
	app_config->status =
		std::vector<entries_status>(app_config->dpdk_config.port_config.nb_queues, entries_status());
}

doca_error_t PSP_GatewayFlows::bind_shared_resources(void)
{
	doca_error_t result = DOCA_SUCCESS;

	std::vector<uint32_t> psp_ids(app_config->max_tunnels);
	for (uint32_t i = 0; i < app_config->max_tunnels; i++) {
		psp_ids[i] = i + 1;
	}

	IF_SUCCESS(result,
		   doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_PSP,
						   psp_ids.data(),
						   app_config->max_tunnels,
						   pf_dev->port_obj));

	return result;
}

doca_error_t PSP_GatewayFlows::create_pipes(void)
{
	doca_error_t result = DOCA_SUCCESS;

	if (sampling_enabled) {
		IF_SUCCESS(result, ingress_sampling_pipe_create());
	}
	IF_SUCCESS(result, ingress_decrypt_pipe_create());

	if (sampling_enabled) {
		IF_SUCCESS(result, empty_pipe_create_not_sampled());
		IF_SUCCESS(result, egress_sampling_pipe_create());
		IF_SUCCESS(result, set_sample_bit_pipe_create());
	}
	IF_SUCCESS(result, egress_acl_pipe_create(true));
	IF_SUCCESS(result, egress_acl_pipe_create(false));
	IF_SUCCESS(result, create_egress_dst_ip6_pipe());
	IF_SUCCESS(result, empty_pipe_create());

	IF_SUCCESS(result, ingress_root_pipe_create());

	return result;
}

doca_error_t PSP_GatewayFlows::rss_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	doca_flow_match match_mask = {};

	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;

	// Note packets sent to RSS will be processed by lcore_pkt_proc_func().

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "RSS_PIPE"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_changeable_rss, nullptr, &rss_pipe));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    rss_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd_ipv4_rss,
				    &ipv4_rss_entry));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    rss_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd_ipv6_rss,
				    &ipv6_rss_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_decrypt_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(sampling_enabled ? ingress_sampling_pipe : ingress_inner_ip_classifier_pipe);
	assert(rss_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	match.parser_meta.port_id = UINT16_MAX;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);

	doca_flow_actions actions = {};
	actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
	actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	actions.crypto.crypto_id = DOCA_FLOW_PSP_DECRYPTION_ID;

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = sampling_enabled ? ingress_sampling_pipe : ingress_inner_ip_classifier_pipe;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "PSP_DECRYPT"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_decrypt_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	doca_flow_match match_uplink = {};
	match_uplink.parser_meta.port_id = 0;

	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_decrypt_pipe,
				    pf_dev->port_obj,
				    &match_uplink,
				    &actions,
				    nullptr,
				    nullptr,
				    &default_decrypt_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_inner_classifier_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.type = DOCA_FLOW_TUN_PSP;
		match.tun.psp.nexthdr = -1;
	} else {
		match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta) - 1; // changeable
	}

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = nullptr;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "PSP_INNER_IP_CLASSIFIER"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_inner_ip_classifier_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV6;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	}

	fwd.next_pipe = !app_config->disable_ingress_acl ? ingress_src_ip6_pipe : ingress_acl_ipv6_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_inner_ip_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ingress_ipv6_clasify_entry));

	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV4;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	}
	fwd.next_pipe = ingress_acl_ipv4_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_inner_ip_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ingress_ipv4_clasify_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_sampling_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(mirror_res_id_ingress);
	assert(rss_pipe);
	assert(sampling_enabled);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match_psp_sampling_bit = {};
	match_psp_sampling_bit.tun.type = DOCA_FLOW_TUN_PSP;
	match_psp_sampling_bit.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;

	doca_flow_monitor mirror_action = {};
	mirror_action.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	mirror_action.shared_mirror_id = mirror_res_id_ingress;

	doca_flow_actions set_meta = {};
	set_meta.meta.pkt_meta = DOCA_HTOBE32(app_config->ingress_sample_meta_indicator);

	doca_flow_actions *actions_arr[] = {&set_meta};

	doca_flow_actions set_meta_mask = {};
	set_meta_mask.meta.pkt_meta = UINT32_MAX;

	doca_flow_actions *actions_masks_arr[] = {&set_meta_mask};

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = ingress_inner_ip_classifier_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INGR_SAMPL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match_psp_sampling_bit, &match_psp_sampling_bit));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &mirror_action));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, nullptr, &fwd_miss, &ingress_sampling_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_sampling_pipe,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &default_ingr_sampling_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::create_ingress_src_ip6_pipe(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};
	doca_flow_actions actions = {};
	doca_flow_header_format *match_hdr = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.inner : &match.outer;
	doca_flow_l3_meta *l3_meta = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.parser_meta.inner_l3_type :
									      &match.parser_meta.outer_l3_type;
	match.tun.type = DOCA_FLOW_TUN_PSP;
	match.tun.psp.spi = UINT32_MAX;
	*l3_meta = DOCA_FLOW_L3_META_IPV6;
	match_hdr->l3_type = DOCA_FLOW_L3_TYPE_IP6;
	SET_IP6_ADDR(match_hdr->ip6.src_ip, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);

	actions.meta.u32[2] = UINT32_MAX;
	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = ingress_acl_ipv6_pipe;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = syndrome_stats_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "ING_SRC_IP6"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_src_ip6_pipe));
	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_acl_pipe_create(bool ipv4)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};
	struct doca_flow_pipe **pipe = ipv4 ? &ingress_acl_ipv4_pipe : &ingress_acl_ipv6_pipe;
	struct doca_flow_pipe_entry **entry = ipv4 ? &default_ingr_acl_ipv4_entry : &default_ingr_acl_ipv6_entry;
	match.parser_meta.psp_syndrome = UINT8_MAX;
	doca_flow_header_format *match_hdr = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.inner : &match.outer;
	if (!app_config->disable_ingress_acl) {
		match_hdr->l3_type = ipv4 ? DOCA_FLOW_L3_TYPE_IP4 : DOCA_FLOW_L3_TYPE_IP6;
		if (ipv4) {
			match.tun.type = DOCA_FLOW_TUN_PSP;
			match.tun.psp.spi = UINT32_MAX;
			match_hdr->ip4.src_ip = UINT32_MAX;
			match_hdr->ip4.dst_ip = UINT32_MAX;
		} else {
			match.meta.u32[2] = UINT32_MAX;
			SET_IP6_ADDR(match_hdr->ip6.dst_ip, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
		}
	}

	doca_flow_actions actions = {};
	actions.has_crypto_encap = true;
	actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
	actions.crypto_encap.net_type = ipv4 ? DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV4 :
					       DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV6;
	actions.crypto_encap.icv_size = PSP_ICV_SIZE;

	// In tunnel mode, we need to decap the eth/ip/udp/psp headers and add ethernet header
	// In transport mode, we only remove the udp/psp headers
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
		actions.crypto_encap.data_size = sizeof(rte_ether_hdr);

		rte_ether_hdr *eth_hdr = (rte_ether_hdr *)actions.crypto_encap.encap_data;
		eth_hdr->ether_type = ipv4 ? RTE_BE16(RTE_ETHER_TYPE_IPV4) : RTE_BE16(RTE_ETHER_TYPE_IPV6);
		eth_hdr->src_addr = pf_dev->src_mac;
		eth_hdr->dst_addr = app_config->dcap_dmac;
	}

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = vf_port_id;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = syndrome_stats_pipe;

	int nr_entries = app_config->disable_ingress_acl ? 1 : app_config->max_tunnels;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, ipv4 ? "INGR_ACL_IPV4" : "INGR_ACL_IPV6"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nr_entries));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	if (app_config->disable_ingress_acl) {
		doca_flow_match match_no_syndrome = {};
		IF_SUCCESS(result,
			   add_single_entry(0,
					    *pipe,
					    pf_dev->port_obj,
					    &match_no_syndrome,
					    &actions,
					    nullptr,
					    nullptr,
					    entry));
	}

	return result;
}

doca_error_t PSP_GatewayFlows::add_ingress_src_ip6_entry(psp_session_t *session, int dst_vip_id)
{
	doca_flow_match match = {};
	match.tun.type = DOCA_FLOW_TUN_PSP;
	match.tun.psp.spi = RTE_BE32(session->spi_ingress);
	doca_flow_header_format *match_hdr = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.inner : &match.outer;
	SET_IP6_ADDR(match_hdr->ip6.src_ip,
		     session->dst_vip.ipv6_addr[0],
		     session->dst_vip.ipv6_addr[1],
		     session->dst_vip.ipv6_addr[2],
		     session->dst_vip.ipv6_addr[3]);

	doca_flow_actions actions = {};
	actions.meta.u32[2] = dst_vip_id;

	return add_single_entry(0, ingress_src_ip6_pipe, pf_dev->port_obj, &match, &actions, nullptr, nullptr, nullptr);
}

doca_error_t PSP_GatewayFlows::add_ingress_acl_entry(psp_session_t *session, uint16_t queue_id)
{
	struct doca_flow_pipe *pipe;
	if (app_config->disable_ingress_acl) {
		DOCA_LOG_ERR("Cannot insert ingress ACL flow; disabled");
		return DOCA_ERROR_BAD_STATE;
	}

	doca_flow_match match = {};
	match.parser_meta.psp_syndrome = 0;
	doca_flow_header_format *match_hdr = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.inner : &match.outer;
	if (session->src_vip.type == DOCA_FLOW_L3_TYPE_IP4) {
		pipe = ingress_acl_ipv4_pipe;
		match.tun.type = DOCA_FLOW_TUN_PSP;
		match.tun.psp.spi = RTE_BE32(session->spi_ingress);
		match_hdr->l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match_hdr->ip4.src_ip = session->dst_vip.ipv4_addr; // use dst_vip of session as src for ingress
		match_hdr->ip4.dst_ip = session->src_vip.ipv4_addr; // use src_vip of session as dst for ingress
	} else {
		pipe = ingress_acl_ipv6_pipe;
		match_hdr->l3_type = DOCA_FLOW_L3_TYPE_IP6;
		SET_IP6_ADDR(match_hdr->ip6.dst_ip,
			     session->src_vip.ipv6_addr[0],
			     session->src_vip.ipv6_addr[1],
			     session->src_vip.ipv6_addr[2],
			     session->src_vip.ipv6_addr[3]);
		int dst_vip_id = rte_hash_lookup(app_config->ip6_table, session->dst_vip.ipv6_addr);
		if (dst_vip_id < 0) {
			DOCA_LOG_WARN("Failed to find source IP in table");
			int ret = rte_hash_add_key(app_config->ip6_table, session->dst_vip.ipv6_addr);
			if (ret < 0) {
				DOCA_LOG_ERR("Failed to add address to hash table");
				return DOCA_ERROR_DRIVER;
			}
			dst_vip_id = rte_hash_lookup(app_config->ip6_table, session->dst_vip.ipv6_addr);
		}
		match.meta.u32[2] = dst_vip_id;
		doca_error_t result = add_ingress_src_ip6_entry(session, dst_vip_id);
		if (result != DOCA_SUCCESS)
			return result;
	}

	doca_error_t result = DOCA_SUCCESS;
	IF_SUCCESS(result,
		   add_single_entry(queue_id,
				    pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    nullptr,
				    &session->acl_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::syndrome_stats_pipe_create(void)
{
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match syndrome_match = {};
	syndrome_match.parser_meta.psp_syndrome = 0xff;

	// If we got here, the packet failed either the PSP decryption syndrome check
	// or the ACL check. Whether the syndrome bits match here or not, the
	// fate of the packet is to be dropped.
	doca_flow_fwd fwd_drop = {};
	fwd_drop.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "SYNDROME_STATS"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NUM_OF_PSP_SYNDROMES));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &syndrome_match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_drop, &fwd_drop, &syndrome_stats_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	for (int i = 0; i < NUM_OF_PSP_SYNDROMES; i++) {
		// We don't hold counter for the SYNDROME_OK enum value (0) so we can skip it
		syndrome_match.parser_meta.psp_syndrome = i + 1;
		IF_SUCCESS(result,
			   add_single_entry(0,
					    syndrome_stats_pipe,
					    pf_dev->port_obj,
					    &syndrome_match,
					    nullptr,
					    &monitor_count,
					    nullptr,
					    &syndrome_stats_entries[i]));
	}

	return result;
}

doca_error_t PSP_GatewayFlows::create_egress_dst_ip6_pipe(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};
	doca_flow_actions actions = {};

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	SET_IP6_ADDR(match.outer.ip6.dst_ip, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);

	actions.meta.u32[2] = UINT32_MAX;
	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_acl_ipv6_pipe;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = rss_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_ACL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &egress_dst_ip6_pipe));
	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::egress_acl_pipe_create(bool ipv4)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(rss_pipe);
	assert(!sampling_enabled || egress_sampling_pipe);
	doca_error_t result = DOCA_SUCCESS;
	struct doca_flow_pipe **pipe = ipv4 ? &egress_acl_ipv4_pipe : &egress_acl_ipv6_pipe;
	doca_flow_match match = {};

	match.parser_meta.outer_l3_type = ipv4 ? DOCA_FLOW_L3_META_IPV4 : DOCA_FLOW_L3_META_IPV6;
	match.outer.l3_type = ipv4 ? DOCA_FLOW_L3_TYPE_IP4 : DOCA_FLOW_L3_TYPE_IP6;
	if (ipv4) {
		match.outer.ip4.dst_ip = UINT32_MAX;
		match.outer.ip4.src_ip = UINT32_MAX;
	} else {
		match.meta.u32[2] = UINT32_MAX;
		SET_IP6_ADDR(match.outer.ip6.src_ip, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
	}

	doca_flow_actions actions = {};
	doca_flow_actions encap_ipv4 = {};
	doca_flow_actions encap_ipv6 = {};

	actions.has_crypto_encap = true;
	actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
	actions.crypto_encap.icv_size = PSP_ICV_SIZE;
	actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
	actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	actions.crypto.crypto_id = UINT32_MAX; // per entry

	encap_ipv6 = actions;
	encap_ipv4 = actions;

	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		encap_ipv6.crypto_encap.net_type = encap_ipv4.crypto_encap.net_type =
			DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
		encap_ipv6.crypto_encap.data_size = sizeof(eth_ipv6_psp_tunnel_hdr);
		encap_ipv4.crypto_encap.data_size = sizeof(eth_ipv4_psp_tunnel_hdr);
	} else {
		encap_ipv6.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV6;
		encap_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV4;
		encap_ipv6.crypto_encap.data_size = encap_ipv4.crypto_encap.data_size = sizeof(udp_psp_transport_hdr);
	}

	if (!app_config->net_config.vc_enabled) {
		encap_ipv6.crypto_encap.data_size -= sizeof(uint64_t);
		encap_ipv4.crypto_encap.data_size -= sizeof(uint64_t);
	}
	memset(encap_ipv6.crypto_encap.encap_data, 0xff, encap_ipv6.crypto_encap.data_size);
	memset(encap_ipv4.crypto_encap.encap_data, 0xff, encap_ipv4.crypto_encap.data_size);

	doca_flow_actions *actions_arr[] = {&encap_ipv6, &encap_ipv4};

	doca_flow_fwd fwd_to_sampling = {};
	fwd_to_sampling.type = DOCA_FLOW_FWD_PIPE;
	fwd_to_sampling.next_pipe = set_sample_bit_pipe;

	doca_flow_fwd fwd_to_wire = {};
	fwd_to_wire.type = DOCA_FLOW_FWD_PORT;
	fwd_to_wire.port_id = pf_dev->port_id;

	auto p_fwd = sampling_enabled ? &fwd_to_sampling : &fwd_to_wire;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = rss_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, ipv4 ? "EGR_ACL_IPV4" : "EGR_ACL_IPV6"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, p_fwd, &fwd_miss, pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::add_egress_dst_ip6_entry(psp_session_t *session, int dst_vip_id)
{
	doca_flow_match match = {};
	SET_IP6_ADDR(match.outer.ip6.dst_ip,
		     session->dst_vip.ipv6_addr[0],
		     session->dst_vip.ipv6_addr[1],
		     session->dst_vip.ipv6_addr[2],
		     session->dst_vip.ipv6_addr[3]);

	doca_flow_actions actions = {};
	actions.meta.u32[2] = dst_vip_id;

	return add_single_entry(0, egress_dst_ip6_pipe, pf_dev->port_obj, &match, &actions, nullptr, nullptr, nullptr);
}

doca_error_t PSP_GatewayFlows::add_encrypt_entry(psp_session_t *session, const void *encrypt_key, uint16_t queue_id)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	std::string dst_pip = ip_to_string(session->dst_pip);
	std::string src_vip = ip_to_string(session->src_vip);
	std::string dst_vip = ip_to_string(session->dst_vip);
	struct doca_flow_pipe *pipe;

	DOCA_LOG_DBG("Creating encrypt flow entry: dst_pip %s, src_vip %s, dst_vip %s, SPI %d, crypto_id %d",
		     dst_pip.c_str(),
		     src_vip.c_str(),
		     dst_vip.c_str(),
		     session->spi_egress,
		     session->crypto_id);

	struct doca_flow_shared_resource_cfg res_cfg = {};
	res_cfg.psp_cfg.key_cfg.key_type = session->psp_proto_ver == 0 ? DOCA_FLOW_CRYPTO_KEY_128 :
									 DOCA_FLOW_CRYPTO_KEY_256;
	res_cfg.psp_cfg.key_cfg.key = (uint32_t *)encrypt_key;

	result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_PSP, session->crypto_id, &res_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure crypto_id %d: %s", session->crypto_id, doca_error_get_descr(result));
		return result;
	}

	doca_flow_match encap_encrypt_match = {};
	if (session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP4) {
		pipe = egress_acl_ipv4_pipe;
		encap_encrypt_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		encap_encrypt_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		encap_encrypt_match.outer.ip4.src_ip = session->src_vip.ipv4_addr;
		encap_encrypt_match.outer.ip4.dst_ip = session->dst_vip.ipv4_addr;
	} else {
		pipe = egress_acl_ipv6_pipe;
		encap_encrypt_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		encap_encrypt_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		SET_IP6_ADDR(encap_encrypt_match.outer.ip6.src_ip,
			     session->src_vip.ipv6_addr[0],
			     session->src_vip.ipv6_addr[1],
			     session->src_vip.ipv6_addr[2],
			     session->src_vip.ipv6_addr[3]);

		int dst_vip_id = rte_hash_lookup(app_config->ip6_table, session->dst_vip.ipv6_addr);
		if (dst_vip_id < 0) {
			DOCA_LOG_WARN("Failed to find source IP in table");
			int ret = rte_hash_add_key(app_config->ip6_table, session->dst_vip.ipv6_addr);
			if (ret < 0) {
				DOCA_LOG_ERR("Failed to add address to hash table");
				return DOCA_ERROR_DRIVER;
			}
			dst_vip_id = rte_hash_lookup(app_config->ip6_table, session->dst_vip.ipv6_addr);
		}
		encap_encrypt_match.meta.u32[2] = dst_vip_id;
		result = add_egress_dst_ip6_entry(session, dst_vip_id);
		if (result != DOCA_SUCCESS)
			return result;
	}

	doca_flow_actions encap_actions = {};
	encap_actions.has_crypto_encap = true;
	/* Add to ipv4/ipv6 according to dst pip in tunnel mode and dst vip in transport mode */
	if ((app_config->mode == PSP_GW_MODE_TUNNEL && session->dst_pip.type == DOCA_FLOW_L3_TYPE_IP6) ||
	    (app_config->mode == PSP_GW_MODE_TRANSPORT && session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP6)) {
		encap_actions.action_idx = 0;
	} else {
		encap_actions.action_idx = 1;
	}
	if (app_config->mode == PSP_GW_MODE_TRANSPORT)
		format_encap_transport_data(session, encap_actions.crypto_encap.encap_data);
	else if (session->dst_pip.type == DOCA_FLOW_L3_TYPE_IP6)
		format_encap_tunnel_data_ipv6(session, encap_actions.crypto_encap.encap_data);
	else
		format_encap_tunnel_data_ipv4(session, encap_actions.crypto_encap.encap_data);

	encap_actions.crypto.crypto_id = session->crypto_id;

	result = add_single_entry(queue_id,
				  pipe,
				  pf_dev->port_obj,
				  &encap_encrypt_match,
				  &encap_actions,
				  nullptr,
				  nullptr,
				  &session->encap_encrypt_entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add encrypt_encap pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created session entry: %p", session->encap_encrypt_entry);

	session->pkt_count_egress = UINT64_MAX; // force next query to detect a change

	return result;
}

void PSP_GatewayFlows::format_encap_tunnel_data_ipv6(const psp_session_t *session, uint8_t *encap_data)
{
	static const doca_be32_t DEFAULT_VTC_FLOW = 0x6 << 28;

	auto *encap_hdr = (eth_ipv6_psp_tunnel_hdr *)encap_data;
	encap_hdr->eth.ether_type = RTE_BE16(RTE_ETHER_TYPE_IPV6);
	encap_hdr->ip.vtc_flow = RTE_BE32(DEFAULT_VTC_FLOW);
	encap_hdr->ip.proto = IPPROTO_UDP;
	encap_hdr->ip.hop_limits = 50;
	encap_hdr->udp.src_port = 0x0; // computed
	encap_hdr->udp.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);
	encap_hdr->psp.nexthdr = session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP6 ? NEXT_HEADER_IPV6 : NEXT_HEADER_IPV4;
	encap_hdr->psp.hdrextlen = (uint8_t)(app_config->net_config.vc_enabled ? 2 : 1);
	encap_hdr->psp.res_cryptofst = (uint8_t)app_config->net_config.crypt_offset;
	encap_hdr->psp.spi = RTE_BE32(session->spi_egress);
	encap_hdr->psp_virt_cookie = RTE_BE64(session->vc);

	const auto &dmac = app_config->nexthop_enable ? app_config->nexthop_dmac : session->dst_mac;
	memcpy(encap_hdr->eth.src_addr.addr_bytes, pf_dev->src_mac.addr_bytes, RTE_ETHER_ADDR_LEN);
	memcpy(encap_hdr->eth.dst_addr.addr_bytes, dmac.addr_bytes, RTE_ETHER_ADDR_LEN);
	memcpy(encap_hdr->ip.src_addr, pf_dev->src_pip.ipv6_addr, IPV6_ADDR_LEN);
	memcpy(encap_hdr->ip.dst_addr, session->dst_pip.ipv6_addr, IPV6_ADDR_LEN);

	encap_hdr->psp.rsrv1 = 1; // always 1
	encap_hdr->psp.ver = session->psp_proto_ver;
	encap_hdr->psp.v = !!app_config->net_config.vc_enabled;
	// encap_hdr->psp.s will be set by the egress_sampling pipe
}

void PSP_GatewayFlows::format_encap_tunnel_data_ipv4(const psp_session_t *session, uint8_t *encap_data)
{
	auto *encap_hdr = (eth_ipv4_psp_tunnel_hdr *)encap_data;
	encap_hdr->eth.ether_type = RTE_BE16(RTE_ETHER_TYPE_IPV4);
	encap_hdr->udp.src_port = 0x0; // computed
	encap_hdr->udp.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);
	encap_hdr->psp.nexthdr = session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP6 ? NEXT_HEADER_IPV6 : NEXT_HEADER_IPV4;
	encap_hdr->psp.hdrextlen = (uint8_t)(app_config->net_config.vc_enabled ? 2 : 1);
	encap_hdr->psp.res_cryptofst = (uint8_t)app_config->net_config.crypt_offset;
	encap_hdr->psp.spi = RTE_BE32(session->spi_egress);
	encap_hdr->psp_virt_cookie = RTE_BE64(session->vc);

	const auto &dmac = app_config->nexthop_enable ? app_config->nexthop_dmac : session->dst_mac;
	memcpy(encap_hdr->eth.src_addr.addr_bytes, pf_dev->src_mac.addr_bytes, RTE_ETHER_ADDR_LEN);
	memcpy(encap_hdr->eth.dst_addr.addr_bytes, dmac.addr_bytes, RTE_ETHER_ADDR_LEN);
	encap_hdr->ip.src_addr = pf_dev->src_pip.ipv4_addr;
	encap_hdr->ip.dst_addr = session->dst_pip.ipv4_addr;
	encap_hdr->ip.version_ihl = 0x45;
	encap_hdr->ip.next_proto_id = IPPROTO_UDP;
	encap_hdr->ip.time_to_live = 64;

	encap_hdr->psp.rsrv1 = 1; // always 1
	encap_hdr->psp.ver = session->psp_proto_ver;
	encap_hdr->psp.v = !!app_config->net_config.vc_enabled;
	// encap_hdr->psp.s will be set by the egress_sampling pipe
}

void PSP_GatewayFlows::format_encap_transport_data(const psp_session_t *session, uint8_t *encap_data)
{
	auto *encap_hdr = (udp_psp_transport_hdr *)encap_data;
	encap_hdr->udp.src_port = 0x0; // computed
	encap_hdr->udp.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);
	encap_hdr->psp.nexthdr = 0; // computed
	encap_hdr->psp.hdrextlen = (uint8_t)(app_config->net_config.vc_enabled ? 2 : 1);
	encap_hdr->psp.res_cryptofst = (uint8_t)app_config->net_config.crypt_offset;
	encap_hdr->psp.spi = RTE_BE32(session->spi_egress);
	encap_hdr->psp_virt_cookie = RTE_BE64(session->vc);

	encap_hdr->psp.rsrv1 = 1; // always 1
	encap_hdr->psp.ver = session->psp_proto_ver;
	encap_hdr->psp.v = !!app_config->net_config.vc_enabled;
	// encap_hdr->psp.s will be set by the egress_sampling pipe
}

doca_error_t PSP_GatewayFlows::remove_encrypt_entry(psp_session_t *session)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	uint16_t pipe_queue = 0;
	uint32_t flags = DOCA_FLOW_NO_WAIT;
	uint32_t num_of_entries = 1;

	result = doca_flow_pipe_remove_entry(pipe_queue, flags, session->encap_encrypt_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Error removing PSP encap entry: %s", doca_error_get_descr(result));
	}

	result = doca_flow_entries_process(pf_dev->port_obj, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t PSP_GatewayFlows::egress_sampling_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(sampling_enabled);

	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match_mask = {};
	match_mask.tun.type = DOCA_FLOW_TUN_PSP;
	match_mask.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;

	doca_flow_match match = {};
	match.tun.type = DOCA_FLOW_TUN_PSP;
	match.tun.psp.s_d_ver_v = -1;

	doca_flow_monitor mirror_action = {};
	mirror_action.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	mirror_action.shared_mirror_id = -1;

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_to_wire_pipe;

	doca_flow_fwd fwd_miss = {};
	if (app_config->maintain_order) {
		fwd_miss.type = DOCA_FLOW_FWD_DROP;
	} else {
		fwd_miss = fwd;
	}

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_SAMPL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &mirror_action));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &egress_sampling_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	match.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	mirror_action.shared_mirror_id = mirror_res_id_rss;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    egress_sampling_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    &mirror_action,
				    nullptr,
				    &egr_sampling_rss));

	if (app_config->maintain_order) {
		match.tun.psp.s_d_ver_v = 0;
		mirror_action.shared_mirror_id = mirror_res_id_drop;
		IF_SUCCESS(result,
			   add_single_entry(0,
					    egress_sampling_pipe,
					    pf_dev->port_obj,
					    &match,
					    nullptr,
					    &mirror_action,
					    nullptr,
					    &egr_sampling_drop));
	}
	return result;
}

doca_error_t PSP_GatewayFlows::empty_pipe_create(void)
{
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	match.outer.eth.type = UINT16_MAX;
	match.meta.pkt_meta = UINT32_MAX;

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EMPTY"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_is_root(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 3));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &empty_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	match.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_ARP);
	match.meta.pkt_meta = RTE_BE32(app_config->return_to_vf_indicator); // ARP indicator
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = vf_port_id;
	IF_SUCCESS(
		result,
		add_single_entry(0, empty_pipe, pf_dev->port_obj, &match, nullptr, nullptr, &fwd, &arp_empty_pipe_entry));

	match.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_IPV6);
	// pkt_meta is already set as return_to_vf_indicator in previous entry
	// fwd.type is already set as DOCA_FLOW_FWD_PORT in previous entry
	// fwd.port_id is already set to vf_port_id in previous entry

	IF_SUCCESS(
		result,
		add_single_entry(0, empty_pipe, pf_dev->port_obj, &match, nullptr, nullptr, &fwd, &ns_empty_pipe_entry));

	match.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	match.meta.pkt_meta = 0;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_acl_ipv4_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ipv4_empty_pipe_entry));

	match.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_IPV6);
	fwd.next_pipe = egress_dst_ip6_pipe;
	// pkt_meta is already set as 0 in previous entry

	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ipv6_empty_pipe_entry));
	return result;
}

doca_error_t PSP_GatewayFlows::fwd_to_wire_pipe_create(void)
{
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = pf_dev->port_id;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "fwd_to_wire"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &fwd_to_wire_pipe));

	IF_SUCCESS(result,
		   add_single_entry(0,
				    fwd_to_wire_pipe,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &fwd_to_wire_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::fwd_to_rss_pipe_create(void)
{
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	doca_flow_match match_mask = {};

	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;

	doca_flow_actions actions = {};
	actions.meta.pkt_meta = DOCA_HTOBE32(app_config->egress_sample_meta_indicator);
	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_actions actions_mask = {};
	actions_mask.meta.pkt_meta = UINT32_MAX;
	doca_flow_actions *actions_masks_arr[] = {&actions_mask};

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "fwd_to_rss"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_changeable_rss, nullptr, &fwd_to_rss_pipe));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    fwd_to_rss_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd_ipv4_rss,
				    &fwd_ipv4_rss_entry));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    fwd_to_rss_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    &fwd_ipv6_rss,
				    &fwd_ipv6_rss_entry));
	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::set_sample_bit_pipe_create(void)
{
	doca_error_t result = DOCA_SUCCESS;

	uint16_t mask = (uint16_t)((1 << app_config->log2_sample_rate) - 1);
	DOCA_LOG_DBG("Sampling: matching (rand & 0x%x) == 1", mask);

	doca_flow_match match_sampling_match_mask = {};
	match_sampling_match_mask.parser_meta.random = DOCA_HTOBE16(mask);

	doca_flow_match match_sampling_match = {};
	match_sampling_match.parser_meta.random = DOCA_HTOBE16(0x1);

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_sampling_pipe;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = egress_sampling_pipe;

	doca_flow_actions set_sample_bit = {};
	set_sample_bit.tun.type = DOCA_FLOW_TUN_PSP;
	set_sample_bit.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;

	doca_flow_actions *actions_arr[] = {&set_sample_bit};
	doca_flow_actions *actions_masks_arr[] = {&set_sample_bit};

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "FWD_TO_RSS"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match_sampling_match, &match_sampling_match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &set_sample_bit_pipe));

	IF_SUCCESS(result,
		   add_single_entry(0,
				    set_sample_bit_pipe,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &set_sample_bit_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::empty_pipe_create_not_sampled(void)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = pf_dev->port_id;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EMPTY_NOT_SAMPLED"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, &empty_pipe_not_sampled));
	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe_not_sampled,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &empty_pipe_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_root_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(ingress_decrypt_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "ROOT"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_is_root(pipe_cfg, true));
	uint32_t nb_entries = app_config->mode == PSP_GW_MODE_TUNNEL ? 7 : 9;
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nb_entries));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, nullptr, nullptr, &ingress_root_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	// Note outer_l4_ok can be matched with spec=true, mask=UINT8_MAX to
	// restrict traffic to TCP/UDP (ICMP would miss to RSS).
	doca_flow_match mask = {};
	mask.parser_meta.port_id = UINT16_MAX;
	mask.parser_meta.outer_l3_ok = UINT8_MAX;
	mask.parser_meta.outer_ip4_checksum_ok = UINT8_MAX;
	mask.outer.eth.type = UINT16_MAX;

	doca_flow_match ipv6_from_uplink = {};
	ipv6_from_uplink.parser_meta.port_id = pf_dev->port_id;
	ipv6_from_uplink.parser_meta.outer_l3_ok = true;
	ipv6_from_uplink.parser_meta.outer_ip4_checksum_ok = false;
	ipv6_from_uplink.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV6);

	doca_flow_match ipv4_from_uplink = {};
	ipv4_from_uplink.parser_meta.port_id = pf_dev->port_id;
	ipv4_from_uplink.parser_meta.outer_l3_ok = true;
	ipv4_from_uplink.parser_meta.outer_ip4_checksum_ok = true;
	ipv4_from_uplink.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV4);

	doca_flow_match ipv4_from_vf = {};
	ipv4_from_vf.parser_meta.port_id = vf_port_id;
	ipv4_from_vf.parser_meta.outer_l3_ok = true;
	ipv4_from_vf.parser_meta.outer_ip4_checksum_ok = true;
	ipv4_from_vf.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV4);

	doca_flow_match ipv6_from_vf = {};
	ipv6_from_vf.parser_meta.port_id = vf_port_id;
	ipv6_from_vf.parser_meta.outer_l3_ok = true;
	ipv6_from_vf.parser_meta.outer_ip4_checksum_ok = false;
	ipv6_from_vf.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV6);

	doca_flow_match arp_mask = {};
	arp_mask.parser_meta.port_id = UINT16_MAX;
	arp_mask.outer.eth.type = UINT16_MAX;

	doca_flow_match ns_mask = {};
	ns_mask.parser_meta.port_id = UINT16_MAX;
	ns_mask.outer.eth.type = UINT16_MAX;
	ns_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	ns_mask.parser_meta.outer_l4_type = (enum doca_flow_l4_meta)UINT32_MAX;
	ns_mask.outer.l4_type_ext = (enum doca_flow_l4_type_ext)UINT32_MAX;
	ns_mask.outer.ip6.next_proto = UINT8_MAX;
	ns_mask.outer.icmp.type = UINT8_MAX;

	doca_flow_match arp_from_vf = {};
	arp_from_vf.parser_meta.port_id = vf_port_id;
	arp_from_vf.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_ARP);

	doca_flow_match arp_from_uplink = {};
	arp_from_uplink.parser_meta.port_id = pf_dev->port_id;
	arp_from_uplink.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_ARP);

	doca_flow_match ns_from_vf = {};
	ns_from_vf.parser_meta.port_id = vf_port_id;
	ns_from_vf.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV6);
	ns_from_vf.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	ns_from_vf.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ICMP;
	ns_from_vf.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ICMP6;
	ns_from_vf.outer.ip6.next_proto = IPPROTO_ICMPV6;
	ns_from_vf.outer.icmp.type = ND_NEIGHBOR_SOLICIT;

	doca_flow_match ns_from_uplink = {};
	ns_from_uplink.parser_meta.port_id = pf_dev->port_id;
	ns_from_uplink.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV6);
	ns_from_uplink.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	ns_from_uplink.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ICMP;
	ns_from_uplink.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ICMP6;
	ns_from_uplink.outer.ip6.next_proto = IPPROTO_ICMPV6;
	ns_from_uplink.outer.icmp.type = ND_NEIGHBOR_SOLICIT;

	doca_flow_match empty_match = {};

	doca_flow_fwd fwd_ingress = {};
	fwd_ingress.type = DOCA_FLOW_FWD_PIPE;
	fwd_ingress.next_pipe = ingress_decrypt_pipe;

	doca_flow_fwd fwd_egress = {};
	fwd_egress.type = DOCA_FLOW_FWD_PIPE;
	fwd_egress.next_pipe = empty_pipe; // and then to egress acl pipes

	doca_flow_fwd fwd_to_vf = {};
	fwd_to_vf.type = DOCA_FLOW_FWD_PORT;
	fwd_to_vf.port_id = vf_port_id;

	doca_flow_fwd fwd_to_wire = {};
	fwd_to_wire.type = DOCA_FLOW_FWD_PORT;
	fwd_to_wire.port_id = pf_dev->port_id;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	uint16_t pipe_queue = 0;

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    2,
						    ingress_root_pipe,
						    &ipv6_from_uplink,
						    &mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_ingress,
						    nullptr,
						    &root_jump_to_ingress_ipv6_entry));

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    1,
						    ingress_root_pipe,
						    &ipv4_from_uplink,
						    &mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_ingress,
						    nullptr,
						    &root_jump_to_ingress_ipv4_entry));

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    2,
						    ingress_root_pipe,
						    &ipv6_from_vf,
						    &mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_egress,
						    nullptr,
						    &root_jump_to_egress_ipv6_entry));

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    2,
						    ingress_root_pipe,
						    &ipv4_from_vf,
						    &mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_egress,
						    nullptr,
						    &root_jump_to_egress_ipv4_entry));

	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		// In tunnel mode, ARP packets are handled by the application
		IF_SUCCESS(result,
			   doca_flow_pipe_control_add_entry(pipe_queue,
							    3,
							    ingress_root_pipe,
							    &arp_from_vf,
							    &arp_mask,
							    nullptr,
							    nullptr,
							    nullptr,
							    nullptr,
							    &monitor_count,
							    &fwd_ipv4_rss,
							    nullptr,
							    &vf_arp_to_rss));

		IF_SUCCESS(result,
			   doca_flow_pipe_control_add_entry(pipe_queue,
							    1,
							    ingress_root_pipe,
							    &ns_from_vf,
							    &ns_mask,
							    nullptr,
							    nullptr,
							    nullptr,
							    nullptr,
							    &monitor_count,
							    &fwd_ipv6_rss,
							    nullptr,
							    &vf_ns_to_rss));
	} else {
		// In transport mode, ARP packets are forwarded to the opposite port (PF or VF)
		IF_SUCCESS(result,
			   doca_flow_pipe_control_add_entry(pipe_queue,
							    3,
							    ingress_root_pipe,
							    &arp_from_vf,
							    &arp_mask,
							    nullptr,
							    nullptr,
							    nullptr,
							    nullptr,
							    &monitor_count,
							    &fwd_to_wire,
							    nullptr,
							    &vf_arp_to_wire));
		IF_SUCCESS(result,
			   doca_flow_pipe_control_add_entry(pipe_queue,
							    3,
							    ingress_root_pipe,
							    &arp_from_uplink,
							    &arp_mask,
							    nullptr,
							    nullptr,
							    nullptr,
							    nullptr,
							    &monitor_count,
							    &fwd_to_vf,
							    nullptr,
							    &uplink_arp_to_vf));

		IF_SUCCESS(result,
			   doca_flow_pipe_control_add_entry(pipe_queue,
							    1,
							    ingress_root_pipe,
							    &ns_from_vf,
							    &ns_mask,
							    nullptr,
							    nullptr,
							    nullptr,
							    nullptr,
							    &monitor_count,
							    &fwd_to_wire,
							    nullptr,
							    &vf_ns_to_wire));
		IF_SUCCESS(result,
			   doca_flow_pipe_control_add_entry(pipe_queue,
							    1,
							    ingress_root_pipe,
							    &ns_from_uplink,
							    &ns_mask,
							    nullptr,
							    nullptr,
							    nullptr,
							    nullptr,
							    &monitor_count,
							    &fwd_to_vf,
							    nullptr,
							    &uplink_ns_to_vf));
	}
	// default miss in switch mode goes to NIC domain. this entry ensures to drop a non-matched packet
	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    4,
						    ingress_root_pipe,
						    &empty_match,
						    &empty_match,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_miss,
						    nullptr,
						    &root_default_drop));

	return result;
}

/*
 * Entry processing callback
 *
 * @entry [in]: entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
void PSP_GatewayFlows::check_for_valid_entry(doca_flow_pipe_entry *entry,
					     uint16_t pipe_queue,
					     enum doca_flow_entry_status status,
					     enum doca_flow_entry_op op,
					     void *user_ctx)
{
	(void)entry;
	(void)op;
	(void)pipe_queue;

	auto *entry_status = (entries_status *)user_ctx;

	if (entry_status == NULL || op != DOCA_FLOW_ENTRY_OP_ADD)
		return;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->failure = true; /* set failure to true if processing failed */

	entry_status->nb_processed++;
	entry_status->entries_in_queue--;
}

doca_error_t PSP_GatewayFlows::add_single_entry(uint16_t pipe_queue,
						doca_flow_pipe *pipe,
						doca_flow_port *port,
						const doca_flow_match *match,
						const doca_flow_actions *actions,
						const doca_flow_monitor *mon,
						const doca_flow_fwd *fwd,
						doca_flow_pipe_entry **entry)
{
	int num_of_entries = 1;
	uint32_t flags = DOCA_FLOW_NO_WAIT;

	app_config->status[pipe_queue] = entries_status();
	app_config->status[pipe_queue].entries_in_queue = num_of_entries;

	doca_error_t result = doca_flow_pipe_add_entry(pipe_queue,
						       pipe,
						       match,
						       actions,
						       mon,
						       fwd,
						       flags,
						       &app_config->status[pipe_queue],
						       entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, pipe_queue, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entry: %s", doca_error_get_descr(result));
		return result;
	}

	if (app_config->status[pipe_queue].nb_processed != num_of_entries || app_config->status[pipe_queue].failure) {
		DOCA_LOG_ERR("Failed to process entry; nb_processed = %d, failure = %d",
			     app_config->status[pipe_queue].nb_processed,
			     app_config->status[pipe_queue].failure);
		return DOCA_ERROR_BAD_STATE;
	}

	return result;
}

struct PSP_GatewayFlows::pipe_query {
	doca_flow_pipe *pipe;	     // used to query misses
	doca_flow_pipe_entry *entry; // used to query static entries
	std::string name;	     // displays the pipe name
};

std::pair<uint64_t, uint64_t> PSP_GatewayFlows::perform_pipe_query(pipe_query *query, bool suppress_output)
{
	uint64_t new_hits = 0;
	uint64_t new_misses = 0;

	if (query->entry) {
		doca_flow_resource_query stats = {};
		doca_error_t result = doca_flow_resource_query_entry(query->entry, &stats);
		if (result == DOCA_SUCCESS) {
			new_hits = stats.counter.total_pkts;
		}
	}
	if (query->pipe) {
		doca_flow_resource_query stats = {};
		doca_error_t result = doca_flow_resource_query_pipe_miss(query->pipe, &stats);
		if (result == DOCA_SUCCESS) {
			new_misses = stats.counter.total_pkts;
		}
	}
	if (!suppress_output) {
		if (query->entry && query->pipe) {
			DOCA_LOG_INFO("%s: %ld hits %ld misses", query->name.c_str(), new_hits, new_misses);
		} else if (query->entry) {
			DOCA_LOG_INFO("%s: %ld hits", query->name.c_str(), new_hits);
		} else if (query->pipe) {
			DOCA_LOG_INFO("%s: %ld misses", query->name.c_str(), new_hits);
		}
	}

	return std::make_pair(new_hits, new_misses);
}

void PSP_GatewayFlows::show_static_flow_counts(void)
{
	std::vector<pipe_query> queries;
	queries.emplace_back(pipe_query{nullptr, ipv4_rss_entry, "ipv4_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, ipv6_rss_entry, "ipv6_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, root_jump_to_ingress_ipv6_entry, "root_jump_to_ingress_ipv6_entry"});
	queries.emplace_back(pipe_query{nullptr, root_jump_to_ingress_ipv4_entry, "root_jump_to_ingress_ipv4_entry"});
	queries.emplace_back(pipe_query{nullptr, root_jump_to_egress_ipv6_entry, "root_jump_to_egress_ipv6_entry"});
	queries.emplace_back(pipe_query{nullptr, root_jump_to_egress_ipv4_entry, "root_jump_to_egress_ipv4_entry"});
	queries.emplace_back(pipe_query{nullptr, vf_arp_to_rss, "vf_arp_to_rss"});
	queries.emplace_back(pipe_query{nullptr, vf_ns_to_rss, "vf_ns_to_rss"});
	queries.emplace_back(pipe_query{nullptr, vf_arp_to_wire, "vf_arp_to_wire"});
	queries.emplace_back(pipe_query{nullptr, uplink_arp_to_vf, "uplink_arp_to_vf"});
	queries.emplace_back(pipe_query{nullptr, vf_ns_to_wire, "vf_ns_to_wire"});
	queries.emplace_back(pipe_query{nullptr, uplink_ns_to_vf, "uplink_ns_to_vf"});
	queries.emplace_back(pipe_query{nullptr, root_default_drop, "root_miss_drop"});
	queries.emplace_back(pipe_query{ingress_decrypt_pipe, default_decrypt_entry, "default_decrypt_entry"});
	queries.emplace_back(
		pipe_query{ingress_inner_ip_classifier_pipe, ingress_ipv4_clasify_entry, "ingress_ipv4_clasify"});
	queries.emplace_back(
		pipe_query{ingress_inner_ip_classifier_pipe, ingress_ipv6_clasify_entry, "ingress_ipv6_clasify"});
	queries.emplace_back(pipe_query{ingress_sampling_pipe, default_ingr_sampling_entry, "ingress_sampling_pipe"});
	queries.emplace_back(pipe_query{ingress_acl_ipv4_pipe, default_ingr_acl_ipv4_entry, "ingress_acl_ipv4_pipe"});
	queries.emplace_back(pipe_query{ingress_acl_ipv6_pipe, default_ingr_acl_ipv6_entry, "ingress_acl_ipv6_pipe"});
	for (int i = 0; i < NUM_OF_PSP_SYNDROMES; i++) {
		switch (i + 1) {
		case DOCA_FLOW_CRYPTO_SYNDROME_ICV_FAIL:
			queries.emplace_back(pipe_query{nullptr, syndrome_stats_entries[i], "syndrome - ICV Fail"});
			break;
		case DOCA_FLOW_CRYPTO_SYNDROME_BAD_TRAILER:
			queries.emplace_back(pipe_query{nullptr, syndrome_stats_entries[i], "syndrome - Bad Trailer"});
			break;
		}
	}
	queries.emplace_back(pipe_query{empty_pipe, nullptr, "egress_root"});
	queries.emplace_back(pipe_query{egress_acl_ipv4_pipe, nullptr, "egress_acl_ipv4_pipe"});
	queries.emplace_back(pipe_query{egress_acl_ipv6_pipe, nullptr, "egress_acl_ipv6_pipe"});
	queries.emplace_back(pipe_query{egress_sampling_pipe, egr_sampling_rss, "egress_sampling_rss"});
	queries.emplace_back(pipe_query{egress_sampling_pipe, egr_sampling_drop, "egress_sampling_drop"});
	queries.emplace_back(pipe_query{nullptr, empty_pipe_entry, "arp_packets_intercepted"});
	queries.emplace_back(pipe_query{fwd_to_wire_pipe, fwd_to_wire_entry, "fwd_to_wire_entry"});
	queries.emplace_back(pipe_query{nullptr, fwd_ipv4_rss_entry, "fwd_ipv4_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, fwd_ipv6_rss_entry, "fwd_ipv6_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, ipv4_empty_pipe_entry, "fwd_egress_acl_ipv4"});
	queries.emplace_back(pipe_query{nullptr, ipv6_empty_pipe_entry, "fwd_egress_acl_ipv6"});
	queries.emplace_back(pipe_query{nullptr, ns_empty_pipe_entry, "ns_empty_pipe_entry"});

	uint64_t total_pkts = 0;
	for (auto &query : queries) {
		auto hits_misses = perform_pipe_query(&query, true);
		total_pkts += hits_misses.first + hits_misses.second;
	}

	if (total_pkts != prev_static_flow_count) {
		total_pkts = 0;
		DOCA_LOG_INFO("-------------------------");
		for (auto &query : queries) {
			auto hits_misses = perform_pipe_query(&query, false);
			total_pkts += hits_misses.first + hits_misses.second;
		}
		prev_static_flow_count = total_pkts;
	}
}

void PSP_GatewayFlows::show_session_flow_count(const session_key session_vips_pair, psp_session_t &session)
{
	if (session.encap_encrypt_entry) {
		doca_flow_resource_query encap_encrypt_stats = {};
		doca_error_t encap_result =
			doca_flow_resource_query_entry(session.encap_encrypt_entry, &encap_encrypt_stats);

		if (encap_result == DOCA_SUCCESS) {
			if (session.pkt_count_egress != encap_encrypt_stats.counter.total_pkts) {
				DOCA_LOG_DBG("Session Egress (%s -> %s) entry: %p",
					     session_vips_pair.first.c_str(),
					     session_vips_pair.second.c_str(),
					     session.encap_encrypt_entry);
				DOCA_LOG_INFO("Session Egress (%s -> %s): %ld hits",
					      session_vips_pair.first.c_str(),
					      session_vips_pair.second.c_str(),
					      encap_encrypt_stats.counter.total_pkts);
				session.pkt_count_egress = encap_encrypt_stats.counter.total_pkts;
			}
		} else {
			DOCA_LOG_INFO("Session Egress (%s -> %s): query failed: %s",
				      session_vips_pair.first.c_str(),
				      session_vips_pair.second.c_str(),
				      doca_error_get_descr(encap_result));
		}
	}

	if (!app_config->disable_ingress_acl && session.acl_entry) {
		doca_flow_resource_query acl_stats = {};
		doca_error_t result = doca_flow_resource_query_entry(session.acl_entry, &acl_stats);

		if (result == DOCA_SUCCESS) {
			if (session.pkt_count_ingress != acl_stats.counter.total_pkts) {
				DOCA_LOG_DBG("Session ACL entry: %p", session.acl_entry);
				DOCA_LOG_INFO("Session Ingress (%s <- %s): %ld hits",
					      session_vips_pair.first.c_str(),
					      session_vips_pair.second.c_str(),
					      acl_stats.counter.total_pkts);
				session.pkt_count_ingress = acl_stats.counter.total_pkts;
			}
		} else {
			DOCA_LOG_INFO("Session Ingress (%s <- %s): query failed: %s",
				      session_vips_pair.first.c_str(),
				      session_vips_pair.second.c_str(),
				      doca_error_get_descr(result));
		}
	}
}
