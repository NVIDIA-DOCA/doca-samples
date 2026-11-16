/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include "../common/dpa_event_channel_common_defs.h"

DOCA_LOG_REGISTER(DPA_EVENT_CHANNEL::SAMPLE);

/**
 * kernel/RPC declaration
 */
doca_dpa_func_t event_channel_completion_id_report_kernel;
doca_dpa_func_t thread_notify_rpc;

/** @brief Raise a completion and verify that the thread reported its ID */
doca_error_t notify_and_compare_completion_id(struct dpa_resources *resources,
					      doca_dpa_dev_notification_completion_t completion_handle,
					      uint32_t completion_id,
					      struct doca_sync_event *comp_se,
					      doca_dpa_dev_uintptr_t func_arg)
{
	doca_error_t doca_err = DOCA_SUCCESS;
	uint64_t retval = 0;
	struct dpa_event_channel_completion_info comp_info = {0};

	DOCA_LOG_INFO("Notifying the thread by raising a completion on DPA completion ID %u", completion_id);

	doca_err = doca_dpa_rpc(resources->pf_dpa_ctx, &thread_notify_rpc, &retval, completion_handle);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_rpc failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	/* Wait until the thread has handled the completion and reported the ID it got from the event channel */
	doca_err = doca_sync_event_wait_gt(comp_se, 0, SYNC_EVENT_MASK_FFS);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_wait_gt failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	doca_err = doca_dpa_d2h_memcpy(resources->pf_dpa_ctx,
				       &comp_info,
				       func_arg,
				       sizeof(struct dpa_event_channel_completion_info));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_d2h_memcpy failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	if (comp_info.comp_se_return_value != completion_id) {
		DOCA_LOG_ERR("Thread reported completion ID %lu, expected completion ID %u",
			     comp_info.comp_se_return_value,
			     completion_id);
		return DOCA_ERROR_UNEXPECTED;
	}

	DOCA_LOG_INFO("Thread reported the expected completion ID %u", completion_id);
	return DOCA_SUCCESS;
}

/**
 * @brief Sample's Logic
 *
 * @resources [in]: DPA resources that the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_event_channel(struct dpa_resources *resources)
{
	doca_error_t doca_err = DOCA_SUCCESS, tmp_doca_err = DOCA_SUCCESS;
	const uint64_t queue_size = DPA_EVENT_CHANNEL_COMPLETION_QUEUE_SIZE;
	uint32_t max_completions_supported = 0;
	doca_dpa_dev_uintptr_t func_arg = 0;
	struct dpa_thread_obj dpa_thread_obj = {0};
	struct dpa_completion_obj dpa_comp_1_obj = {0};
	struct dpa_completion_obj dpa_comp_2_obj = {0};
	uint32_t dpa_comp_1_id = 0;
	uint32_t dpa_comp_2_id = 0;
	struct doca_sync_event *dpa_se = NULL;
	doca_dpa_dev_sync_event_t dpa_se_handle = 0;
	struct dpa_event_channel_completion_info comp_info = {0};

	doca_err = doca_dpa_cap_event_channel_is_supported(doca_dev_as_devinfo(resources->pf_doca_device));
	if (doca_err != DOCA_SUCCESS) {
		if (doca_err == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_ERR("Thread event channel is not supported");
		}
		DOCA_LOG_ERR("Function doca_dpa_cap_event_channel_is_supported failed (%s)",
			     doca_error_get_descr(doca_err));
		goto exit;
	}

	doca_err =
		doca_dpa_cap_get_max_num_completions_per_event_channel(doca_dev_as_devinfo(resources->pf_doca_device),
								       &max_completions_supported);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_cap_get_max_num_completions_per_event_channel failed (%s)",
			     doca_error_get_descr(doca_err));
		goto exit;
	}

	if (max_completions_supported < DPA_EVENT_CHANNEL_MAX_COMPLETIONS) {
		DOCA_LOG_ERR("Max completions supported is less than the requested max completions");
		doca_err = DOCA_ERROR_NOT_SUPPORTED;
		goto exit;
	}

	/* Allocate DPA memory for thread argument */
	doca_err =
		doca_dpa_mem_alloc(resources->pf_dpa_ctx, sizeof(struct dpa_event_channel_completion_info), &func_arg);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(doca_err));
		goto exit;
	}

	/* Create thread */
	dpa_thread_obj.doca_dpa = resources->pf_dpa_ctx;
	dpa_thread_obj.func = &event_channel_completion_id_report_kernel;
	dpa_thread_obj.arg = func_arg;
	dpa_thread_obj.event_channel_enabled = true;
	dpa_thread_obj.max_completions = DPA_EVENT_CHANNEL_MAX_COMPLETIONS;
	doca_err = dpa_thread_obj_init(&dpa_thread_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_func_arg;
	}

	/* Start thread */
	doca_err = doca_dpa_thread_start(dpa_thread_obj.thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_start failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_thread;
	}

	/* Create DPA completion #1 and attach to thread */
	dpa_comp_1_obj.doca_dpa = resources->pf_dpa_ctx;
	dpa_comp_1_obj.queue_size = queue_size;
	dpa_comp_1_obj.thread = dpa_thread_obj.thread;
	doca_err = dpa_completion_obj_init(&dpa_comp_1_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_thread;
	}

	doca_err = doca_dpa_completion_get_id(dpa_comp_1_obj.dpa_comp, &dpa_comp_1_id);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_completion_get_id failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_comp_1;
	}

	/* Create DPA completion #2 and attach to thread */
	dpa_comp_2_obj.doca_dpa = resources->pf_dpa_ctx;
	dpa_comp_2_obj.queue_size = queue_size;
	dpa_comp_2_obj.thread = dpa_thread_obj.thread;
	doca_err = dpa_completion_obj_init(&dpa_comp_2_obj);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_comp_1;
	}

	doca_err = doca_dpa_completion_get_id(dpa_comp_2_obj.dpa_comp, &dpa_comp_2_id);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_completion_get_id failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_comp_2;
	}

	/* Create DPA sync event */
	doca_err = create_doca_dpa_kernel_sync_event(resources->pf_dpa_ctx, &dpa_se);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_doca_dpa_kernel_sync_event failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_comp_2;
	}
	doca_err = doca_sync_event_get_dpa_handle(dpa_se, resources->pf_dpa_ctx, &dpa_se_handle);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_get_dpa_handle failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_se;
	}

	/* Run thread */
	doca_err = doca_dpa_thread_run(dpa_thread_obj.thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_run failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_se;
	}

	/* Share the sync event the thread signals when it is done, and arm the reported ID with the sentinel */
	comp_info.comp_se_handle = dpa_se_handle;
	comp_info.comp_se_return_value = DPA_EVENT_CHANNEL_NO_REPORTED_ID;
	doca_err = doca_dpa_h2d_memcpy(resources->pf_dpa_ctx,
				       func_arg,
				       &comp_info,
				       sizeof(struct dpa_event_channel_completion_info));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_se;
	}

	doca_err = notify_and_compare_completion_id(resources, dpa_comp_1_obj.handle, dpa_comp_1_id, dpa_se, func_arg);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to notify and compare completion ID for DPA completion #1");
		goto destroy_dpa_se;
	}

	/* Reset the sync event, so that the host can wait for the report of the next completion */
	doca_err = doca_sync_event_update_set(dpa_se, 0);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_update_set failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_se;
	}

	/* Reset comp_se_return_value to default, so an old ID is not mistaken for a new report */
	doca_err = doca_dpa_h2d_memcpy(resources->pf_dpa_ctx,
				       func_arg,
				       &comp_info,
				       sizeof(struct dpa_event_channel_completion_info));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_h2d_memcpy failed (%s)", doca_error_get_descr(doca_err));
		goto destroy_dpa_se;
	}

	doca_err = notify_and_compare_completion_id(resources, dpa_comp_2_obj.handle, dpa_comp_2_id, dpa_se, func_arg);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to notify and compare completion ID for DPA completion #2");
		goto destroy_dpa_se;
	}

	DOCA_LOG_INFO("Tear down");

destroy_dpa_se:
	tmp_doca_err = doca_sync_event_destroy(dpa_se);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_sync_event_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_dpa_comp_2:
	tmp_doca_err = dpa_completion_obj_destroy(&dpa_comp_2_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_dpa_comp_1:
	tmp_doca_err = dpa_completion_obj_destroy(&dpa_comp_1_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_thread:
	tmp_doca_err = dpa_thread_obj_destroy(&dpa_thread_obj);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_thread_obj_destroy failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

destroy_func_arg:
	tmp_doca_err = doca_dpa_mem_free(resources->pf_dpa_ctx, func_arg);
	if (tmp_doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_free failed: %s", doca_error_get_descr(tmp_doca_err));
		DOCA_ERROR_PROPAGATE(doca_err, tmp_doca_err);
	}

exit:
	return doca_err;
}
