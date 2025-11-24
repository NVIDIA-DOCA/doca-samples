/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_dpa_dev.h>
#include <doca_dpa_dev_rdma.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_sync_event.h>

#define SYNC_EVENT_MASK_FFS (0xFFFFFFFFFFFFFFFF) /* Mask for doca_dpa_dev_sync_event_wait_gt() wait value */

/*
 * Alltoall kernel function.
 * Performs RDMA write operations using doca_dpa_dev_rdma_write() from local buffer to remote buffer.
 *
 * @rdma_dpa_ctx_handle [in]: Extended DPA context handle for RDMA DOCA device
 * @rdmas_dev_ptr [in]: An array of DOCA DPA RDMA handles
 * @local_buf_addr [in]: local buffer address for alltoall
 * @local_buf_mmap_handle [in]: local buffer mmap handle for alltoall
 * @count [in]: Number of elements to write
 * @type_length [in]: Length of each element
 * @num_ranks [in]: Number of the MPI ranks
 * @my_rank [in]: The rank of the current process
 * @remote_recvbufs_dev_ptr [in]: Device pointer of array holding remote buffers addresses for alltoall
 * @remote_recvbufs_mmap_handles_dev_ptr [in]: Device pointer of array holding remote buffers mmap handle for alltoall
 * @local_events_dev_ptr [in]: Device pointer of DPA handles to communication events that will be updated by remote MPI
 * ranks
 * @remote_events_dev_ptr [in]: Device pointer of DPA handles to communication events on other nodes that will be
 * updated by this rank
 * @a2a_seq_num [in]: The number of times we called the alltoall_kernel in iterations
 */
__dpa_global__ void alltoall_kernel(doca_dpa_dev_t rdma_dpa_ctx_handle,
				    doca_dpa_dev_uintptr_t rdmas_dev_ptr,
				    uint64_t local_buf_addr,
				    doca_dpa_dev_mmap_t local_buf_mmap_handle,
				    uint64_t count,
				    uint64_t type_length,
				    uint64_t num_ranks,
				    uint64_t my_rank,
				    doca_dpa_dev_uintptr_t remote_recvbufs_dev_ptr,
				    doca_dpa_dev_uintptr_t remote_recvbufs_mmap_handles_dev_ptr,
				    doca_dpa_dev_uintptr_t local_events_dev_ptr,
				    doca_dpa_dev_uintptr_t remote_events_dev_ptr,
				    uint64_t a2a_seq_num)
{
	/* Convert the RDMA DPA device pointer to rdma handle type */
	doca_dpa_dev_rdma_t *rdma_handles = (doca_dpa_dev_rdma_t *)rdmas_dev_ptr;
	/* Convert the remote receive buffer addresses DPA device pointer to array of pointers */
	uintptr_t *remote_recvbufs = (uintptr_t *)remote_recvbufs_dev_ptr;
	/* Convert the remote receive buffer mmap handles DPA device pointer to array of mmap handle type */
	doca_dpa_dev_mmap_t *remote_recvbufs_mmap_handles = (doca_dpa_dev_mmap_t *)remote_recvbufs_mmap_handles_dev_ptr;
	/* Convert the local events DPA device pointer to local events handle type */
	doca_dpa_dev_sync_event_t *local_events = (doca_dpa_dev_sync_event_t *)local_events_dev_ptr;
	/* Convert the remote events DPA device pointer to remote events handle type */
	doca_dpa_dev_sync_event_remote_net_t *remote_events =
		(doca_dpa_dev_sync_event_remote_net_t *)remote_events_dev_ptr;
	/* Get the rank of current thread that is running */
	unsigned int thread_rank = doca_dpa_dev_thread_rank();
	/* Get the number of all threads that are running this kernel */
	unsigned int num_threads = doca_dpa_dev_num_threads();
	unsigned int i;

	if (rdma_dpa_ctx_handle) {
		doca_dpa_dev_device_set(rdma_dpa_ctx_handle);
	}

	/*
	 * Each process should perform as the number of processes RDMA write operations with local and remote buffers
	 * according to the rank of the local process and the rank of the remote processes (we iterate over the rank
	 * of the remote process).
	 * Each process runs num_threads threads on this kernel so we divide the number RDMA write operations (which is
	 * the number of processes) by the number of threads.
	 */
	for (i = thread_rank; i < num_ranks; i += num_threads) {
		doca_dpa_dev_rdma_post_write(rdma_handles[i],
					     0,
					     remote_recvbufs_mmap_handles[i],
					     remote_recvbufs[i] + (my_rank * count * type_length),
					     local_buf_mmap_handle,
					     local_buf_addr + (i * count * type_length),
					     (type_length * count),
					     DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS |
						     DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);

		doca_dpa_dev_rdma_signal_set(rdma_handles[i], 0, remote_events[i], a2a_seq_num);
	}

	/*
	 * Each thread should wait on his local events to make sure that the
	 * remote processes have finished RDMA write operations.
	 */
	for (i = thread_rank; i < num_ranks; i += num_threads) {
		doca_dpa_dev_sync_event_wait_gt(local_events[i], a2a_seq_num - 1, SYNC_EVENT_MASK_FFS);
	}
}
