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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_bitfield.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_GENEVE_ENCAP);

#define BUILD_VNI(uint24_vni) (DOCA_HTOBE32((uint32_t)uint24_vni << 8))

/*
 * Create DOCA Flow pipe with 5 tuple match and set pkt meta value
 *
 * @port [in]: port of the pipe
 * @dest_pipe [in]: dest pipe.
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_match_pipe(struct doca_flow_port *port,
				      struct doca_flow_pipe *dest_pipe,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* 5 tuple match */
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;
	match.outer.tcp.l4_port.src_port = 0xffff;
	match.outer.tcp.l4_port.dst_port = 0xffff;

	/* set meta data to match on the egress domain */
	actions.meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MATCH_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* forwarding traffic to other port */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = dest_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe on EGRESS domain with match on the packet meta and encap action with changeable values
 *
 * @port [in]: port of the pipe
 * @port_id [in]: pipe port ID
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_geneve_encap_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions1, actions2, actions3, actions4, *actions_arr[4];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	int i;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions1, 0, sizeof(actions1));
	memset(&actions2, 0, sizeof(actions2));
	memset(&actions3, 0, sizeof(actions3));
	memset(&actions4, 0, sizeof(actions4));
	memset(&fwd, 0, sizeof(fwd));

	/* match on pkt meta */
	match_mask.meta.pkt_meta = UINT32_MAX;

	/* build basic outer GENEVE L3 encap data */
	actions1.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions1.encap_cfg.is_l2 = false;
	SET_MAC_ADDR(actions1.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions1.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions1.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions1.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions1.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions1.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions1.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions1.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_GENEVE_DEFAULT_PORT);
	actions1.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_GENEVE;
	actions1.encap_cfg.encap.tun.geneve.vni = 0xffffffff;
	actions1.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	actions_arr[0] = &actions1;

	/* build basic outer GENEVE + options L3 encap data */
	actions2.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions2.encap_cfg.is_l2 = false;
	SET_MAC_ADDR(actions2.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions2.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions2.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions2.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions2.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions2.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions2.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions2.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_GENEVE_DEFAULT_PORT);
	actions2.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_GENEVE;
	actions2.encap_cfg.encap.tun.geneve.vni = 0xffffffff;
	actions2.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	actions2.encap_cfg.encap.tun.geneve.ver_opt_len = 5;
	for (i = 0; i < actions2.encap_cfg.encap.tun.geneve.ver_opt_len; i++)
		actions2.encap_cfg.encap.tun.geneve_options[i].data = 0xffffffff;
	actions_arr[1] = &actions2;

	/* build basic outer GENEVE L2 encap data */
	actions3.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions3.encap_cfg.is_l2 = true;
	SET_MAC_ADDR(actions3.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions3.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions3.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions3.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions3.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions3.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions3.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions3.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_GENEVE_DEFAULT_PORT);
	actions3.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_GENEVE;
	actions3.encap_cfg.encap.tun.geneve.vni = 0xffffffff;
	actions3.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_TEB);
	actions_arr[2] = &actions3;

	/* build basic outer GENEVE + options L2 encap data */
	actions4.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions4.encap_cfg.is_l2 = true;
	SET_MAC_ADDR(actions4.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions4.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions4.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions4.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions4.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions4.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions4.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_GENEVE;
	actions4.encap_cfg.encap.tun.geneve.vni = 0xffffffff;
	actions4.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions4.encap_cfg.encap.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_GENEVE_DEFAULT_PORT);
	actions4.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_TEB);
	actions4.encap_cfg.encap.tun.geneve.ver_opt_len = 5;
	for (i = 0; i < actions4.encap_cfg.encap.tun.geneve.ver_opt_len; i++)
		actions4.encap_cfg.encap.tun.geneve_options[i].data = 0xffffffff;
	actions_arr[3] = &actions4;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "GENEVE_ENCAP_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, 4);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* forwarding traffic to the wire */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry with example 5 tuple match
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_match_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 2, 3, 4);
	match.outer.tcp.l4_port.dst_port = DOCA_HTOBE16(80);
	match.outer.tcp.l4_port.src_port = DOCA_HTOBE16(1234);

	actions.meta.pkt_meta = DOCA_HTOBE32(1);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	match.outer.tcp.l4_port.src_port = DOCA_HTOBE16(2345);
	actions.meta.pkt_meta = DOCA_HTOBE32(2);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	match.outer.tcp.l4_port.src_port = DOCA_HTOBE16(3456);
	actions.meta.pkt_meta = DOCA_HTOBE32(3);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	match.outer.tcp.l4_port.src_port = DOCA_HTOBE16(4567);
	actions.meta.pkt_meta = DOCA_HTOBE32(4);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow pipe entry with example encap values
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_geneve_encap_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	doca_be32_t encap_dst_ip_addr = BE_IPV4_ADDR(81, 81, 81, 81);
	doca_be32_t encap_src_ip_addr = BE_IPV4_ADDR(11, 21, 31, 41);
	uint8_t encap_ttl = 17;
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.encap_cfg.encap.outer.ip4.src_ip = encap_src_ip_addr;
	actions.encap_cfg.encap.outer.ip4.dst_ip = encap_dst_ip_addr;
	actions.encap_cfg.encap.outer.ip4.ttl = encap_ttl;
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_GENEVE;

	/* L3 encap - GENEVE header only */
	actions.encap_cfg.encap.tun.geneve.vni = BUILD_VNI(0xadadad);
	actions.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	match.meta.pkt_meta = DOCA_HTOBE32(1);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	/* L3 encap - GENEVE header */
	actions.encap_cfg.encap.tun.geneve.vni = BUILD_VNI(0xcdcdcd);
	actions.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	actions.encap_cfg.encap.tun.geneve.ver_opt_len = 5;
	/* First option */
	actions.encap_cfg.encap.tun.geneve_options[0].class_id = DOCA_HTOBE16(0x0107);
	actions.encap_cfg.encap.tun.geneve_options[0].type = 1;
	actions.encap_cfg.encap.tun.geneve_options[0].length = 2;
	actions.encap_cfg.encap.tun.geneve_options[1].data = DOCA_HTOBE32(0x01234567);
	actions.encap_cfg.encap.tun.geneve_options[2].data = DOCA_HTOBE32(0x89abcdef);
	/* Second option */
	actions.encap_cfg.encap.tun.geneve_options[3].class_id = DOCA_HTOBE16(0x0107);
	actions.encap_cfg.encap.tun.geneve_options[3].type = 2;
	actions.encap_cfg.encap.tun.geneve_options[3].length = 1;
	actions.encap_cfg.encap.tun.geneve_options[4].data = DOCA_HTOBE32(0xabbadeba);
	match.meta.pkt_meta = DOCA_HTOBE32(2);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 1, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	/* L2 encap - GENEVE header only */
	actions.encap_cfg.encap.tun.geneve.vni = BUILD_VNI(0xefefef);
	actions.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_TEB);
	actions.encap_cfg.encap.tun.geneve.ver_opt_len = 0;
	match.meta.pkt_meta = DOCA_HTOBE32(3);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 2, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	/* L2 encap - GENEVE header */
	actions.encap_cfg.encap.tun.geneve.vni = BUILD_VNI(0x123456);
	actions.encap_cfg.encap.tun.geneve.next_proto = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_TEB);
	actions.encap_cfg.encap.tun.geneve.ver_opt_len = 5;
	/* Option header */
	actions.encap_cfg.encap.tun.geneve_options[0].class_id = DOCA_HTOBE16(0x0107);
	actions.encap_cfg.encap.tun.geneve_options[0].type = 3;
	actions.encap_cfg.encap.tun.geneve_options[0].length = 4;
	/* Option data */
	actions.encap_cfg.encap.tun.geneve_options[1].data = DOCA_HTOBE32(0x11223344);
	actions.encap_cfg.encap.tun.geneve_options[2].data = DOCA_HTOBE32(0x55667788);
	actions.encap_cfg.encap.tun.geneve_options[3].data = DOCA_HTOBE32(0x99aabbcc);
	actions.encap_cfg.encap.tun.geneve_options[4].data = DOCA_HTOBE32(0xddeeff00);
	match.meta.pkt_meta = DOCA_HTOBE32(4);

	result = doca_flow_pipe_add_entry(0, pipe, &match, 3, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Run flow_geneve_encap sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_geneve_encap(int nb_queues)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe *egress_pipe[nb_ports];
	struct entries_status status_ingress;
	uint32_t num_of_entries_ingress = 4;
	struct entries_status status_egress;
	uint32_t num_of_entries_egress = 4;
	doca_error_t result;
	int port_id;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_encap = num_of_entries_egress;
	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries_ingress + num_of_entries_egress));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		result = create_geneve_encap_pipe(ports[port_id], port_id, &egress_pipe[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create geneve encap pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status_ingress, 0, sizeof(status_ingress));
		memset(&status_egress, 0, sizeof(status_egress));

		result = create_match_pipe(ports[port_id], egress_pipe[port_id ^ 1], &pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create match pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_match_pipe_entries(pipe, &status_ingress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entries to match pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_geneve_encap_pipe_entries(egress_pipe[port_id ^ 1], &status_egress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entries to geneve encap pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = flow_process_entries(ports[port_id], &status_ingress, num_of_entries_ingress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process ingress entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = flow_process_entries(ports[port_id ^ 1], &status_egress, num_of_entries_egress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process egress entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(5);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
