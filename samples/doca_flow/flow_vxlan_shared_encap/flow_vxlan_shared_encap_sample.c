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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_bitfield.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_VXLAN_SHARED_ENCAP);

#define ENCAP_RESOURCE_NUM 2

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
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = 0xffffffff;
	match.outer.ip4.dst_ip = 0xffffffff;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TRANSPORT;
	match.outer.transport.src_port = 0xffff;
	match.outer.transport.dst_port = 0xffff;

	/* set meta data to match on the egress domain */
	actions.meta.pkt_meta = UINT32_MAX;
	actions.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TRANSPORT;
	actions.outer.transport.src_port = 0xffff;
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
static doca_error_t create_vxlan_shared_encap_pipe(struct doca_flow_port *port,
						   int port_id,
						   uint32_t shared_encap_id,
						   struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* match on pkt meta */
	match_mask.meta.pkt_meta = UINT32_MAX;

	/* build basic outer VXLAN encap data*/
	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	actions.shared_encap_id = shared_encap_id;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "VXLAN_ENCAP_PIPE", DOCA_FLOW_PIPE_BASIC, true);
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
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
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
static doca_error_t add_match_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	doca_be32_t dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8);
	doca_be32_t src_ip_addr = BE_IPV4_ADDR(1, 2, 3, 4);
	doca_be16_t dst_port = DOCA_HTOBE16(80);
	doca_be16_t src_port = DOCA_HTOBE16(1234);

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.outer.ip4.dst_ip = dst_ip_addr;
	match.outer.ip4.src_ip = src_ip_addr;
	match.outer.transport.dst_port = dst_port;
	match.outer.transport.src_port = src_port;

	actions.meta.pkt_meta = DOCA_HTOBE32(1);
	actions.outer.transport.src_port = DOCA_HTOBE16(1235);

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
static doca_error_t add_vxlan_shared_encap_pipe_entry(struct doca_flow_pipe *pipe,
						      uint32_t shared_encap_id,
						      struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.meta.pkt_meta = DOCA_HTOBE32(1);

	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	actions.shared_encap_id = shared_encap_id;

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Creat encap action
 *
 * @encap [in]: encap action
 * @id [in]: encap resource id
 */
static void create_encap_action(struct doca_flow_encap_action *encap, uint32_t id)
{
	doca_be32_t encap_dst_ip_addr = BE_IPV4_ADDR(81, 81, 81, 81);
	doca_be32_t encap_src_ip_addr = BE_IPV4_ADDR(11, 21, 31, 41);
	uint8_t encap_ttl = 17;
	doca_be32_t encap_vxlan_tun_id = DOCA_HTOBE32(0xadadad);
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	if (id == 1) {
		encap_dst_ip_addr = BE_IPV4_ADDR(82, 82, 82, 82);
		encap_src_ip_addr = BE_IPV4_ADDR(12, 22, 32, 42);
		encap_ttl = 27;
		encap_vxlan_tun_id = DOCA_HTOBE32(0xaeaeae);
	}

	SET_MAC_ADDR(encap->outer.eth.src_mac, src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
	SET_MAC_ADDR(encap->outer.eth.dst_mac, dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);
	encap->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	encap->outer.ip4.src_ip = encap_src_ip_addr;
	encap->outer.ip4.dst_ip = encap_dst_ip_addr;
	encap->outer.ip4.ttl = encap_ttl;
	encap->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	encap->outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
	encap->tun.type = DOCA_FLOW_TUN_VXLAN;
	encap->tun.vxlan_tun_id = encap_vxlan_tun_id;
}

/*
 * Creat encap action
 *
 * @return: 0 on success and negative value otherwise
 */
static int create_shared_resource_encap(struct doca_flow_port *port, uint32_t *id, uint32_t port_id)
{
	struct doca_flow_shared_resource_cfg res_cfg = {0};
	int ret;

	create_encap_action(&res_cfg.encap_cfg.encap, port_id);
	res_cfg.encap_cfg.is_l2 = true;

	ret = doca_flow_port_shared_resource_get(port, DOCA_FLOW_SHARED_RESOURCE_ENCAP, id);
	if (ret)
		return ret;
	return doca_flow_port_shared_resource_set_cfg(port, DOCA_FLOW_SHARED_RESOURCE_ENCAP, *id, &res_cfg);
}

/*
 * Creat flow vxlan with encap
 *
 * @nb_queues [in]: number of queues
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_vxlan_shared_encap(int nb_queues)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	uint32_t shared_encap_ids[nb_ports];
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe *egress_pipe[nb_ports];
	struct entries_status status_ingress;
	int num_of_entries_ingress = 1;
	struct entries_status status_egress;
	int num_of_entries_egress = 1;
	doca_error_t result;
	int port_id;

	resource.nr_encap = ENCAP_RESOURCE_NUM + 1;
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
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

	for (int i = 0; i < nb_ports; i++) {
		result = create_shared_resource_encap(ports[i], &shared_encap_ids[i], i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Create shared_encap resource failed: ret %d", result);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status_egress, 0, sizeof(status_egress));

		result = create_vxlan_shared_encap_pipe(ports[port_id],
							port_id,
							shared_encap_ids[port_id],
							&egress_pipe[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create vxlan encap pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_vxlan_shared_encap_pipe_entry(egress_pipe[port_id],
							   shared_encap_ids[port_id],
							   &status_egress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry to vxlan encap pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries_egress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		if (status_egress.nb_processed != num_of_entries_egress || status_egress.failure) {
			DOCA_LOG_ERR("Failed to process entries");
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return DOCA_ERROR_BAD_STATE;
		}
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status_ingress, 0, sizeof(status_ingress));

		result = create_match_pipe(ports[port_id], egress_pipe[port_id ^ 1], &pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create match pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_match_pipe_entry(pipe, &status_ingress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry to match pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries_ingress);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		if (status_ingress.nb_processed != num_of_entries_ingress || status_ingress.failure) {
			DOCA_LOG_ERR("Failed to process entries");
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return DOCA_ERROR_BAD_STATE;
		}
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(10);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
