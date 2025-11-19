/*
 * Copyright (c) 2025-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <flow_common.h>
static struct doca_flow_port *ports[2];
static int nb_ports = 2;

DOCA_LOG_REGISTER(FLOW_GTP);

#define CHECK_DOCA_SUCCESS(op, msg) \
	do { \
		result = (op); \
		if (result != DOCA_SUCCESS) { \
			DOCA_LOG_ERR("Failed to " msg ": %s", doca_error_get_descr(result)); \
			stop_doca_flow_ports(nb_ports, ports); \
			doca_flow_destroy(); \
			return result; \
		} \
	} while (0)

/*
 * Create DOCA Flow pipe with changeable match and modify gtp_ext_psc_qfi field
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
int create_modify_gtp_psc_header_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match = {0};
	struct doca_flow_match match_mask = {0};
	struct doca_flow_actions actions = {0};
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd_hit = {0};
	struct doca_flow_fwd fwd_miss = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	/* Set changeable match and match_mask on gtp header */
	match.tun.type = DOCA_FLOW_TUN_GTPU;
	match.tun.gtp_next_ext_hdr_type = UINT8_MAX;
	match.tun.gtp_ext_psc_qfi = UINT8_MAX;
	match_mask.tun.type = DOCA_FLOW_TUN_GTPU;
	match_mask.tun.gtp_next_ext_hdr_type = UINT8_MAX;
	match_mask.tun.gtp_ext_psc_qfi = UINT8_MAX;

	/* Prepare for modifying QFI value */
	actions_arr[0] = &actions;
	actions.tun.type = DOCA_FLOW_TUN_GTPU;
	actions.tun.gtp_ext_psc_qfi = UINT8_MAX;

	/* Forwarding traffic to other port */
	fwd_hit.type = DOCA_FLOW_FWD_PORT;
	fwd_hit.port_id = port_id ^ 1;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = set_flow_pipe_cfg(pipe_cfg, "MATCH_PIPE", DOCA_FLOW_PIPE_BASIC, false);
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

	result = doca_flow_pipe_create(pipe_cfg, &fwd_hit, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry that match and modify gtp_ext_psc_qfi field
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
int add_modify_gtp_psc_header_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	match.tun.type = DOCA_FLOW_TUN_GTPU;
	match.tun.gtp_next_ext_hdr_type = 0x85;
	match.tun.gtp_ext_psc_qfi = 0x01;

	actions.tun.type = DOCA_FLOW_TUN_GTPU;
	actions.tun.gtp_ext_psc_qfi = 0x3a;

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, DOCA_FLOW_NO_WAIT, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe with specific match that a gtp header is gtp psc type and add entry
 *
 * @port [in]: port of the pipe
 * @modify_qfi_pipe [in]: pipe to forward the matched packets
 * @status [in]: user context for adding entry
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_match_gtp_pipe_and_add_entry(struct doca_flow_port *port,
							struct doca_flow_pipe *modify_qfi_pipe,
							struct entries_status *status,
							struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd_hit = {0};
	struct doca_flow_fwd fwd_miss = {0};
	doca_error_t result;

	/* Set specific match on gtp header */
	match.tun.type = DOCA_FLOW_TUN_GTPU;
	match.tun.gtp_next_ext_hdr_type = 0x85;

	/* Forwarding traffic to other port or drop */
	fwd_hit.type = DOCA_FLOW_FWD_PIPE;
	fwd_hit.next_pipe = modify_qfi_pipe;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

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
	result = doca_flow_pipe_create(pipe_cfg, &fwd_hit, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create match gtp psc type pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_add_entry(0, *pipe, NULL, 0, NULL, NULL, NULL, DOCA_FLOW_NO_WAIT, status, &entry);

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Run flow_gtp sample
 * This sample has two pipes and two entries for each. The first pipe forwards the second pipe only the gtp psc packet
 * and drops the others. The second pipe modifies all packets with QFI equals 0x1 to 0x3a and forwards those packets to
 * the other port. All other packets are dropped. Eventually, only gtp psc packets with QFI equal to 0x1 will arrive at
 * the other port with QFI equal to 0x3a, and all the other packets are dropped.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_gtp(int nb_queues)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *modify_qfi_pipe, *gtp_pipe;
	struct entries_status status = {0};
	int num_of_entries = 2;
	doca_error_t result;
	int port_id;

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

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));
		CHECK_DOCA_SUCCESS(create_modify_gtp_psc_header_pipe(ports[port_id], port_id, &modify_qfi_pipe),
				   "create modify GTP PSC header pipe");

		CHECK_DOCA_SUCCESS(add_modify_gtp_psc_header_entry(modify_qfi_pipe, &status),
				   "add GTP PSC header entry");

		CHECK_DOCA_SUCCESS(
			create_match_gtp_pipe_and_add_entry(ports[port_id], modify_qfi_pipe, &status, &gtp_pipe),
			"create match gtp pipe");

		CHECK_DOCA_SUCCESS(flow_process_entries(ports[port_id], &status, num_of_entries), "process entries");
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(5);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
