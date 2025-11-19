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

#include <doca_dpa_dev.h>
#include <doca_dpa_dev_verbs.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>

#include "../common/dpa_verbs_initiator_target_common_defs.h"

/**
 * Expected num of receives
 */
#define EXPECTED_NUM_RECEIVES (3)

/**
 * @brief Post Verbs send work request
 *
 * @qp_handle [in]: QP DPA handle
 * @send_wr [in]: Send work request
 * @sge [in]: Scatter\Gather element
 * @flags [in]: Send WR flags
 * @dst_mmap_handle [in]: Destination mmap DPA handle
 * @dst_addr [in]: Destination address
 * @opcode [in]: Send WR operation opcode
 * @fence_mode [in]: Send WR fence mode
 */
static void post_send_wr(doca_dpa_dev_verbs_qp_t qp_handle,
			 struct doca_dpa_dev_verbs_send_wr *send_wr,
			 struct doca_dpa_dev_verbs_sge *sge,
			 enum doca_dpa_dev_verbs_send_wr_flags flags,
			 uint64_t dst_addr,
			 uint32_t dst_mmap_handle,
			 enum doca_dpa_dev_verbs_send_wr_opcode opcode,
			 enum doca_dpa_dev_verbs_send_wr_fm fence_mode)
{
	doca_dpa_dev_verbs_send_wr_set_sg_list(send_wr, sge);
	doca_dpa_dev_verbs_send_wr_set_sg_num_sge(send_wr, 1);
	doca_dpa_dev_verbs_send_wr_set_send_flags(send_wr, flags);
	doca_dpa_dev_verbs_send_wr_set_rdma_rkey(send_wr, dst_mmap_handle);
	doca_dpa_dev_verbs_send_wr_set_rdma_remote_addr(send_wr, dst_addr);
	doca_dpa_dev_verbs_send_wr_set_opcode(send_wr, opcode);
	doca_dpa_dev_verbs_send_wr_set_fence_mode(send_wr, fence_mode);

	doca_dpa_dev_verbs_qp_post_send_wr(qp_handle, send_wr);
	doca_dpa_dev_verbs_qp_commit_send(qp_handle);
}

/**
 * @brief Post Verbs receive work request
 *
 * @qp_handle [in]: QP DPA handle
 * @recv_wr [in]: Receive work request
 * @sge [in]: Scatter\Gather element
 */
static void post_recv_wr(doca_dpa_dev_verbs_qp_t qp_handle,
			 struct doca_dpa_dev_verbs_recv_wr *recv_wr,
			 struct doca_dpa_dev_verbs_sge *sge)
{
	doca_dpa_dev_verbs_recv_wr_set_sg_list(recv_wr, sge);
	doca_dpa_dev_verbs_recv_wr_set_sg_num_sge(recv_wr, 1);

	doca_dpa_dev_verbs_qp_post_recv_wr(qp_handle, recv_wr);
	doca_dpa_dev_verbs_qp_commit_recv(qp_handle);
}

/**
 * @brief Initiator thread Kernel
 *
 * The initiator kernel performs EXPECTED_NUM_RECEIVES times -
 * Polling local buffer value to synchronize with Target's Write operation,
 * increasing local buffer value by 1 and than posting Send operation.
 * After all iterations, the initiator verifies success by checking local buffer's value.
 *
 * @arg [in]: pointer to thread arguments
 */
__dpa_global__ void initiator_thread_kernel(uint64_t arg)
{
	struct dpa_thread_arg *thread_arg = (struct dpa_thread_arg *)arg;
	doca_dpa_dev_completion_element_t comp_element;
	int num_receives = 0;
	struct doca_dpa_dev_verbs_send_wr send_wr;
	struct doca_dpa_dev_verbs_sge sge;
	sge.addr = thread_arg->local_dpa_buff_addr;
	sge.lkey = thread_arg->local_dpa_buff_addr_mmap_handle;
	sge.length = thread_arg->local_dpa_buff_addr_length;

	if (thread_arg->dpa_ctx_handle) {
		doca_dpa_dev_device_set(thread_arg->dpa_ctx_handle);
	}

	/* wait for send completion from RPC */
	while (!doca_dpa_dev_get_completion(thread_arg->dpa_comp_handle, &comp_element)) {}

	doca_dpa_dev_completion_ack(thread_arg->dpa_comp_handle, 1);
	num_receives++;

	while (1) {
		/* poll buffer's value (wait for Target's write operation) */
		while ((*((uint64_t *)(thread_arg->local_dpa_buff_addr))) <
		       ((uint64_t)VERBS_SAMPLE_LOCAL_BUF_START_VALUE + 2 * num_receives - 1)) {}

		(*((uint64_t *)(thread_arg->local_dpa_buff_addr)))++;

		/* finish iterations */
		if (num_receives == EXPECTED_NUM_RECEIVES) {
			/* check if the final value is as expected */
			if (*((uint64_t *)(thread_arg->local_dpa_buff_addr)) ==
			    (uint64_t)VERBS_SAMPLE_LOCAL_BUF_END_VALUE) {
				DOCA_DPA_DEV_LOG_INFO(
					"---------------------------- SUCCESS ---------------------------- \n");
				DOCA_DPA_DEV_LOG_INFO("Initiator: Local buffer value after sample is 0x%lx\n",
						      *(uint64_t *)thread_arg->local_dpa_buff_addr);
				thread_arg->return_status = 0; // Success
			} else {
				DOCA_DPA_DEV_LOG_INFO(
					"---------------------------- FAILURE ---------------------------- \n");
				DOCA_DPA_DEV_LOG_INFO(
					"Initiator: Local buffer value after sample is 0x%lx, expected value is 0x%lx\n",
					*(uint64_t *)thread_arg->local_dpa_buff_addr,
					(uint64_t)VERBS_SAMPLE_LOCAL_BUF_END_VALUE);
				thread_arg->return_status = 1; // Error
			}

			doca_dpa_dev_sync_event_update_set(thread_arg->comp_sync_event_handle,
							   thread_arg->comp_sync_event_val);
			doca_dpa_dev_thread_finish();
		}

		/* post send */
		post_send_wr(thread_arg->dpa_verbs_qp_handle,
			     &send_wr,
			     &sge,
			     DOCA_DPA_DEV_VERBS_SEND_WR_FLAGS_SIGNALED,
			     thread_arg->remote_dpa_buff_addr,
			     thread_arg->remote_dpa_buff_addr_mmap_handle,
			     DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_SEND,
			     DOCA_DPA_DEV_VERBS_SEND_WR_FM_NO_FENCE);

		/* wait for send completion */
		while (!doca_dpa_dev_get_completion(thread_arg->dpa_comp_handle, &comp_element)) {}

		doca_dpa_dev_completion_ack(thread_arg->dpa_comp_handle, 1);

		num_receives++;
	}
	doca_dpa_dev_thread_finish();
}

/**
 * @brief Target thread Kernel
 *
 * The target kernel performs EXPECTED_NUM_RECEIVES times -
 * posting Receive operation, increasing value by 1 and than posting Write operation.
 *
 * @arg [in]: pointer to thread arguments
 */
__dpa_global__ void target_thread_kernel(uint64_t arg)
{
	struct dpa_thread_arg *thread_arg = (struct dpa_thread_arg *)arg;
	doca_dpa_dev_completion_element_t comp_element;
	int num_receives = 0;
	struct doca_dpa_dev_verbs_send_wr send_wr;
	struct doca_dpa_dev_verbs_recv_wr recv_wr;
	struct doca_dpa_dev_verbs_sge sge;
	sge.addr = thread_arg->local_dpa_buff_addr;
	sge.lkey = thread_arg->local_dpa_buff_addr_mmap_handle;
	sge.length = thread_arg->local_dpa_buff_addr_length;

	if (thread_arg->dpa_ctx_handle) {
		doca_dpa_dev_device_set(thread_arg->dpa_ctx_handle);
	}

	/* wait for receive completion from RPC */
	while (!doca_dpa_dev_get_completion(thread_arg->dpa_comp_handle, &comp_element)) {}

	doca_dpa_dev_completion_ack(thread_arg->dpa_comp_handle, 1);
	num_receives++;

	while (1) {
		(*((uint64_t *)(thread_arg->local_dpa_buff_addr)))++;

		/* post receive */
		post_recv_wr(thread_arg->dpa_verbs_qp_handle, &recv_wr, &sge);

		/* post write */
		post_send_wr(thread_arg->dpa_verbs_qp_handle,
			     &send_wr,
			     &sge,
			     DOCA_DPA_DEV_VERBS_SEND_WR_FLAGS_SIGNALED,
			     thread_arg->remote_dpa_buff_addr,
			     thread_arg->remote_dpa_buff_addr_mmap_handle,
			     DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_WRITE,
			     DOCA_DPA_DEV_VERBS_SEND_WR_FM_NO_FENCE);

		/* wait for first completion */
		while (!doca_dpa_dev_get_completion(thread_arg->dpa_comp_handle, &comp_element)) {}

		doca_dpa_dev_completion_ack(thread_arg->dpa_comp_handle, 1);

		/* finish iterations */
		/* in the last iteration we are getting just one completion */
		if (num_receives == EXPECTED_NUM_RECEIVES) {
			doca_dpa_dev_sync_event_update_set(thread_arg->comp_sync_event_handle,
							   thread_arg->comp_sync_event_val);
			doca_dpa_dev_thread_finish();
		}

		/* wait for second completion */
		while (!doca_dpa_dev_get_completion(thread_arg->dpa_comp_handle, &comp_element)) {}

		doca_dpa_dev_completion_ack(thread_arg->dpa_comp_handle, 1);

		num_receives++;
	}
	doca_dpa_dev_thread_finish();
}

/**
 * @brief RPC Initiator first iteration
 *
 * The initiator RPC performs the first Send operations.
 *
 * @dpa_ctx_handle [in]: DPA context handle
 * @dpa_verbs_qp_handle [in]: DPA verbs QP handle
 * @addr [in]: local buffer address
 * @mkey [in]: local buffer mkey
 * @length [in]: local buffer length
 * @remote_dpa_buff_addr [in]: remote buffer address
 * @remote_dpa_buff_addr_mmap_handle [in]: remote buffer mmap handle
 * @return: zero on success
 */
__dpa_rpc__ uint64_t initiator_trigger_first_iteration_rpc(doca_dpa_dev_t dpa_ctx_handle,
							   doca_dpa_dev_verbs_qp_t dpa_verbs_qp_handle,
							   uint64_t addr,
							   uint32_t mkey,
							   uint32_t length,
							   doca_dpa_dev_uintptr_t remote_dpa_buff_addr,
							   doca_dpa_dev_mmap_t remote_dpa_buff_addr_mmap_handle)
{
	DOCA_DPA_DEV_LOG_INFO("Trigger initiator first iteration\n");

	if (dpa_ctx_handle) {
		doca_dpa_dev_device_set(dpa_ctx_handle);
	}

	struct doca_dpa_dev_verbs_send_wr send_wr;
	struct doca_dpa_dev_verbs_sge sge;
	sge.addr = addr;
	sge.lkey = mkey;
	sge.length = length;

	DOCA_DPA_DEV_LOG_INFO("Initiator: Local buffer value before sample is 0x%lx\n", *(uint64_t *)addr);

	/* post send */
	post_send_wr(dpa_verbs_qp_handle,
		     &send_wr,
		     &sge,
		     DOCA_DPA_DEV_VERBS_SEND_WR_FLAGS_SIGNALED,
		     remote_dpa_buff_addr,
		     remote_dpa_buff_addr_mmap_handle,
		     DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_SEND,
		     DOCA_DPA_DEV_VERBS_SEND_WR_FM_NO_FENCE);

	return 0;
}

/**
 * @brief RPC Target first iteration
 *
 * The target RPC performs the first Receive operations.
 *
 * @dpa_ctx_handle [in]: DPA context handle
 * @dpa_verbs_qp_handle [in]: DPA verbs QP handle
 * @addr [in]: local buffer address
 * @mkey [in]: local buffer mkey
 * @length [in]: local buffer length
 * @return: zero on success
 */
__dpa_rpc__ uint64_t target_trigger_first_iteration_rpc(doca_dpa_dev_t dpa_ctx_handle,
							doca_dpa_dev_verbs_qp_t dpa_verbs_qp_handle,
							uint64_t addr,
							uint32_t mkey,
							uint32_t length)
{
	DOCA_DPA_DEV_LOG_INFO("Trigger target first iteration\n");

	if (dpa_ctx_handle) {
		doca_dpa_dev_device_set(dpa_ctx_handle);
	}

	struct doca_dpa_dev_verbs_recv_wr recv_wr;
	struct doca_dpa_dev_verbs_sge sge;
	sge.addr = addr;
	sge.lkey = mkey;
	sge.length = length;

	/* post receive */
	post_recv_wr(dpa_verbs_qp_handle, &recv_wr, &sge);

	return 0;
}
