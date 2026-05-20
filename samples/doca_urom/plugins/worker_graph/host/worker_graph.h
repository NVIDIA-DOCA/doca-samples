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

#ifndef WORKER_GRAPH_H_
#define WORKER_GRAPH_H_

#include "ucp/api/ucp.h"

#include <doca_urom.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Loopback task callback function, will be called once the loopback task is finished
 *
 * @result [in]: task result
 * @cookie [in]: user cookie
 * @data [in]: loopback data
 */
typedef void (*urom_graph_loopback_finished)(doca_error_t result, union doca_data cookie, uint64_t data);

/*
 * Creates send graph tag task
 *
 * @worker_ctx [in]: DOCA UROM worker context
 * @cookie [in]: user cookie
 * @data [in]: loopback data
 * @cb [in]: program callback to call once the task is finished
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_graph_task_loopback(struct doca_urom_worker *worker_ctx,
				      union doca_data cookie,
				      uint64_t data,
				      urom_graph_loopback_finished cb);

/*
 *
 * This method inits graph plugin.
 *
 * @plugin_id [in]: plugin id
 * @version [in]: plugin version on DPU side
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_graph_init(uint64_t plugin_id, uint64_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WORKER_GRAPH_H_ */
