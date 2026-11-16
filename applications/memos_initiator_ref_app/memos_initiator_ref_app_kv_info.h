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

/*
 * Reporting for the DOCA Memos Initiator: capability queries, configuration read-back, object
 * introspection and volume statistics.
 *
 * Separate from the KV backend because none of it is on the data path - everything here only reads
 * and logs. It exists because a reference application is read as documentation of an API, so the
 * parts of the surface the backend does not act on are still worth exercising.
 */

#ifndef MEMOS_INITIATOR_REF_APP_KV_INFO_H_
#define MEMOS_INITIATOR_REF_APP_KV_INFO_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>
#include <doca_memos_initiator_dev.h>
#include <doca_types.h>

struct doca_dev;
struct doca_pe;
struct doca_memos_initiator_dev;
struct doca_memos_initiator_dev_io;
struct doca_memos_initiator_dev_target;
struct doca_memos_initiator_dev_target_ns;
struct doca_memos_initiator_dev_volume;

/**
 * @brief Hand-off between the statistics events and the caller awaiting them.
 *
 * Collecting statistics is asynchronous. The caller owns one of these, passes it as the events'
 * registration user data, and memos_initiator_ref_app_kv_info_report_stats() drives it.
 */
struct memos_initiator_ref_app_kv_info_stats {
	bool done;
	/** The namespace to report counters for; set before the collect. */
	const struct doca_memos_initiator_dev_target_ns *ns;
	/** The volume the counters are expected to belong to, for cross-checking. */
	const struct doca_memos_initiator_dev_volume *volume;
};

/** Name of a target state, for logging. */
const char *memos_initiator_ref_app_kv_info_target_state_str(enum doca_memos_initiator_dev_target_state state);

/**
 * @brief Log what the device supports, before anything is configured on it.
 *
 * @param dev  the fabric-side DOCA device the initiator was given.
 */
void memos_initiator_ref_app_kv_info_log_device_caps(struct doca_dev *dev);

/** @brief Log which statistics counters exist, per task type. */
void memos_initiator_ref_app_kv_info_log_stats_caps(void);

/**
 * @brief Log the configuration and object graph as the library holds them.
 *
 * Reads the values back from the library instead of echoing what was set, so the log shows what the
 * initiator actually believes - including where the volume clamps the target's limits.
 *
 * @param dev     the initiator device.
 * @param io      the I/O context.
 * @param target  the connected target.
 * @param ns      the adopted target namespace.
 * @param volume  the volume the namespace is attached to.
 */
void memos_initiator_ref_app_kv_info_log_sdk_view(struct doca_memos_initiator_dev *dev,
						  struct doca_memos_initiator_dev_io *io,
						  struct doca_memos_initiator_dev_target *target,
						  struct doca_memos_initiator_dev_target_ns *ns,
						  struct doca_memos_initiator_dev_volume *volume);

/**
 * @brief Statistics-collect handler. Register with doca_memos_initiator_dev_event_volume_collect_stats_register(),
 * with a struct memos_initiator_ref_app_kv_info_stats as the registration user data.
 */
void memos_initiator_ref_app_kv_info_collect_stats_cb(struct doca_memos_initiator_dev_volume *volume,
						      union doca_data reg_user_data,
						      union doca_data op_user_data,
						      doca_error_t status);

/**
 * @brief Statistics-reset handler. Register with the same user data as above.
 */
void memos_initiator_ref_app_kv_info_stats_completion_cb(struct doca_memos_initiator_dev_volume *volume,
							 union doca_data reg_user_data,
							 union doca_data op_user_data,
							 doca_error_t status);

/**
 * @brief Drive every engine the backend owns once.
 *
 * @param ctx  caller-supplied context.
 * @return number of events processed.
 */
typedef int (*memos_initiator_ref_app_kv_info_progress_fn)(void *ctx);

/**
 * @brief Collect and log the volume's statistics, then zero them.
 *
 * Calls @p progress until each operation's event arrives. No-op if @p volume or @p ns is NULL, which
 * is how a teardown after a failed bring-up reaches it. The statistics events are delivered on
 * whichever engine carries the initiator context that services them, so a caller spreading its
 * contexts over several engines must drive them all from @p progress.
 *
 * @param progress      drives the caller's engines.
 * @param progress_ctx  passed to @p progress.
 * @param stats         the caller's hand-off, already registered with both statistics events.
 * @param volume        the volume to report on.
 * @param ns            the namespace to report counters for.
 */
void memos_initiator_ref_app_kv_info_report_stats(memos_initiator_ref_app_kv_info_progress_fn progress,
						  void *progress_ctx,
						  struct memos_initiator_ref_app_kv_info_stats *stats,
						  struct doca_memos_initiator_dev_volume *volume,
						  struct doca_memos_initiator_dev_target_ns *ns);

#endif /* MEMOS_INITIATOR_REF_APP_KV_INFO_H_ */
