/*
 * Copyright (c) 2022 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdint.h>
#include <string.h>
#include <time.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "dma_common.h"

DOCA_LOG_REGISTER(DPU_LOCAL_DMA_COPY);

#define SLEEP_IN_NANOS (10 * 1000) /* Sample the task every 10 microseconds  */

/*
 * Checks that the two buffers are not overlap each other
 *
 * @dst_buffer [in]: Destination buffer
 * @src_buffer [in]: Source buffer
 * @length [in]: Length of both buffers
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t memory_ranges_overlap(const char *dst_buffer, const char *src_buffer, size_t length)
{
	const char *dst_range_end = dst_buffer + length;
	const char *src_range_end = src_buffer + length;

	if (((dst_buffer >= src_buffer) && (dst_buffer < src_range_end)) ||
	    ((src_buffer >= dst_buffer) && (src_buffer < dst_range_end))) {
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Register buffer with mmap and start it
 *
 * @mmap [in]: Memory Map object
 * @buffer [in]: Buffer
 * @length [in]: Buffer's size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_memory_range_and_start_mmap(struct doca_mmap *mmap, char *buffer, size_t length)
{
	doca_error_t result;

	result = doca_mmap_set_memrange(mmap, buffer, length);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_mmap_start(mmap);
}

/*
 * Run DOCA DMA local copy sample
 *
 * @pcie_addr [in]: Device PCI address
 * @dst_buffer [in]: Destination buffer
 * @src_buffer [in]: Source buffer to copy
 * @length [in]: Buffer's size
 * @num_src_buf [in]: Number of source doca_buf to allocate
 * @num_dst_buf [in]: Number of destination doca_buf to allocate
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dma_local_copy(const char *pcie_addr,
			    char *dst_buffer,
			    char *src_buffer,
			    size_t length,
			    int num_src_buf,
			    int num_dst_buf)
{
	struct dma_resources resources;
	struct program_core_objects *state = &resources.state;
	struct doca_dma_task_memcpy *dma_task = NULL;
	struct doca_task *task = NULL;
	union doca_data task_user_data = {0};
	struct doca_buf *src_doca_buf = NULL;
	struct doca_buf *dst_doca_buf = NULL;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result, tmp_result, task_result;

	if (dst_buffer == NULL || src_buffer == NULL || length == 0) {
		DOCA_LOG_ERR("Invalid input values, addresses and sizes must not be 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = memory_ranges_overlap(dst_buffer, src_buffer, length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Memory ranges must not overlap");
		return result;
	}

	/* Allocate resources */
	result = allocate_dma_resources(pcie_addr, num_src_buf + num_dst_buf, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	/* Connect context to progress engine */
	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	result = register_memory_range_and_start_mmap(state->dst_mmap, dst_buffer, length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create and start source mmap: %s", doca_error_get_descr(result));
		goto stop_dma;
	}

	result = register_memory_range_and_start_mmap(state->src_mmap, src_buffer, length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create and start destination mmap: %s", doca_error_get_descr(result));
		goto stop_dma;
	}

	/* Clear destination memory buffer */
	memset(dst_buffer, 0, length);

	/* Construct DOCA buffer for each address range */
	result = allocat_doca_buf_list(state->buf_inv,
				       state->src_mmap,
				       src_buffer,
				       length,
				       num_src_buf,
				       true,
				       &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		goto stop_dma;
	}

	/* Construct DOCA buffer for each address range */
	result = allocat_doca_buf_list(state->buf_inv,
				       state->dst_mmap,
				       dst_buffer,
				       length,
				       num_dst_buf,
				       false,
				       &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		goto destroy_src_buf;
	}

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct DMA task */
	result = doca_dma_task_memcpy_alloc_init(resources.dma_ctx,
						 src_doca_buf,
						 dst_doca_buf,
						 task_user_data,
						 &dma_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA memcpy task: %s", doca_error_get_descr(result));
		goto destroy_dst_buf;
	}
	/* Number of tasks submitted to progress engine */
	resources.num_remaining_tasks = 1;

	task = doca_dma_task_memcpy_as_task(dma_task);

	/* Submit DMA task */
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit DMA task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		goto destroy_dst_buf;
	}

	resources.run_pe_progress = true;

	/* Wait for all tasks to be completed and context stopped */
	while (resources.run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Check result of task according to the result we update in the callbacks */
	if (task_result == DOCA_SUCCESS)
		DOCA_LOG_INFO("Success, memory copied and verified as correct");
	else
		DOCA_LOG_ERR("DMA memcpy task failed: %s", doca_error_get_descr(task_result));

	result = task_result;

destroy_dst_buf:
	tmp_result = doca_buf_dec_refcount(dst_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to decrease DOCA destination buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
	}
destroy_src_buf:
	tmp_result = doca_buf_dec_refcount(src_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to decrease DOCA source buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
	}
stop_dma:
	tmp_result = doca_ctx_stop(state->ctx);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Unable to stop context: %s", doca_error_get_descr(tmp_result));
	}
	state->ctx = NULL;
destroy_resources:
	tmp_result = destroy_dma_resources(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DMA resources: %s", doca_error_get_descr(tmp_result));
	}

	return result;
}
