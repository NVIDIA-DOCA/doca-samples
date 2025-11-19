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

#define INGRESS_ACL_IPV4_SEQ_IDX 0 /* IPv4 sequence index for ordered list */
#define INGRESS_ACL_IPV6_SEQ_IDX 1 /* IPv6 sequence index for ordered list */

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
	IF_SUCCESS(result, start_port(pf_dev->port_id, pf_dev->dev, app_config, nullptr, &pf_dev->port_obj));
	IF_SUCCESS(result, start_port(vf_port_id, nullptr, nullptr, vf_dev_rep, &vf_port));
	IF_SUCCESS(result, bind_shared_resources());
	init_status(app_config);
	IF_SUCCESS(result, rss_pipe_create());
	if (sampling_enabled) {
		IF_SUCCESS(result, fwd_to_rss_pipe_create());
	}
	IF_SUCCESS(result, syndrome_stats_pipe_create());
	IF_SUCCESS(result, ingress_acl_pipe_create());
	IF_SUCCESS(result, match_ingress_acl_pipe_create(true));  // Create IPv4 match pipe
	IF_SUCCESS(result, match_ingress_acl_pipe_create(false)); // Create IPv6 match pipe
	if (!app_config->disable_ingress_acl) {
		IF_SUCCESS(result, ingress_src_ip6_pipe_create());
	}
	IF_SUCCESS(result, ingress_inner_classifier_pipe_create());
	if (sampling_enabled) {
		IF_SUCCESS(result, configure_flooding());
	}
	IF_SUCCESS(result, create_pipes());

	return result;
}

doca_error_t PSP_GatewayFlows::configure_flooding(void)
{
	assert(rss_pipe);
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_fwd fwd = {};

	IF_SUCCESS(result,
		   prepare_flooding_pipe(pf_dev->port_obj,
					 DOCA_FLOW_PIPE_DOMAIN_DEFAULT,
					 &flooding_ingress_classifier_ipv4_rss_pipe));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = ingress_inner_ip_classifier_pipe;
	IF_SUCCESS(result,
		   add_single_flooding_entry(0,
					     flooding_ingress_classifier_ipv4_rss_pipe,
					     pf_dev->port_obj,
					     0,
					     &fwd,
					     &flooding_ingress_rss_ipv4_entry));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = rss_pipe;
	IF_SUCCESS(result,
		   add_single_flooding_entry(0,
					     flooding_ingress_classifier_ipv4_rss_pipe,
					     pf_dev->port_obj,
					     1,
					     &fwd,
					     &flooding_ingress_inner_ipv4_classifier_entry));

	IF_SUCCESS(result,
		   prepare_flooding_pipe(pf_dev->port_obj,
					 DOCA_FLOW_PIPE_DOMAIN_DEFAULT,
					 &flooding_ingress_classifier_ipv6_rss_pipe));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = !app_config->disable_ingress_acl ? ingress_src_ip6_pipe : match_ingress_acl_ipv6_pipe;
	IF_SUCCESS(result,
		   add_single_flooding_entry(0,
					     flooding_ingress_classifier_ipv6_rss_pipe,
					     pf_dev->port_obj,
					     0,
					     &fwd,
					     &flooding_ingress_rss_ipv6_entry));
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = rss_pipe;
	IF_SUCCESS(result,
		   add_single_flooding_entry(0,
					     flooding_ingress_classifier_ipv6_rss_pipe,
					     pf_dev->port_obj,
					     1,
					     &fwd,
					     &flooding_ingress_inner_ipv6_classifier_entry));

	IF_SUCCESS(
		result,
		prepare_flooding_pipe(pf_dev->port_obj, DOCA_FLOW_PIPE_DOMAIN_EGRESS, &flooding_egress_wire_rss_pipe));

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = pf_dev->port_id;
	IF_SUCCESS(result,
		   add_single_flooding_entry(0,
					     flooding_egress_wire_rss_pipe,
					     pf_dev->port_obj,
					     0,
					     &fwd,
					     &flooding_egress_to_wire_entry));

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = fwd_to_rss_pipe;
	IF_SUCCESS(result,
		   add_single_flooding_entry(0,
					     flooding_egress_wire_rss_pipe,
					     pf_dev->port_obj,
					     1,
					     &fwd,
					     &flooding_egress_to_rss_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::start_port(uint16_t port_id,
					  doca_dev *port_dev,
					  const psp_gw_app_config *app_cfg,
					  doca_dev_rep *port_rep,
					  doca_flow_port **port)
{
	doca_flow_port_cfg *port_cfg;
	doca_error_t result = DOCA_SUCCESS;

	IF_SUCCESS(result, doca_flow_port_cfg_create(&port_cfg));

	if (app_cfg) {
		IF_SUCCESS(result,
			   doca_flow_port_cfg_set_nr_resources(port_cfg,
							       DOCA_FLOW_RESOURCE_COUNTER,
							       app_cfg->max_tunnels * NUM_OF_PSP_SYNDROMES + 10));
		IF_SUCCESS(result,
			   doca_flow_port_cfg_set_nr_resources(port_cfg, DOCA_FLOW_RESOURCE_PSP, app_cfg->max_tunnels));
	}
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

	/* Init DOCA Flow with crypto shared resources */
	doca_flow_cfg *flow_cfg;
	IF_SUCCESS(result, doca_flow_cfg_create(&flow_cfg));
	IF_SUCCESS(result, doca_flow_cfg_set_pipe_queues(flow_cfg, nb_queues));
	IF_SUCCESS(result, doca_flow_cfg_set_mode_args(flow_cfg, "switch,hws,isolated,expert"));
	IF_SUCCESS(result, doca_flow_cfg_set_cb_entry_process(flow_cfg, PSP_GatewayFlows::check_for_valid_entry));
	IF_SUCCESS(result, doca_flow_cfg_set_resource_mode(flow_cfg, DOCA_FLOW_RESOURCE_MODE_PORT));
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
		IF_SUCCESS(result,
			   doca_flow_port_shared_resource_get(pf_dev->port_obj,
							      DOCA_FLOW_SHARED_RESOURCE_PSP,
							      &psp_ids[i]));
	}
	pf_dev->crypto_ids = psp_ids;

	return result;
}

doca_error_t PSP_GatewayFlows::create_pipes(void)
{
	doca_error_t result = DOCA_SUCCESS;

	if (sampling_enabled) {
		IF_SUCCESS(result, ingress_sampling_classifier_pipe_create());
	}
	IF_SUCCESS(result, ingress_decrypt_pipe_create());
	IF_SUCCESS(result, match_ingress_decrypt_pipe_create());

	if (sampling_enabled) {
		IF_SUCCESS(result, empty_pipe_not_sampled_create());
		IF_SUCCESS(result, egress_sampling_pipe_create());
		IF_SUCCESS(result, set_sample_bit_pipe_create());
	}
	IF_SUCCESS(result, egress_acl_pipe_create());
	IF_SUCCESS(result, match_egress_acl_pipe_create(true));	 // Create IPv4 match pipe
	IF_SUCCESS(result, match_egress_acl_pipe_create(false)); // Create IPv6 match pipe
	IF_SUCCESS(result, egress_dst_ip6_pipe_create());
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
				    0,
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
				    0,
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
	assert(sampling_enabled ? ingress_sampling_classifier_pipe : ingress_inner_ip_classifier_pipe);
	assert(rss_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_crypto crypto_actions = {};
	crypto_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
	crypto_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	crypto_actions.crypto.crypto_id = DOCA_FLOW_PSP_DECRYPTION_ID;

	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element elements[1];
	elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements[0].crypto = &crypto_actions;

	ordered_list.idx = 0;
	ordered_list.size = 1;
	ordered_list.elements = elements;

	const int nb_ordered_lists = 1;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	ordered_lists[0] = &ordered_list;

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = sampling_enabled ? ingress_sampling_classifier_pipe : ingress_inner_ip_classifier_pipe;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = syndrome_stats_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "DECRYPT"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_decrypt_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}
	pipe_cfg = NULL;

	IF_SUCCESS(result,
		   add_single_entry_ordered_list(0,
						 ingress_decrypt_pipe,
						 pf_dev->port_obj,
						 0,
						 &ordered_list,
						 &fwd,
						 &default_decrypt_entry));
	return result;
}

doca_error_t PSP_GatewayFlows::match_ingress_decrypt_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_pipe_cfg *pipe_cfg = NULL;

	/* Add pipe to match psp packets from uplink and fwd all to ordered list for decryption */
	doca_flow_match match = {};
	match.parser_meta.port_id = UINT16_MAX;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);

	doca_flow_fwd fwd_to_ordered_list = {};
	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = ingress_decrypt_pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = 0;

	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	/* pipe that jump to ordered list pipe must be in the same domain as the ordered list pipe */
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_to_ordered_list, NULL, &match_ingress_decrypt_pipe));
	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	doca_flow_match match_uplink = {};
	match_uplink.parser_meta.port_id = 0;

	IF_SUCCESS(result,
		   add_single_entry(0,
				    match_ingress_decrypt_pipe,
				    pf_dev->port_obj,
				    &match_uplink,
				    0,
				    nullptr,
				    nullptr,
				    &fwd_to_ordered_list,
				    &default_decrypt_match_entry));

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
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = syndrome_stats_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INNER_IP_CLASSIFIER"));
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

	fwd.next_pipe = !app_config->disable_ingress_acl ? ingress_src_ip6_pipe : match_ingress_acl_ipv6_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_inner_ip_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ingress_ipv6_clasify_entry));

	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV4;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	}
	fwd.next_pipe = match_ingress_acl_ipv4_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_inner_ip_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ingress_ipv4_clasify_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_sampling_classifier_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(flooding_ingress_classifier_ipv4_rss_pipe);
	assert(rss_pipe);
	assert(sampling_enabled);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	match.tun.type = DOCA_FLOW_TUN_PSP;
	match.tun.psp.s_d_ver_v = UINT8_MAX;
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.type = DOCA_FLOW_TUN_PSP;
		match.tun.psp.nexthdr = -1;
	} else {
		match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta) - 1; // changeable
	}

	doca_flow_match match_mask = {};
	match_mask.tun.type = DOCA_FLOW_TUN_PSP;
	match_mask.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	match_mask.tun.psp.nexthdr = -1;
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match_mask.tun.type = DOCA_FLOW_TUN_PSP;
		match_mask.tun.psp.nexthdr = -1;
	} else {
		match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta) - 1; // changeable
	}

	doca_flow_actions set_meta = {};
	set_meta.meta.pkt_meta = DOCA_HTOBE32(app_config->ingress_sample_meta_indicator);

	doca_flow_actions *actions_arr[] = {&set_meta};

	doca_flow_actions set_meta_mask = {};
	set_meta_mask.meta.pkt_meta = UINT32_MAX;

	doca_flow_actions *actions_masks_arr[] = {&set_meta_mask};

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = ingress_inner_ip_classifier_pipe;

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INGR_SAMPL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 4));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_sampling_classifier_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	match.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV4;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	}

	fwd.type = DOCA_FLOW_FWD_HASH_PIPE;
	fwd.hash_pipe.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING;
	fwd.hash_pipe.pipe = flooding_ingress_classifier_ipv4_rss_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_sampling_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &default_ingr_sampling_ipv4_entry));

	match.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV6;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	}

	fwd.type = DOCA_FLOW_FWD_HASH_PIPE;
	fwd.hash_pipe.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING;
	fwd.hash_pipe.pipe = flooding_ingress_classifier_ipv6_rss_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_sampling_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &default_ingr_sampling_ipv6_entry));

	match.tun.psp.s_d_ver_v = 0;
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV4;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = match_ingress_acl_ipv4_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_sampling_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &fwd_ipv4_sample_entry));

	match.tun.psp.s_d_ver_v = 0;
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		match.tun.psp.nexthdr = NEXT_HEADER_IPV6;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = !app_config->disable_ingress_acl ? ingress_src_ip6_pipe : match_ingress_acl_ipv6_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_sampling_classifier_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &fwd_ipv6_sample_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_src_ip6_pipe_create(void)
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
	fwd.next_pipe = match_ingress_acl_ipv6_pipe;

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

doca_error_t PSP_GatewayFlows::ingress_acl_pipe_create()
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;

	// Create crypto actions for IPv4 sequence
	doca_flow_crypto crypto_actions_ipv4 = {};
	crypto_actions_ipv4.has_crypto_encap = true;
	crypto_actions_ipv4.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
	crypto_actions_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV4;
	crypto_actions_ipv4.crypto_encap.icv_size = PSP_ICV_SIZE;

	// Create crypto actions for IPv6 sequence
	doca_flow_crypto crypto_actions_ipv6 = {};
	crypto_actions_ipv6.has_crypto_encap = true;
	crypto_actions_ipv6.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
	crypto_actions_ipv6.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV6;
	crypto_actions_ipv6.crypto_encap.icv_size = PSP_ICV_SIZE;

	// In tunnel mode, we need to decap the eth/ip/udp/psp headers and add ethernet header
	// In transport mode, we only remove the udp/psp headers
	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		crypto_actions_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
		crypto_actions_ipv4.crypto_encap.data_size = sizeof(rte_ether_hdr);

		rte_ether_hdr *eth_hdr_ipv4 = (rte_ether_hdr *)crypto_actions_ipv4.crypto_encap.encap_data;
		eth_hdr_ipv4->ether_type = RTE_BE16(RTE_ETHER_TYPE_IPV4);
		eth_hdr_ipv4->src_addr = pf_dev->src_mac;
		eth_hdr_ipv4->dst_addr = app_config->dcap_dmac;

		crypto_actions_ipv6.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
		crypto_actions_ipv6.crypto_encap.data_size = sizeof(rte_ether_hdr);

		rte_ether_hdr *eth_hdr_ipv6 = (rte_ether_hdr *)crypto_actions_ipv6.crypto_encap.encap_data;
		eth_hdr_ipv6->ether_type = RTE_BE16(RTE_ETHER_TYPE_IPV6);
		eth_hdr_ipv6->src_addr = pf_dev->src_mac;
		eth_hdr_ipv6->dst_addr = app_config->dcap_dmac;
	}

	// Create 2 ordered lists (sequences) - one for IPv4, one for IPv6
	const int nb_ordered_lists = 2;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	struct doca_flow_ordered_list ordered_list_ipv4 = {};
	struct doca_flow_ordered_list ordered_list_ipv6 = {};

	// IPv4 sequence elements
	struct doca_flow_ordered_list_element elements_ipv4[1];
	elements_ipv4[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements_ipv4[0].crypto = &crypto_actions_ipv4;

	ordered_list_ipv4.idx = INGRESS_ACL_IPV4_SEQ_IDX;
	ordered_list_ipv4.size = 1;
	ordered_list_ipv4.elements = elements_ipv4;
	ordered_lists[0] = &ordered_list_ipv4;

	// IPv6 sequence elements
	struct doca_flow_ordered_list_element elements_ipv6[1];
	elements_ipv6[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements_ipv6[0].crypto = &crypto_actions_ipv6;

	ordered_list_ipv6.idx = INGRESS_ACL_IPV6_SEQ_IDX;
	ordered_list_ipv6.size = 1;
	ordered_list_ipv6.elements = elements_ipv6;
	ordered_lists[1] = &ordered_list_ipv6;

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = vf_port_id;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = syndrome_stats_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INGR_ACL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_acl_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	// Add default entries for both sequences
	IF_SUCCESS(result,
		   add_single_entry_ordered_list(0,
						 ingress_acl_pipe,
						 pf_dev->port_obj,
						 INGRESS_ACL_IPV4_SEQ_IDX,
						 &ordered_list_ipv4,
						 &fwd,
						 &default_ingr_acl_ipv4_entry));

	IF_SUCCESS(result,
		   add_single_entry_ordered_list(0,
						 ingress_acl_pipe,
						 pf_dev->port_obj,
						 INGRESS_ACL_IPV6_SEQ_IDX,
						 &ordered_list_ipv6,
						 &fwd,
						 &default_ingr_acl_ipv6_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::match_ingress_acl_pipe_create(bool is_ipv4)
{
	doca_error_t result = DOCA_SUCCESS;

	// Create match structure based on IP version
	doca_flow_match match = {};
	doca_flow_header_format *match_hdr = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.inner : &match.outer;

	if (!app_config->disable_ingress_acl) {
		if (is_ipv4) {
			match_hdr->l3_type = DOCA_FLOW_L3_TYPE_IP4;
			match.tun.type = DOCA_FLOW_TUN_PSP;
			match.tun.psp.spi = UINT32_MAX;
			match_hdr->ip4.src_ip = UINT32_MAX;
			match_hdr->ip4.dst_ip = UINT32_MAX;
		} else {
			match_hdr->l3_type = DOCA_FLOW_L3_TYPE_IP6;
			match.meta.u32[2] = UINT32_MAX;
			SET_IP6_ADDR(match_hdr->ip6.dst_ip, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
		}
	}

	doca_flow_fwd fwd_to_ordered_list = {};
	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = ingress_acl_pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = UINT32_MAX;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	doca_flow_pipe **target_pipe = is_ipv4 ? &match_ingress_acl_ipv4_pipe : &match_ingress_acl_ipv6_pipe;
	int nr_entries = app_config->disable_ingress_acl ? 1 : app_config->max_tunnels;

	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nr_entries));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_to_ordered_list, nullptr, target_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	if (app_config->disable_ingress_acl) {
		doca_flow_match match_no_syndrome = {};
		fwd_to_ordered_list.ordered_list_pipe.idx = is_ipv4 ? 0 : 1; // fwd to IPv4 sequence (idx 0) or IPv6
									     // sequence (idx 1)
		doca_flow_pipe_entry **target_entry = is_ipv4 ? &default_ingr_acl_ipv4_match_entry :
								&default_ingr_acl_ipv6_match_entry;

		IF_SUCCESS(result,
			   add_single_entry(0,
					    *target_pipe,
					    pf_dev->port_obj,
					    &match_no_syndrome,
					    0,
					    nullptr,
					    nullptr,
					    &fwd_to_ordered_list,
					    target_entry));
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

	return add_single_entry(0,
				ingress_src_ip6_pipe,
				pf_dev->port_obj,
				&match,
				0,
				&actions,
				nullptr,
				nullptr,
				nullptr);
}

doca_error_t PSP_GatewayFlows::add_ingress_acl_entry(psp_session_t *session, uint16_t queue_id)
{
	struct doca_flow_pipe *pipe;
	uint32_t ordered_list_idx = INGRESS_ACL_IPV4_SEQ_IDX;
	if (app_config->disable_ingress_acl) {
		DOCA_LOG_ERR("Cannot insert ingress ACL flow; disabled");
		return DOCA_ERROR_BAD_STATE;
	}

	doca_flow_match match = {};
	doca_flow_header_format *match_hdr = app_config->mode == PSP_GW_MODE_TUNNEL ? &match.inner : &match.outer;
	if (session->src_vip.type == DOCA_FLOW_L3_TYPE_IP4) {
		pipe = match_ingress_acl_ipv4_pipe;
		match.tun.type = DOCA_FLOW_TUN_PSP;
		match.tun.psp.spi = RTE_BE32(session->spi_ingress);
		match_hdr->l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match_hdr->ip4.src_ip = session->dst_vip.ipv4_addr; // use dst_vip of session as src for ingress
		match_hdr->ip4.dst_ip = session->src_vip.ipv4_addr; // use src_vip of session as dst for ingress
	} else {
		ordered_list_idx = INGRESS_ACL_IPV6_SEQ_IDX;
		pipe = match_ingress_acl_ipv6_pipe;
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

	// Set up forward to ordered list pipe using session->crypto_id as index
	doca_flow_fwd fwd_to_ordered_list = {};
	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = ingress_acl_pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = ordered_list_idx;

	doca_error_t result = DOCA_SUCCESS;
	IF_SUCCESS(result,
		   add_single_entry(queue_id,
				    pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd_to_ordered_list,
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
					    0,
					    nullptr,
					    &monitor_count,
					    nullptr,
					    &syndrome_stats_entries[i]));
	}

	return result;
}

doca_error_t PSP_GatewayFlows::egress_dst_ip6_pipe_create(void)
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
	fwd.next_pipe = match_egress_acl_ipv6_pipe;

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

doca_error_t PSP_GatewayFlows::egress_acl_pipe_create()
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(rss_pipe);
	assert(!sampling_enabled || egress_sampling_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_crypto crypto_actions = {};
	doca_flow_crypto crypto_encap_ipv4 = {};
	doca_flow_crypto crypto_encap_ipv6 = {};

	crypto_actions.has_crypto_encap = true;
	crypto_actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
	crypto_actions.crypto_encap.icv_size = PSP_ICV_SIZE;
	crypto_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
	crypto_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	crypto_actions.crypto.crypto_id = UINT32_MAX; // per entry

	crypto_encap_ipv6 = crypto_actions;
	crypto_encap_ipv4 = crypto_actions;

	if (app_config->mode == PSP_GW_MODE_TUNNEL) {
		crypto_encap_ipv6.crypto_encap.net_type = crypto_encap_ipv4.crypto_encap.net_type =
			DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
		crypto_encap_ipv6.crypto_encap.data_size = sizeof(eth_ipv6_psp_tunnel_hdr);
		crypto_encap_ipv4.crypto_encap.data_size = sizeof(eth_ipv4_psp_tunnel_hdr);
	} else {
		crypto_encap_ipv6.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV6;
		crypto_encap_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_OVER_IPV4;
		crypto_encap_ipv6.crypto_encap.data_size = crypto_encap_ipv4.crypto_encap.data_size =
			sizeof(udp_psp_transport_hdr);
	}

	if (!app_config->net_config.vc_enabled) {
		crypto_encap_ipv6.crypto_encap.data_size -= sizeof(uint64_t);
		crypto_encap_ipv4.crypto_encap.data_size -= sizeof(uint64_t);
	}
	memset(crypto_encap_ipv6.crypto_encap.encap_data, 0xff, crypto_encap_ipv6.crypto_encap.data_size);
	memset(crypto_encap_ipv4.crypto_encap.encap_data, 0xff, crypto_encap_ipv4.crypto_encap.data_size);

	// Create two ordered lists - one for IPv4 and one for IPv6
	struct doca_flow_ordered_list ordered_list_ipv4 = {};
	struct doca_flow_ordered_list ordered_list_ipv6 = {};
	struct doca_flow_ordered_list_element element_ipv4 = {};
	struct doca_flow_ordered_list_element element_ipv6 = {};

	element_ipv4.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	element_ipv4.crypto = &crypto_encap_ipv4;
	element_ipv6.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	element_ipv6.crypto = &crypto_encap_ipv6;

	struct doca_flow_ordered_list_element elements_ipv4[1];
	struct doca_flow_ordered_list_element elements_ipv6[1];
	memcpy(&elements_ipv4[0], &element_ipv4, sizeof(element_ipv4));
	memcpy(&elements_ipv6[0], &element_ipv6, sizeof(element_ipv6));

	ordered_list_ipv4.idx = 0;
	ordered_list_ipv4.size = 1;
	ordered_list_ipv4.elements = elements_ipv4;

	ordered_list_ipv6.idx = 1;
	ordered_list_ipv6.size = 1;
	ordered_list_ipv6.elements = elements_ipv6;

	const int nb_ordered_lists = 2;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	ordered_lists[0] = &ordered_list_ipv4;
	ordered_lists[1] = &ordered_list_ipv6;

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
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_ACL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, p_fwd, &fwd_miss, &egress_acl_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::match_egress_acl_pipe_create(bool is_ipv4)
{
	assert(egress_acl_pipe);
	doca_error_t result = DOCA_SUCCESS;

	// Create match structure based on IP version
	doca_flow_match match = {};
	if (is_ipv4) {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.outer.ip4.dst_ip = UINT32_MAX;
		match.outer.ip4.src_ip = UINT32_MAX;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		match.meta.u32[2] = UINT32_MAX;
		SET_IP6_ADDR(match.outer.ip6.src_ip, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
	}

	doca_flow_fwd fwd_to_ordered_list = {};
	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = egress_acl_pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = UINT32_MAX;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = rss_pipe;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	doca_flow_pipe **target_pipe = is_ipv4 ? &match_egress_acl_ipv4_pipe : &match_egress_acl_ipv6_pipe;
	const char *pipe_name = is_ipv4 ? "MATCH_EGR_ACL_IPV4" : "MATCH_EGR_ACL_IPV6";

	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_to_ordered_list, &fwd_miss, target_pipe));

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

	return add_single_entry(0, egress_dst_ip6_pipe, pf_dev->port_obj, &match, 0, &actions, nullptr, nullptr, nullptr);
}

doca_error_t PSP_GatewayFlows::add_encrypt_entry(psp_session_t *session, const void *encrypt_key, uint16_t queue_id)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	std::string dst_pip = ip_to_string(session->dst_pip);
	std::string src_vip = ip_to_string(session->src_vip);
	std::string dst_vip = ip_to_string(session->dst_vip);
	struct doca_flow_pipe *match_pipe;
	uint32_t egress_acl_entry_idx = session->crypto_id;

	DOCA_LOG_DBG("Creating encrypt flow entry: dst_pip %s, src_vip %s, dst_vip %s, SPI %d, crypto_id %d",
		     dst_pip.c_str(),
		     src_vip.c_str(),
		     dst_vip.c_str(),
		     session->spi_egress,
		     session->crypto_id);

	/* egress_acl ordered list entry */
	// Setup for shared resource
	struct doca_flow_shared_resource_cfg res_cfg = {};
	res_cfg.psp_cfg.key_cfg.key_type = session->psp_proto_ver == 0 ? DOCA_FLOW_CRYPTO_KEY_128 :
									 DOCA_FLOW_CRYPTO_KEY_256;
	res_cfg.psp_cfg.key_cfg.key = (uint32_t *)encrypt_key;

	result = doca_flow_port_shared_resource_set_cfg(pf_dev->port_obj,
							DOCA_FLOW_SHARED_RESOURCE_PSP,
							session->crypto_id,
							&res_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure crypto_id %d: %s", session->crypto_id, doca_error_get_descr(result));
		return result;
	}

	// Setup for crypto actions
	doca_flow_crypto crypto_actions = {};
	crypto_actions.has_crypto_encap = true;
	if (app_config->mode == PSP_GW_MODE_TRANSPORT)
		format_encap_transport_data(session, crypto_actions.crypto_encap.encap_data);
	else if (session->dst_pip.type == DOCA_FLOW_L3_TYPE_IP6)
		format_encap_tunnel_data_ipv6(session, crypto_actions.crypto_encap.encap_data);
	else
		format_encap_tunnel_data_ipv4(session, crypto_actions.crypto_encap.encap_data);
	crypto_actions.crypto.crypto_id = session->crypto_id;

	// Determine which ordered list to use based on IP type
	bool use_ipv6 = (app_config->mode == PSP_GW_MODE_TUNNEL && session->dst_pip.type == DOCA_FLOW_L3_TYPE_IP6) ||
			(app_config->mode == PSP_GW_MODE_TRANSPORT && session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP6);

	// Create ordered list element and list
	struct doca_flow_ordered_list_element element = {};
	element.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	element.crypto = &crypto_actions;

	struct doca_flow_ordered_list ordered_list = {};
	ordered_list.idx = use_ipv6 ? 1 : 0; // Use appropriate ordered list index
	ordered_list.size = 1;
	ordered_list.elements = &element;

	doca_flow_fwd fwd_to_sampling = {};
	fwd_to_sampling.type = DOCA_FLOW_FWD_PIPE;
	fwd_to_sampling.next_pipe = sampling_enabled ? set_sample_bit_pipe : nullptr;

	doca_flow_fwd fwd_to_wire = {};
	fwd_to_wire.type = DOCA_FLOW_FWD_PORT;
	fwd_to_wire.port_id = pf_dev->port_id;

	auto p_fwd = sampling_enabled ? &fwd_to_sampling : &fwd_to_wire;

	// Add entry to egress_acl ordered list using session->crypto_id as entry index
	result = add_single_entry_ordered_list(queue_id,
					       egress_acl_pipe,
					       pf_dev->port_obj,
					       egress_acl_entry_idx,
					       &ordered_list,
					       p_fwd,
					       nullptr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress_acl pipe entry: %s", doca_error_get_descr(result));
		return result;
	} else
		DOCA_LOG_DBG("Added egress_acl pipe entry");

	/* match pipe entry */
	// Setup for match pipe entry
	doca_flow_match encap_encrypt_match = {};
	doca_flow_fwd fwd_to_ordered_list = {};
	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = egress_acl_pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = egress_acl_entry_idx;

	int dst_vip_id = -1; // Initialize for potential IPv6 use
	if (session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP4) {
		match_pipe = match_egress_acl_ipv4_pipe;
		encap_encrypt_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		encap_encrypt_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		encap_encrypt_match.outer.ip4.src_ip = session->src_vip.ipv4_addr;
		encap_encrypt_match.outer.ip4.dst_ip = session->dst_vip.ipv4_addr;
	} else {
		match_pipe = match_egress_acl_ipv6_pipe;
		encap_encrypt_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		encap_encrypt_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		SET_IP6_ADDR(encap_encrypt_match.outer.ip6.src_ip,
			     session->src_vip.ipv6_addr[0],
			     session->src_vip.ipv6_addr[1],
			     session->src_vip.ipv6_addr[2],
			     session->src_vip.ipv6_addr[3]);

		dst_vip_id = rte_hash_lookup(app_config->ip6_table, session->dst_vip.ipv6_addr);
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
	}

	result = add_single_entry(queue_id,
				  match_pipe,
				  pf_dev->port_obj,
				  &encap_encrypt_match,
				  0,
				  nullptr,
				  nullptr,
				  &fwd_to_ordered_list,
				  &session->encap_encrypt_entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add match pipe entry: %s", doca_error_get_descr(result));
		return result;
	} else
		DOCA_LOG_DBG("Added match pipe session entry: %p", session->encap_encrypt_entry);

	/* egress_dst_ip6_entry */
	// Add IPv6 destination entry if needed
	if (session->dst_vip.type == DOCA_FLOW_L3_TYPE_IP6) {
		result = add_egress_dst_ip6_entry(session, dst_vip_id);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to add egress_dst_ip6_entry: %s", doca_error_get_descr(result));
		else
			DOCA_LOG_DBG("Added egress_dst_ip6_entry");
	}

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

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg = NULL;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_SAMPL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &egress_sampling_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	match.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	fwd.type = DOCA_FLOW_FWD_HASH_PIPE;
	fwd.hash_pipe.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING;
	fwd.hash_pipe.pipe = flooding_egress_wire_rss_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    egress_sampling_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    &monitor_count,
				    &fwd,
				    &egr_sampling_rss));

	match.tun.psp.s_d_ver_v = 0;
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = pf_dev->port_id;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    egress_sampling_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    &monitor_count,
				    &fwd,
				    &egr_sampling_wire));

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
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_is_root(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 4));
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
	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &arp_empty_pipe_entry));

	match.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_IPV6);
	// pkt_meta is already set as return_to_vf_indicator in previous entry
	// fwd.type is already set as DOCA_FLOW_FWD_PORT in previous entry
	// fwd.port_id is already set to vf_port_id in previous entry

	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ns_empty_pipe_entry));

	match.outer.eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	match.meta.pkt_meta = 0;
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = match_egress_acl_ipv4_pipe;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe,
				    pf_dev->port_obj,
				    &match,
				    0,
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
				    0,
				    nullptr,
				    nullptr,
				    &fwd,
				    &ipv6_empty_pipe_entry));
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
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "FWD_TO_RSS"));
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
				    0,
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
				    0,
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
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
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
				    0,
				    nullptr,
				    nullptr,
				    nullptr,
				    &set_sample_bit_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::empty_pipe_not_sampled_create(void)
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
				    0,
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
	fwd_ingress.next_pipe = match_ingress_decrypt_pipe;

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
						uint8_t action_idx,
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
						       action_idx,
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

doca_error_t PSP_GatewayFlows::add_single_flooding_entry(uint16_t pipe_queue,
							 doca_flow_pipe *pipe,
							 doca_flow_port *port,
							 uint32_t index,
							 const doca_flow_fwd *fwd,
							 doca_flow_pipe_entry **entry)
{
	int num_of_entries = 1;
	enum doca_flow_flags_type flags = DOCA_FLOW_NO_WAIT;

	app_config->status[pipe_queue] = entries_status();
	app_config->status[pipe_queue].entries_in_queue = num_of_entries;

	doca_error_t result = doca_flow_pipe_hash_add_entry(pipe_queue,
							    pipe,
							    index,
							    0,
							    nullptr,
							    nullptr,
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

doca_error_t PSP_GatewayFlows::add_single_entry_ordered_list(uint16_t pipe_queue,
							     doca_flow_pipe *pipe,
							     doca_flow_port *port,
							     uint32_t idx,
							     const struct doca_flow_ordered_list *ordered_list,
							     const struct doca_flow_fwd *fwd,
							     doca_flow_pipe_entry **entry)
{
	int num_of_entries = 1;
	doca_flow_flags_type flags = DOCA_FLOW_NO_WAIT;

	app_config->status[pipe_queue] = entries_status();
	app_config->status[pipe_queue].entries_in_queue = num_of_entries;

	doca_error_t result = doca_flow_pipe_ordered_list_add_entry(pipe_queue,
								    pipe,
								    idx,
								    ordered_list,
								    fwd,
								    flags,
								    &app_config->status[pipe_queue],
								    entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		return result;
	}
repeat:
	result = doca_flow_entries_process(port, pipe_queue, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entry: %s", doca_error_get_descr(result));
		return result;
	}

	if (app_config->status[pipe_queue].nb_processed != num_of_entries || app_config->status[pipe_queue].failure) {
		DOCA_LOG_ERR("Failed to process entry; nb_processed = %d, failure = %d",
			     app_config->status[pipe_queue].nb_processed,
			     app_config->status[pipe_queue].failure);
		if (app_config->status[pipe_queue].failure)
			return DOCA_ERROR_BAD_STATE;
		goto repeat;
	}

	return result;
}

struct PSP_GatewayFlows::pipe_query {
	doca_flow_pipe *pipe;	     // used to query misses
	doca_flow_pipe_entry *entry; // used to query static entries
	std::string name;	     // displays the pipe name
};

doca_error_t PSP_GatewayFlows::prepare_flooding_pipe(struct doca_flow_port *port,
						     enum doca_flow_pipe_domain domain,
						     struct doca_flow_pipe **pipe)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};
	doca_flow_match match_mask = {};
	doca_flow_pipe_cfg *pipe_cfg = NULL;
	struct doca_flow_fwd fwd = {};

	fwd.type = DOCA_FLOW_FWD_CHANGEABLE;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, port));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_HASH));
	IF_SUCCESS(result,
		   doca_flow_pipe_cfg_set_hash_map_algorithm(pipe_cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, domain));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "FWD_TO_FLOODING"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, pipe));
	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

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
	queries.emplace_back(
		pipe_query{ingress_inner_ip_classifier_pipe, ingress_ipv4_clasify_entry, "ingress_ipv4_clasify"});
	queries.emplace_back(
		pipe_query{ingress_inner_ip_classifier_pipe, ingress_ipv6_clasify_entry, "ingress_ipv6_clasify"});
	queries.emplace_back(pipe_query{ingress_sampling_classifier_pipe,
					default_ingr_sampling_ipv4_entry,
					"ingress_sampling_ipv4_pipe"});
	queries.emplace_back(pipe_query{ingress_sampling_classifier_pipe,
					default_ingr_sampling_ipv6_entry,
					"ingress_sampling_ipv6_pipe"});
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
	queries.emplace_back(pipe_query{egress_sampling_pipe, egr_sampling_rss, "egress_sampling_rss"});
	queries.emplace_back(pipe_query{egress_sampling_pipe, egr_sampling_wire, "egress_sampling_wire"});
	queries.emplace_back(pipe_query{nullptr, empty_pipe_entry, "arp_packets_intercepted"});
	queries.emplace_back(pipe_query{nullptr, fwd_ipv4_rss_entry, "fwd_ipv4_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, fwd_ipv6_rss_entry, "fwd_ipv6_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, ipv4_empty_pipe_entry, "fwd_egress_acl_ipv4"});
	queries.emplace_back(pipe_query{nullptr, ipv6_empty_pipe_entry, "fwd_egress_acl_ipv6"});
	queries.emplace_back(pipe_query{nullptr, fwd_ipv4_sample_entry, "sample_fwd_acl_ipv4"});
	queries.emplace_back(pipe_query{nullptr, fwd_ipv6_sample_entry, "sample_fwd_acl_ipv6"});
	queries.emplace_back(pipe_query{nullptr, ns_empty_pipe_entry, "ns_empty_pipe_entry"});
	queries.emplace_back(pipe_query{nullptr,
					flooding_ingress_inner_ipv4_classifier_entry,
					"flooding_ingress_inner_ipv4_classifier_entry"});
	queries.emplace_back(pipe_query{nullptr,
					flooding_ingress_inner_ipv6_classifier_entry,
					"flooding_ingress_inner_ipv6_classifier_entry"});
	queries.emplace_back(pipe_query{nullptr, flooding_ingress_rss_ipv4_entry, "flooding_ingress_rss_ipv4_entry"});
	queries.emplace_back(pipe_query{nullptr, flooding_ingress_rss_ipv6_entry, "flooding_ingress_rss_ipv6_entry"});
	queries.emplace_back(pipe_query{nullptr, flooding_egress_to_rss_entry, "flooding_egress_to_rss_entry"});
	queries.emplace_back(pipe_query{nullptr, flooding_egress_to_wire_entry, "flooding_egress_to_wire_entry"});

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
