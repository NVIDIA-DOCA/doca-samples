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
#include "../common/dpa_ping_pong_common_defs.h"

DOCA_LOG_REGISTER(DPA_PING_PONG::SAMPLE);

/**
 * kernel/RPC declaration
 */
doca_dpa_func_t thread_kernel;
doca_dpa_func_t trigger_first_iteration_rpc;

/**
 * @brief Create, attach, start and connect DPA RDMA objects
 *
 * This function creates DPA RDMA objects, attaches to DPA completion, start and connect them
 *
 * @resources [in]: DPA resources
 * @ping_dpa_completion_obj [in]: Ping DPA completion
 * @pong_dpa_completion_obj [in]: Pong DPA completion
 * @ping_dpa_rdma_obj [out]: Created ping DPA RDMA
 * @pong_dpa_rdma_obj [out]: Created pong DPA RDMA
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_objs_init(struct dpa_resources *resources,
				   struct dpa_completion_obj *ping_dpa_completion_obj,
				   struct dpa_completion_obj *pong_dpa_completion_obj,
				   struct dpa_rdma_obj *ping_dpa_rdma_obj,
				   struct dpa_rdma_obj *pong_dpa_rdma_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;

	ping_dpa_rdma_obj->doca_device = resources->rdma_doca_device;
	ping_dpa_rdma_obj->doca_dpa = resources->rdma_dpa_ctx;
	ping_dpa_rdma_obj->permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					 DOCA_ACCESS_FLAG_RDMA_READ;
	ping_dpa_rdma_obj->buf_list_len = RDMA_DEFAULT_BUF_LIST_LEN;
	ping_dpa_rdma_obj->gid_index = resources->gid_index;
	ping_dpa_rdma_obj->is_gid_index_set = resources->is_gid_index_set;
	ping_dpa_rdma_obj->recv_queue_size = 1;
	ping_dpa_rdma_obj->dpa_comp = ping_dpa_completion_obj->dpa_comp;
	doca_err = dpa_rdma_obj_init(ping_dpa_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto exit;
	}

	pong_dpa_rdma_obj->doca_device = resources->rdma_doca_device;
	pong_dpa_rdma_obj->doca_dpa = resources->rdma_dpa_ctx;
	pong_dpa_rdma_obj->buf_list_len = RDMA_DEFAULT_BUF_LIST_LEN;
	pong_dpa_rdma_obj->gid_index = resources->gid_index;
	pong_dpa_rdma_obj->is_gid_index_set = resources->is_gid_index_set;
	pong_dpa_rdma_obj->recv_queue_size = 1;
	pong_dpa_rdma_obj->permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					 DOCA_ACCESS_FLAG_RDMA_READ;
	pong_dpa_rdma_obj->dpa_comp = pong_dpa_completion_obj->dpa_comp;
	doca_err = dpa_rdma_obj_init(pong_dpa_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_rdma_obj;
	}

	doca_err = dpa_rdma_obj_start(ping_dpa_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_start failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_rdma_obj;
	}

	doca_err = dpa_rdma_obj_start(pong_dpa_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_start failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_rdma_obj;
	}

	doca_err = doca_rdma_connect(ping_dpa_rdma_obj->rdma,
				     pong_dpa_rdma_obj->connection_details,
				     pong_dpa_rdma_obj->conn_det_len,
				     ping_dpa_rdma_obj->connection);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_connect failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_rdma_obj;
	}

	doca_err = doca_rdma_connect(pong_dpa_rdma_obj->rdma,
				     ping_dpa_rdma_obj->connection_details,
				     ping_dpa_rdma_obj->conn_det_len,
				     pong_dpa_rdma_obj->connection);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_connect failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_rdma_obj;
	}

	return doca_err;

destroy_pong_rdma_obj:
	tmp_doca_err = dpa_rdma_obj_destroy(pong_dpa_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_rdma_obj:
	tmp_doca_err = dpa_rdma_obj_destroy(ping_dpa_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

exit:
	return doca_err;
}

/**
 * @brief Destroy DPA RDMA objects
 *
 * @ping_dpa_rdma_obj [in]: Previously created ping DPA RDMA
 * @pong_dpa_rdma_obj [in]: Previously created pong DPA RDMA
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_objs_destroy(struct dpa_rdma_obj *ping_dpa_rdma_obj, struct dpa_rdma_obj *pong_dpa_rdma_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;

	tmp_doca_err = dpa_rdma_obj_destroy(ping_dpa_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

	tmp_doca_err = dpa_rdma_obj_destroy(pong_dpa_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_rdma_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

	return doca_err;
}

/**
 * @brief Sample's Logic
 *
 * @resources [in]: DPA resources that the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_ping_pong(struct dpa_resources *resources)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;
	doca_dpa_dev_uintptr_t ping_thread_arg_dev_ptr = 0;
	doca_dpa_dev_uintptr_t ping_tls_dev_ptr = 0;
	doca_dpa_dev_uintptr_t ping_recv_arr_dev_ptr = 0;
	struct dpa_ping_pong_tls ping_tls = {0};
	doca_dpa_dev_uintptr_t pong_thread_arg_dev_ptr = 0;
	doca_dpa_dev_uintptr_t pong_tls_dev_ptr = 0;
	doca_dpa_dev_uintptr_t pong_recv_arr_dev_ptr = 0;
	struct dpa_ping_pong_tls pong_tls = {0};
	struct dpa_thread_obj ping_thread_obj = {0};
	struct dpa_thread_obj pong_thread_obj = {0};
	struct dpa_completion_obj ping_dpa_completion_obj = {0};
	struct dpa_completion_obj pong_dpa_completion_obj = {0};
	struct dpa_rdma_obj ping_rdma_obj = {0};
	struct dpa_rdma_obj pong_rdma_obj = {0};
	struct doca_sync_event *ping_comp_se = NULL;
	doca_dpa_dev_sync_event_t ping_comp_se_handle = 0;
	struct doca_sync_event *pong_comp_se = NULL;
	doca_dpa_dev_sync_event_t pong_comp_se_handle = 0;
	const uint64_t wait_sync_ev_threshold = 9;
	doca_dpa_dev_uintptr_t ping_receive_buf_dev_ptr = 0;
	struct doca_mmap_obj ping_receive_mmap_obj = {0};
	doca_dpa_dev_uintptr_t pong_receive_buf_dev_ptr = 0;
	struct doca_mmap_obj pong_receive_mmap_obj = {0};
	doca_dpa_dev_uintptr_t ping_send_buf_dev_ptr = 0;
	struct doca_mmap_obj ping_send_mmap_obj = {0};
	doca_dpa_dev_uintptr_t pong_send_buf_dev_ptr = 0;
	struct doca_mmap_obj pong_send_mmap_obj = {0};
	struct dpa_thread_arg ping_thread_arg = {0};
	struct dpa_thread_arg pong_thread_arg = {0};
	uint64_t retval = 0;

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(struct dpa_thread_arg), &ping_thread_arg_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto exit;
	}
	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, ping_thread_arg_dev_ptr, 0, sizeof(struct dpa_thread_arg));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_thread_arg_dev_ptr;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(struct dpa_ping_pong_tls), &ping_tls_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_thread_arg_dev_ptr;
	}
	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx,
				      sizeof(uint32_t) * EXPECTED_NUM_RECEIVES,
				      &ping_recv_arr_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_tls_dev_ptr;
	}
	ping_tls.received_values_arr_ptr = ping_recv_arr_dev_ptr;
	ping_tls.num_receives = 0;
	ping_tls.is_ping_thread = 1;
	doca_err = doca_dpa_h2d_memcpy(resources->rdma_dpa_ctx,
				       ping_tls_dev_ptr,
				       &ping_tls,
				       sizeof(struct dpa_ping_pong_tls));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_recv_arr_dev_ptr;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(struct dpa_thread_arg), &pong_thread_arg_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_recv_arr_dev_ptr;
	}
	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, pong_thread_arg_dev_ptr, 0, sizeof(struct dpa_thread_arg));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_thread_arg_dev_ptr;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(struct dpa_ping_pong_tls), &pong_tls_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_thread_arg_dev_ptr;
	}
	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx,
				      sizeof(uint32_t) * EXPECTED_NUM_RECEIVES,
				      &pong_recv_arr_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_tls_dev_ptr;
	}
	pong_tls.received_values_arr_ptr = pong_recv_arr_dev_ptr;
	pong_tls.num_receives = 0;
	pong_tls.is_ping_thread = 0;
	doca_err = doca_dpa_h2d_memcpy(resources->rdma_dpa_ctx,
				       pong_tls_dev_ptr,
				       &pong_tls,
				       sizeof(struct dpa_ping_pong_tls));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_recv_arr_dev_ptr;
	}

	ping_thread_obj.doca_dpa = resources->rdma_dpa_ctx;
	ping_thread_obj.func = &thread_kernel;
	ping_thread_obj.arg = ping_thread_arg_dev_ptr;
	ping_thread_obj.tls_dev_ptr = ping_tls_dev_ptr;
	doca_err = dpa_thread_obj_init(&ping_thread_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_recv_arr_dev_ptr;
	}
	pong_thread_obj.doca_dpa = resources->rdma_dpa_ctx;
	pong_thread_obj.func = &thread_kernel;
	pong_thread_obj.arg = pong_thread_arg_dev_ptr;
	pong_thread_obj.tls_dev_ptr = pong_tls_dev_ptr;
	doca_err = dpa_thread_obj_init(&pong_thread_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_thread_obj;
	}

	ping_dpa_completion_obj.doca_dpa = resources->rdma_dpa_ctx;
	ping_dpa_completion_obj.queue_size = 2;
	ping_dpa_completion_obj.thread = ping_thread_obj.thread;
	doca_err = dpa_completion_obj_init(&ping_dpa_completion_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_thread_obj;
	}
	pong_dpa_completion_obj.doca_dpa = resources->rdma_dpa_ctx;
	pong_dpa_completion_obj.queue_size = 2;
	pong_dpa_completion_obj.thread = pong_thread_obj.thread;
	doca_err = dpa_completion_obj_init(&pong_dpa_completion_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_dpa_completion_obj;
	}

	doca_err = rdma_objs_init(resources,
				  &ping_dpa_completion_obj,
				  &pong_dpa_completion_obj,
				  &ping_rdma_obj,
				  &pong_rdma_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_objs_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_dpa_completion_obj;
	}

	doca_err = doca_dpa_thread_run(ping_thread_obj.thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_run failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_rdma_objs;
	}
	doca_err = doca_dpa_thread_run(pong_thread_obj.thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_run failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_rdma_objs;
	}

	doca_err = create_doca_dpa_completion_sync_event(resources->pf_dpa_ctx,
							 resources->pf_doca_device,
							 &ping_comp_se,
							 &ping_comp_se_handle);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_doca_dpa_completion_sync_event failed (%s)",
			     doca_error_get_descr(doca_err));
		goto destroy_rdma_objs;
	}
	doca_err = create_doca_dpa_completion_sync_event(resources->pf_dpa_ctx,
							 resources->pf_doca_device,
							 &pong_comp_se,
							 &pong_comp_se_handle);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_doca_dpa_completion_sync_event failed (%s)",
			     doca_error_get_descr(doca_err));
		goto destroy_ping_completion_sync_event;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(uint64_t), &ping_receive_buf_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_completion_sync_event;
	}

	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, ping_receive_buf_dev_ptr, 0, sizeof(uint64_t));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_receive_buf_dev_ptr;
	}

	ping_receive_mmap_obj.mmap_type = MMAP_TYPE_DPA;
	ping_receive_mmap_obj.doca_dpa = resources->rdma_dpa_ctx;
	ping_receive_mmap_obj.doca_device = resources->rdma_doca_device;
	ping_receive_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					    DOCA_ACCESS_FLAG_RDMA_READ;
	ping_receive_mmap_obj.memrange_addr = (void *)ping_receive_buf_dev_ptr;
	ping_receive_mmap_obj.memrange_len = sizeof(uint64_t);
	doca_err = doca_mmap_obj_init(&ping_receive_mmap_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_receive_buf_dev_ptr;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(uint64_t), &pong_receive_buf_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_receive_mmap_obj;
	}

	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, pong_receive_buf_dev_ptr, 0, sizeof(uint64_t));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_receive_buf_dev_ptr;
	}

	pong_receive_mmap_obj.mmap_type = MMAP_TYPE_DPA;
	pong_receive_mmap_obj.doca_dpa = resources->rdma_dpa_ctx;
	pong_receive_mmap_obj.doca_device = resources->rdma_doca_device;
	pong_receive_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					    DOCA_ACCESS_FLAG_RDMA_READ;
	pong_receive_mmap_obj.memrange_addr = (void *)pong_receive_buf_dev_ptr;
	pong_receive_mmap_obj.memrange_len = sizeof(uint64_t);
	doca_err = doca_mmap_obj_init(&pong_receive_mmap_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_receive_buf_dev_ptr;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(uint64_t), &ping_send_buf_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_receive_mmap_obj;
	}

	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, ping_send_buf_dev_ptr, 0, sizeof(uint64_t));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_send_buf_dev_ptr;
	}

	ping_send_mmap_obj.mmap_type = MMAP_TYPE_DPA;
	ping_send_mmap_obj.doca_dpa = resources->rdma_dpa_ctx;
	ping_send_mmap_obj.doca_device = resources->rdma_doca_device;
	ping_send_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					 DOCA_ACCESS_FLAG_RDMA_READ;
	ping_send_mmap_obj.memrange_addr = (void *)ping_send_buf_dev_ptr;
	ping_send_mmap_obj.memrange_len = sizeof(uint64_t);
	doca_err = doca_mmap_obj_init(&ping_send_mmap_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_send_buf_dev_ptr;
	}

	doca_err = doca_dpa_mem_alloc(resources->rdma_dpa_ctx, sizeof(uint64_t), &pong_send_buf_dev_ptr);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_ping_send_mmap_obj;
	}

	doca_err = doca_dpa_memset(resources->rdma_dpa_ctx, pong_send_buf_dev_ptr, 0, sizeof(uint64_t));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_buf_dev_ptr;
	}

	pong_send_mmap_obj.mmap_type = MMAP_TYPE_DPA;
	pong_send_mmap_obj.doca_dpa = resources->rdma_dpa_ctx;
	pong_send_mmap_obj.doca_device = resources->rdma_doca_device;
	pong_send_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					 DOCA_ACCESS_FLAG_RDMA_READ;
	pong_send_mmap_obj.memrange_addr = (void *)pong_send_buf_dev_ptr;
	pong_send_mmap_obj.memrange_len = sizeof(uint64_t);
	doca_err = doca_mmap_obj_init(&pong_send_mmap_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_buf_dev_ptr;
	}

	ping_thread_arg.dpa_ctx_handle = resources->rdma_dpa_ctx_handle;
	ping_thread_arg.dpa_comp_handle = ping_dpa_completion_obj.handle;
	ping_thread_arg.rdma_handle = (doca_dpa_dev_uintptr_t)ping_rdma_obj.dpa_rdma;
	ping_thread_arg.recv_addr = ping_receive_buf_dev_ptr;
	ping_thread_arg.recv_addr_mmap_handle = ping_receive_mmap_obj.dpa_mmap_handle;
	ping_thread_arg.recv_addr_length = ping_receive_mmap_obj.memrange_len;
	ping_thread_arg.send_addr = ping_send_buf_dev_ptr;
	ping_thread_arg.send_addr_mmap_handle = ping_send_mmap_obj.dpa_mmap_handle;
	ping_thread_arg.send_addr_length = ping_send_mmap_obj.memrange_len;
	ping_thread_arg.comp_sync_event_handle = ping_comp_se_handle;
	ping_thread_arg.comp_sync_event_val = wait_sync_ev_threshold + 1;
	doca_err = doca_dpa_h2d_memcpy(resources->rdma_dpa_ctx,
				       ping_thread_arg_dev_ptr,
				       &ping_thread_arg,
				       sizeof(struct dpa_thread_arg));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_mmap_obj;
	}

	pong_thread_arg.dpa_ctx_handle = resources->rdma_dpa_ctx_handle;
	pong_thread_arg.dpa_comp_handle = pong_dpa_completion_obj.handle;
	pong_thread_arg.rdma_handle = (doca_dpa_dev_uintptr_t)pong_rdma_obj.dpa_rdma;
	pong_thread_arg.recv_addr = pong_receive_buf_dev_ptr;
	pong_thread_arg.recv_addr_mmap_handle = pong_receive_mmap_obj.dpa_mmap_handle;
	pong_thread_arg.recv_addr_length = pong_receive_mmap_obj.memrange_len;
	pong_thread_arg.send_addr = pong_send_buf_dev_ptr;
	pong_thread_arg.send_addr_mmap_handle = pong_send_mmap_obj.dpa_mmap_handle;
	pong_thread_arg.send_addr_length = pong_send_mmap_obj.memrange_len;
	pong_thread_arg.comp_sync_event_handle = pong_comp_se_handle;
	pong_thread_arg.comp_sync_event_val = wait_sync_ev_threshold + 1;
	doca_err = doca_dpa_h2d_memcpy(resources->rdma_dpa_ctx,
				       pong_thread_arg_dev_ptr,
				       &pong_thread_arg,
				       sizeof(struct dpa_thread_arg));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_mmap_obj;
	}

	doca_err = doca_dpa_rpc(resources->rdma_dpa_ctx,
				&trigger_first_iteration_rpc,
				&retval,
				resources->rdma_dpa_ctx_handle,
				ping_thread_arg,
				pong_thread_arg);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_rpc failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_mmap_obj;
	}

	doca_err = doca_sync_event_wait_gt(ping_comp_se, wait_sync_ev_threshold, SYNC_EVENT_MASK_FFS);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_wait_gt failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_mmap_obj;
	}

	doca_err = doca_sync_event_wait_gt(pong_comp_se, wait_sync_ev_threshold, SYNC_EVENT_MASK_FFS);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_wait_gt failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_pong_send_mmap_obj;
	}

destroy_pong_send_mmap_obj:
	tmp_doca_err = doca_mmap_obj_destroy(&pong_send_mmap_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_send_buf_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, pong_send_buf_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_send_mmap_obj:
	tmp_doca_err = doca_mmap_obj_destroy(&ping_send_mmap_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_send_buf_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, ping_send_buf_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_receive_mmap_obj:
	tmp_doca_err = doca_mmap_obj_destroy(&pong_receive_mmap_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_receive_buf_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, pong_receive_buf_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_receive_mmap_obj:
	tmp_doca_err = doca_mmap_obj_destroy(&ping_receive_mmap_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_receive_buf_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, ping_receive_buf_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_completion_sync_event:
	tmp_doca_err = doca_sync_event_destroy(pong_comp_se);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_completion_sync_event:
	tmp_doca_err = doca_sync_event_destroy(ping_comp_se);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_rdma_objs:
	tmp_doca_err = rdma_objs_destroy(&ping_rdma_obj, &pong_rdma_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function rdma_objs_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_dpa_completion_obj:
	tmp_doca_err = dpa_completion_obj_destroy(&pong_dpa_completion_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_dpa_completion_obj:
	tmp_doca_err = dpa_completion_obj_destroy(&ping_dpa_completion_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_thread_obj:
	tmp_doca_err = dpa_thread_obj_destroy(&pong_thread_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_thread_obj:
	tmp_doca_err = dpa_thread_obj_destroy(&ping_thread_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_recv_arr_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, pong_recv_arr_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_tls_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, pong_tls_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_pong_thread_arg_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, pong_thread_arg_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_recv_arr_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, ping_recv_arr_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_tls_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, ping_tls_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_ping_thread_arg_dev_ptr:
	tmp_doca_err = doca_dpa_mem_free(resources->rdma_dpa_ctx, ping_thread_arg_dev_ptr);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

exit:
	return doca_err;
}
