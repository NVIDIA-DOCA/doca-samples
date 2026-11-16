/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stddef.h>
#include <string.h>

#include <doca_flow.h>
#include <doca_flow_definitions.h>
#include <doca_log.h>

#include <flow_common.h>

#define NB_PORTS (2)
#define NB_ENTRIES_PER_PORT (1)
#define NB_ACTIONS_ARR (1)
#define WAIT_TIME_SECONDS (5)

DOCA_LOG_REGISTER(FLOW_DEFINITION_MODE);

/* One application structure supplies both roles, with independently registered members. */
struct definition_mode_fields {
	doca_be32_t service_ip;
	doca_be32_t rewritten_src_ip;
};

/*
 * Initialize DOCA Flow with compact match and actions definitions.
 *
 * @nb_queues [in]: number of queues the sample uses
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t init_definition_mode(int nb_queues)
{
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_definitions_cfg *defs_cfg = NULL;
	struct doca_flow_definitions *definitions = NULL;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_cfg_set_mode(defs_cfg, DOCA_FLOW_DEFINITIONS_MODE_APP_DEFINED);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable flat definitions mode: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_create(defs_cfg, &definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions object: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_cfg_destroy(defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy definitions configuration: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	defs_cfg = NULL;

	result = doca_flow_definitions_add_field(definitions,
						 "match.packet.outer.ipv4.dst_ip",
						 offsetof(struct definition_mode_fields, service_ip),
						 sizeof(((struct definition_mode_fields *)0)->service_ip));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add compact IPv4 destination match: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "actions.packet.outer.ipv4.src_ip",
						 offsetof(struct definition_mode_fields, rewritten_src_ip),
						 sizeof(((struct definition_mode_fields *)0)->rewritten_src_ip));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add compact IPv4 source action: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = init_doca_flow_with_defs(nb_queues, "vnf", &resource, nr_shared_resources, definitions);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to initialize DOCA Flow in flat definitions mode: %s",
			     doca_error_get_descr(result));

cleanup:
	if (definitions != NULL)
		doca_flow_definitions_destroy(definitions);
	if (defs_cfg != NULL)
		doca_flow_definitions_cfg_destroy(defs_cfg);
	return result;
}

/*
 * Create a basic pipe whose compact templates match an IPv4 destination and rewrite an IPv4 source.
 *
 * @port [in]: pipe port
 * @port_id [in]: pipe port ID
 * @pipe [out]: created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_definition_mode_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct definition_mode_fields fields = {.service_ip = UINT32_MAX, .rewritten_src_ip = UINT32_MAX};
	struct definition_mode_fields mask = {.service_ip = UINT32_MAX, .rewritten_src_ip = UINT32_MAX};
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR] = {(struct doca_flow_actions *)&fields};
	struct doca_flow_actions *actions_mask_arr[NB_ACTIONS_ARR] = {(struct doca_flow_actions *)&mask};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe configuration: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "FLAT_IPV4_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* Existing typed APIs require explicit casts for compact buffers. */
	result = doca_flow_pipe_cfg_set_match(pipe_cfg,
					      (struct doca_flow_match *)&fields,
					      (struct doca_flow_match *)&mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set compact pipe match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_mask_arr, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set compact pipe actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;
	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create compact pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add an entry matching 8.8.8.8 and rewriting the IPv4 source to 192.0.2.1.
 *
 * @pipe [in]: pipe receiving the entry
 * @status [in]: asynchronous entry status
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_definition_mode_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct definition_mode_fields fields = {
		.service_ip = BE_IPV4_ADDR(8, 8, 8, 8),
		.rewritten_src_ip = BE_IPV4_ADDR(192, 0, 2, 1),
	};
	struct doca_flow_pipe_entry *entry;

	return doca_flow_pipe_basic_add_entry(0,
					      pipe,
					      (struct doca_flow_match *)&fields,
					      0,
					      (struct doca_flow_actions *)&fields,
					      NULL,
					      NULL,
					      DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH,
					      status,
					      &entry);
}

/*
 * Run the flat definitions mode sample.
 *
 * @nb_queues [in]: number of queues the sample uses
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_definition_mode(int nb_queues)
{
	struct doca_flow_port *ports[NB_PORTS];
	uint32_t actions_mem_size[NB_PORTS];
	struct entries_status status;
	struct doca_flow_pipe *pipe;
	doca_error_t cleanup_result;
	doca_error_t result;
	int port_id;

	result = init_definition_mode(nb_queues);
	if (result != DOCA_SUCCESS)
		return result;

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NB_ENTRIES_PER_PORT));
	result = init_doca_flow_vnf_ports(NB_PORTS, ports, actions_mem_size, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA Flow ports: %s", doca_error_get_descr(result));
		destroy_doca_flow();
		return result;
	}

	for (port_id = 0; port_id < NB_PORTS; port_id++) {
		memset(&status, 0, sizeof(status));
		result = create_definition_mode_pipe(ports[port_id], port_id, &pipe);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = add_definition_mode_entry(pipe, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add compact entry: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		result = doca_flow_pipe_queue_drain(ports[port_id], 0, NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process compact entry: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		if (status.nb_processed != NB_ENTRIES_PER_PORT || status.failure) {
			DOCA_LOG_ERR("Compact entry processing failed");
			result = DOCA_ERROR_BAD_STATE;
			goto cleanup;
		}
	}

	flow_wait_for_packets(WAIT_TIME_SECONDS, NULL, NULL);

cleanup:
	cleanup_result = stop_doca_flow_ports(NB_PORTS, ports);
	destroy_doca_flow();
	DOCA_ERROR_PROPAGATE(result, cleanup_result);
	return result;
}
