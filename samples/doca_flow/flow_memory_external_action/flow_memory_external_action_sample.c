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

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>

#include "doca_bitfield.h"

#include <doca_flow_external_action_memory.h>

DOCA_LOG_REGISTER(FLOW_MEMORY_EXTERNAL_ACTION);

#define META_U32_BIT_OFFSET(idx) (offsetof(struct doca_flow_meta, u32[(idx)]) << 3)

#define NB_MEMORY_RESOURCES 16 /* Number of 64-bit resources (slots) in the memory. */
#define MEMORY_U32_INDEX 0     /* Even meta.u32[] register the action reads/writes (64-bit: u32[0],[1]). */

/* The memory ordered list has two elements per template - the external action and a per-entry counter -
 * in a single template with one entry per slot. */
#define MEMORY_ORDERED_LIST_IDX 0  /* external-action element index */
#define MEMORY_MONITOR_IDX 1	   /* counter (monitor) element index */
#define MEMORY_ORDERED_LIST_SIZE 2 /* elements in the ordered list */
#define MEMORY_NB_ORDERED_LISTS 1  /* number of ordered-list templates */

/* The test framework releases each YAML stage on a "packets to arrive" print; the use cases have up to two
 * stages, so wait twice (a one-stage use case idles through the second window). */
#define NB_TRAFFIC_STAGES 2
#define TRAFFIC_STAGE_WAIT_SEC 5

/* Slot selection masks the low log2(NB_MEMORY_RESOURCES) bits of the port and uses __builtin_ctz as
 * log2, so the count must be a non-zero power of two. */
_Static_assert((NB_MEMORY_RESOURCES & (NB_MEMORY_RESOURCES - 1)) == 0 && NB_MEMORY_RESOURCES > 0,
	       "NB_MEMORY_RESOURCES must be a non-zero power of two");

/*
 * Seed every memory resource with a distinct, recognizable single 64-bit value: slot i gets
 * 0x1122334455667700 + i, so each slot's seed encodes its own index in the low byte.
 * OP_ADD later grows the addressed slot by the value carried on the packet; the read API and the
 * value written back onto the wire let the user observe the 64-bit before/after (including carry
 * from the low into the high half).
 *
 * @memory [in]: memory resource to seed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t seed_memory(struct doca_flow_external_resource_memory *memory)
{
	doca_error_t result;
	uint32_t i;

	for (i = 0; i < NB_MEMORY_RESOURCES; i++) {
		result = doca_flow_external_resource_memory_update(memory, i, 0x1122334455667700ULL + i, UINT64_MAX);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to seed memory slot %u: %s", i, doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

/*
 * Read the whole memory back through the read API into values[].
 *
 * @memory [in]: memory resource to read
 * @values [out]: array of NB_MEMORY_RESOURCES 64-bit values
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t read_memory(struct doca_flow_external_resource_memory *memory, uint64_t *values)
{
	doca_error_t result = doca_flow_external_resource_memory_read(memory, 0, NB_MEMORY_RESOURCES, values);

	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to read memory: %s", doca_error_get_descr(result));
	return result;
}

/*
 * Print the whole memory as one 64-bit value per slot.
 *
 * @label [in]: short label for the printout
 * @values [in]: array of NB_MEMORY_RESOURCES 64-bit values
 */
static void print_memory(const char *label, const uint64_t *values)
{
	uint32_t i;

	DOCA_LOG_INFO("Memory contents (%s):", label);
	for (i = 0; i < NB_MEMORY_RESOURCES; i++)
		DOCA_LOG_INFO("  slot %2u = 0x%016" PRIx64, i, values[i]);
}

/*
 * Create the egress pipe: writes the value the memory operation left in the register pair (its return
 * value - see doca_flow_external_action_memory_operation for what each operation returns) back onto
 * the packet - u32[MEMORY_U32_INDEX] into the source IP and the next register into the destination IP
 * - then forwards to the peer port. So a forwarded packet carries the operation's result on the wire.
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_egress_pipe(struct doca_flow_port *port, int port_id, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_descs descs;
	struct doca_flow_action_descs *descs_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_desc desc_array[2] = {0};
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&descs, 0, sizeof(descs));
	memset(&fwd, 0, sizeof(fwd));

	/* Expose both halves of the slot's original 64-bit value on the wire. */
	desc_array[0].type = DOCA_FLOW_ACTION_COPY;
	desc_array[0].field_op.src.field_string = "meta.data";
	desc_array[0].field_op.src.bit_offset = META_U32_BIT_OFFSET(MEMORY_U32_INDEX);
	desc_array[0].field_op.dst.field_string = "outer.ipv4.src_ip";
	desc_array[0].field_op.dst.bit_offset = 0;
	desc_array[0].field_op.width = 32;

	desc_array[1].type = DOCA_FLOW_ACTION_COPY;
	desc_array[1].field_op.src.field_string = "meta.data";
	desc_array[1].field_op.src.bit_offset = META_U32_BIT_OFFSET(MEMORY_U32_INDEX + 1);
	desc_array[1].field_op.dst.field_string = "outer.ipv4.dst_ip";
	desc_array[1].field_op.dst.bit_offset = 0;
	desc_array[1].field_op.width = 32;

	actions_arr[0] = &actions;
	descs_arr[0] = &descs;
	descs.nb_action_desc = 2;
	descs.desc_array = desc_array;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "EGRESS_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, descs_arr, NB_ACTIONS_ARR);
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
 * Add the single (non-changeable) entry of the egress pipe.
 *
 * @pipe [in]: pipe of the entry
 * @status [in/out]: user context, updated as the entry is processed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_egress_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry *entry;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
}

/*
 * Create the ordered-list pipe whose single external-action element performs the selected memory
 * operation on the addressed slot, then forwards to the egress pipe.
 *
 * @port [in]: port of the pipe
 * @memory [in]: memory resource to operate on
 * @use_access_index [in]: take the offset from the access_index register instead of a static offset
 * @operation [in]: memory operation to perform
 * @next_pipe [in]: pipe to forward matched packets to
 * @ordered_list [in/out]: ordered-list description used both for pipe creation and entry insertion
 * @elements [in/out]: backing element array for ordered_list
 * @monitor [in]: per-entry counter configuration for the ordered-list monitor element
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_memory_pipe(struct doca_flow_port *port,
				       struct doca_flow_external_resource_memory *memory,
				       bool use_access_index,
				       enum doca_flow_external_action_memory_operation operation,
				       struct doca_flow_pipe *next_pipe,
				       struct doca_flow_ordered_list *ordered_list,
				       struct doca_flow_ordered_list_element *elements,
				       struct doca_flow_monitor *monitor,
				       struct doca_flow_pipe **pipe)
{
	struct doca_flow_external_action_memory_pipe_action_cfg memory_cfg = {
		.resource = memory,
		/* UINT32_MAX makes the offset changeable per flow entry (each entry provides its own slot); in
		 * --use-access-index mode the offset comes from the access_index register (value ignored). */
		.resource_offset = UINT32_MAX,
		.operation = operation,
		.u32_index = MEMORY_U32_INDEX,
		.use_access_index = use_access_index,
	};
	int nb_entries = use_access_index ? 1 : NB_MEMORY_RESOURCES;
	struct doca_flow_ordered_list *ordered_lists[MEMORY_NB_ORDERED_LISTS];
	void *memory_pipe_action = NULL;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_fwd fwd;
	doca_error_t result;
	doca_error_t destroy_result;

	memset(&fwd, 0, sizeof(fwd));

	result = doca_flow_external_action_memory_pipe_action_create(&memory_cfg, &memory_pipe_action);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create memory pipe action: %s", doca_error_get_descr(result));
		return result;
	}

	elements[MEMORY_ORDERED_LIST_IDX].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
	elements[MEMORY_ORDERED_LIST_IDX].external_actions = memory_pipe_action;
	elements[MEMORY_MONITOR_IDX].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_MONITOR;
	elements[MEMORY_MONITOR_IDX].monitor = monitor;
	ordered_list->idx = 0;
	ordered_list->size = MEMORY_ORDERED_LIST_SIZE;
	ordered_list->elements = elements;
	ordered_lists[0] = ordered_list;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_action;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MEMORY_PIPE", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nb_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, MEMORY_NB_ORDERED_LISTS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg ordered lists: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (!use_access_index) {
		/* Select the target entry per packet from the classifier's fwd.ordered_list_pipe.idx. */
		result = doca_flow_pipe_cfg_set_ordered_list_fwd_mode(pipe_cfg, DOCA_FLOW_ORDERED_LIST_FWD_MODE_INDEX);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set ordered list fwd mode: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
destroy_action:
	/* doca_flow_pipe_create() has consumed the action handle into the pipe; it is no longer
	 * referenced afterwards, so it can be destroyed here. */
	destroy_result = doca_flow_external_action_memory_pipe_action_destroy(memory_pipe_action);
	if (destroy_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy memory pipe action: %s", doca_error_get_descr(destroy_result));
	return result;
}

/*
 * Add the memory pipe entries. In the default mode one entry per slot is added - entry i drives OP_ADD
 * on resource offset i, and the classifier's port match routes each packet to its entry. In
 * --use-access-index mode a single entry serves every slot, taking the offset from the access_index
 * register. Each entry points the ordered-list element at a dedicated memory entry struct carrying the
 * resource offset.
 *
 * @pipe [in]: pipe of the entries
 * @use_access_index [in]: take the offset from the access_index register instead of a per-entry slot
 * @ordered_list [in]: ordered-list description reused from pipe creation
 * @elements [in/out]: backing element array, repointed to each per-entry struct
 * @memory_entries [in]: per-entry offset carriers (must outlive entry processing)
 * @entries [out]: created entries, one per slot, used later to query each entry's counter
 * @status [in/out]: user context, updated as entries are processed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_memory_pipe_entries(struct doca_flow_pipe *pipe,
					    bool use_access_index,
					    struct doca_flow_ordered_list *ordered_list,
					    struct doca_flow_ordered_list_element *elements,
					    struct doca_flow_external_action_memory_entry *memory_entries,
					    struct doca_flow_pipe_entry **entries,
					    struct entries_status *status)
{
	int nb_entries = use_access_index ? 1 : NB_MEMORY_RESOURCES;
	doca_error_t result;
	int i;

	for (i = 0; i < nb_entries; i++) {
		memory_entries[i].resource_offset = use_access_index ? UINT32_MAX : (uint32_t)i;
		elements[MEMORY_ORDERED_LIST_IDX].external_actions = &memory_entries[i];

		result = doca_flow_pipe_ordered_list_add_entry(0,
							       pipe,
							       i,
							       ordered_list,
							       NULL,
							       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
							       status,
							       &entries[i]);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create the root classifier pipe. It matches IPv4/UDP and copies the packet's source IP into
 * u32[MEMORY_U32_INDEX] and destination IP into u32[MEMORY_U32_INDEX + 1] - the 64-bit operand
 * consumed by STORE/ADD/XOR/OR/AND (ignored by operations that don't read it, e.g. LOAD or plain
 * FFS_*). Both modes select the slot per packet from the low
 * log2(NB_MEMORY_RESOURCES) bits of the L4 source port: the default mode matches the port (one entry per
 * slot) and forwards to the matching memory pipe entry; --use-access-index mode copies the port into the
 * access_index register instead.
 *
 * @port [in]: port of the pipe
 * @use_access_index [in]: select the slot via the access_index register instead of a per-port entry
 * @next_pipe [in]: memory ordered-list pipe to forward into
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_classifier_pipe(struct doca_flow_port *port,
					   bool use_access_index,
					   struct doca_flow_pipe *next_pipe,
					   struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_match *match_mask_ptr = NULL;
	struct doca_flow_actions actions;
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_descs descs;
	struct doca_flow_action_descs *descs_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_desc desc_array[3] = {0};
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	int nb_desc = 0;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&actions, 0, sizeof(actions));
	memset(&descs, 0, sizeof(descs));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* Both modes read the UDP source port (as a per-port match in the default mode, or copied into the
	 * access_index register otherwise), so constrain the match to IPv4/UDP in both. */
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match_mask.parser_meta.outer_l3_type = UINT32_MAX;
	match_mask.parser_meta.outer_l4_type = UINT32_MAX;
	match_mask.outer.l4_type_ext = UINT32_MAX;
	match_mask_ptr = &match_mask;

	if (!use_access_index) {
		/* Default mode: also match the low log2(NB_MEMORY_RESOURCES) bits of the source port so each
		 * entry pins one port value in [0, NB_MEMORY_RESOURCES) and the memory pipe (INDEX fwd mode)
		 * routes it to the matching entry. */
		match.outer.udp.l4_port.src_port = UINT16_MAX;
		match_mask.outer.udp.l4_port.src_port = DOCA_HTOBE16(NB_MEMORY_RESOURCES - 1);
	} else {
		/* Select the memory slot per packet from the L4 source port. Copy only the low
		 * log2(NB_MEMORY_RESOURCES) bits into access_index, so any port maps to a valid slot in
		 * [0, NB_MEMORY_RESOURCES) (NB_MEMORY_RESOURCES is a power of two). The sample injects UDP. */
		desc_array[nb_desc].type = DOCA_FLOW_ACTION_COPY;
		desc_array[nb_desc].field_op.src.field_string = "outer.udp.src_port";
		desc_array[nb_desc].field_op.src.bit_offset = 0;
		desc_array[nb_desc].field_op.dst.field_string = "meta.access_index";
		desc_array[nb_desc].field_op.dst.bit_offset = 0;
		desc_array[nb_desc].field_op.width = __builtin_ctz(NB_MEMORY_RESOURCES);
		nb_desc++;
	}

	/* Copy the packet's source/destination IPs into the register pair as the 64-bit operand
	 * (src_ip -> high half u32[MEMORY_U32_INDEX], dst_ip -> low half u32[MEMORY_U32_INDEX + 1]).
	 * A field-string copy preserves network byte order, matching how the register is read. */
	desc_array[nb_desc].type = DOCA_FLOW_ACTION_COPY;
	desc_array[nb_desc].field_op.src.field_string = "outer.ipv4.src_ip";
	desc_array[nb_desc].field_op.src.bit_offset = 0;
	desc_array[nb_desc].field_op.dst.field_string = "meta.data";
	desc_array[nb_desc].field_op.dst.bit_offset = META_U32_BIT_OFFSET(MEMORY_U32_INDEX);
	desc_array[nb_desc].field_op.width = 32;
	nb_desc++;

	desc_array[nb_desc].type = DOCA_FLOW_ACTION_COPY;
	desc_array[nb_desc].field_op.src.field_string = "outer.ipv4.dst_ip";
	desc_array[nb_desc].field_op.src.bit_offset = 0;
	desc_array[nb_desc].field_op.dst.field_string = "meta.data";
	desc_array[nb_desc].field_op.dst.bit_offset = META_U32_BIT_OFFSET(MEMORY_U32_INDEX + 1);
	desc_array[nb_desc].field_op.width = 32;
	nb_desc++;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "CLASSIFIER_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, match_mask_ptr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	actions_arr[0] = &actions;
	descs_arr[0] = &descs;
	descs.nb_action_desc = nb_desc;
	descs.desc_array = desc_array;
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, descs_arr, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = next_pipe;
	/* idx here is the ordered-list-pipe entry index. Default mode: each classifier entry selects the
	 * target memory entry by its port (UINT32_MAX = changeable per entry). Access-index mode has a
	 * single memory entry, so route straight to entry 0. */
	fwd.ordered_list_pipe.idx = use_access_index ? 0 : UINT32_MAX;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add the classifier root pipe entries. The IP copies (and, in --use-access-index mode, the port copy)
 * are configured on the pipe, so entries only carry match values and forwarding. The default mode adds
 * one entry per slot - entry i matches L4 source port low bits == i and forwards to memory entry i;
 * --use-access-index mode adds a single entry that forwards to the pipe's only memory entry.
 *
 * @pipe [in]: pipe of the entries
 * @use_access_index [in]: single entry routed via the access_index register instead of per-port entries
 * @next_pipe [in]: memory ordered-list pipe forwarded into (per-entry index in the default mode)
 * @status [in/out]: user context, updated as entries are processed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_classifier_pipe_entries(struct doca_flow_pipe *pipe,
						bool use_access_index,
						struct doca_flow_pipe *next_pipe,
						struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;
	int i;

	memset(&actions, 0, sizeof(actions));

	if (use_access_index) {
		memset(&match, 0, sizeof(match));
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &entry);
	}

	memset(&fwd, 0, sizeof(fwd));
	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = next_pipe;

	for (i = 0; i < NB_MEMORY_RESOURCES; i++) {
		memset(&match, 0, sizeof(match));
		match.outer.udp.l4_port.src_port = DOCA_HTOBE16(i);
		fwd.ordered_list_pipe.idx = i;

		result = doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, &actions, NULL, &fwd, 0, status, &entry);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Log the expected effect of the selected memory operation on the addressed slot, as a formula.
 * "user_input" is the 64-bit value the classifier copies from the packet's source/destination IP
 * (src_ip -> high half, dst_ip -> low half) - its concrete value depends on whichever packet is
 * injected, so it is named as a variable here rather than computed. Operations that don't read it
 * (LOAD and the FFS_ and FIND_FIRST_SET_AND_RESET_ family) ignore it entirely.
 *
 * @operation [in]: memory operation selected via --operation
 */
static void print_expected_result(enum doca_flow_external_action_memory_operation operation)
{
	switch (operation) {
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_LOAD:
		DOCA_LOG_INFO("Expected: memory[slot] unchanged; register = memory[slot]");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_STORE:
		DOCA_LOG_INFO("Expected: memory[slot] = user_input; register = memory[slot] (value before the store)");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_ADD:
		DOCA_LOG_INFO(
			"Expected: memory[slot] = memory[slot] + user_input; register = memory[slot] (value before the add)");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_BITWISE_XOR:
		DOCA_LOG_INFO(
			"Expected: memory[slot] = memory[slot] ^ user_input; register = memory[slot] (value before the xor)");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_BITWISE_OR:
		DOCA_LOG_INFO(
			"Expected: memory[slot] = memory[slot] | user_input; register = memory[slot] (value before the or)");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_BITWISE_AND:
		DOCA_LOG_INFO(
			"Expected: memory[slot] = memory[slot] & user_input; register = memory[slot] (value before the and)");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_ID_MSB:
		DOCA_LOG_INFO(
			"Expected: memory[slot] unchanged; register bits[31:26] = bit index (from LSB) of memory[slot]'s highest set bit, register's second u32 bit0 = 1 iff memory[slot] == 0");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_SET_BITMAP_MSB:
		DOCA_LOG_INFO("Expected: memory[slot] unchanged; register = memory[slot]'s highest set bit only");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_ID_LSB:
		DOCA_LOG_INFO(
			"Expected: memory[slot] unchanged; register bits[31:26] = bit index (from LSB) of memory[slot]'s lowest set bit, register's second u32 bit0 = 1 iff memory[slot] == 0");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FFS_SET_BITMAP_LSB:
		DOCA_LOG_INFO("Expected: memory[slot] unchanged; register = memory[slot]'s lowest set bit only");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_ID_MSB:
		DOCA_LOG_INFO(
			"Expected: memory[slot] has its highest set bit cleared; register bits[31:26] = that bit's index (from LSB), register's second u32 bit0 = 1 iff memory[slot] == 0");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_BITMAP_MSB:
		DOCA_LOG_INFO(
			"Expected: memory[slot] has its highest set bit cleared; register = that cleared bit only");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_ID_LSB:
		DOCA_LOG_INFO(
			"Expected: memory[slot] has its lowest set bit cleared; register bits[31:26] = that bit's index (from LSB), register's second u32 bit0 = 1 iff memory[slot] == 0");
		break;
	case DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_FIND_FIRST_SET_AND_RESET_BITMAP_LSB:
		DOCA_LOG_INFO(
			"Expected: memory[slot] has its lowest set bit cleared; register = that cleared bit only");
		break;
	}
}

/*
 * Run flow_memory_external_action sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @use_access_index [in]: take the memory offset per packet from the L4 (UDP) source port
 * @operation [in]: memory operation to perform
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_memory_external_action(int nb_queues,
					 bool use_access_index,
					 enum doca_flow_external_action_memory_operation operation)
{
	const int nb_ports = 2;
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_external_resource_memory_cfg memory_cfg = {
		.num_resources = NB_MEMORY_RESOURCES,
		.granularity = DOCA_FLOW_EXTERNAL_RESOURCE_GRANULARITY_64BIT,
	};
	struct doca_flow_external_resource_memory *memory = NULL;
	struct doca_flow_ordered_list ordered_list;
	struct doca_flow_ordered_list_element elements[MEMORY_ORDERED_LIST_SIZE];
	struct doca_flow_external_action_memory_entry memory_entries[NB_MEMORY_RESOURCES];
	/* Default mode: one classifier + one memory entry per slot, plus the egress entry. Access-index mode:
	 * one classifier + one memory entry + the egress entry. */
	int nb_entries = use_access_index ? 3 : (2 * NB_MEMORY_RESOURCES + 1);
	struct doca_flow_pipe *egress_pipe = NULL;
	struct doca_flow_pipe *memory_pipe = NULL;
	struct doca_flow_pipe *classifier_pipe = NULL;
	struct doca_flow_pipe_entry *memory_pipe_entries[NB_MEMORY_RESOURCES];
	struct doca_flow_monitor memory_monitor;
	struct doca_flow_resource_query query;
	int nb_memory_entries = use_access_index ? 1 : NB_MEMORY_RESOURCES;
	struct entries_status status;
	uint64_t before[NB_MEMORY_RESOURCES] = {0};
	uint64_t after[NB_MEMORY_RESOURCES] = {0};
	doca_error_t result;
	doca_error_t destroy_result;

	memset(&ordered_list, 0, sizeof(ordered_list));
	memset(elements, 0, sizeof(elements));
	memset(memory_entries, 0, sizeof(memory_entries));
	memset(&query, 0, sizeof(query));
	memset(&memory_monitor, 0, sizeof(memory_monitor));
	memset(memory_pipe_entries, 0, sizeof(memory_pipe_entries));
	memset(&status, 0, sizeof(status));

	memory_monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	resource.nr_counters = nb_memory_entries;

	/* The external action type must be registered before DOCA Flow is initialized. */
	result = doca_flow_external_action_memory_register();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register memory external action: %s", doca_error_get_descr(result));
		return result;
	}

	result = init_doca_flow(nb_queues, "vnf", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(nb_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		destroy_doca_flow();
		return result;
	}

	/* Traffic arrives on port 0, has the selected memory operation applied to the port-selected slot,
	 * and is hairpinned out port 1 carrying the operation's return value. */
	result = doca_flow_external_resource_memory_create(ports[0], &memory_cfg, &memory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create memory resource: %s", doca_error_get_descr(result));
		goto cleanup_ports;
	}

	result = seed_memory(memory);
	if (result != DOCA_SUCCESS)
		goto cleanup_pipes;

	/* Read the seeded memory back so its "before" values can be compared with the "after" values later. */
	result = read_memory(memory, before);
	if (result != DOCA_SUCCESS)
		goto cleanup_pipes;
	print_memory("seeded", before);

	result = create_egress_pipe(ports[0], 0, &egress_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create egress pipe: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}
	result = add_egress_pipe_entry(egress_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress pipe entry: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}

	result = create_memory_pipe(ports[0],
				    memory,
				    use_access_index,
				    operation,
				    egress_pipe,
				    &ordered_list,
				    elements,
				    &memory_monitor,
				    &memory_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create memory pipe: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}
	result = add_memory_pipe_entries(memory_pipe,
					 use_access_index,
					 &ordered_list,
					 elements,
					 memory_entries,
					 memory_pipe_entries,
					 &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add memory pipe entries: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}

	result = create_classifier_pipe(ports[0], use_access_index, memory_pipe, &classifier_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create classifier pipe: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}
	result = add_classifier_pipe_entries(classifier_pipe, use_access_index, memory_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add classifier pipe entries: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}

	result = doca_flow_pipe_queue_drain(ports[0], 0, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
		goto cleanup_pipes;
	}
	if (status.nb_processed != nb_entries || status.failure) {
		DOCA_LOG_ERR("Failed to process entries: processed %d of %d", status.nb_processed, nb_entries);
		result = DOCA_ERROR_BAD_STATE;
		goto cleanup_pipes;
	}

	print_expected_result(operation);
	for (int stage = 1; stage <= NB_TRAFFIC_STAGES; stage++) {
		DOCA_LOG_INFO("Wait %d seconds for stage %d packets to arrive", TRAFFIC_STAGE_WAIT_SEC, stage);
		sleep(TRAFFIC_STAGE_WAIT_SEC);
	}

	/* The forwarded packets carried each slot's operation return value; memory holds the result of
	 * applying the operation. Read it back and print before/after. */
	result = read_memory(memory, after);
	if (result != DOCA_SUCCESS)
		goto cleanup_pipes;
	print_memory("after traffic", after);
	for (uint32_t i = 0; i < NB_MEMORY_RESOURCES; i++) {
		if (after[i] != before[i])
			DOCA_LOG_INFO("Slot %u changed: 0x%016" PRIx64 " -> 0x%016" PRIx64, i, before[i], after[i]);
	}

	/* Per-entry counter from the memory ordered-list monitor element: how many packets hit each entry
	 * (in --use-access-index mode a single entry serves every slot). Print only the entries that were hit. */
	for (int i = 0; i < nb_memory_entries; i++) {
		result = doca_flow_resource_query_entry(memory_pipe_entries[i], &query);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query memory entry %d counter: %s", i, doca_error_get_descr(result));
			goto cleanup_pipes;
		}
		if (query.counter.total_pkts == 0)
			continue;
		if (use_access_index)
			DOCA_LOG_INFO("Memory entry counter (per-packet slot from access_index): %" PRIu64
				      " packets, %" PRIu64 " bytes",
				      query.counter.total_pkts,
				      query.counter.total_bytes);
		else
			DOCA_LOG_INFO("Memory entry %d counter: %" PRIu64 " packets, %" PRIu64 " bytes",
				      i,
				      query.counter.total_pkts,
				      query.counter.total_bytes);
	}

cleanup_pipes:
	/* Destroy the pipes (reverse creation order) before the memory resource they reference, so no
	 * installed pipe still points at a freed external resource. */
	if (classifier_pipe)
		doca_flow_pipe_destroy(classifier_pipe);
	if (memory_pipe)
		doca_flow_pipe_destroy(memory_pipe);
	if (egress_pipe)
		doca_flow_pipe_destroy(egress_pipe);
	destroy_result = doca_flow_external_resource_memory_destroy(memory);
	if (destroy_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy memory external resource: %s", doca_error_get_descr(destroy_result));
cleanup_ports:
	stop_doca_flow_ports(nb_ports, ports);
	destroy_doca_flow();
	return result;
}
