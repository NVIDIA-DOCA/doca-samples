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
#include <doca_telemetry_diag.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#include "common.h"
#include "telemetry_diag_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_DIAG::SAMPLE);

#define SECS_TO_NSECS_CONVERSION pow(10, 9)

struct telemetry_diag_sample_objects {
	struct doca_telemetry_diag *telemetry_diag_obj; /* Telemetry diag object*/
	struct doca_dev *dev;				/* Doca device*/
	uint32_t num_data_ids;				/**< the number of entries in the data_ids_struct */
	uint8_t started;				/* True if telemetry_diag struct was started*/
	void *buf;					/* Buf for the sampling output*/
	FILE *output_file;				/* Output file*/
};

/*
 * Verify params
 *
 * make sure all input params are valid and supported.
 *
 * @dev [in]: doca device
 * @log_max_num_samples [in]: log max num of samples to be used
 * @max_num_data_ids [in]: max num data IDs to be used
 * @sync_mode [in]: sync mode to use
 * @sample_mode [in]: sample mode to use
 * @data_clear [in]: whether to use data_clear mode
 * @data_timestamp_source [in]: data timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_NOT_SUPPORTED if any of the params is not supported.
 */
static doca_error_t telemetry_diag_sample_check_capabilities(
	struct doca_dev *dev,
	uint8_t log_max_num_samples,
	uint32_t max_num_data_ids,
	enum doca_telemetry_diag_sync_mode sync_mode,
	enum doca_telemetry_diag_sample_mode sample_mode,
	uint8_t data_clear,
	enum doca_telemetry_diag_timestamp_source data_timestamp_source)
{
	uint32_t cap_32_bit;
	uint8_t cap_8_bit;
	doca_error_t result;

	struct doca_devinfo *devinfo = doca_dev_as_devinfo(dev);

	result = doca_telemetry_diag_cap_is_supported(devinfo);
	if (result == DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Device does not support doca_telemetry_diag");
		return DOCA_ERROR_NOT_SUPPORTED;
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query capability");
		return result;
	}

	result = doca_telemetry_diag_cap_get_log_max_num_samples(devinfo, &cap_8_bit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query capability");
		return result;
	} else if (log_max_num_samples > cap_8_bit) {
		DOCA_LOG_ERR(
			"Parameter log_max_num_samples is larger than supported cap: log_max_num_samples=%d, cap=%d",
			log_max_num_samples,
			cap_8_bit);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	result = doca_telemetry_diag_cap_get_max_num_data_ids(devinfo, &cap_32_bit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query capability");
		return result;
	} else if (max_num_data_ids > cap_32_bit) {
		DOCA_LOG_ERR("Parameter max_num_data_ids is larger than supported cap: max_num_data_ids=%d, cap=%d",
			     max_num_data_ids,
			     cap_32_bit);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (sync_mode == DOCA_TELEMETRY_DIAG_SYNC_MODE_SYNC_START) {
		result = doca_telemetry_diag_cap_is_sync_start_supported(devinfo, &cap_8_bit);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query capability");
			return result;
		} else if (!cap_8_bit) {
			DOCA_LOG_ERR("Sync mode not supported");
			return DOCA_ERROR_NOT_SUPPORTED;
		}
	}

	result = doca_telemetry_diag_cap_is_sample_mode_supported(devinfo, sample_mode, &cap_8_bit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query capability");
		return result;
	} else if (!cap_8_bit) {
		DOCA_LOG_ERR("Sample mode not supported");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (!!data_clear) {
		result = doca_telemetry_diag_cap_is_data_clear_supported(devinfo, &cap_8_bit);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query capability");
			return result;
		} else if (!cap_8_bit) {
			DOCA_LOG_ERR("Data clear mode not supported");
			return DOCA_ERROR_NOT_SUPPORTED;
		}
	}

	result = doca_telemetry_diag_cap_is_data_timestamp_source_supported(devinfo, data_timestamp_source, &cap_8_bit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query capability");
		return result;
	} else if (!cap_8_bit) {
		DOCA_LOG_ERR("Timestamp source not supported");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Clean sample objects
 *
 * Closes and frees sample resources.
 *
 * @sample_objects [in]: sample objects to clean
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_diag_sample_cleanup(struct telemetry_diag_sample_objects *sample_objects)
{
	doca_error_t result;
	int file_result = 0;

	if (sample_objects->buf != NULL) {
		free(sample_objects->buf);
		sample_objects->buf = NULL;
	}

	if (sample_objects->telemetry_diag_obj != NULL) {
		if (sample_objects->started) {
			result = doca_telemetry_diag_stop(sample_objects->telemetry_diag_obj);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_WARN("Failed to stop telemetry_diag with error=%s",
					      doca_error_get_name(result));
				return result;
			}
		}
		result = doca_telemetry_diag_destroy(sample_objects->telemetry_diag_obj);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy telemetry_diag with error=%s", doca_error_get_name(result));
			return result;
		}
		sample_objects->telemetry_diag_obj = NULL;
	}

	if (sample_objects->dev != NULL) {
		result = doca_dev_close(sample_objects->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to close device with error=%s", doca_error_get_name(result));
			return result;
		}
		sample_objects->dev = NULL;
	}

	if (sample_objects->output_file != NULL) {
		file_result = fclose(sample_objects->output_file);
		if (file_result != 0) {
			DOCA_LOG_WARN("Failed to close output file with errno=%s (%d)", strerror(errno), errno);
			return DOCA_ERROR_UNEXPECTED;
		}
		sample_objects->output_file = NULL;
	}
	return DOCA_SUCCESS;
}

/*
 * set telemetry_diag properties
 *
 * @diag [in]: telemetry_diag context to set properties to
 * @output_format [in]: output format to be used
 * @sample_period [in]: sample period to be used
 * @log_max_num_samples [in]: log max num of samples to be used
 * @max_num_data_ids [in]: max num data IDs to be used
 * @sync_mode [in]: sync mode to use
 * @sample_mode [in]: sample mode to use
 * @data_clear [in]: whether to use data_clear mode
 * @data_timestamp_source [in]: data timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_diag_sample_set_properties(struct doca_telemetry_diag *diag,
							 enum doca_telemetry_diag_output_format output_format,
							 uint64_t sample_period,
							 uint8_t log_max_num_samples,
							 uint32_t max_num_data_ids,
							 enum doca_telemetry_diag_sync_mode sync_mode,
							 enum doca_telemetry_diag_sample_mode sample_mode,
							 uint8_t data_clear,
							 enum doca_telemetry_diag_timestamp_source data_timestamp_source)
{
	doca_error_t result;

	result = doca_telemetry_diag_set_output_format(diag, output_format);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set output_format with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_sample_period(diag, sample_period);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sample_period with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_log_max_num_samples(diag, log_max_num_samples);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set log_max_num_samples with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_max_num_data_ids(diag, max_num_data_ids);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max_num_data_ids with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_sync_mode(diag, sync_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sync_mode with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_sample_mode(diag, sample_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sample_mode with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_data_clear(diag, data_clear);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sample_mode with error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_diag_set_data_timestamp_source(diag, data_timestamp_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set data_timestamp_source with error=%s", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Calculate difference between two timespecs, in nanoseconds
 *
 *
 * @start [in]: start of time period
 * @end [in]: end of time period
 * @return: difference between times, in nanoseconds
 */
static inline uint64_t telemetry_diag_sample_time_diff_nsec(struct timespec start, struct timespec end)
{
	return ((end.tv_nsec + (end.tv_sec - start.tv_sec) * SECS_TO_NSECS_CONVERSION) - start.tv_nsec);
}

/*
 * Initialize telemetry diag context object
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @data_ids_struct [in]: array of data_id_entry structures
 * @num_data_ids [in]: the number of entries in the data_ids_struct
 * @sample_period_ns [in]: sample period to be used
 * @log_max_num_samples [in]: log max num of samples to be used
 * @sample_mode [in]: sample mode to use
 * @output_format [in]: output format to use
 * @sync_mode [in]: sync mode to use
 * @timestamp_source [in]: timestamp source to use
 * @force_ownership [in]: force ownership when creating diag context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_diag_sample_context_init(struct telemetry_diag_sample_objects *sample_objects,
						       struct data_id_entry *data_ids_struct,
						       uint32_t num_data_ids,
						       uint64_t sample_period_ns,
						       uint8_t log_max_num_samples,
						       enum doca_telemetry_diag_sample_mode sample_mode,
						       enum doca_telemetry_diag_output_format output_format,
						       enum doca_telemetry_diag_sync_mode sync_mode,
						       enum doca_telemetry_diag_timestamp_source timestamp_source,
						       uint8_t force_ownership)
{
	uint8_t data_clear = 0;
	uint32_t max_num_data_ids = num_data_ids;

	doca_error_t result, teardown_result;
	uint64_t counter_id_failure = 0;

	uint64_t data_ids[(size_t)num_data_ids];

	/* Check support for input arguments */
	result = telemetry_diag_sample_check_capabilities(sample_objects->dev,
							  log_max_num_samples,
							  max_num_data_ids,
							  sync_mode,
							  sample_mode,
							  data_clear,
							  timestamp_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed capability checks with error=%s", doca_error_get_name(result));
		return result;
	}

	/* Create context and set properties */
	result = doca_telemetry_diag_create(sample_objects->dev, force_ownership, &sample_objects->telemetry_diag_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create telemetry diag object with error=%s", doca_error_get_name(result));
		goto teardown_init;
	}

	result = telemetry_diag_sample_set_properties(sample_objects->telemetry_diag_obj,
						      output_format,
						      sample_period_ns,
						      log_max_num_samples,
						      max_num_data_ids,
						      sync_mode,
						      sample_mode,
						      data_clear,
						      timestamp_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set properties with error=%s", doca_error_get_name(result));
		goto teardown_init;
	}

	sample_objects->num_data_ids = num_data_ids;

	result = doca_telemetry_diag_apply_config(sample_objects->telemetry_diag_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply config with error=%s", doca_error_get_name(result));
		goto teardown_init;
	}

	for (uint32_t i = 0; i < num_data_ids; i++) {
		data_ids[i] = data_ids_struct[i].data_id;
	}

	result = doca_telemetry_diag_apply_counters_list_by_id(sample_objects->telemetry_diag_obj,
							       data_ids,
							       num_data_ids,
							       &counter_id_failure);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply counters with error=%s, counter_id_failure=%ld",
			     doca_error_get_name(result),
			     counter_id_failure);
		goto teardown_init;
	}

	return DOCA_SUCCESS;

teardown_init:
	teardown_result = telemetry_diag_sample_cleanup(sample_objects);
	if (teardown_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Teardown failed with error=%s", doca_error_get_name(teardown_result));
	}
	return result;
}

static inline int write_timestamp_to_file(FILE *file,
					  uint32_t timestamp_h,
					  uint32_t timestamp_l,
					  enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	if (timestamp_source == DOCA_TELEMETRY_DIAG_TIMESTAMP_SOURCE_RTC) {
		/* Write rtc timestamp to csv file. The below timestamp print is only valid for RTC timestamp format
		 * The timestamp_h is seconds, the timestamp_l is sub-second time, given in nanoseconds, thus,
		 * (timestamp_h * NSEC_IN_SEC + timestamp_l) is the timestamp in nanseconds.
		 */
		return fprintf(file, ", %u.%09u", timestamp_h, timestamp_l);
	} else {
		/* Write frc timestamp to csv file. */
		return fprintf(file, ", %lu", ((uint64_t)timestamp_h << 32) | timestamp_l);
	}
}

/*
 * Process and write the results of a single query to the output file when using output format 0
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @num_actual_samples [in]: the number of samples that were returned from the query
 * @size_of_sample [in]: size of a single sample in the result buf
 * @timestamp_source [in]: the timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static inline doca_error_t telemetry_diag_sample_write_sample_format_0(
	struct telemetry_diag_sample_objects *sample_objects,
	uint32_t num_actual_samples,
	uint32_t size_of_sample,
	enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	int write_result = 0;

	struct doca_telemetry_diag_data_sample_format_0 *current_sample =
		(struct doca_telemetry_diag_data_sample_format_0 *)sample_objects->buf;
	for (uint32_t sample_index = 0; sample_index < num_actual_samples; sample_index++) {
		write_result = fprintf(sample_objects->output_file, "\n%u", current_sample->sample_id);
		if (write_result < 0) {
			DOCA_LOG_ERR("Failed to write to output file with errno=%s (%d)", strerror(errno), errno);
			return DOCA_ERROR_IO_FAILED;
		}

		for (uint32_t j = 0; j < sample_objects->num_data_ids; j++) {
			write_result =
				fprintf(sample_objects->output_file, ", 0x%016lx", current_sample->value[j].data_id);
			if (write_result < 0) {
				DOCA_LOG_ERR("Failed to write data_id to output file with errno=%s (%d)",
					     strerror(errno),
					     errno);
				return DOCA_ERROR_IO_FAILED;
			}
			write_result = write_timestamp_to_file(sample_objects->output_file,
							       current_sample->value[j].timestamp_h,
							       current_sample->value[j].timestamp_l,
							       timestamp_source);
			if (write_result < 0) {
				DOCA_LOG_ERR("Failed to write timestamp to output file with errno=%s (%d)",
					     strerror(errno),
					     errno);
				return DOCA_ERROR_IO_FAILED;
			}
			write_result =
				fprintf(sample_objects->output_file, ", %lu", current_sample->value[j].data_value);
			if (write_result < 0) {
				DOCA_LOG_ERR("Failed to write counter value to output file with errno=%s (%d)",
					     strerror(errno),
					     errno);
				return DOCA_ERROR_IO_FAILED;
			}
		}

		/* Update the pointer to the next sample location */
		current_sample =
			(doca_telemetry_diag_data_sample_format_0 *)((uint8_t *)current_sample + size_of_sample);
	}
	return DOCA_SUCCESS;
}

static inline int write_format_1_and_2_sample_id_and_timestamp_to_file(
	FILE *file,
	uint32_t sample_id,
	uint32_t earliest_data_timestamp_h,
	uint32_t earliest_data_timestamp_l,
	uint32_t latest_data_timestamp_l,
	enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	int write_result = 0;
	uint32_t latest_timestamp_h;

	/* If the end timestamp lower bits is smaller than the start timestamp lower bits, it means the upper
	 * bits need to be advanced
	 */
	latest_timestamp_h = earliest_data_timestamp_h;
	if (latest_data_timestamp_l < earliest_data_timestamp_l)
		latest_timestamp_h++;

	write_result = fprintf(file, "\n%u", sample_id);
	if (write_result < 0) {
		DOCA_LOG_ERR("Failed to write sample_id to output file with errno=%s (%d)", strerror(errno), errno);
		return write_result;
	};
	write_result =
		write_timestamp_to_file(file, earliest_data_timestamp_h, earliest_data_timestamp_l, timestamp_source);
	if (write_result < 0) {
		DOCA_LOG_ERR("Failed to write start timestamp to output file with errno=%s (%d)",
			     strerror(errno),
			     errno);
		return write_result;
	};
	write_result = write_timestamp_to_file(file, latest_timestamp_h, latest_data_timestamp_l, timestamp_source);
	if (write_result < 0) {
		DOCA_LOG_ERR("Failed to write end timestamp to output file with errno=%s (%d)", strerror(errno), errno);
		return write_result;
	};
	return write_result;
}

/*
 * Process and write the results of a single query to the output file when using output format 1
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @num_actual_samples [in]: the number of samples that were returned from the query
 * @size_of_sample [in]: size of a single sample in the result buf
 * @timestamp_source [in]: the timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static inline doca_error_t telemetry_diag_sample_write_sample_format_1(
	struct telemetry_diag_sample_objects *sample_objects,
	uint32_t num_actual_samples,
	uint32_t size_of_sample,
	enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	int write_result = 0;

	struct doca_telemetry_diag_data_sample_format_1 *current_sample =
		(struct doca_telemetry_diag_data_sample_format_1 *)sample_objects->buf;
	for (uint32_t sample_index = 0; sample_index < num_actual_samples; sample_index++) {
		write_result =
			write_format_1_and_2_sample_id_and_timestamp_to_file(sample_objects->output_file,
									     current_sample->sample_id,
									     current_sample->earliest_data_timestamp_h,
									     current_sample->earliest_data_timestamp_l,
									     current_sample->latest_data_timestamp_l,
									     timestamp_source);
		if (write_result < 0) {
			DOCA_LOG_ERR("Failed to write to output file with errno=%s (%d)", strerror(errno), errno);
			return DOCA_ERROR_IO_FAILED;
		}

		for (uint32_t j = 0; j < sample_objects->num_data_ids; j++) {
			write_result = fprintf(sample_objects->output_file, ", %lu", current_sample->data_value[j]);
			if (write_result < 0) {
				DOCA_LOG_ERR("Failed to write to output file with errno=%s (%d)",
					     strerror(errno),
					     errno);
				return DOCA_ERROR_IO_FAILED;
			}
		}

		/* Update the pointer to the next sample location */
		current_sample =
			(doca_telemetry_diag_data_sample_format_1 *)((uint8_t *)current_sample + size_of_sample);
	}
	return DOCA_SUCCESS;
}

/*
 * Process and write the results of a single query to the output file when using output format 2
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @num_actual_samples [in]: the number of samples that were returned from the query
 * @size_of_sample [in]: size of a single sample in the result buf
 * @timestamp_source [in]: the timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static inline doca_error_t telemetry_diag_sample_write_sample_format_2(
	struct telemetry_diag_sample_objects *sample_objects,
	uint32_t num_actual_samples,
	uint32_t size_of_sample,
	enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	int write_result = 0;

	struct doca_telemetry_diag_data_sample_format_2 *current_sample =
		(struct doca_telemetry_diag_data_sample_format_2 *)sample_objects->buf;
	for (uint32_t sample_index = 0; sample_index < num_actual_samples; sample_index++) {
		write_result =
			write_format_1_and_2_sample_id_and_timestamp_to_file(sample_objects->output_file,
									     current_sample->sample_id,
									     current_sample->earliest_data_timestamp_h,
									     current_sample->earliest_data_timestamp_l,
									     current_sample->latest_data_timestamp_l,
									     timestamp_source);
		if (write_result < 0) {
			DOCA_LOG_ERR("Failed to write to output file with errno=%s (%d)", strerror(errno), errno);
			return DOCA_ERROR_IO_FAILED;
		}

		for (uint32_t j = 0; j < sample_objects->num_data_ids; j++) {
			write_result = fprintf(sample_objects->output_file, ", %u", current_sample->data_value[j]);
			if (write_result < 0) {
				DOCA_LOG_ERR("Failed to write to output file with errno=%s (%d)",
					     strerror(errno),
					     errno);
				return DOCA_ERROR_IO_FAILED;
			}
		}

		/* Update the pointer to the next sample location */
		current_sample =
			(doca_telemetry_diag_data_sample_format_2 *)((uint8_t *)current_sample + size_of_sample);
	}
	return DOCA_SUCCESS;
}

/*
 * Process and write the results of a single query to the output file
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @num_actual_samples [in]: the number of samples that were returned from the query
 * @size_of_sample [in]: size of a single sample in the result buf
 * @output_format [in]: the output format used by the sample
 * @timestamp_source [in]: the timestamp source used by the sample
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_diag_sample_write_output(struct telemetry_diag_sample_objects *sample_objects,
						       uint32_t num_actual_samples,
						       uint32_t size_of_sample,
						       enum doca_telemetry_diag_output_format output_format,
						       enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	switch (output_format) {
	case DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_0:
		return telemetry_diag_sample_write_sample_format_0(sample_objects,
								   num_actual_samples,
								   size_of_sample,
								   timestamp_source);
	case DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_1:
		return telemetry_diag_sample_write_sample_format_1(sample_objects,
								   num_actual_samples,
								   size_of_sample,
								   timestamp_source);
	case DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_2:
		return telemetry_diag_sample_write_sample_format_2(sample_objects,
								   num_actual_samples,
								   size_of_sample,
								   timestamp_source);
	default:
		DOCA_LOG_ERR("Output format %d not recognized", output_format);
		return DOCA_ERROR_NOT_SUPPORTED;
	}
}

/*
 * Run the main loop for query counters in repetitive sample mode
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @max_num_samples_per_read [in]: max number of samples per read
 * @total_run_time_nsec [in]: the total run time for the query loop
 * @size_of_sample [in]: size of a single sample in the result buf
 * @poll_interval [in]: Average time between calls to read samples
 * @output_format [in]: the output format to use
 * @timestamp_source [in]: the timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_diag_sample_run_query_counters_repetitive(
	struct telemetry_diag_sample_objects *sample_objects,
	uint32_t max_num_samples_per_read,
	uint64_t total_run_time_nsec,
	uint32_t size_of_sample,
	uint64_t poll_interval,
	enum doca_telemetry_diag_output_format output_format,
	enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	uint64_t process_period_nsec;
	struct timespec t_period_start = {0, 0};			      /* Will be used per sample */
	struct timespec t_polling_start = {0, 0}, t_polling_current = {0, 0}; /* Will be used for overall runtime */
	uint32_t num_actual_samples = 0;

	doca_error_t result;

	if (clock_gettime(CLOCK_REALTIME, &t_polling_start) < 0) {
		DOCA_LOG_ERR("Failed to get time with errno=%s (%d)", strerror(errno), errno);
	}
	t_polling_current = t_polling_start;

	while (telemetry_diag_sample_time_diff_nsec(t_polling_start, t_polling_current) < total_run_time_nsec) {
		result = doca_telemetry_diag_query_counters(sample_objects->telemetry_diag_obj,
							    sample_objects->buf,
							    max_num_samples_per_read,
							    &num_actual_samples);
		if (result == DOCA_ERROR_SKIPPED) {
			DOCA_LOG_INFO("One or more samples were skipped");
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query counters with error=%s", doca_error_get_name(result));
			return result;
		}

		/* Start counting processing time */
		if (clock_gettime(CLOCK_REALTIME, &t_period_start) < 0) {
			DOCA_LOG_ERR("Failed to get time with errno=%s (%d)", strerror(errno), errno);
		}
		result = telemetry_diag_sample_write_output(sample_objects,
							    num_actual_samples,
							    size_of_sample,
							    output_format,
							    timestamp_source);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process samples with error=%s", doca_error_get_name(result));
			return result;
		}

		if (clock_gettime(CLOCK_REALTIME, &t_polling_current) < 0) {
			DOCA_LOG_ERR("Failed to get time with errno=%s (%d)", strerror(errno), errno);
		}

		process_period_nsec = telemetry_diag_sample_time_diff_nsec(t_period_start, t_polling_current);
		/* If we got the max num of samples when querying, it is possible there are more samples to be polled,
		 * so sleep only if we got less samples from max. */
		if ((num_actual_samples < max_num_samples_per_read) && poll_interval > process_period_nsec)
			usleep((poll_interval - process_period_nsec) / 1000); /* Convert nseconds to useconds*/
	}
	return DOCA_SUCCESS;
}

/*
 * Run the main loop for query counters until num_samples_to_read samples are read
 *
 * @sample_objects [in]: sample objects struct for the sample
 * @num_samples_to_read [in]: the total number of samples to read
 * @max_num_samples_per_read [in]: max number of samples per read
 * @size_of_sample [in]: size of a single sample in the result buf
 * @output_format [in]: the output format to use
 * @timestamp_source [in]: the timestamp source to use
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_diag_sample_run_query_counters_by_max_samples(
	struct telemetry_diag_sample_objects *sample_objects,
	uint32_t num_samples_to_read,
	uint32_t max_num_samples_per_read,
	uint32_t size_of_sample,
	enum doca_telemetry_diag_output_format output_format,
	enum doca_telemetry_diag_timestamp_source timestamp_source)
{
	uint32_t num_actual_samples = 0;
	uint32_t total_num_samples_read = 0;
	uint32_t num_samples_to_read_in_query = max_num_samples_per_read;

	doca_error_t result;

	while (total_num_samples_read < num_samples_to_read) {
		if (num_samples_to_read_in_query > (num_samples_to_read - total_num_samples_read))
			num_samples_to_read_in_query = (num_samples_to_read - total_num_samples_read);

		result = doca_telemetry_diag_query_counters(sample_objects->telemetry_diag_obj,
							    sample_objects->buf,
							    num_samples_to_read_in_query,
							    &num_actual_samples);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query counters with error=%s", doca_error_get_name(result));
			return result;
		}

		result = telemetry_diag_sample_write_output(sample_objects,
							    num_actual_samples,
							    size_of_sample,
							    output_format,
							    timestamp_source);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process samples with error=%s", doca_error_get_name(result));
			return result;
		}

		total_num_samples_read += num_actual_samples;
	}
	return DOCA_SUCCESS;
}

/*
 * Reconfigure the sample period
 *
 * @diag [in]: the telemetry diag object
 * @reconfig_sample_period [in]: the reconfig sample period in nanoseconds
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_diag_sample_reconfig_sample_period(struct doca_telemetry_diag *diag,
								 uint64_t reconfig_sample_period)
{
	uint64_t actual_sample_period;
	doca_error_t result;

	result = doca_telemetry_diag_reconfig_sample_period(diag, reconfig_sample_period);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to reconfig sample period with error=%s", doca_error_get_name(result));
		return result;
	}

	/* Value may differ if the device cannot support the requested sample period */
	result = doca_telemetry_diag_get_sample_period(diag, &actual_sample_period);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get actual reconfigured sample period with error=%s",
			     doca_error_get_name(result));
		return result;
	}
	if (actual_sample_period != reconfig_sample_period) {
		DOCA_LOG_WARN("Actual sample period %lu is different from requested reconfig sample period %lu",
			      actual_sample_period,
			      reconfig_sample_period);
	}

	return DOCA_SUCCESS;
}

doca_error_t telemetry_diag_sample_run(const struct telemetry_diag_sample_cfg *cfg)
{
	doca_error_t result = DOCA_SUCCESS, teardown_result = DOCA_SUCCESS;
	struct telemetry_diag_sample_objects sample_objects = {0};

	uint32_t size_of_sample;
	uint32_t buf_size;

	uint64_t actual_sample_period;
	uint64_t poll_interval;
	uint64_t total_run_time_nsec = (uint64_t)cfg->run_time * SECS_TO_NSECS_CONVERSION; /* convert total poll
											    * time to nsec
											    */

	DOCA_LOG_DBG("Started doca_telemetry_diag sample with the following parameters: ");
	DOCA_LOG_DBG("	pci_addr='%s'", cfg->pci_addr);
	DOCA_LOG_DBG("	sample_run_time=%u", cfg->run_time);
	DOCA_LOG_DBG("	output='%s'", cfg->output_path);
	DOCA_LOG_DBG("	sample_period=%lu", cfg->sample_period);
	DOCA_LOG_DBG("	log_num_samples=%u", cfg->log_max_num_samples);
	DOCA_LOG_DBG("	max_samples_per_read=%u", cfg->max_num_samples_per_read);
	DOCA_LOG_DBG("	sample_mode=%u", cfg->sample_mode);
	DOCA_LOG_DBG("	output_format=%u", cfg->output_format);
	DOCA_LOG_DBG("	sync_mode=%u", cfg->sync_mode);
	DOCA_LOG_DBG("	timestamp_source=%u", cfg->timestamp_source);
	DOCA_LOG_DBG("	force_ownership=%u", cfg->force_ownership);
	DOCA_LOG_DBG("	data_ids='%s'", cfg->data_ids_input_path);
	DOCA_LOG_DBG("	num_data_id=%u", cfg->num_data_ids);
	DOCA_LOG_DBG("	reconfig_sample_period=%lu", cfg->reconfig_sample_period);
	for (uint32_t i = 0; i < cfg->num_data_ids; i++) {
		DOCA_LOG_DBG("	entry %u: data_id=%llx, name='%s'",
			     i,
			     (unsigned long long int)cfg->data_ids_struct[i].data_id,
			     cfg->data_ids_struct[i].name);
	}

	if (cfg->output_format > DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_2) {
		DOCA_LOG_ERR("Output format %u is currently not supported by the sample", cfg->output_format);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (cfg->sync_mode > DOCA_TELEMETRY_DIAG_SYNC_MODE_SYNC_START) {
		DOCA_LOG_ERR("Sync mode %u is currently not supported by the sample", cfg->sync_mode);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (cfg->timestamp_source > DOCA_TELEMETRY_DIAG_TIMESTAMP_SOURCE_RTC) {
		DOCA_LOG_ERR("Timestamp source %u is currently not supported by the sample", cfg->timestamp_source);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Open DOCA device based on the given PCI address */
	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &sample_objects.dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device with error=%s", doca_error_get_name(result));
	}

	result = telemetry_diag_sample_context_init(&sample_objects,
						    cfg->data_ids_struct,
						    cfg->num_data_ids,
						    cfg->sample_period,
						    cfg->log_max_num_samples,
						    cfg->sample_mode,
						    cfg->output_format,
						    cfg->sync_mode,
						    cfg->timestamp_source,
						    cfg->force_ownership);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init sample objects with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	sample_objects.output_file = fopen(cfg->output_path, "wr");
	if (sample_objects.output_file == NULL) {
		DOCA_LOG_ERR("Failed to open output file \"%s\" with errno=%s (%d)",
			     cfg->output_path,
			     strerror(errno),
			     errno);
		result = DOCA_ERROR_NO_MEMORY;
		goto teardown;
	}

	/* Write first line of CSV output file*/
	if (cfg->output_format == DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_0) {
		fprintf(sample_objects.output_file, "sample_id");
		for (uint32_t i = 0; i < cfg->num_data_ids; i++) {
			fprintf(sample_objects.output_file, ", data_id, sample_time, %s", cfg->data_ids_struct[i].name);
		}
	} else {
		fprintf(sample_objects.output_file, "sample_id, sample_time_start, sample_time_end");
		for (uint32_t i = 0; i < cfg->num_data_ids; i++) {
			fprintf(sample_objects.output_file, ", %s", cfg->data_ids_struct[i].name);
		}
	}

	result = doca_telemetry_diag_start(sample_objects.telemetry_diag_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_diag with error=%s", doca_error_get_name(result));
		goto teardown;
	}
	sample_objects.started = 1;

	result = doca_telemetry_diag_get_sample_size(sample_objects.telemetry_diag_obj, &size_of_sample);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get sample size with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	buf_size = size_of_sample * cfg->max_num_samples_per_read;
	sample_objects.buf = malloc(buf_size);
	if (sample_objects.buf == NULL) {
		DOCA_LOG_ERR("Failed to allocate output buffer");
		result = DOCA_ERROR_NO_MEMORY;
		goto teardown;
	}

	/* Value may differ if the device cannot support the requested sample period */
	result = doca_telemetry_diag_get_sample_period(sample_objects.telemetry_diag_obj, &actual_sample_period);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get sample period with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	switch ((cfg->sample_mode)) {
	case DOCA_TELEMETRY_DIAG_SAMPLE_MODE_SINGLE:
		/* Sleep for the time it takes to fill the whole buffer, then query the results */
		usleep((actual_sample_period * (1U << cfg->log_max_num_samples)) / 1000); /* Convert nseconds to
											     useconds*/
	/* Fallthrough */
	case DOCA_TELEMETRY_DIAG_SAMPLE_MODE_ON_DEMAND:
		result = telemetry_diag_sample_run_query_counters_by_max_samples(&sample_objects,
										 (1U << cfg->log_max_num_samples),
										 cfg->max_num_samples_per_read,
										 size_of_sample,
										 cfg->output_format,
										 cfg->timestamp_source);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to run query counters with error=%s", doca_error_get_name(result));
			goto teardown;
		}

		if ((cfg->reconfig_sample_period == 0 &&
		     cfg->sample_mode == DOCA_TELEMETRY_DIAG_SAMPLE_MODE_ON_DEMAND) ||
		    (cfg->reconfig_sample_period != 0 && cfg->sample_mode == DOCA_TELEMETRY_DIAG_SAMPLE_MODE_SINGLE)) {
			result = telemetry_diag_sample_reconfig_sample_period(sample_objects.telemetry_diag_obj,
									      cfg->reconfig_sample_period);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to reconfig sample period with error=%s",
					     doca_error_get_name(result));
				goto teardown;
			}
			result = telemetry_diag_sample_run_query_counters_by_max_samples(&sample_objects,
											 (1U
											  << cfg->log_max_num_samples),
											 cfg->max_num_samples_per_read,
											 size_of_sample,
											 cfg->output_format,
											 cfg->timestamp_source);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to run query counters after reconfig new sample period with error=%s",
					doca_error_get_name(result));
			}
		}
		break;
	case DOCA_TELEMETRY_DIAG_SAMPLE_MODE_REPETITIVE:
		poll_interval = actual_sample_period * (cfg->max_num_samples_per_read - 1); /* Average time between
											calls to read samples. */
		result = telemetry_diag_sample_run_query_counters_repetitive(&sample_objects,
									     cfg->max_num_samples_per_read,
									     total_run_time_nsec,
									     size_of_sample,
									     poll_interval,
									     cfg->output_format,
									     cfg->timestamp_source);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to run query counters with error=%s", doca_error_get_name(result));
			goto teardown;
		}

		if (cfg->reconfig_sample_period != 0) {
			result = telemetry_diag_sample_reconfig_sample_period(sample_objects.telemetry_diag_obj,
									      cfg->reconfig_sample_period);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to reconfig sample period with error=%s",
					     doca_error_get_name(result));
				goto teardown;
			}
			result = telemetry_diag_sample_run_query_counters_repetitive(&sample_objects,
										     cfg->max_num_samples_per_read,
										     total_run_time_nsec,
										     size_of_sample,
										     poll_interval,
										     cfg->output_format,
										     cfg->timestamp_source);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to run query counters after reconfig new sample period with error=%s",
					doca_error_get_name(result));
			}
		}
		break;
	default:
		DOCA_LOG_ERR("Failed to run query counters: unknown sample mode %d", cfg->sample_mode);
		break;
	}

teardown:
	teardown_result = telemetry_diag_sample_cleanup(&sample_objects);
	if (teardown_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Teardown failed with error=%s", doca_error_get_name(teardown_result));
		DOCA_ERROR_PROPAGATE(result, teardown_result);
	}
	return result;
}
