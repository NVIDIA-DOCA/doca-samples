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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>
#include "doca_flow_definitions.h"

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_PSP_EXT);

#define PSP_FIELD_SIZE(field) sizeof(((struct psp_ext_header *)0)->field)
#define PSP_FIELD_MATCH_OFF(field) offsetof(struct definition_match, psp_ext.field)
#define PSP_FIELD_ACTIONS_OFF(field) offsetof(struct definition_actions, psp_ext.field)

enum {
	WIRE_PORT = 0,	    /* Port ID of the switch manager (PF) */
	FIRST_VM_PORT = 1,  /* Port ID of the first representor (VF/SF) */
	SECOND_VM_PORT = 2, /* Port ID of the second representor (VF/SF) */
};

struct psp_ext_header {
	doca_be32_t dw6;
	doca_be32_t dw7;
	doca_be32_t dw8;
	doca_be32_t dw9;
};

struct definition_match {
	struct doca_flow_match base;
	/* Legacy match structure - must be first field in this structure */
	struct psp_ext_header psp_ext;
	/* PSP extension structure, its fields are defined by definitions API */
};

struct definition_actions {
	struct doca_flow_actions base;
	/* Legacy actions structure - must be first field in this structure */
	struct psp_ext_header psp_ext;
	/* PSP extension structure, its fields are defined by definitions API */
};

/*
 * The DOCA Flow API expects pointers to the legacy match/actions structures.
 * To maintain binary compatibility, the "base" field must remain the first
 * member of the extended structures. This guarantees that casting between
 * legacy and extended structures is valid.
 */
static_assert(offsetof(struct definition_match, base) == 0,
	      "definition_match: 'base' must be the first field to preserve compatibility with legacy doca_flow_match");
static_assert(
	offsetof(struct definition_actions, base) == 0,
	"definition_actions: 'base' must be the first field to preserve compatibility with legacy doca_flow_actions");

/*
 * Initialize DOCA Flow with PSP extension fields as a definitions.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t init_doca_flow_with_psp_ext(int nb_queues)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_definitions_cfg *defs_cfg;
	struct doca_flow_definitions *definitions;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions configuration structure: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_create(defs_cfg, &definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create definitions object: %s", doca_error_get_descr(result));
		doca_flow_definitions_cfg_destroy(defs_cfg);
		return result;
	}

	result = doca_flow_definitions_cfg_destroy(defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy definitions configuration structure: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "match.packet.tunnel.psp.dw6",
						 PSP_FIELD_MATCH_OFF(dw6),
						 PSP_FIELD_SIZE(dw6));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add match dword 6 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "match.packet.tunnel.psp.dw7",
						 PSP_FIELD_MATCH_OFF(dw7),
						 PSP_FIELD_SIZE(dw7));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add match dword 7 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "match.packet.tunnel.psp.dw8",
						 PSP_FIELD_MATCH_OFF(dw8),
						 PSP_FIELD_SIZE(dw8));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add match dword 8 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "match.packet.tunnel.psp.dw9",
						 PSP_FIELD_MATCH_OFF(dw9),
						 PSP_FIELD_SIZE(dw9));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add match dword 9 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "actions.packet.tunnel.psp.dw6",
						 PSP_FIELD_ACTIONS_OFF(dw6),
						 PSP_FIELD_SIZE(dw6));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add actions dword 6 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "actions.packet.tunnel.psp.dw7",
						 PSP_FIELD_ACTIONS_OFF(dw7),
						 PSP_FIELD_SIZE(dw7));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add actions dword 7 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "actions.packet.tunnel.psp.dw8",
						 PSP_FIELD_ACTIONS_OFF(dw8),
						 PSP_FIELD_SIZE(dw8));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add actions dword 8 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = doca_flow_definitions_add_field(definitions,
						 "actions.packet.tunnel.psp.dw9",
						 PSP_FIELD_ACTIONS_OFF(dw9),
						 PSP_FIELD_SIZE(dw9));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add actions dword 9 field: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	result = init_doca_flow_with_defs(nb_queues, "switch", &resource, nr_shared_resources, definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		doca_flow_definitions_destroy(definitions);
		return result;
	}

	doca_flow_definitions_destroy(definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy definitions object: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Set configurations and create root basic pipe.
 *
 * @port [in]: pointer to the port contains this pipe.
 * @match [in]: match structure of the pipe.
 * @actions [in]: actions structure of the pipe.
 * @fwd [in]: forward structure of the pipe.
 * @nr_entries [in]: maximal number of pipe entries.
 * @pipe [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port,
				     struct doca_flow_match *match,
				     struct doca_flow_actions *actions,
				     struct doca_flow_fwd *fwd,
				     uint32_t nr_entries,
				     struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "psp_ext_pipe", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nr_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, &actions, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe matching PSP extension fields and modify them.
 * Match only traffic from the wire which have PSP header with at least 4 extension DWs.
 *
 * @port [in]: pointer to the port contains this pipe.
 * @status [in]: user context for adding entries.
 * @pipe [out]: created pipe pointer.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_pipe(struct doca_flow_port *port, struct entries_status *status, uint32_t nr_entries)
{
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct definition_match match;
	struct definition_actions actions;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* Match specific value to filter only packets from wire */
	match.base.parser_meta.port_id = WIRE_PORT;
	/* Match specific value this is UDP packet */
	match.base.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	/* Match specific value this is PSP packet */
	match.base.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.base.outer.udp.l4_port.dst_port = DOCA_HTOBE16(DOCA_FLOW_PSP_DEFAULT_PORT);
	/* Match specific value the DW6-8 exist in the packet */
	match.base.tun.type = DOCA_FLOW_TUN_PSP;
	match.base.tun.psp.hdrextlen = 4;
	/* Match DW6-8 values - changeable per entry */
	match.psp_ext.dw6 = 0xffffffff;
	match.psp_ext.dw7 = 0xffffffff;
	match.psp_ext.dw8 = 0xffffffff;
	match.psp_ext.dw9 = 0xffffffff;

	/*
	 * Set DW6-8 new values - changeable per entry.
	 * The `actions.base.tun.type = DOCA_FLOW_TUN_PSP` selectore isn't needed for PSP extension fields.
	 */
	actions.psp_ext.dw6 = 0xffffffff;
	actions.psp_ext.dw7 = 0xffffffff;
	actions.psp_ext.dw8 = 0xffffffff;
	actions.psp_ext.dw9 = 0xffffffff;

	/* Port ID to forward to is defined per entry */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 0xffff;

	result = create_root_pipe(port, &match.base, &actions.base, &fwd, nr_entries, &pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PSP root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	/* Update values to match in first entry */
	match.psp_ext.dw6 = DOCA_HTOBE32(0x12345678);
	match.psp_ext.dw7 = DOCA_HTOBE32(0x12345678);
	match.psp_ext.dw8 = DOCA_HTOBE32(0x12345678);
	match.psp_ext.dw9 = DOCA_HTOBE32(0x12345678);
	/* Update values to set in first entry */
	actions.psp_ext.dw6 = DOCA_HTOBE32(0x22222221);
	actions.psp_ext.dw7 = DOCA_HTOBE32(0x33333331);
	actions.psp_ext.dw8 = DOCA_HTOBE32(0x44444441);
	actions.psp_ext.dw9 = DOCA_HTOBE32(0x55555551);
	/* First entry packets go to the first VF/SF */
	fwd.port_id = FIRST_VM_PORT;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match.base,
					  0,
					  &actions.base,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PSP root pipe first entry: %s", doca_error_get_descr(result));
		return result;
	}

	/* Update values to match in second entry */
	match.psp_ext.dw6 = DOCA_HTOBE32(0xabcdef90);
	match.psp_ext.dw7 = DOCA_HTOBE32(0xabcdef90);
	match.psp_ext.dw8 = DOCA_HTOBE32(0xabcdef90);
	match.psp_ext.dw9 = DOCA_HTOBE32(0xabcdef90);
	/* Update values to set in second entry */
	actions.psp_ext.dw6 = DOCA_HTOBE32(0xaaaaaaa2);
	actions.psp_ext.dw7 = DOCA_HTOBE32(0xbbbbbbb2);
	actions.psp_ext.dw8 = DOCA_HTOBE32(0xccccccc2);
	actions.psp_ext.dw9 = DOCA_HTOBE32(0xddddddd2);
	/* Second entry packets go to the second VF/SF */
	fwd.port_id = SECOND_VM_PORT;

	result = doca_flow_pipe_add_entry(0,
					  pipe,
					  &match.base,
					  0,
					  &actions.base,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  status,
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PSP root pipe second entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_psp_ext sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_psp_ext(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct entries_status status;
	uint32_t nr_entries = 2;
	doca_error_t result;

	result = init_doca_flow_with_psp_ext(nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow with definitions, nr_queues=%d", nb_queues);
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(nr_entries));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size,
					     NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	memset(&status, 0, sizeof(status));
	result = create_pipe(ports[0], &status, nr_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PSP pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = flow_process_entries(ports[0], &status, nr_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(15);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
