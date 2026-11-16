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

/*
 * Mutex external action sample
 * ============================
 *
 * Demonstrates the hardware-enforced critical section provided by the mutex
 * external action by running a read-modify-write sequence against a single
 * shared ASO-memory slot.
 *
 * Topology
 * --------
 *   inbound traffic -> basic pipe -> ordered-list pipe -> RSS (2 queues) -> host
 *
 * Per-flow setup
 * --------------
 *   Two source IPs, A and B, map to two basic-pipe entries:
 *     - SIP A : meta scratchpad pair <- 1
 *     - SIP B : meta scratchpad pair <- 10
 *   Each entry then forwards into the ordered-list pipe.
 *
 *   The ASO-memory action works at 64-bit granularity and its u32_index selects
 *   the register pair meta.u32[i]/meta.u32[i + 1]; since DOCA Flow metadata has
 *   no defined initial value, both halves are written so the arithmetic below is
 *   well defined.
 *
 * Ordered-list pipe (with-mutex, 6 elements)
 *   element 0 : MUTEX LOCK        (resource_offset bound per entry)
 *   element 1 : ASO_MEMORY STORE  (input_output = scratchpad register, resource_offset bound per entry)
 *   element 2 : ASO_MEMORY ADD    (same input_output, resource_offset bound per entry)
 *   element 3 : ASO_MEMORY LOAD   (same input_output, resource_offset bound per entry)
 *   element 4 : MUTEX UNLOCK      (resource_offset bound per entry)
 *   element 5 : ACTION_COPY       (scratchpad register -> meta.pkt_meta)
 *
 * Ordered-list pipe (without-mutex, 4 elements)
 *   element 0 : ASO_MEMORY STORE
 *   element 1 : ASO_MEMORY ADD
 *   element 2 : ASO_MEMORY LOAD
 *   element 3 : ACTION_COPY
 *
 * Pipe-action factories register cfg.resource_offset = UINT32_MAX (changeable
 * per entry); the single ordered-list entry installed at idx 0 resolves it to
 * resource_offset = 0 on every external-action element.
 *
 * The selection between the two pipes is driven by the CLI flag --no-mutex
 * (mutex enabled by default). At pipe exit, FWD -> RSS hashing on SIP spreads
 * the two flows over the RX queues; which queue a flow lands on is decided by
 * the RSS hash and is not fixed per flow.
 *
 * Host side
 * ---------
 *   The post-RMW scratchpad register is copied into meta.pkt_meta so it
 *   surfaces on the mbuf via RTE_FLOW_DYNF_METADATA(mbuf). Two RX threads,
 *   one per RSS queue, classify each packet by the value it carries rather
 *   than by the queue it arrived on:
 *     - 2  (= 1 + 1)   from flow A
 *     - 20 (= 10 + 10) from flow B
 *   Anything else is an anomaly (e.g. 11 from a 1+10 interleave):
 *     - with mutex    : anomalies must be 0, and any anomaly fails the run.
 *     - without mutex : anomalies are expected on contention and do not fail.
 *   Both modes fail when no packet arrives or when a packet carries no
 *   usable metadata, since neither case can be told apart from a broken
 *   pipeline.
 *
 * Current state: fully implemented.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_dpdk.h>
#include <doca_flow.h>

#include <doca_flow_mutex.h>
#include <doca_flow_external_action_memory.h>

#include <flow_common.h>

#include "flow_external_action_mutex_sample.h"

DOCA_LOG_REGISTER(FLOW_EXTERNAL_ACTION_MUTEX);

#define NB_PORTS 2
#define NUM_MUTEXES 1
#define NUM_MEMORY_SLOTS 1
#define MAX_NUM_ENTRIES 4

/* meta.data[WORKING_REG] is the 32-bit scratchpad the ASO chain reads/writes. */
#define WORKING_REG 0
#define SCRATCHPAD_BITS 32
#define META_U32_BIT_OFFSET(idx) (offsetof(struct doca_flow_meta, u32[(idx)]) << 3)
#define META_PKT_META_BIT_OFFSET (offsetof(struct doca_flow_meta, pkt_meta) << 3)

/* No-mutex ordered list: STORE -> ADD -> LOAD -> COPY(meta.data[WORKING_REG] -> meta.pkt_meta) */
#define NB_MEM_ACTIONS 3
#define NB_OL_NO_MUTEX_ELEMENTS (NB_MEM_ACTIONS + 1)

/* With-mutex ordered list: LOCK -> STORE -> ADD -> LOAD -> UNLOCK -> COPY */
#define NB_MUTEX_FENCE_ACTIONS 2 /* LOCK + UNLOCK */
#define NB_OL_MUTEX_ELEMENTS (NB_MUTEX_FENCE_ACTIONS + NB_MEM_ACTIONS + 1)

/* Two flows: SIP_A -> scratchpad = 1 (expected post-RMW = 2),
 *            SIP_B -> scratchpad = 10 (expected post-RMW = 20).
 */
#define SIP_A_ADDR BE_IPV4_ADDR(1, 2, 3, 4)
#define SIP_B_ADDR BE_IPV4_ADDR(5, 6, 7, 8)
#define SIP_A_REG_VALUE 1
#define SIP_B_REG_VALUE 10

#define NB_BASIC_ENTRIES 2
#define NB_RSS_QUEUES 2

/* Expected post-RMW values surfacing in meta.pkt_meta on each side of the RSS split. */
#define EXPECTED_VALUE_A (SIP_A_REG_VALUE + SIP_A_REG_VALUE) /* = 2 */
#define EXPECTED_VALUE_B (SIP_B_REG_VALUE + SIP_B_REG_VALUE) /* = 20 */

#define INGRESS_PORT_ID 0
#define RX_BURST_SIZE 64

static doca_error_t register_external_actions(void)
{
	doca_error_t result;

	result = doca_flow_external_action_mutex_register();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register mutex external action: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_external_action_memory_register();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register memory external action: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t get_doca_devs(struct doca_dev *dev_arr[])
{
	doca_error_t result;
	int i;

	for (i = 0; i < NB_PORTS; i++) {
		result = doca_dpdk_port_as_dev(i, &dev_arr[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get device for port %d: %s", i, doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

static doca_error_t create_mutex_resource(struct doca_flow_port *port, struct doca_flow_external_resource_mutex **mutex)
{
	struct doca_flow_external_resource_mutex_cfg cfg = {
		.num_of_mutexes = NUM_MUTEXES,
	};
	doca_error_t result;

	result = doca_flow_external_resource_mutex_create(&cfg, port, mutex);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mutex resource: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_INFO("Mutex resource created: %p (num_of_mutexes=%u)", (void *)*mutex, NUM_MUTEXES);
	return DOCA_SUCCESS;
}

static doca_error_t create_memory_resource(struct doca_flow_port *port,
					   struct doca_flow_external_resource_memory **memory)
{
	struct doca_flow_external_resource_memory_cfg cfg = {
		.num_resources = NUM_MEMORY_SLOTS,
		.granularity = DOCA_FLOW_EXTERNAL_RESOURCE_GRANULARITY_64BIT,
	};
	doca_error_t result;

	result = doca_flow_external_resource_memory_create(port, &cfg, memory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create memory resource: %s", doca_error_get_descr(result));
		return result;
	}

	/* Initialize the single slot to 0 - matches the STORE/ADD/LOAD chain expectations. */
	result = doca_flow_external_resource_memory_update(*memory, 0, 0, UINT64_MAX);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize memory slot: %s", doca_error_get_descr(result));
		(void)doca_flow_external_resource_memory_destroy(*memory);
		*memory = NULL;
		return result;
	}
	DOCA_LOG_INFO("Memory resource created: %p (num_resources=%u, granularity=64-bit)",
		      (void *)*memory,
		      NUM_MEMORY_SLOTS);
	return DOCA_SUCCESS;
}

static doca_error_t create_memory_pipe_action(struct doca_flow_external_resource_memory *memory,
					      enum doca_flow_external_action_memory_operation op,
					      void **pipe_action)
{
	struct doca_flow_external_action_memory_pipe_action_cfg cfg = {
		.resource = memory,
		.resource_offset = UINT32_MAX, /* changeable per entry */
		.operation = op,
		.u32_index = WORKING_REG,
		.use_access_index = false,
	};

	return doca_flow_external_action_memory_pipe_action_create(&cfg, pipe_action);
}

/*
 * Build the without-mutex ordered-list pipe used as the contention baseline.
 *
 * Element order:
 *   0: ASO_MEMORY STORE  (write scratchpad register into the shared slot)
 *   1: ASO_MEMORY ADD    (slot += scratchpad)
 *   2: ASO_MEMORY LOAD   (slot value -> scratchpad register)
 *   3: ACTION_COPY       (scratchpad register -> meta.pkt_meta, so the host sees
 *                          the post-RMW value via RTE_FLOW_DYNF_METADATA)
 *
 * FWD is supplied by the caller; this sample passes the 2-queue RSS exit.
 */
static doca_error_t create_no_mutex_ordered_list_pipe(struct doca_flow_port *port,
						      struct doca_flow_external_resource_memory *memory,
						      const struct doca_flow_fwd *fwd,
						      struct doca_flow_pipe **pipe)
{
	static const enum doca_flow_external_action_memory_operation ops[NB_MEM_ACTIONS] = {
		DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_STORE,
		DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_ADD,
		DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_LOAD,
	};
	void *mem_actions[NB_MEM_ACTIONS] = {NULL};
	struct doca_flow_ordered_list_element elements[NB_OL_NO_MUTEX_ELEMENTS] = {0};
	struct doca_flow_ordered_list ordered_list = {0};
	struct doca_flow_ordered_list *ordered_lists[1];
	struct doca_flow_action_desc copy_desc = {
		.type = DOCA_FLOW_ACTION_COPY,
		.field_op.src = {.field_string = "meta.data", .bit_offset = META_U32_BIT_OFFSET(WORKING_REG)},
		.field_op.dst = {.field_string = "meta.data", .bit_offset = META_PKT_META_BIT_OFFSET},
		.field_op.width = SCRATCHPAD_BITS,
	};
	struct doca_flow_action_descs copy_descs = {
		.desc_array = &copy_desc,
		.nb_action_desc = 1,
	};
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	int i;
	doca_error_t result;

	for (i = 0; i < NB_MEM_ACTIONS; i++) {
		result = create_memory_pipe_action(memory, ops[i], &mem_actions[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create memory pipe action %d: %s", i, doca_error_get_descr(result));
			goto cleanup;
		}
		elements[i].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		elements[i].external_actions = mem_actions[i];
	}
	elements[NB_MEM_ACTIONS].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements[NB_MEM_ACTIONS].action_descs = &copy_descs;

	ordered_list.idx = 0;
	ordered_list.size = NB_OL_NO_MUTEX_ELEMENTS;
	ordered_list.elements = elements;
	ordered_lists[0] = &ordered_list;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MUTEX_NO_MUTEX_OL_PIPE", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered lists: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create no-mutex ordered-list pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	DOCA_LOG_INFO("No-mutex ordered-list pipe created: %p (STORE/ADD/LOAD/COPY)", (void *)*pipe);

cleanup:
	/* Pipe-create captures the handles; safe to release on success or failure. */
	for (i = 0; i < NB_MEM_ACTIONS; i++)
		if (mem_actions[i])
			doca_flow_external_action_memory_pipe_action_destroy(mem_actions[i]);
	if (pipe_cfg)
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t create_mutex_pipe_action(struct doca_flow_external_resource_mutex *mutex,
					     enum doca_flow_external_action_mutex_operation op,
					     void **pipe_action)
{
	struct doca_flow_external_action_mutex_pipe_action_cfg cfg = {
		.resource = mutex,
		.resource_offset = UINT32_MAX, /* changeable per entry */
		.op = op,
		.nested_lock = false,
		.use_access_index = false,
	};

	return doca_flow_external_action_mutex_pipe_action_create(&cfg, pipe_action);
}

/*
 * Build the with-mutex ordered-list pipe used for the race-free RMW demo.
 *
 * Element order:
 *   0: MUTEX LOCK
 *   1: ASO_MEMORY STORE  (write scratchpad register into the shared slot)
 *   2: ASO_MEMORY ADD    (slot += scratchpad)
 *   3: ASO_MEMORY LOAD   (slot value -> scratchpad register)
 *   4: MUTEX UNLOCK
 *   5: ACTION_COPY       (scratchpad register -> meta.pkt_meta)
 */
static doca_error_t create_mutex_ordered_list_pipe(struct doca_flow_port *port,
						   struct doca_flow_external_resource_mutex *mutex,
						   struct doca_flow_external_resource_memory *memory,
						   const struct doca_flow_fwd *fwd,
						   struct doca_flow_pipe **pipe)
{
	static const enum doca_flow_external_action_memory_operation mem_ops[NB_MEM_ACTIONS] = {
		DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_STORE,
		DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_ADD,
		DOCA_FLOW_EXTERNAL_ACTION_MEMORY_OP_LOAD,
	};
	void *lock_action = NULL;
	void *unlock_action = NULL;
	void *mem_actions[NB_MEM_ACTIONS] = {NULL};
	struct doca_flow_ordered_list_element elements[NB_OL_MUTEX_ELEMENTS] = {0};
	struct doca_flow_ordered_list ordered_list = {0};
	struct doca_flow_ordered_list *ordered_lists[1];
	struct doca_flow_action_desc copy_desc = {
		.type = DOCA_FLOW_ACTION_COPY,
		.field_op.src = {.field_string = "meta.data", .bit_offset = META_U32_BIT_OFFSET(WORKING_REG)},
		.field_op.dst = {.field_string = "meta.data", .bit_offset = META_PKT_META_BIT_OFFSET},
		.field_op.width = SCRATCHPAD_BITS,
	};
	struct doca_flow_action_descs copy_descs = {
		.desc_array = &copy_desc,
		.nb_action_desc = 1,
	};
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	int i;
	doca_error_t result;

	result = create_mutex_pipe_action(mutex, DOCA_FLOW_EXTERNAL_ACTION_MUTEX_OP_LOCK, &lock_action);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create LOCK pipe action: %s", doca_error_get_descr(result));
		goto cleanup_actions;
	}
	elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
	elements[0].external_actions = lock_action;

	for (i = 0; i < NB_MEM_ACTIONS; i++) {
		result = create_memory_pipe_action(memory, mem_ops[i], &mem_actions[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create memory pipe action %d: %s", i, doca_error_get_descr(result));
			goto cleanup_actions;
		}
		elements[1 + i].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		elements[1 + i].external_actions = mem_actions[i];
	}

	result = create_mutex_pipe_action(mutex, DOCA_FLOW_EXTERNAL_ACTION_MUTEX_OP_UNLOCK, &unlock_action);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create UNLOCK pipe action: %s", doca_error_get_descr(result));
		goto cleanup_actions;
	}
	elements[1 + NB_MEM_ACTIONS].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
	elements[1 + NB_MEM_ACTIONS].external_actions = unlock_action;

	elements[NB_OL_MUTEX_ELEMENTS - 1].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;
	elements[NB_OL_MUTEX_ELEMENTS - 1].action_descs = &copy_descs;

	ordered_list.idx = 0;
	ordered_list.size = NB_OL_MUTEX_ELEMENTS;
	ordered_list.elements = elements;
	ordered_lists[0] = &ordered_list;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup_actions;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MUTEX_OL_PIPE", DOCA_FLOW_PIPE_ORDERED_LIST, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr entries: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered lists: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mutex ordered-list pipe: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}
	DOCA_LOG_INFO("Mutex ordered-list pipe created: %p (LOCK/STORE/ADD/LOAD/UNLOCK/COPY)", (void *)*pipe);

cleanup_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
cleanup_actions:
	if (lock_action)
		doca_flow_external_action_mutex_pipe_action_destroy(lock_action);
	for (i = 0; i < NB_MEM_ACTIONS; i++)
		if (mem_actions[i])
			doca_flow_external_action_memory_pipe_action_destroy(mem_actions[i]);
	if (unlock_action)
		doca_flow_external_action_mutex_pipe_action_destroy(unlock_action);
	return result;
}

/*
 * Install the single ordered-list entry that binds resource_offset = 0 to every
 * external action in the OL template at idx 0.
 *
 * The pipe-action factories register cfg.resource_offset = UINT32_MAX (changeable
 * per entry); this entry resolves it to the only slot allocated on each
 * resource (NUM_MUTEXES = NUM_MEMORY_SLOTS = 1, offset 0).
 *
 * Element layout matches the pipe-create template:
 *   with-mutex : LOCK / STORE / ADD / LOAD / UNLOCK / COPY
 *   no-mutex   :        STORE / ADD / LOAD /          COPY
 *
 * The trailing COPY element carries no per-entry override (action is fixed at
 * pipe-create time), so its actions/action_descs stay NULL.
 */
static doca_error_t add_ordered_list_pipe_entry(struct doca_flow_port *port,
						struct doca_flow_pipe *pipe,
						bool use_mutex,
						struct entries_status *status)
{
	struct doca_flow_external_action_mutex_entry mutex_entry = {.resource_offset = 0};
	struct doca_flow_external_action_memory_entry mem_entry = {.resource_offset = 0};
	struct doca_flow_ordered_list_element elements[NB_OL_MUTEX_ELEMENTS] = {0};
	struct doca_flow_ordered_list ordered_list = {
		.idx = 0,
		.size = use_mutex ? NB_OL_MUTEX_ELEMENTS : NB_OL_NO_MUTEX_ELEMENTS,
		.elements = elements,
	};
	uint32_t copy_idx;
	int i;
	doca_error_t result;

	memset(status, 0, sizeof(*status));

	if (use_mutex) {
		elements[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		elements[0].external_actions = &mutex_entry;
		for (i = 0; i < NB_MEM_ACTIONS; i++) {
			elements[1 + i].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
			elements[1 + i].external_actions = &mem_entry;
		}
		elements[1 + NB_MEM_ACTIONS].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		elements[1 + NB_MEM_ACTIONS].external_actions = &mutex_entry;
		copy_idx = NB_OL_MUTEX_ELEMENTS - 1;
	} else {
		for (i = 0; i < NB_MEM_ACTIONS; i++) {
			elements[i].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
			elements[i].external_actions = &mem_entry;
		}
		copy_idx = NB_OL_NO_MUTEX_ELEMENTS - 1;
	}
	elements[copy_idx].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_ACTIONS;

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       pipe,
						       0,
						       &ordered_list,
						       NULL,
						       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						       status,
						       NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ordered-list entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process ordered-list entry: %s", doca_error_get_descr(result));
		return result;
	}
	if (status->nb_processed != 1 || status->failure) {
		DOCA_LOG_ERR("Ordered-list entry processing incomplete (processed=%d, failure=%d)",
			     status->nb_processed,
			     status->failure);
		return DOCA_ERROR_BAD_STATE;
	}
	DOCA_LOG_INFO("Ordered-list entry installed at idx 0 (resource_offset=0 on every external action).");
	return DOCA_SUCCESS;
}

/*
 * Build the RSS forward used at the ordered-list pipe exit.
 *
 * Spreads inbound traffic over the RX queues by hashing on outer IPv4. Which
 * queue a flow lands on is opaque and may collide: the host RX threads
 * identify a flow by the post-RMW value carried in meta.pkt_meta, never by
 * the queue it arrived on.
 */
static void build_rss_fwd(struct doca_flow_fwd *fwd, uint16_t rss_queues[NB_RSS_QUEUES])
{
	uint32_t i;

	for (i = 0; i < NB_RSS_QUEUES; i++)
		rss_queues[i] = (uint16_t)i;

	memset(fwd, 0, sizeof(*fwd));
	fwd->type = DOCA_FLOW_FWD_RSS;
	fwd->rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd->rss.outer_flags = DOCA_FLOW_RSS_IPV4;
	fwd->rss.queues_array = rss_queues;
	fwd->rss.nr_queues = NB_RSS_QUEUES;
}

/*
 * Create the basic pipe that initializes the scratchpad register per flow
 * and forwards into the ordered-list pipe.
 *
 * Match: outer.ip4.src_ip is changeable per entry.
 * Action: meta.u32[WORKING_REG] is changeable per entry; the entries pin
 *         it to SIP_A_REG_VALUE (=1) for SIP_A and SIP_B_REG_VALUE (=10)
 *         for SIP_B.
 *
 * The ASO-memory operand is the 64-bit u32[WORKING_REG]/u32[WORKING_REG + 1]
 * pair, so both halves are in the action set.
 */
static doca_error_t create_basic_pipe(struct doca_flow_port *port,
				      struct doca_flow_pipe *ordered_list_pipe,
				      struct doca_flow_pipe **basic_pipe)
{
	struct doca_flow_match match = {0};
	struct doca_flow_actions actions = {0};
	struct doca_flow_actions *actions_arr[1] = {&actions};
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE,
		.ordered_list_pipe = {.pipe = ordered_list_pipe, .idx = 0},
	};
	doca_error_t result;

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = UINT32_MAX;

	actions.meta.u32[WORKING_REG] = UINT32_MAX;
	actions.meta.u32[WORKING_REG + 1] = UINT32_MAX;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "MUTEX_BASIC_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set basic pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set basic pipe match: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set basic pipe actions: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, basic_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	DOCA_LOG_INFO("Basic pipe created: %p (match SIP, set meta.u32[%d], fwd ordered-list)",
		      (void *)*basic_pipe,
		      WORKING_REG);

cleanup:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t add_basic_pipe_entry(struct doca_flow_pipe *pipe,
					 doca_be32_t src_ip,
					 uint32_t reg_value,
					 uint32_t flags,
					 struct entries_status *status)
{
	struct doca_flow_match match = {0};
	struct doca_flow_actions actions = {0};
	struct doca_flow_pipe_entry *entry;

	match.outer.ip4.src_ip = src_ip;
	actions.meta.u32[WORKING_REG] = DOCA_HTOBE32(reg_value);
	actions.meta.u32[WORKING_REG + 1] = 0;

	return doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, flags, status, &entry);
}

/*
 * Pin the two SIPs to their scratchpad values.
 *
 * Entry 0: SIP_A -> meta.u32[WORKING_REG] = SIP_A_REG_VALUE
 * Entry 1: SIP_B -> meta.u32[WORKING_REG] = SIP_B_REG_VALUE
 */
static doca_error_t add_basic_pipe_entries(struct doca_flow_port *port,
					   struct doca_flow_pipe *pipe,
					   struct entries_status *status)
{
	doca_error_t result;

	memset(status, 0, sizeof(*status));

	result = add_basic_pipe_entry(pipe, SIP_A_ADDR, SIP_A_REG_VALUE, DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add basic pipe entry (SIP A): %s", doca_error_get_descr(result));
		return result;
	}

	result = add_basic_pipe_entry(pipe, SIP_B_ADDR, SIP_B_REG_VALUE, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add basic pipe entry (SIP B): %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, NB_BASIC_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process basic pipe entries: %s", doca_error_get_descr(result));
		return result;
	}
	if (status->nb_processed != NB_BASIC_ENTRIES || status->failure) {
		DOCA_LOG_ERR("Basic pipe entry processing incomplete (processed=%d, failure=%d)",
			     status->nb_processed,
			     status->failure);
		return DOCA_ERROR_BAD_STATE;
	}
	return DOCA_SUCCESS;
}

struct rx_worker_stats {
	uint64_t packets;
	uint64_t value_a;     /* meta.pkt_meta == EXPECTED_VALUE_A */
	uint64_t value_b;     /* meta.pkt_meta == EXPECTED_VALUE_B */
	uint64_t anomalies;   /* any other value (e.g., 11 from a 1+10 interleave) */
	uint64_t no_metadata; /* DPDK mbuf metadata not available for the packet */
};

struct rx_worker_ctx {
	uint16_t port_id;
	uint16_t queue_id;
	uint32_t duration_sec;
	struct rx_worker_stats stats;
};

/*
 * Per-queue RX worker: polls one RSS queue for the configured duration and
 * classifies each received packet by its post-RMW meta.pkt_meta value.
 */
static void *rx_worker(void *arg)
{
	struct rx_worker_ctx *ctx = (struct rx_worker_ctx *)arg;
	struct rte_mbuf *bufs[RX_BURST_SIZE];
	const time_t end_ts = time(NULL) + (time_t)ctx->duration_sec;
	bool meta_warned = false;
	uint16_t nb, i;

	while (time(NULL) < end_ts) {
		nb = rte_eth_rx_burst(ctx->port_id, ctx->queue_id, bufs, RX_BURST_SIZE);
		if (nb == 0)
			continue;

		ctx->stats.packets += nb;
		for (i = 0; i < nb; i++) {
			if (!rte_flow_dynf_metadata_avail() || !(bufs[i]->ol_flags & rte_flow_dynf_metadata_mask)) {
				ctx->stats.no_metadata++;
				if (!meta_warned) {
					if (!rte_flow_dynf_metadata_avail())
						DOCA_LOG_WARN(
							"Queue %u: rte_flow dynamic metadata field is not registered",
							ctx->queue_id);
					else
						DOCA_LOG_WARN("Queue %u: packet carries no dynamic metadata",
							      ctx->queue_id);
					meta_warned = true;
				}
			} else {
				uint32_t v = *RTE_FLOW_DYNF_METADATA(bufs[i]);

				if (v == EXPECTED_VALUE_A)
					ctx->stats.value_a++;
				else if (v == EXPECTED_VALUE_B)
					ctx->stats.value_b++;
				else
					ctx->stats.anomalies++;
			}
			rte_pktmbuf_free(bufs[i]);
		}
	}
	return NULL;
}

/*
 * Spawn one RX worker per RSS queue, wait for them to finish, and turn the
 * combined counts into a verdict. Anomalies fail the run in mutex mode only;
 * no traffic or unusable metadata fails either mode.
 */
static doca_error_t run_rx_workers(uint32_t duration_sec, bool use_mutex)
{
	struct rx_worker_ctx ctxs[NB_RSS_QUEUES] = {0};
	pthread_t tids[NB_RSS_QUEUES] = {0};
	uint64_t total_no_metadata = 0;
	uint64_t total_anomalies = 0;
	uint64_t total_packets = 0;
	uint32_t i;
	int err;

	if (!rte_flow_dynf_metadata_avail())
		DOCA_LOG_WARN("rte_flow dynamic metadata field is not registered - "
			      "expecting all packets to be counted as no_metadata.");

	DOCA_LOG_INFO("RX workers: starting %d threads for %u sec (use_mutex=%s).",
		      NB_RSS_QUEUES,
		      duration_sec,
		      use_mutex ? "true" : "false");

	for (i = 0; i < NB_RSS_QUEUES; i++) {
		ctxs[i].port_id = INGRESS_PORT_ID;
		ctxs[i].queue_id = (uint16_t)i;
		ctxs[i].duration_sec = duration_sec;

		err = pthread_create(&tids[i], NULL, rx_worker, &ctxs[i]);
		if (err != 0) {
			DOCA_LOG_ERR("Failed to create RX worker for queue %u: %s", i, strerror(err));
			while (i-- > 0)
				(void)pthread_join(tids[i], NULL);
			return DOCA_ERROR_DRIVER;
		}
	}

	for (i = 0; i < NB_RSS_QUEUES; i++)
		(void)pthread_join(tids[i], NULL);

	for (i = 0; i < NB_RSS_QUEUES; i++) {
		DOCA_LOG_INFO("Queue %u: packets=%" PRIu64 " value=%u:%" PRIu64 " value=%u:%" PRIu64
			      " anomalies=%" PRIu64 " no_metadata=%" PRIu64,
			      i,
			      ctxs[i].stats.packets,
			      EXPECTED_VALUE_A,
			      ctxs[i].stats.value_a,
			      EXPECTED_VALUE_B,
			      ctxs[i].stats.value_b,
			      ctxs[i].stats.anomalies,
			      ctxs[i].stats.no_metadata);
		total_packets += ctxs[i].stats.packets;
		total_anomalies += ctxs[i].stats.anomalies;
		total_no_metadata += ctxs[i].stats.no_metadata;
	}

	DOCA_LOG_INFO("Summary (use_mutex=%s): total_packets=%" PRIu64 " total_anomalies=%" PRIu64
		      " total_no_metadata=%" PRIu64 "  (expected: 0 with mutex, >0 without under contention)",
		      use_mutex ? "true" : "false",
		      total_packets,
		      total_anomalies,
		      total_no_metadata);

	if (total_packets == 0) {
		DOCA_LOG_ERR("No packets received - the %s path could not be validated.",
			     use_mutex ? "with-mutex" : "no-mutex");
		return DOCA_ERROR_BAD_STATE;
	}

	if (total_no_metadata != 0) {
		DOCA_LOG_ERR("%" PRIu64 " packet(s) carried no usable metadata - the post-RMW value is unobservable.",
			     total_no_metadata);
		return DOCA_ERROR_BAD_STATE;
	}

	if (use_mutex && total_anomalies != 0) {
		DOCA_LOG_ERR("Mutex mode observed %" PRIu64
			     " anomalies - the critical section did not serialize the RMW sequence.",
			     total_anomalies);
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

doca_error_t flow_external_action_mutex(int nb_queues, const struct flow_external_action_mutex_cfg *cfg)
{
	struct flow_resources resources = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[NB_PORTS] = {NULL};
	struct doca_dev *dev_arr[NB_PORTS] = {NULL};
	uint32_t actions_mem_size[NB_PORTS];
	struct doca_flow_external_resource_mutex *mutex_resource = NULL;
	struct doca_flow_external_resource_memory *memory_resource = NULL;
	struct doca_flow_pipe *active_ol_pipe = NULL;
	struct doca_flow_pipe *basic_pipe = NULL;
	struct doca_flow_fwd ol_fwd;
	uint16_t rss_queues[NB_RSS_QUEUES];
	struct entries_status status = {0};
	doca_error_t result;

	if (nb_queues < NB_RSS_QUEUES) {
		DOCA_LOG_ERR("Sample needs at least %d RX queues, got %d", NB_RSS_QUEUES, nb_queues);
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Mutex sample: use_mutex=%s, duration_sec=%u",
		      cfg->use_mutex ? "true" : "false",
		      cfg->duration_sec);

	result = register_external_actions();
	if (result != DOCA_SUCCESS)
		return result;

	result = init_doca_flow(nb_queues, "vnf", &resources, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	result = get_doca_devs(dev_arr);
	if (result != DOCA_SUCCESS)
		goto destroy_flow;

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(MAX_NUM_ENTRIES));

	result = init_doca_flow_ports(NB_PORTS, ports, false, dev_arr, actions_mem_size, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow ports: %s", doca_error_get_descr(result));
		goto destroy_flow;
	}

	result = create_mutex_resource(ports[0], &mutex_resource);
	if (result != DOCA_SUCCESS)
		goto stop_ports;

	result = create_memory_resource(ports[0], &memory_resource);
	if (result != DOCA_SUCCESS)
		goto stop_ports;

	build_rss_fwd(&ol_fwd, rss_queues);

	if (cfg->use_mutex) {
		result = create_mutex_ordered_list_pipe(ports[0],
							mutex_resource,
							memory_resource,
							&ol_fwd,
							&active_ol_pipe);
		if (result != DOCA_SUCCESS)
			goto stop_ports;
		DOCA_LOG_INFO("Pipeline built (with-mutex): basic pipe -> LOCK/STORE/ADD/LOAD/UNLOCK/COPY -> RSS.");
	} else {
		result = create_no_mutex_ordered_list_pipe(ports[0], memory_resource, &ol_fwd, &active_ol_pipe);
		if (result != DOCA_SUCCESS)
			goto stop_ports;
		DOCA_LOG_INFO("Pipeline built (no-mutex): basic pipe -> STORE/ADD/LOAD/COPY -> RSS.");
	}

	result = add_ordered_list_pipe_entry(ports[0], active_ol_pipe, cfg->use_mutex, &status);
	if (result != DOCA_SUCCESS)
		goto stop_ports;

	result = create_basic_pipe(ports[0], active_ol_pipe, &basic_pipe);
	if (result != DOCA_SUCCESS)
		goto stop_ports;

	result = add_basic_pipe_entries(ports[0], basic_pipe, &status);
	if (result != DOCA_SUCCESS)
		goto stop_ports;

	DOCA_LOG_INFO("Basic pipe entries installed; waiting for packets (%u sec).", cfg->duration_sec);

	result = run_rx_workers(cfg->duration_sec, cfg->use_mutex);

stop_ports:
	/* Pipes must be destroyed before the memory resource they reference; mutex is auto-released at port stop. */
	if (basic_pipe != NULL)
		doca_flow_pipe_destroy(basic_pipe);
	if (active_ol_pipe != NULL)
		doca_flow_pipe_destroy(active_ol_pipe);
	if (memory_resource != NULL) {
		doca_error_t destroy_result = doca_flow_external_resource_memory_destroy(memory_resource);

		if (destroy_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy memory external resource: %s",
				     doca_error_get_descr(destroy_result));
			if (result == DOCA_SUCCESS)
				result = destroy_result;
		}
		memory_resource = NULL;
	}
	(void)stop_doca_flow_ports(NB_PORTS, ports);
destroy_flow:
	destroy_doca_flow();
	return result;
}
