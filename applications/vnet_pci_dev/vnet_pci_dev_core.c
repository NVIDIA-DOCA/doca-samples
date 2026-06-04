/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <linux/if_ether.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_vnet.h>
#include <doca_devemu_vnet_offload_engine.h>
#include <doca_devemu_virtio.h>
#include <doca_devemu_virtio_tlp.h>
#include <doca_devemu_vnet_io.h>
#include <doca_devemu_vnet_counters.h>
#include <doca_buf.h>
#include <infiniband/verbs.h>

#include "common.h"
#include "vnet_pci_dev_core.h"
#include "vnet_pci_dev_lu.h"
#include "vnet_pci_device.h"
#include "vnet_virtio_types.h"

DOCA_LOG_REGISTER(VNET_PCI_DEV_CORE);

#define STATS_POLL_INTERVAL_USEC 10000	 /* 10ms polling interval for stats populate completion */
#define MAX_CLEANUP_WAIT_ITERATIONS 1000 /* 10 seconds at 10ms per iteration for cleanup timeout */
#define UNPLUG_TIMEOUT_SEC 30		 /* Seconds to wait for host Power OFF before forcing unplug */
#define MSI_RETRY_ABANDON_SEC 5		 /* Give up MSI retry after this; host will poll Slot Status */
#define UNPLUG_PLUG_ACK_WAIT_MS 2000	 /* Max time to wait for host to ACK prior plug (DLActive=1) before unplug */
#define UNPLUG_PLUG_ACK_POLL_US 1000	 /* Poll interval while waiting for DLActive during unplug defer */
#define VNET_CTRL_REQ_DELAY_USEC 400000	 /* One-time initial-stage ctrl_req delay */
#define VNET_NSEC_PER_SEC 1000000000ULL

/**
 * @brief Close stats log files during final cleanup
 *
 * This is now a no-op since log files are per-controller and closed
 * in vnet_controller_cleanup(). Kept for API compatibility.
 */
static void close_stats_log_files(void)
{
	/* Per-controller log files are closed in vnet_controller_cleanup() */
}

static uint64_t vnet_timespec_to_ns(const struct timespec *ts)
{
	if (ts == NULL || ts->tv_sec < 0 || ts->tv_nsec < 0)
		return 0;

	return (uint64_t)ts->tv_sec * VNET_NSEC_PER_SEC + (uint64_t)ts->tv_nsec;
}

static bool vnet_pci_dev_diagnostics_cancelled(struct vnet_pci_dev_controller *controller)
{
	struct vnet_pci_device *dev;

	if (controller == NULL)
		return true;

	dev = atomic_load(&controller->virtio_device);
	if (dev == NULL)
		return true;

	return atomic_load(&controller->shutting_down) || atomic_load(&controller->stop_stats_collection) ||
	       atomic_load(&controller->cleanup_running) || atomic_load(&controller->initialization_in_progress) ||
	       atomic_load(&dev->cancel_in_progress);
}

static bool vnet_controller_should_defer_initial_data_qps(const struct vnet_pci_dev_controller *controller)
{
	return controller != NULL && atomic_load(&controller->deferred_mq.initial_data_qps_deferred);
}

static bool vnet_controller_has_reset_teardown_work(const struct vnet_pci_dev_controller *controller)
{
	if (controller == NULL)
		return false;

	return atomic_load(&controller->offload_engine_started) || atomic_load(&controller->vqs_initialized) ||
	       atomic_load(&controller->engine_enabled) || atomic_load(&controller->stats_ref) != NULL;
}

static void vnet_controller_clear_deferred_mq_start(struct vnet_pci_dev_controller *controller)
{
	if (controller == NULL)
		return;

	atomic_store(&controller->deferred_mq.start_deferred, false);
	atomic_store(&controller->deferred_mq.old_qps, 0);
	atomic_store(&controller->deferred_mq.new_qps, 0);
}

static void vnet_controller_set_deferred_mq_start(struct vnet_pci_dev_controller *controller,
						  uint16_t old_qps,
						  uint16_t new_qps)
{
	if (controller == NULL)
		return;

	atomic_store(&controller->deferred_mq.old_qps, old_qps);
	atomic_store(&controller->deferred_mq.new_qps, new_qps);
	atomic_store(&controller->deferred_mq.start_deferred, true);

	DOCA_LOG_INFO("MQ defer: deferring MQ start %u -> %u until all %u static devices are live",
		      old_qps,
		      new_qps,
		      controller->tlp_ctx != NULL ? controller->tlp_ctx->num_ep : 0);
}

static void vnet_controller_maybe_submit_full_stats_list(struct vnet_pci_dev_controller *controller)
{
	if (controller == NULL)
		return;

	if (!atomic_load(&controller->engine_enabled) ||
	    atomic_load(&controller->deferred_mq.initial_data_qps_deferred))
		return;

	pci_cfg_workqueue_submit_create_stats_list(controller);
}

static bool vnet_tlp_ctx_should_defer_mq_start(const struct tlp_context *tlp_ctx)
{
	return tlp_ctx != NULL && !tlp_ctx->hotplug_mode && tlp_ctx->num_ep > 1;
}

static bool vnet_controller_ready_for_deferred_mq_start(const struct vnet_pci_dev_controller *controller)
{
	struct vnet_pci_device *dev;
	const struct vnet_virtio_common_config *common_cfg;

	if (controller == NULL || !controller->io_ctx_started)
		return false;

	dev = atomic_load(&controller->virtio_device);
	if (dev == NULL)
		return false;

	if (!atomic_load(&controller->engine_enabled) || atomic_load(&controller->cleanup_running) ||
	    atomic_load(&controller->initialization_in_progress))
		return false;

	common_cfg = vnet_pci_device_get_pci_cfg(dev);
	return common_cfg != NULL && (common_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) != 0;
}

static bool vnet_tlp_ctx_all_controllers_ready_for_mq_start(const struct tlp_context *tlp_ctx)
{
	uint32_t i;

	if (!vnet_tlp_ctx_should_defer_mq_start(tlp_ctx))
		return true;

	for (i = 0; i < tlp_ctx->num_ep; i++) {
		if (!vnet_controller_ready_for_deferred_mq_start(&tlp_ctx->vnet_controller[i]))
			return false;
	}

	return true;
}

static void vnet_maybe_release_deferred_mq_starts(struct tlp_context *tlp_ctx)
{
	uint32_t deferred_count = 0;
	uint16_t old_qps;
	uint16_t new_qps;
	uint32_t i;

	if (!vnet_tlp_ctx_all_controllers_ready_for_mq_start(tlp_ctx))
		return;

	for (i = 0; i < tlp_ctx->num_ep; i++) {
		struct vnet_pci_dev_controller *controller = &tlp_ctx->vnet_controller[i];

		if (atomic_load(&controller->deferred_mq.start_deferred))
			deferred_count++;
	}

	if (deferred_count != 0) {
		DOCA_LOG_INFO("MQ defer: all %u/%u controllers live - releasing %u deferred MQ starts",
			      tlp_ctx->num_ep,
			      tlp_ctx->num_ep,
			      deferred_count);
	}

	for (i = 0; i < tlp_ctx->num_ep; i++) {
		struct vnet_pci_dev_controller *controller = &tlp_ctx->vnet_controller[i];

		if (!atomic_load(&controller->deferred_mq.start_deferred))
			continue;

		old_qps = atomic_load(&controller->deferred_mq.old_qps);
		new_qps = atomic_load(&controller->deferred_mq.new_qps);
		atomic_store(&controller->deferred_mq.start_deferred, false);
		atomic_store(&controller->deferred_mq.old_qps, 0);
		atomic_store(&controller->deferred_mq.new_qps, 0);

		DOCA_LOG_INFO("MQ defer: enumeration settled, releasing deferred MQ start %u -> %u on PF[%u]",
			      old_qps,
			      new_qps,
			      i);
		pci_cfg_workqueue_submit_mq_start_qps(controller, old_qps, new_qps);
	}
}

/**
 * @brief Print VirtIO queue statistics
 *
 * Prints detailed statistics for a VirtIO queue including queue ID, size,
 * in-flight requests, and various index values from both hardware and driver perspectives.
 *
 * @param[in] log_file Log file to write stats to
 * @param[in] stats Queue statistics structure to print
 */
static void print_vq_stats(FILE *log_file, struct doca_devemu_virtio_queue_dbg_state *stats)
{
	uint16_t id, size, inflights, hw_avail_idx, driver_avail_idx, hw_used_idx, driver_used_idx;
	uint8_t enabled;

	doca_devemu_virtio_queue_dbg_state_get_id(stats, &id);
	doca_devemu_virtio_queue_dbg_state_get_enabled(stats, &enabled);

	/* Only print stats for enabled/active queues */
	if (enabled != 1)
		return;

	doca_devemu_virtio_queue_dbg_state_get_size(stats, &size);
	doca_devemu_virtio_queue_dbg_state_get_inflights(stats, &inflights);
	doca_devemu_virtio_queue_dbg_state_get_hw_avail_idx(stats, &hw_avail_idx);
	doca_devemu_virtio_queue_dbg_state_get_driver_avail_idx(stats, &driver_avail_idx);
	doca_devemu_virtio_queue_dbg_state_get_hw_used_idx(stats, &hw_used_idx);
	doca_devemu_virtio_queue_dbg_state_get_driver_used_idx(stats, &driver_used_idx);

	fprintf(log_file, "[%ld] INFO: === VQ%u Statistics ===\n", time(NULL), id);
	fprintf(log_file, "[%ld] INFO:   Queue Size: %u\n", time(NULL), size);
	fprintf(log_file, "[%ld] INFO:   In-flight Packets: %u\n", time(NULL), inflights);
	fprintf(log_file, "[%ld] INFO:   HW Available Index: %u\n", time(NULL), hw_avail_idx);
	fprintf(log_file, "[%ld] INFO:   Driver Available Index: %u\n", time(NULL), driver_avail_idx);
	fprintf(log_file, "[%ld] INFO:   HW Used Index: %u\n", time(NULL), hw_used_idx);
	fprintf(log_file, "[%ld] INFO:   Driver Used Index: %u\n", time(NULL), driver_used_idx);
}

/**
 * @brief Poll and drain in-flight stats operation
 *
 * Called from cleanup to check if an async stats populate operation has completed.
 * If completed (or error), clears stats_in_progress flag so cleanup can proceed.
 * Acquires stats_ref_mutex to prevent UAF if the workqueue is swapping stats_ref.
 *
 * @param[in] controller VNet controller with stats list
 * @return true if stats_in_progress is now false (safe to destroy), false if still in progress
 */
static bool vnet_drain_stats_in_progress(struct vnet_pci_dev_controller *controller)
{
	struct vnet_stats_list_ref *local_ref;
	uint8_t is_populated;
	doca_error_t result;
	bool drained = false;

	/* If not in progress, nothing to drain */
	if (!atomic_load(&controller->dbg_state.stats_in_progress))
		return true;

	/* Lock stats_ref_mutex to prevent UAF if the workqueue thread is
	 * concurrently swapping/freeing stats_ref (e.g., on driver rebind). */
	pthread_mutex_lock(&controller->stats_ref_mutex);

	/* Get stats_ref - if NULL, stats_in_progress shouldn't be true but clear it anyway */
	local_ref = atomic_load(&controller->stats_ref);
	if (!local_ref) {
		DOCA_LOG_WARN("stats_in_progress true but stats_ref is NULL - clearing");
		atomic_store(&controller->dbg_state.stats_in_progress, false);
		pthread_mutex_unlock(&controller->stats_ref_mutex);
		return true;
	}

	/* Poll is_populated to check if DOCA has finished using state_list */
	result = doca_devemu_virtio_queue_dbg_state_is_populated(local_ref->state_list, &is_populated);
	if (result != DOCA_SUCCESS) {
		if (result == DOCA_ERROR_BAD_STATE) {
			/* Engine/VQs in bad state - DOCA confirms it's not using state_list */
			DOCA_LOG_DBG("Stats drain: BAD_STATE - clearing stats_in_progress");
			atomic_store(&controller->dbg_state.stats_in_progress, false);
			drained = true;
		}
		/* Other errors - keep waiting */
		pthread_mutex_unlock(&controller->stats_ref_mutex);
		return drained;
	}

	if (is_populated) {
		/* DOCA finished - safe to clear flag */
		DOCA_LOG_DBG("Stats drain: populated complete - clearing stats_in_progress");
		atomic_store(&controller->dbg_state.stats_in_progress, false);
		drained = true;
	}

	pthread_mutex_unlock(&controller->stats_ref_mutex);
	return drained;
}

/**
 * @brief Collect and print VNet queue statistics periodically
 *
 * Manages the asynchronous collection of VirtIO queue statistics and prints them
 * every 5 seconds. Uses a state machine approach to handle the async populate/poll pattern.
 *
 * @param[in] controller VNet controller with stats list
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 *
 * @note This function is polled from the worker-side diagnostics path
 * @note Uses static variables to maintain state between calls
 */
static doca_error_t collect_and_print_vnet_stats(struct vnet_pci_dev_controller *controller)
{
	time_t current_time;
	doca_error_t result;
	uint8_t is_populated;
	uint16_t pf_index;
	struct vnet_stats_list_ref *local_ref;
	struct vnet_pci_device *dev;

	/* Skip if no controller */
	if (!controller)
		return DOCA_SUCCESS;

	/* Skip if virtio_device is NULL - device not yet created (hotplug mode) */
	dev = atomic_load(&controller->virtio_device);
	if (dev == NULL)
		return DOCA_SUCCESS;
	if (vnet_pci_dev_diagnostics_cancelled(controller))
		return DOCA_SUCCESS;

	pf_index = dev->pf_index;

	/* Open per-controller log file on first call */
	if (controller->stats_log_file == NULL) {
		char log_path[64];
		snprintf(log_path, sizeof(log_path), "/tmp/vnet_stats_pf%u.log", pf_index);
		controller->stats_log_file = fopen(log_path, "a");
		if (controller->stats_log_file == NULL) {
			DOCA_LOG_ERR("Failed to open stats log file: %s", log_path);
			return DOCA_ERROR_IO_FAILED;
		}
		fprintf(controller->stats_log_file,
			"\n=== VNet Stats Log Started at %ld (PF%u) ===\n",
			time(NULL),
			pf_index);
		fflush(controller->stats_log_file);
	}

	/* Try to acquire stats_ref_mutex - NON-BLOCKING to keep PE1 responsive.
	 * The mutex may be held by the workqueue (swap), cleanup (destroy), or
	 * a concurrent stats collection. Skipping is safe since stats are
	 * collected every 5s and missing one iteration is acceptable. */
	if (pthread_mutex_trylock(&controller->stats_ref_mutex) != 0) {
		/* Mutex held - skip this iteration */
		return DOCA_SUCCESS;
	}

	/* Load stats_ref under mutex protection */
	local_ref = atomic_load(&controller->stats_ref);

	if (vnet_pci_dev_diagnostics_cancelled(controller) && !atomic_load(&controller->dbg_state.stats_in_progress))
		goto unlock_and_return;

	/* Skip if no stats list available */
	if (!local_ref) {
		/* Reset state machine if stats_ref was destroyed (e.g., during reset/shutdown)
		 * This prevents stale state from carrying over after driver rebind */
		if (atomic_load(&controller->dbg_state.stats_in_progress)) {
			DOCA_LOG_DBG("PF%u: Stats in progress but stats_ref destroyed - resetting", pf_index);
			atomic_store(&controller->dbg_state.stats_in_progress, false);
		}
		pthread_mutex_unlock(&controller->stats_ref_mutex);
		return DOCA_SUCCESS;
	}

	/* Skip starting NEW stats collection if cleanup/shutdown in progress.
	 * NOTE: If stats_in_progress is true, we must NOT clear it here - the async populate
	 * may still be in flight. We continue to the polling section below which will check
	 * is_populated() to determine when the async operation actually completes.
	 * Cleanup calls doca_pe_progress() to drive the async operation to completion. */
	if (atomic_load(&controller->stop_stats_collection) || atomic_load(&controller->shutting_down)) {
		if (!atomic_load(&controller->dbg_state.stats_in_progress)) {
			/* No stats in progress - nothing to do */
			goto unlock_and_return;
		}
		/* Stats in progress - fall through to polling section to check completion.
		 * Do NOT clear stats_in_progress here as DOCA may still be using state_list. */
	}

	current_time = time(NULL);

	/* Non-blocking state machine for stats collection.
	 * This function is polled repeatedly from the worker diagnostics path and must
	 * not block, allowing doca_pe_progress() to continue processing other async
	 * operations. */
	if (atomic_load(&controller->dbg_state.stats_in_progress)) {
		/* Check if cleanup started - we must still poll is_populated() to detect when
		 * the async operation completes. We cannot just clear stats_in_progress because
		 * DOCA may still be using state_list internally. Cleanup drives PE progress
		 * and waits for stats_in_progress to become false via is_populated() completion. */
		bool cleanup_requested = atomic_load(&controller->stop_stats_collection) ||
					 atomic_load(&controller->shutting_down) ||
					 vnet_pci_dev_diagnostics_cancelled(controller);

		/* Non-blocking poll: check if stats are ready.
		 * We MUST call is_populated() to determine if the async operation completed.
		 * Only clear stats_in_progress when is_populated returns true or a definitive error.
		 * This ensures DOCA is no longer using state_list before cleanup can destroy it. */
		result = doca_devemu_virtio_queue_dbg_state_is_populated(local_ref->state_list, &is_populated);
		if (result != DOCA_SUCCESS) {
			if (result == DOCA_ERROR_BAD_STATE) {
				/* Engine/VQs in bad state - DOCA confirms it's not using state_list.
				 * Safe to clear stats_in_progress. */
				DOCA_LOG_DBG("PF%u: Stats list in BAD_STATE - aborting", pf_index);
				atomic_store(&controller->dbg_state.stats_in_progress, false);
			}
			/* For other errors, will retry on next call */
			goto unlock_and_return;
		}

		if (!is_populated) {
			/* Not ready yet - async operation still in flight.
			 * Keep polling until is_populated returns true.
			 * Cleanup has its own timeout and will skip destruction if needed. */
			goto unlock_and_return;
		}

		/* Stats are ready (is_populated returned true).
		 * DOCA is no longer using state_list - safe to clear stats_in_progress.
		 * Skip printing during cleanup to reduce noise.
		 *
		 * We use local_ref which was captured atomically at function entry,
		 * guaranteeing consistent (state_list, list_len) pair. */
		if (!cleanup_requested) {
			fprintf(controller->stats_log_file,
				"[%ld] PF%u: Queue stats collected (%u queues)\n",
				time(NULL),
				pf_index,
				local_ref->list_len);
			for (uint32_t i = 0; i < local_ref->list_len; i++)
				print_vq_stats(controller->stats_log_file, local_ref->state_list[i]);
			/* Flush periodically - reduce I/O overhead.
			 * Use last_collection_time as flush interval reference. */
			fflush(controller->stats_log_file);
		}

		atomic_store(&controller->dbg_state.stats_in_progress, false);
	} else if (current_time - atomic_load(&controller->dbg_state.last_collection_time) >= 5) {
		/* CAS to update collection time - prevents race with counters.
		 * If counters already updated it (stats was skipped), CAS fails and we skip.
		 * This ensures only one function advances the timestamp per cycle. */
		time_t expected_time = atomic_load(&controller->dbg_state.last_collection_time);
		if (!atomic_compare_exchange_strong(&controller->dbg_state.last_collection_time,
						    &expected_time,
						    current_time)) {
			/* Counters already updated - skip this cycle */
			goto unlock_and_return;
		}
		/* Time to start new stats collection.
		 * We only transition stats_in_progress from false->true if cleanup is not requested.
		 * This avoids a TOCTOU window with cleanup and prevents using a freed stats_ref. */
		if (vnet_pci_dev_diagnostics_cancelled(controller))
			goto unlock_and_return;

		bool expected = false;
		if (!atomic_compare_exchange_strong(&controller->dbg_state.stats_in_progress, &expected, true)) {
			/* Another stats collection started concurrently */
			goto unlock_and_return;
		}

		/* Re-check if cleanup started after we set the flag - abort before using state_list. */
		if (vnet_pci_dev_diagnostics_cancelled(controller)) {
			DOCA_LOG_DBG("PF%u: Diagnostics cancelled - aborting stats start", pf_index);
			atomic_store(&controller->dbg_state.stats_in_progress, false);
			goto unlock_and_return;
		}

		result = doca_devemu_virtio_queue_dbg_state_populate_list(local_ref->state_list);
		if (result != DOCA_SUCCESS) {
			if (result == DOCA_ERROR_IN_PROGRESS) {
				/* DOCA is already populating - keep stats_in_progress true and continue polling.
				 * Do NOT clear the flag as DOCA is actively using state_list. */
				DOCA_LOG_DBG("PF%u: Stats population already in progress (DOCA)", pf_index);
				goto unlock_and_return;
			}
			/* Other errors - clear flag and abort */
			DOCA_LOG_DBG("PF%u: Failed to populate stats list: %s", pf_index, doca_error_get_descr(result));
			atomic_store(&controller->dbg_state.stats_in_progress, false);
			pthread_mutex_unlock(&controller->stats_ref_mutex);
			return result;
		}
	}

unlock_and_return:
	pthread_mutex_unlock(&controller->stats_ref_mutex);
	return DOCA_SUCCESS;
}

static doca_error_t collect_and_print_vnet_virtqueue_counters(struct vnet_pci_dev_controller *controller)
{
	struct doca_devemu_vnet_counters *local_counters;
	time_t current_time;
	doca_error_t cnt_res;
	uint16_t total_vqs;
	uint16_t pf_index;
	struct vnet_pci_device *dev;

	if (controller == NULL)
		return DOCA_SUCCESS;

	/* Skip if virtio_device is NULL - device not yet created (hotplug mode) */
	dev = atomic_load(&controller->virtio_device);
	if (dev == NULL)
		return DOCA_SUCCESS;
	if (vnet_pci_dev_diagnostics_cancelled(controller))
		return DOCA_SUCCESS;

	/* Get PF index for per-controller log file naming */
	pf_index = dev->pf_index;

	/* Open per-controller log file on first call */
	if (controller->counters_log_file == NULL) {
		char log_path[64];
		snprintf(log_path, sizeof(log_path), "/tmp/vnet_counters_pf%u.log", pf_index);
		controller->counters_log_file = fopen(log_path, "a");
		if (controller->counters_log_file == NULL) {
			DOCA_LOG_ERR("Failed to open counters log file: %s", log_path);
			return DOCA_ERROR_IO_FAILED;
		}
		fprintf(controller->counters_log_file,
			"\n=== VNet Virtqueue Counters Log Started at %ld (PF%u) ===\n",
			time(NULL),
			pf_index);
		fflush(controller->counters_log_file);
	}

	/* Skip if cleanup/shutdown in progress.
	 * Clear counters_in_progress if it was set to allow cleanup to proceed. */
	if (vnet_pci_dev_diagnostics_cancelled(controller)) {
		if (atomic_load(&controller->dbg_state.counters_in_progress))
			atomic_store(&controller->dbg_state.counters_in_progress, false);
		return DOCA_SUCCESS;
	}

	current_time = time(NULL);

	/* Throttle check: only run if 5+ seconds since last collection.
	 * Use CAS to atomically update last_collection_time - prevents repeated runs
	 * when stats skips due to trylock failure (stats normally updates this). */
	time_t last_time = atomic_load(&controller->dbg_state.last_collection_time);
	if (current_time - last_time < 5)
		return DOCA_SUCCESS;

	/* CAS: only proceed if we successfully update last_collection_time.
	 * If stats already updated it, CAS fails and we skip (both ran in same cycle).
	 * If stats skipped (trylock failed), CAS succeeds and we run once. */
	if (!atomic_compare_exchange_strong(&controller->dbg_state.last_collection_time, &last_time, current_time))
		return DOCA_SUCCESS;

	/* Set counters_in_progress BEFORE using vnet_counters to prevent race with cleanup.
	 * Cleanup waits for counters_in_progress to become false before destroying vnet_counters. */
	atomic_store(&controller->dbg_state.counters_in_progress, true);

	/* Check if cleanup started after we set the flag - abort before using counters */
	if (vnet_pci_dev_diagnostics_cancelled(controller)) {
		atomic_store(&controller->dbg_state.counters_in_progress, false);
		return DOCA_SUCCESS;
	}

	/* Capture local copy after cleanup check - pointer is now safe while counters_in_progress is true */
	local_counters = controller->vnet_counters;
	if (local_counters == NULL) {
		atomic_store(&controller->dbg_state.counters_in_progress, false);
		return DOCA_SUCCESS;
	}

	cnt_res = doca_devemu_vnet_counters_populate_sync(local_counters);
	if (cnt_res != DOCA_SUCCESS) {
		DOCA_LOG_DBG("Failed to populate counters: %s", doca_error_get_descr(cnt_res));
		atomic_store(&controller->dbg_state.counters_in_progress, false);
		return DOCA_SUCCESS;
	}

	/* Check for cleanup request after sync operation */
	if (vnet_pci_dev_diagnostics_cancelled(controller)) {
		atomic_store(&controller->dbg_state.counters_in_progress, false);
		return DOCA_SUCCESS;
	}

	fprintf(controller->counters_log_file,
		"[%" PRIdMAX "] INFO: === PF%u VNet Virtqueue Counters ===\n",
		(intmax_t)time(NULL),
		pf_index);

	total_vqs = VNET_TOTAL_VQS(controller->max_queue_pairs);
	for (uint16_t vq_index = 0; vq_index < total_vqs; vq_index++) {
		struct doca_devemu_vnet_vq_counters_rx rx_cnt = {0};
		struct doca_devemu_vnet_vq_counters_tx tx_cnt = {0};

		if (vnet_pci_dev_diagnostics_cancelled(controller)) {
			DOCA_LOG_DBG("Counters collection aborted during VQ iteration");
			break;
		}

		/* RX query: returns INVALID_VALUE for non-RX queues */
		cnt_res = doca_devemu_vnet_counters_rx_vq_query(local_counters, vq_index, &rx_cnt);
		if (cnt_res == DOCA_SUCCESS) {
			intmax_t ts = (intmax_t)time(NULL);
			fprintf(controller->counters_log_file,
				"[%" PRIdMAX "] INFO: RX VQ%u: pkts=%" PRIu64 " bytes=%" PRIu64 "\n",
				ts,
				vq_index,
				rx_cnt.common.packets,
				rx_cnt.common.bytes);
			fprintf(controller->counters_log_file,
				"[%" PRIdMAX "] INFO:   size bins: <=64=%" PRIu64 " 65-127=%" PRIu64 " 128-255=%" PRIu64
				" 256-511=%" PRIu64 " 512-1023=%" PRIu64 "\n",
				ts,
				rx_cnt.packets.rx_64_or_less_octet_packets,
				rx_cnt.packets.rx_65_to_127_octet_packets,
				rx_cnt.packets.rx_128_to_255_octet_packets,
				rx_cnt.packets.rx_256_to_511_octet_packets,
				rx_cnt.packets.rx_512_to_1023_octet_packets);
			fprintf(controller->counters_log_file,
				"[%" PRIdMAX "] INFO:   size bins: 1024-1522=%" PRIu64 " 1523-2047=%" PRIu64
				" 2048-4095=%" PRIu64 " 4096-8191=%" PRIu64 " 8192-9022=%" PRIu64 "\n",
				ts,
				rx_cnt.packets.rx_1024_to_1522_octet_packets,
				rx_cnt.packets.rx_1523_to_2047_octet_packets,
				rx_cnt.packets.rx_2048_to_4095_octet_packets,
				rx_cnt.packets.rx_4096_to_8191_octet_packets,
				rx_cnt.packets.rx_8192_to_9022_octet_packets);
		} else if (cnt_res != DOCA_ERROR_INVALID_VALUE) {
			DOCA_LOG_DBG("RX VQ%u query failed: %s", vq_index, doca_error_get_descr(cnt_res));
		}

		/* TX query: returns INVALID_VALUE for non-TX queues */
		cnt_res = doca_devemu_vnet_counters_tx_vq_query(local_counters, vq_index, &tx_cnt);
		if (cnt_res == DOCA_SUCCESS) {
			intmax_t ts = (intmax_t)time(NULL);
			fprintf(controller->counters_log_file,
				"[%" PRIdMAX "] INFO: TX VQ%u: pkts=%" PRIu64 " bytes=%" PRIu64 "\n",
				ts,
				vq_index,
				tx_cnt.common.packets,
				tx_cnt.common.bytes);
		} else if (cnt_res != DOCA_ERROR_INVALID_VALUE) {
			DOCA_LOG_DBG("TX VQ%u query failed: %s", vq_index, doca_error_get_descr(cnt_res));
		}
	}

	/* Flush after each collection cycle */
	fflush(controller->counters_log_file);
	/* Time already updated by stats function at start of collection cycle */
	atomic_store(&controller->dbg_state.counters_in_progress, false);
	return DOCA_SUCCESS;
}

void vnet_pci_dev_worker_collect_diagnostics(struct vnet_pci_dev_controller *controller)
{
	doca_error_t result;

	if (controller == NULL || !atomic_load(&controller->engine_enabled) ||
	    vnet_pci_dev_diagnostics_cancelled(controller))
		return;

	result = collect_and_print_vnet_stats(controller);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_DBG("Worker diagnostics: stats collection skipped/failed: %s", doca_error_get_descr(result));
	}
	if (vnet_pci_dev_diagnostics_cancelled(controller))
		return;

	result = collect_and_print_vnet_virtqueue_counters(controller);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_DBG("Worker diagnostics: counters collection skipped/failed: %s",
			     doca_error_get_descr(result));
	}
}

/**
 * @brief Find and open DOCA device by IB device name or PCI address
 *
 * If ibdev_name is non-empty, searches by IB device name (e.g., "mlx5_bond_0_pci_dev").
 * Otherwise falls back to PCI address (e.g., "0000:03:00.0").
 *
 * @param[in] pci_addr PCI address string (used when ibdev_name is empty)
 * @param[in] ibdev_name IB device name string (takes priority if non-empty)
 * @param[out] doca_dev Pointer to store the opened DOCA device handle
 * @return DOCA_SUCCESS on success, DOCA_ERROR on failure
 */
static doca_error_t find_doca_device(const char *pci_addr, const char *ibdev_name, struct doca_dev **doca_dev)
{
	doca_error_t result;
	bool use_ibdev = (ibdev_name != NULL && ibdev_name[0] != '\0');

	if (use_ibdev) {
		DOCA_LOG_DBG("Looking for DOCA device by IB name: %s", ibdev_name);
		result = open_doca_device_with_ibdev_name((const uint8_t *)ibdev_name,
							  strlen(ibdev_name),
							  NULL,
							  doca_dev);
	} else {
		DOCA_LOG_DBG("Looking for DOCA device by PCI address: %s", pci_addr);
		result = open_doca_device_with_pci(pci_addr, NULL, doca_dev);
	}

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device %s: %s",
			     use_ibdev ? ibdev_name : pci_addr,
			     doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_INFO("Opened DOCA device: %s", use_ibdev ? ibdev_name : pci_addr);
	return DOCA_SUCCESS;
}

/**
 * @brief Initialize DOCA Progress Engine in TLP context
 *
 * Creates and initializes a DOCA Progress Engine for handling asynchronous
 * operations and events. The PE is essential for processing TLP events
 * and managing device interactions.
 *
 * @param[in,out] tlp_ctx TLP context to store progress engine
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 *
 * @note PE must be destroyed with doca_pe_destroy() during cleanup
 */
static doca_error_t init_progress_engine(struct tlp_context *tlp_ctx)
{
	doca_error_t result;

	result = doca_pe_create(&tlp_ctx->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Progress engine (PE1) initialized successfully");
	return DOCA_SUCCESS;
}

/*
 * Cleanup progress engine in TLP context
 *
 * @tlp_ctx [in]: TLP context containing progress engine
 */
static void cleanup_progress_engine(struct tlp_context *tlp_ctx)
{
	if (!tlp_ctx || !tlp_ctx->pe)
		return;

	(void)doca_pe_destroy(tlp_ctx->pe);
	tlp_ctx->pe = NULL;
	DOCA_LOG_INFO("Progress engine cleaned up successfully");
}

/**
 * @brief Parse MAC address string into byte array
 *
 * Converts a MAC address string in standard format (XX:XX:XX:XX:XX:XX)
 * into a ETH_ALEN-byte array. Validates format and range of each octet.
 *
 * @param[in] mac_str MAC address string in "XX:XX:XX:XX:XX:XX" format
 * @param[out] mac_bytes ETH_ALEN-byte array to store parsed MAC address
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE on parse error
 *
 * @note Accepts hexadecimal values (0-FF) for each octet
 * @note Case insensitive for hex digits (a-f or A-F)
 */
static doca_error_t parse_mac_address(const char *mac_str, uint8_t *mac_bytes)
{
	int parsed_mac_bytes[ETH_ALEN];
	int result;

	result = sscanf(mac_str,
			"%x:%x:%x:%x:%x:%x",
			&parsed_mac_bytes[0],
			&parsed_mac_bytes[1],
			&parsed_mac_bytes[2],
			&parsed_mac_bytes[3],
			&parsed_mac_bytes[4],
			&parsed_mac_bytes[5]);

	if (result != ETH_ALEN) {
		DOCA_LOG_ERR("Invalid MAC address format: %s", mac_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (int i = 0; i < ETH_ALEN; i++) {
		if (parsed_mac_bytes[i] < 0 || parsed_mac_bytes[i] > 255) {
			DOCA_LOG_ERR("Invalid MAC address byte value: %d", parsed_mac_bytes[i]);
			return DOCA_ERROR_INVALID_VALUE;
		}
		mac_bytes[i] = parsed_mac_bytes[i];
	}

	return DOCA_SUCCESS;
}

/*********************************************************************************************************************
 * VNet Controller Management Functions
 *********************************************************************************************************************/

doca_error_t vnet_pci_dev_initialize_vqs(struct vnet_pci_dev_controller *controller)
{
	doca_error_t result;
	uint16_t i;

	if (!controller->offload_engine) {
		DOCA_LOG_WARN("Offload engine destroyed - skipping VQ init (stale work item)");
		return DOCA_ERROR_BAD_STATE;
	}

	if (atomic_load(&controller->cleanup_running)) {
		DOCA_LOG_WARN("Cleanup in progress - skipping VQ init");
		return DOCA_ERROR_BAD_STATE;
	}

	if (!atomic_load(&controller->offload_engine_started)) {
		DOCA_LOG_ERR("Engine must be started before VQ initialization");
		return DOCA_ERROR_BAD_STATE;
	}

	if (atomic_load(&controller->vqs_initialized)) {
		DOCA_LOG_ERR("VQs are already initialized");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Initialize all VQ pointers to NULL (up to configured max) */
	for (i = 0; i < controller->max_queue_pairs; i++) {
		controller->rx_vqs[i] = NULL;
		controller->tx_vqs[i] = NULL;
	}
	controller->cvq = NULL;

	/* Create ALL VQs */
	for (i = 0; i < controller->max_queue_pairs; i++) {
		/* Create RX VQ for this QP */
		result = doca_devemu_vnet_rx_vq_create(controller->offload_engine, &controller->rx_vqs[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create RX VQ%d: %s", i * 2, doca_error_get_descr(result));
			goto cleanup_vqs;
		}

		/* Create TX VQ for this QP */
		result = doca_devemu_vnet_tx_vq_create(controller->offload_engine, &controller->tx_vqs[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create TX VQ%d: %s", i * 2 + 1, doca_error_get_descr(result));
			goto cleanup_vqs;
		}

		DOCA_LOG_DBG("Created QP%d: RX VQ%d, TX VQ%d", i, i * 2, i * 2 + 1);
	}

	DOCA_LOG_INFO("Created %d queue pairs (VQ0-VQ%d)",
		      controller->max_queue_pairs,
		      controller->max_queue_pairs * 2 - 1);

	/* Create CVQ ONLY if MQ feature was negotiated */
	if (controller->mq_feature_negotiated) {
		result = doca_devemu_vnet_ctrl_vq_create(controller->offload_engine, &controller->cvq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create CVQ: %s", doca_error_get_descr(result));
			goto cleanup_vqs;
		}
		DOCA_LOG_INFO("Created CVQ (VQ%d)", VNET_CVQ_INDEX(controller->max_queue_pairs));
	} else {
		DOCA_LOG_INFO("CVQ not created - MQ feature not negotiated (single-QP mode)");
	}

	atomic_store(&controller->vqs_initialized, true);
	DOCA_LOG_INFO("VQ initialization complete - ready for host configuration");
	return DOCA_SUCCESS;

cleanup_vqs:
	/* Destroy any VQs that were created (up to configured max) */
	for (i = 0; i < controller->max_queue_pairs; i++) {
		if (controller->tx_vqs[i])
			doca_devemu_vnet_tx_vq_destroy(controller->tx_vqs[i]);
		if (controller->rx_vqs[i])
			doca_devemu_vnet_rx_vq_destroy(controller->rx_vqs[i]);
	}

	return result;
}

doca_error_t vnet_pci_dev_enable_engine(struct vnet_pci_dev_controller *controller)
{
	doca_error_t result;
	struct doca_devemu_virtio_offload_engine *virtio_engine;

	if (!controller->offload_engine) {
		DOCA_LOG_WARN("Offload engine destroyed - skipping enable (stale work item)");
		return DOCA_ERROR_BAD_STATE;
	}

	if (atomic_load(&controller->cleanup_running)) {
		DOCA_LOG_WARN("Cleanup in progress - skipping engine enable");
		return DOCA_ERROR_BAD_STATE;
	}

	if (!atomic_load(&controller->vqs_initialized)) {
		DOCA_LOG_ERR("VQs must be initialized before engine enable");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Idempotent: if already enabled, return success.
	 * This handles duplicate start_and_enable calls from deferred init
	 * and IO context completion racing with each other. */
	if (atomic_load(&controller->engine_enabled)) {
		DOCA_LOG_DBG("Engine already enabled - skipping (idempotent)");
		return DOCA_SUCCESS;
	}

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(controller->offload_engine);
	if (!virtio_engine) {
		DOCA_LOG_ERR("Failed to get virtio offload engine");
		return DOCA_ERROR_UNEXPECTED;
	}

	/* This will implicitly start and enable both embedded VQs */
	result = doca_devemu_virtio_offload_engine_enable(virtio_engine);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable offload engine: %s", doca_error_get_descr(result));
		return result;
	}

	atomic_store(&controller->engine_enabled, true);
	DOCA_LOG_INFO("VNet offload engine enabled successfully");
	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_dev_start_vqs(struct vnet_pci_dev_controller *controller, struct vnet_pci_device *dev)
{
	const struct vnet_virtio_queue_config *vqs;
	doca_error_t result;
	uint16_t cvq_index;
	uint16_t initial_data_qps;
	uint16_t active_qps;
	uint16_t i;

	if (!controller || !dev) {
		DOCA_LOG_ERR("Invalid parameters for start_vqs");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!controller->offload_engine) {
		DOCA_LOG_WARN("Offload engine destroyed - skipping start_vqs (stale work item)");
		return DOCA_ERROR_BAD_STATE;
	}

	if (atomic_load(&controller->cleanup_running)) {
		DOCA_LOG_WARN("Cleanup in progress - skipping start_vqs");
		return DOCA_ERROR_BAD_STATE;
	}

	struct vnet_virtio_common_config *common_cfg = vnet_pci_device_get_pci_cfg(dev);
	if (!(common_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK)) {
		DOCA_LOG_DBG("Device not in DRIVER_OK state (status=0x%02x) - skipping start_vqs",
			     common_cfg->device_status);
		return DOCA_SUCCESS;
	}

	vqs = vnet_pci_device_get_virtq_pci_cfg(dev);
	cvq_index = VNET_CVQ_INDEX(controller->max_queue_pairs);
	active_qps = atomic_load(&controller->num_active_qps);
	/* In deferred first-stage bring-up, start CVQ only and skip data VQs for now. */
	initial_data_qps = vnet_controller_should_defer_initial_data_qps(controller) ? 0 : active_qps;

	/* Start active queue pairs only.
	 * Validate that each VQ has complete configuration (all addresses set) before starting.
	 * Host may set queue_enable before completing all address writes, so we must check
	 * all fields to avoid intermittent start failures. Skip incompletely configured VQs
	 * - later VQ config updates will retry start_and_enable under DRIVER_OK. */
	for (i = 0; i < initial_data_qps; i++) {
		uint16_t rx_idx = i * 2;

		if (controller->rx_vqs[i] && vqs[rx_idx].queue_enable && vqs[rx_idx].queue_desc != 0 &&
		    vqs[rx_idx].queue_driver != 0 && vqs[rx_idx].queue_device != 0) {
			struct doca_devemu_virtio_vq *rx_vq = doca_devemu_vnet_rx_vq_as_vq(controller->rx_vqs[i]);

			result = doca_devemu_virtio_vq_set_conf(rx_vq,
								rx_idx,
								vqs[rx_idx].queue_size,
								vqs[rx_idx].queue_msix_vector,
								vqs[rx_idx].queue_desc,
								vqs[rx_idx].queue_driver,
								vqs[rx_idx].queue_device);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to configure RX VQ%d: %s", rx_idx, doca_error_get_descr(result));
				return result;
			}

			result = doca_devemu_virtio_vq_start(rx_vq);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to start RX VQ%d: %s", rx_idx, doca_error_get_descr(result));
				return result;
			}
			DOCA_LOG_DBG("VQ%d (QP%d RX) started (%d/%d active)",
				     rx_idx,
				     i,
				     active_qps,
				     controller->max_queue_pairs);
		} else if (controller->rx_vqs[i] && vqs[rx_idx].queue_enable) {
			DOCA_LOG_DBG("VQ%d (RX) not fully configured yet - deferring start", rx_idx);
		}

		uint16_t tx_idx = i * 2 + 1;
		if (controller->tx_vqs[i] && vqs[tx_idx].queue_enable && vqs[tx_idx].queue_desc != 0 &&
		    vqs[tx_idx].queue_driver != 0 && vqs[tx_idx].queue_device != 0) {
			struct doca_devemu_virtio_vq *tx_vq = doca_devemu_vnet_tx_vq_as_vq(controller->tx_vqs[i]);

			result = doca_devemu_virtio_vq_set_conf(tx_vq,
								tx_idx,
								vqs[tx_idx].queue_size,
								vqs[tx_idx].queue_msix_vector,
								vqs[tx_idx].queue_desc,
								vqs[tx_idx].queue_driver,
								vqs[tx_idx].queue_device);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to configure TX VQ%d: %s", tx_idx, doca_error_get_descr(result));
				return result;
			}

			result = doca_devemu_virtio_vq_start(tx_vq);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to start TX VQ%d: %s", tx_idx, doca_error_get_descr(result));
				return result;
			}
			DOCA_LOG_DBG("VQ%d (QP%d TX) started (%d/%d active)",
				     tx_idx,
				     i,
				     active_qps,
				     controller->max_queue_pairs);
		} else if (controller->tx_vqs[i] && vqs[tx_idx].queue_enable) {
			DOCA_LOG_DBG("VQ%d (TX) not fully configured yet - deferring start", tx_idx);
		}
	}

	if (controller->cvq && vqs[cvq_index].queue_enable && vqs[cvq_index].queue_desc != 0 &&
	    vqs[cvq_index].queue_driver != 0 && vqs[cvq_index].queue_device != 0) {
		struct doca_devemu_virtio_vq *cvq_vq = doca_devemu_vnet_ctrl_vq_as_vq(controller->cvq);
		struct doca_devemu_virtio_io *virtio_io = doca_devemu_vnet_io_as_virtio_io(controller->io_ctx);

		result = doca_devemu_virtio_vq_set_conf(cvq_vq,
							cvq_index,
							vqs[cvq_index].queue_size,
							vqs[cvq_index].queue_msix_vector,
							vqs[cvq_index].queue_desc,
							vqs[cvq_index].queue_driver,
							vqs[cvq_index].queue_device);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure CVQ: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_devemu_virtio_vq_start(cvq_vq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start CVQ: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_DBG("VQ%d (CVQ) started", cvq_index);

		result = doca_devemu_virtio_io_bind_vq(virtio_io, cvq_vq, controller);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to bind CVQ to IO context: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_DBG("VQ%d (CVQ) bound to IO context", cvq_index);
		atomic_store(&controller->cvq_bound, true);
	} else if (controller->cvq && vqs[cvq_index].queue_enable) {
		DOCA_LOG_DBG("VQ%d (CVQ) not fully configured yet - deferring start", cvq_index);
	} else if (controller->mq_feature_negotiated) {
		DOCA_LOG_DBG("CVQ not yet configured - will start when host configures it");
	} else {
		DOCA_LOG_DBG("MQ not negotiated - CVQ not required");
	}

	DOCA_LOG_INFO("All VQs configured and started (engine not yet enabled)");
	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_dev_start_and_enable(struct vnet_pci_dev_controller *controller, struct vnet_pci_device *dev)
{
	doca_error_t result;

	if (!controller || !dev) {
		DOCA_LOG_ERR("Invalid parameters for start and enable");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!controller->offload_engine) {
		DOCA_LOG_WARN("Offload engine destroyed - skipping start_and_enable (stale work item)");
		return DOCA_ERROR_BAD_STATE;
	}

	if (atomic_load(&controller->cleanup_running)) {
		DOCA_LOG_WARN("Cleanup in progress - skipping start_and_enable");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Idempotent: skip if already enabled (deferred init + IO path can race). */
	if (controller->engine_enabled) {
		DOCA_LOG_DBG("Engine already enabled - skipping start_and_enable (idempotent)");
		return DOCA_SUCCESS;
	}

	/* start_vqs succeeds even without DRIVER_OK; guard here to avoid
	 * proceeding to enable_engine on stale work items. */
	struct vnet_virtio_common_config *common_cfg = vnet_pci_device_get_pci_cfg(dev);
	if (!(common_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK)) {
		DOCA_LOG_DBG("Device not in DRIVER_OK state (status=0x%02x) - skipping start_and_enable",
			     common_cfg->device_status);
		return DOCA_SUCCESS;
	}

	/* Queue configuration can still be arriving after DRIVER_OK.
	 * Defer the actual enable until the first-stage queues needed for
	 * bring-up are fully configured; later VQ config updates will
	 * requeue start_and_enable while engine_enabled is false. */
	const struct vnet_virtio_queue_config *vqs = vnet_pci_device_get_virtq_pci_cfg(dev);
	uint16_t cvq_index = VNET_CVQ_INDEX(controller->max_queue_pairs);
	bool first_qp_ready = vqs[0].queue_enable && vqs[0].queue_desc != 0 && vqs[0].queue_driver != 0 &&
			      vqs[0].queue_device != 0 && vqs[1].queue_enable && vqs[1].queue_desc != 0 &&
			      vqs[1].queue_driver != 0 && vqs[1].queue_device != 0;
	bool cvq_ready = !controller->mq_feature_negotiated ||
			 (vqs[cvq_index].queue_enable && vqs[cvq_index].queue_desc != 0 &&
			  vqs[cvq_index].queue_driver != 0 && vqs[cvq_index].queue_device != 0);

	if (vnet_controller_should_defer_initial_data_qps(controller)) {
		if (!cvq_ready) {
			DOCA_LOG_DBG("start_and_enable deferred until CVQ configuration is complete");
			return DOCA_SUCCESS;
		}
	} else if (!first_qp_ready) {
		DOCA_LOG_DBG("start_and_enable deferred until first QP configuration is complete");
		return DOCA_SUCCESS;
	}

	result = vnet_pci_dev_start_vqs(controller, dev);
	if (result != DOCA_SUCCESS)
		return result;

	/* Enable engine - makes device operational */
	DOCA_LOG_INFO("Enabling VNet offload engine");
	result = vnet_pci_dev_enable_engine(controller);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable engine: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("VNet offload engine enabled - device ready for DRIVER_OK");

	/* Create VNET virtqueue counters instance - must be done AFTER engine is started/enabled. */
	if (controller->vnet_counters == NULL) {
		doca_error_t cnt_result =
			doca_devemu_vnet_counters_create(controller->offload_engine, &controller->vnet_counters);
		if (cnt_result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to create VNET counters: %s", doca_error_get_descr(cnt_result));
			controller->vnet_counters = NULL;
		} else {
			(void)doca_devemu_vnet_counters_reset(controller->vnet_counters);
			atomic_store(&controller->dbg_state.last_collection_time, time(NULL));
			DOCA_LOG_DBG("VNET counters initialized");
		}
	}

	vnet_controller_maybe_submit_full_stats_list(controller);
	vnet_maybe_release_deferred_mq_starts(controller->tlp_ctx);

	return DOCA_SUCCESS;
}

/*********************************************************************************************************************
 * VQ Lifecycle Helper Functions
 *
 * These helpers provide a single source of truth for VQ operations, ensuring:
 * - Consistent NULL safety checks
 * - Correct DOCA API workflow compliance
 * - Reduced code duplication between RESET and shutdown paths
 *********************************************************************************************************************/

/**
 * @brief Check if controller has any VQs allocated
 *
 * @param[in] ctrl Controller to check
 * @return true if VQs are initialized, false otherwise
 */
static bool vnet_controller_has_vqs(struct vnet_pci_dev_controller *ctrl)
{
	if (!ctrl)
		return false;

	/* Use the explicit flag rather than checking array pointers.
	 * The arrays (rx_vqs, tx_vqs) remain allocated even after VQs are destroyed,
	 * so checking them would give false positives. */
	return atomic_load(&ctrl->vqs_initialized);
}

/**
 * @brief Disable all VQs (must be called while engine is ENABLED)
 *
 * This step allows engine_disable() to skip the DISABLING state and go
 * directly to DISABLED, preventing blocking behavior.
 *
 * @param[in] ctrl Controller with VQs to disable
 */
static void vnet_controller_disable_all_vqs(struct vnet_pci_dev_controller *ctrl)
{
	if (!ctrl || !ctrl->rx_vqs || !ctrl->tx_vqs)
		return;

	for (uint16_t i = 0; i < ctrl->max_queue_pairs; i++) {
		if (ctrl->rx_vqs[i]) {
			struct doca_devemu_virtio_vq *vq = doca_devemu_vnet_rx_vq_as_vq(ctrl->rx_vqs[i]);
			if (vq)
				(void)doca_devemu_virtio_vq_disable(vq);
		}
		if (ctrl->tx_vqs[i]) {
			struct doca_devemu_virtio_vq *vq = doca_devemu_vnet_tx_vq_as_vq(ctrl->tx_vqs[i]);
			if (vq)
				(void)doca_devemu_virtio_vq_disable(vq);
		}
	}

	if (ctrl->cvq) {
		struct doca_devemu_virtio_vq *vq = doca_devemu_vnet_ctrl_vq_as_vq(ctrl->cvq);
		if (vq)
			(void)doca_devemu_virtio_vq_disable(vq);
	}
}

/**
 * @brief Stop all VQs (must be called when engine is DISABLED, not DISABLING)
 *
 * @param[in] ctrl Controller with VQs to stop
 */
static void vnet_controller_stop_all_vqs(struct vnet_pci_dev_controller *ctrl)
{
	if (!ctrl || !ctrl->rx_vqs || !ctrl->tx_vqs)
		return;

	for (uint16_t i = 0; i < ctrl->max_queue_pairs; i++) {
		if (ctrl->rx_vqs[i]) {
			struct doca_devemu_virtio_vq *vq = doca_devemu_vnet_rx_vq_as_vq(ctrl->rx_vqs[i]);
			if (vq)
				(void)doca_devemu_virtio_vq_stop(vq);
		}
		if (ctrl->tx_vqs[i]) {
			struct doca_devemu_virtio_vq *vq = doca_devemu_vnet_tx_vq_as_vq(ctrl->tx_vqs[i]);
			if (vq)
				(void)doca_devemu_virtio_vq_stop(vq);
		}
	}

	if (ctrl->cvq) {
		struct doca_devemu_virtio_vq *vq = doca_devemu_vnet_ctrl_vq_as_vq(ctrl->cvq);

		if (vq) {
			/* Unbind CVQ from IO context before stopping (only if IO context exists) */
			if (ctrl->io_ctx && atomic_load(&ctrl->cvq_bound)) {
				struct doca_devemu_virtio_io *virtio_io =
					doca_devemu_vnet_io_as_virtio_io(ctrl->io_ctx);
				if (virtio_io) {
					doca_devemu_virtio_io_flush_vq(virtio_io, vq);
					(void)doca_devemu_virtio_io_unbind_vq(virtio_io, vq);
					atomic_store(&ctrl->cvq_bound, false);
				}
			}
			(void)doca_devemu_virtio_vq_stop(vq);
		}
	}
}

/**
 * @brief Destroy all VQs and clear pointers
 *
 * @param[in] ctrl Controller with VQs to destroy
 */
static void vnet_controller_destroy_all_vqs(struct vnet_pci_dev_controller *ctrl)
{
	if (!ctrl)
		return;

	if (ctrl->rx_vqs && ctrl->tx_vqs) {
		for (uint16_t i = 0; i < ctrl->max_queue_pairs; i++) {
			if (ctrl->rx_vqs[i]) {
				(void)doca_devemu_vnet_rx_vq_destroy(ctrl->rx_vqs[i]);
				ctrl->rx_vqs[i] = NULL;
			}
			if (ctrl->tx_vqs[i]) {
				(void)doca_devemu_vnet_tx_vq_destroy(ctrl->tx_vqs[i]);
				ctrl->tx_vqs[i] = NULL;
			}
		}
	}

	if (ctrl->cvq) {
		(void)doca_devemu_vnet_ctrl_vq_destroy(ctrl->cvq);
		ctrl->cvq = NULL;
	}

	atomic_store(&ctrl->cvq_bound, false);
	atomic_store(&ctrl->vqs_initialized, false);
}

/**
 * @brief Cleanup IO context using worker PE (PE2)
 *
 * Dual PE Architecture:
 * IO context is connected to PE2 (worker PE), which worker thread drives.
 * No need to pause main thread's PE1 - we just drive PE2 ourselves.
 * This eliminates cross-thread PE synchronization complexity.
 *
 * @param[in] controller VNet controller
 */
static void vnet_pci_dev_cleanup_io_context(struct vnet_pci_dev_controller *controller)
{
	struct doca_devemu_vnet_io *saved_io_ctx;
	struct doca_devemu_virtio_io *virtio_io;
	struct doca_ctx *io_ctx;
	enum doca_ctx_states ctx_state = DOCA_CTX_STATE_STARTING;
	doca_error_t err;
	int drain_iterations = 0;

	if (!controller->io_ctx)
		return;

	DOCA_LOG_INFO("Cleaning up IO context using worker PE");

	/* Save pointer and clear controller->io_ctx FIRST.
	 * This prevents any new access to the context from other threads. */
	saved_io_ctx = controller->io_ctx;
	controller->io_ctx = NULL;
	controller->io_ctx_started = false;

	virtio_io = doca_devemu_vnet_io_as_virtio_io(saved_io_ctx);
	io_ctx = doca_devemu_virtio_io_as_ctx(virtio_io);

	/* Flush any pending tasks before stopping */
	(void)doca_ctx_flush_tasks(io_ctx);

	/* Stop the context */
	err = doca_ctx_stop(io_ctx);
	if (err == DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_DBG("IO context stop in progress - draining to IDLE state");

		/* Guard against NULL worker_pe - can happen on early failure cleanup paths
		 * where worker PE was not yet created. Skip drain and proceed to destroy. */
		if (controller->worker_pe == NULL) {
			DOCA_LOG_WARN("Worker PE is NULL - cannot drain IO context, forcing destroy");
		} else {
			/* Drain using worker PE (PE2) - no conflict with main loop's PE1 */
			do {
				(void)doca_pe_progress(controller->worker_pe);
				err = doca_ctx_get_state(io_ctx, &ctx_state);
				if (err != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to get IO context state: %s", doca_error_get_descr(err));
					break;
				}
				drain_iterations++;
			} while (ctx_state != DOCA_CTX_STATE_IDLE &&
				 drain_iterations < VNET_PCI_DEV_PE_DRAIN_MAX_ITERATIONS);
		}

		if (controller->worker_pe != NULL) {
			if (drain_iterations >= VNET_PCI_DEV_PE_DRAIN_MAX_ITERATIONS) {
				DOCA_LOG_WARN("IO context drain timeout after %d iterations (state=%d)",
					      drain_iterations,
					      ctx_state);
			} else if (ctx_state == DOCA_CTX_STATE_IDLE) {
				DOCA_LOG_INFO("IO context drained to IDLE after %d PE progress calls",
					      drain_iterations);
			}
		}
	} else if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop IO context: %s", doca_error_get_descr(err));
	} else {
		DOCA_LOG_INFO("IO context stopped successfully (was already idle)");
		ctx_state = DOCA_CTX_STATE_IDLE;
	}

	/* Destroy the context - with dedicated worker PE, we can destroy immediately */
	(void)doca_devemu_vnet_io_destroy(saved_io_ctx);
	DOCA_LOG_INFO("IO context destroyed");
}

doca_error_t vnet_controller_cleanup(struct vnet_pci_dev_controller *ctrl, bool destroy_engine)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine = NULL;
	doca_error_t result = DOCA_SUCCESS;
	bool has_vqs;
	bool cleanup_incomplete = false;

	if (!ctrl)
		return DOCA_SUCCESS;

	/* Prevent double cleanup - use atomic CAS to ensure only one thread proceeds */
	bool expected_cleanup = false;
	if (!atomic_compare_exchange_strong(&ctrl->cleanup_running, &expected_cleanup, true)) {
		DOCA_LOG_DBG("Cleanup already in progress, skipping");
		return DOCA_SUCCESS;
	}

	vnet_controller_clear_deferred_mq_start(ctrl);

	/* Get virtio engine handle if offload engine exists */
	if (ctrl->offload_engine)
		virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine);

	has_vqs = vnet_controller_has_vqs(ctrl);

	/* Step 0: Stop stats collection before destroying counters/state_list to prevent race conditions.
	 * Both vnet_counters and state_list are accessed from the worker-side
	 * diagnostics path.
	 *
	 * Dual PE Architecture: Worker PE (PE2) handles stats async operations independently.
	 * We drive PE2 to complete any pending async stats operations.
	 * Main loop continues driving PE1 (TLP) - no synchronization needed.
	 */
	if (ctrl->vnet_counters || atomic_load(&ctrl->stats_ref)) {
		atomic_store(&ctrl->stop_stats_collection, true);

		/* Wait for stats and counters collection to complete.
		 * We drive worker PE (PE2) to complete any async stats operations.
		 * Timeout after 10 seconds to prevent hanging if there's a bug. */
		int wait_iterations = 0;

		while ((atomic_load(&ctrl->dbg_state.stats_in_progress) ||
			atomic_load(&ctrl->dbg_state.counters_in_progress)) &&
		       wait_iterations < MAX_CLEANUP_WAIT_ITERATIONS) {
			/* Drive worker PE to complete async stats operations */
			if (ctrl->worker_pe)
				(void)doca_pe_progress(ctrl->worker_pe);

			/* Directly poll stats completion status and clear flag if done */
			(void)vnet_drain_stats_in_progress(ctrl);

			usleep(STATS_POLL_INTERVAL_USEC);
			wait_iterations++;
		}

		/* Final drain attempt */
		if (ctrl->worker_pe)
			(void)doca_pe_progress(ctrl->worker_pe);
		(void)vnet_drain_stats_in_progress(ctrl);
	}

	/* Check if stats/counters collection is still in progress after waiting.
	 * If so, we CANNOT safely destroy the resources - skip destruction to prevent UAF. */
	bool skip_stats_destroy = atomic_load(&ctrl->dbg_state.stats_in_progress);
	bool skip_counters_destroy = atomic_load(&ctrl->dbg_state.counters_in_progress);

	if (skip_stats_destroy || skip_counters_destroy) {
		DOCA_LOG_ERR("Stats/counters collection did not complete within 10s timeout - "
			     "skipping resource destruction to prevent UAF (stats=%d, counters=%d)",
			     skip_stats_destroy,
			     skip_counters_destroy);
		cleanup_incomplete = true;
	}

	/* Step 0a: Destroy VNET virtqueue counters instance (only if counters collection stopped) */
	if (ctrl->vnet_counters && !skip_counters_destroy) {
		DOCA_LOG_INFO("Cleanup Step 0a: Destroying VNET virtqueue counters...");
		(void)doca_devemu_vnet_counters_destroy(ctrl->vnet_counters);
		ctrl->vnet_counters = NULL;
	} else if (ctrl->vnet_counters && skip_counters_destroy) {
		DOCA_LOG_WARN("Cleanup Step 0a: Skipping counters destroy - still in use");
	}

	/* Step 0b: Destroy stats list if exists (only if stats collection stopped) */
	if (!skip_stats_destroy) {
		/* Use bounded trylock loop instead of blocking mutex_lock.
		 * Prevents hang if workqueue is holding mutex during slow DOCA API call.
		 * Timeout after 2 seconds - matches other cleanup timeouts. */
		int mutex_acquired = 0;
		for (int try_count = 0; try_count < 200; try_count++) {
			if (pthread_mutex_trylock(&ctrl->stats_ref_mutex) == 0) {
				mutex_acquired = 1;
				break;
			}
			usleep(10000); /* 10ms */
		}

		if (!mutex_acquired) {
			DOCA_LOG_WARN("Cleanup Step 0b: Mutex timeout - skipping stats list destroy");
			cleanup_incomplete = true;
		} else {
			struct vnet_stats_list_ref *local_ref = atomic_load(&ctrl->stats_ref);
			if (local_ref) {
				DOCA_LOG_INFO("Cleanup Step 0b: Destroying stats list...");

				/* Clear stats_ref before destroying - under mutex protection */
				atomic_store(&ctrl->stats_ref, NULL);

				result = doca_devemu_virtio_queue_dbg_state_destroy_list(local_ref->state_list);
				if (result != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to destroy stats list: %s", doca_error_get_descr(result));
				} else {
					DOCA_LOG_INFO("Cleanup Step 0b: Stats list destroyed");
				}
				free(local_ref);
			}
			pthread_mutex_unlock(&ctrl->stats_ref_mutex);
		}
	} else {
		DOCA_LOG_WARN("Cleanup Step 0b: Skipping stats list destroy - still in use");
	}

	/* Step 0c: Config MSI-X lifecycle management.
	 * On reset (destroy_engine=false): keep the MSI-X alive -- vnet_ensure_config_msix()
	 * at the next config change will recreate it only if the host changed the vector.
	 * On full teardown (destroy_engine=true): destroy it now since the endpoint is going away. */
	if (ctrl->config_msix && destroy_engine) {
		DOCA_LOG_INFO("Cleanup Step 0c: Destroying config MSI-X (full teardown)...");
		doca_error_t msix_result = doca_devemu_pci_msix_destroy(ctrl->config_msix);
		if (msix_result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Config MSI-X destroy returned: %s", doca_error_get_descr(msix_result));
		else
			DOCA_LOG_INFO("Config MSI-X destroyed");
		ctrl->config_msix = NULL;
		ctrl->config_msix_vector_cached = VIRTIO_MSI_NO_VECTOR;
	}

	/* Step 1: Disable all VQs individually (while engine is ENABLED)
	 * This allows engine_disable() to skip DISABLING state and avoid blocking */
	if (has_vqs && atomic_load(&ctrl->offload_engine_started) && atomic_load(&ctrl->engine_enabled)) {
		DOCA_LOG_INFO("Cleanup Step 1: Disabling VQs (while engine enabled)...");
		vnet_controller_disable_all_vqs(ctrl);
		DOCA_LOG_INFO("Cleanup Step 1: All VQs disabled");
	}

	/* Step 2: Disable engine (fast - VQs already disabled, skips DISABLING) */
	if (atomic_load(&ctrl->engine_enabled) && virtio_engine) {
		DOCA_LOG_INFO("Cleanup Step 2: Disabling engine...");
		result = doca_devemu_virtio_offload_engine_disable(virtio_engine);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_WARN("Engine disable returned: %s", doca_error_get_descr(result));
		}
		atomic_store(&ctrl->engine_enabled, false);
	}

	/* Drive worker PE progress before VQ stop to complete any pending async operations */
	if (ctrl->worker_pe)
		(void)doca_pe_progress(ctrl->worker_pe);

	/* Step 3: Stop all VQs (engine is now DISABLED, not DISABLING) */
	if (has_vqs && atomic_load(&ctrl->offload_engine_started)) {
		DOCA_LOG_INFO("Cleanup Step 3: Stopping VQs...");
		vnet_controller_stop_all_vqs(ctrl);
		DOCA_LOG_INFO("Cleanup Step 3: All VQs stopped");
	}

	/* Drive worker PE progress after VQ stop */
	if (ctrl->worker_pe)
		(void)doca_pe_progress(ctrl->worker_pe);

	/* Cleanup IO context (after VQ operations).
	 * With dual PE: IO context is on PE2 (worker PE), so we drive PE2 ourselves
	 * to drain and destroy the context. No impact on PE1 (main PE). */
	vnet_pci_dev_cleanup_io_context(ctrl);

	/* Step 4: Destroy all VQs */
	if (has_vqs && atomic_load(&ctrl->offload_engine_started)) {
		DOCA_LOG_INFO("Cleanup Step 4: Destroying VQs...");
		vnet_controller_destroy_all_vqs(ctrl);
		DOCA_LOG_INFO("Cleanup Step 4: All VQs destroyed");
	} else if (has_vqs) {
		/* VQs exist but engine not started - just clear pointers */
		DOCA_LOG_WARN("VQs exist but engine not started - clearing pointers only");
		vnet_controller_destroy_all_vqs(ctrl);
	}

	/* Drive worker PE progress before engine stop to complete any pending async operations */
	if (ctrl->worker_pe)
		(void)doca_pe_progress(ctrl->worker_pe);

	/* Step 5: Stop engine (now safe - no VQs associated) */
	if (atomic_load(&ctrl->offload_engine_started) && virtio_engine) {
		DOCA_LOG_INFO("Cleanup Step 5: Stopping engine...");
		result = doca_devemu_virtio_offload_engine_stop(virtio_engine);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_WARN("Engine stop returned: %s", doca_error_get_descr(result));
		}
		atomic_store(&ctrl->offload_engine_started, false);
	}

	/* Step 6: Destroy engine (only for app shutdown, not RESET) */
	if (destroy_engine && ctrl->offload_engine) {
		/* If cleanup was incomplete (resources not destroyed), abort before engine destroy.
		 * This prevents resource leaks and teardown ordering violations.
		 * Caller should NOT free the controller if this returns error. */
		if (cleanup_incomplete) {
			DOCA_LOG_ERR("CRITICAL: Cannot destroy engine - stats/counters resources "
				     "not cleaned up. Aborting to prevent leaks.");
			atomic_store(&ctrl->cleanup_running, false);
			return DOCA_ERROR_IN_PROGRESS;
		}

		DOCA_LOG_INFO("Cleanup Step 6: Destroying offload engine...");
		result = doca_devemu_vnet_offload_engine_destroy(ctrl->offload_engine);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("CRITICAL: Engine destroy failed: %s", doca_error_get_descr(result));
			/* Don't return early - must reset flags in cleanup_done */
			goto cleanup_done;
		}
		ctrl->offload_engine = NULL;
		DOCA_LOG_INFO("Offload engine destroyed successfully");
	}

cleanup_done:
	/* Reset stats collection flags after cleanup.
	 * For RESET case: allows stats to resume after rebind
	 * For DESTROY case: prevents log spam since controller struct still exists */
	if (!cleanup_incomplete) {
		atomic_store(&ctrl->stop_stats_collection, false);
	} else if (!destroy_engine) {
		DOCA_LOG_WARN("Reset cleanup incomplete - keeping stats collection stopped");
	}
	atomic_store(&ctrl->shutting_down, false);

	/* Close per-controller log files on full shutdown */
	if (destroy_engine && ctrl->stats_log_file) {
		fprintf(ctrl->stats_log_file, "[%ld] INFO: === VNet Stats Log Closed ===\n", time(NULL));
		fclose(ctrl->stats_log_file);
		ctrl->stats_log_file = NULL;
	}

	if (destroy_engine && ctrl->counters_log_file) {
		fprintf(ctrl->counters_log_file,
			"[%ld] INFO: === VNet Virtqueue Counters Log Closed ===\n",
			time(NULL));
		fclose(ctrl->counters_log_file);
		ctrl->counters_log_file = NULL;
	}

	/* Clear transient cleanup flags before releasing any host-visible reset hold. */
	atomic_store(&ctrl->cleanup_running, false);
	atomic_store(&ctrl->initialization_in_progress, false);
	atomic_store(&ctrl->deferred_mq.first_ctrl_req_delay_done, false);
	atomic_store(&ctrl->deferred_mq.initial_data_qps_deferred, false);

	if (!destroy_engine) {
		struct vnet_pci_device *dev = atomic_load(&ctrl->virtio_device);
		uint32_t expected_state = VNET_RESET_STATUS_HELD;
		enum vnet_reset_status_state next_state = (dev == NULL) ? VNET_RESET_STATUS_IDLE :
									  VNET_RESET_STATUS_RELEASE_PENDING;
		bool reset_hold_finished;

		if (cleanup_incomplete) {
			if (atomic_load(&ctrl->reset_status_state) == VNET_RESET_STATUS_HELD)
				DOCA_LOG_WARN("RESET: controller cleanup incomplete, keeping host-visible reset hold");
			goto reset_hold_done;
		}

		reset_hold_finished =
			atomic_compare_exchange_strong(&ctrl->reset_status_state, &expected_state, next_state);
		if (reset_hold_finished) {
			if (dev == NULL) {
				DOCA_LOG_WARN("RESET: cleanup complete with no VirtIO device, reset hold dropped");
			} else {
				DOCA_LOG_INFO("RESET: controller cleanup complete, reset hold release queued");
			}
		}
		if (reset_hold_finished && dev != NULL)
			atomic_store(&dev->cancel_in_progress, false);
	}

reset_hold_done:

	/* For reset case with incomplete cleanup, return error but allow re-initialization attempts.
	 * Stats collection remains stopped to prevent use of leaked resources. */
	if (!destroy_engine && cleanup_incomplete) {
		DOCA_LOG_WARN("Reset cleanup incomplete - returning error (stats stopped, deferred init skipped)");
		return DOCA_ERROR_IN_PROGRESS;
	}

	if (result == DOCA_SUCCESS)
		DOCA_LOG_INFO("Controller cleanup completed successfully");
	else
		DOCA_LOG_WARN("Controller cleanup completed with errors");

	/* Check if FEATURES_OK arrived during cleanup and was deferred.
	 * If so, trigger initialization now that cleanup is complete.
	 * This handles the case where host doesn't retry status changes.
	 * NOTE: Only execute if cleanup was complete (cleanup_incomplete == false). */
	if (atomic_load(&ctrl->deferred_init_pending) && !destroy_engine && !cleanup_incomplete) {
		bool has_cvq, has_mq;
		struct vnet_pci_device *dev = atomic_load(&ctrl->virtio_device);

		if (dev == NULL) {
			DOCA_LOG_WARN("Deferred init skipped - no active VirtIO device");
			atomic_store(&ctrl->deferred_init_pending, false);
			return result;
		}

		DOCA_LOG_INFO("Deferred init: FEATURES_OK was skipped, starting initialization now");
		atomic_store(&ctrl->deferred_init_pending, false);
		atomic_store(&ctrl->initialization_in_progress, true);
		__sync_synchronize();

		/* Set up MQ features (same as FEATURES_OK handler) */
		has_cvq = !!(dev->driver_features & (1ULL << VIRTIO_NET_F_CTRL_VQ));
		has_mq = !!(dev->driver_features & (1ULL << VIRTIO_NET_F_MQ));
		ctrl->mq_feature_negotiated = (has_cvq && has_mq);

		if (ctrl->mq_feature_negotiated) {
			DOCA_LOG_INFO("Deferred init: MQ features negotiated (CVQ+MQ)");
			atomic_store(&ctrl->num_active_qps, VNET_DEFAULT_QUEUE_PAIRS);
			ctrl->max_queue_pairs = ctrl->tlp_ctx->max_queue_pairs;
			atomic_store(&ctrl->deferred_mq.initial_data_qps_deferred,
				     vnet_tlp_ctx_should_defer_mq_start(ctrl->tlp_ctx));
		} else {
			DOCA_LOG_INFO("Deferred init: MQ NOT negotiated - single QP mode");
			atomic_store(&ctrl->num_active_qps, 1);
			ctrl->max_queue_pairs = 1;
			atomic_store(&ctrl->deferred_mq.initial_data_qps_deferred, false);
		}

		/* Queue initialization work items - same as FEATURES_OK handler.
		 * We're on worker thread, so these will execute after this function returns. */
		if (!ctrl->offload_engine_started) {
			DOCA_LOG_INFO("Deferred init: Submitting engine_start");
			pci_cfg_workqueue_submit_engine_start(ctrl);
		}
		DOCA_LOG_INFO("Deferred init: Submitting initialize_vqs");
		pci_cfg_workqueue_submit_initialize_vqs(ctrl);
		DOCA_LOG_INFO("Deferred init: Submitting initialize_io_context");
		pci_cfg_workqueue_submit_initialize_io_context(ctrl);

		/* Also queue start_and_enable since DRIVER_OK is likely already set.
		 * The function will check engine_enabled to avoid duplicate enable. */
		DOCA_LOG_INFO("Deferred init: Submitting start_and_enable (DRIVER_OK likely set)");
		pci_cfg_workqueue_submit_start_and_enable(ctrl, dev);
	}

	return result;
}

/**
 * @brief Shutdown VNet device with proper error handling and API compliance
 *
 * Uses the unified cleanup function to ensure consistent lifecycle management.
 * This is used for complete teardown (application exit).
 *
 * @param[in] controller VNet controller
 * @return DOCA_SUCCESS on complete success, DOCA_ERROR_* on any failure
 */
static doca_error_t vnet_pci_dev_shutdown_device(struct vnet_pci_dev_controller *controller)
{
	if (!controller)
		return DOCA_SUCCESS;

	DOCA_LOG_INFO("Beginning device shutdown (vqs_initialized=%d, engine_started=%d, engine_enabled=%d)...",
		      atomic_load(&controller->vqs_initialized),
		      atomic_load(&controller->offload_engine_started),
		      atomic_load(&controller->engine_enabled));

	/* Set shutdown flag to prevent callbacks from interfering */
	atomic_store(&controller->shutting_down, true);

	/* Use unified cleanup with destroy_engine=true for full teardown */
	return vnet_controller_cleanup(controller, true);
}

/**
 * @brief Query and display SF representor information
 *
 * Queries the SF representor from the offload engine and displays its information.
 * This can be called immediately after engine creation, before host power on.
 *
 * @param[in] controller VNet controller with offload engine
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t vnet_controller_query_and_display_rep_info(struct vnet_pci_dev_controller *controller)
{
	enum doca_pci_func_type pci_func_type;
	struct doca_devinfo_rep *devinfo_rep;
	struct doca_dev_rep *rep = NULL;
	uint32_t sf_index = 0;
	uint16_t vhca_id = 0;
	uint32_t ifindex = 0;
	doca_error_t result;

	if (!controller || !controller->offload_engine) {
		DOCA_LOG_ERR("Invalid controller or offload engine");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_devemu_vnet_offload_engine_get_rep(controller->offload_engine, &rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get representor: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get devinfo for querying attributes */
	devinfo_rep = doca_dev_rep_as_devinfo(rep);
	if (!devinfo_rep) {
		DOCA_LOG_WARN("Failed to get devinfo from representor");
		return DOCA_SUCCESS; /* Rep is still valid, just can't display all info */
	}

	/* Get VHCA ID */
	result = doca_devinfo_rep_get_vhca_id(devinfo_rep, &vhca_id);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_DBG("Failed to get VHCA ID: %s", doca_error_get_descr(result));

	/* Get interface index */
	result = doca_devinfo_rep_get_iface_index(devinfo_rep, &ifindex);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_DBG("Failed to get interface index: %s", doca_error_get_descr(result));

	/* Get PCI function type */
	result = doca_devinfo_rep_get_pci_func_type(devinfo_rep, &pci_func_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_DBG("Failed to get PCI function type: %s", doca_error_get_descr(result));
		pci_func_type = DOCA_PCI_FUNC_TYPE_SF; /* Assume SF for vnet */
	}

	/* Map type to string */
	const char *type_str = (pci_func_type == DOCA_PCI_FUNC_TYPE_PF) ? "PF" :
			       (pci_func_type == DOCA_PCI_FUNC_TYPE_VF) ? "VF" :
			       (pci_func_type == DOCA_PCI_FUNC_TYPE_SF) ? "SF" :
									  "Unknown";

	/* Display SF representor info */
	DOCA_LOG_INFO("============================");
	DOCA_LOG_INFO("SF Representor Info:");
	DOCA_LOG_INFO("============================");
	DOCA_LOG_INFO("  Type: %s", type_str);
	DOCA_LOG_INFO("  VHCA ID: 0x%04X", vhca_id);
	DOCA_LOG_INFO("  Interface Index: %u", ifindex);

	/* For SF type, also get SF index */
	if (pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
		result = doca_devinfo_rep_get_sf_index(devinfo_rep, &sf_index);
		if (result == DOCA_SUCCESS)
			DOCA_LOG_INFO("  SF Index: %u", sf_index);
		else
			DOCA_LOG_DBG("  SF Index: N/A (err: %s)", doca_error_get_descr(result));
	}
	DOCA_LOG_INFO("============================");

	return DOCA_SUCCESS;
}

static doca_error_t vnet_pci_dev_vnet_controller_init(struct vnet_pci_dev_resources *resources,
						      struct vnet_pci_dev_config *config)
{
	doca_error_t result;

	/* Initialize all controllers in the array with atomic variables.
	 * Note: stats_ref_mutex is already initialized in init_tlp_context() immediately
	 * after allocation, so tlp_ctx_cleanup() can safely destroy it on any failure path.
	 */
	for (uint32_t i = 0; i < resources->tlp_ctx->num_ep; i++) {
		struct vnet_pci_dev_controller *ctrl = &resources->tlp_ctx->vnet_controller[i];
		atomic_init(&ctrl->virtio_device, NULL);
		atomic_init(&ctrl->shutting_down, false);
		atomic_init(&ctrl->stop_stats_collection, false);
		atomic_init(&ctrl->cleanup_running, false);
		atomic_init(&ctrl->initialization_in_progress, false);
		atomic_init(&ctrl->reset_status_state, VNET_RESET_STATUS_IDLE);
		atomic_init(&ctrl->deferred_mq.first_ctrl_req_delay_done, false);
		atomic_init(&ctrl->offload_engine_started, false);
		atomic_init(&ctrl->vqs_initialized, false);
		atomic_init(&ctrl->cvq_bound, false);
		atomic_init(&ctrl->engine_enabled, false);
		atomic_init(&ctrl->num_active_qps, 0);
		atomic_init(&ctrl->dbg_state.stats_in_progress, false);
		atomic_init(&ctrl->dbg_state.counters_in_progress, false);
		atomic_init(&ctrl->dbg_state.last_collection_time, 0);
		atomic_init(&ctrl->stats_ref, NULL);
		atomic_init(&ctrl->deferred_mq.initial_data_qps_deferred, false);
		atomic_init(&ctrl->deferred_mq.start_deferred, false);
		atomic_init(&ctrl->deferred_mq.old_qps, 0);
		atomic_init(&ctrl->deferred_mq.new_qps, 0);
		/* worker_pe will be set after this function returns */
	}

	/* Initialize VNet subsystem */
	result = doca_devemu_vnet_add_dev(resources->tlp_ctx->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to VNet subsystem: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_vnet_init();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize VNet subsystem: %s", doca_error_get_descr(result));
		goto rm_dev;
	}

	/* Parse MAC address from config->mac_addr */
	result = parse_mac_address(config->mac_addr, resources->tlp_ctx->mac_bytes_base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse MAC address '%s': %s", config->mac_addr, doca_error_get_descr(result));
		goto teardown_vnet;
	}
	resources->tlp_ctx->max_queue_pairs = config->max_queue_pairs;
	resources->tlp_ctx->queue_size = config->queue_size;
	resources->tlp_ctx->mtu = config->mtu;

	DOCA_LOG_INFO("VNet controller init - waiting for host configuration");
	return DOCA_SUCCESS;

teardown_vnet:
	doca_devemu_vnet_teardown();
rm_dev:
	doca_devemu_vnet_rm_dev(resources->tlp_ctx->dev);
	return result;
}

/**
 * @brief Destroy VNet controller with proper error handling
 *
 * Destroys VNet controller using the library's intended workflow with
 * proper error handling and API contract compliance.
 *
 * @param[in] resources Application resources
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t vnet_pci_dev_vnet_controller_uninit(struct vnet_pci_dev_resources *resources)
{
	/* With dual PE architecture, IO contexts are destroyed immediately in cleanup,
	 * so no pending contexts to handle here. */

	/* Cleanup VNet subsystem */
	doca_devemu_vnet_teardown();
	doca_devemu_vnet_rm_dev(resources->tlp_ctx->dev);

	return DOCA_SUCCESS;
}

static doca_error_t vnet_pci_dev_vnet_controller_create(struct tlp_context *tlp_ctx, struct pci_device_config *endpoint)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	struct vnet_pci_dev_controller *controller;
	struct doca_devemu_pci_ep *pci_ep;
	const void *import_desc = NULL;
	size_t import_desc_len = 0;
	bool imported_offload = false;
	doca_error_t destroy_result;
	doca_error_t result;
	uint16_t total_vqs;
	uint32_t pf_index;
	uint8_t mac_bytes[ETH_ALEN];

	doca_error_t ret = get_pf_index_for_device(tlp_ctx, endpoint, &pf_index);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PF index for device %02x:%02x.%x",
			     endpoint->bus,
			     endpoint->device,
			     endpoint->function);
		return DOCA_ERROR_EMPTY;
	}

	controller = &tlp_ctx->vnet_controller[pf_index];
	/* Initialize atomic flags */
	atomic_store(&controller->shutting_down, false);
	atomic_store(&controller->stop_stats_collection, false);
	/* Initialize dbg_state atomics */
	atomic_store(&controller->dbg_state.stats_in_progress, false);
	atomic_store(&controller->dbg_state.counters_in_progress, false);
	atomic_store(&controller->dbg_state.last_collection_time, 0);
	atomic_store(&controller->stats_ref, NULL);
	/* Link to existing components */
	atomic_store(&controller->virtio_device, &tlp_ctx->virtio_dev[pf_index]);
	controller->tlp_ctx = tlp_ctx;
	pci_ep = doca_devemu_pci_tlp_dev_as_ep(endpoint->tlp_dev);

	result = vnet_lu_get_import_desc(pf_index, &import_desc, &import_desc_len);
	if (result == DOCA_SUCCESS) {
		result = doca_devemu_vnet_offload_engine_create_from_export(import_desc,
									    import_desc_len,
									    pci_ep,
									    &controller->offload_engine);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create VNet offload engine from export: %s",
				     doca_error_get_descr(result));
			return result;
		}
		imported_offload = true;
		DOCA_LOG_INFO("VNet offload engine created from export blob (%zu bytes)", import_desc_len);
	} else {
		result = doca_devemu_vnet_offload_engine_create(pci_ep, &controller->offload_engine);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create VNet offload engine: %s", doca_error_get_descr(result));
			return result;
		}
	}

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(controller->offload_engine);
	if (!virtio_engine) {
		DOCA_LOG_ERR("Failed to get VirtIO offload engine from VNet offload engine");
		result = DOCA_ERROR_UNEXPECTED;
		goto destroy_offload_engine;
	}

	if (!imported_offload) {
		result = doca_devemu_vnet_offload_engine_set_mtu(controller->offload_engine, tlp_ctx->mtu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set MTU to %d: %s", tlp_ctx->mtu, doca_error_get_descr(result));
			goto destroy_offload_engine;
		}
		DOCA_LOG_INFO("VNet offload engine MTU configured: %d bytes", tlp_ctx->mtu);

		memcpy(mac_bytes, tlp_ctx->mac_bytes_base, ETH_ALEN);
		mac_bytes[5] += pf_index;
		result = doca_devemu_vnet_offload_engine_set_mac(controller->offload_engine, mac_bytes);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set MAC address: %s", doca_error_get_descr(result));
			goto destroy_offload_engine;
		}
		DOCA_LOG_INFO("VNet offload engine MAC configured: %02x:%02x:%02x:%02x:%02x:%02x",
			      mac_bytes[0],
			      mac_bytes[1],
			      mac_bytes[2],
			      mac_bytes[3],
			      mac_bytes[4],
			      mac_bytes[5]);

		total_vqs = tlp_ctx->max_queue_pairs * 2 + 1;
		result = doca_devemu_virtio_offload_engine_set_num_queues(virtio_engine, total_vqs);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set number of queues: %s", doca_error_get_descr(result));
			goto destroy_offload_engine;
		}
		DOCA_LOG_INFO("VNet offload engine with %d VQs (%d QPs), queue_size=%u",
			      total_vqs,
			      tlp_ctx->max_queue_pairs,
			      tlp_ctx->queue_size);
	} else {
		DOCA_LOG_INFO("VNet offload engine imported: MTU, MAC, num_queues already set from export blob");
	}

	/* Engine will be started on FIRST FEATURES_OK (not during init) */
	atomic_init(&controller->offload_engine_started, false);
	atomic_init(&controller->vqs_initialized, false);
	atomic_init(&controller->cvq_bound, false);
	atomic_init(&controller->engine_enabled, false);
	atomic_init(&controller->num_active_qps, 0);
	atomic_init(&controller->cleanup_running, false);
	atomic_init(&controller->initialization_in_progress, false);
	atomic_init(&controller->reset_status_state, VNET_RESET_STATUS_IDLE);
	atomic_init(&controller->deferred_mq.first_ctrl_req_delay_done, false);
	controller->deferred_init_pending = false;
	atomic_init(&controller->deferred_mq.initial_data_qps_deferred, false);
	atomic_init(&controller->deferred_mq.start_deferred, false);
	atomic_init(&controller->deferred_mq.old_qps, 0);
	atomic_init(&controller->deferred_mq.new_qps, 0);
	DOCA_LOG_INFO("VNet offload engine created (will start on FEATURES_OK)");

	/* Initialize max_queue_pairs from config
	 * This prevents false "queue count changed" detection on first FEATURES_OK */
	controller->max_queue_pairs = tlp_ctx->max_queue_pairs;
	DOCA_LOG_DBG("Controller initialized with max_queue_pairs=%d", controller->max_queue_pairs);
	/* Allocate VQ pointer arrays dynamically based on configured max_queue_pairs */
	controller->rx_vqs = calloc(controller->max_queue_pairs, sizeof(struct doca_devemu_vnet_rx_vq *));
	if (!controller->rx_vqs) {
		DOCA_LOG_ERR("Failed to allocate RX VQ array for %d queue pairs", controller->max_queue_pairs);
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_engine;
	}
	controller->tx_vqs = calloc(controller->max_queue_pairs, sizeof(struct doca_devemu_vnet_tx_vq *));
	if (!controller->tx_vqs) {
		DOCA_LOG_ERR("Failed to allocate TX VQ array for %d queue pairs", controller->max_queue_pairs);
		result = DOCA_ERROR_NO_MEMORY;
		goto free_rx_vqs;
	}
	/* Query SF representor info immediately after engine creation */
	result = vnet_controller_query_and_display_rep_info(controller);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to query SF rep info early: %s (will retry later)", doca_error_get_descr(result));
		/* Non-fatal - continue with initialization */
	}

	DOCA_LOG_DBG("Allocated VQ arrays: max_qp=%d, rx=%zu bytes, tx=%zu bytes",
		     controller->max_queue_pairs,
		     controller->max_queue_pairs * sizeof(void *),
		     controller->max_queue_pairs * sizeof(void *));

	/* Set time value to current time to enforce 5-second delay before first
	 * stats/counters collection (instead of immediate trigger). */
	atomic_store(&controller->dbg_state.last_collection_time, time(NULL));

	DOCA_LOG_INFO("VNet controller created - waiting for host configuration");
	return DOCA_SUCCESS;

free_rx_vqs:
	free(controller->rx_vqs);
	controller->rx_vqs = NULL;
destroy_engine:
destroy_offload_engine:
	/* Engine was just created and should not be started - destroy must succeed */
	destroy_result = doca_devemu_vnet_offload_engine_destroy(controller->offload_engine);
	if (destroy_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Engine destroy failed during creation cleanup - library bug!");
	}

	return result;
}

/**
 * @brief Destroy VNet controller with proper error handling
 *
 * Destroys VNet controller using the library's intended workflow with
 * proper error handling and API contract compliance.
 *
 * @param[in] resources Application resources
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t vnet_pci_dev_vnet_controller_destroy(struct tlp_context *tlp_ctx,
							 struct pci_device_config *endpoint)
{
	struct vnet_pci_dev_controller *controller;
	doca_error_t result;
	uint32_t pf_index;

	if (!tlp_ctx || !endpoint) {
		DOCA_LOG_ERR("Invalid parameters: tlp_ctx or endpoint is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	doca_error_t ret = get_pf_index_for_device(tlp_ctx, endpoint, &pf_index);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PF index for device %02x:%02x.%x",
			     endpoint->bus,
			     endpoint->device,
			     endpoint->function);
		return DOCA_ERROR_EMPTY;
	}
	controller = &tlp_ctx->vnet_controller[pf_index];
	/* Defensive NULL check to satisfy -Werror=null-dereference when function is inlined.
	 * In practice, &array[index] never returns NULL for a valid array, but GCC's
	 * static analysis can't prove this across inlined function boundaries. */
	if (controller == NULL) {
		DOCA_LOG_ERR("Controller is NULL for pf_index %u (should never happen)", pf_index);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = vnet_pci_dev_shutdown_device(controller);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("VNet device shutdown failed: %s", doca_error_get_descr(result));
		/* If cleanup was incomplete (stats/counters resources not destroyed),
		 * do NOT proceed to free the controller - this would leak resources
		 * and violate teardown ordering. Return error to caller. */
		if (result == DOCA_ERROR_IN_PROGRESS) {
			DOCA_LOG_ERR("CRITICAL: Controller cleanup incomplete - cannot free. "
				     "Resources (vnet_counters/stats_ref) may still be in use.");
			return result;
		}
		/* For other errors, continue with best-effort cleanup */
	}

	/* Free dynamically allocated VQ pointer arrays */
	if (controller->rx_vqs) {
		free(controller->rx_vqs);
		controller->rx_vqs = NULL;
	}
	if (controller->tx_vqs) {
		free(controller->tx_vqs);
		controller->tx_vqs = NULL;
	}

	/* Clear virtio_device pointer to prevent access after destroy
	 * (progress loop checks this to skip destroyed devices) */
	atomic_store(&controller->virtio_device, NULL);

	/* Note: Do NOT destroy stats_ref_mutex here.
	 * Controllers are stored in a persistent array (tlp_ctx->vnet_controller[])
	 * and can be reused on hotplug re-plug. The mutex is init-once in
	 * vnet_pci_dev_vnet_controller_init() and destroy-once in
	 * vnet_pci_dev_vnet_controller_uninit() when the array is freed. */

	if (result == DOCA_SUCCESS) {
		DOCA_LOG_INFO("VNet controller destroyed successfully");
	} else {
		DOCA_LOG_WARN("VNet controller destroyed with errors");
	}

	return result;
}

/*********************************************************************************************************************
 * IO Context Management for CVQ Command Handling
 *
 * The IO context receives ctrl_req events from the backend when the host sends control commands
 * via CVQ (e.g., ethtool -L for MQ configuration).
 *
 * Flow: Host CVQ cmd → DPA → backend → IO context callback → app handler
 *********************************************************************************************************************/

/**
 * @brief Stop extra queue pairs when MQ decreases num_qps
 *
 * Design pattern:
 *   1. doca_devemu_virtio_vq_group_disable() - disable VQs in group
 *   2. doca_devemu_virtio_vq_stop(rx) - stop each RX VQ
 *   3. doca_devemu_virtio_vq_stop(tx) - stop each TX VQ
 *
 * @param[in] ctrl Controller context
 * @param[in] old_qps Current number of active QPs
 * @param[in] new_qps Target number of active QPs (new_qps < old_qps)
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t stop_extra_queue_pairs(struct vnet_pci_dev_controller *ctrl, uint16_t old_qps, uint16_t new_qps)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	uint16_t start_vq_idx, end_vq_idx;
	struct doca_devemu_virtio_vq *vq;
	doca_error_t result;
	uint16_t i;

	/* Get the virtio offload engine for VQ group operations */
	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine);
	if (!virtio_engine) {
		DOCA_LOG_ERR("Failed to get virtio offload engine");
		return DOCA_ERROR_UNEXPECTED;
	}

	/* Calculate VQ index range for QPs [new_qps, old_qps)
	 * Each QP has 2 VQs: RX (even) and TX (odd)
	 */
	start_vq_idx = new_qps * 2;
	end_vq_idx = old_qps * 2 - 1;

	DOCA_LOG_INFO("Stopping QPs %u-%u (VQs %u-%u)", new_qps, old_qps - 1, start_vq_idx, end_vq_idx);

	/* Step 1: Group disable - disable all VQs in range */
	result = doca_devemu_virtio_vq_group_disable(virtio_engine, start_vq_idx, end_vq_idx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to disable VQ group [%u-%u]: %s",
			     start_vq_idx,
			     end_vq_idx,
			     doca_error_get_descr(result));
		return result;
	}

	/* Step 2: Stop individual VQs */
	for (i = new_qps; i < old_qps; i++) {
		/* Stop RX VQ */
		if (ctrl->rx_vqs[i]) {
			vq = doca_devemu_vnet_rx_vq_as_vq(ctrl->rx_vqs[i]);
			result = doca_devemu_virtio_vq_stop(vq);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop RX VQ for QP%u: %s", i, doca_error_get_descr(result));
				return result;
			}
		}

		/* Stop TX VQ */
		if (ctrl->tx_vqs[i]) {
			vq = doca_devemu_vnet_tx_vq_as_vq(ctrl->tx_vqs[i]);
			result = doca_devemu_virtio_vq_stop(vq);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop TX VQ for QP%u: %s", i, doca_error_get_descr(result));
				return result;
			}
		}
		DOCA_LOG_DBG("Stopped QP%u", i);
	}

	DOCA_LOG_INFO("Successfully stopped QPs %u-%u", new_qps, old_qps - 1);
	return DOCA_SUCCESS;
}

/**
 * @brief Start additional queue pairs when MQ increases num_qps
 *
 * Design pattern:
 *   1. doca_devemu_virtio_vq_start(rx) - start each RX VQ first
 *   2. doca_devemu_virtio_vq_start(tx) - start each TX VQ first
 *   3. doca_devemu_virtio_vq_group_enable() - then enable all started VQs in group
 *
 * @param[in] ctrl Controller context
 * @param[in] old_qps Current number of active QPs
 * @param[in] new_qps Target number of active QPs (new_qps > old_qps)
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t start_additional_queue_pairs(struct vnet_pci_dev_controller *ctrl,
						 uint16_t old_qps,
						 uint16_t new_qps)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	const struct vnet_virtio_queue_config *vqs;
	struct vnet_pci_device *dev;
	uint16_t start_vq_idx, end_vq_idx;
	struct doca_devemu_virtio_vq *vq;
	doca_error_t result;
	uint16_t i;

	/* Get the virtio offload engine for VQ group operations */
	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine);
	if (!virtio_engine) {
		DOCA_LOG_ERR("Failed to get virtio offload engine");
		return DOCA_ERROR_UNEXPECTED;
	}

	/* Get VQ configuration from device */
	dev = atomic_load(&ctrl->virtio_device);
	if (dev == NULL) {
		DOCA_LOG_WARN("start_additional_queue_pairs: no active VirtIO device");
		return DOCA_ERROR_BAD_STATE;
	}
	vqs = vnet_pci_device_get_virtq_pci_cfg(dev);

	/* Calculate VQ index range for QPs [old_qps, new_qps)
	 * Each QP has 2 VQs: RX (even) and TX (odd)
	 */
	start_vq_idx = old_qps * 2;
	end_vq_idx = new_qps * 2 - 1;

	DOCA_LOG_INFO("Starting QPs %u-%u (VQs %u-%u)", old_qps, new_qps - 1, start_vq_idx, end_vq_idx);

	/* Step 1: Start individual VQs FIRST */
	for (i = old_qps; i < new_qps; i++) {
		uint16_t rx_vq_idx = i * 2;
		uint16_t tx_vq_idx = i * 2 + 1;

		/* Check if device reset was requested - bail out early.
		 * The reset path sets cancel_in_progress before waiting on the
		 * per-device barrier. This bounds the maximum barrier wait to one
		 * VQ start operation (~25-50ms) instead of all QPs (seconds).
		 * Partially-started VQs are safe: the subsequent device reset and
		 * controller cleanup will tear them down properly. */
		if (atomic_load(&dev->cancel_in_progress)) {
			DOCA_LOG_INFO("start_additional_queue_pairs: cancelled at QP%u/%u due to device reset",
				      i,
				      new_qps);
			return DOCA_ERROR_IN_PROGRESS;
		}

		/* Start RX VQ */
		if (ctrl->rx_vqs[i]) {
			vq = doca_devemu_vnet_rx_vq_as_vq(ctrl->rx_vqs[i]);

			/* Apply VQ configuration from host TLP writes */
			result = doca_devemu_virtio_vq_set_conf(vq,
								rx_vq_idx,
								vqs[rx_vq_idx].queue_size,
								vqs[rx_vq_idx].queue_msix_vector,
								vqs[rx_vq_idx].queue_desc,
								vqs[rx_vq_idx].queue_driver,
								vqs[rx_vq_idx].queue_device);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to configure RX VQ for QP%u: %s", i, doca_error_get_descr(result));
				return result;
			}

			result = doca_devemu_virtio_vq_start(vq);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to start RX VQ for QP%u: %s", i, doca_error_get_descr(result));
				return result;
			}
		}

		/* Start TX VQ */
		if (ctrl->tx_vqs[i]) {
			vq = doca_devemu_vnet_tx_vq_as_vq(ctrl->tx_vqs[i]);

			/* Apply VQ configuration from host TLP writes */
			result = doca_devemu_virtio_vq_set_conf(vq,
								tx_vq_idx,
								vqs[tx_vq_idx].queue_size,
								vqs[tx_vq_idx].queue_msix_vector,
								vqs[tx_vq_idx].queue_desc,
								vqs[tx_vq_idx].queue_driver,
								vqs[tx_vq_idx].queue_device);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to configure TX VQ for QP%u: %s", i, doca_error_get_descr(result));
				return result;
			}

			result = doca_devemu_virtio_vq_start(vq);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to start TX VQ for QP%u: %s", i, doca_error_get_descr(result));
				return result;
			}
		}
		DOCA_LOG_DBG("Started QP%u", i);

		/* Yield after each QP to avoid starving PE1's TLP processing.
		 *
		 * Each doca_devemu_virtio_vq_start() sends synchronous firmware
		 * commands. When multiple MQ start threads run in parallel (up to
		 * MQ_START_MAX_CONCURRENT), their tight VQ start loops can saturate
		 * the firmware command channel, preventing PE1 from processing
		 * host TLP reads/writes for other devices still being enumerated.
		 *
		 * Without this yield: 2 threads x 126 QPs x 2 VQ starts = 504
		 * back-to-back firmware commands. PE1 gets zero bandwidth -> host
		 * TLP writes time out -> PCIe Completion Timeout -> host hang.
		 *
		 * With 1ms yield per QP: ~126ms overhead per device (acceptable
		 * given the 8s total), but PE1 gets regular processing windows. */
		usleep(1000);
	}

	/* Step 2: Group enable - enable all VQs in range AFTER starting */
	result = doca_devemu_virtio_vq_group_enable(virtio_engine, start_vq_idx, end_vq_idx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enable VQ group [%u-%u]: %s",
			     start_vq_idx,
			     end_vq_idx,
			     doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Successfully started QPs %u-%u", old_qps, new_qps - 1);
	return DOCA_SUCCESS;
}

/**
 * @brief Handle VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET command
 *
 * Guest requests to change number of active queue pairs.
 *
 * @param[in] ctrl Controller context
 * @param[in] req ctrl_req with command data
 * @return VIRTIO_NET_OK on success, VIRTIO_NET_ERR on failure
 */
static uint8_t handle_mq_vq_pairs_set(struct vnet_pci_dev_controller *ctrl, struct doca_devemu_vnet_ctrl_req *req)
{
	uint16_t old_qps = atomic_load(&ctrl->num_active_qps);
	struct doca_buf *data_buf;
	doca_error_t result;
	uint32_t data_len;
	uint16_t new_qps;
	void *data_ptr;

	data_len = doca_devemu_vnet_ctrl_req_get_data_len(req);

	if (data_len < sizeof(uint16_t)) {
		DOCA_LOG_ERR("MQ_VQ_PAIRS_SET: data too short (%u bytes)", data_len);
		return VIRTIO_NET_ERR;
	}

	/* Get data buffer from request */
	data_buf = doca_devemu_vnet_ctrl_req_get_data(req);
	if (!data_buf) {
		DOCA_LOG_ERR("MQ_VQ_PAIRS_SET: no data buffer");
		return VIRTIO_NET_ERR;
	}

	/* Extract num_qps from doca_buf */
	result = doca_buf_get_data(data_buf, &data_ptr);
	if (result != DOCA_SUCCESS || !data_ptr) {
		DOCA_LOG_ERR("MQ_VQ_PAIRS_SET: failed to get data from buffer: %s", doca_error_get_descr(result));
		return VIRTIO_NET_ERR;
	}

	new_qps = *(uint16_t *)data_ptr;

	/* Validate new_qps */
	if (new_qps == 0 || new_qps > ctrl->max_queue_pairs) {
		DOCA_LOG_ERR("Invalid num_qps: %u (valid: 1-%u)", new_qps, ctrl->max_queue_pairs);
		return VIRTIO_NET_ERR;
	}

	DOCA_LOG_INFO("MQ_VQ_PAIRS_SET: %u -> %u QPs", old_qps, new_qps);

	if (vnet_controller_should_defer_initial_data_qps(ctrl) && old_qps != 0)
		old_qps = 0;

	if (new_qps == old_qps) {
		vnet_controller_clear_deferred_mq_start(ctrl);
		DOCA_LOG_DBG("num_qps unchanged");
		return VIRTIO_NET_OK;
	}

	if (new_qps < old_qps) {
		vnet_controller_clear_deferred_mq_start(ctrl);
		/* Decreasing: disable + stop extra QPs (synchronous - fast operation) */
		result = stop_extra_queue_pairs(ctrl, old_qps, new_qps);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop extra queue pairs");
			return VIRTIO_NET_ERR;
		}
		atomic_store(&ctrl->num_active_qps, new_qps);
		atomic_store(&ctrl->deferred_mq.initial_data_qps_deferred, false);
		return VIRTIO_NET_OK;
	}

	/* Increasing QPs: complete the CVQ response IMMEDIATELY, defer the
	 * detached MQ start until static enumeration has settled, then release
	 * all deferred MQ starts together.
	 *
	 * start_additional_queue_pairs() is a blocking operation that takes ~65ms per QP
	 * (two VQ starts per QP). For 127 QPs this totals ~8.2 seconds.
	 *
	 * CRITICAL: The host's virtnet_send_command() busy-polls waiting for the CVQ
	 * response. If we defer the response until QPs finish, the host CPU is stuck
	 * for 8s per device, and the host kernel probes devices serially (each probe
	 * blocks on work_for_cpu_fn). With 9 devices this means 72s of cumulative
	 * blocking, triggering soft lockups.
	 *
	 * By completing the ctrl_req immediately:
	 * 1. Host gets instant CVQ response -> virtnet_send_command returns
	 * 2. Host can proceed to probe the next device immediately
	 * 3. The deferred path delays MQ expansion until every static PF is fully live
	 * 4. Once enumeration settles, detached threads start the extra QPs
	 *
	 * Safety: The host driver just records the queue count after this command.
	 * The extra queues' desc/driver/device addresses are already written by the
	 * host before sending this command. Actual data traffic on extra queues
	 * only starts later, by which time the deferred async QP start should have
	 * completed.
	 */
	if (!vnet_tlp_ctx_all_controllers_ready_for_mq_start(ctrl->tlp_ctx)) {
		vnet_controller_set_deferred_mq_start(ctrl, old_qps, new_qps);
		return VIRTIO_NET_OK;
	}

	vnet_controller_clear_deferred_mq_start(ctrl);
	pci_cfg_workqueue_submit_mq_start_qps(ctrl, old_qps, new_qps);

	/* Return VIRTIO_NET_OK - the ctrl_req is completed here, not deferred.
	 * The detached thread will start QPs in the background. */
	return VIRTIO_NET_OK;
}

doca_error_t vnet_pci_dev_execute_mq_start_qps(struct vnet_pci_dev_controller *controller,
					       uint16_t old_qps,
					       uint16_t new_qps)
{
	doca_error_t result;

	DOCA_LOG_INFO("Background thread: starting MQ QPs %u -> %u", old_qps, new_qps);

	result = start_additional_queue_pairs(controller, old_qps, new_qps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Background thread: MQ start QPs failed: %s", doca_error_get_descr(result));
	} else {
		atomic_store(&controller->num_active_qps, new_qps);
		atomic_store(&controller->deferred_mq.initial_data_qps_deferred, false);
		vnet_controller_maybe_submit_full_stats_list(controller);
		DOCA_LOG_INFO("Background thread: MQ start QPs %u -> %u completed successfully", old_qps, new_qps);
	}

	/* NOTE: ctrl_req was already completed with VIRTIO_NET_OK in
	 * handle_mq_vq_pairs_set() BEFORE this thread was spawned.
	 * This prevents the host from busy-polling in virtnet_send_command()
	 * and allows parallel device probing. */
	return result;
}

/**
 * @brief ctrl_req event callback handler
 *
 * Called when ctrl_req is forwarded from backend via CVQ.
 * Handles MQ commands for dynamic queue pair management.
 *
 * @param[in] req ctrl_req object containing command data
 * @param[in] cls VirtIO control command class
 * @param[in] cmd VirtIO control command
 * @param[in] user_data User context (controller pointer)
 */
void vnet_pci_dev_ctrl_req_handler(struct doca_devemu_vnet_ctrl_req *req, uint8_t cls, uint8_t cmd, void *user_data)
{
	struct vnet_pci_dev_controller *ctrl = NULL;
	uint8_t ack = VIRTIO_NET_OK;
	(void)user_data;

	ctrl = (struct vnet_pci_dev_controller *)doca_devemu_vnet_ctrl_req_get_vq_user_data(req);
	if (ctrl == NULL) {
		DOCA_LOG_ERR("Failed to get Controller from VQ user data");
		doca_devemu_vnet_ctrl_req_complete(req, VIRTIO_NET_ERR, 0);
		return;
	}

	DOCA_LOG_INFO("ctrl_req received: cls=%u, cmd=%u, controller=%p", cls, cmd, (void *)ctrl);

	/* During deferred initial bring-up, let the first CVQ request wait once
	 * for the enabled offload engine to become fully live. */
	if (atomic_load(&ctrl->deferred_mq.initial_data_qps_deferred) &&
	    !atomic_exchange(&ctrl->deferred_mq.first_ctrl_req_delay_done, true))
		usleep(VNET_CTRL_REQ_DELAY_USEC);

	switch (cls) {
	case VIRTIO_NET_CTRL_MQ:
		switch (cmd) {
		case VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET:
			ack = handle_mq_vq_pairs_set(ctrl, req);
			break;
		default:
			DOCA_LOG_WARN("Unsupported MQ cmd: %u", cmd);
			ack = VIRTIO_NET_ERR;
		}
		break;

	case VIRTIO_NET_CTRL_MAC:
		/* MAC commands handled by backend */
		DOCA_LOG_INFO("MAC ctrl cmd: %u (handled by backend)", cmd);
		ack = VIRTIO_NET_OK;
		break;

	case VIRTIO_NET_CTRL_RX:
		/* RX mode commands handled by backend */
		DOCA_LOG_INFO("RX ctrl cmd: %u (handled by backend)", cmd);
		ack = VIRTIO_NET_OK;
		break;

	case VIRTIO_NET_CTRL_VLAN:
		/* VLAN commands handled by backend */
		DOCA_LOG_INFO("VLAN ctrl cmd: %u (handled by backend)", cmd);
		ack = VIRTIO_NET_OK;
		break;

	default:
		DOCA_LOG_WARN("Unsupported ctrl class: %u", cls);
		ack = VIRTIO_NET_ERR;
	}

	/* Complete the request - backend will read completion_ack */
	doca_devemu_vnet_ctrl_req_complete(req, ack, sizeof(uint8_t));
}

doca_error_t vnet_pci_dev_initialize_io_context(struct vnet_pci_dev_controller *controller)
{
	struct doca_devemu_virtio_io *virtio_io;
	struct doca_ctx *ctx;
	doca_error_t result;

	if (!controller->offload_engine) {
		DOCA_LOG_WARN("Offload engine destroyed - skipping IO context init (stale work item)");
		return DOCA_ERROR_BAD_STATE;
	}

	if (atomic_load(&controller->cleanup_running)) {
		DOCA_LOG_WARN("Cleanup in progress - skipping IO context init");
		return DOCA_ERROR_BAD_STATE;
	}

	if (!controller->mq_feature_negotiated) {
		DOCA_LOG_DBG("MQ not negotiated - skipping IO context initialization");
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("Initializing IO context for CVQ command handling");

	/* Step 1: Create IO context from offload engine */
	result = doca_devemu_vnet_io_create_from_offload_engine(controller->offload_engine, &controller->io_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IO context: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_INFO("IO context created: %p", (void *)controller->io_ctx);

	virtio_io = doca_devemu_vnet_io_as_virtio_io(controller->io_ctx);
	if (!virtio_io) {
		DOCA_LOG_ERR("Failed to get virtio io handle");
		goto destroy_io_ctx;
	}

	ctx = doca_devemu_virtio_io_as_ctx(virtio_io);
	if (!ctx) {
		DOCA_LOG_ERR("Failed to get ctx handle");
		goto destroy_io_ctx;
	}

	/* Connect IO context to worker PE (PE2) instead of main PE (PE1).
	 * This allows IO context cleanup to happen on worker thread without
	 * blocking the main thread's TLP handling. */
	result = doca_pe_connect_ctx(controller->worker_pe, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach IO context to worker PE: %s", doca_error_get_descr(result));
		goto destroy_io_ctx;
	}
	DOCA_LOG_INFO("IO context attached to worker PE=%p, controller=%p",
		      (void *)controller->worker_pe,
		      (void *)controller);

	/* Step 2: Register ctrl_req handler with IO context.
	 * Per the DOCA API contract, the handler must be registered while the IO ctx is idle
	 * (i.e. before doca_ctx_start()). */
	result = doca_devemu_vnet_io_event_vnet_ctrl_req_register(controller->io_ctx, vnet_pci_dev_ctrl_req_handler);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ctrl_req handler: %s", doca_error_get_descr(result));
		goto destroy_io_ctx;
	}
	DOCA_LOG_INFO("ctrl_req handler registered for io_ctx=%p", (void *)controller->io_ctx);

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start IO context: %s", doca_error_get_descr(result));
		goto destroy_io_ctx;
	}
	DOCA_LOG_INFO("IO context started, io_ctx=%p", (void *)controller->io_ctx);

	controller->io_ctx_started = true;

	/* Signal that async initialization from FEATURES_OK is complete.
	 * Deferred MQ readiness checks use this flag as part of controller readiness. */
	atomic_store(&controller->initialization_in_progress, false);

	DOCA_LOG_INFO("IO context initialization complete");
	/*
	 * Do NOT re-submit start_and_enable here. The DRIVER_OK handler in
	 * virtio_net_ctrl_change_cb() already submits it, and the worker FIFO
	 * guarantees it runs strictly after the init_vqs / init_io_context work
	 * items that were queued by the preceding FEATURES_OK.
	 *
	 * Re-submitting from this tail path created a second start_and_enable
	 * that re-ran on stale state (e.g. after the device had been torn down
	 * by a pending unplug), producing "stale work item" warnings and
	 * occasional re-enable attempts against a destroyed offload engine.
	 */
	return DOCA_SUCCESS;

destroy_io_ctx:
	doca_devemu_vnet_io_destroy(controller->io_ctx);
	controller->io_ctx = NULL;
	/* Also clear on error path so readiness state is not left stuck. */
	atomic_store(&controller->initialization_in_progress, false);
	return result;
}

/*********************************************************************************************************************
 * Config Change MSI-X Functions
 *********************************************************************************************************************/

doca_error_t vnet_ensure_config_msix(struct vnet_pci_dev_controller *controller)
{
	struct vnet_pci_device *dev;
	struct vnet_virtio_common_config *pci_cfg;
	uint16_t live_vector;

	if (controller == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	dev = atomic_load(&controller->virtio_device);
	if (dev == NULL)
		return DOCA_ERROR_BAD_STATE;

	pci_cfg = vnet_pci_device_get_pci_cfg(dev);
	live_vector = pci_cfg->config_msix_vector;

	/* If MSI-X exists but the host assigned a different vector, destroy the stale one */
	if (controller->config_msix != NULL && controller->config_msix_vector_cached != live_vector) {
		DOCA_LOG_INFO("Config MSI-X vector changed (%u -> %u), destroying old MSI-X",
			      controller->config_msix_vector_cached,
			      live_vector);
		doca_error_t result = doca_devemu_pci_msix_destroy(controller->config_msix);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy stale config MSI-X: %s", doca_error_get_descr(result));
			return result;
		}
		controller->config_msix = NULL;
	}

	/* If no MSI-X exists and the host assigned a valid vector, create one */
	if (controller->config_msix == NULL && live_vector != VIRTIO_MSI_NO_VECTOR) {
		struct pci_device_config *endpoint =
			&controller->tlp_ctx->devs_config[FIRST_PF_IDX(controller->tlp_ctx) + dev->pf_index];
		struct doca_devemu_pci_ep *pci_ep = doca_devemu_pci_tlp_dev_as_ep(endpoint->tlp_dev);

		doca_error_t result = doca_devemu_pci_ep_create_msix(pci_ep,
								     VNET_VIRTIO_BAR_ID,
								     VNET_VIRTIO_MSIX_TABLE_OFFSET,
								     live_vector,
								     &controller->config_msix);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create config MSI-X (vector=%u): %s",
				     live_vector,
				     doca_error_get_descr(result));
			return result;
		}
		controller->config_msix_vector_cached = live_vector;
		DOCA_LOG_INFO("Config MSI-X created (vector=%u) - ready for config change notifications", live_vector);
	}

	if (controller->config_msix == NULL) {
		DOCA_LOG_DBG("Config MSI-X not available (vector=0x%04x)", live_vector);
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_dev_execute_speed_change(struct vnet_pci_dev_controller *controller, uint32_t new_speed)
{
	struct vnet_pci_device *dev;
	struct vnet_virtio_common_config *pci_cfg;
	struct vnet_virtio_net_config *vnet_cfg;

	if (controller == NULL) {
		DOCA_LOG_ERR("Speed change: invalid controller or device");
		return DOCA_ERROR_INVALID_VALUE;
	}

	dev = atomic_load(&controller->virtio_device);
	if (dev == NULL) {
		DOCA_LOG_ERR("Speed change: invalid controller or device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	pci_cfg = vnet_pci_device_get_pci_cfg(dev);
	vnet_cfg = &dev->vnet_cfg;

	/* Ensure config MSI-X is ready (creates on-demand or recreates if vector changed) */
	doca_error_t msix_result = vnet_ensure_config_msix(controller);
	if (msix_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Speed change: config MSI-X not available: %s", doca_error_get_descr(msix_result));
		return msix_result;
	}

	uint32_t old_speed = vnet_cfg->speed;

	if (old_speed == new_speed) {
		DOCA_LOG_INFO("Speed change: already at %u Mbps, skipping", new_speed);
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("Speed change: %u -> %u Mbps (link bounce + MSI-X)", old_speed, new_speed);

	/* Step 1: Clear LINK_UP and raise MSI-X.
	 * The Linux virtio-net driver only calls virtnet_update_settings() on a
	 * LINK_UP transition (virtnet_config_changed_work returns early if status
	 * unchanged).  A link bounce is therefore required to make it re-read speed. */
	pci_cfg->config_generation++;
	vnet_cfg->status &= ~VIRTIO_NET_S_LINK_UP;
	doca_devemu_pci_msix_raise(controller->config_msix);

	/* Brief delay for host driver to process link-down event.
	 * The host ISR schedules a work item that reads status -- give it time. */
	usleep(50000); /* 50ms */

	/* Step 2: Update speed while link is down */
	vnet_cfg->speed = new_speed;

	/* Step 3: Set LINK_UP and raise MSI-X.
	 * Host driver detects link-up transition and re-reads speed via virtnet_update_settings(). */
	pci_cfg->config_generation++;
	vnet_cfg->status |= VIRTIO_NET_S_LINK_UP;
	doca_devemu_pci_msix_raise(controller->config_msix);

	DOCA_LOG_INFO("Speed change: done (%u -> %u Mbps)", old_speed, new_speed);

	return DOCA_SUCCESS;
}

/*********************************************************************************************************************
 * End of VNet Controller Management Functions
 *********************************************************************************************************************/

static void vnet_pci_dev_force_reset(struct vnet_virtio_common_config *pci_cfg, struct vnet_pci_device *dev)
{
	if (pci_cfg) {
		pci_cfg->device_status = 0;
		dev->prev_status = 0;
		DOCA_LOG_INFO("Forced VirtIO RESET on pf_index=%d (device_status=0)", dev->pf_index);
	}
}

/**
 * @brief VirtIO device status change callback
 *
 * Monitors and logs VirtIO device status transitions during host driver
 * initialization and operation. Tracks standard VirtIO status progression:
 * ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK or FAILED states.
 *
 * @param[in] dev VirtIO device experiencing status change
 * @param[in] arg User argument context (unused)
 *
 * @note Implements VirtIO 1.0 specification status change handling
 * @note Provides detailed logging for debugging driver interactions
 * @note Detects device resets and status transitions
 */
static void virtio_net_ctrl_change_cb(struct vnet_pci_device *dev, void *arg)
{
	struct vnet_virtio_common_config *pci_cfg = vnet_pci_device_get_pci_cfg(dev);
	struct vnet_pci_dev_resources *resources = arg;
	/* Use per-device prev_status instead of static variable for multi-device support */
	uint8_t prev_status = dev->prev_status;

	if (pci_cfg->device_status == 0 && resources && resources->tlp_ctx && dev->pf_index >= 0 &&
	    (uint32_t)dev->pf_index < resources->tlp_ctx->num_ep) {
		struct vnet_pci_dev_controller *ctrl = &resources->tlp_ctx->vnet_controller[dev->pf_index];

		if (atomic_load(&ctrl->reset_status_state) == VNET_RESET_STATUS_HELD) {
			pci_cfg->device_status = VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET;
			DOCA_LOG_INFO("RESET cleanup pending - reasserting device_status=0x40");
			return;
		}
	}

	/* Device Reset Detection */
	if (pci_cfg->device_status == 0 && prev_status != 0 && prev_status != 0xFF) {
		DOCA_LOG_INFO("VNet device %02x:%02x.%x: RESET detected - performing cleanup",
			      dev->bus,
			      dev->device,
			      dev->function);

		if (resources && resources->tlp_ctx && dev->pf_index >= 0 &&
		    (uint32_t)dev->pf_index < resources->tlp_ctx->num_ep) {
			struct vnet_pci_dev_controller *ctrl = &resources->tlp_ctx->vnet_controller[dev->pf_index];
			struct pci_device_config *endpoint =
				&resources->tlp_ctx->devs_config[FIRST_PF_IDX(resources->tlp_ctx) + dev->pf_index];

			/* Don't interfere if application is shutting down */
			if (atomic_load(&ctrl->shutting_down)) {
				DOCA_LOG_DBG("Ignoring reset during application shutdown");
				dev->prev_status = 0;
				return;
			}

			if (!vnet_controller_has_reset_teardown_work(ctrl)) {
				DOCA_LOG_DBG("RESET: no controller resources to tear down, completing immediately");
				dev->prev_status = 0;
				atomic_store(&dev->cancel_in_progress, false);
				return;
			}

			/*
			 * Reset may arrive in two situations:
			 *
			 * 1) Normal rebind: host driver is resetting to start a fresh
			 *    probe cycle. Engine must stay alive to serve the ensuing
			 *    FEATURES_OK/DRIVER_OK.
			 *
			 * 2) Unplug in progress: host pciehp is tearing down the driver
			 *    before writing Power OFF.
			 *
			 * We CANNOT distinguish (1) from (2) by device_status alone --
			 * both are device_status == 0. Historically the code chose
			 * destroy_engine=true in case (2), but that races fatally when
			 * the host queues a unplug for a device it hasn't finished
			 * probing: the fresh-rebind reset in (1) gets misclassified as
			 * (2) because pending_unplug was pre-armed by the CLI, the
			 * engine is destroyed mid-probe, and virtnet_probe() wedges
			 * forever waiting for a CVQ that can no longer respond.
			 *
			 * The safe choice is always destroy_engine=false here. The
			 * real destroy happens exactly once, when the host later writes
			 * SLOT_CTRL bit10=1 (Power OFF) and config_space_cap_pcie_write
			 * submits delayed_destroy on the DLActive 1->0 transition.
			 */
			atomic_store(&ctrl->reset_status_state, VNET_RESET_STATUS_HELD);
			pci_cfg->device_status = VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET;
			DOCA_LOG_INFO("RESET: holding device_status=0x40 until controller cleanup completes");

			if (atomic_load(&endpoint->pending_unplug)) {
				DOCA_LOG_INFO("Pending unplug: host reset - keeping engine alive until Power OFF");
				pci_cfg_workqueue_submit_controller_cleanup(ctrl, false);
				dev->prev_status = 0;
				return;
			}

			/* Use unified cleanup with destroy_engine=false (keep engine for rebind) - ASYNC */
			pci_cfg_workqueue_submit_controller_cleanup(ctrl, false);
			DOCA_LOG_INFO("RESET cleanup queued - ready for fresh start on FEATURES_OK");
		}

		dev->prev_status = 0;
		return;
	}

	/* Log intermediate status transitions */
	if (pci_cfg->device_status != prev_status && pci_cfg->device_status != 0) {
		if ((pci_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_ACK) &&
		    !(prev_status & VNET_VIRTIO_DEVICE_STATUS_ACK)) {
			DOCA_LOG_INFO("%02x:%02x.%x:VirtIO Status: <ACKNOWLEDGE> bit set",
				      dev->bus,
				      dev->device,
				      dev->function);
		}

		if ((pci_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER) &&
		    !(prev_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER)) {
			DOCA_LOG_INFO("%02x:%02x.%x:VirtIO Status: <DRIVER> bit set",
				      dev->bus,
				      dev->device,
				      dev->function);
		}

		if ((pci_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK) &&
		    !(prev_status & VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK)) {
			DOCA_LOG_INFO(
				"%02x:%02x.%x:VirtIO Status: <FEATURES_OK> bit set - starting hardware preparation",
				dev->bus,
				dev->device,
				dev->function);

			/* VirtIO Spec Compliance: Start hardware preparation during FEATURES_OK phase */
			if (resources && resources->tlp_ctx && dev->pf_index >= 0 &&
			    (uint32_t)dev->pf_index < resources->tlp_ctx->num_ep) {
				struct vnet_pci_dev_controller *ctrl =
					&resources->tlp_ctx->vnet_controller[dev->pf_index];
				struct pci_device_config *endpoint =
					&resources->tlp_ctx
						 ->devs_config[FIRST_PF_IDX(resources->tlp_ctx) + dev->pf_index];
				bool has_cvq, has_mq;

				/* Allow FEATURES_OK even when pending_unplug is set.
				 * The host driver is actively probing and will send CVQ
				 * commands after DRIVER_OK. Blocking init here leaves the
				 * engine disabled and causes the host to spin-wait forever
				 * on CVQ completion, triggering an RCU stall.
				 * The device will be torn down later when the host responds
				 * to the ABP attention-button / power-off sequence. */
				if (atomic_load(&endpoint->pending_unplug))
					DOCA_LOG_INFO("FEATURES_OK: pending_unplug set on DSP[%d] - "
						      "proceeding with init (host driver is active)",
						      dev->pf_index);

				/* NON-BLOCKING: If cleanup is in progress, defer initialization.
				 * Cleanup will check deferred_init_pending and trigger init when complete.
				 * This keeps the callback responsive for TLP handling (< 1ms latency). */
				if (atomic_load(&ctrl->cleanup_running)) {
					DOCA_LOG_INFO("FEATURES_OK: Cleanup in progress - deferring init");
					atomic_store(&ctrl->deferred_init_pending, true);
					goto features_ok_done;
				}

				DOCA_LOG_DBG(
					"VirtIO Spec Compliance: Hardware preparation starting (FEATURES_OK phase)");

				/* Mark initialization as in progress - DRIVER_OK will queue work after */
				atomic_store(&ctrl->initialization_in_progress, true);

				/* Check MQ feature negotiation (requires BOTH F_CTRL_VQ and F_MQ) */
				has_cvq = !!(dev->driver_features & (1ULL << VIRTIO_NET_F_CTRL_VQ));
				has_mq = !!(dev->driver_features & (1ULL << VIRTIO_NET_F_MQ));

				ctrl->mq_feature_negotiated = (has_cvq && has_mq);

				if (ctrl->mq_feature_negotiated) {
					DOCA_LOG_INFO(
						"MQ features negotiated: F_CTRL_VQ + F_MQ -- CVQ + MQ support enabled");
					atomic_store(&ctrl->num_active_qps, VNET_DEFAULT_QUEUE_PAIRS);
					ctrl->max_queue_pairs = resources->tlp_ctx->max_queue_pairs;
					atomic_store(&ctrl->deferred_mq.initial_data_qps_deferred,
						     vnet_tlp_ctx_should_defer_mq_start(resources->tlp_ctx));
				} else {
					DOCA_LOG_INFO("MQ features NOT negotiated (CTRL_VQ=%d, MQ=%d) - single QP mode",
						      has_cvq,
						      has_mq);
					atomic_store(&ctrl->num_active_qps, 1);
					ctrl->max_queue_pairs = 1;
					atomic_store(&ctrl->deferred_mq.initial_data_qps_deferred, false);
				}

				/* Log workqueue status before submitting work */
				pci_cfg_workqueue_log_status();

				/* Start engine if not already started (first boot or post-reset) - ASYNC */
				DOCA_LOG_INFO("FEATURES_OK: About to queue async work (engine_started=%d)",
					      atomic_load(&ctrl->offload_engine_started));
				if (!atomic_load(&ctrl->offload_engine_started)) {
					DOCA_LOG_INFO("FEATURES_OK: Submitting engine_start work item");
					pci_cfg_workqueue_submit_engine_start(ctrl);
					DOCA_LOG_INFO("FEATURES_OK: engine_start submitted");
				}

				DOCA_LOG_INFO("FEATURES_OK: MQ mode=%s, max_qps=%d, active_qps=%d",
					      ctrl->mq_feature_negotiated ? "YES" : "NO",
					      ctrl->max_queue_pairs,
					      atomic_load(&ctrl->num_active_qps));

				/* Create VQ structures (NOT started yet - created only) - ASYNC */
				DOCA_LOG_INFO("FEATURES_OK: Submitting initialize_vqs work item");
				pci_cfg_workqueue_submit_initialize_vqs(ctrl);
				DOCA_LOG_INFO("FEATURES_OK: initialize_vqs submitted");

				/* Initialize IO context for CVQ command handling - ASYNC
				 * NOTE: This clears initialization_in_progress when complete */
				DOCA_LOG_INFO("FEATURES_OK: Submitting initialize_io_context work item");
				pci_cfg_workqueue_submit_initialize_io_context(ctrl);
				DOCA_LOG_INFO("FEATURES_OK: All async work items submitted, handler complete");
			}
features_ok_done:;
		}
	}

	/* VirtIO DRIVER_OK State - Make Device Operational */
	if ((pci_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) &&
	    !(prev_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK)) {
		if (resources && resources->tlp_ctx && dev->pf_index >= 0 &&
		    (uint32_t)dev->pf_index < resources->tlp_ctx->num_ep) {
			struct vnet_pci_dev_controller *controller =
				&resources->tlp_ctx->vnet_controller[dev->pf_index];
			struct pci_device_config *endpoint =
				&resources->tlp_ctx->devs_config[FIRST_PF_IDX(resources->tlp_ctx) + dev->pf_index];
			const struct vnet_virtio_net_config *net_cfg = vnet_pci_device_get_vnet_dev_cfg(dev);
			const struct vnet_virtio_queue_config *vqs = vnet_pci_device_get_virtq_pci_cfg(dev);
			uint8_t enabled_queues = 0;
			uint8_t i;

			/* Allow DRIVER_OK even when pending_unplug is set.
			 * The host driver has completed probe and will immediately
			 * issue CVQ commands (e.g. VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET).
			 * The engine must be enabled so these commands complete;
			 * otherwise the host spins forever in virtnet_send_command()
			 * causing an RCU stall and system-wide lock-up.
			 * Cleanup happens later via host ABP -> power-off -> destroy. */
			if (atomic_load(&endpoint->pending_unplug))
				DOCA_LOG_INFO("%02x:%02x.%x: DRIVER_OK with pending_unplug on DSP[%d] - "
					      "proceeding (host driver needs working CVQ)",
					      dev->bus,
					      dev->device,
					      dev->function,
					      dev->pf_index);

			/* NON-BLOCKING: Workqueue is FIFO, so start_and_enable will execute
			 * after any pending init work completes. No need to block here.
			 * If cleanup is running, deferred_init will queue start_and_enable.
			 * If initial queues are still incomplete, later VQ config updates
			 * under DRIVER_OK will requeue start_and_enable. */

			/* DRIVER_OK is set - queue work to make device operational.
			 * Workqueue ensures proper ordering: init work -> start_and_enable */
			if (!atomic_load(&controller->engine_enabled)) {
				DOCA_LOG_INFO("%02x:%02x.%x: DRIVER_OK set - starting VQs and enabling engine",
					      dev->bus,
					      dev->device,
					      dev->function);

				/* Start VQs and enable engine - ASYNC.
				 * Counters/stats creation is done in start_and_enable after engine is ready. */
				pci_cfg_workqueue_submit_start_and_enable(controller, dev);
			}

			/* Count configured queues for logging */
			for (i = 0; i < pci_cfg->num_queues; i++) {
				if (vqs[i].queue_enable)
					enabled_queues++;
			}

			DOCA_LOG_INFO("%02x:%02x.%x: VNet: now OPERATIONAL - MAC %02x:%02x:%02x:%02x:%02x:%02x",
				      dev->bus,
				      dev->device,
				      dev->function,
				      net_cfg->mac[0],
				      net_cfg->mac[1],
				      net_cfg->mac[2],
				      net_cfg->mac[3],
				      net_cfg->mac[4],
				      net_cfg->mac[5]);

			DOCA_LOG_INFO("  Configured VQs: %d, Active QPs: %d/%d",
				      enabled_queues,
				      atomic_load(&controller->num_active_qps),
				      controller->max_queue_pairs);
			DOCA_LOG_INFO("  CVQ: %s", controller->cvq ? "started" : "not present");
			DOCA_LOG_INFO("%02x:%02x.%x: VirtIO DRIVER_OK detected - device is live now",
				      dev->bus,
				      dev->device,
				      dev->function);

			/* Note: VNET counters are now created in vnet_pci_dev_start_and_enable()
			 * after engine is guaranteed to be started. This fixes the race condition
			 * where counters_create was called before the async start_and_enable completed. */

		} else {
			DOCA_LOG_ERR("%02x:%02x.%x: No controller available during DRIVER_OK - critical error",
				     dev->bus,
				     dev->device,
				     dev->function);
		}

		dev->prev_status = pci_cfg->device_status;
	} else if (pci_cfg->device_status != prev_status && pci_cfg->device_status != 0) {
		dev->prev_status = pci_cfg->device_status;
	}
}

/*
 * Check if stdin has input available without blocking
 *
 * @return: true if input is available, false otherwise
 */
static bool stdin_has_input(void)
{
	struct timeval tv = {0, 0};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* CLI command types */
enum cli_cmd_type {
	CLI_CMD_PLUG,
	CLI_CMD_UNPLUG,
	CLI_CMD_SPEED,
};

/* Parsed CLI command */
struct cli_command {
	enum cli_cmd_type type;
	uint32_t ep_index;
	uint32_t speed; /* only for CLI_CMD_SPEED */
};

/*
 * Read and parse CLI command from stdin (non-blocking)
 *
 * Supported commands:
 *   plug <ep_idx>            - Hotplug a device
 *   unplug <ep_idx>          - Remove a hotplugged device
 *   speed <ep_idx> <mbps>    - Change link speed with MSI-X notification
 *
 * @max_ep [in]: Maximum endpoint count (for index validation)
 * @cmd [out]: Parsed command
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t read_cli_command(uint32_t max_ep, struct cli_command *cmd)
{
	char line[128];
	char command[32];
	uint32_t idx;
	int speed_val;

	if (fgets(line, sizeof(line), stdin) == NULL) {
		clearerr(stdin);
		return DOCA_ERROR_IO_FAILED;
	}

	char *p = line;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p == '\0')
		return DOCA_ERROR_EMPTY;

	/* Try 3-argument format first (speed <idx> <value>) */
	if (sscanf(line, "%31s %u %d", command, &idx, &speed_val) == 3 && strcmp(command, "speed") == 0) {
		if (idx >= max_ep) {
			DOCA_LOG_ERR("EP index %u out of range (max: %u)", idx, max_ep - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}
		if (speed_val <= 0) {
			DOCA_LOG_ERR("Speed value must be greater than 0 Mbps");
			return DOCA_ERROR_INVALID_VALUE;
		}
		cmd->type = CLI_CMD_SPEED;
		cmd->ep_index = idx;
		cmd->speed = (uint32_t)speed_val;
		return DOCA_SUCCESS;
	}

	/* Try 2-argument format (plug/unplug <idx>) */
	if (sscanf(line, "%31s %u", command, &idx) == 2) {
		if (idx >= max_ep) {
			DOCA_LOG_ERR("EP index %u out of range (max: %u)", idx, max_ep - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}
		if (strcmp(command, "plug") == 0) {
			cmd->type = CLI_CMD_PLUG;
			cmd->ep_index = idx;
			return DOCA_SUCCESS;
		} else if (strcmp(command, "unplug") == 0) {
			cmd->type = CLI_CMD_UNPLUG;
			cmd->ep_index = idx;
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_ERR("Unknown command. Usage: plug <idx> | unplug <idx> | speed <idx> <mbps>");
	return DOCA_ERROR_INVALID_VALUE;
}

/*
 * Enqueue ACG credit to the circular queue
 *
 * @tlp_ctx [in]: TLP context
 * @acg_req [in]: ACG request to enqueue
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_NO_MEMORY if queue is full or NULL
 */
doca_error_t acg_queue_push(struct tlp_context *tlp_ctx, struct doca_devemu_pci_tlp_channel_req *acg_req)
{
	doca_error_t result = DOCA_SUCCESS;

	pthread_mutex_lock(&tlp_ctx->acg_queue_lock);
	if (tlp_ctx->acg_queue == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto unlock;
	}
	if (tlp_ctx->acg_queue_count >= tlp_ctx->acg_queue_size) {
		result = DOCA_ERROR_NO_MEMORY;
		goto unlock;
	}

	tlp_ctx->acg_queue[tlp_ctx->acg_queue_tail] = acg_req;
	tlp_ctx->acg_queue_tail = (tlp_ctx->acg_queue_tail + 1) % tlp_ctx->acg_queue_size;
	tlp_ctx->acg_queue_count++;

unlock:
	pthread_mutex_unlock(&tlp_ctx->acg_queue_lock);
	return result;
}

void acg_queue_stats_get(struct tlp_context *tlp_ctx, uint16_t *count, uint16_t *size)
{
	pthread_mutex_lock(&tlp_ctx->acg_queue_lock);
	*count = tlp_ctx->acg_queue_count;
	*size = tlp_ctx->acg_queue_size;
	pthread_mutex_unlock(&tlp_ctx->acg_queue_lock);
}

/*
 * Dequeue ACG credit from the circular queue
 *
 * @tlp_ctx [in]: TLP context
 * @return: ACG request pointer, or NULL if queue is empty or NULL
 */
struct doca_devemu_pci_tlp_channel_req *acg_queue_pop(struct tlp_context *tlp_ctx)
{
	struct doca_devemu_pci_tlp_channel_req *acg_req = NULL;

	pthread_mutex_lock(&tlp_ctx->acg_queue_lock);
	if (tlp_ctx->acg_queue == NULL || tlp_ctx->acg_queue_count == 0)
		goto unlock;

	acg_req = tlp_ctx->acg_queue[tlp_ctx->acg_queue_head];
	tlp_ctx->acg_queue_head = (tlp_ctx->acg_queue_head + 1) % tlp_ctx->acg_queue_size;
	tlp_ctx->acg_queue_count--;

unlock:
	pthread_mutex_unlock(&tlp_ctx->acg_queue_lock);
	return acg_req;
}

/*
 * Initialize PCI capabilities for endpoint (matches tlp_handler implementation)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_endpoint_capabilities(struct pci_device_config *dev_cfg)
{
	/* Express capability */
	dev_cfg->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dev_cfg->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_VPD;
	dev_cfg->caps.express.pcie_cap_register = 0x0002;
	dev_cfg->caps.express.dev_capabilities = 0x112c8fe2;
	dev_cfg->caps.express.dev_control = 0x2950;
	dev_cfg->caps.express.dev_status = 0x0000;
	dev_cfg->caps.express.link_capabilities = 0x00500104;
	dev_cfg->caps.express.link_control = 0x0000;
	dev_cfg->caps.express.link_status = 0x3104;
	dev_cfg->caps.express.slot_capabilities = 0;
	dev_cfg->caps.express.slot_control = 0;
	dev_cfg->caps.express.slot_status = 0;
	dev_cfg->caps.express.root_control = 0;
	dev_cfg->caps.express.root_capabilities = 0;
	dev_cfg->caps.express.root_status = 0;
	dev_cfg->caps.express.dev_capabilities2 = 0x00030397;
	dev_cfg->caps.express.dev_control2 = 0;
	dev_cfg->caps.express.dev_status2 = 0;
	dev_cfg->caps.express.link_capabilities2 = 0x0180003e;
	dev_cfg->caps.express.link_control2 = 0;
	dev_cfg->caps.express.link_status2 = 0;
	dev_cfg->caps.express.slot_capabilities2 = 0;
	dev_cfg->caps.express.slot_control2 = 0;
	dev_cfg->caps.express.slot_status2 = 0;

	/* VPD capability */
	dev_cfg->caps.vpd.cap_id = TLP_PCI_CAP_ID_VPD;
	dev_cfg->caps.vpd.next_cap_ptr = TLP_PCI_CAP_OFFSET_MSIX;
	dev_cfg->caps.vpd.addr_register = 0x0000;
	dev_cfg->caps.vpd.data_register = 0x00000000;

	/* MSI-X capability */
	dev_cfg->caps.msix.cap_id = TLP_PCI_CAP_ID_MSIX;
	dev_cfg->caps.msix.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dev_cfg->caps.msix.message_control = 0x0000;
	dev_cfg->caps.msix.table_offset = 0x00001000;
	dev_cfg->caps.msix.pba_offset = 0x00002000;

	/* PM capability */
	dev_cfg->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dev_cfg->caps.pm.next_cap_ptr = 0x00;
	dev_cfg->caps.pm.pmc = 0xC803;
	dev_cfg->caps.pm.pmcsr = 0x0008;
	dev_cfg->caps.pm.reserved = 0x00;
	dev_cfg->caps.pm.data = 0x00;
}

/*
 * Initialize USP (Upstream Port) capabilities
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_usp_capabilities(struct pci_device_config *dev_cfg)
{
	/* Express capability for upstream port */
	dev_cfg->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dev_cfg->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dev_cfg->caps.express.pcie_cap_register = 0x0052;    /* Type 5: Upstream Port, No Slot, Version=2 */
	dev_cfg->caps.express.dev_capabilities = 0x00008001; /* Max_Payload_Size=128, Extended Tag */
	dev_cfg->caps.express.dev_control = 0x0000;
	dev_cfg->caps.express.dev_status = 0x0000;
	dev_cfg->caps.express.link_capabilities = 0x00500104;
	dev_cfg->caps.express.link_control = 0x0000;
	dev_cfg->caps.express.link_status = 0x1104;
	dev_cfg->caps.express.slot_capabilities = 0;
	dev_cfg->caps.express.slot_control = 0;
	dev_cfg->caps.express.slot_status = 0;

	dev_cfg->caps.express.root_control = 0;
	dev_cfg->caps.express.root_capabilities = 0;
	dev_cfg->caps.express.root_status = 0;
	dev_cfg->caps.express.dev_capabilities2 = 0;
	dev_cfg->caps.express.dev_control2 = 0;
	dev_cfg->caps.express.dev_status2 = 0;
	dev_cfg->caps.express.link_capabilities2 = 0;
	dev_cfg->caps.express.link_control2 = 0;
	dev_cfg->caps.express.link_status2 = 0;
	dev_cfg->caps.express.slot_capabilities2 = 0;
	dev_cfg->caps.express.slot_control2 = 0;
	dev_cfg->caps.express.slot_status2 = 0;

	/* PM capability */
	dev_cfg->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dev_cfg->caps.pm.next_cap_ptr = 0x00;
	dev_cfg->caps.pm.pmc = 0xC803;
	dev_cfg->caps.pm.pmcsr = 0x0008;
	dev_cfg->caps.pm.reserved = 0x00;
	dev_cfg->caps.pm.data = 0x00;

	/* VPD and MSI not needed for USP */
	memset(&dev_cfg->caps.vpd, 0, sizeof(dev_cfg->caps.vpd));
	memset(&dev_cfg->caps.msi, 0, sizeof(dev_cfg->caps.msi));
}

/*
 * Initialize PCI capabilities for DSP bridge (for hotplug support)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 * @slot_number [in]: Physical slot number for this DSP (must be unique per DSP)
 */
static void init_dsp_capabilities(struct pci_device_config *dev_cfg, uint32_t slot_number)
{
	/* Express capability for bridge with hotplug support */
	dev_cfg->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dev_cfg->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_MSI;
	dev_cfg->caps.express.pcie_cap_register = 0x0162;    /* Type 6: DSP, Slot Implemented=1, Version=2 */
	dev_cfg->caps.express.dev_capabilities = 0x00008001; /* Max_Payload_Size=128, Extended Tag */
	dev_cfg->caps.express.dev_control = 0x0000;
	dev_cfg->caps.express.dev_status = 0x0000;
	dev_cfg->caps.express.link_capabilities = 0x01500104;
	dev_cfg->caps.express.link_control = 0x0000;
	dev_cfg->caps.express.link_status = 0x1104;
	/* Slot Capabilities: AttnBtn=0, PwrCtrl=1, HotPlug=1, Surprise=1, NoCompl=0
	 * Physical Slot Number = 0 forces Linux to use PCI geographic naming (enp<bus>s<slot>) */
	dev_cfg->caps.express.slot_capabilities =
		(SLOT_CAP_PWR_CTRL_PRESENT | SLOT_CAP_HP_SURPRISE | SLOT_CAP_HP_CAPABLE |
		 (TLP_BRIDGE_SLOT_PWR_LIMIT << SLOT_CAP_PWR_LIMIT_VALUE_SHIFT) |
		 (slot_number << SLOT_CAP_PHYS_SLOT_NUM_SHIFT));
	/*
	 * Slot Control: Let Host set enable bits via pcie_enable_notification()
	 * Only initialize Power OFF (bit10=1)
	 */
	dev_cfg->caps.express.slot_control = SLOT_CTRL_POWER_CONTROLLER;
	dev_cfg->caps.express.slot_status = 0x0000;

	dev_cfg->caps.express.root_control = 0;
	dev_cfg->caps.express.root_capabilities = 0;
	dev_cfg->caps.express.root_status = 0;
	dev_cfg->caps.express.dev_capabilities2 = 0;
	dev_cfg->caps.express.dev_control2 = 0;
	dev_cfg->caps.express.dev_status2 = 0;
	dev_cfg->caps.express.link_capabilities2 = 0;
	dev_cfg->caps.express.link_control2 = 0;
	dev_cfg->caps.express.link_status2 = 0;
	dev_cfg->caps.express.slot_capabilities2 = 0;
	dev_cfg->caps.express.slot_control2 = 0;
	dev_cfg->caps.express.slot_status2 = 0;

	dev_cfg->caps.msi.cap_id = TLP_PCI_CAP_ID_MSI;
	dev_cfg->caps.msi.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dev_cfg->caps.msi.message_control = 0x0080;
	dev_cfg->caps.msi.message_address_low = 0x00000000;
	dev_cfg->caps.msi.message_address_high = 0x00000000;
	dev_cfg->caps.msi.message_data = 0x0000;
	dev_cfg->caps.msi.reserved = 0x0000;
	dev_cfg->caps.msi.mask_bits = 0x00000000;
	dev_cfg->caps.msi.pending_bits = 0x00000000;

	/* PM capability */
	dev_cfg->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dev_cfg->caps.pm.next_cap_ptr = 0x00;
	dev_cfg->caps.pm.pmc = 0xC803;
	dev_cfg->caps.pm.pmcsr = 0x0008;
	dev_cfg->caps.pm.reserved = 0x00;
	dev_cfg->caps.pm.data = 0x00;

	memset(&dev_cfg->caps.vpd, 0, sizeof(dev_cfg->caps.vpd));
}

/*
 * Initialize default Type 0 configuration space header (Endpoint)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_type0_header(struct pci_device_config *dev_cfg)
{
	dev_cfg->cfg_space_hdr.type0.vendor_id = TLP_PCI_TYPE_VENDOR_ID;
	dev_cfg->cfg_space_hdr.type0.device_id = TLP_PCI_TYPE_ENDPOINT_DEVICE_ID;
	dev_cfg->cfg_space_hdr.type0.command = 0x0000;
	dev_cfg->cfg_space_hdr.type0.status = 0x0010; /* Capabilities list present */
	dev_cfg->cfg_space_hdr.type0.class_code = TLP_PCI_CLASS_CODE_ENDPOINT;
	dev_cfg->cfg_space_hdr.type0.revision_id = TLP_PCI_TYPE_REVISION_ID;
	dev_cfg->cfg_space_hdr.type0.bist = 0x00;
	dev_cfg->cfg_space_hdr.type0.header_type = HEADER_TYPE_ENDPOINT;
	dev_cfg->cfg_space_hdr.type0.latency_timer = 0x00;
	dev_cfg->cfg_space_hdr.type0.cache_line_size = 0x10;

	/* BAR configuration - endpoints will have BAR0-1 as 64-bit prefetchable memory */
	dev_cfg->cfg_space_hdr.type0.bar[0] = 0x00000000 | BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT |
					      BAR_MEM_PREFETCHABLE;
	dev_cfg->cfg_space_hdr.type0.bar[1] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[2] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[3] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[4] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[5] = 0x00000000;

	dev_cfg->cfg_space_hdr.type0.cardbus_cis_pointer = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.subsystem_vendor_id = TLP_PCI_TYPE_SUBSYSTEM_VENDOR_ID;
	dev_cfg->cfg_space_hdr.type0.subsystem_id = TLP_PCI_TYPE_SUBSYSTEM_ID;
	dev_cfg->cfg_space_hdr.type0.exp_rom_base_addr = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.cap_ptr = 0x60; /* First capability at offset 0x60 (Express Cap) */
	dev_cfg->cfg_space_hdr.type0.reserved_at_14 = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.interrupt_line = 0x00;
	dev_cfg->cfg_space_hdr.type0.interrupt_pin = 0x00;
	dev_cfg->cfg_space_hdr.type0.min_grant = 0x00;
	dev_cfg->cfg_space_hdr.type0.max_latency = 0x00;
}

/*
 * Initialize default Type 1 configuration space header (Bridge)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_type1_header(struct pci_device_config *dev_cfg)
{
	dev_cfg->cfg_space_hdr.type1.vendor_id = TLP_PCI_TYPE_VENDOR_ID;
	dev_cfg->cfg_space_hdr.type1.device_id = TLP_PCI_TYPE_BRIDGE_DEVICE_ID;
	dev_cfg->cfg_space_hdr.type1.command = 0x0400; /* Bus Master Enable */
	dev_cfg->cfg_space_hdr.type1.status = 0x0010;  /* Capabilities list present */
	dev_cfg->cfg_space_hdr.type1.class_code = TLP_PCI_CLASS_CODE_BRIDGE;
	dev_cfg->cfg_space_hdr.type1.revision_id = TLP_PCI_TYPE_REVISION_ID;
	dev_cfg->cfg_space_hdr.type1.bist = 0x00;
	dev_cfg->cfg_space_hdr.type1.header_type = HEADER_TYPE_BRIDGE;
	dev_cfg->cfg_space_hdr.type1.latency_timer = 0x00;
	dev_cfg->cfg_space_hdr.type1.cache_line_size = 0x00;

	dev_cfg->cfg_space_hdr.type1.bar[0] = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.bar[1] = 0x00000000;
	/* Bus numbers - will be configured by Host during PCI enumeration */
	dev_cfg->cfg_space_hdr.type1.primary_bus = 0x00;
	dev_cfg->cfg_space_hdr.type1.secondary_bus = 0x00;
	dev_cfg->cfg_space_hdr.type1.subordinate_bus = 0x00;
	dev_cfg->cfg_space_hdr.type1.secondary_latency = 0x00;

	dev_cfg->cfg_space_hdr.type1.io_base = 0x00;
	dev_cfg->cfg_space_hdr.type1.io_limit = 0x00;
	dev_cfg->cfg_space_hdr.type1.secondary_status = 0x0000;
	dev_cfg->cfg_space_hdr.type1.memory_base = 0x0000;
	dev_cfg->cfg_space_hdr.type1.memory_limit = 0x0000;
	dev_cfg->cfg_space_hdr.type1.pre_memory_base = 0x0001;
	dev_cfg->cfg_space_hdr.type1.pre_memory_limit = 0x0001;
	dev_cfg->cfg_space_hdr.type1.pre_memory_base_upper_32bit = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.pre_memory_limit_upper_32bit = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.io_base_upper_16bit = 0x0000;
	dev_cfg->cfg_space_hdr.type1.io_limit_upper_16bit = 0x0000;
	dev_cfg->cfg_space_hdr.type1.cap_ptr = 0x60; /* First capability at offset 0x60 */
	dev_cfg->cfg_space_hdr.type1.exp_rom_base_addr = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.interrupt_line = 0x00;
	dev_cfg->cfg_space_hdr.type1.interrupt_pin = 0x00;
	dev_cfg->cfg_space_hdr.type1.bridge_control = 0x0000;
}

/*
 * Initialize PCI device topology
 *
 * @tlp_ctx [in/out]: TLP context containing device array
 */
static void init_device_topology(struct tlp_context *tlp_ctx)
{
	uint32_t i;
	/* Note: devs_config is already zero-initialized by calloc() in init_tlp_context().
	 * Do NOT memset() here as it would overwrite the endpoint_lock rwlocks
	 * that were initialized in init_tlp_context(). */

	/* Initialize all bridges (USP + DSPs) - bridges do not need VHCA IDs */
	for (i = 0; i < tlp_ctx->num_bridges; i++) {
		init_type1_header(&tlp_ctx->devs_config[i]);
		tlp_ctx->devs_config[i].is_bridge = true;
		tlp_ctx->devs_config[i].is_endpoint = false;
		tlp_ctx->devs_config[i].is_dummy = false;
		tlp_ctx->devs_config[i].vhca_id = 0; /* Bridges do not need VHCA IDs */
	}
	/* Configure USP (index 0) - Root of the switch */
	init_usp_capabilities(&tlp_ctx->devs_config[USP_IDX(tlp_ctx)]);
	tlp_ctx->devs_config[USP_IDX(tlp_ctx)].device = 0x00;
	tlp_ctx->devs_config[USP_IDX(tlp_ctx)].function = 0x00;
	/*
	 * Bus numbers are initialized to 0 and will be configured by Host during PCI enumeration.
	 * This matches the behavior of tlp_emu and real PCI hardware.
	 * Host will write bus numbers via Type 1 config write (register 0x06).
	 */
	tlp_ctx->devs_config[USP_IDX(tlp_ctx)].cfg_space_hdr.type1.primary_bus = 0x00;
	tlp_ctx->devs_config[USP_IDX(tlp_ctx)].cfg_space_hdr.type1.secondary_bus = 0x00;
	tlp_ctx->devs_config[USP_IDX(tlp_ctx)].cfg_space_hdr.type1.subordinate_bus = 0x00;

	/* Configure DSPs - Downstream ports (dynamic count) */
	/* Physical Slot Number = 0 forces Linux to use PCI geographic naming (enp<bus>s<slot>) */
	for (i = 0; i < tlp_ctx->num_dsp; i++) {
		uint32_t physical_slot_number = 0; /* 0 = use PCI naming instead of slot naming */
		struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i];

		init_dsp_capabilities(dsp, physical_slot_number);
		dsp->device = i;
		dsp->function = 0x00;
		/* Bus numbers will be configured by Host during enumeration */
		dsp->cfg_space_hdr.type1.primary_bus = 0x00;
		dsp->cfg_space_hdr.type1.secondary_bus = 0x00;
		dsp->cfg_space_hdr.type1.subordinate_bus = 0x00;
		/* Explicitly initialize MSI retry atomic - calloc zeroes memory but
		 * atomic_store is the correct way to initialize atomic types. */
		atomic_store(&dsp->msi_retry_pending, false);
		atomic_store(&dsp->msi_retry_since_ns, 0);
	}

	/* Initialize all endpoint devices (PFs) */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
		init_type0_header(ep);
		init_endpoint_capabilities(ep);
		ep->is_bridge = false;
		ep->is_endpoint = true;
		ep->is_dummy = false;
		/* Explicitly initialize atomic members - calloc zeroes memory but
		 * atomic_store is the correct way to initialize atomic types. */
		atomic_store(&ep->device_present, false);
		atomic_store(&ep->pending_unplug, false);
		atomic_store(&ep->pending_destroy, false);
		/* Note: endpoint_lock rwlock is already initialized in init_tlp_context() */
		ep->rep = NULL;
		ep->tlp_dev = NULL;
		ep->vhca_id = 0;
	}

	/* Configure Single-PF devices (each with function 0) */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i].device = 0x00;
		tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i].function = 0x00;
	}

	/* Initialize dummy device */
	init_type0_header(&tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)]);
	tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)].is_bridge = false;
	tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)].is_endpoint = false;
	tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)].is_dummy = true;
	DOCA_LOG_INFO("Initialized topology: %u bridges (1 USP + %u DSP), %u endpoints",
		      tlp_ctx->num_bridges,
		      tlp_ctx->num_dsp,
		      tlp_ctx->num_ep);
	DOCA_LOG_INFO("  - %u Single-PF devices (function 0 each)", tlp_ctx->num_ep);
	DOCA_LOG_INFO("Bus numbers will be configured by Host during PCI enumeration");
	DOCA_LOG_INFO("Host will write bus numbers via Type 1 config write to bridges");
}

/*
 * Update DSP Slot Status when device is plugged/unplugged
 *
 * @dsp [in/out]: DSP bridge device
 * @device_present [in]: True if device present, false if removed
 */
static void update_dsp_slot_status(struct pci_device_config *dsp, bool device_present)
{
	/*
	 * ABP (Attention Button Pressed) mode:
	 * - Plug: Set ABP + PDC + PDS, wait for Host to Power ON
	 * - Unplug: Set ABP only, let Host handle the removal sequence
	 *
	 * Note: Do NOT clear DLActive here on unplug!
	 * DLActive is cleared when Host writes Power OFF (SLOT_CTRL bit10=1).
	 * The destroy logic checks both pending_unplug AND DLActive=0.
	 */
	dsp->caps.express.slot_status |= SLOT_STS_ATTN_BTN_PRESSED;

	if (device_present) {
		/* Plug: ABP + PDC + PDS */
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_CHANGED;
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_STATE;
		/* Do NOT set DLActive - wait for Host to write Power ON */
		DOCA_LOG_INFO("Device plugged (ABP mode): ABP=1, PDC=1, PDS=1, DLActive=0");
	} else {
		/*
		 * Unplug: ABP only
		 * - Keep PDS=1 (let Host detect via ABP, not presence change)
		 * - Do NOT clear DLActive here - wait for Host Power OFF command
		 * - Host pciehp will detect ABP, unbind driver, then issue Power OFF
		 */
		DOCA_LOG_INFO("Device unplugged (ABP mode): ABP=1, waiting for Host Power OFF");
	}

	DOCA_LOG_DBG("DSP Slot Status updated: present=%d, status=0x%04X, link_status=0x%04X",
		     device_present,
		     dsp->caps.express.slot_status,
		     dsp->caps.express.link_status);
}

/*
 * Create endpoint device (representor + TLP device)
 * Used by both static mode (batch creation) and hotplug mode (on-demand).
 * For LU standby, @lu_ep_vhca_id is App_A's endpoint vhca_id so we reattach
 * to the existing emulated device rather than hotplugging a new one.
 *
 * @tlp_ctx [in]: TLP context
 * @endpoint [in/out]: Endpoint device configuration
 * @lu_ep_vhca_id [in]: If non-zero, reattach to existing rep with this vhca_id
 * @return: DOCA_SUCCESS on success, error otherwise
 */
static doca_error_t create_device(struct tlp_context *tlp_ctx,
				  struct pci_device_config *endpoint,
				  uint16_t lu_ep_vhca_id)
{
	const struct doca_devinfo_rep *devinfo_rep;
	uint16_t vhca_id_16;
	doca_error_t result;

	/* Reset pending flags to ensure clean state for new device.
	 * This handles edge case where previous destroy failed and left flags set.
	 * A successful plug should always start from a clean state. */
	atomic_store(&endpoint->pending_unplug, false);
	atomic_store(&endpoint->pending_destroy, false);

	if (lu_ep_vhca_id != 0) {
		/* LU standby: reattach to App_A's existing representor.
		 * Must succeed -- a new rep would have a different vhca_id
		 * and break the offload engine handover. */
		result = vnet_lu_find_existing_rep(tlp_ctx->pci_type, lu_ep_vhca_id, &endpoint->rep);
	} else {
		result = doca_devemu_pci_type_create_rep(tlp_ctx->pci_type, &endpoint->rep);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create/open representor: %s", doca_error_get_descr(result));
		return result;
	}
	endpoint->lu_rep_opened = (lu_ep_vhca_id != 0);

	/* Get VHCA ID */
	devinfo_rep = doca_dev_rep_as_devinfo(endpoint->rep);
	result = doca_devinfo_rep_get_vhca_id(devinfo_rep, &vhca_id_16);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get VHCA ID: %s", doca_error_get_descr(result));
		goto clean_rep;
	}
	endpoint->vhca_id = vhca_id_16;

	/* Create TLP device */
	result = doca_devemu_pci_tlp_dev_create(tlp_ctx->pci_type, endpoint->rep, &endpoint->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create TLP device: %s", doca_error_get_descr(result));
		goto clean_rep;
	}

	/* Start TLP device */
	result = doca_devemu_pci_tlp_dev_start(endpoint->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start TLP device: %s", doca_error_get_descr(result));
		goto clean_dev;
	}

	result = vnet_pci_dev_vnet_controller_create(tlp_ctx, endpoint);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VNet controller: %s", doca_error_get_descr(result));
		goto stop_dev;
	}

	atomic_store(&endpoint->device_present, true);
	DOCA_LOG_INFO("Device created: VHCA ID=0x%x", endpoint->vhca_id);

	return DOCA_SUCCESS;

stop_dev:
	doca_devemu_pci_tlp_dev_stop(endpoint->tlp_dev);
clean_dev:
	doca_devemu_pci_tlp_dev_destroy(endpoint->tlp_dev);
	endpoint->tlp_dev = NULL;
clean_rep:
	if (endpoint->lu_rep_opened)
		doca_dev_rep_close(endpoint->rep);
	else
		doca_devemu_pci_type_destroy_rep(endpoint->rep);
	endpoint->rep = NULL;
	return result;
}

/*
 * Destroy endpoint device (for both hotplug and static modes)
 *
 * @tlp_ctx [in]: TLP context
 * @endpoint [in/out]: Endpoint device configuration
 * @return: DOCA_SUCCESS on success, error otherwise
 */
doca_error_t vnet_pci_dev_destroy_device(struct tlp_context *tlp_ctx, struct pci_device_config *endpoint)
{
	doca_error_t first_error = DOCA_SUCCESS;
	doca_error_t tmp_result;
	struct doca_devemu_pci_tlp_dev *tlp_dev_to_destroy;
	struct doca_dev_rep *rep_to_destroy;
	uint32_t pf_index = 0;
	bool pf_index_valid = get_pf_index_for_device(tlp_ctx, endpoint, &pf_index) == DOCA_SUCCESS;

	DOCA_LOG_DBG("Destroying device: vhca_id=0x%x", endpoint->vhca_id);

	/*
	 * CRITICAL: Acquire write lock to synchronize with TLP request handlers.
	 * TLP handlers acquire read lock while using tlp_dev. Write lock blocks until
	 * all readers finish, ensuring no in-flight TLP requests use tlp_dev during destroy.
	 *
	 * Protocol:
	 * 1) Acquire write lock - waits for all TLP handlers to complete
	 * 2) Mark device undiscoverable (device_present=false, clear BARs)
	 * 3) Save and clear pointers (tlp_dev, rep)
	 * 4) Release write lock - new TLP requests will see tlp_dev=NULL and bail out
	 * 5) Safely destroy tlp_dev and rep (no concurrent users possible)
	 */
	pthread_rwlock_wrlock(&endpoint->endpoint_lock);

	atomic_store(&endpoint->device_present, false);
	endpoint->cfg_space_hdr.type0.bar[0] = 0;
	endpoint->cfg_space_hdr.type0.bar[1] = 0;
	endpoint->vhca_id = 0;

	/* Save pointers and clear them before releasing lock.
	 * After unlock, TLP handlers will see NULL and return error to host. */
	tlp_dev_to_destroy = endpoint->tlp_dev;
	rep_to_destroy = endpoint->rep;
	endpoint->tlp_dev = NULL;
	endpoint->rep = NULL;

	pthread_rwlock_unlock(&endpoint->endpoint_lock);

	/* Now safe to destroy - no concurrent users can hold references to tlp_dev */
	if (pf_index_valid && pf_index < tlp_ctx->num_dsp) {
		struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + pf_index];

		atomic_store(&dsp->msi_retry_pending, false);
		atomic_store(&dsp->msi_retry_since_ns, 0);
	}

	/* Destroy VNet controller - continue even on failure to clean up as much as possible */
	tmp_result = vnet_pci_dev_vnet_controller_destroy(tlp_ctx, endpoint);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy VNet controller: %s", doca_error_get_descr(tmp_result));
		if (first_error == DOCA_SUCCESS)
			first_error = tmp_result;
		/* Continue cleanup to leave system in recoverable state */
	}

	/* Stop and destroy TLP device */
	if (tlp_dev_to_destroy != NULL) {
		tmp_result = doca_devemu_pci_tlp_dev_stop(tlp_dev_to_destroy);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(tmp_result));
			if (first_error == DOCA_SUCCESS)
				first_error = tmp_result;
			/* Try to destroy anyway */
		} else {
			DOCA_LOG_DBG("TLP device stopped");
		}

		tmp_result = doca_devemu_pci_tlp_dev_destroy(tlp_dev_to_destroy);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy TLP device: %s", doca_error_get_descr(tmp_result));
			if (first_error == DOCA_SUCCESS)
				first_error = tmp_result;
		} else {
			DOCA_LOG_DBG("TLP device destroyed");
		}
	}

	/* Close or destroy representor depending on how it was obtained */
	if (rep_to_destroy != NULL) {
		if (endpoint->lu_rep_opened)
			tmp_result = doca_dev_rep_close(rep_to_destroy);
		else
			tmp_result = doca_devemu_pci_type_destroy_rep(rep_to_destroy);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close/destroy representor: %s", doca_error_get_descr(tmp_result));
			if (first_error == DOCA_SUCCESS)
				first_error = tmp_result;
		}
	}

	if (first_error != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Device destroy completed with errors");
		return first_error;
	}

	return DOCA_SUCCESS;
}

/**
 * Set Memory Write TLP header for MMIO operations (e.g., MSI interrupt)
 *
 * @tlp_header_buf [out]: Pointer to TLP header buffer (ACG buffer)
 * @msi_addr [in]: Target MSI address (32-bit or 64-bit)
 * @msi_data [in]: MSI data value
 * @requester_bdf [in]: Requester BDF (for requester ID field)
 * @return: Size of constructed TLP header in bytes
 */
static inline size_t set_memory_write_tlp_header(void *tlp_header_buf,
						 uint64_t msi_addr,
						 uint16_t msi_data,
						 uint16_t requester_bdf)
{
	uint32_t *header_dw = (uint32_t *)tlp_header_buf;
	bool use_64bit = (msi_addr >> 32) != 0;
	size_t header_dwords = use_64bit ? 5 : 4;

	/* Zero the header buffer to ensure no stale bits leak into masked fields.
	 * This is critical because DOCA_BE32P_SET only sets specified bits,
	 * leaving other bits unchanged (e.g., PH bits in addr field). */
	memset(tlp_header_buf, 0, header_dwords * sizeof(uint32_t));

	/* DW0: fmt[31:29], type[28:24], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29),
		       use_64bit ? MEM_WR_FMT_4DW_W_DATA : MEM_WR_FMT_3DW_W_DATA,
		       &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), MEM_WR_TYPE, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), 1, &header_dw[0]); /* Length = 1 DWORD of data */

	/* DW1: requester_id[31:16], tag[15:8], last_dw_be[7:4], first_dw_be[3:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), requester_bdf, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), 0, &header_dw[1]);  /* Tag = 0 */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(7, 4), 0, &header_dw[1]);   /* Last DW BE = 0 */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(3, 0), 0xF, &header_dw[1]); /* First DW BE = 0xF (all bytes valid) */

	if (use_64bit) {
		/* 4DW header format */
		/* DW2: addr[63:32] */
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 0), (uint32_t)(msi_addr >> 32), &header_dw[2]);
		/* DW3: addr[31:2], ph[1:0] (ph bits zeroed by memset above) */
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 2), (uint32_t)(msi_addr >> 2), &header_dw[3]);
		/* DW4: data[31:0] (little endian for payload) */
		header_dw[4] = msi_data;
	} else {
		/* 3DW header format */
		/* DW2: addr[31:2], ph[1:0] (ph bits zeroed by memset above) */
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 2), (uint32_t)(msi_addr >> 2), &header_dw[2]);
		/* DW3: data[31:0] (little endian for payload) */
		header_dw[3] = msi_data;
	}

	return header_dwords * sizeof(uint32_t);
}

/*
 * Send MSI interrupt by constructing and sending Memory Write TLP via DOCA API
 *
 * @tlp_ctx [in]: TLP context
 * @dsp [in]: DSP bridge device
 * @return: DOCA_SUCCESS on success
 */
doca_error_t send_msi_via_memory_write_tlp(struct tlp_context *tlp_ctx, struct pci_device_config *dsp)
{
	/* Check if MSI is enabled */
	if (!(dsp->caps.msi.message_control & 0x0001)) {
		DOCA_LOG_DBG("MSI not enabled (Message Control=0x%04X)", dsp->caps.msi.message_control);
		return DOCA_ERROR_BAD_STATE;
	}

	/* Check if Host has configured MSI address */
	uint64_t msi_addr = ((uint64_t)dsp->caps.msi.message_address_high << 32) | dsp->caps.msi.message_address_low;
	if (msi_addr == 0) {
		DOCA_LOG_DBG("MSI address not configured by Host");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Get MSI data */
	uint16_t msi_data = dsp->caps.msi.message_data;

	/* Calculate DSP's real BDF from primary_bus (updated by Host during enumeration) */
	uint16_t dsp_bdf = BDF(dsp->cfg_space_hdr.type1.primary_bus, dsp->device, dsp->function);

	DOCA_LOG_DBG("Sending MSI: addr=0x%lX, data=0x%X, requester=0x%04X", msi_addr, msi_data, dsp_bdf);

	/* Get ACG credit from queue */
	struct doca_devemu_pci_tlp_channel_req *acg_req = acg_queue_pop(tlp_ctx);
	if (acg_req == NULL) {
		DOCA_LOG_DBG("No ACG credit available, Host will poll to detect hotplug");
		return DOCA_ERROR_AGAIN;
	}

	/* Get ACG buffer to populate MMIO Write TLP */
	void *acg_buf = doca_devemu_pci_tlp_channel_req_get_acg_buf(acg_req);
	if (acg_buf == NULL) {
		DOCA_LOG_ERR("Failed to get ACG buffer from ACG request");
		doca_devemu_pci_tlp_channel_req_complete_acg(acg_req,
							     0,
							     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		return DOCA_ERROR_DRIVER;
	}

	size_t tlp_size = set_memory_write_tlp_header(acg_buf, msi_addr, msi_data, dsp_bdf);

	/* Print Memory Write TLP content for debugging */
	uint32_t *tlp_dw = (uint32_t *)acg_buf;
	bool is_64bit = (tlp_size == 20);
	DOCA_LOG_DBG("TX MemWr TLP: BDF=%04x Addr=%016lx Data=%04x [", dsp_bdf, msi_addr, msi_data);
	for (size_t i = 0; i < tlp_size / 4; i++)
		DOCA_LOG_DBG("%08x%s", DOCA_BETOH32(tlp_dw[i]), (i < tlp_size / 4 - 1) ? " " : "");
	DOCA_LOG_DBG("] %s\n", is_64bit ? "64bit" : "32bit");

	doca_devemu_pci_tlp_channel_req_complete_acg(acg_req,
						     tlp_size,
						     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_MMIO_WRITE);
	DOCA_LOG_DBG("MSI interrupt sent: bus=0x%02X addr=0x%lX data=0x%X (size: %zu)",
		     dsp->bus,
		     msi_addr,
		     msi_data,
		     tlp_size);

	return DOCA_SUCCESS;
}

/*
 * Trigger hotplug event for a DSP slot
 *
 * @tlp_ctx [in]: TLP context
 * @dsp_index [in]: DSP index (0 to num_dsp-1)
 * @plug [in]: True for plug (hotplug), false for unplug (hotunplug)
 * @return: DOCA_SUCCESS on success, error otherwise
 */
static doca_error_t trigger_hotplug_event(struct tlp_context *tlp_ctx, uint32_t dsp_index, bool plug)
{
	doca_error_t result;

	if (dsp_index >= tlp_ctx->num_dsp) {
		DOCA_LOG_ERR("Invalid DSP index: %u (max: %u)", dsp_index, tlp_ctx->num_dsp - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + dsp_index];

	struct pci_device_config *endpoint = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + dsp_index];

	/* Check pending_unplug before HPIE: during normal unplug, host clears HPIE
	 * before writing Power OFF. Without this check, plug during that window
	 * would report "HP interrupt not enabled" instead of the real reason. */
	if (plug && atomic_load(&endpoint->pending_unplug)) {
		DOCA_LOG_WARN("DSP[%u] plug rejected: unplug still in progress", dsp_index);
		return DOCA_ERROR_IN_PROGRESS;
	}

	/*
	 * Hotplug requires HP interrupt enabled (HPIE). After a timeout-forced
	 * unplug, host pciehp clears HPIE via pcie_disable_notification() during
	 * its Power OFF sequence. HPIE is re-enabled when pciehp completes its
	 * state machine and returns to OFF_STATE (via pcie_enable_notification).
	 * This naturally blocks plug until the host is ready to accept new devices.
	 */
	if (!(dsp->caps.express.slot_control & SLOT_CTRL_HP_INT_EN)) {
		if (plug && !atomic_load(&endpoint->device_present)) {
			DOCA_LOG_WARN("DSP[%u] plug: HP interrupt not enabled - host pciehp may still be "
				      "completing power off sequence, retry later",
				      dsp_index);
			return DOCA_ERROR_AGAIN;
		}
		DOCA_LOG_ERR("HP interrupt not enabled for DSP[%u], hotplug not supported", dsp_index);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Note: ACG credit availability is checked later in send_msi_via_memory_write_tlp().
	 * If no credit is available, MSI won't be sent but the device state change still
	 * proceeds - Host can poll Slot Status to detect the hotplug event. */

	if (plug) {
		if (atomic_load(&endpoint->device_present)) {
			DOCA_LOG_WARN("DSP[%u] already has device, cannot plug again", dsp_index);
			return DOCA_ERROR_ALREADY_EXIST;
		}
		atomic_store(&dsp->msi_retry_pending, false);
		atomic_store(&dsp->msi_retry_since_ns, 0);
		DOCA_LOG_INFO("Creating device for DSP[%u]", dsp_index);
		result = create_device(tlp_ctx, endpoint, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create device: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		if (!atomic_load(&endpoint->device_present)) {
			DOCA_LOG_ERR("DSP[%u] has no device, cannot unplug", dsp_index);
			return DOCA_ERROR_NOT_FOUND;
		}
		if (atomic_load(&endpoint->pending_unplug)) {
			DOCA_LOG_WARN("DSP[%u] already pending unplug", dsp_index);
			return DOCA_ERROR_IN_PROGRESS;
		}

		/*
		 * Defer unplug until the host has acknowledged the prior plug
		 * (Power ON received on this DSP, i.e. LINK_STS_DL_ACTIVE==1).
		 *
		 * Rationale:
		 *   In a batched "plug 0..N; unplug 0..N" sequence the LAST DSP's
		 *   plug MSI and unplug MSI are emitted by the worker thread back-
		 *   to-back (~sub-millisecond apart, no intervening plug work is
		 *   queued). Host pciehp coalesces the two MSIs into a single
		 *   interrupt and reads a merged slot status. Because
		 *   update_dsp_slot_status() for unplug only re-asserts ABP (which
		 *   plug already set), the merged state is indistinguishable from
		 *   a plain plug event. The unplug ABP is silently lost - host
		 *   finishes probing the device and never issues RESET/Power-OFF,
		 *   so the device stays visible on the host until the DPU's 30s
		 *   UNPLUG_TIMEOUT_SEC safety net force-destroys the DPU side
		 *   (host side remains stuck).
		 *
		 *   Waiting for DLActive=1 guarantees pciehp has already processed
		 *   the plug MSI (it issued Power ON in response), so the next ABP
		 *   we assert is observed as a fresh rising-edge event.
		 *
		 * Thread model:
		 *   This runs on the pci_cfg workqueue thread (PE2). DLActive is
		 *   flipped by the TLP thread (PE1) when Power ON arrives, so PE1
		 *   keeps progressing independently of our wait. Each DSP has its
		 *   own link_status, so only the racing DSP pays any wait cost.
		 *
		 * Memory ordering:
		 *   link_status is a plain uint16_t, not _Atomic. We use
		 *   __atomic_load_n with ACQUIRE semantics to ensure we observe
		 *   the value published by the TLP thread without relying on the
		 *   implicit barrier from usleep() syscall.
		 *
		 * Bounded wait:
		 *   Cap at UNPLUG_PLUG_ACK_WAIT_MS (2s). If the host chose not to
		 *   power on at all we proceed anyway; the 30s UNPLUG_TIMEOUT_SEC
		 *   safety net in run_progress_loop() still covers that case.
		 */
		{
			int waited_ms = 0;

			while (!(__atomic_load_n(&dsp->caps.express.link_status, __ATOMIC_ACQUIRE) &
				 LINK_STS_DL_ACTIVE) &&
			       waited_ms < UNPLUG_PLUG_ACK_WAIT_MS) {
				usleep(UNPLUG_PLUG_ACK_POLL_US);
				waited_ms++;
			}
			if (waited_ms > 0) {
				bool acked = (__atomic_load_n(&dsp->caps.express.link_status, __ATOMIC_ACQUIRE) &
					      LINK_STS_DL_ACTIVE) != 0;
				if (acked) {
					DOCA_LOG_INFO("DSP[%u] unplug: waited %dms for host plug-ack",
						      dsp_index,
						      waited_ms);
				} else {
					DOCA_LOG_WARN("DSP[%u] unplug: host did not ACK plug within %dms "
						      "(DLActive=0); proceeding - UNPLUG_TIMEOUT_SEC safety "
						      "net will force-destroy if host also ignores ABP",
						      dsp_index,
						      UNPLUG_PLUG_ACK_WAIT_MS);
				}
			}
		}

		/*
		 * Unplug flow - unified path using PCIe power off detection:
		 * 1. Set pending_unplug flag
		 * 2. Update slot status (presence detect changed)
		 * 3. Send MSI to notify host
		 * 4. Host pciehp detects presence change, starts device removal
		 * 5. Host pciehp calls pciehp_power_off_slot() -> writes SLOT_CTRL_PWR_CTRL=1
		 * 6. DPU detects power off command and destroys device
		 *
		 * Note: This works regardless of whether device was probed by driver,
		 * because pciehp operates at PCIe bus level, not device driver level.
		 */
		DOCA_LOG_INFO("Initiating unplug for DSP[%u] - waiting for host power off", dsp_index);
		if (clock_gettime(CLOCK_MONOTONIC, &endpoint->unplug_start_time) != 0) {
			DOCA_LOG_WARN("DSP[%u] clock_gettime failed - unplug timeout disabled", dsp_index);
			endpoint->unplug_start_time.tv_sec = LONG_MAX;
			endpoint->unplug_start_time.tv_nsec = 0;
		}
		atomic_store(&endpoint->pending_unplug, true);
	}

	update_dsp_slot_status(dsp, plug);

	/* Send MSI to notify Host (optional - Host can also poll Slot Status).
	 * On ACG credit exhaustion, mark the DSP for retry from the main progress
	 * loop instead of dropping the notification. Without retry, a dropped MSI
	 * for an unplug means the host relies on slow polling (or doesn't notice at
	 * all while its pciehp thread is busy), which can cascade into the 30s
	 * UNPLUG_TIMEOUT_SEC safety-net path and force-destroy. */
	result = send_msi_via_memory_write_tlp(tlp_ctx, dsp);
	if (result == DOCA_ERROR_AGAIN) {
		struct timespec retry_since = {0};
		uint64_t retry_since_ns = 0;

		DOCA_LOG_DBG("DSP[%u] %s: ACG exhausted, scheduling MSI retry", dsp_index, plug ? "plug" : "unplug");
		if (clock_gettime(CLOCK_MONOTONIC, &retry_since) == 0)
			retry_since_ns = vnet_timespec_to_ns(&retry_since);
		atomic_store(&dsp->msi_retry_since_ns, retry_since_ns);
		atomic_store(&dsp->msi_retry_pending, true);
	} else if (result != DOCA_SUCCESS) {
		/* Permanent failure (e.g. MSI disabled by host, MSI address not yet configured).
		 * For plug this is expected pre-enumeration - host will poll.
		 * For unplug it means the host may learn about the unplug late; log at DBG
		 * to avoid warning spam during rapid plug/unplug cycles. */
		DOCA_LOG_DBG("DSP[%u] %s: MSI not sent (%s) - host will poll Slot Status",
			     dsp_index,
			     plug ? "plug" : "unplug",
			     doca_error_get_descr(result));
	}

	DOCA_LOG_INFO("Hotplug: DSP[%u] %s%s",
		      dsp_index,
		      plug ? "plug" : "unplug",
		      (!plug) ? " (pending host reset)" : "");
	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_dev_execute_hotplug(struct tlp_context *tlp_ctx, uint32_t dsp_index, bool plug)
{
	return trigger_hotplug_event(tlp_ctx, dsp_index, plug);
}

/*
 * Initialize ACG queue for caching multiple ACG credits
 *
 * @tlp_ctx [in/out]: TLP context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t init_acg_queue(struct tlp_context *tlp_ctx)
{
	doca_error_t result;
	const struct doca_devinfo *devinfo = doca_dev_as_devinfo(tlp_ctx->dev);

	/* Query max ACG credits capability */
	result = doca_devemu_pci_tlp_cap_get_max_acg(devinfo, &tlp_ctx->acg_queue_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query max ACG: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_DBG("Max ACG credits: %u", tlp_ctx->acg_queue_size);

	/* Handle zero-size case: calloc(0, ...) may legally return NULL.
	 * If device reports no ACG capability, skip allocation and proceed.
	 * ACG is optional - static mode doesn't require it, hotplug uses it for MSI. */
	if (tlp_ctx->acg_queue_size == 0) {
		DOCA_LOG_WARN("Device reports 0 ACG credits - ACG queue disabled");
		tlp_ctx->acg_queue = NULL;
		tlp_ctx->acg_queue_head = 0;
		tlp_ctx->acg_queue_tail = 0;
		tlp_ctx->acg_queue_count = 0;
		return DOCA_SUCCESS;
	}

	/* Allocate circular queue for ACG credits */
	tlp_ctx->acg_queue =
		(struct doca_devemu_pci_tlp_channel_req **)calloc(tlp_ctx->acg_queue_size,
								  sizeof(struct doca_devemu_pci_tlp_channel_req *));
	if (tlp_ctx->acg_queue == NULL) {
		DOCA_LOG_ERR("Failed to allocate ACG queue");
		return DOCA_ERROR_NO_MEMORY;
	}
	tlp_ctx->acg_queue_head = 0;
	tlp_ctx->acg_queue_tail = 0;
	tlp_ctx->acg_queue_count = 0;
	DOCA_LOG_INFO("ACG queue initialized: capacity=%u", tlp_ctx->acg_queue_size);

	return DOCA_SUCCESS;
}

/*
 * Create all endpoint devices in static mode
 *
 * @tlp_ctx [in/out]: TLP context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_all_devices(struct tlp_context *tlp_ctx, enum vnet_lu_mode lu_mode)
{
	doca_error_t result;
	uint32_t i;

	if (tlp_ctx->num_ep == 0) {
		DOCA_LOG_INFO("No devices to create (bridge-only topology)");
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("Creating %u endpoint device(s)", tlp_ctx->num_ep);

	for (i = 0; i < tlp_ctx->num_ep; i++) {
		struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
		struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i];
		uint16_t ep_vhca_id = 0;

		if (vnet_lu_is_standby(lu_mode)) {
			result = vnet_lu_get_ep_vhca_id(i, &ep_vhca_id);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("LU: failed to get ep_vhca_id for device %u: %s",
					     i,
					     doca_error_get_descr(result));
				goto rollback;
			}
		}

		result = create_device(tlp_ctx, ep, ep_vhca_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create EP %u: %s", i, doca_error_get_descr(result));
			goto rollback;
		}

		/* Static mode: Set Power ON (bit10=0) and DLActive (device already present at boot) */
		dsp->caps.express.slot_control &= ~SLOT_CTRL_POWER_CONTROLLER;
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_STATE;
		dsp->caps.express.link_status |= LINK_STS_DL_ACTIVE;
		DOCA_LOG_DBG("Static Mode: DSP[%u] initialized with Power ON (bit10=0), PDS=1, DLActive=1", i);
	}

	DOCA_LOG_INFO("All %u endpoint devices created successfully", tlp_ctx->num_ep);
	return DOCA_SUCCESS;

rollback:
	/* Destroy already-created endpoints on failure */
	DOCA_LOG_INFO("Rolling back %u already-created endpoint(s)", i);
	for (uint32_t j = 0; j < i; j++) {
		struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + j];
		struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + j];

		if (atomic_load(&ep->device_present) || ep->rep || ep->tlp_dev) {
			DOCA_LOG_DBG("Destroying EP %u during rollback", j);
			(void)vnet_pci_dev_destroy_device(tlp_ctx, ep);
		}
		/* Restore DSP slot/link bits */
		dsp->caps.express.slot_control |= SLOT_CTRL_POWER_CONTROLLER;
		dsp->caps.express.slot_status &= ~SLOT_STS_PRESENCE_DETECT_STATE;
		dsp->caps.express.link_status &= ~LINK_STS_DL_ACTIVE;
	}
	return result;
}

/*
 * Initialize transaction region memory for MMIO
 * Allocates independent memory region for each PF
 *
 * @tlp_ctx [in/out]: TLP context
 * @size [in]: Size of each transaction region
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_transaction_region(struct tlp_context *tlp_ctx, size_t size)
{
	uint32_t i;

	if (tlp_ctx->num_ep == 0) {
		DOCA_LOG_INFO("No transaction regions to create (bridge-only topology)");
		return DOCA_SUCCESS;
	}

	/* Allocate array of pointers for transaction region memories */
	tlp_ctx->transaction_region_memories = (void **)calloc(tlp_ctx->num_ep, sizeof(void *));
	if (!tlp_ctx->transaction_region_memories) {
		DOCA_LOG_ERR("Failed to allocate transaction region memory array");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Allocate independent memory region for each PF */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		tlp_ctx->transaction_region_memories[i] = malloc(size);
		if (!tlp_ctx->transaction_region_memories[i]) {
			DOCA_LOG_ERR("Failed to allocate transaction region memory for PF %u", i);
			/* Cleanup already allocated regions */
			while (i-- > 0) {
				free(tlp_ctx->transaction_region_memories[i]);
				tlp_ctx->transaction_region_memories[i] = NULL;
			}
			free(tlp_ctx->transaction_region_memories);
			tlp_ctx->transaction_region_memories = NULL;
			return DOCA_ERROR_NO_MEMORY;
		}
		memset(tlp_ctx->transaction_region_memories[i], 0xAA, size);
		DOCA_LOG_INFO("Transaction region initialized for PF %u: size=%zu bytes, base_addr=%p",
			      i,
			      size,
			      tlp_ctx->transaction_region_memories[i]);
	}

	tlp_ctx->transaction_region_size = size;
	DOCA_LOG_INFO("All transaction regions initialized: %u regions, %zu bytes each", tlp_ctx->num_ep, size);
	return DOCA_SUCCESS;
}

/*
 * Initialize VirtIO network device
 *
 * @resources [in/out]: Application resources
 * @config [in]: Application configuration
 * @mac_bytes [in]: MAC address bytes
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_virtio_network_device(struct vnet_pci_dev_resources *resources,
					       struct vnet_pci_dev_config *config,
					       uint8_t *mac_bytes)
{
	struct vnet_virtio_net_config net_cfg = {0};
	struct vnet_pci_device_attrs attr = {0};
	union doca_data user_data = {0};
	bool skip_tlp_channel = vnet_lu_is_standby(config->vnet_lu_mode);
	doca_error_t result;

	/* Initialize vnet_pci_dev framework */
	result = vnet_pci_dev_init(resources->tlp_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize vnet_pci_dev framework: %s", doca_error_get_descr(result));
		return result;
	}

	/* TLP channel setup -- skipped for LU standby.
	 * The standby receives the offload engine via device LU SHM and the
	 * TLP channel via channel LU (create_from_export) after device LU. */
	if (!skip_tlp_channel) {
		/* Start vnet_pci_dev TLP channel (creates the channel) */
		result = vnet_pci_dev_start(resources->tlp_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start vnet_pci_dev: %s", doca_error_get_descr(result));
			goto cleanup_vnet_pci_dev;
		}

		/* Active LU: enable channel export for future handover */
		if (vnet_lu_is_enabled(config->vnet_lu_mode)) {
			result = vnet_lu_channel_enable_export(resources->tlp_ctx->tlp_channel);
			if (result != DOCA_SUCCESS)
				goto cleanup_vnet_pci_dev_stop;
		}

		/* Connect vnet_pci_dev event channel to progress engine */
		result = doca_pe_connect_ctx(resources->tlp_ctx->pe, vnet_pci_dev_tlp_channel_ctx(resources->tlp_ctx));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to attach vnet_pci_dev context to progress engine: %s",
				     doca_error_get_descr(result));
			goto cleanup_vnet_pci_dev_stop;
		}

		/* Set user data for TLP channel context */
		user_data.ptr = resources->tlp_ctx;
		result = doca_ctx_set_user_data(vnet_pci_dev_tlp_channel_ctx(resources->tlp_ctx), user_data);
		if (result != DOCA_SUCCESS) {
			goto cleanup_vnet_pci_dev_stop;
		}
		/* Start the TLP channel context after PE connection */
		result = doca_ctx_start(vnet_pci_dev_tlp_channel_ctx(resources->tlp_ctx));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start TLP channel context: %s", doca_error_get_descr(result));
			goto cleanup_vnet_pci_dev_stop;
		}

		/* The single-USP topology (1 USP + N DSP + N EP) requires
		 * exactly one physical NV switch TLP downstream port.
		 * N (emulated endpoints from -n) is unaffected by this check. */
		uint8_t num_nv_switch_tlp_dsp = 0;

		result = doca_devemu_pci_tlp_channel_get_num_dsp(resources->tlp_ctx->tlp_channel,
								 &num_nv_switch_tlp_dsp);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get num_dsp: %s", doca_error_get_descr(result));
			goto cleanup_vnet_pci_dev_stop;
		}

		if (num_nv_switch_tlp_dsp != 1) {
			DOCA_LOG_ERR("Expected 1 NV switch TLP DSP, got %u; "
				     "use mlxconfig to set TLP ports to 1",
				     num_nv_switch_tlp_dsp);
			result = DOCA_ERROR_NOT_SUPPORTED;
			goto cleanup_vnet_pci_dev_stop;
		}

		/* Initialize ACG queue for MSI interrupt credits */
		result = init_acg_queue(resources->tlp_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize ACG queue: %s", doca_error_get_descr(result));
			goto cleanup_vnet_pci_dev_stop;
		}
	} else {
		DOCA_LOG_INFO("LU standby: skipping TLP channel setup (channel LU handled after device LU)");
	}

	/* Initialize device topology (software configuration) */
	init_device_topology(resources->tlp_ctx);

	/* Initialize VNet controller subsystem first (required before creating devices) */
	DOCA_LOG_INFO("About to call vnet_pci_dev_vnet_controller_init");
	result = vnet_pci_dev_vnet_controller_init(resources, config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize VNet controller: %s", doca_error_get_descr(result));
		goto cleanup_vnet_pci_dev_stop;
	}

	/* Configure network device */
	memcpy(net_cfg.mac, mac_bytes, ETH_ALEN);
	net_cfg.status = VIRTIO_NET_S_LINK_UP;		       /* Link up */
	net_cfg.max_virtqueue_pairs = config->max_queue_pairs; /* Max queue pairs (VirtIO MQ) */
	net_cfg.mtu = config->mtu;
	net_cfg.speed = config->speed;
	net_cfg.duplex = config->duplex;
	net_cfg.rss_max_key_size = 40;
	net_cfg.rss_max_indirection_table_length = 128;
	net_cfg.supported_hash_types = 0x3F; /* Support common hash types */

	/* Create VirtIO network device - calculate total VQs from queue pairs
	 * NOTE: Must be done BEFORE create_all_devices() because
	 * vnet_pci_dev_vnet_controller_create() needs virtio_dev array initialized */
	attr.num_queues = config->max_queue_pairs * 2 + 1; /* RX + TX per pair + CVQ */
	attr.queue_size = config->queue_size;		   /* VirtQueue size (entries per queue) */
	attr.virtio_type = VNET_VIRTIO_NETWORK_DEVICE;
	attr.device_features = VNET_PCI_DEV_DEFAULT_FEATURES;
	attr.dev_cfg = &net_cfg;
	attr.pci_cfg_change_cb = virtio_net_ctrl_change_cb;
	attr.cb_arg = resources;

	DOCA_LOG_DBG("Creating VirtIO device with features: MAC + STATUS + CSUM + MTU (0x%lx)",
		     (unsigned long)VNET_PCI_DEV_DEFAULT_FEATURES);

	result = vnet_pci_device_create(resources->tlp_ctx, &attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VirtIO network device: %s", doca_error_get_descr(result));
		goto cleanup_vnet_controller_init;
	}

	/* Create all devices in static mode */
	if (!resources->tlp_ctx->hotplug_mode) {
		DOCA_LOG_INFO("Static Mode: Creating all %u EPs at startup", resources->tlp_ctx->num_ep);
		result = create_all_devices(resources->tlp_ctx, config->vnet_lu_mode);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create devices: %s", doca_error_get_descr(result));
			goto cleanup_virtio_device;
		}
	} else {
		DOCA_LOG_INFO("Hotplug Mode: EPs will be created dynamically");
	}

	/* Initialize transaction region for MMIO */
	result = init_transaction_region(resources->tlp_ctx, TRANSACTION_REGION_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize transaction region: %s", doca_error_get_descr(result));
		goto cleanup_virtio_device;
	}

	DOCA_LOG_INFO("VNet device and VNet controller created successfully");
	return DOCA_SUCCESS;

cleanup_virtio_device:
	for (uint32_t i = 0; i < resources->tlp_ctx->num_ep; i++) {
		vnet_pci_device_destroy(&resources->tlp_ctx->virtio_dev[i]);
	}

cleanup_vnet_controller_init:
	doca_ctx_stop(vnet_pci_dev_tlp_channel_ctx(resources->tlp_ctx));
	vnet_pci_dev_vnet_controller_uninit(resources);

cleanup_vnet_pci_dev_stop:
	vnet_pci_dev_stop(resources->tlp_ctx);
cleanup_vnet_pci_dev:
	vnet_pci_dev_reset(resources->tlp_ctx);
	return result;
}

/*
 * Cleanup VirtIO network device
 *
 * @resources [in]: Application resources
 */
static void cleanup_virtio_network_device(struct vnet_pci_dev_resources *resources)
{
	bool destroyed_all_offloads = true;
	doca_error_t result;
	uint16_t i;

	/* Shutdown workqueue first - wait for pending work to complete.
	 * This prevents UAF when work items reference dev->vqs that we're about to free. */
	pci_cfg_workqueue_shutdown();

	/* Destroy VirtIO devices (this frees vqs arrays) */
	if (resources->tlp_ctx->num_ep) {
		for (i = 0; i < resources->tlp_ctx->num_ep; i++) {
			vnet_pci_device_destroy(&resources->tlp_ctx->virtio_dev[i]);
		}
		DOCA_LOG_INFO("VNet device destroyed successfully");
	}

	if (vnet_lu_handover_was_triggered()) {
		/* LU handover: the switchover path already stopped VQs and the engine
		 * via vnet_controller_cleanup(..., false). The newer backend supports
		 * destroying the active-side offload-engine object after handover, so
		 * do that here before tearing down the process-global VNET state. */
		DOCA_LOG_INFO("LU active: destroying stopped offload engines and releasing per-process handles");
		for (i = 0; i < resources->tlp_ctx->num_ep; i++) {
			struct pci_device_config *ep =
				&resources->tlp_ctx->devs_config[FIRST_PF_IDX(resources->tlp_ctx) + i];
			struct vnet_pci_dev_controller *ctrl = &resources->tlp_ctx->vnet_controller[i];
			doca_error_t err;

			if (ctrl->offload_engine != NULL) {
				err = doca_devemu_vnet_offload_engine_destroy(ctrl->offload_engine);
				if (err != DOCA_SUCCESS) {
					DOCA_LOG_ERR("LU: offload engine destroy failed for EP %u: %s",
						     i,
						     doca_error_get_descr(err));
					destroyed_all_offloads = false;
				} else
					DOCA_LOG_INFO("LU: offload engine destroyed for EP %u", i);

				ctrl->offload_engine = NULL;
			}

			free(ctrl->rx_vqs);
			ctrl->rx_vqs = NULL;
			free(ctrl->tx_vqs);
			ctrl->tx_vqs = NULL;
			atomic_store(&ctrl->virtio_device, NULL);

			if (ep->tlp_dev) {
				doca_error_t tmp = doca_devemu_pci_tlp_dev_stop(ep->tlp_dev);

				if (tmp != DOCA_SUCCESS)
					DOCA_LOG_WARN("LU: TLP dev stop failed: %s", doca_error_get_descr(tmp));
				tmp = doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
				if (tmp != DOCA_SUCCESS)
					DOCA_LOG_WARN("LU: TLP dev destroy failed: %s", doca_error_get_descr(tmp));
				else
					DOCA_LOG_INFO("LU: TLP device destroyed for EP %u", i);
				ep->tlp_dev = NULL;
			}

			if (ep->rep) {
				doca_dev_rep_close(ep->rep);
				ep->rep = NULL;
			}
			atomic_store(&ep->device_present, false);
		}

		if (destroyed_all_offloads) {
			result = vnet_pci_dev_vnet_controller_uninit(resources);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("LU active: VNet controller uninit failed: %s",
					     doca_error_get_descr(result));
			else
				DOCA_LOG_INFO("LU active: VNet controller uninitialized after handover");
		} else
			DOCA_LOG_WARN(
				"LU active: skipping VNet controller uninit because offload-engine destroy failed");
	} else {
		/* Normal cleanup: full device destruction */
		for (i = 0; i < resources->tlp_ctx->num_ep; i++) {
			struct pci_device_config *ep =
				&resources->tlp_ctx->devs_config[FIRST_PF_IDX(resources->tlp_ctx) + i];
			if (atomic_load(&ep->device_present) || ep->rep != NULL || ep->tlp_dev != NULL) {
				DOCA_LOG_INFO("Cleaning up EP %u", i);
				(void)vnet_pci_dev_destroy_device(resources->tlp_ctx, ep);
			}
		}
		result = vnet_pci_dev_vnet_controller_uninit(resources);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("VNet controller destroy failed: %s", doca_error_get_descr(result));
		else
			DOCA_LOG_INFO("VNet controller destroyed successfully");
	}

	if (resources->tlp_ctx->acg_queue != NULL) {
		struct doca_devemu_pci_tlp_channel_req *acg_req;
		uint16_t count = 0;
		while ((acg_req = acg_queue_pop(resources->tlp_ctx)) != NULL) {
			doca_devemu_pci_tlp_channel_req_complete_acg(
				acg_req,
				0,
				DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
			count++;
		}
		DOCA_LOG_DBG("Flushed %u ACG credits", count);
	}

	/* Stop TLP context and wait for it to become idle.
	 * Similar to IO context cleanup, we must drain to IDLE before destroy. */
	struct doca_ctx *tlp_channel_ctx = vnet_pci_dev_tlp_channel_ctx(resources->tlp_ctx);
	if (tlp_channel_ctx) {
		doca_error_t stop_err = doca_ctx_stop(tlp_channel_ctx);
		if (stop_err == DOCA_ERROR_IN_PROGRESS) {
			enum doca_ctx_states ctx_state = DOCA_CTX_STATE_STARTING;
			int drain_iterations = 0;

			DOCA_LOG_DBG("TLP channel stop in progress - draining to IDLE");
			do {
				(void)doca_pe_progress(resources->tlp_ctx->pe);
				stop_err = doca_ctx_get_state(tlp_channel_ctx, &ctx_state);
				if (stop_err != DOCA_SUCCESS)
					break;
				drain_iterations++;
			} while (ctx_state != DOCA_CTX_STATE_IDLE &&
				 drain_iterations < VNET_PCI_DEV_PE_DRAIN_MAX_ITERATIONS);

			if (drain_iterations >= VNET_PCI_DEV_PE_DRAIN_MAX_ITERATIONS) {
				DOCA_LOG_WARN("TLP channel drain timeout (state=%d)", ctx_state);
			} else if (ctx_state == DOCA_CTX_STATE_IDLE) {
				DOCA_LOG_DBG("TLP channel drained to IDLE after %d iterations", drain_iterations);
			}
		} else if (stop_err != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop TLP channel: %s", doca_error_get_descr(stop_err));
		}
	}

	/* Destroy TLP channel (context is now idle) */
	vnet_pci_dev_stop(resources->tlp_ctx);

	/* Reset vnet_pci_dev (destroys PCI type) */
	vnet_pci_dev_reset(resources->tlp_ctx);

	/* Close global log files - only here during final cleanup */
	close_stats_log_files();

	DOCA_LOG_INFO("VNet device cleaned up successfully");
}

/*
 * Initialize TLP context - allocate memory for context structure
 *
 * @tlp_ctx [out]: TLP context to initialize
 * @num_ep [in]: Number of endpoints to create
 * @hotplug_mode [in]: Hotplug mode flag
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_tlp_context(struct tlp_context **tlp_ctx, uint32_t num_ep, bool hotplug_mode)
{
	struct tlp_context *ctx;
	uint32_t num_dsp = num_ep;
	uint32_t num_bridges = MAX_NUM_USP + num_dsp;
	uint32_t num_pci_devices = num_bridges + num_ep + DUMMY_DEV_NUM;
	int ret;

	/* Validate num_ep to prevent stack overflow in vnet_pci_dev_run() worker_pes array */
	if (num_ep == 0 || num_ep > MAX_NUM_EP) {
		DOCA_LOG_ERR("Invalid num_ep=%u, must be 1..%u", num_ep, MAX_NUM_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx = (struct tlp_context *)calloc(1, sizeof(struct tlp_context));
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to allocate TLP context");
		return DOCA_ERROR_NO_MEMORY;
	}

	ret = pthread_mutex_init(&ctx->acg_queue_lock, NULL);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to initialize ACG queue mutex: %d", ret);
		free(ctx);
		return DOCA_ERROR_INITIALIZATION;
	}

	ctx->num_ep = num_ep;
	ctx->num_dsp = num_dsp;
	ctx->num_bridges = num_bridges;
	ctx->num_devices = num_pci_devices;
	ctx->hotplug_mode = hotplug_mode;

	DOCA_LOG_INFO("Initializing TLP context: %u EP, %u DSP, %u total bridges, %u total devices",
		      num_ep,
		      num_dsp,
		      num_bridges,
		      num_pci_devices);

	/* Allocate device array - reps and tlp_devs will be stored in pci_device_config */
	ctx->devs_config = (struct pci_device_config *)calloc(num_pci_devices, sizeof(struct pci_device_config));
	if (ctx->devs_config == NULL) {
		DOCA_LOG_ERR("Failed to allocate device array");
		goto alloc_devs_config_failed;
	}

	/* Initialize endpoint RW-locks immediately after allocation.
	 * This ensures tlp_ctx_cleanup() can safely call pthread_rwlock_destroy()
	 * even on early failure paths before init_device_topology() runs. */
	for (uint32_t i = 0; i < num_ep; i++) {
		struct pci_device_config *ep = &ctx->devs_config[num_bridges + i];
		pthread_rwlock_init(&ep->endpoint_lock, NULL);
	}

	/* Initialize BDF map for fast device lookup */
	memset(ctx->bdf_map, 0, sizeof(ctx->bdf_map));

	/* Allocate BDF map entries - one per device */
	ctx->bdf_entries = (struct bdf_map_entry *)calloc(num_pci_devices, sizeof(struct bdf_map_entry));
	if (ctx->bdf_entries == NULL) {
		DOCA_LOG_ERR("Failed to allocate BDF map entries");
		goto alloc_bdf_entries_failed;
	}

	/* Allocate VirtIO device array */
	ctx->virtio_dev = (struct vnet_pci_device *)calloc(num_ep, sizeof(struct vnet_pci_device));
	if (ctx->virtio_dev == NULL) {
		DOCA_LOG_ERR("Failed to allocate VirtIO device array");
		goto alloc_virtio_dev_failed;
	}

	/* Allocate VNet controller array */
	ctx->vnet_controller = (struct vnet_pci_dev_controller *)calloc(num_ep, sizeof(struct vnet_pci_dev_controller));
	if (ctx->vnet_controller == NULL) {
		DOCA_LOG_ERR("Failed to allocate VNet controller array");
		goto alloc_vnet_controller_failed;
	}

	/* Initialize mutexes and defaults immediately after allocation */
	for (uint32_t i = 0; i < num_ep; i++) {
		pthread_mutex_init(&ctx->vnet_controller[i].stats_ref_mutex, NULL);
		ctx->vnet_controller[i].config_msix_vector_cached = VIRTIO_MSI_NO_VECTOR;
	}

	/* ACG queue will be initialized after querying device capabilities */
	ctx->acg_queue = NULL;
	ctx->acg_queue_head = 0;
	ctx->acg_queue_tail = 0;
	ctx->acg_queue_count = 0;
	ctx->acg_queue_size = 0;
	ctx->queue_size = 0;

	*tlp_ctx = ctx;
	return DOCA_SUCCESS;
alloc_vnet_controller_failed:
	free(ctx->virtio_dev);
alloc_virtio_dev_failed:
	free(ctx->bdf_entries);
alloc_bdf_entries_failed:
	/* Destroy rwlocks before freeing devs_config to avoid resource leak */
	for (uint32_t i = 0; i < num_ep; i++) {
		struct pci_device_config *ep = &ctx->devs_config[num_bridges + i];
		pthread_rwlock_destroy(&ep->endpoint_lock);
	}
	free(ctx->devs_config);
alloc_devs_config_failed:
	pthread_mutex_destroy(&ctx->acg_queue_lock);
	free(ctx);
	return DOCA_ERROR_NO_MEMORY;
}

/*
 * Cleanup resources
 *
 * @tlp_ctx [in/out]: TLP context to clean up
 */
static void tlp_ctx_cleanup(struct tlp_context *tlp_ctx)
{
	uint32_t i;

	if (tlp_ctx == NULL)
		return;

	if (tlp_ctx->dev != NULL) {
		doca_error_t result = doca_dev_close(tlp_ctx->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(result));
		tlp_ctx->dev = NULL;
	}

	/* ibv handles from LU device reconstruction; must be freed after doca_dev_close() */
	if (tlp_ctx->imported_ibv_pd != NULL) {
		(void)ibv_dealloc_pd(tlp_ctx->imported_ibv_pd);
		tlp_ctx->imported_ibv_pd = NULL;
	}
	if (tlp_ctx->imported_ibv_ctx != NULL) {
		(void)ibv_close_device(tlp_ctx->imported_ibv_ctx);
		tlp_ctx->imported_ibv_ctx = NULL;
	}

	if (tlp_ctx->acg_queue != NULL) {
		free(tlp_ctx->acg_queue);
		tlp_ctx->acg_queue = NULL;
	}
	pthread_mutex_destroy(&tlp_ctx->acg_queue_lock);

	if (tlp_ctx->devs_config != NULL) {
		/* Destroy RW-locks for all endpoint devices before freeing the array */
		for (i = 0; i < tlp_ctx->num_ep; i++) {
			struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
			pthread_rwlock_destroy(&ep->endpoint_lock);
		}
		free(tlp_ctx->devs_config);
		tlp_ctx->devs_config = NULL;
	}

	/* Free BDF map entries */
	if (tlp_ctx->bdf_entries != NULL) {
		free(tlp_ctx->bdf_entries);
		tlp_ctx->bdf_entries = NULL;
		memset(tlp_ctx->bdf_map, 0, sizeof(tlp_ctx->bdf_map));
	}

	/* Free VNet controller array - destroy mutexes first */
	if (tlp_ctx->vnet_controller != NULL) {
		for (i = 0; i < tlp_ctx->num_ep; i++) {
			pthread_mutex_destroy(&tlp_ctx->vnet_controller[i].stats_ref_mutex);
		}
		free(tlp_ctx->vnet_controller);
		tlp_ctx->vnet_controller = NULL;
	}

	/* Free VirtIO device array */
	if (tlp_ctx->virtio_dev != NULL) {
		free(tlp_ctx->virtio_dev);
		tlp_ctx->virtio_dev = NULL;
	}

	/* Free all transaction region memories for each PF */
	if (tlp_ctx->transaction_region_memories != NULL) {
		for (i = 0; i < tlp_ctx->num_ep; i++) {
			if (tlp_ctx->transaction_region_memories[i] != NULL) {
				free(tlp_ctx->transaction_region_memories[i]);
				tlp_ctx->transaction_region_memories[i] = NULL;
			}
		}
		free(tlp_ctx->transaction_region_memories);
		tlp_ctx->transaction_region_memories = NULL;
		tlp_ctx->transaction_region_size = 0;
	}

	free(tlp_ctx);

	DOCA_LOG_INFO("Cleanup completed");
}

static bool tlp_ctx_has_pending_unplug(struct tlp_context *tlp_ctx)
{
	for (uint32_t i = 0; i < tlp_ctx->num_ep; i++) {
		struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];

		if (atomic_load(&ep->pending_unplug))
			return true;
	}

	return false;
}

/*
 * Drain any pending slot-event MSI retries across all DSPs.
 *
 * Called from the main progress loop after doca_pe_progress() so that ACG
 * credits replenished by the FW are immediately visible to the retry attempt.
 * Each pending DSP is retried at most once per progress iteration; AGAIN leaves
 * the flag set for the next iteration, SUCCESS clears it, and any other error
 * or exceeding MSI_RETRY_ABANDON_SEC abandons the retry (host falls back to
 * polling Slot Status - it still sees ABP set, so ABP-based hotplug semantics
 * remain correct, just delayed).
 *
 * @tlp_ctx [in]: TLP context
 */
static void drain_msi_retries(struct tlp_context *tlp_ctx)
{
	struct timespec now_ts;
	uint64_t now_ns = 0;
	bool have_now = (clock_gettime(CLOCK_MONOTONIC, &now_ts) == 0);

	if (have_now)
		now_ns = vnet_timespec_to_ns(&now_ts);

	for (uint32_t i = 0; i < tlp_ctx->num_dsp; i++) {
		struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i];
		doca_error_t result;

		if (!atomic_load(&dsp->msi_retry_pending))
			continue;

		result = send_msi_via_memory_write_tlp(tlp_ctx, dsp);
		if (result == DOCA_SUCCESS) {
			atomic_store(&dsp->msi_retry_pending, false);
			atomic_store(&dsp->msi_retry_since_ns, 0);
			DOCA_LOG_DBG("DSP[%u] MSI retry succeeded", i);
			continue;
		}
		if (result == DOCA_ERROR_AGAIN) {
			uint64_t retry_since_ns = atomic_load(&dsp->msi_retry_since_ns);

			/* Still out of credits - check abandonment window */
			if (have_now && retry_since_ns != 0 && now_ns >= retry_since_ns) {
				uint64_t elapsed_ns = now_ns - retry_since_ns;

				if (elapsed_ns >= (uint64_t)MSI_RETRY_ABANDON_SEC * VNET_NSEC_PER_SEC) {
					uint64_t elapsed_sec = elapsed_ns / VNET_NSEC_PER_SEC;

					atomic_store(&dsp->msi_retry_pending, false);
					atomic_store(&dsp->msi_retry_since_ns, 0);
					DOCA_LOG_DBG("DSP[%u] MSI retry abandoned after %" PRIu64 "s - "
						     "host will poll Slot Status",
						     i,
						     elapsed_sec);
				}
			}
			continue;
		}
		/* Permanent error (e.g. MSI disabled, address cleared) - stop retrying */
		atomic_store(&dsp->msi_retry_pending, false);
		atomic_store(&dsp->msi_retry_since_ns, 0);
		DOCA_LOG_DBG("DSP[%u] MSI retry stopped: %s", i, doca_error_get_descr(result));
	}
}

/*
 * Run the main progress loop
 *
 * Dual PE Architecture:
 * - PE1 (main PE): Handles TLP events (time-critical, < 1ms latency required)
 * - PE2 (worker PE): Handles heavy operations (VQ lifecycle, IO context)
 *
 * Main thread only drives PE1 for TLP handling. Worker thread drives PE2
 * independently, so no cross-thread PE dependency and no deadlock risk.
 *
 * @resources [in]: Application resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_progress_loop(struct vnet_pci_dev_resources *resources)
{
	bool shutdown_wait_logged = false;
	bool shutdown_wait_started = false;
	struct timespec shutdown_wait_start = {0};

	vnet_pci_device_pin_main_thread();
	DOCA_LOG_INFO("VNet device ready - waiting for host PCIe enumeration...");
	DOCA_LOG_INFO("Dual PE architecture: PE1=TLP handling (main), PE2=heavy ops (worker)");
	pci_cfg_workqueue_set_diag_collection(resources->tlp_ctx, true);

	while (true) {
		bool quit_requested = *(resources->force_quit);
		bool pending_hot_unplug = tlp_ctx_has_pending_unplug(resources->tlp_ctx);

		if (quit_requested && !pending_hot_unplug)
			break;

		/* Check if live update handover was requested via SIGUSR1 */
		if (vnet_lu_handover_was_triggered()) {
			DOCA_LOG_INFO("SIGUSR1 received -- exiting progress loop for handover");
			break;
		}

		/* Drive main PE (PE1) for TLP handling - no synchronization needed
		 * Worker thread drives PE2 independently for heavy operations */
		(void)doca_pe_progress(resources->tlp_ctx->pe);

		/* Retry any slot-event MSIs that were dropped due to ACG credit
		 * exhaustion. Done right after PE progress so credits replenished
		 * via the ACG callback are visible here. Keeps the host in sync
		 * with plug/unplug events without relying on Slot Status polling. */
		drain_msi_retries(resources->tlp_ctx);

		/* Diagnostics are collected on the worker thread only.
		 * Keeping PE1 dedicated to TLP progress avoids completion latency spikes
		 * when the host concurrently probes config/VPD/capability space. */

		/* Check for unplug timeout: if an endpoint has been pending_unplug
		 * for longer than UNPLUG_TIMEOUT_SEC without host writing Power OFF,
		 * force-complete the unplug from the DPU side. This handles the case
		 * where the host driver enters FAILED state and never powers off the slot. */
		if (resources->tlp_ctx->hotplug_mode) {
			struct timespec now;
			static bool clock_warned;
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
				if (!clock_warned) {
					DOCA_LOG_WARN("clock_gettime failed - unplug timeout check skipped");
					clock_warned = true;
				}
				goto skip_timeout_check;
			}
			for (uint32_t i = 0; i < resources->tlp_ctx->num_ep; i++) {
				struct pci_device_config *ep =
					&resources->tlp_ctx->devs_config[FIRST_PF_IDX(resources->tlp_ctx) + i];
				if (!atomic_load(&ep->pending_unplug) || atomic_load(&ep->pending_destroy))
					continue;
				long elapsed = (now.tv_sec - ep->unplug_start_time.tv_sec);
				if (elapsed < UNPLUG_TIMEOUT_SEC)
					continue;

				DOCA_LOG_WARN("DSP[%u] unplug timeout (%lds) - forcing device removal from DPU",
					      i,
					      elapsed);

				/* Force device_status=0 so host sees RESET if it tries to use the device */
				struct vnet_pci_device *vdev = &resources->tlp_ctx->virtio_dev[i];
				vnet_pci_dev_force_reset(vnet_pci_device_get_pci_cfg(vdev), vdev);

				/* Clear DLActive on the DSP bridge so host sees link-down */
				struct pci_device_config *dsp =
					&resources->tlp_ctx->devs_config[FIRST_DSP_IDX(resources->tlp_ctx) + i];
				dsp->caps.express.link_status &= ~LINK_STS_DL_ACTIVE;

				/* Clear Presence Detect State so host sees slot empty */
				dsp->caps.express.slot_status &= ~SLOT_STS_PRESENCE_DETECT_STATE;
				dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_CHANGED;

				pci_cfg_workqueue_submit_delayed_destroy(resources->tlp_ctx, ep);
				if (atomic_load(&ep->pending_destroy)) {
					/* Enqueue succeeded - reset timer for retry backoff if destroy fails */
					ep->unplug_start_time = now;
					DOCA_LOG_INFO("DSP[%u] forced unplug: delayed_destroy submitted", i);
				} else {
					DOCA_LOG_WARN("DSP[%u] forced unplug: failed to enqueue destroy, "
						      "will retry next loop",
						      i);
				}
			}
skip_timeout_check:;
		}

		quit_requested = *(resources->force_quit);
		pending_hot_unplug = tlp_ctx_has_pending_unplug(resources->tlp_ctx);

		if (quit_requested && pending_hot_unplug) {
			struct timespec now;
			long shutdown_elapsed;

			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
				DOCA_LOG_WARN("clock_gettime failed during shutdown wait - proceeding with cleanup");
				break;
			}

			if (!shutdown_wait_started) {
				shutdown_wait_start = now;
				shutdown_wait_started = true;
			}

			if (!shutdown_wait_logged) {
				DOCA_LOG_INFO("Shutdown requested - waiting for pending hot-unplug slots to complete");
				shutdown_wait_logged = true;
			}

			shutdown_elapsed = now.tv_sec - shutdown_wait_start.tv_sec;
			if (shutdown_elapsed >= UNPLUG_TIMEOUT_SEC) {
				DOCA_LOG_WARN("Shutdown wait timed out after %lds - proceeding with cleanup",
					      shutdown_elapsed);
				break;
			}

			continue;
		}

		/* Check for user input (non-blocking).
		 * CLI commands are submitted to the worker thread so the main thread
		 * NEVER blocks. This ensures doca_pe_progress() keeps running and
		 * host TLPs are processed within PCIe Completion Timeout windows.
		 * Speed commands are always available; hotplug requires hotplug_mode. */
		if (!quit_requested && stdin_has_input()) {
			struct cli_command cmd;
			doca_error_t ret = read_cli_command(resources->tlp_ctx->num_ep, &cmd);
			if (ret == DOCA_SUCCESS) {
				switch (cmd.type) {
				case CLI_CMD_PLUG:
				case CLI_CMD_UNPLUG:
					if (!resources->tlp_ctx->hotplug_mode) {
						DOCA_LOG_ERR("Hotplug commands require --hotplug mode");
						break;
					}
					DOCA_LOG_INFO("%s on DSP[%u] (queuing to worker thread)",
						      cmd.type == CLI_CMD_PLUG ? "PLUG" : "UNPLUG",
						      cmd.ep_index);
					pci_cfg_workqueue_submit_hotplug(resources->tlp_ctx,
									 cmd.ep_index,
									 cmd.type == CLI_CMD_PLUG);
					break;
				case CLI_CMD_SPEED:
					DOCA_LOG_INFO("Speed change EP[%u] to %u Mbps (queuing to worker)",
						      cmd.ep_index,
						      cmd.speed);
					pci_cfg_workqueue_submit_speed_change(
						&resources->tlp_ctx->vnet_controller[cmd.ep_index],
						cmd.speed);
					break;
				}
			}
		}
	}

	pci_cfg_workqueue_set_diag_collection(resources->tlp_ctx, false);
	DOCA_LOG_INFO("Exiting progress loop...");
	return DOCA_SUCCESS;
}

/**
 * Complete standby restore: send DEV_ACK to active, perform channel LU
 * (export receive, config apply, primary takeover), initialize ACG queue,
 * close the UDS connection, and transition to chainable-active mode.
 */
static void vnet_lu_standby_post_restore(struct vnet_pci_dev_config *config, struct vnet_pci_dev_resources *resources)
{
	doca_error_t result;

	result = vnet_lu_restore_complete();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send DEV_ACK: %s (skipping channel LU)", doca_error_get_descr(result));
		goto transition;
	}

	result = vnet_lu_channel_restore(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Channel LU restore failed: %s", doca_error_get_descr(result));
		goto transition;
	}

	result = init_acg_queue(resources->tlp_ctx);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("Channel LU: ACG queue init failed: %s", doca_error_get_descr(result));

transition:
	vnet_lu_close_conn();

	DOCA_LOG_INFO("Transitioning from standby to active mode (chainable LU)");
	config->vnet_lu_mode = VNET_LU_MODE_ACTIVE;
	result = vnet_lu_active_init();
	if (result != DOCA_SUCCESS) {
		config->vnet_lu_mode = VNET_LU_MODE_NONE;
		DOCA_LOG_WARN("Failed to init active mode after restore: %s", doca_error_get_descr(result));
	}
	DOCA_LOG_INFO("LU restore complete -- process is active (pid=%d)", getpid());
}

doca_error_t vnet_pci_dev_run(struct vnet_pci_dev_config *config, volatile bool *force_quit)
{
	struct vnet_pci_dev_resources resources = {0};
	doca_error_t result = DOCA_SUCCESS;
	uint8_t mac_bytes[ETH_ALEN];

	DOCA_LOG_INFO("Initializing VNet device: 1 USP + %u DSPs + %u EPs (%s mode)",
		      config->num_ep,
		      config->num_ep,
		      config->hotplug_mode ? "Hotplug" : "Static");

	/* Store force quit reference */
	resources.force_quit = force_quit;

	/* Initialize TLP context - allocate memory */
	result = init_tlp_context(&resources.tlp_ctx, config->num_ep, config->hotplug_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize TLP context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Parse MAC address */
	result = parse_mac_address(config->mac_addr, mac_bytes);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse MAC address: %s", doca_error_get_descr(result));
		goto error;
	}

	/* === LU Phase 1 (pre-copy): App_A still serving traffic.
	 * Receive cmd_fd + SHM, reconstruct device, run heavyweight init. === */
	if (vnet_lu_is_standby(config->vnet_lu_mode)) {
		result = vnet_lu_restore_early(&resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to restore from active: %s", doca_error_get_descr(result));
			goto error;
		}
		vnet_lu_override_config(config, resources.tlp_ctx, mac_bytes);
	} else {
		result = find_doca_device(config->pci_address, config->ibdev_name, &resources.tlp_ctx->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to find DOCA device: %s", doca_error_get_descr(result));
			goto error;
		}
	}

	/* Initialize main progress engine (PE1) for TLP handling */
	result = init_progress_engine(resources.tlp_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize progress engine: %s", doca_error_get_descr(result));
		goto error;
	}

	vnet_pci_device_configure_affinity(config->tlp_core_idx, config->worker_core_idx, config->mq_core_idx);

	/* Initialize VirtIO network device (this also initializes workqueue and controllers) */
	result = init_virtio_network_device(&resources, config, mac_bytes);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize VirtIO network device: %s", doca_error_get_descr(result));
		goto cleanup_pe;
	}

	/* Initialize worker progress engines (PE2) for each controller.
	 * Must be done AFTER init_virtio_network_device which initializes controllers.
	 * Each controller gets its own worker PE for heavy operations like
	 * IO context cleanup, VQ lifecycle, etc. */
	for (uint32_t i = 0; i < config->num_ep; i++) {
		struct vnet_pci_dev_controller *ctrl = &resources.tlp_ctx->vnet_controller[i];
		result = doca_pe_create(&ctrl->worker_pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create worker PE for controller %u: %s",
				     i,
				     doca_error_get_descr(result));
			/* Cleanup already created worker PEs */
			for (uint32_t j = 0; j < i; j++) {
				if (resources.tlp_ctx->vnet_controller[j].worker_pe) {
					doca_pe_destroy(resources.tlp_ctx->vnet_controller[j].worker_pe);
					resources.tlp_ctx->vnet_controller[j].worker_pe = NULL;
				}
			}
			goto cleanup_virtio;
		}
		DOCA_LOG_DBG("Worker PE (PE2) created for controller %u", i);
	}
	DOCA_LOG_INFO("Worker progress engines (PE2) initialized for %u controllers", config->num_ep);

	if (vnet_lu_is_standby(config->vnet_lu_mode)) {
		/* Phase 1 replay: everything except oe_enable. */
		result = vnet_lu_apply_shm_replay(&resources, config);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("LU phase1 replay failed: %s", doca_error_get_descr(result));
			goto cleanup_virtio;
		}
		/* Phase 1 complete: signal active to start switchover. */
		result = vnet_lu_phase1_send_ready();
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send ready signal: %s", doca_error_get_descr(result));
			goto cleanup_virtio;
		}

		/* Phase 2: parallel per-device enable (one thread per 'G'). */
		result = vnet_lu_phase2_enable_engines(&resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("LU phase2 enable failed: %s", doca_error_get_descr(result));
			goto cleanup_virtio;
		}
	}

	/* Pass all controller worker PEs to workqueue for async operations.
	 * Multi-endpoint mode: each controller has its own worker PE and IO contexts
	 * are attached to their respective PEs. Workqueue must progress ALL worker PEs
	 * to avoid stalling controllers 1..N. */
	{
		struct doca_pe *worker_pes[MAX_NUM_EP];
		for (uint32_t j = 0; j < config->num_ep; j++)
			worker_pes[j] = resources.tlp_ctx->vnet_controller[j].worker_pe;
		pci_cfg_workqueue_set_worker_pes(worker_pes, config->num_ep);
	}

	/* LU standby: stats creation is async, requires workqueue PEs. */
	if (vnet_lu_is_standby(config->vnet_lu_mode)) {
		for (uint32_t i = 0; i < config->num_ep; i++)
			pci_cfg_workqueue_submit_create_stats_list(&resources.tlp_ctx->vnet_controller[i]);
	}

	DOCA_LOG_INFO("VNet device initialized successfully");
	DOCA_LOG_DBG("  MAC=%s, MTU=%u", config->mac_addr, config->mtu);

	/* ACTIVE: init now. STANDBY defers to post-restore below. */
	if (config->vnet_lu_mode == VNET_LU_MODE_ACTIVE) {
		result = vnet_lu_active_init();
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize active mode: %s", doca_error_get_descr(result));
			goto cleanup_virtio;
		}
	}

	if (config->hotplug_mode) {
		DOCA_LOG_INFO("Hotplug control (enter commands):");
		DOCA_LOG_INFO("  plug <DSP_IDX>   - Plug device to DSP slot");
		DOCA_LOG_INFO("  unplug <DSP_IDX> - Unplug device from DSP slot");
		DOCA_LOG_INFO("Example: plug 0");
	}

	if (vnet_lu_is_standby(config->vnet_lu_mode))
		vnet_lu_standby_post_restore(config, &resources);

	/* Run progress loop */
	result = run_progress_loop(&resources);

	/* Active post-loop: execute handover (if SIGUSR1) + cleanup. */
	if (vnet_lu_is_enabled(config->vnet_lu_mode)) {
		if (vnet_lu_active_post_loop(&resources)) {
			DOCA_LOG_INFO("LU handover complete -- process is idle (pid=%d), Ctrl+C to exit", getpid());
			while (!*resources.force_quit)
				sleep(1);
		}
	}

cleanup_virtio:
	/* Cleanup on normal exit.
	 * IMPORTANT: Must shutdown workqueue BEFORE destroying worker PEs.
	 * cleanup_virtio_network_device() calls pci_cfg_workqueue_shutdown() which
	 * joins the worker thread. Worker thread may be progressing PEs, so we
	 * must wait for it to exit before destroying the PEs it's using. */
	cleanup_virtio_network_device(&resources);

	/* Now safe to destroy worker PEs - worker thread has exited */
	pci_cfg_workqueue_clear_worker_pes();
	for (uint32_t i = 0; i < config->num_ep; i++) {
		if (resources.tlp_ctx->vnet_controller[i].worker_pe) {
			doca_pe_destroy(resources.tlp_ctx->vnet_controller[i].worker_pe);
			resources.tlp_ctx->vnet_controller[i].worker_pe = NULL;
		}
	}
	DOCA_LOG_INFO("Worker progress engines cleaned up");

cleanup_pe:
	cleanup_progress_engine(resources.tlp_ctx);

error:
	tlp_ctx_cleanup(resources.tlp_ctx);

	/* Live update standby cleanup is idempotent when no restore was done. */
	vnet_lu_close_conn();

	DOCA_LOG_INFO("VirtIO Net PCI device cleanup completed");
	return result;
}
