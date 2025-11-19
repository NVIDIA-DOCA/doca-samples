/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include "../common/dpa_basic_initiator_target_common_defs.h"

/**
 * @brief Kernel function for DPA thread
 *
 * This kernel is triggered when a completion is received on attached RDMA context.
 * This kernel is triggered once when it receives data on DPA buffer with value 10.
 * On completion, it gets and dumps completion info.
 * This kernel sets host completion sync event with the received data value to let host application
 * start destroying all resources and finishing the application
 *
 * @arg [in]: Kernel argument
 */
__dpa_global__ void thread_kernel(uint64_t arg)
{
	DOCA_DPA_DEV_LOG_INFO("%s: Hello from Thread\n", __func__);

	struct dpa_thread_arg *thread_arg = (struct dpa_thread_arg *)arg;

	if (thread_arg->dpa_ctx_handle) {
		doca_dpa_dev_device_set(thread_arg->dpa_ctx_handle);
	}

	DOCA_DPA_DEV_LOG_INFO("%s: Read completion info\n", __func__);
	doca_dpa_dev_completion_element_t comp_element;
	int found = doca_dpa_dev_get_completion(thread_arg->dpa_comp_handle, &comp_element);
	DOCA_DPA_DEV_LOG_INFO("%s: doca_dpa_dev_get_completion() returned %d\n", __func__, found);

	if (found) {
		DOCA_DPA_DEV_LOG_INFO("%s: doca_dpa_dev_get_completion_type() returned %d\n",
				      __func__,
				      doca_dpa_dev_get_completion_type(comp_element));
		DOCA_DPA_DEV_LOG_INFO("%s: doca_dpa_dev_get_completion_user_data() returned %u\n",
				      __func__,
				      doca_dpa_dev_get_completion_user_data(comp_element));

		DOCA_DPA_DEV_LOG_INFO("%s: Value of DPA received buffer is %lu\n",
				      __func__,
				      *((uint64_t *)(thread_arg->local_buf_addr)));
	}

	DOCA_DPA_DEV_LOG_INFO("%s: Set sync event to %lu\n", __func__, *((uint64_t *)(thread_arg->local_buf_addr)));
	doca_dpa_dev_sync_event_update_set(thread_arg->sync_event_handle, *((uint64_t *)(thread_arg->local_buf_addr)));

	DOCA_DPA_DEV_LOG_INFO("%s: Finish\n", __func__);
	doca_dpa_dev_thread_finish();
}

/**
 * @brief RPC function to post RDMA receive operation
 *
 * This RPC is used by target host application to post RDMA receive operation on DPA local buffer
 *
 * @rdma_dpa_ctx_handle [in]: DPA context handle used for RDMA DOCA device
 * @rdma [in]: RDMA DPA handle
 * @local_buf_addr [in]: address of received buffer
 * @dpa_mmap_handle [in]: received DOCA Mmap handle
 * @length [in]: length of received buffer
 * @return: RPC function always succeed and returns 0
 */
__dpa_rpc__ uint64_t rdma_post_receive_rpc(doca_dpa_dev_t rdma_dpa_ctx_handle,
					   doca_dpa_dev_rdma_t rdma,
					   doca_dpa_dev_uintptr_t local_buf_addr,
					   doca_dpa_dev_mmap_t dpa_mmap_handle,
					   uint64_t length)
{
	DOCA_DPA_DEV_LOG_INFO("%s: Post RDMA receive on DPA Mmap handle %u, address 0x%lx, length %lu\n",
			      __func__,
			      dpa_mmap_handle,
			      local_buf_addr,
			      length);
	if (rdma_dpa_ctx_handle) {
		doca_dpa_dev_device_set(rdma_dpa_ctx_handle);
	}
	doca_dpa_dev_rdma_post_receive(rdma, dpa_mmap_handle, local_buf_addr, length);

	return 0;
}

/**
 * @brief RPC function to post RDMA send operation
 *
 * This RPC is used by initiator host application to post RDMA send operation on host local buffer
 *
 * @rdma_dpa_ctx_handle [in]: DPA context handle used for RDMA DOCA device
 * @rdma [in]: RDMA DPA handle
 * @local_buf_addr [in]: address of send buffer
 * @dpa_mmap_handle [in]: send DOCA Mmap handle
 * @length [in]: length of send buffer
 * @return: RPC function always succeed and returns 0
 */
__dpa_rpc__ uint64_t rdma_post_send_rpc(doca_dpa_dev_t rdma_dpa_ctx_handle,
					doca_dpa_dev_rdma_t rdma,
					uintptr_t local_buf_addr,
					doca_dpa_dev_mmap_t dpa_mmap_handle,
					uint64_t length)
{
	DOCA_DPA_DEV_LOG_INFO("%s: Post RDMA send on DPA Mmap handle %u, address 0x%lx, length %lu\n",
			      __func__,
			      dpa_mmap_handle,
			      local_buf_addr,
			      length);
	if (rdma_dpa_ctx_handle) {
		doca_dpa_dev_device_set(rdma_dpa_ctx_handle);
	}
	doca_dpa_dev_rdma_post_send(rdma,
				    0,
				    dpa_mmap_handle,
				    local_buf_addr,
				    length,
				    DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH | DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);

	return 0;
}
