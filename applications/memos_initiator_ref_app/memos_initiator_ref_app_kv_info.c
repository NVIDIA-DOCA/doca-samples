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

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_kv.h"
#include "memos_initiator_ref_app_kv_info.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_memos_initiator.h>
#include <doca_memos_initiator_dev.h>
#include <doca_memos_initiator_dev_io.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_KV_INFO);

#define KV_INFO_POLL_SLEEP_NS (10 * 1000) /* 10 us */

/* Unbounded: both statistics events have handlers, so one always arrives. */
static void kv_info_poll_until(memos_initiator_ref_app_kv_info_progress_fn progress,
			       void *progress_ctx,
			       const bool *done)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = KV_INFO_POLL_SLEEP_NS};

	while (!*done) {
		if (progress(progress_ctx) == 0) {
			struct timespec ts = sleep_ts;

			(void)nanosleep(&ts, NULL);
		}
	}
}

const char *memos_initiator_ref_app_kv_info_target_state_str(enum doca_memos_initiator_dev_target_state state)
{
	switch (state) {
	case DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECTED:
		return "disconnected";
	case DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECT_IN_PROGRESS:
		return "disconnect_in_progress";
	case DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_CONNECT_IN_PROGRESS:
		return "connect_in_progress";
	case DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_CONNECTED:
		return "connected";
	default:
		return "unknown";
	}
}

static const char *kv_stats_task_type_str(enum doca_memos_initiator_task_type t)
{
	switch (t) {
	case DOCA_MEMOS_INITIATOR_TASK_TYPE_STORE:
		return "store";
	case DOCA_MEMOS_INITIATOR_TASK_TYPE_RETRIEVE:
		return "retrieve";
	case DOCA_MEMOS_INITIATOR_TASK_TYPE_EXIST:
		return "exist";
	default:
		return "unknown";
	}
}

/*****************************************************************************
 * SDK introspection - exercised to document the API surface; these only log.
 *****************************************************************************/

void memos_initiator_ref_app_kv_info_log_device_caps(struct doca_dev *dev)
{
	struct doca_devinfo *devinfo = doca_dev_as_devinfo(dev);
	uint8_t supported = 0;

	if (doca_memos_initiator_cap_is_supported(devinfo, &supported) == DOCA_SUCCESS)
		DOCA_LOG_INFO("Device cap: memos initiator supported=%u", supported);
	if (doca_memos_initiator_cap_is_trtype_supported(devinfo, MEMOS_INITIATOR_REF_APP_TRTYPE_RDMA, &supported) ==
	    DOCA_SUCCESS)
		DOCA_LOG_INFO("Device cap: trtype RDMA supported=%u", supported);
	if (doca_memos_initiator_cap_is_adrfam_supported(devinfo, MEMOS_INITIATOR_REF_APP_ADRFAM_IPV4, &supported) ==
	    DOCA_SUCCESS)
		DOCA_LOG_INFO("Device cap: adrfam IPv4 supported=%u", supported);
	if (doca_memos_initiator_cap_is_volume_ek_size_supported(devinfo,
								 DOCA_MEMOS_INITIATOR_VOLUME_EK_SIZE_256,
								 &supported) == DOCA_SUCCESS)
		DOCA_LOG_INFO("Device cap: volume EK 256-bit supported=%u", supported);
}

void memos_initiator_ref_app_kv_info_log_stats_caps(void)
{
	enum doca_memos_initiator_task_type type;
	uint8_t supported;

	for (type = DOCA_MEMOS_INITIATOR_TASK_TYPE_STORE; type <= DOCA_MEMOS_INITIATOR_TASK_TYPE_EXIST; ++type) {
		uint8_t inflights_mean = 0, inflights_stdev = 0, completed = 0, value_bytes = 0;

		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_inflights_mean_supported(type, &supported) ==
		    DOCA_SUCCESS)
			inflights_mean = supported;
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_inflights_stdev_supported(type, &supported) ==
		    DOCA_SUCCESS)
			inflights_stdev = supported;
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_completed_with_status_supported(type,
													DOCA_SUCCESS,
													&supported) ==
		    DOCA_SUCCESS)
			completed = supported;
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_completed_value_bytes_supported(type,
													&supported) ==
		    DOCA_SUCCESS)
			value_bytes = supported;

		DOCA_LOG_INFO("Stats cap [%s]: inflights_mean=%u inflights_stdev=%u completed=%u value_bytes=%u",
			      kv_stats_task_type_str(type),
			      inflights_mean,
			      inflights_stdev,
			      completed,
			      value_bytes);
	}
}

void memos_initiator_ref_app_kv_info_log_sdk_view(struct doca_memos_initiator_dev *dev,
						  struct doca_memos_initiator_dev_io *io,
						  struct doca_memos_initiator_dev_target *target,
						  struct doca_memos_initiator_dev_target_ns *ns,
						  struct doca_memos_initiator_dev_volume *volume)
{
	char traddr[MEMOS_INITIATOR_REF_APP_TRADDR_SIZE] = {0};
	char trsvcid[MEMOS_INITIATOR_REF_APP_TRSVCID_SIZE] = {0};
	char subnqn[MEMOS_INITIATOR_REF_APP_NQN_SIZE] = {0};
	char hostnqn[MEMOS_INITIATOR_REF_APP_NQN_SIZE] = {0};
	struct doca_memos_initiator_dev_ns_info **ns_info_list = NULL;
	struct doca_memos_initiator_dev_target *ns_target = NULL;
	struct doca_memos_initiator_dev_volume *ns_volume = NULL;
	struct doca_memos_initiator_dev *target_dev = NULL;
	uint8_t nguid[MEMOS_INITIATOR_REF_APP_KV_NGUID_LEN] = {0};
	uint32_t num_io = 0, num_tasks = 0, nb_ns = 0, max_attached = 0, reconnect = 0, nsid = 0;
	uint32_t vol_max_value = 0, ns_max_value = 0;
	uint16_t vol_max_key = 0, ns_max_key = 0;
	uint64_t ns_size = 0, tenant_id = 0;
	uint8_t trtype = 0, adrfam = 0, attached = 0;
	enum doca_memos_initiator_dev_target_state state;

	(void)doca_memos_initiator_dev_get_num_io_ctxs(dev, &num_io);
	(void)doca_memos_initiator_dev_io_get_num_tasks(io, &num_tasks);
	DOCA_LOG_INFO("SDK device: io_ctxs=%u tasks_per_io=%u io_belongs_to_dev=%u",
		      num_io,
		      num_tasks,
		      doca_memos_initiator_dev_io_get_dev(io) == dev);

	(void)doca_memos_initiator_dev_target_get_trtype(target, &trtype);
	(void)doca_memos_initiator_dev_target_get_adrfam(target, &adrfam);
	(void)doca_memos_initiator_dev_target_get_traddr(target, traddr, sizeof(traddr));
	(void)doca_memos_initiator_dev_target_get_trsvcid(target, trsvcid, sizeof(trsvcid));
	(void)doca_memos_initiator_dev_target_get_subnqn(target, subnqn, sizeof(subnqn));
	(void)doca_memos_initiator_dev_target_get_hostnqn(target, hostnqn, sizeof(hostnqn));
	(void)doca_memos_initiator_dev_target_get_reconnect_delay(target, &reconnect);
	(void)doca_memos_initiator_dev_target_get_dev(target, &target_dev);
	if (doca_memos_initiator_dev_target_get_state(target, &state) != DOCA_SUCCESS)
		state = DOCA_MEMOS_INITIATOR_DEV_TARGET_STATE_DISCONNECTED;
	DOCA_LOG_INFO("SDK target: trtype=%u adrfam=%u %s:%s state=%s reconnect_delay=%us dev_matches=%u",
		      trtype,
		      adrfam,
		      traddr,
		      trsvcid,
		      memos_initiator_ref_app_kv_info_target_state_str(state),
		      reconnect,
		      target_dev == dev);
	DOCA_LOG_INFO("SDK target: subnqn=%s hostnqn=%s", subnqn, hostnqn);

	(void)doca_memos_initiator_dev_target_ns_get_nsid(ns, &nsid);
	(void)doca_memos_initiator_dev_target_ns_get_size(ns, &ns_size);
	(void)doca_memos_initiator_dev_target_ns_get_max_key_len(ns, &ns_max_key);
	(void)doca_memos_initiator_dev_target_ns_get_max_value_len(ns, &ns_max_value);
	(void)doca_memos_initiator_dev_target_ns_get_nguid(ns, nguid, sizeof(nguid));
	(void)doca_memos_initiator_dev_target_ns_is_attached(ns, &attached);
	(void)doca_memos_initiator_dev_target_ns_get_target(ns, &ns_target);
	(void)doca_memos_initiator_dev_target_ns_get_volume(ns, &ns_volume);
	DOCA_LOG_INFO("SDK target ns: nsid=%u size=%" PRIu64
		      " max_key=%u max_value=%u attached=%u target_matches=%u volume_matches=%u",
		      nsid,
		      ns_size,
		      ns_max_key,
		      ns_max_value,
		      attached,
		      ns_target == target,
		      ns_volume == volume);

	(void)doca_memos_initiator_dev_volume_get_max_key_len(volume, &vol_max_key);
	(void)doca_memos_initiator_dev_volume_get_max_value_len(volume, &vol_max_value);
	(void)doca_memos_initiator_dev_volume_get_max_attached_ns(volume, &max_attached);
	(void)doca_memos_initiator_dev_volume_get_tenant_id(volume, &tenant_id);
	{
		char key[64] = {0};
		enum doca_memos_initiator_volume_ek_size dek_size = DOCA_MEMOS_INITIATOR_VOLUME_EK_SIZE_NONE;
		enum doca_memos_initiator_volume_ek_size tek_size = DOCA_MEMOS_INITIATOR_VOLUME_EK_SIZE_NONE;

		(void)doca_memos_initiator_dev_volume_get_dek(volume, key, sizeof(key), &dek_size);
		(void)doca_memos_initiator_dev_volume_get_tek(volume, key, sizeof(key), &tek_size);
		DOCA_LOG_INFO("SDK volume: dek_size=%d tek_size=%d (0 = unencrypted)", (int)dek_size, (int)tek_size);
	}

	DOCA_LOG_INFO("SDK volume: max_key=%u max_value=%u max_attached_ns=%u tenant_id=%" PRIu64,
		      vol_max_key,
		      vol_max_value,
		      max_attached,
		      tenant_id);

	/* The device-wide list; bring-up uses the per-target one. Identical with a single target. */
	if (doca_memos_initiator_dev_create_ns_info_list(dev, &ns_info_list, &nb_ns) == DOCA_SUCCESS) {
		if (ns_info_list != NULL && nb_ns > 0) {
			struct doca_memos_initiator_dev_target *info_target = NULL;

			(void)doca_memos_initiator_dev_ns_info_get_target(ns_info_list[0], &info_target);
			DOCA_LOG_INFO("SDK device ns list: %u namespace(s), first belongs to our target=%u",
				      nb_ns,
				      info_target == target);
		}
		(void)doca_memos_initiator_dev_destroy_ns_info_list(ns_info_list);
	}
}

void memos_initiator_ref_app_kv_info_collect_stats_cb(struct doca_memos_initiator_dev_volume *volume,
						      union doca_data reg_user_data,
						      union doca_data op_user_data,
						      doca_error_t status)
{
	struct memos_initiator_ref_app_kv_info_stats *st =
		(struct memos_initiator_ref_app_kv_info_stats *)reg_user_data.ptr;
	enum doca_memos_initiator_task_type type;

	(void)op_user_data;

	if (status != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Volume statistics collect failed: %s", doca_error_get_name(status));
		st->done = true;
		return;
	}

	DOCA_LOG_INFO("Volume statistics (volume_matches=%u):", volume == st->volume);

	for (type = DOCA_MEMOS_INITIATOR_TASK_TYPE_STORE; type <= DOCA_MEMOS_INITIATOR_TASK_TYPE_EXIST; ++type) {
		uint64_t mean = 0, stdev = 0, completed = 0, value_bytes = 0;

		uint8_t supported = 0;

		/* Not every counter exists for every task type - an Exist task moves no value bytes, and
		 * reading that counter for it is rejected. Ask before reading. */
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_inflights_mean_supported(type, &supported) ==
			    DOCA_SUCCESS &&
		    supported)
			(void)doca_memos_initiator_dev_volume_get_stats_ns_num_inflights_mean(volume,
											      st->ns,
											      type,
											      &mean);
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_inflights_stdev_supported(type, &supported) ==
			    DOCA_SUCCESS &&
		    supported)
			(void)doca_memos_initiator_dev_volume_get_stats_ns_num_inflights_stdev(volume,
											       st->ns,
											       type,
											       &stdev);
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_completed_with_status_supported(type,
													DOCA_SUCCESS,
													&supported) ==
			    DOCA_SUCCESS &&
		    supported)
			(void)doca_memos_initiator_dev_volume_get_stats_ns_num_completed_with_status(volume,
												     st->ns,
												     type,
												     DOCA_SUCCESS,
												     &completed);
		if (doca_memos_initiator_cap_is_volume_get_stats_ns_num_completed_value_bytes_supported(type,
													&supported) ==
			    DOCA_SUCCESS &&
		    supported)
			(void)doca_memos_initiator_dev_volume_get_stats_ns_num_completed_value_bytes(volume,
												     st->ns,
												     type,
												     &value_bytes);
		DOCA_LOG_INFO("  %-8s completed_ok=%" PRIu64 " value_bytes=%" PRIu64 " inflight_mean=%" PRIu64
			      " inflight_stdev=%" PRIu64,
			      kv_stats_task_type_str(type),
			      completed,
			      value_bytes,
			      mean,
			      stdev);
	}

	st->done = true;
}

void memos_initiator_ref_app_kv_info_stats_completion_cb(struct doca_memos_initiator_dev_volume *volume,
							 union doca_data reg_user_data,
							 union doca_data op_user_data,
							 doca_error_t status)
{
	struct memos_initiator_ref_app_kv_info_stats *st =
		(struct memos_initiator_ref_app_kv_info_stats *)reg_user_data.ptr;

	(void)volume;
	(void)op_user_data;
	if (status != DOCA_SUCCESS)
		DOCA_LOG_WARN("Volume statistics operation failed: %s", doca_error_get_name(status));
	st->done = true;
}

void memos_initiator_ref_app_kv_info_report_stats(memos_initiator_ref_app_kv_info_progress_fn progress,
						  void *progress_ctx,
						  struct memos_initiator_ref_app_kv_info_stats *stats,
						  struct doca_memos_initiator_dev_volume *volume,
						  struct doca_memos_initiator_dev_target_ns *ns)
{
	union doca_data op_user_data = {0};

	if (volume == NULL || ns == NULL)
		return;

	stats->volume = volume;
	stats->ns = ns;

	stats->done = false;
	if (doca_memos_initiator_dev_volume_collect_stats(volume, op_user_data) == DOCA_SUCCESS)
		kv_info_poll_until(progress, progress_ctx, &stats->done);

	stats->done = false;
	if (doca_memos_initiator_dev_volume_reset_stats(volume, op_user_data) == DOCA_SUCCESS)
		kv_info_poll_until(progress, progress_ctx, &stats->done);
}
