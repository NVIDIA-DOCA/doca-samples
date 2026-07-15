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

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_GENEVE_OPT);

#define SAMPLE_CLASS_ID 0x107

#define CHANGEABLE_32 (UINT32_MAX)
#define CHANGEABLE_16 (UINT16_MAX)
#define FULL_MASK_32 (UINT32_MAX)

#define BUILD_VNI(uint24_vni) (DOCA_HTOBE32((uint32_t)uint24_vni << 8))

/*
 * Fill list of GENEVE options parser user configuration
 *
 * @list [out]: list of option configurations
 */
static void fill_parser_geneve_opt_cfg_list(struct doca_flow_parser_geneve_opt_cfg *list)
{
	/*
	 * Prepare the configuration for first option.
	 *
	 * 0                   1                   2                   3
	 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |        class (0x107)          |    type (1)   |     | len (5) |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW0 (not part of the parser)                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW1 (part of the parser)                     |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW2 (not part of the parser)                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW3 (part of the parser)                     |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW4 (not part of the parser)                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	list[0].match_on_class_mode = DOCA_FLOW_PARSER_GENEVE_OPT_MODE_FIXED;
	list[0].option_class = DOCA_HTOBE16(SAMPLE_CLASS_ID);
	list[0].option_type = 1;
	list[0].option_len = 5; /* Data length - excluding the option header */
	list[0].data_mask[0] = 0x0;
	list[0].data_mask[1] = FULL_MASK_32;
	list[0].data_mask[2] = 0x0;
	list[0].data_mask[3] = FULL_MASK_32;
	list[0].data_mask[4] = 0x0;

	/*
	 * Prepare the configuration for second option.
	 *
	 * 0                   1                   2                   3
	 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |        class (0x107)          |    type (2)   |     | len (2) |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW0 (part of the parser)                     |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW1 (part of the parser)                     |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	list[1].match_on_class_mode = DOCA_FLOW_PARSER_GENEVE_OPT_MODE_FIXED;
	list[1].option_class = DOCA_HTOBE16(SAMPLE_CLASS_ID);
	list[1].option_type = 2;
	list[1].option_len = 2; /* Data length - excluding the option header */
	list[1].data_mask[0] = FULL_MASK_32;
	list[1].data_mask[1] = FULL_MASK_32;

	/*
	 * Prepare the configuration for third option.
	 *
	 * 0                   1                   2                   3
	 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |        class (0x107)          |    type (3)   |     | len (4) |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW0 (not part of the parser)                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW1 (not part of the parser)                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW2 (not part of the parser)                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  DW3 (part of the parser)                     |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	list[2].match_on_class_mode = DOCA_FLOW_PARSER_GENEVE_OPT_MODE_FIXED;
	list[2].option_class = DOCA_HTOBE16(SAMPLE_CLASS_ID);
	list[2].option_type = 3;
	list[2].option_len = 4; /* Data length - excluding the option header */
	list[2].data_mask[0] = 0x0;
	list[2].data_mask[1] = 0x0;
	list[2].data_mask[2] = 0x0;
	list[2].data_mask[3] = FULL_MASK_32;
}

/*
 * Create DOCA Flow pipe that match GENEVE traffic with changeable GENEVE VNI and options and decap it.
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_geneve_opt_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions1, actions2, *actions_arr[2];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint8_t mac_addr[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions1, 0, sizeof(actions1));
	memset(&actions2, 0, sizeof(actions2));
	memset(&fwd, 0, sizeof(fwd));

	actions_arr[0] = &actions1;
	actions_arr[1] = &actions2;

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match_mask.parser_meta.outer_l4_type = FULL_MASK_32;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match_mask.parser_meta.outer_l3_type = FULL_MASK_32;
	match.tun.type = DOCA_FLOW_TUN_GENEVE;
	match.tun.geneve.vni = CHANGEABLE_32;
	match_mask.tun.geneve.vni = BUILD_VNI(0xffffff);

	/* First option - index 0 describes the option header */
	match.tun.geneve_options[0].class_id = DOCA_HTOBE16(SAMPLE_CLASS_ID);
	match.tun.geneve_options[0].type = 1;
	match.tun.geneve_options[0].length = 5;
	match_mask.tun.geneve_options[0].class_id = 0xffff;
	match_mask.tun.geneve_options[0].type = 0xff;
	/*
	 * Indexes 1-5 describe the option data, index 4 describes the 4th DW in data.
	 * Make data as changeable by cover all data (5 DWs).
	 */
	match.tun.geneve_options[1].data = CHANGEABLE_32;
	match.tun.geneve_options[2].data = CHANGEABLE_32;
	match.tun.geneve_options[3].data = CHANGEABLE_32;
	match.tun.geneve_options[4].data = CHANGEABLE_32;
	match.tun.geneve_options[5].data = CHANGEABLE_32;
	/* Mask the only DW we want to match */
	match_mask.tun.geneve_options[4].data = FULL_MASK_32;

	/*
	 * Second option - index 6 describes the option header.
	 * The order of options in match structure is regardless to options order in parser creation.
	 * This pipe will match if the options will be present in any kind of order.
	 */
	match.tun.geneve_options[6].class_id = DOCA_HTOBE16(SAMPLE_CLASS_ID);
	match.tun.geneve_options[6].type = 2;
	match.tun.geneve_options[6].length = 2;
	match_mask.tun.geneve_options[6].class_id = 0xffff;
	match_mask.tun.geneve_options[6].type = 0xff;
	/*
	 * Indexes 7-8 describe the option data, index 7 describes the 1st DW in data and index 8
	 * describes the 2nd DW in data.
	 * Make data as changeable by cover all data (2 DWs).
	 */
	match.tun.geneve_options[7].data = CHANGEABLE_32;
	match.tun.geneve_options[8].data = CHANGEABLE_32;
	/* We want to match the all DWs in data */
	match_mask.tun.geneve_options[7].data = FULL_MASK_32;
	match_mask.tun.geneve_options[8].data = FULL_MASK_32;

	/* Third option - index 9 describes the option header */
	match.tun.geneve_options[9].class_id = DOCA_HTOBE16(SAMPLE_CLASS_ID);
	match.tun.geneve_options[9].type = 3;
	match.tun.geneve_options[9].length = 4;
	match_mask.tun.geneve_options[9].class_id = 0xffff;
	match_mask.tun.geneve_options[9].type = 0xff;
	/*
	 * Indexes 10-13 describe the option data, index 13 describes the last DW in data (the 4th).
	 * Make data as changeable by cover all data (4 DWs).
	 */
	match.tun.geneve_options[10].data = CHANGEABLE_32;
	match.tun.geneve_options[11].data = CHANGEABLE_32;
	match.tun.geneve_options[12].data = CHANGEABLE_32;
	match.tun.geneve_options[13].data = CHANGEABLE_32;
	/* Mask the only DW we want to match */
	match_mask.tun.geneve_options[13].data = FULL_MASK_32;

	actions1.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions1.decap_cfg.is_l2 = true;

	actions2.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions2.decap_cfg.is_l2 = false;
	/* Append eth header after decap GENEVE L3 tunnel */
	SET_MAC_ADDR(actions2.decap_cfg.eth.src_mac,
		     mac_addr[0],
		     mac_addr[1],
		     mac_addr[2],
		     mac_addr[3],
		     mac_addr[4],
		     mac_addr[5]);
	SET_MAC_ADDR(actions2.decap_cfg.eth.dst_mac,
		     mac_addr[0],
		     mac_addr[1],
		     mac_addr[2],
		     mac_addr[3],
		     mac_addr[4],
		     mac_addr[5]);
	actions2.decap_cfg.eth.type = DOCA_HTOBE16(CHANGEABLE_16);

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "GENEVE_OPT_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries with example GENEVE VNI to match
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_geneve_opt_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t mac2[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.tun.geneve.vni = BUILD_VNI(0xabcdef);
	/* First option - data example */
	match.tun.geneve_options[4].data = DOCA_HTOBE32(0x00abcdef);
	/* Second option - data example */
	match.tun.geneve_options[7].data = DOCA_HTOBE32(0x00abcdef);
	match.tun.geneve_options[8].data = DOCA_HTOBE32(0x00abcdef);
	/* Third option - data example */
	match.tun.geneve_options[13].data = DOCA_HTOBE32(0x00abcdef);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	match.tun.geneve.vni = BUILD_VNI(0x123456);
	/* First option - data example */
	match.tun.geneve_options[4].data = DOCA_HTOBE32(0x00123456);
	/* Second option - data example */
	match.tun.geneve_options[7].data = DOCA_HTOBE32(0x00123456);
	match.tun.geneve_options[8].data = DOCA_HTOBE32(0x00123456);
	/* Third option - data example */
	match.tun.geneve_options[13].data = DOCA_HTOBE32(0x00123456);
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac, mac1[0], mac1[1], mac1[2], mac1[3], mac1[4], mac1[5]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac, mac2[0], mac2[1], mac2[2], mac2[3], mac2[4], mac2[5]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV6);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 1, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	match.tun.geneve.vni = BUILD_VNI(0x778899);
	/* First option - data example */
	match.tun.geneve_options[4].data = DOCA_HTOBE32(0x00778899);
	/* Second option - data example */
	match.tun.geneve_options[7].data = DOCA_HTOBE32(0x00778899);
	match.tun.geneve_options[8].data = DOCA_HTOBE32(0x00778899);
	/* Third option - data example */
	match.tun.geneve_options[13].data = DOCA_HTOBE32(0x00778899);
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac, mac1[5], mac1[4], mac1[3], mac1[2], mac1[1], mac1[0]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac, mac2[5], mac2[4], mac2[3], mac2[2], mac2[1], mac2[0]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 1, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Run flow_geneve_opt sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_geneve_opt(int nb_queues)
{
	int nb_ports = 2;
	uint8_t nb_options = 3;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_parser_geneve_opt_cfg tlv_list[nb_options];
	struct doca_flow_parser *parsers[nb_ports];
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipes[nb_ports];
	struct entries_status status;
	uint32_t num_of_entries = 3;
	doca_error_t result;
	int port_id;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_decap = num_of_entries;
	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	memset(tlv_list, 0, sizeof(tlv_list[0]) * nb_options);
	fill_parser_geneve_opt_cfg_list(tlv_list);

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));

		result = doca_flow_parser_geneve_opt_create(ports[port_id], tlv_list, nb_options, &parsers[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create geneve parser: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_geneve_opt_pipe(ports[port_id], port_id, &pipes[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add geneve opt pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_geneve_opt_pipe_entries(pipes[port_id], &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add geneve pipe match entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = flow_process_entries(ports[port_id], &status, num_of_entries);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(10);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
