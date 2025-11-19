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
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>

#include <doca_flow.h>
#include <doca_log.h>
#include <doca_bitfield.h>

#include "flow_decrypt.h"

DOCA_LOG_REGISTER(IPSEC_SECURITY_GW::flow_decrypt);

#define DECAP_MAC_TYPE_IDX 12	   /* index in decap raw data for inner l3 type */
#define DECAP_IDX_SRC_MAC 6	   /* index in decap raw data for source mac */
#define DECAP_MARKER_HEADER_SIZE 8 /* non-ESP marker header size */
#define UDP_DST_PORT_FOR_ESP 4500  /* the udp dest port will be 4500 when next header is ESP */
#define CALC_DISTANCE(S, W) ((((uint64_t)1 << 32) + (S) - (W)) & (((uint64_t)1 << 32) - 1))
#define MAX_DISTANCE 0x80000000

/*
 * Create ipsec decrypt pipe with shared IPSEC decryption object
 *
 * @port [in]: port of the pipe
 * @expected_entries [in]: expected number of entries in the pipe
 * @app_cfg [in]: application configuration struct
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_decrypt_pipe(struct doca_flow_port *port,
					      int expected_entries,
					      struct ipsec_security_gw_config *app_cfg,
					      struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_crypto crypto_actions = {0};
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_actions meta_actions = {};
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element elements[2] = {0};
	const int nb_ordered_lists = 1;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	doca_error_t result;

	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	crypto_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
	crypto_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
	crypto_actions.crypto.ipsec_sa.sn_en = !app_cfg->sw_antireplay;
	crypto_actions.crypto.crypto_id = UINT32_MAX;

	meta_actions.meta.pkt_meta = 0xffffffff;

	elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements[0].crypto = &crypto_actions;
	elements[1].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements[1].actions = &meta_actions;

	ordered_list.idx = 0;
	ordered_list.size = 2;
	ordered_list.elements = elements;

	ordered_lists[0] = &ordered_list;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "DECRYPT_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg miss_counter: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg ordered_lists: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = app_cfg->decrypt_pipes.match_decap_pipe.pipe;

	if (app_cfg->debug_mode) {
		fwd_miss.type = DOCA_FLOW_FWD_PIPE;
		fwd_miss.next_pipe = app_cfg->decrypt_pipes.bad_syndrome_pipe.pipe;
	} else {
		fwd_miss.type = DOCA_FLOW_FWD_DROP;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decrypt ordered list pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create ipsec decrypt match pipe with ESP header match that forwards to ordered list pipe
 *
 * @port [in]: port of the pipe
 * @expected_entries [in]: expected number of entries in the pipe
 * @l3_type [in]: DOCA_FLOW_L3_TYPE_IP4 / DOCA_FLOW_L3_TYPE_IP6
 * @ordered_list_pipe [in]: ordered list pipe to forward to
 * @app_cfg [in]: application configuration struct
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_decrypt_match_pipe(struct doca_flow_port *port,
						    int expected_entries,
						    enum doca_flow_l3_type l3_type,
						    struct doca_flow_pipe *ordered_list_pipe,
						    struct ipsec_security_gw_config *app_cfg,
						    struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	match.outer.l3_type = l3_type;
	if (l3_type == DOCA_FLOW_L3_TYPE_IP4)
		match.outer.ip4.dst_ip = 0xffffffff;
	else
		SET_IP6_ADDR(match.outer.ip6.dst_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);

	/* the control pipe coming before will verify the existence of a ESP header */
	match.tun.type = DOCA_FLOW_TUN_ESP;
	match.tun.esp_spi = 0xffffffff;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "DECRYPT_MATCH_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ordered_list_pipe;
	fwd.ordered_list_pipe.idx = 0xffffffff; /* changeable per entry */

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decrypt match pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(expected_entries,
								     sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create the DOCA Flow forward struct based on the user configuration
 *
 * @app_cfg [in]: application configuration struct
 * @rss_queues [in]: rss queues array to fill in case of rss forward
 * @fwd [out]: the created forward struct
 */
static void get_bad_syndrome_pipe_fwd(struct ipsec_security_gw_config *app_cfg,
				      uint16_t *rss_queues,
				      uint32_t flags,
				      struct doca_flow_fwd *fwd)
{
	uint32_t nb_queues = app_cfg->dpdk_config->port_config.nb_queues;
	uint32_t i;

	memset(fwd, 0, sizeof(*fwd));

	if (app_cfg->syndrome_fwd == IPSEC_SECURITY_GW_FWD_SYNDROME_RSS) {
		/* for software handling the packets will be sent to the application by RSS queues */
		if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
			fwd->type = DOCA_FLOW_FWD_PIPE;
			fwd->next_pipe = app_cfg->switch_pipes.rss_pipe.pipe;
		} else {
			fwd->type = DOCA_FLOW_FWD_RSS;
			fwd->rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
			if (rss_queues == NULL) {
				fwd->rss.nr_queues = -1;
				return;
			}
			/* when we reach the bad syndrome pipe, decrypt might have failed (authentication syndrome), so
			 * there is no inner data */
			fwd->rss.outer_flags = flags;
			fwd->rss.queues_array = rss_queues;
			fwd->rss.nr_queues = nb_queues - 1;
			for (i = 0; i < nb_queues - 1; i++)
				rss_queues[i] = i + 1;
		}
	} else {
		fwd->type = DOCA_FLOW_FWD_DROP;
	}
}

/*
 * Create pipe that match on spi, IP, and bad syndromes, each entry will have a counter
 *
 * @app_cfg [in]: application configuration struct
 * @port [in]: port of the pipe
 * @expected_entries [in]: expected number of entries in the pipe
 * @pipe [out]: the created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_bad_syndrome_pipe(struct ipsec_security_gw_config *app_cfg,
					     struct doca_flow_port *port,
					     int expected_entries,
					     struct doca_flow_pipe **pipe)
{
	int nb_actions = 1;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_pipe_cfg *pipe_cfg;
	union security_gateway_pkt_meta meta = {0};
	struct doca_flow_actions actions, *actions_arr[nb_actions];
	struct doca_flow_actions actions_mask, *actions_mask_arr[nb_actions];
	union security_gateway_pkt_meta actions_meta = {0};

	doca_error_t result;

	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));
	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&actions_mask, 0, sizeof(actions_mask));

	/* ipsec syndrome */
	match_mask.parser_meta.ipsec_syndrome = 0xff;
	match.parser_meta.ipsec_syndrome = 0xff;
	/* Anti replay syndrome */
	match_mask.parser_meta.ipsec_ar_syndrome = 0xff;
	match.parser_meta.ipsec_ar_syndrome = 0xff;

	/* when we reach the bad syndrome pipe, decrypt might have failed (authentication syndrome), so there is no
	 * inner data */
	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;

	/* rule index */
	meta.rule_id = -1;
	match_mask.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	match.meta.pkt_meta = 0xffffffff;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "bad_syndrome");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries * NUM_OF_SYNDROMES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (app_cfg->syndrome_fwd == IPSEC_SECURITY_GW_FWD_SYNDROME_RSS) {
		actions.meta.pkt_meta = 0xffffffff;
		actions_meta.decrypt_syndrome = -1;
		actions_meta.antireplay_syndrome = -1;
		actions_mask.meta.pkt_meta = DOCA_HTOBE32(actions_meta.u32);
		actions_arr[0] = &actions;
		actions_mask_arr[0] = &actions_mask;
		result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_mask_arr, NULL, nb_actions);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	get_bad_syndrome_pipe_fwd(app_cfg, NULL, 0, &fwd);

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create decrypt pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add 4 entries for the rule, each entry match the same IP and spi, and different syndrome
 *
 * @pipe [in]: pipe to add the entries
 * @rule [in]: rule of the entries - spi and ip address
 * @rule_id [in]: rule idx to match on
 * @decrypt_status [in]: user ctx
 * @flags [in]: wait for batch / no wait flag for the last entry
 * @queue_id [in]: queue id
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_bad_syndrome_pipe_entry(struct ipsec_security_gw_config *app_cfg,
						struct doca_flow_pipe *pipe,
						struct decrypt_rule *rule,
						uint32_t rule_id,
						struct entries_status *decrypt_status,
						enum doca_flow_flags_type flags,
						int queue_id)
{
	uint16_t rss_queues[app_cfg->dpdk_config->port_config.nb_queues];
	uint32_t rss_flags;
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	union security_gateway_pkt_meta actions_meta = {0};
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	meta.rule_id = rule_id;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	match.parser_meta.ipsec_syndrome = 1;
	match.parser_meta.ipsec_ar_syndrome = 0;

	match.parser_meta.outer_l3_type = rule->l3_type == DOCA_FLOW_L3_TYPE_IP4 ? DOCA_FLOW_L3_META_IPV4 :
										   DOCA_FLOW_L3_META_IPV6;
	rss_flags = rule->l3_type == DOCA_FLOW_L3_TYPE_IP4 ? DOCA_FLOW_RSS_IPV4 : DOCA_FLOW_RSS_IPV6;

	actions_meta.decrypt_syndrome = 1;
	actions_meta.antireplay_syndrome = 0;
	actions.meta.pkt_meta = DOCA_HTOBE32(actions_meta.u32);

	get_bad_syndrome_pipe_fwd(app_cfg, rss_queues, rss_flags, &fwd);

	/* match on ipsec syndrome 1 */
	result = doca_flow_pipe_add_entry(queue_id,
					  pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  decrypt_status,
					  &rule->entries[0].entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add bad syndrome pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.ipsec_syndrome = 2;

	actions_meta.decrypt_syndrome = 2;
	actions.meta.pkt_meta = DOCA_HTOBE32(actions_meta.u32);

	/* match on ipsec syndrome 2 */
	result = doca_flow_pipe_add_entry(queue_id,
					  pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  decrypt_status,
					  &rule->entries[1].entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add bad syndrome pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.ipsec_syndrome = 0;
	match.parser_meta.ipsec_ar_syndrome = 1;

	actions_meta.decrypt_syndrome = 0;
	actions_meta.antireplay_syndrome = 1;
	actions.meta.pkt_meta = DOCA_HTOBE32(actions_meta.u32);

	/* match on ASO syndrome 1 */
	result = doca_flow_pipe_add_entry(queue_id,
					  pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  decrypt_status,
					  &rule->entries[2].entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add bad syndrome pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.ipsec_ar_syndrome = 2;
	actions_meta.antireplay_syndrome = 2;
	actions.meta.pkt_meta = DOCA_HTOBE32(actions_meta.u32);

	/* match on ASO syndrome 2 */
	result = doca_flow_pipe_add_entry(queue_id,
					  pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  &fwd,
					  flags,
					  decrypt_status,
					  &rule->entries[3].entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add bad syndrome pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Add vxlan decap pipe entry
 *
 * @port [in]: port of the pipe
 * @pipe [in]: pipe to add entry
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_vxlan_decap_pipe_entry(struct doca_flow_port *port,
					       struct security_gateway_pipe_info *pipe,
					       struct ipsec_security_gw_config *app_cfg)
{
	int num_of_entries = 1;
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry **entry = NULL;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	if (app_cfg->debug_mode) {
		pipe->entries_info =
			(struct security_gateway_entry_info *)calloc(1, sizeof(struct security_gateway_entry_info));
		if (pipe->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			return DOCA_ERROR_NO_MEMORY;
		}
		snprintf(pipe->entries_info[pipe->nb_entries].name, MAX_NAME_LEN, "vxlan_decap");
		entry = &pipe->entries_info[pipe->nb_entries++].entry;
	}

	result = doca_flow_pipe_add_entry(0,
					  pipe->pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ipv4 entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS)
		return result;
	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

/*
 * Create pipe that match vxlan traffic with specific tunnel ID and decap action
 *
 * @port [in]: port of the pipe
 * @app_cfg [in]: application configuration struct
 * @next_pipe [in]: next pipe to send the packets
 * @pipe [in]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_vxlan_decap_pipe(struct doca_flow_port *port,
					    struct ipsec_security_gw_config *app_cfg,
					    struct doca_flow_pipe *next_pipe,
					    struct security_gateway_pipe_info *pipe)
{
	int nb_actions = 1;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, *actions_arr[nb_actions];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	match.tun.type = DOCA_FLOW_TUN_VXLAN;
	match.tun.vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_STANDARD;
	match.tun.vxlan_tun_id = DOCA_HTOBE32(app_cfg->vni);

	actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.decap_cfg.is_l2 = true;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "VXLAN_DECAP_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, nb_actions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vxlan decap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return add_vxlan_decap_pipe_entry(port, pipe, app_cfg);

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create pipe for decap ESP header with crypto_encap actions
 *
 * @port [in]: port of the pipe
 * @app_cfg [in]: application configuration struct
 * @fwd [in]: pointer to forward struct
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_decap_pipe(struct doca_flow_port *port,
					    struct ipsec_security_gw_config *app_cfg,
					    struct doca_flow_fwd *fwd,
					    struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_crypto crypto_actions = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element elements[1] = {0};
	const int nb_ordered_lists = 1;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	doca_error_t result;

	if (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH ||
	    app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP) {
		crypto_actions.has_crypto_encap = true;
		crypto_actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
		crypto_actions.crypto_encap.icv_size = get_icv_len_int(app_cfg->icv_length);

		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL) {
			crypto_actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_TUNNEL;
			uint8_t reformat_decap_data[14] = {
				0xff,
				0xff,
				0xff,
				0xff,
				0xff,
				0xff, /* mac_dst */
				0xff,
				0xff,
				0xff,
				0xff,
				0xff,
				0xff, /* mac_src */
				0xff,
				0xff /* mac_type */
			};
			memcpy(crypto_actions.crypto_encap.encap_data,
			       reformat_decap_data,
			       sizeof(reformat_decap_data));
			crypto_actions.crypto_encap.data_size = sizeof(reformat_decap_data);
		} else if (app_cfg->mode == IPSEC_SECURITY_GW_TRANSPORT)
			crypto_actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_OVER_IPV4;
		else
			crypto_actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_UDP_ESP_OVER_IPV4;
	}

	elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements[0].crypto = &crypto_actions;

	ordered_list.idx = 0;
	ordered_list.size = 1;
	ordered_list.elements = elements;

	ordered_lists[0] = &ordered_list;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "DECAP_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg miss_counter: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg ordered_lists: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decap ordered list pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create match pipe for decap ESP header that forwards to ordered list pipe
 *
 * @port [in]: port of the pipe
 * @app_cfg [in]: application configuration struct
 * @ordered_list_pipe [in]: ordered list pipe to forward to
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_decap_match_pipe(struct doca_flow_port *port,
						  struct ipsec_security_gw_config *app_cfg,
						  struct doca_flow_pipe *ordered_list_pipe,
						  struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd, fwd_miss;
	struct doca_flow_fwd *fwd_miss_ptr = NULL;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint32_t nb_entries = 2;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* ipsec syndrome */
	match_mask.parser_meta.ipsec_syndrome = 0xff;
	match.parser_meta.ipsec_syndrome = 0xff;

	/* anti-replay syndrome */
	match_mask.parser_meta.ipsec_ar_syndrome = 0xff;
	match.parser_meta.ipsec_ar_syndrome = 0xff;

	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL)
		meta.inner_ipv6 = 1;
	else {
		match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
		match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	}

	match_mask.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	match.meta.pkt_meta = 0xffffffff;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "DECAP_MATCH_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nb_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		fwd_miss.type = DOCA_FLOW_FWD_PIPE;
		fwd_miss.next_pipe = app_cfg->decrypt_pipes.bad_syndrome_pipe.pipe;
		fwd_miss_ptr = &fwd_miss;
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ordered_list_pipe;
	fwd.ordered_list_pipe.idx = 0xffffffff;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, fwd_miss_ptr, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decap match pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(nb_entries,
								     sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create ingress pipe to remove non-ESP marker and forward to IPv4 or IPv6 pipes
 *
 * @port [in]: port of the pipe
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_marker_decap_pipe(struct doca_flow_port *port, struct ipsec_security_gw_config *app_cfg)
{
	int nb_actions = 1;
	int num_of_entries = 2;
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, actions_arr[nb_actions];
	struct doca_flow_actions *actions_list[] = {&actions_arr[0]};
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct security_gateway_pipe_info *pipe_info = &app_cfg->decrypt_pipes.marker_remove_pipe;
	struct doca_flow_pipe_entry *entry = NULL;
	doca_error_t result;

	if (app_cfg->offload != IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH &&
	    app_cfg->offload != IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP) {
		return DOCA_SUCCESS;
	}

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
	match_mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;

	actions.has_crypto_encap = true;
	actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
	actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_NON_ESP_MARKER;

	actions_arr[0] = actions;
	actions_arr[0].crypto_encap.data_size = DECAP_MARKER_HEADER_SIZE;

	strcpy(pipe_info->name, "MARKER_DECAP_PIPE");
	pipe_info->nb_entries = 0;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_info->name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_list, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = NULL;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create non-ESP marker decap ingress pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(2, sizeof(struct security_gateway_entry_info));

		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
			goto destroy_pipe_cfg;
		}
	}

	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)DOCA_FLOW_L3_META_IPV4;
	fwd.next_pipe = app_cfg->decrypt_pipes.match_decrypt_ipv4_pipe.pipe;
	result = doca_flow_pipe_add_entry(0,
					  pipe_info->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add non-ESP marker decap ingress IPv4 entry: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (result == DOCA_SUCCESS && pipe_info->entries_info != NULL) {
		snprintf(pipe_info->entries_info[pipe_info->nb_entries].name, MAX_NAME_LEN, "marker_decap_ipv4");
		pipe_info->entries_info[pipe_info->nb_entries++].entry = entry;
	}

	match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)DOCA_FLOW_L3_META_IPV6;
	fwd.next_pipe = app_cfg->decrypt_pipes.match_decrypt_ipv6_pipe.pipe;
	result = doca_flow_pipe_add_entry(0,
					  pipe_info->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add non-ESP marker decap ingress IPv6 entry: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (result == DOCA_SUCCESS && pipe_info->entries_info != NULL) {
		snprintf(pipe_info->entries_info[pipe_info->nb_entries].name, MAX_NAME_LEN, "marker_decap_ipv6");
		pipe_info->entries_info[pipe_info->nb_entries++].entry = entry;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS)
		goto destroy_pipe_cfg;

	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure)
		result = DOCA_ERROR_BAD_STATE;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Update the crypto config for decrypt tunnel mode
 *
 * @crypto_cfg [in]: shared object config
 * @eth_header [in]: contains the src mac address
 * @inner_l3_type [in]: DOCA_FLOW_L3_TYPE_IP4 / DOCA_FLOW_L3_TYPE_IP6
 */
static void create_tunnel_decap_tunnel(struct doca_flow_header_eth *eth_header,
				       enum doca_flow_l3_type inner_l3_type,
				       uint8_t *reformat_data,
				       uint16_t *reformat_data_sz)
{
	uint8_t reformat_decap_data[14] = {
		0xaa,
		0xbb,
		0xcc,
		0xdd,
		0xee,
		0xff, /* mac_dst */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00, /* mac_src */
		0x00,
		0x00 /* mac_type */
	};

	reformat_decap_data[DECAP_IDX_SRC_MAC] = eth_header->src_mac[0];
	reformat_decap_data[DECAP_IDX_SRC_MAC + 1] = eth_header->src_mac[1];
	reformat_decap_data[DECAP_IDX_SRC_MAC + 2] = eth_header->src_mac[2];
	reformat_decap_data[DECAP_IDX_SRC_MAC + 3] = eth_header->src_mac[3];
	reformat_decap_data[DECAP_IDX_SRC_MAC + 4] = eth_header->src_mac[4];
	reformat_decap_data[DECAP_IDX_SRC_MAC + 5] = eth_header->src_mac[5];

	if (inner_l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		reformat_decap_data[DECAP_MAC_TYPE_IDX] = 0x08;
		reformat_decap_data[DECAP_MAC_TYPE_IDX + 1] = 0x00;
	} else {
		reformat_decap_data[DECAP_MAC_TYPE_IDX] = 0x86;
		reformat_decap_data[DECAP_MAC_TYPE_IDX + 1] = 0xdd;
	}

	memcpy(reformat_data, reformat_decap_data, sizeof(reformat_decap_data));
	*reformat_data_sz = sizeof(reformat_decap_data);
}

/*
 * Add entries to the decap ordered list pipe and match pipe
 *
 * @app_cfg [in]: application configuration struct
 * @port [in]: port of the pipe
 * @eth_header [in]: contains the src mac address
 * @match_pipe [in]: match pipe to add entries to
 * @ordered_list_pipe [in]: ordered list pipe to add entries to
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_decap_pipe_entries(struct ipsec_security_gw_config *app_cfg,
					   struct ipsec_security_gw_ports_map *port,
					   struct doca_flow_header_eth *eth_header,
					   struct security_gateway_pipe_info *match_pipe,
					   struct security_gateway_pipe_info *ordered_list_pipe)
{
	uint32_t nb_queues = app_cfg->dpdk_config->port_config.nb_queues;
	uint16_t rss_queues[nb_queues];
	union security_gateway_pkt_meta meta = {0};
	struct doca_flow_crypto crypto_actions = {};
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element element = {};
	struct doca_flow_fwd fwd_to_ordered_list = {};
	struct doca_flow_match match;
	struct doca_flow_pipe_entry **entry = NULL;
	doca_error_t result;
	uint32_t rss_flags;
	struct doca_flow_fwd fwd;
	uint32_t entry_idx_ipv4 = 0, entry_idx_ipv6 = 1;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	/* Add default entry to ordered list pipe */
	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL)
		create_tunnel_decap_tunnel(eth_header,
					   DOCA_FLOW_L3_TYPE_IP4,
					   crypto_actions.crypto_encap.encap_data,
					   &crypto_actions.crypto_encap.data_size);

	element.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	element.crypto = &crypto_actions;

	ordered_list.idx = 0; /* default entry */
	ordered_list.size = 1;
	ordered_list.elements = &element;

	/* Add default entry to ordered list pipe for IPv4 */
	rss_flags = DOCA_FLOW_RSS_IPV4;
	create_hairpin_pipe_fwd(app_cfg, port->port_id, false, rss_queues, rss_flags, &fwd);

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       ordered_list_pipe->pipe,
						       entry_idx_ipv4,
						       &ordered_list,
						       &fwd,
						       DOCA_FLOW_WAIT_FOR_BATCH,
						       &app_cfg->secured_status[0],
						       entry);
	if (result != DOCA_SUCCESS)
		return result;
	app_cfg->secured_status[0].entries_in_queue += 1;

	/* Add default entry to ordered list pipe for IPv6 */
	memset(&crypto_actions, 0, sizeof(crypto_actions));
	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL)
		create_tunnel_decap_tunnel(eth_header,
					   DOCA_FLOW_L3_TYPE_IP6,
					   crypto_actions.crypto_encap.encap_data,
					   &crypto_actions.crypto_encap.data_size);
	rss_flags = DOCA_FLOW_RSS_IPV6;
	create_hairpin_pipe_fwd(app_cfg, port->port_id, false, rss_queues, rss_flags, &fwd);

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       ordered_list_pipe->pipe,
						       entry_idx_ipv6,
						       &ordered_list,
						       &fwd,
						       DOCA_FLOW_WAIT_FOR_BATCH,
						       &app_cfg->secured_status[0],
						       entry);
	if (result != DOCA_SUCCESS)
		return result;
	app_cfg->secured_status[0].entries_in_queue += 1;

	/* Add entries to match pipe */
	/* IPv4 */
	meta.inner_ipv6 = 0;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = ordered_list_pipe->pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = entry_idx_ipv4;

	if (app_cfg->debug_mode) {
		snprintf(match_pipe->entries_info[match_pipe->nb_entries].name, MAX_NAME_LEN, "match_decap_ipv4");
		entry = &match_pipe->entries_info[match_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(0,
					  match_pipe->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd_to_ordered_list,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	app_cfg->secured_status[0].entries_in_queue += 1;

	/* IPv6 */
	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL)
		meta.inner_ipv6 = 1;
	else
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	fwd_to_ordered_list.ordered_list_pipe.idx = entry_idx_ipv6;

	if (app_cfg->debug_mode) {
		snprintf(match_pipe->entries_info[match_pipe->nb_entries].name, MAX_NAME_LEN, "match_decap_ipv6");
		entry = &match_pipe->entries_info[match_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(0,
					  match_pipe->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd_to_ordered_list,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	app_cfg->secured_status[0].entries_in_queue += 1;

	do {
		result = process_entries(port->port, &app_cfg->secured_status[0], DEFAULT_TIMEOUT_US, 0);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->secured_status[0].entries_in_queue > 0);
	return DOCA_SUCCESS;
}

/*
 * Create control pipe for secured port
 *
 * @port [in]: port of the pipe
 * @is_root [in]: true in vnf mode
 * @debug_mode [in]: true if running in debug mode
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_control_pipe(struct doca_flow_port *port,
					bool is_root,
					bool debug_mode,
					struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "CONTROL_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create control pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(2, sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add control pipe entries - one entry that forwards IPv4 traffic to decrypt IPv4 pipe,
 * and one entry that forwards IPv6 traffic to decrypt IPv6 pipe
 *
 * @control_pipe [in]: control pipe pointer
 * @app_cfg [in]: application configuration struct
 * @is_root [in]: true in vnf mode
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_control_pipe_entries(struct security_gateway_pipe_info *control_pipe,
					     struct ipsec_security_gw_config *app_cfg,
					     bool is_root)
{
	struct doca_flow_pipe_entry **entry = NULL;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_monitor *monitor_ptr = NULL;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	struct doca_flow_header_format *ip_esp_header = app_cfg->vxlan_encap ? &(match.inner) : &(match.outer);
	enum doca_flow_l3_meta *meta_l3_type = app_cfg->vxlan_encap ? &(match.parser_meta.inner_l3_type) :
								      &(match.parser_meta.outer_l3_type);
	enum doca_flow_l4_meta *meta_l4_type = app_cfg->vxlan_encap ? &(match.parser_meta.inner_l4_type) :
								      &(match.parser_meta.outer_l4_type);

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	if (app_cfg->debug_mode && !is_root) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		monitor_ptr = &monitor;
	}

	if (app_cfg->marker_encap && app_cfg->vxlan_encap) {
		DOCA_LOG_ERR("Non-ESP marker is not supported over VXLAN encapsulation");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (!app_cfg->vxlan_encap) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = app_cfg->marker_encap ? app_cfg->decrypt_pipes.marker_remove_pipe.pipe :
							app_cfg->decrypt_pipes.match_decrypt_ipv4_pipe.pipe;
	} else {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = app_cfg->decrypt_pipes.vxlan_decap_ipv4_pipe.pipe;
	}

	if (app_cfg->mode != IPSEC_SECURITY_GW_UDP_TRANSPORT) {
		if (is_root) {
			ip_esp_header->eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
			ip_esp_header->l3_type = DOCA_FLOW_L3_TYPE_IP4;
			ip_esp_header->ip4.next_proto = IPPROTO_ESP;
		} else {
			*meta_l3_type = DOCA_FLOW_L3_META_IPV4;
			*meta_l4_type = DOCA_FLOW_L4_META_ESP;
		}
	} else {
		if (is_root) {
			ip_esp_header->eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
			ip_esp_header->l3_type = DOCA_FLOW_L3_TYPE_IP4;
			ip_esp_header->ip4.next_proto = IPPROTO_UDP;
		} else {
			*meta_l3_type = DOCA_FLOW_L3_META_IPV4;
			*meta_l4_type = DOCA_FLOW_L4_META_UDP;
		}
		ip_esp_header->l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
		ip_esp_header->udp.l4_port.dst_port = rte_cpu_to_be_16(UDP_DST_PORT_FOR_ESP);
	}

	if (app_cfg->debug_mode) {
		snprintf(control_pipe->entries_info[control_pipe->nb_entries].name, MAX_NAME_LEN, "ipv4");
		entry = &control_pipe->entries_info[control_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  control_pipe->pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor_ptr,
						  &fwd,
						  NULL,
						  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add IPv4 control entry: %s", doca_error_get_descr(result));
		return result;
	}

	memset(&match, 0, sizeof(match));

	if (!app_cfg->vxlan_encap) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = app_cfg->marker_encap ? app_cfg->decrypt_pipes.marker_remove_pipe.pipe :
							app_cfg->decrypt_pipes.match_decrypt_ipv6_pipe.pipe;
	} else {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = app_cfg->decrypt_pipes.vxlan_decap_ipv6_pipe.pipe;
	}

	if (app_cfg->mode != IPSEC_SECURITY_GW_UDP_TRANSPORT) {
		if (is_root) {
			ip_esp_header->eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV6);
			ip_esp_header->l3_type = DOCA_FLOW_L3_TYPE_IP6;
			ip_esp_header->ip6.next_proto = IPPROTO_ESP;
		} else {
			*meta_l3_type = DOCA_FLOW_L3_META_IPV6;
			*meta_l4_type = DOCA_FLOW_L4_META_ESP;
		}
	} else {
		if (is_root) {
			ip_esp_header->eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV6);
			ip_esp_header->l3_type = DOCA_FLOW_L3_TYPE_IP6;
			ip_esp_header->ip6.next_proto = IPPROTO_UDP;
		} else {
			*meta_l3_type = DOCA_FLOW_L3_META_IPV6;
			*meta_l4_type = DOCA_FLOW_L4_META_UDP;
		}
		ip_esp_header->l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
		ip_esp_header->udp.l4_port.dst_port = rte_cpu_to_be_16(UDP_DST_PORT_FOR_ESP);
	}

	if (app_cfg->debug_mode) {
		snprintf(control_pipe->entries_info[control_pipe->nb_entries].name, MAX_NAME_LEN, "ipv6");
		entry = &control_pipe->entries_info[control_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  control_pipe->pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor_ptr,
						  &fwd,
						  NULL,
						  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add IPv6 control entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Convert numeric window size to DOCA Flow's enum
 *
 * @window_size [in]: window size in bytes
 * @return: DOCA Flow's crypto replay window size enum
 */
static enum doca_flow_crypto_replay_win_size convert_window_size_to_enum(uint32_t window_size)
{
	switch (window_size) {
	case 32:
		return DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_32;
	case 64:
		return DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_64;
	case 128:
		return DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_128;
	case 256:
		return DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_256;
	default:
		DOCA_LOG_ERR("Invalid window size: %u, reverting to 128 bytes", window_size);
		return DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_128;
	}
}

/*
 * Config and bind shared IPSEC object for decryption
 *
 * @app_sa_attrs [in]: SA attributes
 * @app_cfg [in]: application configuration struct
 * @ipsec_id [in]: index of shared object
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_decrypt_shared_object(struct doca_flow_port *secured_port,
						       struct ipsec_security_gw_sa_attrs *app_sa_attrs,
						       struct ipsec_security_gw_config *app_cfg,
						       uint32_t ipsec_id)
{
	struct doca_flow_shared_resource_cfg cfg;
	doca_error_t result;

	memset(&cfg, 0, sizeof(cfg));

	cfg.ipsec_sa_cfg.icv_len = app_cfg->icv_length;
	cfg.ipsec_sa_cfg.salt = app_sa_attrs->salt;
	cfg.ipsec_sa_cfg.implicit_iv = app_sa_attrs->iv;
	cfg.ipsec_sa_cfg.key_cfg.key_type = app_sa_attrs->key_type;
	cfg.ipsec_sa_cfg.key_cfg.key = (void *)&app_sa_attrs->enc_key_data;
	cfg.ipsec_sa_cfg.sn_initial = app_cfg->sn_initial;
	cfg.ipsec_sa_cfg.esn_en = app_sa_attrs->esn_en;
	if (!app_cfg->sw_antireplay) {
		cfg.ipsec_sa_cfg.sn_offload_type = DOCA_FLOW_CRYPTO_SN_OFFLOAD_AR;
		cfg.ipsec_sa_cfg.win_size = convert_window_size_to_enum(HW_WINDOW_SIZE);
		cfg.ipsec_sa_cfg.lifetime_threshold = app_sa_attrs->lifetime_threshold;
	}
	/* config ipsec object */
	result = doca_flow_port_shared_resource_set_cfg(secured_port,
							DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA,
							ipsec_id,
							&cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cfg shared ipsec object: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t add_decrypt_entry(struct decrypt_rule *rule,
			       int rule_id,
			       struct doca_flow_port *port,
			       struct ipsec_security_gw_config *app_cfg)

{
	struct doca_flow_match match;
	struct doca_flow_pipe_entry **entry = NULL;
	struct security_gateway_pipe_info *decrypt_pipe;
	struct doca_flow_crypto crypto_actions = {};
	struct doca_flow_actions meta_actions = {};
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element elements[2] = {0};
	struct doca_flow_fwd fwd = {};
	struct doca_flow_fwd fwd_to_ordered_list = {};
	uint32_t flags;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	uint32_t ipsec_id = IPSEC_ID(rule_id);

	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));
	memset(&match, 0, sizeof(match));

	/* create ipsec shared objects */
	result = create_ipsec_decrypt_shared_object(port, &rule->sa_attrs, app_cfg, ipsec_id);
	if (result != DOCA_SUCCESS)
		return result;

	/* Add entry to ordered list pipe */
	crypto_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
	crypto_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
	crypto_actions.crypto.ipsec_sa.sn_en = !app_cfg->sw_antireplay;
	crypto_actions.crypto.crypto_id = ipsec_id;

	/* save rule index in metadata */
	meta.decrypt = 1;
	meta.rule_id = rule_id;
	meta.inner_ipv6 = 0;
	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL && rule->inner_l3_type == DOCA_FLOW_L3_TYPE_IP6)
		meta.inner_ipv6 = 1;
	meta_actions.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements[0].crypto = &crypto_actions;
	elements[1].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements[1].actions = &meta_actions;

	ordered_list.idx = 0;
	ordered_list.size = 2;
	ordered_list.elements = elements;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = app_cfg->decrypt_pipes.match_decap_pipe.pipe;

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       app_cfg->decrypt_pipes.decrypt_pipe.pipe,
						       rule_id,
						       &ordered_list,
						       &fwd,
						       DOCA_FLOW_WAIT_FOR_BATCH,
						       &app_cfg->secured_status[0],
						       NULL);
	if (result != DOCA_SUCCESS)
		return result;
	app_cfg->secured_status[0].entries_in_queue += 1;

	/* build rule match with specific destination IP and ESP SPI for match pipe */
	match.tun.esp_spi = RTE_BE32(rule->esp_spi);

	if (rule->l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		decrypt_pipe = &app_cfg->decrypt_pipes.match_decrypt_ipv4_pipe;
		match.outer.ip4.dst_ip = rule->dst_ip4;
	} else {
		decrypt_pipe = &app_cfg->decrypt_pipes.match_decrypt_ipv6_pipe;
		memcpy(match.outer.ip6.dst_ip, rule->dst_ip6, sizeof(rule->dst_ip6));
	}

	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = app_cfg->decrypt_pipes.decrypt_pipe.pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = rule_id;

	if (app_cfg->debug_mode) {
		flags = DOCA_FLOW_WAIT_FOR_BATCH;
		snprintf(decrypt_pipe->entries_info[decrypt_pipe->nb_entries].name, MAX_NAME_LEN, "rule%d", rule_id);
		entry = &decrypt_pipe->entries_info[decrypt_pipe->nb_entries++].entry;
	} else
		flags = DOCA_FLOW_NO_WAIT;

	result = doca_flow_pipe_add_entry(0,
					  decrypt_pipe->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd_to_ordered_list,
					  flags,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	app_cfg->secured_status[0].entries_in_queue++;

	if (app_cfg->debug_mode) {
		result = add_bad_syndrome_pipe_entry(app_cfg,
						     app_cfg->decrypt_pipes.bad_syndrome_pipe.pipe,
						     rule,
						     rule_id,
						     &app_cfg->secured_status[0],
						     DOCA_FLOW_NO_WAIT,
						     0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
		app_cfg->secured_status[0].entries_in_queue += NUM_OF_SYNDROMES;
	}

	/* process the entries in the decryption pipe*/
	do {
		result = process_entries(port, &app_cfg->secured_status[0], DEFAULT_TIMEOUT_US, 0);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->secured_status[0].entries_in_queue > 0);
	return DOCA_SUCCESS;
}

doca_error_t get_decrypt_ids(int nb_rules, int initial_id, struct doca_flow_port *port)
{
	doca_error_t result;
	int i, array_len = nb_rules;

	if (array_len == 0)
		return DOCA_SUCCESS;

	for (i = 0; i < nb_rules; i++) {
		result = doca_flow_port_shared_resource_get(port,
							    DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA,
							    &IPSEC_ID(initial_id + i));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate decrypt IDs to the port");
			return result;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t add_decrypt_entries(struct ipsec_security_gw_config *app_cfg,
				 struct ipsec_security_gw_ports_map *port,
				 uint16_t queue_id,
				 int nb_rules,
				 int rule_offset)
{
	struct doca_flow_match decrypt_match;
	struct doca_flow_pipe_entry **entry = NULL;
	struct security_gateway_pipe_info *decrypt_pipe;
	struct doca_flow_port *secured_port;
	enum doca_flow_flags_type flags;
	enum doca_flow_flags_type decrypt_flags;
	int i, rule_id;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	struct doca_flow_crypto crypto_actions = {};
	struct doca_flow_actions meta_actions = {};
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element elements[2] = {0};
	struct doca_flow_fwd fwd = {};
	struct doca_flow_fwd fwd_to_ordered_list = {};
	struct decrypt_rule *rules = app_cfg->app_rules.decrypt_rules;
	struct decrypt_pipes *pipes = &app_cfg->decrypt_pipes;
	int nb_encrypt_rules = app_cfg->app_rules.nb_encrypt_rules;
	uint32_t ipsec_id;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		secured_port = port->port;
	} else {
		secured_port = doca_flow_port_switch_get(NULL);
	}

	memset(&app_cfg->secured_status[queue_id], 0, sizeof(app_cfg->secured_status[queue_id]));
	memset(&decrypt_match, 0, sizeof(decrypt_match));

	for (i = 0; i < nb_rules; i++) {
		rule_id = rule_offset + i;
		ipsec_id = IPSEC_ID(nb_encrypt_rules + rule_id);
		if (i == nb_rules - 1 || app_cfg->secured_status[queue_id].entries_in_queue == QUEUE_DEPTH - 8)
			flags = DOCA_FLOW_NO_WAIT;
		else
			flags = DOCA_FLOW_WAIT_FOR_BATCH;

		/* create ipsec shared objects */
		result = create_ipsec_decrypt_shared_object(secured_port, &rules[rule_id].sa_attrs, app_cfg, ipsec_id);
		if (result != DOCA_SUCCESS)
			return result;

		/* Add entry to ordered list pipe */
		crypto_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
		crypto_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
		crypto_actions.crypto.ipsec_sa.sn_en = !app_cfg->sw_antireplay;
		crypto_actions.crypto.crypto_id = ipsec_id;

		/* save rule index in metadata */
		meta.decrypt = 1;
		meta.rule_id = rule_id;
		meta.inner_ipv6 = 0;
		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL && rules[rule_id].inner_l3_type == DOCA_FLOW_L3_TYPE_IP6)
			meta.inner_ipv6 = 1;
		meta_actions.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

		elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
		elements[0].crypto = &crypto_actions;
		elements[1].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
		elements[1].actions = &meta_actions;

		ordered_list.idx = 0;
		ordered_list.size = 2;
		ordered_list.elements = elements;

		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = pipes->match_decap_pipe.pipe;

		result = doca_flow_pipe_ordered_list_add_entry(queue_id,
							       pipes->decrypt_pipe.pipe,
							       rule_id,
							       &ordered_list,
							       &fwd,
							       DOCA_FLOW_WAIT_FOR_BATCH,
							       &app_cfg->secured_status[queue_id],
							       NULL);
		if (result != DOCA_SUCCESS)
			return result;
		app_cfg->secured_status[queue_id].entries_in_queue += 1;

		/* build rule match with specific destination IP and ESP SPI */
		decrypt_match.tun.esp_spi = RTE_BE32(rules[rule_id].esp_spi);

		if (rules[rule_id].l3_type == DOCA_FLOW_L3_TYPE_IP4) {
			decrypt_pipe = &pipes->match_decrypt_ipv4_pipe;
			decrypt_match.outer.ip4.dst_ip = rules[rule_id].dst_ip4;
		} else {
			decrypt_pipe = &pipes->match_decrypt_ipv6_pipe;
			memcpy(decrypt_match.outer.ip6.dst_ip, rules[rule_id].dst_ip6, sizeof(rules[rule_id].dst_ip6));
		}

		fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
		fwd_to_ordered_list.ordered_list_pipe.pipe = pipes->decrypt_pipe.pipe;
		fwd_to_ordered_list.ordered_list_pipe.idx = rule_id;

		if (app_cfg->debug_mode) {
			decrypt_flags = DOCA_FLOW_WAIT_FOR_BATCH;
			snprintf(decrypt_pipe->entries_info[decrypt_pipe->nb_entries].name, MAX_NAME_LEN, "rule%d", i);
			entry = &decrypt_pipe->entries_info[decrypt_pipe->nb_entries++].entry;
		} else
			decrypt_flags = flags;
		result = doca_flow_pipe_add_entry(queue_id,
						  decrypt_pipe->pipe,
						  &decrypt_match,
						  0,
						  NULL,
						  NULL,
						  &fwd_to_ordered_list,
						  decrypt_flags,
						  &app_cfg->secured_status[queue_id],
						  entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
		app_cfg->secured_status[queue_id].entries_in_queue++;

		if (app_cfg->debug_mode) {
			result = add_bad_syndrome_pipe_entry(app_cfg,
							     pipes->bad_syndrome_pipe.pipe,
							     &rules[rule_id],
							     rule_id,
							     &app_cfg->secured_status[queue_id],
							     flags,
							     queue_id);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
				return result;
			}
			app_cfg->secured_status[queue_id].entries_in_queue += NUM_OF_SYNDROMES;
		}
		if (app_cfg->secured_status[queue_id].entries_in_queue >= QUEUE_DEPTH - 8) {
			result = doca_flow_entries_process(secured_port,
							   queue_id,
							   DEFAULT_TIMEOUT_US,
							   app_cfg->secured_status[queue_id].entries_in_queue);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
				return result;
			}
			if (app_cfg->secured_status[queue_id].failure ||
			    app_cfg->secured_status[queue_id].entries_in_queue == QUEUE_DEPTH) {
				DOCA_LOG_ERR("Failed to process entries");
				return DOCA_ERROR_BAD_STATE;
			}
		}
	}

	/* process the entries in the decryption pipe*/
	do {
		result =
			process_entries(secured_port, &app_cfg->secured_status[queue_id], DEFAULT_TIMEOUT_US, queue_id);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->secured_status[queue_id].entries_in_queue > 0);
	return DOCA_SUCCESS;
}

doca_error_t ipsec_security_gw_insert_decrypt_rules(struct ipsec_security_gw_ports_map *ports[],
						    struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_port *secured_port;
	struct doca_flow_fwd fwd;
	bool is_root;
	doca_error_t result;
	int expected_entries;

	if (app_cfg->socket_ctx.socket_conf)
		expected_entries = MAX_NB_RULES;
	else if (app_cfg->app_rules.nb_decrypt_rules > 0)
		expected_entries = app_cfg->app_rules.nb_decrypt_rules;
	else /* default value - no entries expected, putting a default value so that pipe creation won't fail */
		expected_entries = DEF_EXPECTED_ENTRIES;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		secured_port = ports[SECURED_IDX]->port;
		is_root = true;
	} else {
		secured_port = doca_flow_port_switch_get(NULL);
		is_root = false;
	}

	if (app_cfg->debug_mode) {
		DOCA_LOG_DBG("Creating bad syndrome pipe");
		result = create_bad_syndrome_pipe(app_cfg,
						  secured_port,
						  expected_entries,
						  &app_cfg->decrypt_pipes.bad_syndrome_pipe.pipe);
		if (result != DOCA_SUCCESS)
			return result;
	}

	create_hairpin_pipe_fwd(app_cfg, ports[SECURED_IDX]->port_id, false, NULL, 0, &fwd);

	DOCA_LOG_DBG("Creating decap pipe");
	snprintf(app_cfg->decrypt_pipes.decap_pipe.name, MAX_NAME_LEN, "decap");
	result = create_ipsec_decap_pipe(secured_port, app_cfg, &fwd, &app_cfg->decrypt_pipes.decap_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_DBG("Creating decap match pipe");
	snprintf(app_cfg->decrypt_pipes.match_decap_pipe.name, MAX_NAME_LEN, "decap_match");
	result = create_ipsec_decap_match_pipe(secured_port,
					       app_cfg,
					       app_cfg->decrypt_pipes.decap_pipe.pipe,
					       &app_cfg->decrypt_pipes.match_decap_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	result = add_decap_pipe_entries(app_cfg,
					ports[SECURED_IDX],
					&ports[UNSECURED_IDX]->eth_header,
					&app_cfg->decrypt_pipes.match_decap_pipe,
					&app_cfg->decrypt_pipes.decap_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add decap pipe entries: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_DBG("Creating decrypt pipe");
	snprintf(app_cfg->decrypt_pipes.decrypt_pipe.name, MAX_NAME_LEN, "decrypt_pipe");
	result = create_ipsec_decrypt_pipe(secured_port,
					   expected_entries,
					   app_cfg,
					   &app_cfg->decrypt_pipes.decrypt_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_DBG("Creating IPv4 decrypt match pipe");
	snprintf(app_cfg->decrypt_pipes.match_decrypt_ipv4_pipe.name, MAX_NAME_LEN, "IPv4_decrypt_match");
	result = create_ipsec_decrypt_match_pipe(secured_port,
						 expected_entries,
						 DOCA_FLOW_L3_TYPE_IP4,
						 app_cfg->decrypt_pipes.decrypt_pipe.pipe,
						 app_cfg,
						 &app_cfg->decrypt_pipes.match_decrypt_ipv4_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_DBG("Creating IPv6 decrypt match pipe");
	snprintf(app_cfg->decrypt_pipes.match_decrypt_ipv6_pipe.name, MAX_NAME_LEN, "IPv6_decrypt_match");
	result = create_ipsec_decrypt_match_pipe(secured_port,
						 expected_entries,
						 DOCA_FLOW_L3_TYPE_IP6,
						 app_cfg->decrypt_pipes.decrypt_pipe.pipe,
						 app_cfg,
						 &app_cfg->decrypt_pipes.match_decrypt_ipv6_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	if (app_cfg->vxlan_encap) {
		if (app_cfg->marker_encap) {
			DOCA_LOG_ERR("Non-ESP marker is not supported over VXLAN encapsulation");
			return DOCA_ERROR_NOT_SUPPORTED;
		}

		DOCA_LOG_DBG("Creating inner IPv4 vxlan decap pipe");
		snprintf(app_cfg->decrypt_pipes.vxlan_decap_ipv4_pipe.name, MAX_NAME_LEN, "vxlan_decap_in_IPv4");
		result = create_vxlan_decap_pipe(secured_port,
						 app_cfg,
						 app_cfg->decrypt_pipes.match_decrypt_ipv4_pipe.pipe,
						 &app_cfg->decrypt_pipes.vxlan_decap_ipv4_pipe);
		if (result != DOCA_SUCCESS)
			return result;

		DOCA_LOG_DBG("Creating inner IPv6 vxlan decap pipe");
		snprintf(app_cfg->decrypt_pipes.vxlan_decap_ipv6_pipe.name, MAX_NAME_LEN, "vxlan_decap_in_IPv6");
		result = create_vxlan_decap_pipe(secured_port,
						 app_cfg,
						 app_cfg->decrypt_pipes.match_decrypt_ipv6_pipe.pipe,
						 &app_cfg->decrypt_pipes.vxlan_decap_ipv6_pipe);
		if (result != DOCA_SUCCESS)
			return result;
	} else if (app_cfg->marker_encap) {
		result = create_marker_decap_pipe(secured_port, app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create non-ESP marker decap ingress pipe: %s",
				     doca_error_get_descr(result));
			return result;
		}
	}

	DOCA_LOG_DBG("Creating control pipe");
	snprintf(app_cfg->decrypt_pipes.decrypt_root.name, MAX_NAME_LEN, "decrypt_root");
	result = create_control_pipe(secured_port, is_root, app_cfg->debug_mode, &app_cfg->decrypt_pipes.decrypt_root);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_DBG("Adding control pipe entries");
	result = add_control_pipe_entries(&app_cfg->decrypt_pipes.decrypt_root, app_cfg, is_root);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Find packet's segment for the specified offset.
 *
 * @mb [in]: packet mbuf
 * @offset [in]: offset in the packet
 * @seg_buf [out]: the segment that contain the offset
 * @seg_offset [out]: offset in the segment
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mbuf_get_seg_ofs(struct rte_mbuf *mb,
				     uint32_t offset,
				     struct rte_mbuf **seg_buf,
				     uint32_t *seg_offset)
{
	uint32_t packet_len, seg_len;
	struct rte_mbuf *tmp_buf;

	packet_len = mb->pkt_len;

	/* if offset is the end of packet */
	if (offset >= packet_len) {
		DOCA_LOG_ERR("Packet offset is invalid");
		return DOCA_ERROR_INVALID_VALUE;
	}

	tmp_buf = mb;
	for (seg_len = rte_pktmbuf_data_len(tmp_buf); seg_len <= offset; seg_len = rte_pktmbuf_data_len(tmp_buf)) {
		tmp_buf = tmp_buf->next;
		offset -= seg_len;
	}

	*seg_offset = offset;
	*seg_buf = tmp_buf;
	return DOCA_SUCCESS;
}

/*
 * Remove packet trailer - padding, ESP tail, and ICV
 *
 * @m [in]: the mbuf to update
 * @icv_len [in]: ICV length
 * @next_proto [out]: ESP tail next protocol field
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t remove_packet_tail(struct rte_mbuf **m, uint32_t icv_len, uint32_t *next_proto)
{
	struct rte_mbuf *ml;
	const struct rte_esp_tail *esp_tail;
	uint32_t esp_tail_offset, esp_tail_seg_offset, trailer_len;
	doca_error_t result;

	/* remove trailing zeros */
	remove_ethernet_padding(m);

	/* find tail offset */
	trailer_len = icv_len + sizeof(struct rte_esp_tail);

	/* find tail offset */
	esp_tail_offset = (*m)->pkt_len - trailer_len;

	/* get the segment with the tail offset */
	result = mbuf_get_seg_ofs(*m, esp_tail_offset, &ml, &esp_tail_seg_offset);
	if (result != DOCA_SUCCESS)
		return result;

	esp_tail = rte_pktmbuf_mtod_offset(ml, const struct rte_esp_tail *, esp_tail_seg_offset);
	*next_proto = esp_tail->next_proto;
	trailer_len += esp_tail->pad_len;
	esp_tail_seg_offset -= esp_tail->pad_len;

	/* remove padding, tail and icv from the end of the packet */
	(*m)->pkt_len -= trailer_len;
	ml->data_len = esp_tail_seg_offset;
	return DOCA_SUCCESS;
}

/*
 * Decap mbuf for tunnel mode
 *
 * @m [in]: the mbuf to update
 * @ctx [in]: the security gateway context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t decap_packet_tunnel(struct rte_mbuf **m, struct ipsec_security_gw_core_ctx *ctx)
{
	uint32_t iv_len = 8;
	struct rte_ether_hdr *l2_header;
	struct rte_ipv4_hdr *ipv4;
	uint32_t proto, l3_len;
	char *np;
	uint32_t icv_len = get_icv_len_int(ctx->config->icv_length);
	uint16_t reformat_decap_data_len;
	enum doca_flow_l3_type inner_l3_type;
	doca_error_t result;

	result = remove_packet_tail(m, icv_len, &proto);
	if (result != DOCA_SUCCESS)
		return result;

	/* calculate l3 len  */
	l2_header = rte_pktmbuf_mtod(*m, struct rte_ether_hdr *);
	if (RTE_ETH_IS_IPV4_HDR((*m)->packet_type)) {
		ipv4 = (void *)(l2_header + 1);
		l3_len = rte_ipv4_hdr_len(ipv4);
	} else {
		l3_len = sizeof(struct rte_ipv6_hdr);
	}

	/* remove l3 and ESP header from the beginning of the packet */
	np = rte_pktmbuf_adj(*m, l3_len + sizeof(struct rte_esp_hdr) + iv_len);
	if (unlikely(np == NULL))
		return DOCA_ERROR_INVALID_VALUE;

	/* change the ether type based on the tail proto */
	l2_header = rte_pktmbuf_mtod(*m, struct rte_ether_hdr *);
	if (proto == IPPROTO_IPV6) {
		inner_l3_type = DOCA_FLOW_L3_TYPE_IP6;
	} else {
		inner_l3_type = DOCA_FLOW_L3_TYPE_IP4;
	}
	create_tunnel_decap_tunnel(&ctx->ports[UNSECURED_IDX]->eth_header,
				   inner_l3_type,
				   (uint8_t *)np,
				   &reformat_decap_data_len);

	return DOCA_SUCCESS;
}

/*
 * Decap mbuf for transport and udp transport mode
 *
 * @m [in]: the mbuf to update
 * @ctx [in]: the security gateway context
 * @udp_transport [in]: true for UDP transport mode
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t decap_packet_transport(struct rte_mbuf **m,
					   struct ipsec_security_gw_core_ctx *ctx,
					   bool udp_transport)
{
	uint32_t iv_len = 8;
	struct rte_ether_hdr *l2_header;
	char *op, *np;
	struct rte_ipv4_hdr *ipv4 = NULL;
	struct rte_ipv6_hdr *ipv6 = NULL;
	uint32_t l2_l3_len, proto;
	int i;
	uint32_t icv_len = get_icv_len_int(ctx->config->icv_length);
	doca_error_t result;

	result = remove_packet_tail(m, icv_len, &proto);
	if (result != DOCA_SUCCESS)
		return result;

	/* calculate l2 and l3 len  */
	l2_header = rte_pktmbuf_mtod(*m, struct rte_ether_hdr *);
	if (RTE_ETH_IS_IPV4_HDR((*m)->packet_type)) {
		ipv4 = (void *)(l2_header + 1);
		l2_l3_len = rte_ipv4_hdr_len(ipv4) + sizeof(struct rte_ether_hdr);
	} else {
		ipv6 = (void *)(l2_header + 1);
		l2_l3_len = sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_ether_hdr);
	}

	/* remove ESP header from the beginning of the packet and UDP header in udp_transport mode*/
	op = rte_pktmbuf_mtod(*m, char *);
	if (!udp_transport)
		np = rte_pktmbuf_adj(*m, sizeof(struct rte_esp_hdr) + iv_len);
	else
		np = rte_pktmbuf_adj(*m, sizeof(struct rte_esp_hdr) + sizeof(struct rte_udp_hdr) + iv_len);
	if (unlikely(np == NULL))
		return DOCA_ERROR_INVALID_VALUE;

	/* align IP length, next protocol and IPv4 checksum */
	if (RTE_ETH_IS_IPV4_HDR((*m)->packet_type)) {
		ipv4->next_proto_id = proto;
		ipv4->total_length = rte_cpu_to_be_16((*m)->pkt_len - sizeof(struct rte_ether_hdr));
		ipv4->hdr_checksum = 0;
		ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
	} else {
		ipv6->proto = proto;
		ipv6->payload_len = rte_cpu_to_be_16((*m)->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(*ipv6));
		/* IPv6 does not have checksum */
	}

	/* copy old l2 and l3 to the new beginning */
	for (i = l2_l3_len - 1; i >= 0; i--)
		np[i] = op[i];
	return DOCA_SUCCESS;
}

/*
 * extract the sn from the mbuf
 *
 * @m [in]: the mbuf to extract from
 * @mode [in]: application running mode
 * @sn [out]: the sn
 */
static void get_esp_sn(struct rte_mbuf *m, enum ipsec_security_gw_mode mode, uint32_t *sn)
{
	uint32_t l2_l3_len;
	struct rte_ether_hdr *oh;
	struct rte_ipv4_hdr *ipv4;
	struct rte_esp_hdr *esp_hdr;

	oh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	if (RTE_ETH_IS_IPV4_HDR(m->packet_type)) {
		ipv4 = (void *)(oh + 1);
		l2_l3_len = rte_ipv4_hdr_len(ipv4) + sizeof(struct rte_ether_hdr);
	} else {
		l2_l3_len = sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_ether_hdr);
	}

	if (mode == IPSEC_SECURITY_GW_UDP_TRANSPORT)
		l2_l3_len += sizeof(struct rte_udp_hdr);

	esp_hdr = rte_pktmbuf_mtod_offset(m, struct rte_esp_hdr *, l2_l3_len);
	*sn = rte_be_to_cpu_32(esp_hdr->seq);
}

/*
 * Perform anti replay check on a packet and update the state accordingly
 * (1) If sn is left (smaller) from window - drop.
 * (2) Else, if sn is in the window - check if it was already received (drop) or not (update bitmap).
 * (3) Else, if sn is larger than window - slide the window so that sn is the last packet in the window and
 * update bitmap.
 *
 * @sn [in]: the sequence number to check
 * @state [in/out]: the anti replay state
 * @drop [out]: true if the packet should be dropped
 *
 * @NOTE: Only supports 64 window size and regular sn (not ESN)
 */
static void anti_replay(uint32_t sn, struct antireplay_state *state, bool *drop)
{
	uint32_t distance, shift, beg_win_sn;
	uint32_t window_size = state->window_size;
	uint32_t end_win_sn = state->end_win_sn;
	uint64_t *bitmap = &state->bitmap;

	beg_win_sn = end_win_sn + 1 - window_size; /* the first sn in the window */
	*drop = true;

	/* (0) Make Coverity happy by making it clear the below shifts are not undefined */
	if (window_size > 64) {
		DOCA_LOG_ERR("Window size (%u) is too large and may trigger undefined behavior", window_size);
		return; /* drop */
	}

	/* (1) Check if the sn is too far from the beginning of the window */
	distance = CALC_DISTANCE(sn, beg_win_sn);
	if (distance > MAX_DISTANCE) {
		return; /* drop */
	}

	/* (2) Are we inside the window? */
	if (distance < window_size) {
		/* Check if sn is already received */
		if (*bitmap & (((uint64_t)1) << distance)) {
			return; /* drop */
		}
		*bitmap |= (((uint64_t)1) << distance);
		*drop = false;
		return; /* pass */
	}

	/* (3) We should shift the end of the window to the new SN and set the last bit */
	shift = distance - window_size;
	if (shift >= window_size) {
		*bitmap = 0;
	} else {
		*bitmap >>= shift;
	}
	*bitmap |= ((uint64_t)1) << (window_size - 1);
	*drop = false;
	state->end_win_sn = sn;
	return; /* pass */
}

doca_error_t handle_secured_packets_received(struct rte_mbuf **packet,
					     bool bad_syndrome_check,
					     struct ipsec_security_gw_core_ctx *ctx)
{
	uint32_t pkt_meta;
	uint32_t rule_idx;
	uint32_t sn;
	union security_gateway_pkt_meta meta;
	doca_error_t result;
	bool drop;

	pkt_meta = *RTE_FLOW_DYNF_METADATA(*packet);
	meta = (union security_gateway_pkt_meta)pkt_meta;
	rule_idx = meta.rule_id;
	if (bad_syndrome_check) {
		if (meta.decrypt_syndrome != 0 || meta.antireplay_syndrome != 0)
			return DOCA_ERROR_BAD_STATE;
	}
	if (ctx->config->sw_antireplay) {
		/* Validate anti replay according to the entry's state */
		get_esp_sn(*packet, ctx->config->mode, &sn);
		/* No synchronization needed, same rule is processed by the same core */
		anti_replay(sn, &(ctx->decrypt_rules[rule_idx].antireplay_state), &drop);
		if (drop) {
			DOCA_LOG_WARN("Anti Replay mechanism dropped packet- sn: %u, rule index: %d", sn, rule_idx);
			return DOCA_ERROR_BAD_STATE;
		}
		/* Only update the state if the packet passed the anti replay check */
		result = doca_flow_port_crypto_ipsec_update_sn(
			ctx->ports[SECURED_IDX]->port,
			IPSEC_ID(ctx->config->app_rules.nb_encrypt_rules + rule_idx),
			sn);
		if (result != DOCA_SUCCESS)
			return result;
	}

	if (ctx->config->mode == IPSEC_SECURITY_GW_TRANSPORT)
		return decap_packet_transport(packet, ctx, false);
	else if (ctx->config->mode == IPSEC_SECURITY_GW_UDP_TRANSPORT)
		return decap_packet_transport(packet, ctx, true);
	else
		return decap_packet_tunnel(packet, ctx);
}
