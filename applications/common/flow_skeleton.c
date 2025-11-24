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

#include <stdlib.h>

#include <doca_log.h>
#include <doca_error.h>

#include "flow_skeleton.h"

DOCA_LOG_REGISTER(FLOW_SKELETON);

#define DEFAULT_QUEUE_DEPTH 128	 /* DOCA Flow default queue depth */
#define DEFAULT_TIMEOUT_US 10000 /* Timeout for processing pipe entries */
#define QUOTA_TIME 20		 /* max handling aging time in ms */

struct skeleton_ctx {
	uint32_t queue_depth;		       /* DOCA Flow queue depth */
	struct flow_skeleton_cfg skeleton_cfg; /* pointer to skeleton config */
	uint32_t **queue_state;		       /* array to monitor the queues state */
	uint32_t *queue_counter;	       /* array to count how many entries processed */
	struct flow_skeleton_entry *entries;   /* array of flow skeleton entries */
	void **program_ctx;		       /* application context from main loop */
};

static struct skeleton_ctx skeleton_ctx;
static volatile bool force_quit;

/*
 * Entry processing callback
 *
 * @entry [in]: entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
static void process_callback(struct doca_flow_pipe_entry *entry,
			     uint16_t pipe_queue,
			     enum doca_flow_entry_status status,
			     enum doca_flow_entry_op op,
			     void *user_ctx)
{
	doca_error_t result;

	if (op == DOCA_FLOW_ENTRY_OP_ADD) {
		/* call application callback */
		if (skeleton_ctx.skeleton_cfg.add_cb != NULL)
			skeleton_ctx.skeleton_cfg.add_cb(entry, pipe_queue, status, user_ctx, skeleton_ctx.program_ctx);
		if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry");
			/* if status is not success - the skeleton will remove the entry */
			result = doca_flow_pipe_remove_entry(pipe_queue, DOCA_FLOW_WAIT_FOR_BATCH, entry);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to remove entry: %s", doca_error_get_descr(result));
		} else
			skeleton_ctx.queue_counter[pipe_queue]++;
	} else if (op == DOCA_FLOW_ENTRY_OP_DEL) {
		/* call application callback */
		if (skeleton_ctx.skeleton_cfg.remove_cb != NULL)
			skeleton_ctx.skeleton_cfg.remove_cb(entry,
							    pipe_queue,
							    status,
							    user_ctx,
							    skeleton_ctx.program_ctx[pipe_queue]);
		if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
			/* if status is not success - notify the application about a failure */
			DOCA_LOG_ERR("Failed to remove entry");
			if (skeleton_ctx.skeleton_cfg.failure_cb != NULL)
				skeleton_ctx.skeleton_cfg.failure_cb();
		} else
			skeleton_ctx.queue_counter[pipe_queue]++;
	} else if (op == DOCA_FLOW_ENTRY_OP_AGED) {
		struct flow_skeleton_aging_op aging_op = {0};

		skeleton_ctx.skeleton_cfg.aging_cb(entry, &aging_op);
		if (aging_op.to_remove) {
			result = doca_flow_pipe_remove_entry(pipe_queue, DOCA_FLOW_WAIT_FOR_BATCH, entry);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to remove entry: %s", doca_error_get_descr(result));
		}
	}
}

doca_error_t flow_skeleton_init(struct doca_flow_cfg *flow_cfg, struct flow_skeleton_cfg *skeleton_cfg)
{
	uint32_t i;
	doca_error_t result;

	memcpy(&skeleton_ctx.skeleton_cfg, skeleton_cfg, sizeof(struct flow_skeleton_cfg));
	skeleton_ctx.queue_depth = skeleton_cfg->queue_depth == 0 ? DEFAULT_QUEUE_DEPTH : skeleton_cfg->queue_depth;

	if (skeleton_cfg->entries_acquisition_cb == NULL) {
		DOCA_LOG_ERR("Entries acquisition callback must be provided");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (skeleton_cfg->handle_aging && skeleton_cfg->aging_cb == NULL) {
		DOCA_LOG_ERR("Aging callback must be provided when enable handle aging");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (skeleton_cfg->nb_entries > skeleton_ctx.queue_depth / 2) {
		DOCA_LOG_ERR("Queue depth should be at least twice larger than nb_entries");
		return DOCA_ERROR_INVALID_VALUE;
	}

	skeleton_ctx.entries =
		(struct flow_skeleton_entry *)calloc(skeleton_cfg->nb_entries, sizeof(struct flow_skeleton_entry));
	if (skeleton_ctx.entries == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}
	skeleton_ctx.program_ctx = (void *)calloc(skeleton_cfg->nb_queues, sizeof(void *));
	if (skeleton_ctx.program_ctx == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_entries;
	}
	skeleton_ctx.queue_counter = (uint32_t *)calloc(skeleton_cfg->nb_queues, sizeof(uint32_t));
	if (skeleton_ctx.queue_counter == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_program_ctx;
	}
	skeleton_ctx.queue_state = (uint32_t **)calloc(skeleton_cfg->nb_ports, sizeof(uint32_t *));
	if (skeleton_ctx.program_ctx == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_queue_counter;
	}
	for (i = 0; i < skeleton_cfg->nb_ports; i++) {
		skeleton_ctx.queue_state[i] = (uint32_t *)calloc(skeleton_cfg->nb_queues, sizeof(uint32_t));
		if (skeleton_ctx.program_ctx == NULL) {
			uint32_t j;

			DOCA_LOG_ERR("Failed to allocate memory");
			for (j = 0; j < i; j++)
				free(skeleton_ctx.queue_state[j]);
			result = DOCA_ERROR_NO_MEMORY;
			goto free_queue_state;
		}
	}

	result = doca_flow_cfg_create(&flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		goto free_queue_state_raw;
	}

	result = doca_flow_cfg_set_pipe_queues(flow_cfg, skeleton_cfg->nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(result));
		goto destroy_flow_cfg;
	}

	result = doca_flow_cfg_set_queue_depth(flow_cfg, skeleton_cfg->queue_depth);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg queue_depth: %s", doca_error_get_descr(result));
		goto destroy_flow_cfg;
	}

	result = doca_flow_cfg_set_cb_entry_process(flow_cfg, process_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg cb_entry_process: %s", doca_error_get_descr(result));
		goto destroy_flow_cfg;
	}

	result = doca_flow_init(flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_flow_cfg;
	}

	result = doca_flow_cfg_destroy(flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow_cfg: %s", doca_error_get_descr(result));
		goto free_queue_state_raw;
	}

	force_quit = false;
	return DOCA_SUCCESS;

destroy_flow_cfg:
	doca_flow_cfg_destroy(flow_cfg);
free_queue_state_raw:
	for (i = 0; i < skeleton_cfg->nb_ports; i++)
		free(skeleton_ctx.queue_state[i]);
free_queue_state:
	free(skeleton_ctx.queue_state);
free_queue_counter:
	free(skeleton_ctx.queue_counter);
free_program_ctx:
	free(skeleton_ctx.program_ctx);
free_entries:
	free(skeleton_ctx.entries);
	return result;
}

void flow_skeleton_destroy(void)
{
	uint32_t i;

	if (skeleton_ctx.entries != NULL)
		free(skeleton_ctx.entries);

	if (skeleton_ctx.program_ctx != NULL)
		free(skeleton_ctx.program_ctx);

	if (skeleton_ctx.queue_counter != NULL)
		free(skeleton_ctx.queue_counter);

	if (skeleton_ctx.queue_state != NULL) {
		for (i = 0; i < skeleton_ctx.skeleton_cfg.nb_ports; i++) {
			if (skeleton_ctx.queue_state[i] != NULL)
				free(skeleton_ctx.queue_state[i]);
		}
		free(skeleton_ctx.queue_state);
	}
	doca_flow_destroy();
}

/*
 * Add entry based on the given flow_skeleton_entry struct
 *
 * @pipe_queue [in]: queue identifier
 * @flag [in]: DOCA_FLOW_WAIT_FOR_BATCH / DOCA_FLOW_NO_WAIT
 * @entry [in]: application input information for adding the entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_entry(uint16_t pipe_queue, uint32_t flag, struct flow_skeleton_entry *entry)
{
	doca_error_t result = DOCA_SUCCESS;

	switch (entry->ctx.type) {
	case DOCA_FLOW_PIPE_BASIC:
		result = doca_flow_pipe_add_entry(pipe_queue,
						  entry->ctx.pipe,
						  entry->ctx.match,
						  entry->ctx.actions_idx,
						  entry->ctx.actions,
						  entry->ctx.monitor,
						  entry->ctx.fwd,
						  flag,
						  entry->ctx.usr_ctx,
						  entry->ctx.entry);
		break;
	case DOCA_FLOW_PIPE_CONTROL:
		result = doca_flow_pipe_control_add_entry(pipe_queue,
							  entry->ctx.priority,
							  entry->ctx.pipe,
							  entry->ctx.match,
							  entry->ctx.match_mask,
							  NULL,
							  entry->ctx.actions,
							  entry->ctx.actions_mask,
							  entry->ctx.action_descs,
							  entry->ctx.monitor,
							  entry->ctx.fwd,
							  entry->ctx.usr_ctx,
							  entry->ctx.entry);
		break;
	case DOCA_FLOW_PIPE_LPM:
		result = doca_flow_pipe_lpm_add_entry(pipe_queue,
						      entry->ctx.pipe,
						      entry->ctx.match,
						      entry->ctx.match_mask,
						      entry->ctx.actions_idx,
						      entry->ctx.actions,
						      entry->ctx.monitor,
						      entry->ctx.fwd,
						      flag,
						      entry->ctx.usr_ctx,
						      entry->ctx.entry);
		break;
	case DOCA_FLOW_PIPE_ACL:
		result = doca_flow_pipe_acl_add_entry(pipe_queue,
						      entry->ctx.pipe,
						      entry->ctx.match,
						      entry->ctx.match_mask,
						      entry->ctx.actions,
						      entry->ctx.priority,
						      entry->ctx.fwd,
						      flag,
						      entry->ctx.usr_ctx,
						      entry->ctx.entry);
		break;
	case DOCA_FLOW_PIPE_ORDERED_LIST:
		result = doca_flow_pipe_ordered_list_add_entry(pipe_queue,
							       entry->ctx.pipe,
							       entry->ctx.idx,
							       entry->ctx.ordered_list,
							       entry->ctx.fwd,
							       flag,
							       entry->ctx.usr_ctx,
							       entry->ctx.entry);
		break;
	case DOCA_FLOW_PIPE_HASH:
		result = doca_flow_pipe_hash_add_entry(pipe_queue,
						       entry->ctx.pipe,
						       entry->ctx.idx,
						       entry->ctx.actions_idx,
						       entry->ctx.actions,
						       entry->ctx.monitor,
						       entry->ctx.fwd,
						       flag,
						       entry->ctx.usr_ctx,
						       entry->ctx.entry);
		break;
	default:
		DOCA_LOG_ERR("Unsupported pipe type");
		result = DOCA_ERROR_INVALID_VALUE;
	}
	return result;
}

/*
 * Add a batch of entries and process
 *
 * @params [in]: main loop params
 * @nb_entries [in]: number of entries in the batch
 * @port_id [in]: port ID of the entries
 */
static void add_batch_entries(struct main_loop_params *params, uint32_t nb_entries, int port_id)
{
	uint32_t flags;
	uint32_t i;
	doca_error_t result;

	/* Add all the entries with DOCA_FLOW_WAIT_FOR_BATCH flag */
	flags = DOCA_FLOW_WAIT_FOR_BATCH;
	for (i = 0; i < nb_entries - 1; i++) {
		if (skeleton_ctx.entries[i].ctx.op == DOCA_FLOW_ENTRY_OP_ADD)
			result = add_entry(params->pipe_queue, flags, &skeleton_ctx.entries[i]);
		else if (skeleton_ctx.entries[i].ctx.op == DOCA_FLOW_ENTRY_OP_DEL)
			result = doca_flow_pipe_remove_entry(params->pipe_queue,
							     flags,
							     *skeleton_ctx.entries[i].ctx.entry);
		else {
			DOCA_LOG_ERR("DOCA Flow op [%d] is not supported", skeleton_ctx.entries[i].ctx.op);
			result = DOCA_ERROR_INVALID_VALUE;
		}
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to add/remove entry in index [%d]: %s", i, doca_error_get_descr(result));
		else
			skeleton_ctx.queue_state[port_id][params->pipe_queue]++;
	}

	/* Add last entry with DOCA_FLOW_NO_WAIT */
	flags = DOCA_FLOW_NO_WAIT;
	if (skeleton_ctx.entries[nb_entries - 1].ctx.op == DOCA_FLOW_ENTRY_OP_ADD)
		result = add_entry(params->pipe_queue, flags, &skeleton_ctx.entries[nb_entries - 1]);
	else if (skeleton_ctx.entries[nb_entries - 1].ctx.op == DOCA_FLOW_ENTRY_OP_DEL)
		result = doca_flow_pipe_remove_entry(params->pipe_queue,
						     flags,
						     *skeleton_ctx.entries[nb_entries - 1].ctx.entry);
	else {
		DOCA_LOG_ERR("DOCA Flow op [%d] is not supported", skeleton_ctx.entries[nb_entries - 1].ctx.op);
		result = DOCA_ERROR_INVALID_VALUE;
	}
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add/remove entry in index [%d]: %s",
			     nb_entries - 1,
			     doca_error_get_descr(result));
	else
		skeleton_ctx.queue_state[port_id][params->pipe_queue]++;

	result = doca_flow_entries_process(params->ports[port_id], params->pipe_queue, 0, nb_entries);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("DOCA Flow entries process failed %s", doca_error_get_descr(result));

	skeleton_ctx.queue_state[port_id][params->pipe_queue] -= skeleton_ctx.queue_counter[params->pipe_queue];
}

void flow_skeleton_main_loop(void *main_loop_params)
{
	struct main_loop_params *params = (struct main_loop_params *)main_loop_params;
	uint32_t nb_entries;
	int port_id;
	doca_error_t result;

	skeleton_ctx.program_ctx[params->pipe_queue] = params->program_ctx;

	if (params->initialization) {
		for (port_id = 0; port_id < params->nb_ports; port_id++) {
			skeleton_ctx.queue_counter[params->pipe_queue] = 0;
			/* Get array of entries to add or remove */
			if (skeleton_ctx.skeleton_cfg.init_cb == NULL)
				break;
			skeleton_ctx.skeleton_cfg.init_cb(skeleton_ctx.entries,
							  port_id,
							  params->program_ctx,
							  &nb_entries);
			if (nb_entries == 0)
				continue;
			add_batch_entries(params, nb_entries, port_id);
		}
	}

	while (!force_quit) {
		for (port_id = 0; port_id < params->nb_ports; port_id++) {
			skeleton_ctx.queue_counter[params->pipe_queue] = 0;
			/* Get array of entries to add or remove */
			skeleton_ctx.skeleton_cfg.entries_acquisition_cb(skeleton_ctx.entries,
									 port_id,
									 params->program_ctx,
									 &nb_entries);
			if (nb_entries == 0)
				continue;
			if (skeleton_ctx.queue_state[port_id][params->pipe_queue] + nb_entries >=
			    skeleton_ctx.queue_depth) {
				DOCA_LOG_WARN("Queue %d on port %d is full", params->pipe_queue, port_id);
				result = doca_flow_entries_process(params->ports[port_id],
								   params->pipe_queue,
								   0,
								   nb_entries);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("DOCA Flow entries process failed %s",
						     doca_error_get_descr(result));
			}
			add_batch_entries(params, nb_entries, port_id);
			if (skeleton_ctx.skeleton_cfg.handle_aging)
				doca_flow_aging_handle(params->ports[port_id], params->pipe_queue, QUOTA_TIME, 0);
		}
	}

	/* empty the queue before exit */
	for (port_id = 0; port_id < params->nb_ports; port_id++) {
		while (skeleton_ctx.queue_state[port_id][params->pipe_queue] > 0) {
			skeleton_ctx.queue_counter[params->pipe_queue] = 0;
			result = doca_flow_entries_process(params->ports[port_id],
							   params->pipe_queue,
							   DEFAULT_TIMEOUT_US,
							   skeleton_ctx.queue_state[port_id][params->pipe_queue]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("DOCA Flow entries process failed %s", doca_error_get_descr(result));
				break;
			}
			skeleton_ctx.queue_state[port_id][params->pipe_queue] -=
				skeleton_ctx.queue_counter[params->pipe_queue];
		}
	}
}

void flow_skeleton_notify_exit(void)
{
	force_quit = true;
}
