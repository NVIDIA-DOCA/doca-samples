/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef WORKER_SANDBOX_H_
#define WORKER_SANDBOX_H_

#include "ucp/api/ucp.h"

#include <doca_urom.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tag send task callback function, will be called once the send task is finished
 *
 * @result [in]: task status
 * @cookie [in]: user cookie
 * @context [in]: task user context
 * @status [in]: UCX status
 */
typedef void (*urom_sandbox_send_finished)(doca_error_t result,
					   union doca_data cookie,
					   union doca_data context,
					   ucs_status_t status);

/*
 * Tag recv task callback function, will be called once the recv task is finished
 *
 * @result [in]: task status
 * @cookie [in]: user cookie
 * @context [in]: task user context
 * @buffer [in]: inline receive data, NULL if RDMA
 * @count [in]: data bytes count
 * @sender_tag [in]: sender tag
 * @status [in]: UCX status
 */
typedef void (*urom_sandbox_recv_finished)(doca_error_t result,
					   union doca_data cookie,
					   union doca_data context,
					   void *buffer,
					   uint64_t count,
					   uint64_t sender_tag,
					   ucs_status_t status);

/*
 * Memory map task callback function, will be called once the mem map task is finished
 *
 * @result [in]: task status
 * @cookie [in]: user cookie
 * @context [in]: task user context
 * @memh_id [out]: UCX memory ID
 */
typedef void (*urom_sandbox_mem_map_finished)(doca_error_t result,
					      union doca_data cookie,
					      union doca_data context,
					      uint64_t memh_id);

/*
 * Create send sandbox tag task
 *
 * @worker_ctx [in]: DOCA UROM worker context
 * @cookie [in]: user cookie
 * @context [in]: task user context
 * @dest [in]: destination id
 * @buffer [in]: data pointer
 * @count [in]: data bytes count
 * @tag [in]: tag send
 * @memh_id [in]: memory handle id
 * @cb [in]: program callback to call once the task is finished
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_sandbox_tag_task_send(struct doca_urom_worker *worker_ctx,
					union doca_data cookie,
					union doca_data context,
					uint64_t dest,
					uint64_t buffer,
					uint64_t count,
					uint64_t tag,
					uint64_t memh_id,
					urom_sandbox_send_finished cb);

/*
 * Create recv sandbox tag task
 *
 * @worker_ctx [in]: DOCA UROM worker context
 * @cookie [in]: user cookie
 * @context [in]: task user context
 * @buffer [in]: data pointer
 * @count [in]: data bytes count
 * @tag [in]: tag recv
 * @tag_mask [in]: tag recv mask
 * @memh_id [in]: memory handle id
 * @cb [in]: program callback to call once the task is finished
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 *
 * @NOTE: If buffer data is included as part of the notification (inline), it is the user's responsibility to copy
 * the buffer data.
 */
doca_error_t urom_sandbox_tag_task_recv(struct doca_urom_worker *worker_ctx,
					union doca_data cookie,
					union doca_data context,
					uint64_t buffer,
					uint64_t count,
					uint64_t tag,
					uint64_t tag_mask,
					uint64_t memh_id,
					urom_sandbox_recv_finished cb);

/*
 * Create recv sandbox tag task
 *
 * @worker_ctx [in]: DOCA UROM worker context
 * @cookie [in]: user cookie
 * @context [in]: task user context
 * @map_params [in]: memory map parameters
 * @exported_memh_buffer_len [in]: exported buffer length
 * @cb [in]: program callback to call once the task is finished
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_sandbox_task_mem_map(struct doca_urom_worker *worker_ctx,
				       union doca_data cookie,
				       union doca_data context,
				       ucp_mem_map_params_t map_params,
				       size_t exported_memh_buffer_len,
				       urom_sandbox_mem_map_finished cb);

/*
 * This method inits sandbox plugin.
 *
 * @plugin_id [in]: UROM plugin ID
 * @version [in]: plugin version on DPU side
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_sandbox_init(uint64_t plugin_id, uint64_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WORKER_SANDBOX_H_ */
