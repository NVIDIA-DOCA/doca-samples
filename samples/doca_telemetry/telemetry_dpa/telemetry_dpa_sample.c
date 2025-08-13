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

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_telemetry_dpa.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "telemetry_dpa_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_DPA::SAMPLE);

struct telemetry_dpa_sample_objects {
	struct doca_telemetry_dpa *telemetry_dpa_obj;	     /* doca telemetry dpa perf object*/
	struct doca_dev *dev;				     /* Doca device*/
	doca_telemetry_dpa_process_info_t *dpa_process_list; /* Structure that represent the DPA process running */
	doca_telemetry_dpa_thread_info_t *dpa_thread_list;   /* Structure that represent the DPA threads running */
	doca_telemetry_dpa_cumul_info_t *cumul_info_list;   /* Structure that represent the DPA cumul counter results */
	doca_telemetry_dpa_event_sample_t *event_info_list; /* Structure that represent the DPA perf event results */
	uint32_t process_id;				    /**< specific process id */
	uint32_t thread_id;				    /**< specific threads id */
	uint32_t dpa_process_num;			    /**< specific threads id */
	uint32_t dpa_timer_freq;			    /**< specific threads id */
	uint32_t dpa_threads_num;			    /**< specific threads id */
	uint32_t cumul_samples_num;			    /**< specific threads id */
	uint32_t perf_event_samples_num;		    /**< specific threads id */
};

/*
 * Return perf event type description string
 *
 * @event_sample_type [in]: Event type to match
 * @return: String that describes the perf event type
 */
const char *telemetry_dpa_event_sample_type_get_str(enum doca_telemetry_dpa_event_sample_type event_sample_type)
{
	switch (event_sample_type) {
	case DOCA_TELEMETRY_DPA_EVENT_SAMPLE_TYPE_EMPTY_SAMPLE:
		return "Event sample type empty sample";
	case DOCA_TELEMETRY_DPA_EVENT_SAMPLE_TYPE_SCHEDULE_IN:
		return "Event sample type schedule in";
	case DOCA_TELEMETRY_DPA_EVENT_SAMPLE_TYPE_SCHEDULE_OUT:
		return "Event sample type schedule out";
	case DOCA_TELEMETRY_DPA_EVENT_SAMPLE_TYPE_BUFFER_FULL:
		return "Event sample type buffer full";
	default:
		return "Unrecognized event sample type";
	}
}

/*
 * Print process list
 *
 * Print the contents of the extracted process list.
 *
 * @dpa_process_num [in]: Number of DPA process list to print
 * @dpa_process_list [in]: Extracted list to print
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_dpa_print_process_list(uint32_t dpa_process_num,
						     doca_telemetry_dpa_process_info_t *dpa_process_list)
{
	if (dpa_process_list == NULL) {
		DOCA_LOG_ERR("Failed to print processes list: parameter dpa_process_list=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (dpa_process_num == 0) {
		DOCA_LOG_ERR("Failed to print processes list: no processes found in context");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Number of processes extracted: %u", dpa_process_num);
	DOCA_LOG_INFO("Extracted process list:");
	for (uint32_t idx = 0; idx < dpa_process_num; idx++) {
		DOCA_LOG_INFO("  dpa_process_id=%u", dpa_process_list[idx].dpa_process_id);
		DOCA_LOG_INFO("  num_of_threads=%u", dpa_process_list[idx].num_of_threads);
		DOCA_LOG_INFO("  process_name=%s", dpa_process_list[idx].process_name);
	}

	return DOCA_SUCCESS;
}

/*
 * Print thread list
 *
 * Print the contents of the extracted thread list.
 *
 * @dpa_threads_num [in]: Number of DPA threads list to print
 * @dpa_process_list [in]: Extracted list to print
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_dpa_print_thread_list(uint32_t dpa_threads_num,
						    doca_telemetry_dpa_thread_info_t *dpa_thread_list)
{
	if (dpa_thread_list == NULL) {
		DOCA_LOG_ERR("Failed to print thread list: parameter dpa_thread_list=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (dpa_threads_num == 0) {
		DOCA_LOG_ERR("Failed to print thread list: no threads found in context");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Number of threads extracted: %u", dpa_threads_num);
	DOCA_LOG_INFO("Extracted thread list:");
	for (uint32_t idx = 0; idx < dpa_threads_num; idx++) {
		DOCA_LOG_INFO("  dpa_process_id=%u", dpa_thread_list[idx].dpa_process_id);
		DOCA_LOG_INFO("  dpa_thread_id=%u", dpa_thread_list[idx].dpa_thread_id);
		DOCA_LOG_INFO("  thread_name=%s", dpa_thread_list[idx].thread_name);
	}

	return DOCA_SUCCESS;
}

/*
 * Print cumul list
 *
 * Print the contents of the extracted cumul list.
 *
 * @cumul_samples_num [in]: Number of cumul samples to print
 * @cumul_info_list [in]: Extracted list to print
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_dpa_print_cumul_list(uint32_t cumul_samples_num,
						   doca_telemetry_dpa_cumul_info_t *cumul_info_list)
{
	if (cumul_info_list == NULL) {
		DOCA_LOG_ERR("Failed to print cumul list: parameter cumul_info_list=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cumul_samples_num == 0) {
		DOCA_LOG_ERR("Failed to print cumul list: no samples found in context");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Number of cumul samples_num: %u", cumul_samples_num);
	DOCA_LOG_INFO("Extracted cumul list:");
	for (uint32_t idx = 0; idx < cumul_samples_num; idx++) {
		if (cumul_info_list[idx].instructions == 0)
			continue;
		DOCA_LOG_INFO("  dpa_process_id=%u", cumul_info_list[idx].dpa_process_id);
		DOCA_LOG_INFO("  dpa_thread_id=%u", cumul_info_list[idx].dpa_thread_id);
		DOCA_LOG_INFO("  time=%lu", cumul_info_list[idx].time);
		DOCA_LOG_INFO("  cycles=%lu", cumul_info_list[idx].cycles);
		DOCA_LOG_INFO("  instructions=%lu", cumul_info_list[idx].instructions);
		DOCA_LOG_INFO("  num_executions=%lu", cumul_info_list[idx].num_executions);
	}

	return DOCA_SUCCESS;
}

/*
 * Print perf event list
 *
 * Print the contents of the extracted perf event list.
 *
 * @perf_event_samples_num [in]: Number of perf event samples to print
 * @event_info_list [in]: Extracted list to print
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_dpa_print_perf_event_list(uint32_t perf_event_samples_num,
							doca_telemetry_dpa_event_sample_t *event_info_list)
{
	if (event_info_list == NULL) {
		DOCA_LOG_ERR("Failed to print perf event list: parameter event_info_list=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (perf_event_samples_num == 0) {
		DOCA_LOG_ERR("Failed to print perf event list: no samples found in context");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_DBG("Number of perf event samples_num: %u", perf_event_samples_num);
	DOCA_LOG_DBG("Extracted perf event counter list:");
	for (uint32_t idx = 0; idx < perf_event_samples_num; idx++) {
		if (event_info_list[idx].instructions == 0)
			continue;
		DOCA_LOG_INFO("  dpa_thread_id=%u", event_info_list[idx].dpa_thread_id);
		DOCA_LOG_INFO("  timestamp=%lu", event_info_list[idx].timestamp);
		DOCA_LOG_INFO("  cycles=%lu", event_info_list[idx].cycles);
		DOCA_LOG_INFO("  instructions=%u", event_info_list[idx].instructions);
		DOCA_LOG_INFO("  sample id per execution unit=%u", event_info_list[idx].sample_id_in_eu);
		DOCA_LOG_INFO("  execution unit id=%u", event_info_list[idx].eu_id);
		DOCA_LOG_INFO("  type=%s", telemetry_dpa_event_sample_type_get_str(event_info_list[idx].type));
	}

	return DOCA_SUCCESS;
}

/*
 * Clean sample objects
 *
 * Closes and frees sample resources.
 *
 * @sample_objects [in]: sample objects to clean
 * @sample_objects [in]: type of counter to be reset
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_dpa_sample_cleanup(struct telemetry_dpa_sample_objects *sample_objects,
						 enum doca_telemetry_dpa_counter_type counter_type)
{
	doca_error_t result;

	if (sample_objects->dpa_process_list) {
		DOCA_LOG_INFO("process_list %p: process list was destroyed", sample_objects->dpa_process_list);
		free(sample_objects->dpa_process_list);
		sample_objects->dpa_process_list = NULL;
	}

	if (sample_objects->dpa_thread_list) {
		DOCA_LOG_INFO("process_list %p: thread list was destroyed", sample_objects->dpa_thread_list);
		free(sample_objects->dpa_thread_list);
		sample_objects->dpa_thread_list = NULL;
	}

	if (sample_objects->cumul_info_list) {
		DOCA_LOG_INFO("cumul_info_list %p: list of cumulative info counter was destroyed",
			      sample_objects->cumul_info_list);
		free(sample_objects->cumul_info_list);
		sample_objects->cumul_info_list = NULL;
	}

	if (sample_objects->event_info_list) {
		DOCA_LOG_INFO("perf_event_list %p: event counters list was destroyed", sample_objects->event_info_list);
		free(sample_objects->event_info_list);
		sample_objects->event_info_list = NULL;
	}

	if (sample_objects->telemetry_dpa_obj != NULL) {
		result = doca_telemetry_dpa_counter_restart(sample_objects->telemetry_dpa_obj,
							    sample_objects->process_id,
							    counter_type);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to restart telemetry_dpa counter counter with error=%s",
				      doca_error_get_name(result));
			return result;
		}

		result = doca_telemetry_dpa_stop(sample_objects->telemetry_dpa_obj);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop telemetry_dpa with error=%s", doca_error_get_name(result));
			return result;
		}

		result = doca_telemetry_dpa_destroy(sample_objects->telemetry_dpa_obj);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy telemetry_dpa with error=%s", doca_error_get_name(result));
			return result;
		}
		sample_objects->telemetry_dpa_obj = NULL;
	}

	if (sample_objects->dev != NULL) {
		result = doca_dev_close(sample_objects->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to close device with error=%s", doca_error_get_name(result));
			return result;
		}
		sample_objects->dev = NULL;
	}

	return DOCA_SUCCESS;
}

/*
 * Allocate telemetry dpa output object
 *
 * @sample_objects [in]: sample objects struct for the sample
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_dpa_sample_allocate_output_objects(enum doca_telemetry_dpa_counter_type counter_type,
								 uint32_t max_event_sample,
								 struct telemetry_dpa_sample_objects *sample_objects)
{
	doca_error_t result;
	uint32_t dpa_process_list_size = 0;
	uint32_t dpa_thread_list_size = 0;
	uint32_t dpa_cumul_samples_size = 0;
	uint32_t dpa_max_perf_event_samples = 0;

	result = doca_telemetry_dpa_get_process_list_size(sample_objects->telemetry_dpa_obj,
							  sample_objects->process_id,
							  &dpa_process_list_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create process list: failed to retrieve the process list size");
		return DOCA_ERROR_NO_MEMORY;
	}
	sample_objects->dpa_process_list = (doca_telemetry_dpa_process_info_t *)malloc(dpa_process_list_size);
	if (sample_objects->dpa_process_list == NULL) {
		DOCA_LOG_ERR("Failed to create process list: failed to allocate memory for processes info");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_telemetry_dpa_get_thread_list_size(sample_objects->telemetry_dpa_obj,
							 sample_objects->process_id,
							 sample_objects->thread_id,
							 &dpa_thread_list_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create thread list: failed to retrieve the thread list size");
		return DOCA_ERROR_NO_MEMORY;
	}
	sample_objects->dpa_thread_list = (doca_telemetry_dpa_thread_info_t *)malloc(dpa_thread_list_size);
	if (sample_objects->dpa_thread_list == NULL) {
		DOCA_LOG_ERR("Failed to create thread list: failed to allocate memory for threads info");
		return DOCA_ERROR_NO_MEMORY;
	}

	if (counter_type == DOCA_TELEMETRY_DPA_COUNTER_TYPE_CUMULATIVE_EVENT) {
		result = doca_telemetry_dpa_get_cumul_samples_size(sample_objects->telemetry_dpa_obj,
								   sample_objects->process_id,
								   sample_objects->thread_id,
								   &dpa_cumul_samples_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create cumul info list: failed to retrieve cumul samples list size");
			return DOCA_ERROR_NO_MEMORY;
		}
		sample_objects->cumul_info_list = (doca_telemetry_dpa_cumul_info_t *)malloc(dpa_cumul_samples_size);
		if (sample_objects->cumul_info_list == NULL) {
			DOCA_LOG_ERR(
				"Failed to create cumul info list: failed to allocate memory for cumul samples info");
			return DOCA_ERROR_NO_MEMORY;
		}
	} else {
		if (max_event_sample != 0) {
			result = doca_telemetry_dpa_set_max_perf_event_samples(sample_objects->telemetry_dpa_obj,
									       max_event_sample);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to create event info list: failed to set maximum number of perf event samples");
				return DOCA_ERROR_NO_MEMORY;
			}
		}

		result = doca_telemetry_dpa_get_max_perf_event_samples(sample_objects->telemetry_dpa_obj,
								       &dpa_max_perf_event_samples);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create event info list: failed to retrieve maximum number of perf event samples");
			return DOCA_ERROR_NO_MEMORY;
		}

		if (dpa_max_perf_event_samples != 0) {
			sample_objects->event_info_list = (doca_telemetry_dpa_event_sample_t *)malloc(
				sizeof(doca_telemetry_dpa_event_sample_t) * dpa_max_perf_event_samples);
			if (sample_objects->event_info_list == NULL) {
				DOCA_LOG_ERR(
					"Failed to create event info list: failed to allocate memory for perf event samples info");
				return DOCA_ERROR_NO_MEMORY;
			}
		} else {
			DOCA_LOG_ERR(
				"Failed to create event info list: maximum number of perf event samples is set to 0");
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize telemetry dpa context object
 *
 * @sample_objects [in]: sample objects struct for the sample
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_dpa_sample_context_init(uint8_t process_id_set,
						      uint8_t thread_id_set,
						      uint32_t process_id,
						      uint32_t thread_id,
						      enum doca_telemetry_dpa_counter_type counter_type,
						      uint32_t max_event_sample,
						      struct telemetry_dpa_sample_objects *sample_objects)
{
	doca_error_t result, teardown_result;

	struct doca_devinfo *devinfo = doca_dev_as_devinfo(sample_objects->dev);

	result = doca_telemetry_dpa_cap_is_supported(devinfo);
	if (result == DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa: device does not support doca_telemetry_dpa");
		return DOCA_ERROR_NOT_SUPPORTED;
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa: failed to query capability");
		return result;
	}

	/* Create context and set properties */
	result = doca_telemetry_dpa_create(sample_objects->dev, &sample_objects->telemetry_dpa_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa: failed to create telemetry dpa object with error=%s",
			     doca_error_get_name(result));
		goto teardown_init;
	}

	if (!process_id_set) {
		result = doca_telemetry_dpa_get_all_process_id(devinfo, &sample_objects->process_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start telemetry_dpa: failed to retrieve all process id with error=%s",
				     doca_error_get_name(result));
			goto teardown_init;
		}
	} else {
		sample_objects->process_id = process_id;
	}
	if (!thread_id_set) {
		result = doca_telemetry_dpa_get_all_thread_id(devinfo, &sample_objects->thread_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start telemetry_dpa: failed to retrieve all threads id with error=%s",
				     doca_error_get_name(result));
			goto teardown_init;
		}
	} else {
		sample_objects->thread_id = thread_id;
	}

	result = telemetry_dpa_sample_allocate_output_objects(counter_type, max_event_sample, sample_objects);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa: failed to init sample objects with error=%s",
			     doca_error_get_name(result));
		goto teardown_init;
	}

	result = doca_telemetry_dpa_start(sample_objects->telemetry_dpa_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry dpa object with error=%s", doca_error_get_name(result));
		goto teardown_init;
	}

	return DOCA_SUCCESS;

teardown_init:
	teardown_result = telemetry_dpa_sample_cleanup(sample_objects, counter_type);
	if (teardown_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa: Teardown failed with error=%s",
			     doca_error_get_name(teardown_result));
	}

	return result;
}

doca_error_t telemetry_dpa_sample_run(const struct telemetry_dpa_sample_cfg *cfg)
{
	doca_error_t result = DOCA_SUCCESS, teardown_result = DOCA_SUCCESS;
	struct telemetry_dpa_sample_objects sample_objects = {0};

	DOCA_LOG_INFO("Started doca_telemetry_dpa sample with the following parameters: ");
	DOCA_LOG_INFO("	pci_addr='%s'", cfg->pci_addr);
	DOCA_LOG_INFO("	sample_run_time=%u", cfg->run_time);
	if (cfg->process_id_set) {
		DOCA_LOG_INFO("	process_id=%u", cfg->process_id);
	}
	if (cfg->thread_id_set) {
		DOCA_LOG_INFO("	thread_id=%u", cfg->thread_id);
	}
	if (cfg->counter_type) {
		DOCA_LOG_INFO("	counter_type=%u", cfg->counter_type);
	}

	/* Open DOCA device based on the given PCI address */
	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &sample_objects.dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device with error=%s", doca_error_get_name(result));
		return result;
	}

	result = telemetry_dpa_sample_context_init(cfg->process_id_set,
						   cfg->thread_id_set,
						   cfg->process_id,
						   cfg->thread_id,
						   cfg->counter_type,
						   cfg->max_event_sample,
						   &sample_objects);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init sample objects with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	result = doca_telemetry_dpa_read_processes_list(sample_objects.telemetry_dpa_obj,
							sample_objects.process_id,
							&sample_objects.dpa_process_num,
							sample_objects.dpa_process_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read the process list with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	result = telemetry_dpa_print_process_list(sample_objects.dpa_process_num, sample_objects.dpa_process_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to print the process list with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	result =
		doca_telemetry_dpa_get_dpa_timer_freq(sample_objects.telemetry_dpa_obj, &sample_objects.dpa_timer_freq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to retrieve telemetry_dpa dpa timer freq with error=%s",
			     doca_error_get_name(result));
		goto teardown;
	}
	DOCA_LOG_INFO("DPA timer ticks frequency (in kHZ): %u", sample_objects.dpa_timer_freq);

	result = doca_telemetry_dpa_read_thread_list(sample_objects.telemetry_dpa_obj,
						     sample_objects.process_id,
						     sample_objects.thread_id,
						     &sample_objects.dpa_threads_num,
						     sample_objects.dpa_thread_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read the thread list with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	result = telemetry_dpa_print_thread_list(sample_objects.dpa_threads_num, sample_objects.dpa_thread_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to print the thread list with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	result = doca_telemetry_dpa_counter_restart(sample_objects.telemetry_dpa_obj,
						    sample_objects.process_id,
						    cfg->counter_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to restart telemetry_dpa counter counter with error=%s",
			      doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_dpa_counter_start(sample_objects.telemetry_dpa_obj,
						  sample_objects.process_id,
						  cfg->counter_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa counter with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	usleep(cfg->run_time * 1000);

	result = doca_telemetry_dpa_counter_stop(sample_objects.telemetry_dpa_obj,
						 sample_objects.process_id,
						 cfg->counter_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop telemetry_dpa counter with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	if (cfg->counter_type == DOCA_TELEMETRY_DPA_COUNTER_TYPE_CUMULATIVE_EVENT) {
		result = doca_telemetry_dpa_read_cumul_info_list(sample_objects.telemetry_dpa_obj,
								 sample_objects.process_id,
								 sample_objects.thread_id,
								 &sample_objects.cumul_samples_num,
								 sample_objects.cumul_info_list);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read the cumul counter list with error=%s",
				     doca_error_get_name(result));
			goto teardown;
		}

		result = telemetry_dpa_print_cumul_list(sample_objects.cumul_samples_num,
							sample_objects.cumul_info_list);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to print the cumul list with error=%s", doca_error_get_name(result));
			goto teardown;
		}
	} else {
		result = doca_telemetry_dpa_read_perf_event_list(sample_objects.telemetry_dpa_obj,
								 sample_objects.process_id,
								 sample_objects.thread_id,
								 &sample_objects.perf_event_samples_num,
								 sample_objects.event_info_list);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read the perf event list with error=%s", doca_error_get_name(result));
			goto teardown;
		}
		result = telemetry_dpa_print_perf_event_list(sample_objects.perf_event_samples_num,
							     sample_objects.event_info_list);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to print the perf event list with error=%s", doca_error_get_name(result));
			goto teardown;
		}
	}

teardown:
	teardown_result = telemetry_dpa_sample_cleanup(&sample_objects, cfg->counter_type);
	if (teardown_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Teardown failed with error=%s", doca_error_get_name(teardown_result));
		DOCA_ERROR_PROPAGATE(result, teardown_result);
	}

	return result;
}
