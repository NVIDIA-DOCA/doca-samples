/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_flow.h>
#include <doca_log.h>
#include <doca_bitfield.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_CONTROL_PIPE);

#define NB_ACTION_DESC (1)
#define DEFAULT_CTRL_PIPE_SIZE (8192)

/*
 * Create DOCA Flow pipe that match VXLAN traffic with changeable VXLAN tunnel ID and decap action
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_vxlan_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	match.tun.type = DOCA_FLOW_TUN_VXLAN;
	match.tun.vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_STANDARD;
	match.tun.vxlan_tun_id = 0xffffffff;

	actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.decap_cfg.is_l2 = true;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "VXLAN_PIPE", DOCA_FLOW_PIPE_BASIC, false);
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry with example VXLAN tunnel ID to match
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_vxlan_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;

	doca_be32_t vxlan_tun_id = DOCA_HTOBE32(0xcdab12);
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.tun.vxlan_tun_id = vxlan_tun_id;
	actions.action_idx = 0;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  &actions,
					  NULL,
					  NULL,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe that match VXLAN-GPE traffic with changeable VXLAN tunnel ID/next
 * protocol and fwd to peer port
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_vxlan_gpe_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&actions, 0, sizeof(actions));

	match.tun.type = DOCA_FLOW_TUN_VXLAN;
	match.tun.vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_GPE;
	match.tun.vxlan_tun_id = 0xffffffff;
	match.tun.vxlan_gpe_next_protocol = 0xff;
	match.tun.vxlan_gpe_flags = 0xff;

	actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.decap_cfg.is_l2 = false;
	/* append eth header after decap vxlan-gpe tunnel for case next_proto is IPV4 */
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "VXLAN_PIPE_GPE", DOCA_FLOW_PIPE_BASIC, false);
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry with example VXLAN-GPE tunnel ID to match
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_vxlan_gpe_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_entry *entry;

	doca_be32_t vxlan_tun_id = DOCA_HTOBE32(0xcdab12);
	doca_error_t result;

	memset(&match, 0, sizeof(match));

	match.tun.vxlan_tun_id = vxlan_tun_id;
	match.tun.vxlan_gpe_next_protocol = DOCA_FLOW_VXLAN_GPE_TYPE_IPV4;
	match.tun.vxlan_gpe_flags = 0xF;

	result = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, DOCA_FLOW_WAIT_FOR_BATCH, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe that match MPLS traffic with changeable MPLS tunnel ID and decap action
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_mpls_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.decap_cfg.is_l2 = false;
	/* append eth header after decap MPLS tunnel */
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	actions_arr[0] = &actions;

	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_MPLS_DEFAULT_PORT);
	match.tun.type = DOCA_FLOW_TUN_MPLS_O_UDP;
	match.tun.mpls[2].label = 0xffffffff;

	result = doca_flow_mpls_label_encode(0xfffff, 0, 0, true, &match_mask.tun.mpls[2]);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MPLS_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry with example MPLS tunnel ID to match
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_mpls_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	actions.action_idx = 0;

	result = doca_flow_mpls_label_encode(0xababa, 0, 0, true, &match.tun.mpls[2]);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match,
					  &actions,
					  NULL,
					  NULL,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe that match GRE traffic with changeable GRE key and decap action
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_gre_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint8_t src_mac[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	uint8_t dst_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	match.tun.type = DOCA_FLOW_TUN_GRE;
	match.tun.key_present = true;
	match.tun.gre_key = 0xffffffff;

	actions.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.decap_cfg.is_l2 = false;

	/* append eth header after decap GRE tunnel */
	SET_MAC_ADDR(actions.decap_cfg.eth.src_mac,
		     src_mac[0],
		     src_mac[1],
		     src_mac[2],
		     src_mac[3],
		     src_mac[4],
		     src_mac[5]);
	SET_MAC_ADDR(actions.decap_cfg.eth.dst_mac,
		     dst_mac[0],
		     dst_mac[1],
		     dst_mac[2],
		     dst_mac[3],
		     dst_mac[4],
		     dst_mac[5]);
	actions.decap_cfg.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "GRE_PIPE", DOCA_FLOW_PIPE_BASIC, false);
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry with example GRE key to match
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_gre_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_be32_t gre_key = DOCA_HTOBE32(900);
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.tun.gre_key = gre_key;
	actions.action_idx = 0;

	result = doca_flow_pipe_add_entry(0, pipe, &match, &actions, NULL, NULL, DOCA_FLOW_NO_WAIT, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Creates a DOCA Flow pipe and entry that matches NVGRE traffic with changeable vs_id/flow_id,
 * and forwards packets to the other port.
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @status [in]: user context for adding entry
 * @num_of_entries [in]: total entry number
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_nvgre_pipe_and_entry(struct doca_flow_port *port,
						int port_id,
						struct entries_status *status,
						int *num_of_entries,
						struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	match.tun.type = DOCA_FLOW_TUN_GRE;
	match.tun.gre_type = DOCA_FLOW_TUN_EXT_GRE_NVGRE;
	match.tun.protocol = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_TEB);
	match.tun.nvgre_vs_id = 0xffffffff;
	match.tun.nvgre_flow_id = 0xff;
	match.inner.eth.type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
	match.inner.ip4.next_proto = DOCA_FLOW_PROTO_UDP;
	match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.inner.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.inner.udp.l4_port.src_port = DOCA_HTOBE16(1111);

	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "NVGRE_PIPE", DOCA_FLOW_PIPE_BASIC, false);
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create nvgre pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	match.tun.nvgre_vs_id = DOCA_HTOBE32((uint32_t)0x123456 << 8);
	match.tun.nvgre_flow_id = 0x78;
	actions.action_idx = 0;

	result = doca_flow_pipe_add_entry(0, *pipe, &match, &actions, NULL, NULL, DOCA_FLOW_NO_WAIT, status, &entry);
	if (result == DOCA_SUCCESS) {
		(*num_of_entries)++;
	}
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow control pipe
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_control_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "CONTROL_PIPE", DOCA_FLOW_PIPE_CONTROL, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow control entry jump to the NVGRE pipe
 *
 * @control_pipe [in]: the control pipe
 * @nvgre_pipe [in]: the nvgre pipe
 * @status [in]: user context for adding entry
 * @priority [in]: the priority of the new entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t control_add_nvgre_entry(struct doca_flow_pipe *control_pipe,
					    struct doca_flow_pipe *nvgre_pipe,
					    struct entries_status *status,
					    uint8_t priority)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.next_proto = DOCA_FLOW_PROTO_GRE;
	match.tun.type = DOCA_FLOW_TUN_GRE;
	match.tun.gre_type = DOCA_FLOW_TUN_EXT_GRE_STANDARD;
	match.tun.protocol = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_TEB);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = nvgre_pipe;

	return doca_flow_pipe_control_add_entry(0,
						priority,
						control_pipe,
						&match,
						NULL,
						NULL,
						NULL,
						NULL,
						NULL,
						NULL,
						&fwd,
						status,
						NULL);
}

/*
 * Add DOCA Flow pipe entries to the control pipe:
 * - entry with VXLAN match that forward the matched packet to vxlan_pipe
 * - entry with VXLAN-GPE match that forward the matched packet to vxlan_gpe_pipe
 * - entry with MPLS match that forward the matched packet to mpls_pipe
 * - entry with GRE match that forward the matched packet to gre_pipe
 *
 * @control_pipe [in]: pipe of the entry
 * @vxlan_pipe [in]: pipe to forward VXLAN traffic
 * @vxlan_gpe_pipe [in]: pipe to forward VXLAN-GPE traffic
 * @mpls_pipe [in]: pipe to forward MPLS traffic
 * @gre_pipe [in]: pipe to forward GRE traffic
 * @nvgre_pipe [in]: pipe to forward NVGRE traffic
 * @status [in]: user context for adding entry
 * @num_of_entries [in]: total entry number
 * @shared_counter_id [in]: shared counter id
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_control_pipe_entries(struct doca_flow_pipe *control_pipe,
					     struct doca_flow_pipe *vxlan_pipe,
					     struct doca_flow_pipe *vxlan_gpe_pipe,
					     struct doca_flow_pipe *mpls_pipe,
					     struct doca_flow_pipe *gre_pipe,
					     struct doca_flow_pipe *nvgre_pipe,
					     struct entries_status *status,
					     int *num_of_entries,
					     int shared_counter_id)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct doca_flow_monitor monitor;
	uint8_t priority = 0;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	/* set shared counter ID */
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
	monitor.shared_counter.shared_counter_id = shared_counter_id;

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_VXLAN_GPE_DEFAULT_PORT);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = vxlan_gpe_pipe;

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  control_pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  &fwd,
						  status,
						  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	(*num_of_entries)++;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = vxlan_pipe;

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  control_pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  &fwd,
						  status,
						  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	(*num_of_entries)++;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_MPLS_DEFAULT_PORT);

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = mpls_pipe;

	result = doca_flow_pipe_control_add_entry(0,
						  priority,
						  control_pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  &fwd,
						  status,
						  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	(*num_of_entries)++;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.next_proto = DOCA_FLOW_PROTO_GRE;

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = gre_pipe;

	result = doca_flow_pipe_control_add_entry(0,
						  priority + 1,
						  control_pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  &monitor,
						  &fwd,
						  status,
						  NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	(*num_of_entries)++;

	if (nvgre_pipe) {
		result = control_add_nvgre_entry(control_pipe, nvgre_pipe, status, priority);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add control pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
		(*num_of_entries)++;
	}
	return DOCA_SUCCESS;
}

/*
 * Run flow_control_pipe sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_control_pipe(int nb_queues)
{
	int nb_ports = 2;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *vxlan_pipe;
	struct doca_flow_pipe *vxlan_gpe_pipe;
	struct doca_flow_pipe *mpls_pipe;
	struct doca_flow_pipe *gre_pipe;
	struct doca_flow_pipe *nvgre_pipe;
	struct doca_flow_pipe *control_pipe;
	struct entries_status status;
	int num_of_entries = 0;
	doca_error_t result;
	int port_id;
	uint32_t shared_counter_ids[] = {0, 1};
	struct doca_flow_shared_resource_cfg cfg = {0};
	struct doca_flow_resource_query query_results_array[nb_ports];

	nr_shared_resources[DOCA_FLOW_SHARED_RESOURCE_COUNTER] = 2;
	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(DEFAULT_CTRL_PIPE_SIZE));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	for (port_id = 0; port_id < nb_ports; port_id++, num_of_entries = 0) {
		memset(&status, 0, sizeof(status));

		/* config and bind shared counter to port */
		result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_COUNTER, port_id, &cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure shared counter to port %d", port_id);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_COUNTER,
							 &shared_counter_ids[port_id],
							 1,
							 ports[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to bind shared counter to pipe");
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_vxlan_pipe(ports[port_id], port_id, &vxlan_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add vxlan pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_vxlan_pipe_entry(vxlan_pipe, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add vxlan pipe entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		num_of_entries++;

		result = create_vxlan_gpe_pipe(ports[port_id], port_id, &vxlan_gpe_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add vxlan gpe pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_vxlan_gpe_pipe_entry(vxlan_gpe_pipe, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add vxlan gpe pipe entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		num_of_entries++;

		result = create_mpls_pipe(ports[port_id], port_id, &mpls_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add mpls pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_mpls_pipe_entry(mpls_pipe, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add mpls pipe entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		num_of_entries++;

		result = create_gre_pipe(ports[port_id], port_id, &gre_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add gre pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_gre_pipe_entry(gre_pipe, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add gre pipe entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		num_of_entries++;

		result = create_nvgre_pipe_and_entry(ports[port_id], port_id, &status, &num_of_entries, &nvgre_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add gre pipe or entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = create_control_pipe(ports[port_id], &control_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create control pipe: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = add_control_pipe_entries(control_pipe,
						  vxlan_pipe,
						  vxlan_gpe_pipe,
						  mpls_pipe,
						  gre_pipe,
						  nvgre_pipe,
						  &status,
						  &num_of_entries,
						  shared_counter_ids[port_id]);
		if (result != DOCA_SUCCESS) {
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}

		if (status.nb_processed != num_of_entries || status.failure) {
			DOCA_LOG_ERR("Failed to process entries");
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return DOCA_ERROR_BAD_STATE;
		}
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(10);

	result = doca_flow_shared_resources_query(DOCA_FLOW_SHARED_RESOURCE_COUNTER,
						  shared_counter_ids,
						  query_results_array,
						  nb_ports);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		DOCA_LOG_INFO("Port %d:", port_id);
		DOCA_LOG_INFO("Total bytes: %ld", query_results_array[port_id].counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_results_array[port_id].counter.total_pkts);
	}

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
