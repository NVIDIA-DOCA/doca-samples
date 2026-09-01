/*
 * Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef COMMON_FLOW_PIPES_MANAGER_H_
#define COMMON_FLOW_PIPES_MANAGER_H_

#include <doca_flow.h>

struct pipe_info {
	struct doca_flow_pipe *pipe;		 /* DOCA Flow pipe pointer */
	struct rte_hash *entries_table;		 /* Pipe entries table */
	struct rte_hash *port_id_to_pipes_table; /* Pipes table for the specific port on which the pipe was created */
};

struct flow_pipes_manager {
	struct rte_hash *pipe_id_to_pipe_info_table; /* map pipe id to all relevant entries to support
							doca_flow_destroy_port() */
	struct rte_hash *entry_id_to_pipe_id_table;  /* map entry id to pipe id to support
							doca_flow_pipe_remove_entry() */
	struct rte_hash *port_id_to_pipes_id_table;  /* map port id to all relevant pipes to support
							doca_flow_port_pipes_flush() */
};

/*
 * Create flow pipe manager structures
 *
 * @pipes_manager [out]: Pointer to the newly created pipes manager
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_create(struct flow_pipes_manager **pipes_manager);

/*
 * Destroy flow pipe manager
 *
 * @manager [in]: Pipes manager to destroy
 */
void pipes_manager_destroy(struct flow_pipes_manager *manager);

/*
 * Save the given DOCA Flow pipe and generate an ID for it
 *
 * @manager [in]: Pipes manager pointer
 * @pipe [in]: DOCA Flow pipe pointer
 * @port_id [in]: ID of the relevant port to add pipe to
 * @pipe_id [out]: Generated pipe ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_pipe_create(struct flow_pipes_manager *manager,
				       struct doca_flow_pipe *pipe,
				       uint16_t port_id,
				       uint64_t *pipe_id);

/*
 * Save the given DOCA Flow entry and generate an ID for it
 *
 * @manager [in]: Pipes manager pointer
 * @entry [in]: DOCA Flow entry pointer
 * @pipe_id [out]: ID of pipe to add entry to
 * @entry_id [out]: Generated entry ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_pipe_add_entry(struct flow_pipes_manager *manager,
					  struct doca_flow_pipe_entry *entry,
					  uint64_t pipe_id,
					  uint64_t *entry_id);

/*
 * Get the DOCA Flow pipe pointer of the given pipe ID
 *
 * @manager [in]: Pipes manager pointer
 * @pipe_id [in]: ID of the needed pipe to get
 * @pipe [out]: DOCA Flow pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_get_pipe(struct flow_pipes_manager *manager, uint64_t pipe_id, struct doca_flow_pipe **pipe);

/*
 * Get the DOCA Flow entry pointer of the given entry ID
 *
 * @manager [in]: Pipes manager pointer
 * @entry_id [in]: ID of the needed entry
 * @entry [out]: DOCA Flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_get_entry(struct flow_pipes_manager *manager,
				     uint64_t entry_id,
				     struct doca_flow_pipe_entry **entry);

/*
 * Destroy a pipe and all of its entries
 *
 * @manager [in]: Pipes manager pointer
 * @pipe_id [in]: ID of the needed pipe to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_pipe_destroy(struct flow_pipes_manager *manager, uint64_t pipe_id);

/*
 * Remove an entry
 *
 * @manager [in]: Pipes manager pointer
 * @entry_id [in]: ID of the needed entry to remove
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_pipe_rm_entry(struct flow_pipes_manager *manager, uint64_t entry_id);

/*
 * Remove all pipes and their entries for a specific port ID
 *
 * @manager [in]: Pipes manager pointer
 * @port_id [in]: ID of the needed port
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t pipes_manager_pipes_flush(struct flow_pipes_manager *manager, uint16_t port_id);

#endif /* COMMON_FLOW_PIPES_MANAGER_H_ */
