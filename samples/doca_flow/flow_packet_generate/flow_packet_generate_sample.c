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
#include <stdint.h>
#include <string.h>

#include <doca_flow.h>
#include <doca_flow_definitions.h>
#include <doca_flow_external_action_packet_generate.h>
#include <doca_log.h>

#include <flow_common.h>
#include <flow_switch_common.h>

DOCA_LOG_REGISTER(FLOW_PACKET_GENERATE);

#define PACKET_GENERATE_PREFILL_SIZE 256
#define PACKET_GENERATE_NB_PREFILLS (1U << 16)
#define PACKET_GENERATE_DEFAULT_INLINE_SIZE sizeof(doca_be16_t)
#define PACKET_GENERATE_INLINE_ONLY_SIZE 14
#define PACKET_GENERATE_EXTENDED_INLINE_SIZE 16
#define PACKET_GENERATE_QP_DEPTH 128
#define PACKET_GENERATE_NB_CASES 3
#define PACKET_GENERATE_NB_PORTS 3
#define PACKET_GENERATE_MAX_ACTIONS 2
#define PACKET_GENERATE_MAX_ELEMENTS (1 + 2 * PACKET_GENERATE_MAX_ACTIONS)
#define PACKET_GENERATE_MAX_ACTION_DESCS (DOCA_FLOW_EXTERNAL_ACTION_PACKET_GENERATE_INPUT_U32_NUM * 2)
#define PACKET_GENERATE_NB_ENTRIES (2 + 2 * PACKET_GENERATE_NB_CASES)
#define PACKET_GENERATE_ACTIONS_MEM_ENTRIES 64
#define PACKET_GENERATE_WAIT_SEC 15
#define PACKET_GENERATE_FIRST_UDP_PORT 80

struct packet_generate_sample_actions {
	struct doca_flow_actions base;
	doca_be32_t input[DOCA_FLOW_EXTERNAL_ACTION_PACKET_GENERATE_INPUT_U32_NUM];
};

struct packet_generate_case_cfg {
	const char *name;
	const char *pipe_name;
	uint16_t udp_dst_port;
	uint32_t prefill_size;
	uint32_t nb_prefills;
	uint32_t inline_data_size;
	uint32_t dynamic_inline_size;
};

static const struct packet_generate_case_cfg packet_generate_cases[PACKET_GENERATE_NB_CASES] = {
	{
		.name = "prefill + short inline (one WQE)",
		.pipe_name = "PACKET_GENERATE_ORDERED_LIST_DEFAULT",
		.udp_dst_port = PACKET_GENERATE_FIRST_UDP_PORT,
		.prefill_size = PACKET_GENERATE_PREFILL_SIZE,
		.nb_prefills = PACKET_GENERATE_NB_PREFILLS,
		.inline_data_size = PACKET_GENERATE_DEFAULT_INLINE_SIZE,
		.dynamic_inline_size = PACKET_GENERATE_DEFAULT_INLINE_SIZE,
	},
	{
		.name = "inline only (one WQE)",
		.pipe_name = "PACKET_GENERATE_ORDERED_LIST_INLINE_ONLY",
		.udp_dst_port = PACKET_GENERATE_FIRST_UDP_PORT + 1,
		.prefill_size = 0,
		.nb_prefills = 0,
		.inline_data_size = PACKET_GENERATE_INLINE_ONLY_SIZE,
		.dynamic_inline_size = sizeof(doca_be16_t),
	},
	{
		.name = "prefill + extended inline (two WQEs)",
		.pipe_name = "PACKET_GENERATE_ORDERED_LIST_TWO_WQE",
		.udp_dst_port = PACKET_GENERATE_FIRST_UDP_PORT + 2,
		.prefill_size = PACKET_GENERATE_PREFILL_SIZE,
		.nb_prefills = PACKET_GENERATE_NB_PREFILLS,
		.inline_data_size = PACKET_GENERATE_EXTENDED_INLINE_SIZE,
		.dynamic_inline_size = PACKET_GENERATE_EXTENDED_INLINE_SIZE,
	},
};

struct packet_generate_case_runtime {
	struct doca_flow_external_resource_packet_generate *resource;
	void *pipe_actions[PACKET_GENERATE_MAX_ACTIONS];
	uint32_t nb_actions;
};

struct packet_generate_stats_ctx {
	struct doca_flow_pipe_entry *default_root_entries[PACKET_GENERATE_NB_CASES];
	struct doca_flow_pipe_entry *ordered_list_entries[PACKET_GENERATE_NB_CASES];
	struct doca_flow_pipe_entry *egress_root_entry;
	struct doca_flow_pipe_entry *egress_b_entry;
};

/*
 * Create a one-entry basic pipe.
 *
 * @port [in]: port that owns the pipe
 * @name [in]: pipe name
 * @is_root [in]: whether the pipe is a domain root
 * @domain [in]: pipe domain
 * @match [in]: fixed pipe match, or NULL for match-all
 * @fwd [in]: fixed pipe forwarding
 * @nr_entries [in]: number of entries reserved by the pipe
 * @pipe [out]: created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_basic_pipe(struct doca_flow_port *port,
				      const char *name,
				      bool is_root,
				      enum doca_flow_pipe_domain domain,
				      const struct doca_flow_match *match,
				      const struct doca_flow_fwd *fwd,
				      uint32_t nr_entries,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create %s pipe cfg: %s", name, doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, name, DOCA_FLOW_PIPE_BASIC, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set %s pipe cfg: %s", name, doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nr_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set %s pipe entries: %s", name, doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, domain);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set %s pipe domain: %s", name, doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (match != NULL) {
		result = doca_flow_pipe_cfg_set_match(pipe_cfg, match, NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set %s pipe match: %s", name, doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set %s pipe monitor: %s", name, doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create %s pipe: %s", name, doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add a match-all entry to a basic pipe.
 *
 * @pipe [in]: basic pipe
 * @flags [in]: entry insertion flags
 * @status [in,out]: entry processing status
 * @entry [out]: created entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_basic_entry(struct doca_flow_pipe *pipe,
				    uint32_t flags,
				    struct entries_status *status,
				    struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match = {0};

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, flags, status, entry);
}

/*
 * Register the packet-generation input window after the base actions structure.
 *
 * @definitions [out]: definitions used to initialize DOCA Flow
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_packet_generate_definitions(struct doca_flow_definitions **definitions)
{
	struct doca_flow_definitions_cfg *cfg;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_flow_definitions_create(cfg, definitions);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;

	result = doca_flow_definitions_add_field(*definitions,
						 "actions.packet.meta.packet_generate_input",
						 offsetof(struct packet_generate_sample_actions, input),
						 sizeof(((struct packet_generate_sample_actions *)0)->input));
	if (result != DOCA_SUCCESS) {
		doca_flow_definitions_destroy(*definitions);
		*definitions = NULL;
	}

destroy_cfg:
	doca_flow_definitions_cfg_destroy(cfg);
	return result;
}

/*
 * Initialize every static prefill with alternating 0xaa and 0xbb bytes.
 *
 * @port [in]: packet-generation resource port
 * @resource [in]: packet-generation resource
 * @prefill_size [in]: bytes in each prefill
 * @nb_prefills [in]: number of prefills
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t initialize_prefills(struct doca_flow_port *port,
					struct doca_flow_external_resource_packet_generate *resource,
					uint32_t prefill_size,
					uint32_t nb_prefills)
{
	uint8_t prefill[PACKET_GENERATE_PREFILL_SIZE];
	doca_be32_t prefill_index;
	doca_error_t result;
	uint32_t i;

	if (prefill_size > sizeof(prefill)) {
		DOCA_LOG_ERR("Prefill size %u exceeds buffer size %u", prefill_size, (uint32_t)sizeof(prefill));
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (i = 0; i < prefill_size; i++)
		prefill[i] = (i & 1U) == 0 ? 0xaa : 0xbb;

	DOCA_LOG_INFO("Initializing %u packet-generation prefills", nb_prefills);
	for (i = 0; i < nb_prefills; i++) {
		prefill_index = DOCA_HTOBE32(i);
		memcpy(prefill, &prefill_index, sizeof(prefill_index));
		result = doca_flow_external_resource_packet_generate_prefill_update(port, resource, i, prefill);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize prefill %u: %s", i, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create the shared packet-generation ordered-list pipe.
 *
 * Each case owns one template index. Within a template, every ACTIONS element stages the inline
 * bytes carried by the following generate action.
 *
 * @port [in]: switch port
 * @cases [in,out]: packet-generation case runtime array
 * @pipe [out]: created ordered-list pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_packet_generate_ordered_list(
	struct doca_flow_port *port,
	struct packet_generate_case_runtime cases[PACKET_GENERATE_NB_CASES],
	struct doca_flow_pipe **pipe)
{
	struct doca_flow_action_desc desc_arrays[PACKET_GENERATE_NB_CASES][PACKET_GENERATE_MAX_ACTIONS]
						[PACKET_GENERATE_MAX_ACTION_DESCS] = {0};
	struct doca_flow_action_descs action_descs[PACKET_GENERATE_NB_CASES][PACKET_GENERATE_MAX_ACTIONS] = {0};
	struct doca_flow_ordered_list_element elements[PACKET_GENERATE_NB_CASES][PACKET_GENERATE_MAX_ELEMENTS] = {0};
	struct packet_generate_sample_actions actions[PACKET_GENERATE_NB_CASES][PACKET_GENERATE_MAX_ACTIONS] = {0};
	struct doca_flow_monitor monitors[PACKET_GENERATE_NB_CASES] = {0};
	struct doca_flow_ordered_list ordered_lists[PACKET_GENERATE_NB_CASES] = {0};
	struct doca_flow_ordered_list *ordered_list_ptrs[PACKET_GENERATE_NB_CASES] = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_PORT,
		.port_id = 1,
	};
	uint32_t case_index;
	doca_error_t result;

	*pipe = NULL;

	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		const struct packet_generate_case_cfg *case_cfg = &packet_generate_cases[case_index];
		struct packet_generate_case_runtime *case_runtime = &cases[case_index];
		uint32_t remaining = case_cfg->inline_data_size;
		uint32_t dynamic_remaining = case_cfg->dynamic_inline_size;
		uint32_t action_index;

		result = doca_flow_external_action_packet_generate_get_nb_actions(case_runtime->resource,
										  &case_runtime->nb_actions);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query case %u packet-generation action count: %s",
				     case_index,
				     doca_error_get_descr(result));
			goto destroy_pipe_actions;
		}
		if (case_runtime->nb_actions == 0 || case_runtime->nb_actions > PACKET_GENERATE_MAX_ACTIONS) {
			DOCA_LOG_ERR("Case %u action count %u exceeds sample maximum %u",
				     case_index,
				     case_runtime->nb_actions,
				     PACKET_GENERATE_MAX_ACTIONS);
			result = DOCA_ERROR_BAD_STATE;
			goto destroy_pipe_actions;
		}

		monitors[case_index].counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		elements[case_index][0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
		elements[case_index][0].monitor = &monitors[case_index];

		for (action_index = 0; action_index < case_runtime->nb_actions; action_index++) {
			struct doca_flow_external_action_packet_generate_pipe_action_cfg action_cfg = {
				.resource = case_runtime->resource,
				.action_index = action_index,
			};
			uint32_t first_dw;
			uint32_t action_bytes;
			uint32_t dynamic_action_bytes;
			uint32_t nb_dw;
			uint32_t desc_index;
			uint32_t dw;

			result = doca_flow_external_action_packet_generate_get_action_dws(case_runtime->resource,
											  action_index,
											  &first_dw,
											  &nb_dw);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query case %u action %u input DWs: %s",
					     case_index,
					     action_index,
					     doca_error_get_descr(result));
				goto destroy_pipe_actions;
			}
			if (nb_dw == 0 || first_dw + nb_dw > DOCA_FLOW_EXTERNAL_ACTION_PACKET_GENERATE_INPUT_U32_NUM) {
				DOCA_LOG_ERR("Case %u action %u returned an invalid input window",
					     case_index,
					     action_index);
				result = DOCA_ERROR_BAD_STATE;
				goto destroy_pipe_actions;
			}

			action_bytes = nb_dw * sizeof(doca_be32_t);
			if (action_bytes > remaining)
				action_bytes = remaining;
			if (action_bytes == 0 || (action_bytes % sizeof(doca_be16_t)) != 0) {
				DOCA_LOG_ERR("Case %u action %u has invalid inline byte count %u",
					     case_index,
					     action_index,
					     action_bytes);
				result = DOCA_ERROR_BAD_STATE;
				goto destroy_pipe_actions;
			}

			dynamic_action_bytes = action_bytes < dynamic_remaining ? action_bytes : dynamic_remaining;
			action_descs[case_index][action_index].nb_action_desc =
				dynamic_action_bytes / sizeof(doca_be16_t);
			action_descs[case_index][action_index].desc_array = desc_arrays[case_index][action_index];
			for (dw = first_dw;
			     dw < first_dw + (action_bytes + sizeof(doca_be32_t) - 1) / sizeof(doca_be32_t);
			     dw++)
				actions[case_index][action_index].input[dw] = UINT32_MAX;
			for (desc_index = 0; desc_index < action_descs[case_index][action_index].nb_action_desc;
			     desc_index++) {
				uint32_t desc_dw = first_dw + desc_index / 2;
				uint32_t dst_offset = desc_dw * 32 + ((desc_index & 1U) == 0 ? 16 : 0);
				struct doca_flow_action_desc *desc = &desc_arrays[case_index][action_index][desc_index];

				desc->type = DOCA_FLOW_ACTION_COPY;
				desc->field_op.src.field_string = "outer.udp.dst_port";
				desc->field_op.src.bit_offset = 0;
				desc->field_op.dst.field_string = "meta.packet_generate_input";
				desc->field_op.dst.bit_offset = dst_offset;
				desc->field_op.width = 16;
			}

			elements[case_index][1 + 2 * action_index].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
			elements[case_index][1 + 2 * action_index].actions = &actions[case_index][action_index].base;
			elements[case_index][1 + 2 * action_index].action_descs =
				&action_descs[case_index][action_index];

			result = doca_flow_external_action_packet_generate_pipe_action_create(
				&action_cfg,
				&case_runtime->pipe_actions[action_index]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create case %u packet-generation action %u: %s",
					     case_index,
					     action_index,
					     doca_error_get_descr(result));
				goto destroy_pipe_actions;
			}
			elements[case_index][2 + 2 * action_index].type =
				DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
			elements[case_index][2 + 2 * action_index].external_actions =
				case_runtime->pipe_actions[action_index];
			remaining -= action_bytes;
			dynamic_remaining -= dynamic_action_bytes;
		}

		if (remaining != 0 || dynamic_remaining != 0) {
			DOCA_LOG_ERR("Case %u leaves %u inline bytes and %u dynamic bytes unstaged",
				     case_index,
				     remaining,
				     dynamic_remaining);
			result = DOCA_ERROR_BAD_STATE;
			goto destroy_pipe_actions;
		}

		ordered_lists[case_index].idx = case_index;
		ordered_lists[case_index].size = 1 + 2 * case_runtime->nb_actions;
		ordered_lists[case_index].elements = elements[case_index];
		ordered_list_ptrs[case_index] = &ordered_lists[case_index];
	}

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ordered-list pipe cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_actions;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "PACKET_GENERATE_ORDERED_LIST", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered-list pipe cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, PACKET_GENERATE_NB_CASES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered-list entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_list_ptrs, PACKET_GENERATE_NB_CASES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered-list templates: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create packet-generation ordered-list pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	if (result == DOCA_SUCCESS)
		return DOCA_SUCCESS;
destroy_pipe_actions:
	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		struct packet_generate_case_runtime *case_runtime = &cases[case_index];

		while (case_runtime->nb_actions > 0) {
			case_runtime->nb_actions--;
			if (case_runtime->pipe_actions[case_runtime->nb_actions] != NULL) {
				doca_flow_external_action_packet_generate_pipe_action_destroy(
					case_runtime->pipe_actions[case_runtime->nb_actions]);
				case_runtime->pipe_actions[case_runtime->nb_actions] = NULL;
			}
		}
	}
	return result;
}

/*
 * Add one entry for each ordered-list template.
 *
 * @pipe [in]: ordered-list pipe
 * @cases [in]: packet-generation case runtime array
 * @status [in,out]: entry processing status
 * @entries [out]: created entries
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_packet_generate_ordered_list_entries(
	struct doca_flow_pipe *pipe,
	const struct packet_generate_case_runtime cases[PACKET_GENERATE_NB_CASES],
	struct entries_status *status,
	struct doca_flow_pipe_entry *entries[PACKET_GENERATE_NB_CASES])
{
	uint32_t case_index;

	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		struct doca_flow_monitor monitor = {
			.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
		};
		struct doca_flow_ordered_list_element elements[PACKET_GENERATE_MAX_ELEMENTS] = {0};
		struct doca_flow_ordered_list ordered_list = {
			.idx = case_index,
			.size = 1 + 2 * cases[case_index].nb_actions,
			.elements = elements,
		};
		uint32_t action_index;
		doca_error_t result;

		elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
		elements[0].monitor = &monitor;
		for (action_index = 0; action_index < cases[case_index].nb_actions; action_index++) {
			elements[1 + 2 * action_index].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
			elements[2 + 2 * action_index].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		}

		result = doca_flow_pipe_ordered_list_add_entry(0,
							       pipe,
							       case_index,
							       &ordered_list,
							       NULL,
							       DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH,
							       status,
							       &entries[case_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add ordered-list case %u entry: %s",
				     case_index,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Add one UDP-port dispatch entry per packet-generation case.
 *
 * @pipe [in]: default root pipe
 * @ordered_list_pipe [in]: shared ordered-list pipe
 * @status [in,out]: entry processing status
 * @entries [out]: created default-root entries
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_default_root_entries(struct doca_flow_pipe *pipe,
					     struct doca_flow_pipe *ordered_list_pipe,
					     struct entries_status *status,
					     struct doca_flow_pipe_entry *entries[PACKET_GENERATE_NB_CASES])
{
	uint32_t case_index;

	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		struct doca_flow_match match = {0};
		struct doca_flow_fwd fwd = {
			.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE,
			.ordered_list_pipe =
				{
					.pipe = ordered_list_pipe,
					.idx = case_index,
				},
		};
		uint32_t flags = case_index + 1 == PACKET_GENERATE_NB_CASES ? DOCA_FLOW_ENTRY_FLAGS_NO_WAIT :
									      DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH;
		doca_error_t result;

		match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(packet_generate_cases[case_index].udp_dst_port);
		result = doca_flow_pipe_basic_add_entry(0,
							pipe,
							&match,
							0,
							NULL,
							NULL,
							&fwd,
							flags,
							status,
							&entries[case_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add default-root case %u entry: %s",
				     case_index,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Print one pipe entry counter.
 *
 * @name [in]: pipe name
 * @entry [in]: pipe entry
 */
static void print_entry_counter(const char *name, struct doca_flow_pipe_entry *entry)
{
	struct doca_flow_resource_query stats = {0};
	doca_error_t result;

	result = doca_flow_resource_query_entry(entry, &stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query %s entry: %s", name, doca_error_get_descr(result));
		return;
	}

	DOCA_LOG_INFO("%s: pkts=%lu bytes=%lu",
		      name,
		      (unsigned long)stats.counter.total_pkts,
		      (unsigned long)stats.counter.total_bytes);
}

/*
 * Print packet-generation pipeline counters.
 *
 * @context [in]: packet-generation statistics context
 */
static void print_packet_generate_stats(void *context)
{
	struct packet_generate_stats_ctx *ctx = context;
	uint32_t case_index;

	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		print_entry_counter(packet_generate_cases[case_index].name, ctx->default_root_entries[case_index]);
		print_entry_counter(packet_generate_cases[case_index].pipe_name, ctx->ordered_list_entries[case_index]);
	}
	print_entry_counter("egress_root", ctx->egress_root_entry);
	print_entry_counter("egress_b", ctx->egress_b_entry);
}

/*
 * Run the packet-generation switch-mode sample.
 *
 * Topology:
 *
 *   UDP dst 80 -> prefill + 2-byte inline, one generate action
 *   UDP dst 81 -> 14-byte inline-only Ethernet frame, one generate action
 *   UDP dst 82 -> prefill + 16-byte inline, two chained generate actions
 *
 *   default root -> selected ordered list -> original packet to port 1
 *                                      \-> generated packet -> egress B -> port 2
 *   The user egress root is a sentinel: generated traffic bypasses it through the internal SQN rule,
 *   so its counter should remain zero.
 *
 * @nb_queues [in]: number of queues
 * @nb_ports [in]: number of switch-mode ports
 * @devs_manager [in]: switch-manager device bundles
 * @nb_devs [in]: number of switch-manager device bundles
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_packet_generate(int nb_queues, int nb_ports, struct flow_devs_manager devs_manager[], uint16_t nb_devs)
{
	struct packet_generate_case_runtime cases[PACKET_GENERATE_NB_CASES] = {0};
	struct doca_flow_definitions *definitions = NULL;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct flow_resources resources = {
		.mode = DOCA_FLOW_RESOURCE_MODE_PORT,
		.nr_counters = PACKET_GENERATE_NB_ENTRIES,
		.nr_packet_generate = PACKET_GENERATE_NB_CASES,
	};
	struct doca_flow_match udp_match = {0};
	struct doca_flow_pipe *egress_b_pipe = NULL;
	struct doca_flow_pipe *egress_root_pipe = NULL;
	struct doca_flow_pipe *ordered_list_pipe = NULL;
	struct doca_flow_pipe *default_root_pipe = NULL;
	struct doca_flow_port *switch_port;
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd packet_fwd = {0};
	struct entries_status status = {0};
	struct packet_generate_stats_ctx stats_ctx = {0};
	bool flow_initialized = false;
	bool ports_initialized = false;
	uint32_t case_index;
	uint32_t action_index;
	doca_error_t result;

	if (nb_ports < PACKET_GENERATE_NB_PORTS) {
		DOCA_LOG_ERR("Sample requires at least %d switch-mode ports, got %d",
			     PACKET_GENERATE_NB_PORTS,
			     nb_ports);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_flow_external_action_packet_generate_register();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register packet-generation external action: %s", doca_error_get_descr(result));
		return result;
	}

	result = create_packet_generate_definitions(&definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create packet-generation definitions: %s", doca_error_get_descr(result));
		return result;
	}

	result = init_doca_flow_with_defs(nb_queues, "switch,expert", &resources, nr_shared_resources, definitions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA Flow: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	flow_initialized = true;

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(PACKET_GENERATE_ACTIONS_MEM_ENTRIES));
	result = init_doca_flow_switch_ports(devs_manager, nb_devs, ports, nb_ports, actions_mem_size, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize switch ports: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	ports_initialized = true;

	switch_port = doca_flow_port_switch_get(ports[0]);
	if (switch_port == NULL) {
		DOCA_LOG_ERR("Failed to get switch port");
		result = DOCA_ERROR_BAD_STATE;
		goto cleanup;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 2;
	result = create_basic_pipe(switch_port,
				   "PACKET_GENERATE_EGRESS_B",
				   false,
				   DOCA_FLOW_PIPE_DOMAIN_EGRESS,
				   NULL,
				   &fwd,
				   1,
				   &egress_b_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	packet_fwd.type = DOCA_FLOW_FWD_PIPE;
	packet_fwd.next_pipe = egress_b_pipe;
	result = create_basic_pipe(switch_port,
				   "PACKET_GENERATE_EGRESS_ROOT",
				   true,
				   DOCA_FLOW_PIPE_DOMAIN_EGRESS,
				   NULL,
				   &packet_fwd,
				   1,
				   &egress_root_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		const struct packet_generate_case_cfg *case_cfg = &packet_generate_cases[case_index];
		struct doca_flow_external_resource_packet_generate_cfg packet_cfg = {
			.prefill_size = case_cfg->prefill_size,
			.nb_prefills = case_cfg->nb_prefills,
			.inline_data_size = case_cfg->inline_data_size,
			.icrc_en = false,
			.fwd = &packet_fwd,
			.num_qps = 1,
			.qp_depth = PACKET_GENERATE_QP_DEPTH,
		};

		if (case_cfg->nb_prefills > 1) {
			packet_cfg.prefill_selector.field_string = "outer.udp.dst_port";
			packet_cfg.prefill_selector.bit_offset = 0;
		}

		result = doca_flow_external_resource_packet_generate_create(&packet_cfg,
									    switch_port,
									    &cases[case_index].resource);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create packet-generation case %u resource: %s",
				     case_index,
				     doca_error_get_descr(result));
			goto cleanup;
		}

		if (case_cfg->nb_prefills != 0) {
			result = initialize_prefills(switch_port,
						     cases[case_index].resource,
						     case_cfg->prefill_size,
						     case_cfg->nb_prefills);
			if (result != DOCA_SUCCESS)
				goto cleanup;
		}
	}

	result = create_packet_generate_ordered_list(switch_port, cases, &ordered_list_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	for (case_index = 0; case_index < PACKET_GENERATE_NB_CASES; case_index++) {
		const struct packet_generate_case_cfg *case_cfg = &packet_generate_cases[case_index];

		DOCA_LOG_INFO("Case %u: UDP dst %u, prefills %u, inline %u bytes (%u dynamic), actions %u",
			      case_index,
			      case_cfg->udp_dst_port,
			      case_cfg->nb_prefills,
			      case_cfg->inline_data_size,
			      case_cfg->dynamic_inline_size,
			      cases[case_index].nb_actions);
	}

	udp_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	udp_match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	udp_match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	udp_match.outer.udp.l4_port.dst_port = UINT16_MAX;

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ordered_list_pipe;
	fwd.ordered_list_pipe.idx = UINT32_MAX;
	result = create_basic_pipe(switch_port,
				   "PACKET_GENERATE_DEFAULT_ROOT",
				   true,
				   DOCA_FLOW_PIPE_DOMAIN_DEFAULT,
				   &udp_match,
				   &fwd,
				   PACKET_GENERATE_NB_CASES,
				   &default_root_pipe);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = add_basic_entry(egress_b_pipe,
				 DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH,
				 &status,
				 &stats_ctx.egress_b_entry);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = add_basic_entry(egress_root_pipe,
				 DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH,
				 &status,
				 &stats_ctx.egress_root_entry);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = add_packet_generate_ordered_list_entries(ordered_list_pipe,
							  cases,
							  &status,
							  stats_ctx.ordered_list_entries);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result =
		add_default_root_entries(default_root_pipe, ordered_list_pipe, &status, stats_ctx.default_root_entries);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = doca_flow_pipe_queue_drain(switch_port, 0, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process sample entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	if (status.nb_processed != PACKET_GENERATE_NB_ENTRIES || status.failure) {
		DOCA_LOG_ERR("Entry processing failed: processed=%u expected=%u failure=%d",
			     status.nb_processed,
			     PACKET_GENERATE_NB_ENTRIES,
			     status.failure);
		result = DOCA_ERROR_BAD_STATE;
		goto cleanup;
	}

	DOCA_LOG_INFO("Send IPv4/UDP traffic with destination ports %u, %u, and %u into the switch root.",
		      packet_generate_cases[0].udp_dst_port,
		      packet_generate_cases[1].udp_dst_port,
		      packet_generate_cases[2].udp_dst_port);
	DOCA_LOG_INFO("Original packets target port 1; generated packets enter egress B and target port 2.");
	flow_wait_for_packets(PACKET_GENERATE_WAIT_SEC, print_packet_generate_stats, &stats_ctx);

cleanup:
	if (default_root_pipe != NULL)
		doca_flow_pipe_destroy(default_root_pipe);
	if (ordered_list_pipe != NULL)
		doca_flow_pipe_destroy(ordered_list_pipe);
	for (case_index = PACKET_GENERATE_NB_CASES; case_index > 0; case_index--) {
		struct packet_generate_case_runtime *case_runtime = &cases[case_index - 1];

		for (action_index = case_runtime->nb_actions; action_index > 0; action_index--) {
			if (case_runtime->pipe_actions[action_index - 1] != NULL)
				doca_flow_external_action_packet_generate_pipe_action_destroy(
					case_runtime->pipe_actions[action_index - 1]);
		}
		if (case_runtime->resource != NULL)
			doca_flow_external_resource_packet_generate_destroy(case_runtime->resource);
	}
	if (egress_root_pipe != NULL)
		doca_flow_pipe_destroy(egress_root_pipe);
	if (egress_b_pipe != NULL)
		doca_flow_pipe_destroy(egress_b_pipe);
	if (ports_initialized) {
		doca_error_t cleanup_result = stop_doca_flow_ports(nb_ports, ports);

		if (result == DOCA_SUCCESS)
			result = cleanup_result;
	}
	if (flow_initialized)
		destroy_doca_flow();
	if (definitions != NULL)
		doca_flow_definitions_destroy(definitions);
	return result;
}
