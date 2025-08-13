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

#include "dpa_common.h"
#include "../common/dpa_basic_initiator_target_common_defs.h"

DOCA_LOG_REGISTER(DPA_BASIC_INITIATOR_TARGET::SAMPLE);

/**
 * Initiator RPC declaration
 */
doca_dpa_func_t rdma_post_send_rpc;

/**
 * Target kernels/RPC declaration
 */
doca_dpa_func_t thread_kernel;
doca_dpa_func_t rdma_post_receive_rpc;

/**
 * @brief Create, attach and start DPA RDMA object
 *
 * This function creates DPA RDMA object, attaches to DPA RDMA completion and start it
 *
 * @resources [in]: DPA resources
 * @dpa_completion_obj [in]: DPA completion
 * @dpa_rdma_obj [out]: Created DPA RDMA
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_obj_create(struct dpa_resources *resources,
				    struct dpa_completion_obj *dpa_completion_obj,
				    struct dpa_rdma_obj *dpa_rdma_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;

	DOCA_LOG_INFO("Create DOCA RDMA");
	dpa_rdma_obj->doca_device = resources->rdma_doca_device;
	dpa_rdma_obj->doca_dpa = resources->rdma_dpa_ctx;
	dpa_rdma_obj->permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
				    DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC;
	dpa_rdma_obj->buf_list_len = RDMA_DEFAULT_BUF_LIST_LEN;
	dpa_rdma_obj->gid_index = resources->gid_index;
	dpa_rdma_obj->is_gid_index_set = resources->is_gid_index_set;
	dpa_rdma_obj->recv_queue_size = 1;
	dpa_rdma_obj->dpa_comp = dpa_completion_obj->dpa_comp;
	doca_err = dpa_rdma_obj_init(dpa_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto exit;
	}

	DOCA_LOG_INFO("Start DOCA RDMA");
	doca_err = dpa_rdma_obj_start(dpa_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_start failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_rdma_obj;
	}

	return doca_err;

destroy_rdma_obj:
	tmp_doca_err = dpa_rdma_obj_destroy(dpa_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

exit:
	return doca_err;
}

/**
 * @brief Destroy Initiator/Target DPA RDMA object
 *
 * @dpa_rdma_obj [in]: Previously created Initiator/Target DPA RDMA
 * @is_initiator [in]: true if object is Initiator DPA RDMA object
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_obj_destroy(struct dpa_rdma_obj *dpa_rdma_obj, uint8_t is_initiator)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;

	DOCA_LOG_INFO("Destroy %s DOCA RDMA", (is_initiator ? "Initiator" : "Target"));
	tmp_doca_err = dpa_rdma_obj_destroy(dpa_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

	return doca_err;
}

/**
 * @brief RDMA Connect Initiator & Target DPA RDMA objects
 *
 * This function connect Initiator DPA RDMA with Target DPA RDMA
 *
 * @initiator_rdma_obj [in]: Initiator DPA RDMA
 * @target_rdma_obj [in]: Target DPA RDMA
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_objs_connect(struct dpa_rdma_obj *initiator_rdma_obj, struct dpa_rdma_obj *target_rdma_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS;

	DOCA_LOG_INFO("Connect Initiator DOCA RDMA to Target DOCA RDMA");
	doca_err = doca_rdma_connect(initiator_rdma_obj->rdma,
				     target_rdma_obj->connection_details,
				     target_rdma_obj->conn_det_len,
				     initiator_rdma_obj->connection);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_connect failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	DOCA_LOG_INFO("Connect Target DOCA RDMA to Initiator DOCA RDMA");
	doca_err = doca_rdma_connect(target_rdma_obj->rdma,
				     initiator_rdma_obj->connection_details,
				     initiator_rdma_obj->conn_det_len,
				     target_rdma_obj->connection);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_connect failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	return doca_err;
}

/**
 * @brief Sample's Logic
 *
 * @resources [in]: DPA resources that the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_basic_initiator_target(struct dpa_resources *resources)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;
	struct dpa_rdma_obj initiator_rdma_obj = {0};
	doca_dpa_dev_uintptr_t thread_arg_dev_ptr = 0;
	struct dpa_thread_obj target_thread_obj = {0};
	struct dpa_completion_obj initiator_dpa_completion_obj = {0};
	struct dpa_completion_obj target_dpa_completion_obj = {0};
	struct dpa_rdma_obj target_rdma_obj = {0};
	struct doca_sync_event *comp_event = NULL;
	doca_dpa_dev_sync_event_t dpa_dev_se_handle = 0;
	doca_dpa_dev_uintptr_t received_buf_dev_ptr = 0;
	struct doca_mmap_obj dpa_mmap_obj = {0};
	struct dpa_thread_arg thread_arg = {0};
	uint64_t send_val = 10;
	struct doca_mmap_obj host_mmap_obj = {0};
	uint64_t retval = 0;
	const uint64_t expected_receive_val = 10;

	DOCA_LOG_INFO("Create Initiator DOCA DPA Completion");
	initiator_dpa_completion_obj.doca_dpa = resources->rdma_dpa_ctx;
	initiator_dpa_completion_obj.queue_size = 2;
	doca_err = dpa_completion_obj_init(&initiator_dpa_completion_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto exit;
	}

	DOCA_LOG_INFO("Initialize Initiator DOCA RDMA");
	doca_err = rdma_obj_create(resources, &initiator_dpa_completion_obj, &initiator_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_obj_create failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_initiator_rdma_completion_obj;
	}

	DOCA_LOG_INFO("Allocate Target DPA thread device argument");
	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(struct dpa_thread_arg), &thread_arg_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_initiator_rdma_obj;
	}

	DOCA_LOG_INFO("Create Target DOCA DPA Thread");
	target_thread_obj.doca_dpa = resources->rdma_dpa_ctx;
	target_thread_obj.func = &thread_kernel;
	target_thread_obj.arg = thread_arg_dev_ptr;
	target_thread_obj.affinity = resources->num_affinities > 0 ? resources->affinities[0] : NULL;

	doca_err = dpa_thread_obj_init(&target_thread_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_target_thread_arg_dev_ptr;
	}

	DOCA_LOG_INFO("Create Target DOCA DPA Completion");
	target_dpa_completion_obj.doca_dpa = resources->rdma_dpa_ctx;
	target_dpa_completion_obj.queue_size = 2;
	target_dpa_completion_obj.thread = target_thread_obj.thread;
	doca_err = dpa_completion_obj_init(&target_dpa_completion_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_target_thread_obj;
	}

	DOCA_LOG_INFO("Initialize Target DOCA RDMA");
	doca_err = rdma_obj_create(resources, &target_dpa_completion_obj, &target_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_obj_create failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_target_rdma_completion_obj;
	}

	doca_err = rdma_objs_connect(&initiator_rdma_obj, &target_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_objs_connect failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_target_rdma_obj;
	}

	DOCA_LOG_INFO("Run Target DOCA DPA Thread");
	doca_err = doca_dpa_thread_run(target_thread_obj.thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_run failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_target_rdma_obj;
	}

	DOCA_LOG_INFO("Create completion DOCA sync event");
	doca_err = create_doca_dpa_completion_sync_event(resources->pf_dpa_ctx,
							 resources->pf_doca_device,
							 &comp_event,
							 &dpa_dev_se_handle);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_doca_dpa_completion_sync_event failed (%s)",
			     doca_error_get_descr(doca_err));
		goto destroy_target_rdma_obj;
	}

	DOCA_LOG_INFO("Create Target DOCA MMAP for received buffer on DPA");
	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(uint64_t), &received_buf_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_completion_sync_event;
	}

	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, received_buf_dev_ptr, 0, sizeof(uint64_t));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_received_buf_dev_ptr;
	}

	dpa_mmap_obj.mmap_type = MMAP_TYPE_DPA;
	dpa_mmap_obj.doca_dpa = resources->rdma_dpa_ctx;
	dpa_mmap_obj.doca_device = resources->rdma_doca_device;
	dpa_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
				   DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC;
	dpa_mmap_obj.memrange_addr = (void *)received_buf_dev_ptr;
	dpa_mmap_obj.memrange_len = sizeof(uint64_t);
	doca_err = doca_mmap_obj_init(&dpa_mmap_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_received_buf_dev_ptr;
	}

	DOCA_LOG_INFO("Update Target DPA thread device argument");
	thread_arg.dpa_ctx_handle = resources->rdma_dpa_ctx_handle;
	thread_arg.dpa_comp_handle = target_dpa_completion_obj.handle;
	thread_arg.local_buf_addr = received_buf_dev_ptr;
	thread_arg.sync_event_handle = dpa_dev_se_handle;
	doca_err = doca_dpa_h2d_memcpy(resources->rdma_dpa_ctx,
				       thread_arg_dev_ptr,
				       &thread_arg,
				       sizeof(struct dpa_thread_arg));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_mmap_obj;
	}

	DOCA_LOG_INFO("Create Initiator DOCA MMAP for send buffer on Host");
	host_mmap_obj.mmap_type = MMAP_TYPE_CPU;
	host_mmap_obj.doca_dpa = resources->rdma_dpa_ctx;
	host_mmap_obj.doca_device = resources->rdma_doca_device;
	host_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
				    DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC;
	host_mmap_obj.memrange_addr = (void *)&send_val;
	host_mmap_obj.memrange_len = sizeof(uint64_t);
	doca_err = doca_mmap_obj_init(&host_mmap_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_mmap_obj;
	}

	DOCA_LOG_INFO("Trigger an RPC to prepare Target DPA receive memory");
	doca_err = doca_dpa_rpc(resources->rdma_dpa_ctx,
				&rdma_post_receive_rpc,
				&retval,
				resources->rdma_dpa_ctx_handle,
				target_rdma_obj.dpa_rdma,
				received_buf_dev_ptr,
				dpa_mmap_obj.dpa_mmap_handle,
				dpa_mmap_obj.memrange_len);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_rpc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_host_mmap_obj;
	}

	DOCA_LOG_INFO("Trigger an RPC to post send on Initiator DOCA RDMA with buffer %lu", send_val);
	doca_err = doca_dpa_rpc(resources->rdma_dpa_ctx,
				&rdma_post_send_rpc,
				&retval,
				resources->rdma_dpa_ctx_handle,
				initiator_rdma_obj.dpa_rdma,
				&send_val,
				host_mmap_obj.dpa_mmap_handle,
				host_mmap_obj.memrange_len);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_rpc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_host_mmap_obj;
	}

	DOCA_LOG_INFO("Waiting for Target Thread to set completion Sync Event value to be greater than %lu",
		      (expected_receive_val - 1));
	doca_err = doca_sync_event_wait_gt(comp_event, (expected_receive_val - 1), SYNC_EVENT_MASK_FFS);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_wait_gt failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_host_mmap_obj;
	}

	DOCA_LOG_INFO("Tear down");

destroy_host_mmap_obj:
	tmp_doca_err = doca_mmap_obj_destroy(&host_mmap_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_dpa_mmap_obj:
	tmp_doca_err = doca_mmap_obj_destroy(&dpa_mmap_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_received_buf_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, received_buf_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_completion_sync_event:
	tmp_doca_err = doca_sync_event_destroy(comp_event);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_target_rdma_obj:
	tmp_doca_err = rdma_obj_destroy(&target_rdma_obj, 0);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_objs_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_target_rdma_completion_obj:
	tmp_doca_err = dpa_completion_obj_destroy(&target_dpa_completion_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_target_thread_obj:
	tmp_doca_err = dpa_thread_obj_destroy(&target_thread_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_target_thread_arg_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, thread_arg_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_initiator_rdma_obj:
	tmp_doca_err = rdma_obj_destroy(&initiator_rdma_obj, 1);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_objs_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_initiator_rdma_completion_obj:
	tmp_doca_err = dpa_completion_obj_destroy(&initiator_dpa_completion_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

exit:
	return doca_err;
}
