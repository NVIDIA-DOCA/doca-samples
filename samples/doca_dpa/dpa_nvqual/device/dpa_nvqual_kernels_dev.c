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
 * @brief Memory stress kernel function of DPA nvqual thread
 *
 * The function performs intensive memory write operations to stress the memory subsystem,
 * and returns the number of cycles of the DPA thread it took
 *
 * The function uses thread local storage parameters:
 * destination buffer, buffers size, number of operations.
 *
 * After checks and experiments, this memory stress function proposed to gain more memory stress:
 * 	This memory function performs 32 separate memory writes per iteration, each with different patterns.
 * 	The code uses large strides (increments of 8, 16, 24, ..., up to 248) between memory writes within a single
 * iteration. These large strides are chosen to minimize cache hits (skip L1 and L2 cache) and maximize memory subsystem
 * stress, ensuring that each write targets a different cache line or memory bank. The base index for each set of writes
 * is calculated using both the operation index and the EU id to further spread accesses across the buffer and avoid
 * overlapping between threads. This approach increases the likelihood of exercising different memory channels and
 * reduces the effectiveness of hardware prefetchers, thereby providing a more intensive and realistic memory stress
 * test. Or operation is used to avoid compiler optimizations and ensure memory barriers.
 *
 */
__dpa_global__ void dpa_nvqual_memory_stress_kernel(void)
{
	DOCA_DPA_DEV_LOG_DBG("DPA thread memory stress kernel has been activated\n");

	struct dpa_nvqual_tls *tls = (struct dpa_nvqual_tls *)doca_dpa_dev_thread_get_local_storage();

	DOCA_DPA_DEV_LOG_DBG("%s called with dst=%ld, size=%ld, num_ops=%ld\n",
			     __func__,
			     tls->dst_buf,
			     tls->buffers_size,
			     tls->num_ops);

	// volatile is used to avoid compiler optimizations and ensure memory barriers
	volatile uint64_t *data = (volatile uint64_t *)tls->dst_buf;
	uint64_t eu_id = tls->eu;
	size_t num_elements = tls->buffers_size / sizeof(uint64_t); // Convert bytes to elements
	uint64_t num_ops = tls->num_ops;
	uint64_t start_cycles, end_cycles;

	start_cycles = __dpa_thread_cycles();
	for (uint64_t op_idx = 0; op_idx < num_ops; op_idx++) {
		// Calculate base index to spread memory accesses across buffer and avoid cache hits:
		// - Multiply by 256 elements (2KB) to create large strides between accesses
		// - Use (op_idx % 4) to cycle through 4 different base regions per EU
		// - Use (4 * eu_id) to separate each EU's access pattern, preventing overlap
		// - Modulo num_elements to wrap around and stay within buffer bounds
		size_t index = (256 * (op_idx % 4 + 4 * eu_id)) % num_elements;

		// 32 memory writes in 16 pairs - stress the memory subsystem, where 248 is the highest offset
		// Using loop for better readability while maintaining same memory access pattern
		// 0xcafecafe00000000ULL and 0xdeadbeef00000000ULL are used to avoid compiler optimizations
		for (int i = 0; i < 16; i++) {
			// Prevent compiler optimizations and ensure memory barriers for each iteration
			__dpa_compiler_barrier();

			size_t offset = index + (i * 16);
			if (offset + 8 < num_elements) {
				data[offset] = 0xcafecafe00000000ULL | eu_id;
				data[offset + 8] = 0xdeadbeef00000000ULL | op_idx;
			}
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
 * @brief Arithmetic stress kernel function of DPA nvqual thread
 *
 * The function performs intensive arithmetic operations to stress the arithmetic unit,
 * and returns the number of cycles of the DPA thread it took
 *
 * The function uses thread local storage parameters:
 * number of operations.
 *
 * After checks and experiments, this arithmetic stress function proposed to gain more arithmetic stress:
 * 	This arithmetic function performs 100 iterations of 10 arithmetic operations per iteration.
 * 	Each arithmetic operation is a complex arithmetic operation that is not easily optimized by the compiler.
 * 	Disabling compiler optimizations is used to avoid compiler optimizations and ensure memory barriers.
 *
 */
__dpa_global__ void dpa_nvqual_arithmetic_stress_kernel(void)
{
	DOCA_DPA_DEV_LOG_DBG("DPA thread arithmetic stress kernel has been activated\n");

	struct dpa_nvqual_tls *tls = (struct dpa_nvqual_tls *)doca_dpa_dev_thread_get_local_storage();

	DOCA_DPA_DEV_LOG_DBG("%s called with num_ops=%ld\n", __func__, tls->num_ops);

	uint64_t num_ops = tls->num_ops;
	uint64_t start_cycles, end_cycles;

	// Define local variables for arithmetic operations
	volatile int64_t array[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	volatile int sum = 0;

	start_cycles = __dpa_thread_cycles();
	for (uint64_t op_idx = 0; op_idx < num_ops * 100; op_idx++) {
		for (int j = 0; j < 10; ++j) {
			// Prevent compiler optimizations
			__dpa_compiler_barrier();
			sum += array[j];					// Sum the elements
			array[j] *= array[j];					// Square the elements
			array[j] += sum;					// Add sum to each element (dependency)
			array[j] = (array[j] * j) - (sum * j) + (array[j] * j); // Complex arithmetic
			array[j] += 5;						// Add sum to each element (dependency)
			array[j] += sum;					// Add sum to each element (dependency)
			sum -= array[j];					// Subtract modified element from sum
			sum = (sum * 7) + (array[j] * 3);			// Avoid division and modulo
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