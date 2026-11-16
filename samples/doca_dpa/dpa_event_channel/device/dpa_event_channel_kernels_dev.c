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

#include <dpaintrin.h>

#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include "../common/dpa_event_channel_common_defs.h"

/**
 * @brief RPC function to trigger thread notification completion
 *
 * This RPC function notifies a thread by triggering a completion object.
 *
 * @comp_handle [in]: Notification completion handle to trigger
 * @return: Always returns 0 indicating successful notification
 */
__dpa_rpc__ uint64_t thread_notify_rpc(doca_dpa_dev_notification_completion_t comp_handle)
{
	DOCA_DPA_DEV_LOG_INFO("Producer: raising a completion on the thread's event channel from RPC\n");
	doca_dpa_dev_thread_notify(comp_handle);
	return 0;
}

/** @brief Report the ID of the completion that woke the thread up, and signal the host that the kernel is done */
__dpa_global__ void event_channel_completion_id_report_kernel(uint64_t arg)
{
	struct dpa_event_channel_completion_info *comp_info = (struct dpa_event_channel_completion_info *)arg;

	int id = doca_dpa_dev_thread_get_completion_id_from_event_channel();
	if (id == -1) {
		DOCA_DPA_DEV_LOG_ERR("%s: Failed to get a completion from the event channel\n", __func__);
		comp_info->comp_se_return_value = DPA_EVENT_CHANNEL_NO_REPORTED_ID;
	} else {
		DOCA_DPA_DEV_LOG_INFO("%s: Event channel reported completion ID %d\n", __func__, id);
		comp_info->comp_se_return_value = (uint64_t)id;
	}

	/* The host reads the reported ID only after it sees the sync event, so the ID must be visible first */
	__dpa_thread_fence(__DPA_SYSTEM, __DPA_W, __DPA_W);
	doca_dpa_dev_sync_event_update_set(comp_info->comp_se_handle, 1);

	doca_dpa_dev_thread_reschedule();
}
