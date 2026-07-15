/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>

#include <rte_ethdev.h>

#include <doca_flow.h>
#include <doca_bitfield.h>
#include <doca_log.h>

#include "upf_accel.h"
#include "upf_accel_pipeline.h"

DOCA_LOG_REGISTER(UPF_ACCEL::PIPELINE);

#define UPF_ACCEL_PSC_EXTENSION_CODE 0x85

#define UPF_ACCEL_BUILD_VNI(uint24_vni) (DOCA_HTOBE32((uint32_t)uint24_vni << 8)) /* Create VNI match field */

doca_error_t upf_accel_pipe_static_entry_add(struct upf_accel_ctx *upf_accel_ctx,
					     enum upf_accel_port port_id,
					     uint16_t pipe_queue,
					     struct doca_flow_pipe *pipe,
					     const struct doca_flow_match *match,
					     uint8_t action_idx,
					     const struct doca_flow_actions *actions,
					     const struct doca_flow_monitor *mon,
					     const struct doca_flow_fwd *fwd,
					     uint32_t flags,
					     void *usr_ctx,
					     struct doca_flow_pipe_entry **entry)
{
	const doca_error_t result =
		doca_flow_pipe_add_entry(pipe_queue, pipe, match, action_idx, actions, mon, fwd, flags, usr_ctx, entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add static entry: %s", doca_error_get_descr(result));
		return result;
	}
	upf_accel_ctx->num_static_entries[port_id]++;

	return result;
}

/*
 * Create a flow pipe
 *
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_create(struct upf_accel_pipe_cfg *pipe_cfg, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, pipe_cfg->port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, pipe_cfg->name, DOCA_FLOW_PIPE_BASIC, pipe_cfg->is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_domain(cfg, pipe_cfg->domain);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(cfg, pipe_cfg->num_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg num_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (pipe_cfg->match != NULL) {
		result = doca_flow_pipe_cfg_set_match(cfg, pipe_cfg->match, pipe_cfg->match_mask);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	if (pipe_cfg->mon != NULL) {
		result = doca_flow_pipe_cfg_set_monitor(cfg, pipe_cfg->mon);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	if (pipe_cfg->actions.num_actions) {
		result = doca_flow_pipe_cfg_set_actions(cfg,
							pipe_cfg->actions.action_list,
							NULL,
							pipe_cfg->actions.action_desc_list,
							pipe_cfg->actions.num_actions);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	result = doca_flow_pipe_create(cfg, pipe_cfg->fwd, pipe_cfg->fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create UPF accel pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/*
 * Create a flow pipe that drops the packets
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @drop_type [in]: Drop pipe type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_drop_create(struct upf_accel_ctx *upf_accel_ctx,
					       struct upf_accel_pipe_cfg *pipe_cfg,
					       enum upf_accel_pipe_drop_type drop_type)
{
	enum upf_accel_pipe_type drop_pipe_idx = upf_accel_drop_idx_get(pipe_cfg, drop_type);
	struct doca_flow_pipe **pipe = &upf_accel_ctx->pipes[pipe_cfg->port_id][drop_pipe_idx];
	struct doca_flow_monitor mon = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	uint8_t entry_idx = upf_accel_domain_idx_get(pipe_cfg->port_id, pipe_cfg->domain);
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_match match = {0};
	char *pipe_name = "DROP_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = &mon;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = NULL;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create drop pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 NULL,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 &upf_accel_ctx->drop_entries[drop_type][entry_idx]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add drop pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create the drop flow pipes
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_drops_create(struct upf_accel_ctx *upf_accel_ctx,
						struct upf_accel_pipe_cfg *pipe_cfg)
{
	doca_error_t result;
	int i;

	for (i = 0; i < UPF_ACCEL_DROP_NUM; i++) {
		result = upf_accel_pipe_drop_create(upf_accel_ctx, pipe_cfg, i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create drop pipe %d: %s", i, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create a flow pipe that sends the packets to SW
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @is_ul [in]: boolean to indicate whether this pipe is for UL (GTP) pkts
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_to_sw_create(struct upf_accel_ctx *upf_accel_ctx,
						struct upf_accel_pipe_cfg *pipe_cfg,
						bool is_ul,
						bool is_ipv6,
						struct doca_flow_pipe **pipe)
{
	const uint32_t outer_flags = is_ul ? 0 : (is_ipv6 ? DOCA_FLOW_RSS_IPV6_DST : DOCA_FLOW_RSS_IPV4_DST);
	const uint32_t inner_flags = is_ul ? (is_ipv6 ? DOCA_FLOW_RSS_IPV6_SRC : DOCA_FLOW_RSS_IPV4_SRC) : 0;
	uint16_t rss_queues[RTE_MAX_LCORE];
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_RSS,
				    .rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
				    .rss.outer_flags = outer_flags,
				    .rss.inner_flags = inner_flags,
				    .rss.queues_array = rss_queues,
				    .rss.nr_queues = upf_accel_ctx->num_queues - 1,
				    .rss.rss_hash_func = DOCA_FLOW_RSS_HASH_FUNCTION_TOEPLITZ};
	struct doca_flow_actions act_set_dir = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_actions *action_list[] = {&act_set_dir};
	uint16_t queue_id = 1; /* Queue 0 is reserved */
	struct doca_flow_match match = {0};
	char *pipe_name = "TO_SW_PIPE";
	doca_error_t result;
	int i;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = NULL;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	for (i = 0; i < upf_accel_ctx->num_queues; ++i)
		rss_queues[i] = queue_id++;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create to sw pipe: %s", doca_error_get_descr(result));
		return result;
	}

	act_set_dir.meta.pkt_meta = is_ul ? rte_cpu_to_be_32(UPF_ACCEL_META_PKT_DIR_UL) :
					    rte_cpu_to_be_32(UPF_ACCEL_META_PKT_DIR_DL);

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 NULL,
						 0,
						 &act_set_dir,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add to sw pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a TX root flow pipe.
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_tx_root_create(struct upf_accel_ctx *upf_accel_ctx,
						  struct upf_accel_pipe_cfg *pipe_cfg,
						  struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_TX_SHARED_METERS_START]};
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_match match = {0};
	char *pipe_name = "TX_ROOT_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = true;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tx root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 NULL,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add tx root pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that does encap and counter
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @fwd [in]: fwd upon hit
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_encap_counter_create(struct upf_accel_ctx *upf_accel_ctx,
							struct upf_accel_pipe_cfg *pipe_cfg,
							struct doca_flow_fwd *fwd,
							struct doca_flow_pipe **pipe)
{
	struct doca_flow_monitor mon = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED,
		.shared_counter.shared_counter_id = 0xffffffff,
	};
	struct doca_flow_match match_mask = {.meta.pkt_meta = rte_cpu_to_be_32(UPF_ACCEL_MAX_NUM_PDR - 1)};
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_actions act_encap_ip4_4g = {
		.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
		.encap_cfg.encap = {.outer = {.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4),
					      .l3_type = DOCA_FLOW_L3_TYPE_IP4,
					      .ip4 = {.src_ip = DOCA_HTOBE32(UPF_ACCEL_SRC_IP),
						      .dst_ip = UINT32_MAX,
						      .version_ihl = DOCA_HTOBE16(UPF_ACCEL_VERSION_IHL_IPV4),
						      .ttl = UPF_ACCEL_ENCAP_TTL,
						      .next_proto = DOCA_FLOW_PROTO_UDP},
					      .l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP,
					      .udp.l4_port = {.src_port = DOCA_HTOBE16(DOCA_FLOW_GTPU_DEFAULT_PORT - 2),
							      .dst_port = DOCA_HTOBE16(DOCA_FLOW_GTPU_DEFAULT_PORT)}},
				    .tun = {.type = DOCA_FLOW_TUN_GTPU, .gtp_teid = UINT32_MAX}}};
	struct doca_flow_actions *action_list[UPF_ACCEL_ENCAP_ACTION_NUM];
	const uint16_t src_ipv6[] = UPF_ACCEL_SRC_IPV6;
	const uint8_t src_mac[] = UPF_ACCEL_SRC_MAC;
	const uint8_t dst_mac[] = UPF_ACCEL_DST_MAC;
	struct doca_flow_actions act_encap_ip4_5g;
	struct doca_flow_actions act_encap_ip6_4g;
	struct doca_flow_actions act_encap_ip6_5g;
	struct doca_flow_actions act_none = {0};
	struct doca_flow_match match = {0};
	char *pipe_name = "ENCAP_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_PDR;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = &match_mask;
	pipe_cfg->mon = &mon;
	pipe_cfg->fwd = fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = UPF_ACCEL_ENCAP_ACTION_NUM;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	SET_MAC_ADDR(act_encap_ip4_4g.encap_cfg.encap.outer.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(act_encap_ip4_4g.encap_cfg.encap.outer.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);

	memcpy(&act_encap_ip6_4g, &act_encap_ip4_4g, sizeof(act_encap_ip4_4g));
	act_encap_ip6_4g.encap_cfg.encap.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV6);
	act_encap_ip6_4g.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	memcpy(act_encap_ip6_4g.encap_cfg.encap.outer.ip6.src_ip, src_ipv6, UPF_ACCEL_NUM_BYTES_IPV6);
	memset(act_encap_ip6_4g.encap_cfg.encap.outer.ip6.dst_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);

	memcpy(&act_encap_ip4_5g, &act_encap_ip4_4g, sizeof(act_encap_ip4_4g));
	act_encap_ip4_5g.encap_cfg.encap.tun.gtp_next_ext_hdr_type = UPF_ACCEL_PSC_EXTENSION_CODE;
	act_encap_ip4_5g.encap_cfg.encap.tun.gtp_ext_psc_qfi = UINT8_MAX;

	memcpy(&act_encap_ip6_5g, &act_encap_ip6_4g, sizeof(act_encap_ip6_4g));
	act_encap_ip6_5g.encap_cfg.encap.tun.gtp_next_ext_hdr_type = UPF_ACCEL_PSC_EXTENSION_CODE;
	act_encap_ip6_5g.encap_cfg.encap.tun.gtp_ext_psc_qfi = UINT8_MAX;

	action_list[UPF_ACCEL_ENCAP_ACTION_IPV4_4G] = &act_encap_ip4_4g;
	action_list[UPF_ACCEL_ENCAP_ACTION_IPV4_5G] = &act_encap_ip4_5g;
	action_list[UPF_ACCEL_ENCAP_ACTION_IPV6_4G] = &act_encap_ip6_4g;
	action_list[UPF_ACCEL_ENCAP_ACTION_IPV6_5G] = &act_encap_ip6_5g;
	action_list[UPF_ACCEL_ENCAP_ACTION_NONE] = &act_none;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that does vxlan encap
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_vxlan_encap_create(struct upf_accel_ctx *upf_accel_ctx,
						      struct upf_accel_pipe_cfg *pipe_cfg,
						      struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = pipe_cfg->port_id};
	struct doca_flow_actions act_encap = {
		.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
		.encap_cfg.is_l2 = true,
		.encap_cfg.encap = {.outer = {.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4),
					      .l3_type = DOCA_FLOW_L3_TYPE_IP4,
					      .ip4 = {.src_ip = DOCA_HTOBE32(UPF_ACCEL_SRC_IP),
						      .dst_ip = DOCA_HTOBE32(UPF_ACCEL_DST_IP),
						      .version_ihl = DOCA_HTOBE16(UPF_ACCEL_VERSION_IHL_IPV4),
						      .ttl = UPF_ACCEL_ENCAP_TTL,
						      .next_proto = DOCA_FLOW_PROTO_UDP},
					      .l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP,
					      .udp.l4_port.dst_port = DOCA_HTOBE16(RTE_VXLAN_DEFAULT_PORT)},
				    .tun = {.type = DOCA_FLOW_TUN_VXLAN, .vxlan_tun_id = UINT32_MAX}}};
	struct doca_flow_actions *action_list[] = {&act_encap};
	const uint8_t src_mac[] = UPF_ACCEL_SRC_MAC;
	const uint8_t dst_mac[] = UPF_ACCEL_DST_MAC;
	char *pipe_name = "VXLAN_ENCAP_PIPE";
	struct doca_flow_match match = {0};
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_VNIS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	SET_MAC_ADDR(match.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);

	SET_MAC_ADDR(act_encap.encap_cfg.encap.outer.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(act_encap.encap_cfg.encap.outer.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vxlan encap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that implements FAR
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_far_create(struct upf_accel_ctx *upf_accel_ctx,
					      struct upf_accel_pipe_cfg *pipe_cfg,
					      struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT,
				    .port_id = upf_accel_ctx->get_fwd_port(pipe_cfg->port_id)};
	struct doca_flow_action_desc ad_dec_ttl = {.type = DOCA_FLOW_ACTION_ADD,
						   .field_op.dst.field_string = "outer.ipv4.ttl"};
	struct doca_flow_action_descs ads = {.nb_action_desc = 1, .desc_array = &ad_dec_ttl};
	struct doca_flow_actions act_dec_ttl = {.outer.ip4.ttl = -1};
	struct doca_flow_action_descs *action_desc_list[] = {&ads};
	struct doca_flow_actions *action_list[] = {&act_dec_ttl};
	struct doca_flow_match match = {0};
	char *pipe_name = "FAR_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = action_desc_list;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create far pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 NULL,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add far pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that does decap
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_decap_create(struct upf_accel_ctx *upf_accel_ctx,
						struct upf_accel_pipe_cfg *pipe_cfg,
						struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_FAR]};
	struct doca_flow_actions act_decap = {.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
					      .decap_cfg.eth.type = UINT16_MAX};
	struct doca_flow_match match_mask = {.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV4 |
									  DOCA_FLOW_L3_META_IPV6};
	struct doca_flow_match match = {.parser_meta.inner_l3_type = UINT32_MAX};
	struct doca_flow_actions *action_list[] = {&act_decap};
	const uint8_t src_mac[] = UPF_ACCEL_SRC_MAC;
	const uint8_t dst_mac[] = UPF_ACCEL_DST_MAC;
	char *pipe_name = "DECAP_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 2;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = &match_mask;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	SET_MAC_ADDR(act_decap.decap_cfg.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(act_decap.decap_cfg.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV4;
	act_decap.decap_cfg.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 &act_decap,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add decap pipe IPv4 entry: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV6;
	act_decap.decap_cfg.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV6);
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 &act_decap,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add decap pipe IPv6 entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that does vxlan decap
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_vxlan_decap_create(struct upf_accel_ctx *upf_accel_ctx,
						      struct upf_accel_pipe_cfg *pipe_cfg,
						      struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_ULDL]};
	struct doca_flow_actions act_decap = {.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
					      .decap_cfg.is_l2 = true,
					      /* This assumes VXLAN uses IPv4 */
					      .decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4)};
	struct doca_flow_match match = {.outer = {.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP,
						  .udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_VXLAN_DEFAULT_PORT)},
					.tun = {.type = DOCA_FLOW_TUN_VXLAN, .vxlan_tun_id = UINT32_MAX}};
	struct doca_flow_actions *action_list[] = {&act_decap};
	const uint8_t src_mac[] = UPF_ACCEL_SRC_MAC;
	const uint8_t dst_mac[] = UPF_ACCEL_DST_MAC;
	char *pipe_name = "VXLAN_DECAP_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_VNIS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	SET_MAC_ADDR(match.inner.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);

	SET_MAC_ADDR(act_decap.decap_cfg.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(act_decap.decap_cfg.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vxlan decap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 NULL,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add vxlan decap pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that does meter color matching
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @green_fwd_pipe [in]: the pipe to forward as a green action
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_color_match_create(struct upf_accel_ctx *upf_accel_ctx,
						      struct upf_accel_pipe_cfg *pipe_cfg,
						      struct doca_flow_pipe *green_fwd_pipe,
						      struct doca_flow_pipe **pipe)
{
	enum upf_accel_pipe_type drop_pipe_idx = upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_RATE);
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE,
					 .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][drop_pipe_idx]};
	struct doca_flow_match match = {.parser_meta.meter_color = UINT8_MAX};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE};
	char *pipe_name = "COLOR_MATCH_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 2;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create color match pipe %s", doca_error_get_descr(result));
		return result;
	}

	fwd.next_pipe = green_fwd_pipe;
	match.parser_meta.meter_color = DOCA_FLOW_METER_COLOR_GREEN;

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 &fwd,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add green match pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	fwd.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][drop_pipe_idx];
	match.parser_meta.meter_color = DOCA_FLOW_METER_COLOR_RED;
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 &fwd,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add red match pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a flow pipe that has shared meter
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_shared_meter_create(struct upf_accel_ctx *upf_accel_ctx,
						       struct upf_accel_pipe_cfg *pipe_cfg,
						       struct doca_flow_pipe **pipe)
{
	struct doca_flow_monitor mon = {.meter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED,
					.shared_meter.shared_meter_id = UINT32_MAX};
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe =
			upf_accel_ctx->pipes[pipe_cfg->port_id][upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_DBG)]};
	struct doca_flow_match match_mask = {.meta.pkt_meta = rte_cpu_to_be_32(UPF_ACCEL_MAX_NUM_PDR - 1)};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE};
	char *pipe_name = "SHARED_METER_PIPE";
	struct doca_flow_match match = {0};
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_PDR;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = &match_mask;
	pipe_cfg->mon = &mon;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create shared meter pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create a shared meter chain, example:
 *
 * [Meter A] --(Hit) HasAnotherMeter--> [Color A] --Green--> [Meter B] --...
 *           |                             |
 *           |                             --Red--> [RateDrop]
 *           |                             |
 *           --(Hit) NoMoreMeters-->    [Color NoMoreMeters] --Green--> Out
 *           |
 *           --(Miss) [DebugDrop]
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @color_pipe_next_pipe [in]: the pipe to jump to after chain ends
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_meter_chain_create(struct upf_accel_ctx *upf_accel_ctx,
						      struct upf_accel_pipe_cfg *pipe_cfg,
						      struct doca_flow_pipe *color_pipe_next_pipe)
{
	doca_error_t result;
	int i;

	if (pipe_cfg->domain == DOCA_FLOW_PIPE_DOMAIN_DEFAULT) {
		DOCA_LOG_ERR("Meter chain cannot be created in ingress");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Create single NoMoreMeters pipe, jump to it if no more meters after each meters pipe. */
	result = upf_accel_pipe_color_match_create(
		upf_accel_ctx,
		pipe_cfg,
		color_pipe_next_pipe,
		&upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_TX_COLOR_MATCH_NO_MORE_METERS]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create color pipe NoMoreMeters: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = upf_accel_ctx->upf_accel_cfg->qers->num_qers - 1; i >= 0; --i) {
		result = upf_accel_pipe_shared_meter_create(
			upf_accel_ctx,
			pipe_cfg,
			&upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_TX_SHARED_METERS_START + i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create meter pipe num %d: %s", i, doca_error_get_descr(result));
			return result;
		}

		/*
		 * The following color match pipe is to continue to the next meters pipe, since
		 * the last meters pipe always points to NoMoreMeters for color match, we skip it.
		 */
		if (i == (int)(upf_accel_ctx->upf_accel_cfg->qers->num_qers - 1))
			continue;

		result = upf_accel_pipe_color_match_create(
			upf_accel_ctx,
			pipe_cfg,
			upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_TX_SHARED_METERS_START + i + 1],
			&upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_TX_COLOR_MATCH_START + i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create color pipe num %d: %s", i, doca_error_get_descr(result));
			return result;
		}
	}

	return result;
}

/*
 * Create an 8t match extension pipe
 * There is a limitation on the amount of matched fields
 * To adhere to this limitation long matches are split between different entries
 * on following pipes
 *
 * matches GTPU TEID, GTPU extension PSC QFI and inner src IP
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_8t_ext_create(struct upf_accel_ctx *upf_accel_ctx,
						 struct upf_accel_pipe_cfg *pipe_cfg,
						 struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {
		.tun = {.type = DOCA_FLOW_TUN_GTPU, .gtp_teid = UINT32_MAX, .gtp_ext_psc_qfi = UINT8_MAX},
		.inner = {.l3_type = DOCA_FLOW_L3_TYPE_IP6}};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE,
					 .next_pipe =
						 upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV6]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_8T_IPV6]};
	struct doca_flow_actions act_pdr2md = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_actions *action_list[] = {&act_pdr2md};
	char *pipe_name = "8T_EXT_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	memset(match.inner.ip6.src_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create an 8T extension pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create 8 tuple pipe
 * If one or more IP layers are IPv6 an 8t_ext pipe is created and
 * the 8t pipe forwards to it, creating a match chain
 * There is a limitation on the amount of matched fields
 * To adhere to this limitation long matches are split between different entries
 * on following pipes
 *
 * Inner and outer IPv4: matches outer src IP, GTP TEID, GTP extension PSC QFI, inner src IP,
 * dst IP, src port, dst port, transport layer protocol
 * Inner IPv6 and outer IPv4: outer src IP, inner dst IP, src port, dst port,
 * transport layer protocol and metadata.
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @inner_ip_version [in]: version of inner ip layer
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_8t_create(struct upf_accel_ctx *upf_accel_ctx,
					     struct upf_accel_pipe_cfg *pipe_cfg,
					     enum doca_flow_l3_type inner_ip_version,
					     struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {.outer = {.l3_type = DOCA_FLOW_L3_TYPE_IP4, .ip4.src_ip = UINT32_MAX},
					.inner = {.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP,
						  .udp.l4_port = {.src_port = UINT16_MAX, .dst_port = UINT16_MAX}}};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_DECAP]};
	struct doca_flow_monitor mon = {.aging_sec = upf_accel_ctx->upf_accel_cfg->hw_aging_time_sec};
	struct doca_flow_actions act_pdr2md = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE};
	struct doca_flow_actions *action_list[] = {&act_pdr2md};
	char *pipe_name = "8T_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = &mon;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	if (inner_ip_version == DOCA_FLOW_L3_TYPE_IP4) {
		match.tun.type = DOCA_FLOW_TUN_GTPU;
		match.tun.gtp_teid = UINT32_MAX;
		match.tun.gtp_ext_psc_qfi = UINT8_MAX;
		match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.inner.ip4.src_ip = UINT32_MAX;
		match.inner.ip4.dst_ip = UINT32_MAX;
		match.inner.ip4.next_proto = UINT8_MAX;

		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV4];
	} else if (inner_ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		match.meta.pkt_meta = UINT32_MAX;
		match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		memset(match.inner.ip6.dst_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);
		match.inner.ip6.next_proto = UINT8_MAX;

		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV6];
	} else {
		DOCA_LOG_ERR("Invalid inner IP version: %d", inner_ip_version);
		assert(0);
	}

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 8t pipe: %s", doca_error_get_descr(result));
		return result;
	}

	if (inner_ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		result = upf_accel_pipe_8t_ext_create(
			upf_accel_ctx,
			pipe_cfg,
			&upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_8T_IPV6_EXT]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create 8t ipv6 extension pipe: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return result;
}

/*
 * Create an 7t match extension pipe
 * There is a limitation on the amount of matched fields
 * To adhere to this limitation long matches are split between different entries
 * on following pipes
 *
 * matches GTPU TEID and inner src IP
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_7t_ext_create(struct upf_accel_ctx *upf_accel_ctx,
						 struct upf_accel_pipe_cfg *pipe_cfg,
						 struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {.tun = {.type = DOCA_FLOW_TUN_GTPU, .gtp_teid = UINT32_MAX},
					.inner = {.l3_type = DOCA_FLOW_L3_TYPE_IP6}};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE,
					 .next_pipe =
						 upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV6]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_7T_IPV6]};
	struct doca_flow_actions act_pdr2md = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_actions *action_list[] = {&act_pdr2md};
	char *pipe_name = "7T_EXT_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	memset(match.inner.ip6.src_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create an 7T extension pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create 7 tuple pipe
 * If one or more IP layers are IPv6 an 7t_ext pipe is created and
 * the 7t pipe forwards to it, creating a match chain
 * There is a limitation on the amount of matched fields
 * To adhere to this limitation long matches are split between different entries
 * on following pipes
 *
 * Inner and outer IPv4: matches outer src IP, GTP TEID, inner src IP, dst IP,
 * src port, dst port, transport layer protocol
 * Inner IPv6 and outer IPv4: outer src IP, inner dst IP, src port, dst port,
 * transport layer protocol and metadata.
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @inner_ip_version [in]: version of inner ip layer
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_7t_create(struct upf_accel_ctx *upf_accel_ctx,
					     struct upf_accel_pipe_cfg *pipe_cfg,
					     enum doca_flow_l3_type inner_ip_version,
					     struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {.outer = {.l3_type = DOCA_FLOW_L3_TYPE_IP4, .ip4.src_ip = UINT32_MAX},
					.inner = {.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP,
						  .udp.l4_port = {.src_port = UINT16_MAX, .dst_port = UINT16_MAX}}};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_DECAP]};
	struct doca_flow_monitor mon = {.aging_sec = upf_accel_ctx->upf_accel_cfg->hw_aging_time_sec};
	struct doca_flow_actions act_pdr2md = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE};
	struct doca_flow_actions *action_list[] = {&act_pdr2md};
	char *pipe_name = "7T_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = &mon;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	if (inner_ip_version == DOCA_FLOW_L3_TYPE_IP4) {
		match.tun.type = DOCA_FLOW_TUN_GTPU;
		match.tun.gtp_teid = UINT32_MAX;
		match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.inner.ip4.src_ip = UINT32_MAX;
		match.inner.ip4.dst_ip = UINT32_MAX;
		match.inner.ip4.next_proto = UINT8_MAX;

		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV4];
	} else if (inner_ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		match.meta.pkt_meta = UINT32_MAX;
		match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		memset(match.inner.ip6.dst_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);
		match.inner.ip6.next_proto = UINT8_MAX;

		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV6];
	} else {
		DOCA_LOG_ERR("Invalid inner IP version: %d", inner_ip_version);
		assert(0);
	}

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 7t pipe: %s", doca_error_get_descr(result));
		return result;
	}

	if (inner_ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		result = upf_accel_pipe_7t_ext_create(
			upf_accel_ctx,
			pipe_cfg,
			&upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_7T_IPV6_EXT]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create 7t ipv6 extension pipe: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return result;
}

/*
 * Create 5 tuple pipe 1st match extension pipe
 * There is a limitation on the amount of matched fields
 * To adhere to this limitation long matches are split between different entries
 * on following pipes
 * In extension pipes metadata is always matched, since the value is set by the
 * match pipe, and is used to direct to the only possibly correct entry
 *
 * Matches metadata and dst IP
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_5t_ext_create(struct upf_accel_ctx *upf_accel_ctx,
						 struct upf_accel_pipe_cfg *pipe_cfg,
						 struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE,
					 .next_pipe =
						 upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_DL_TO_SW_IPV6]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_5T_IPV6]};
	struct doca_flow_actions act_pdr2md = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_actions *action_list[] = {&act_pdr2md};
	char *pipe_name = "5T_EXT_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	memset(match.outer.ip6.dst_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 5t extension pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create 5 tuple pipe
 * If one or more IP layers are IPv6 an 5t_match_ext pipe is created and
 * the 5t pipe forwards to it, creating a match chain
 * There is a limitation on the amount of matched fields
 * To adhere to this limitation long matches are split between different entries
 * on following pipes
 *
 * IPv4: matches src IP, dst IP, src port, dst port, transport layer protocol
 * IPv6: matches src IP, src port, dst port, transport layer protocol, and
 * forward to 5t_match_ext
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @ip_version [in]: IP version for the pipe to match on
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_5t_create(struct upf_accel_ctx *upf_accel_ctx,
					     struct upf_accel_pipe_cfg *pipe_cfg,
					     enum doca_flow_l3_type ip_version,
					     struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {.outer = {.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP,
						  .udp.l4_port = {.dst_port = UINT16_MAX, .src_port = UINT16_MAX}}};
	struct doca_flow_monitor mon = {.aging_sec = upf_accel_ctx->upf_accel_cfg->hw_aging_time_sec};
	struct doca_flow_actions act_pdr2md = {.meta.pkt_meta = UINT32_MAX};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_FAR]};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE};
	struct doca_flow_actions *action_list[] = {&act_pdr2md};
	char *pipe_name = "5T_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = &mon;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 1;
	pipe_cfg->actions.action_list = action_list;
	pipe_cfg->actions.action_desc_list = NULL;

	if (ip_version == DOCA_FLOW_L3_TYPE_IP4) {
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.outer.ip4.dst_ip = UINT32_MAX;
		match.outer.ip4.src_ip = UINT32_MAX;
		match.outer.ip4.next_proto = UINT8_MAX;

		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_DL_TO_SW_IPV4];
	} else if (ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		match.meta.pkt_meta = UINT32_MAX;
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		memset(match.outer.ip6.src_ip, 0xff, UPF_ACCEL_NUM_BYTES_IPV6);
		match.outer.ip6.next_proto = UINT8_MAX;

		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_DL_TO_SW_IPV6];
	} else {
		DOCA_LOG_ERR("Invalid inner IP version: %d", ip_version);
		assert(0);
	}

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 5t pipe: %s", doca_error_get_descr(result));
		return result;
	}

	if (ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		result = upf_accel_pipe_5t_ext_create(
			upf_accel_ctx,
			pipe_cfg,
			&upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_5T_IPV6_EXT]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create 5t ipv6 extension pipe: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return result;
}

/*
 * Create inner ip type match pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @is_8t [out]: indicator for extended gtp packets
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_inner_ip_type_create(struct upf_accel_ctx *upf_accel_ctx,
							struct upf_accel_pipe_cfg *pipe_cfg,
							bool is_8t,
							struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {.parser_meta.inner_l3_type = UINT32_MAX};
	struct doca_flow_match match_mask = {.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV4 |
									  DOCA_FLOW_L3_META_IPV6};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE};

	char *pipe_name = "INNER_IP_TYPE_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = &match_mask;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	if (is_8t) {
		fwd.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_8T_IPV4];
		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_8T_IPV6_EXT];
	} else {
		fwd.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_7T_IPV4];
		fwd_miss.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_7T_IPV6_EXT];
	}

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create inner ip type pipe: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.inner_l3_type = DOCA_FLOW_L3_META_IPV4;
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 &fwd,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add inner ip type pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create outer ip type match pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_outer_ip_type_create(struct upf_accel_ctx *upf_accel_ctx,
							struct upf_accel_pipe_cfg *pipe_cfg,
							struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_5T_IPV4]};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE,
					 .next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_5T_IPV6]};
	struct doca_flow_match match = {.parser_meta.outer_l3_type = UINT32_MAX};
	struct doca_flow_match match_mask = {.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4 |
									  DOCA_FLOW_L3_META_IPV6};

	char *pipe_name = "OUTER_IP_TYPE_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = &match_mask;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create outer ip type pipe: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 &fwd,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add outer ip type pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create GTP extension match pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_ext_gtp_create(struct upf_accel_ctx *upf_accel_ctx,
						  struct upf_accel_pipe_cfg *pipe_cfg,
						  struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE,
				    .next_pipe =
					    upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_8T_INNER_IP_TYPE]};
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_7T_INNER_IP_TYPE]};
	struct doca_flow_match match = {
		.tun = {.type = DOCA_FLOW_TUN_GTPU, .gtp_next_ext_hdr_type = UPF_ACCEL_PSC_EXTENSION_CODE}};

	char *pipe_name = "EXTENDED_GTP_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create extended gtp pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add extended gtp pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create UL/DL demux pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_uldl_create(struct upf_accel_ctx *upf_accel_ctx,
					       struct upf_accel_pipe_cfg *pipe_cfg,
					       struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][UPF_ACCEL_PIPE_5T_OUTER_IP_TYPE]};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE};
	uint32_t fixed_port = upf_accel_ctx->upf_accel_cfg->fixed_port;
	struct doca_flow_fwd *fwd_miss_ptr;
	struct doca_flow_match match = {0};
	char *pipe_name = "ULDL_PIPE";
	uint32_t fwd_pipe_idx;
	doca_error_t result;

	if (fixed_port == (uint32_t)UPF_ACCEL_FIXED_PORT_NONE) {
		match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
		match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_GTPU_DEFAULT_PORT);
		fwd_pipe_idx = UPF_ACCEL_PIPE_EXT_GTP;
		fwd_miss_ptr = &fwd_miss;
	} else {
		fwd_pipe_idx = (pipe_cfg->port_id == fixed_port) ? UPF_ACCEL_PIPE_EXT_GTP :
								   UPF_ACCEL_PIPE_5T_OUTER_IP_TYPE;
		fwd_miss_ptr = NULL;
	}

	fwd.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id][fwd_pipe_idx];

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = false;
	pipe_cfg->num_entries = 1;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = fwd_miss_ptr;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create uldl pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 NULL,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add uldl pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create root pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @pipe_cfg [in]: UPF Acceleration pipe configuration
 * @fwd_pipe [in]: the pipe to forward to if ttl > 1
 * @pipe [out]: pointer to store the created pipe at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_root_create(struct upf_accel_ctx *upf_accel_ctx,
					       struct upf_accel_pipe_cfg *pipe_cfg,
					       struct doca_flow_pipe *fwd_pipe,
					       struct doca_flow_pipe **pipe)
{
	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = upf_accel_ctx->pipes[pipe_cfg->port_id]
						 [upf_accel_drop_idx_get(pipe_cfg, UPF_ACCEL_DROP_FILTER)]};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = fwd_pipe};
	struct doca_flow_match match = {.outer = {.l3_type = DOCA_FLOW_L3_TYPE_IP4, .ip4.ttl = UINT8_MAX}};
	char *pipe_name = "ROOT_PIPE";
	doca_error_t result;

	pipe_cfg->name = pipe_name;
	pipe_cfg->is_root = true;
	pipe_cfg->num_entries = 2;
	pipe_cfg->match = &match;
	pipe_cfg->match_mask = NULL;
	pipe_cfg->mon = NULL;
	pipe_cfg->fwd = &fwd;
	pipe_cfg->fwd_miss = &fwd_miss;
	pipe_cfg->actions.num_actions = 0;

	result = upf_accel_pipe_create(pipe_cfg, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	match.outer.ip4.ttl = 0;
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ttl pipe entry 0: %s", doca_error_get_descr(result));
		return result;
	}

	match.outer.ip4.ttl = 1;
	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 pipe_cfg->port_id,
						 0,
						 *pipe,
						 &match,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[pipe_cfg->port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ttl pipe entry 1: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create RX pipeline
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @port_id [in]: Port ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipeline_rx_create(struct upf_accel_ctx *upf_accel_ctx, enum upf_accel_port port_id)
{
	struct upf_accel_pipe_cfg pipe_cfg = {
		.port_id = port_id,
		.port = upf_accel_ctx->ports[port_id],
		.domain = DOCA_FLOW_PIPE_DOMAIN_DEFAULT,
	};
	struct doca_flow_pipe *root_fwd_pipe;
	doca_error_t result;

	result = upf_accel_pipe_drops_create(upf_accel_ctx, &pipe_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx drop pipes: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_to_sw_create(upf_accel_ctx,
					     &pipe_cfg,
					     false,
					     false,
					     &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_DL_TO_SW_IPV4]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx DL-to-sw pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_to_sw_create(upf_accel_ctx,
					     &pipe_cfg,
					     true,
					     false,
					     &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV4]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx UL-to-sw pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_to_sw_create(upf_accel_ctx,
					     &pipe_cfg,
					     false,
					     true,
					     &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_DL_TO_SW_IPV6]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx DL-to-sw pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_to_sw_create(upf_accel_ctx,
					     &pipe_cfg,
					     true,
					     true,
					     &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_UL_TO_SW_IPV6]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx UL-to-sw pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_far_create(upf_accel_ctx,
					   &pipe_cfg,
					   &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_FAR]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx far pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_decap_create(upf_accel_ctx,
					     &pipe_cfg,
					     &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_DECAP]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx decap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_5t_create(upf_accel_ctx,
					  &pipe_cfg,
					  DOCA_FLOW_L3_TYPE_IP4,
					  &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_5T_IPV4]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 5t inner IPv4 pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_5t_create(upf_accel_ctx,
					  &pipe_cfg,
					  DOCA_FLOW_L3_TYPE_IP6,
					  &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_5T_IPV6]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 5t inner IPv6 pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_outer_ip_type_create(
		upf_accel_ctx,
		&pipe_cfg,
		&upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_5T_OUTER_IP_TYPE]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 5t outer ip type pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_7t_create(upf_accel_ctx,
					  &pipe_cfg,
					  DOCA_FLOW_L3_TYPE_IP4,
					  &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_7T_IPV4]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 7t inner IPv4 pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_7t_create(upf_accel_ctx,
					  &pipe_cfg,
					  DOCA_FLOW_L3_TYPE_IP6,
					  &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_7T_IPV6]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 7t inner IPv6 pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_inner_ip_type_create(
		upf_accel_ctx,
		&pipe_cfg,
		false,
		&upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_7T_INNER_IP_TYPE]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 7t inner ip type pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_8t_create(upf_accel_ctx,
					  &pipe_cfg,
					  DOCA_FLOW_L3_TYPE_IP4,
					  &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_8T_IPV4]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 8t inner IPv4 pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_8t_create(upf_accel_ctx,
					  &pipe_cfg,
					  DOCA_FLOW_L3_TYPE_IP6,
					  &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_8T_IPV6]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 8t inner IPv6 pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_inner_ip_type_create(
		upf_accel_ctx,
		&pipe_cfg,
		true,
		&upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_8T_INNER_IP_TYPE]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create 8t inner ip type pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_ext_gtp_create(upf_accel_ctx,
					       &pipe_cfg,
					       &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_EXT_GTP]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ext-gtp pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_uldl_create(upf_accel_ctx,
					    &pipe_cfg,
					    &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_ULDL]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create uldl pipe: %s", doca_error_get_descr(result));
		return result;
	}
	root_fwd_pipe = upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_ULDL];

	if (upf_accel_ctx->upf_accel_cfg->vxlan_config_file_path) {
		result = upf_accel_pipe_vxlan_decap_create(
			upf_accel_ctx,
			&pipe_cfg,
			&upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_RX_VXLAN_DECAP]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create rx vxlan decap pipe: %s", doca_error_get_descr(result));
			return result;
		}
		root_fwd_pipe = upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_RX_VXLAN_DECAP];
	}

	result = upf_accel_pipe_root_create(upf_accel_ctx,
					    &pipe_cfg,
					    root_fwd_pipe,
					    &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_RX_ROOT]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Create TX pipeline
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @port_id [in]: Port ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipeline_tx_create(struct upf_accel_ctx *upf_accel_ctx, enum upf_accel_port port_id)
{
	struct upf_accel_pipe_cfg pipe_cfg = {
		.port_id = port_id,
		.port = upf_accel_ctx->ports[pipe_cfg.port_id],
		.domain = DOCA_FLOW_PIPE_DOMAIN_EGRESS,
	};
	struct doca_flow_fwd counter_fwd;
	doca_error_t result;

	result = upf_accel_pipe_drops_create(upf_accel_ctx, &pipe_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tx drop pipes: %s", doca_error_get_descr(result));
		return result;
	}

	if (upf_accel_ctx->upf_accel_cfg->vxlan_config_file_path) {
		result = upf_accel_pipe_vxlan_encap_create(
			upf_accel_ctx,
			&pipe_cfg,
			&upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_TX_VXLAN_ENCAP]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create tx vxlan encap pipe: %s", doca_error_get_descr(result));
			return result;
		}
		counter_fwd.type = DOCA_FLOW_FWD_PIPE;
		counter_fwd.next_pipe = upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_TX_VXLAN_ENCAP];
	} else {
		counter_fwd.type = DOCA_FLOW_FWD_PORT;
		counter_fwd.port_id = pipe_cfg.port_id;
	}

	result =
		upf_accel_pipe_encap_counter_create(upf_accel_ctx,
						    &pipe_cfg,
						    &counter_fwd,
						    &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_TX_COUNTER]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tx encap pipe: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_meter_chain_create(upf_accel_ctx,
						   &pipe_cfg,
						   upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_TX_COUNTER]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tx meter_chain: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_pipe_tx_root_create(upf_accel_ctx,
					       &pipe_cfg,
					       &upf_accel_ctx->pipes[pipe_cfg.port_id][UPF_ACCEL_PIPE_TX_ROOT]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create tx root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Insert entry to a vxlan encap and a vxlan decap pipes
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @vxlan [in]: VXLAN decoded struct
 * @port_id [in]: port ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_vxlan_insert(struct upf_accel_ctx *upf_accel_ctx,
						const struct upf_accel_vxlan *vxlan,
						enum upf_accel_port port_id)
{
	struct doca_flow_actions act_encap = {.encap_cfg.encap.tun.vxlan_tun_id = UPF_ACCEL_BUILD_VNI(vxlan->vni)};
	struct doca_flow_match match = {0};
	doca_error_t result;

	SET_MAC_ADDR(match.outer.eth.dst_mac,
		     vxlan->mac[0],
		     vxlan->mac[1],
		     vxlan->mac[2],
		     vxlan->mac[3],
		     vxlan->mac[4],
		     vxlan->mac[5]);

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 port_id,
						 0,
						 upf_accel_ctx->pipes[port_id][UPF_ACCEL_PIPE_TX_VXLAN_ENCAP],
						 &match,
						 0,
						 &act_encap,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add vxlan encap entry: %s", doca_error_get_descr(result));
		return result;
	}

	/* Nullify outer dst mac before using match again for vxlan decap pipe entry */
	SET_MAC_ADDR(match.outer.eth.dst_mac, 0, 0, 0, 0, 0, 0);
	match.tun.vxlan_tun_id = UPF_ACCEL_BUILD_VNI(vxlan->vni);
	SET_MAC_ADDR(match.inner.eth.dst_mac,
		     vxlan->mac[0],
		     vxlan->mac[1],
		     vxlan->mac[2],
		     vxlan->mac[3],
		     vxlan->mac[4],
		     vxlan->mac[5]);

	result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
						 port_id,
						 0,
						 upf_accel_ctx->pipes[port_id][UPF_ACCEL_PIPE_RX_VXLAN_DECAP],
						 &match,
						 0,
						 NULL,
						 NULL,
						 NULL,
						 0,
						 &upf_accel_ctx->static_entry_ctx[port_id],
						 NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add vxlan decap entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Add all VXLAN related rules
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_vxlan_rules_add(struct upf_accel_ctx *upf_accel_ctx)
{
	const struct upf_accel_vxlans *vxlans = upf_accel_ctx->upf_accel_cfg->vxlans;
	const size_t num_vxlans = vxlans->num_vxlans;
	const struct upf_accel_vxlan *vxlan;
	enum upf_accel_port port_id;
	doca_error_t result;
	uint32_t vxlan_idx;

	for (vxlan_idx = 0; vxlan_idx < num_vxlans; vxlan_idx++) {
		vxlan = &vxlans->arr_vxlans[vxlan_idx];

		for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
			result = upf_accel_pipe_vxlan_insert(upf_accel_ctx, vxlan, port_id);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add on port %u vxlan rules for vxlan idx %u: %s",
					     port_id,
					     vxlan_idx,
					     doca_error_get_descr(result));
				return result;
			}
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t upf_accel_pipeline_create(struct upf_accel_ctx *upf_accel_ctx)
{
	enum upf_accel_port port_id;
	doca_error_t result;

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		result = upf_accel_pipeline_rx_create(upf_accel_ctx, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create rx pipeline in port %d: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}

		result = upf_accel_pipeline_tx_create(upf_accel_ctx, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create tx pipeline in port %d: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
	}

	if (upf_accel_ctx->upf_accel_cfg->vxlan_config_file_path) {
		result = upf_accel_vxlan_rules_add(upf_accel_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add vxlan rules: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return result;
}
