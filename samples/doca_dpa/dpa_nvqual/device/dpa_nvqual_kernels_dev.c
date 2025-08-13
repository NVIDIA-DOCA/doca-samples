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

#include <stdint.h>
#include <stddef.h>
#include <dpaintrin.h>
#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include "../common/dpa_nvqual_common_defs.h"

/**
 * @brief Kernel function of DPA nvqual thread
 *
 * The function copies source buffer to destination buffer,
 * and returns the number of cycles of the DPA thread it took
 *
 * The function uses thread local storage parameters:
 * source buffer, destination buffer, buffers size and number of operations.
 *
 */
__dpa_global__ void dpa_nvqual_kernel(void)
{
	DOCA_DPA_DEV_LOG_DBG("DPA thread kernel has been activated\n");

	struct dpa_nvqual_tls *tls = (struct dpa_nvqual_tls *)doca_dpa_dev_thread_get_local_storage();

	DOCA_DPA_DEV_LOG_DBG("%s called with src=%ld, dst=%ld, size=%ld, num_ops=%ld\n",
			     __func__,
			     tls->src_buf,
			     tls->dst_buf,
			     tls->buffers_size,
			     tls->num_ops);

	uint64_t src = tls->src_buf;
	uint64_t dst = tls->dst_buf;
	size_t size = tls->buffers_size;
	uint64_t num_ops = tls->num_ops;
	uint64_t start_cycles, end_cycles;

	start_cycles = __dpa_thread_cycles();
	for (uint64_t op_idx = 0; op_idx < num_ops; op_idx++) {
		for (size_t i = 0; i < size; i++) {
			((uint8_t *)dst)[i] = ((uint8_t *)src)[i];
		}
	}

	end_cycles = __dpa_thread_cycles();

	uint64_t ret_val = end_cycles - start_cycles;

	((struct dpa_nvqual_thread_ret *)tls->thread_ret)->eu = tls->eu;
	((struct dpa_nvqual_thread_ret *)tls->thread_ret)->val = ret_val;

	doca_dpa_dev_sync_event_update_add(tls->dev_se, 1);

	doca_dpa_dev_thread_reschedule();
}

/**
 * @brief DPA nvqual entry point RPC function
 *
 * The function notifies all threads from notification completion pointer
 *
 * @dev_notification_completions [in]: Notification completion pointer
 * @num_threads [in]: Number of threads
 * @return: Zero on success
 */
__dpa_rpc__ uint64_t dpa_nvqual_entry_point(doca_dpa_dev_uintptr_t dev_notification_completions, uint64_t num_threads)
{
	DOCA_DPA_DEV_LOG_DBG("DPA entry-point RPC has been called\n");

	for (uint64_t i = 0; i < num_threads; i++) {
		doca_dpa_dev_thread_notify(((doca_dpa_dev_notification_completion_t *)dev_notification_completions)[i]);
	}

	DOCA_DPA_DEV_LOG_DBG("DPA entry-point RPC completed\n");

	return 0;
}