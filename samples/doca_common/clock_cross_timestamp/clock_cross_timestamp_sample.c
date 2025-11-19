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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>

#include "clock_cross_timestamp_sample.h"
#include <samples/common.h>

DOCA_LOG_REGISTER(CLOCK_CROSS_TIMESTAMP::SAMPLE);

/*
 * Get ASCII representation of clock from lib define value
 *
 * @clock_type [in]: uint64_t lib representation of clock
 * @return: ASCII string representation of clock
 */
static char *get_clock_name(uint64_t clock_type)
{
	switch (clock_type) {
	case DOCA_CLOCK_NIC_FREE_RUNNING:
		return "DOCA_CLOCK_NIC_FREE_RUNNING";
	case DOCA_CLOCK_NIC_REAL_TIME:
		return "DOCA_CLOCK_NIC_REAL_TIME";
	case DOCA_CLOCK_NIC_DPA_TIMER:
		return "DOCA_CLOCK_NIC_DPA_TIMER";
	case DOCA_CLOCK_HOST_COUNTER_CYCLES:
		return "DOCA_CLOCK_HOST_COUNTER_CYCLES";
	case DOCA_CLOCK_HOST_REAL_TIME:
		return "DOCA_CLOCK_HOST_REAL_TIME";
	case DOCA_CLOCK_HOST_MONOTONIC:
		return "DOCA_CLOCK_HOST_MONOTONIC";
	case DOCA_CLOCK_HOST_MONOTONIC_RAW:
		return "DOCA_CLOCK_HOST_MONOTONIC_RAW";
	default:
		return "CLOCK_UNKNOWN";
	}
}

/*
 * Read a single clock timestamp and log result
 *
 * @clock [in]: doca_clock context
 * @clock_type [in]: Lib representation of clock to be read
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_single_timestamp(const struct doca_clock *clock, uint64_t clock_type)
{
	union doca_clock_timespec_t clock_ts = {};
	doca_error_t result;

	result = doca_clock_get_timestamp(clock, clock_type, &clock_ts);
	if (result != DOCA_SUCCESS)
		return result;

	if (clock_type == DOCA_CLOCK_HOST_COUNTER_CYCLES)
		DOCA_LOG_INFO("Timestamp - %s: %lu cycles", get_clock_name(clock_type), clock_ts.counter);
	else
		DOCA_LOG_INFO("Timestamp - %s: %lu.%lu seconds",
			      get_clock_name(clock_type),
			      clock_ts.ts.tv_sec,
			      clock_ts.ts.tv_nsec);

	return result;
}

/*
 * Get cross-timesetmp of 2 clocks and log result
 *
 * @clock [in]: doca_clock context
 * @prim_clock [in]: Primary clock to use for cross-timestamp
 * @clock_type [in]: NIC clock to use for cross-timestamp
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_cross_timestamp(const struct doca_clock *clock, uint64_t prim_clock, uint64_t nic_clock)
{
	union doca_clock_timespec_t prim_ts = {}, nic_ts = {};
	uint64_t accuracy = 0;
	doca_error_t result;

	result = doca_clock_get_crosstimestamp(clock, prim_clock, nic_clock, &prim_ts, &nic_ts, &accuracy);
	if (result != DOCA_SUCCESS)
		return result;

	if (prim_clock == DOCA_CLOCK_HOST_COUNTER_CYCLES)
		DOCA_LOG_INFO("Cross-timestamp - %s: %lu cycles, %s: %lu.%lu seconds, accuracy: %lu cycles",
			      get_clock_name(prim_clock),
			      prim_ts.counter,
			      get_clock_name(nic_clock),
			      nic_ts.ts.tv_sec,
			      nic_ts.ts.tv_nsec,
			      accuracy);
	else
		DOCA_LOG_INFO("Cross-timestamp - %s: %lu.%lu seconds, %s: %lu.%lu seconds, accuracy: %lu nanoseconds",
			      get_clock_name(prim_clock),
			      prim_ts.ts.tv_sec,
			      prim_ts.ts.tv_nsec,
			      get_clock_name(nic_clock),
			      nic_ts.ts.tv_sec,
			      nic_ts.ts.tv_nsec,
			      accuracy);

	return result;
}

/**
 * Run the clock cross-timestamp sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t run_clock_cross_timestamp_sample(const struct clock_cross_timestamp_sample_cfg *cfg)
{
	struct doca_clock *clock;
	struct doca_dev *dev;
	doca_error_t result, tmp_result;

	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device. error=%s", doca_error_get_name(result));
		return result;
	}

	if (cfg->primary_clock == DOCA_CLOCK_NIC_FREE_RUNNING || cfg->nic_clock == DOCA_CLOCK_NIC_FREE_RUNNING) {
		result = doca_clock_cap_nic_free_running_is_supported(doca_dev_as_devinfo(dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("NIC free running clock not supported. Error=%s", doca_error_get_name(result));
			goto close_dev;
		}
	}

	if (cfg->primary_clock == DOCA_CLOCK_NIC_REAL_TIME || cfg->nic_clock == DOCA_CLOCK_NIC_REAL_TIME) {
		result = doca_clock_cap_nic_real_time_is_supported(doca_dev_as_devinfo(dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("NIC real time clock not supported. Error=%s", doca_error_get_name(result));
			goto close_dev;
		}
	}

	if (cfg->primary_clock == DOCA_CLOCK_NIC_DPA_TIMER || cfg->nic_clock == DOCA_CLOCK_NIC_DPA_TIMER) {
		result = doca_clock_cap_nic_dpa_timer_is_supported(doca_dev_as_devinfo(dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("NIC DPA timer clock not supported. Error=%s", doca_error_get_name(result));
			goto close_dev;
		}
	}

	result = doca_clock_create(dev, &clock);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca clock. Error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	/* If (second) nic_clock was not input, get the timestamp of primary clock only */
	if (cfg->nic_clock == 0) {
		result = get_single_timestamp(clock, cfg->primary_clock);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get timestamp. Error=%s", doca_error_get_name(result));
			goto destroy_clock;
		}
	} else {
		result = get_cross_timestamp(clock, cfg->primary_clock, cfg->nic_clock);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get cross-timestamp. Error=%s", doca_error_get_name(result));
			goto destroy_clock;
		}
	}

destroy_clock:
	tmp_result = doca_clock_destroy(clock);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy doca clock. Error=%s", doca_error_get_name(tmp_result));
close_dev:
	(void)doca_dev_close(dev);

	return result;
}
