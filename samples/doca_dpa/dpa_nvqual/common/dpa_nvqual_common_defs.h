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

#ifndef DPA_NVQUAL_COMMON_DEFS_H_
#define DPA_NVQUAL_COMMON_DEFS_H_

#include <stdbool.h>

#include <doca_sync_event.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPA sample application
 */
extern struct doca_dpa_app *dpa_sample_app;

/**
 * DPA frequency in MHz
 */
#define DPA_NVQUAL_DPA_FREQ (1800.00f)

/**
 * Size of copy bytes latency microseconds
 */
#define DPA_NVQUAL_COPY_BYTE_LATENCY_USEC (0.1f)

/**
 * Watchdog time seconds to be calculated for iteration duration
 */
#define DPA_NVQUAL_WATCHDOG_TIME_SEC (2 * 60)

/**
 * Size to multiply to convert from seconds to microseconds
 */
#define DPA_NVQUAL_SEC_TO_USEC (1000000)

/**
 * Seconds in hour
 */
#define DPA_NVQUAL_SEC_IN_HOUR (3600)

/**
 * Seconds in minute
 */
#define DPA_NVQUAL_SEC_IN_MINUTE (60)

/**
 * Default factor to be calculated for iteration duration
 */
#define DPA_NVQUAL_FACTOR (0.75f)

/**
 * Default iteration duration seconds
 */
#define DPA_NVQUAL_ITERATION_DURATION_SEC (DPA_NVQUAL_WATCHDOG_TIME_SEC * DPA_NVQUAL_FACTOR)

/**
 * Default allocated DPA heap size for flow configuration
 */
#define DPA_NVQUAL_ALLOCATED_DPA_HEAP_SIZE (268435456)

/**
 * Max possible EUs
 */
#define DPA_NVQUAL_MAX_EUS (256)

/**
 * Buffer size to hold printing sample data
 */
#define DPA_NVQUAL_PRINT_BUFFER_SIZE (2048)

/**
 * Default size of input of excluded EUs
 */
#define DPA_NVQUAL_MAX_INPUT_EXCLUDED_EUS_SIZE (1024)

/**
 * Buffer size to hold Infiniband/RoCE device name. Including a null terminator.
 */
#define DPA_NVQUAL_DOCA_DEVINFO_IBDEV_NAME_SIZE 64

/**
 * Sync event mask
 */
#define DPA_NVQUAL_SYNC_EVENT_MASK (0xffffffffffffffff)

/**
 * DPA nvqual flow configuration struct
 */
struct dpa_nvqual_flow_config {
	bool excluded_eus[DPA_NVQUAL_MAX_EUS];
	unsigned int excluded_eus_size;
	const char *dev_name;
	uint64_t test_duration_sec;
	uint64_t iteration_duration_sec;
	uint64_t allocated_dpa_heap_size;
};

/**
 * DPA nvqual output struct
 */
struct dpa_nvqual_run_output {
	bool available_eus[DPA_NVQUAL_MAX_EUS];
	unsigned int available_eus_size;
	uint64_t total_dpa_run_time_sec;
	unsigned int total_num_eus;
};

/**
 * DPA nvqual thread local storage struct
 */
struct dpa_nvqual_tls {
	unsigned int eu;
	doca_dpa_dev_uintptr_t thread_ret;
	doca_dpa_dev_uintptr_t src_buf;
	doca_dpa_dev_uintptr_t dst_buf;
	size_t buffers_size;
	uint64_t num_ops;
	doca_dpa_dev_sync_event_t dev_se;
};

/**
 * DPA nvqual thread's return values struct
 */
struct dpa_nvqual_thread_ret {
	unsigned int eu;
	uint64_t val;
};

/**
 * DPA nvqual struct
 */
struct dpa_nvqual {
	struct doca_devinfo **dev_list;
	struct doca_dev *dev;
	struct doca_dpa *dpa;
	struct doca_dpa_eu_affinity *affinity;
	struct dpa_nvqual_tls tlss[DPA_NVQUAL_MAX_EUS];
	doca_dpa_dev_uintptr_t dev_tlss[DPA_NVQUAL_MAX_EUS];
	struct doca_dpa_thread *threads[DPA_NVQUAL_MAX_EUS];
	doca_dpa_dev_uintptr_t thread_rets;
	struct doca_dpa_notification_completion *notification_completions[DPA_NVQUAL_MAX_EUS];
	doca_dpa_dev_uintptr_t dev_notification_completions;
	uint64_t num_threads;
	struct doca_sync_event *se;
	doca_dpa_dev_sync_event_t dev_se;
	bool available_eus[DPA_NVQUAL_MAX_EUS];
	unsigned int available_eus_size;
	unsigned int total_num_eus;
	size_t buffers_size;
	uint64_t num_ops;
	uint64_t total_dpa_run_time_usec;
	struct dpa_nvqual_flow_config flow_cfg;
	uint32_t avg_latency_single_op; // float
	uint64_t data_size;
};

/**
 * DPA nvqual parameters configuration struct
 */
struct dpa_nvqual_config {
	char dev_name[DPA_NVQUAL_DOCA_DEVINFO_IBDEV_NAME_SIZE];
	uint64_t test_duration_sec;
	uint32_t user_factor; // float
	bool excluded_eus[DPA_NVQUAL_MAX_EUS];
	unsigned int excluded_eus_size;
};

#ifdef __cplusplus
}
#endif

#endif /* DPA_NVQUAL_COMMON_DEFS_H_ */