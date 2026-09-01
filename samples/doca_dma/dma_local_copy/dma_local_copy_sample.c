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
 * Free DOCA buffer array
 *
 * @doca_buf [in]: DOCA buffer array
 * @num_buf [in]: Number of DOCA buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t free_doca_buf_array(struct doca_buf *doca_buf[], int num_buf)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp_result;

	for (int i = 0; i < num_buf; i++) {
		if (doca_buf[i] == NULL)
			continue;
		tmp_result = doca_buf_dec_refcount(doca_buf[i], NULL);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to decrease DOCA buffer reference count: %s",
				     doca_error_get_descr(tmp_result));
		}
		doca_buf[i] = NULL;
	}
	return result;
}

/*
 * Allocate DOCA buffer for task
 *
 * @doca_buf [in]: DOCA buffer array
 * @dma_conf [in]: DMA configuration
 * @state [in]: Program core objects
 * @buffer [in]: Buffer start address
 * @length [in]: Single allocated DOCA buffer's size
 * @is_source [in]: True if source buffer, false if destination buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t allocate_doca_buf_for_task(struct doca_buf *doca_buf[],
					       const struct dma_config *dma_conf,
					       struct program_core_objects *state,
					       char *buffer,
					       size_t length,
					       bool is_source)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp_result;
	struct doca_mmap *mmap = is_source ? state->src_mmap : state->dst_mmap;
	int num_buf_elem = is_source ? dma_conf->num_src_buf : dma_conf->num_dst_buf;
	int num_tasks = dma_conf->num_tasks;

	for (int i = 0; i < num_tasks; i++) {
		result = allocate_doca_buf_list(state->buf_inv,
						mmap,
						buffer + i * length,
						length,
						num_buf_elem,
						is_source,
						&doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing %s buffer %d: %s",
				     is_source ? "source" : "destination",
				     i,
				     doca_error_get_descr(result));
			tmp_result = free_doca_buf_array(doca_buf, i);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_ERROR_PROPAGATE(result, tmp_result);
				DOCA_LOG_ERR(
					"Failed to decrease DOCA buffer reference count representing %d %s buffers: %s",
					i,
					is_source ? "source" : "destination",
					doca_error_get_descr(tmp_result));
			}
			break;
		}
	}

	return result;
}

static inline void free_dma_tasks(struct doca_dma_task_memcpy *dma_tasks[], int start_index, int end_index)
{
	for (int i = start_index; i < end_index; i++) {
		if (dma_tasks[i] == NULL)
			continue;
		doca_task_free(doca_dma_task_memcpy_as_task(dma_tasks[i]));
		dma_tasks[i] = NULL;
	}
}

/*
 * Run DOCA DMA local copy sample
 *
 * @dma_conf [in]: DMA sample configuration
 * @dst_buffer [in]: Destination buffer
 * @src_buffer [in]: Source buffer to copy
 * @length [in]: Buffer's size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dma_local_copy(const struct dma_config *dma_conf, char *dst_buffer, char *src_buffer, size_t length)
{
	struct dma_resources resources;
	struct program_core_objects *state = &resources.state;
	struct doca_dma_task_memcpy *dma_tasks[MAX_NUM_TASKS] = {NULL};
	struct doca_task *tasks = NULL;
	union doca_data task_user_data[MAX_NUM_TASKS] = {0};
	struct doca_buf *src_doca_buf[MAX_NUM_TASKS] = {NULL};
	struct doca_buf *dst_doca_buf[MAX_NUM_TASKS] = {NULL};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;
	doca_error_t tmp_result;
	doca_error_t task_results[MAX_NUM_TASKS] = {DOCA_SUCCESS};
	int i = 0;
	int num_submitted = 0;
	int num_tasks = dma_conf->num_tasks;
	size_t total_length = length * num_tasks;
	int max_num_range_profiling_displayed = dma_conf->max_num_range_profiling_displayed;

	if (dst_buffer == NULL || src_buffer == NULL || length == 0) {
		DOCA_LOG_ERR("Invalid input values, addresses and sizes must not be 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = memory_ranges_overlap(dst_buffer, src_buffer, total_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Memory ranges must not overlap");
		return result;
	}

	/* Allocate resources */
	result = allocate_dma_resources(dma_conf->pci_address,
					dma_conf->num_src_buf,
					dma_conf->num_dst_buf,
					num_tasks,
					&resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	/*
	 * Set maximum number of NVTX ranges that can be displayed in parallel.
	 * If max_num_range_profiling_displayed is set to 0 , NVTX trace collecting is disabled.
	 */
	if (max_num_range_profiling_displayed > 0) {
		result = doca_ctx_profiler_set_max_nranges(state->ctx, max_num_range_profiling_displayed);
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_WARN(
				"Failed to set maximum number of NVTX ranges due to NVTX profiling is not supported: %s",
				doca_error_get_descr(result));
		} else {
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to set maximum number of NVTX ranges that can be displayed in parallel: %s",
					doca_error_get_descr(result));
				goto destroy_resources;
			}
		}
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

	result = register_memory_range_and_start_mmap(state->dst_mmap, dst_buffer, total_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create and start destination mmap: %s", doca_error_get_descr(result));
		goto stop_dma;
	}

	result = register_memory_range_and_start_mmap(state->src_mmap, src_buffer, total_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create and start source mmap: %s", doca_error_get_descr(result));
		goto stop_dma;
	}

	/* Clear destination memory buffer */
	memset(dst_buffer, 0, total_length);

	/* Construct DOCA buffer for each address range */
	result = allocate_doca_buf_for_task(src_doca_buf, dma_conf, state, src_buffer, length, true);
	if (result != DOCA_SUCCESS) {
		goto stop_dma;
	}

	/* Construct DOCA buffer for each address range */
	result = allocate_doca_buf_for_task(dst_doca_buf, dma_conf, state, dst_buffer, length, false);
	if (result != DOCA_SUCCESS) {
		goto destroy_src_buf;
	}

	for (i = 0; i < num_tasks; i++) {
		/* Include result in user data of task to be used in the callbacks */
		task_user_data[i].ptr = &task_results[i];
		/* Allocate and construct DMA task */
		result = doca_dma_task_memcpy_alloc_init(resources.dma_ctx,
							 src_doca_buf[i],
							 dst_doca_buf[i],
							 task_user_data[i],
							 &dma_tasks[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate DMA memcpy task %d: %s", i, doca_error_get_descr(result));
			/* Free all tasks allocated until now */
			free_dma_tasks(dma_tasks, 0, i);
			goto destroy_dst_buf;
		}
	}

	/* Number of tasks submitted to progress engine */
	resources.num_remaining_tasks = 0;
	num_submitted = 0;

	for (i = 0; i < num_tasks; i++) {
		tasks = doca_dma_task_memcpy_as_task(dma_tasks[i]);

		/* Submit DMA task */
		if (max_num_range_profiling_displayed == 0) {
			/* Without task range profiling requirement, submit it with default flag
			 * (DOCA_TASK_SUBMIT_FLAG_FLUSH) */
			result = doca_task_submit(tasks);
		} else {
			/* With task range profiling requirement, submit it with range profiling requested flag
			 * (DOCA_TASK_SUBMIT_FLAG_RANGE_PROFILING_REQUESTED) */
			result = doca_task_submit_ex(tasks,
						     DOCA_TASK_SUBMIT_FLAG_FLUSH |
							     DOCA_TASK_SUBMIT_FLAG_RANGE_PROFILING_REQUESTED);
		}
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit DMA task %d: %s", i, doca_error_get_descr(result));
			/* Free all not submitted but allocated tasks */
			free_dma_tasks(dma_tasks, i, num_tasks);
			/* Go to progress_pe to process all submitted but not completed tasks */
			break;
		}

		num_submitted++;
		(resources.num_remaining_tasks)++;
	}
	if (num_submitted == 0) {
		DOCA_LOG_ERR("No tasks submitted successfully");
		goto destroy_dst_buf;
	}

	resources.run_pe_progress = true;

	/* Wait for all tasks to be completed and context stopped */
	while (resources.run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* If any task submission fails, do not check the results of the submitted tasks, just return the error */
	if (result != DOCA_SUCCESS) {
		goto destroy_dst_buf;
	}

	for (i = 0; i < num_submitted; i++) {
		/* Check result of task according to the result we update in the callbacks */
		if (task_results[i] == DOCA_SUCCESS) {
			DOCA_LOG_INFO("Success, DMA memory task %d copied and verified as correct", i);
		} else {
			DOCA_LOG_ERR("DMA memcpy task %d failed: %s", i, doca_error_get_descr(task_results[i]));
			DOCA_ERROR_PROPAGATE(result, task_results[i]);
		}
	}

destroy_dst_buf:
	tmp_result = free_doca_buf_array(dst_doca_buf, num_tasks);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to free DOCA destination buffer array: %s", doca_error_get_descr(tmp_result));
	}
destroy_src_buf:
	tmp_result = free_doca_buf_array(src_doca_buf, num_tasks);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to free DOCA source buffer array: %s", doca_error_get_descr(tmp_result));
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
