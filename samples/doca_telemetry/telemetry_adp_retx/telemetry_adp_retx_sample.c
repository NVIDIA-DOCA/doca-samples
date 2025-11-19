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

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_telemetry_adp_retx.h>
#include <errno.h>
#include <unistd.h>

#include "common.h"
#include "telemetry_adp_retx_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_ADP_RETX::SAMPLE);

#define BIN_NUM_WIDTH 2
#define TIME_UNIT_WIDTH 7

/*
 * Set the configuration for the adp retx histogram based on user input
 *
 * @adp_retx [in]: Adp_retx context to configure
 * @cfg [in]: Sample configuration
 *
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
doca_error_t configure_histogram(struct doca_telemetry_adp_retx *adp_retx,
				 const struct telemetry_adp_retx_sample_cfg *cfg)
{
	doca_error_t result;

	result = doca_telemetry_adp_retx_set_hist_num_bins(adp_retx, cfg->bin_num);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set number of bins. error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_adp_retx_set_hist_bin0_width(adp_retx, cfg->bin0_width);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set bin 0 width. error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_adp_retx_set_hist_bin1_width(adp_retx, cfg->bin1_width);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set bin 1 width. error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_adp_retx_set_hist_bin_width_mode(adp_retx, cfg->bin_width_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set bin width mode. error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_adp_retx_set_hist_time_unit(adp_retx, cfg->time_unit);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set time unit. error=%s", doca_error_get_name(result));
		return result;
	}

	if (cfg->vhca_id != NO_VHCA_ID) {
		result = doca_telemetry_adp_retx_set_hist_vhca_id(adp_retx, cfg->vhca_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set vhca id. error=%s", doca_error_get_name(result));
			return result;
		}
	}

	if (cfg->clear_on_read == true) {
		result = doca_telemetry_adp_retx_set_hist_clear_on_read(adp_retx, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set clear on read value. error=%s", doca_error_get_name(result));
			return result;
		}
	}

	/* count enable is always set */
	result = doca_telemetry_adp_retx_set_hist_count_enable(adp_retx, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set count enable value. error=%s", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

static inline char *get_time_unit(enum doca_telemetry_adp_retx_hist_time_unit unit)
{
	switch (unit) {
	case DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_NSEC:
		return "nsec";
	case DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC:
		return "usec";
	case DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC_100:
		return "usec_100";
	case DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_MSEC:
		return "msec";
	default:
		return "invalid";
	}
}

/*
 * Run the sample and print histogram values
 *
 * @cfg [in]: Sample configuration
 *
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
doca_error_t telemetry_adp_retx_sample_run(const struct telemetry_adp_retx_sample_cfg *cfg)
{
	uint32_t max_bins, time_units, bins_populated, i, bin_width, mode_inc, next_start, reads;
	struct doca_telemetry_adp_retx *adp_retx;
	struct doca_dev *dev;
	uint64_t *bins;
	char *time_unit;
	doca_error_t result, tmp_result;

	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device. error=%s", doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_adp_retx_cap_is_supported(doca_dev_as_devinfo(dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Doca telemetry adp retx not supported. error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	result = doca_telemetry_adp_retx_cap_histogram_is_supported(doca_dev_as_devinfo(dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Doca telemetry adp retx histogram not supported. error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	result = doca_telemetry_adp_retx_cap_get_hist_time_units(doca_dev_as_devinfo(dev), &time_units);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get supported time units. error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	/* Log the supported time units */
	if (time_units & DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_NSEC)
		DOCA_LOG_INFO("Device cap - Supported time unit: NSECs");
	if (time_units & DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC)
		DOCA_LOG_INFO("Device cap - Supported time unit: USECs");
	if (time_units & DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_USEC_100)
		DOCA_LOG_INFO("Device cap - Supported time unit: USEC_100s");
	if (time_units & DOCA_TELEMETRY_ADP_RETX_HIST_TIME_UNIT_MSEC)
		DOCA_LOG_INFO("Device cap - Supported time unit: MSECs");

	if ((cfg->time_unit & time_units) == 0) {
		DOCA_LOG_ERR("Input time unit is not supported on the device. Supported bitmask (0x%x)", time_units);
		goto close_dev;
	}

	result = doca_telemetry_adp_retx_cap_get_hist_max_bins(doca_dev_as_devinfo(dev), &max_bins);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max_bins. error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	DOCA_LOG_INFO("Device cap - Max bins: %u", max_bins);

	if (cfg->bin_num > max_bins) {
		DOCA_LOG_ERR("Input bin number (%u) exceeds max bins (%u)", cfg->bin_num, max_bins);
		goto close_dev;
	}

	bins = malloc(sizeof(uint64_t) * max_bins);
	if (bins == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for histogram bins");
		result = DOCA_ERROR_NO_MEMORY;
		goto close_dev;
	}

	result = doca_telemetry_adp_retx_create(dev, &adp_retx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create adp_retx context. error=%s", doca_error_get_name(result));
		goto release_bins;
	}

	result = configure_histogram(adp_retx, cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure histogram. error=%s", doca_error_get_name(result));
		goto destroy_ctx;
	}

	result = doca_telemetry_adp_retx_start(adp_retx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context. error=%s", doca_error_get_name(result));
		goto destroy_ctx;
	}

	printf("\nHistogram started...\n");
	for (reads = 0; reads < cfg->num_reads; reads++) {
		/* Wait for a given time to allow histogram to capture data */
		sleep(cfg->wait_time);

		result = doca_telemetry_adp_retx_read_hist_bins(adp_retx, &bins_populated, bins);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read histogram bins. error=%s", doca_error_get_name(result));
			goto stop_ctx;
		}

		bin_width = cfg->bin0_width;
		mode_inc = cfg->bin_width_mode == DOCA_TELEMETRY_ADP_RETX_HIST_BIN_WIDTH_MODE_FIXED ? 1 : 2;
		next_start = 0;

		time_unit = get_time_unit(cfg->time_unit);

		printf("\nIteration: %u - Successfully read %u adaptive retransmission histogram bins:\n\n",
		       reads + 1,
		       bins_populated);
		for (i = 0; i < bins_populated; i++) {
			if (i == bins_populated - 1) {
				printf("Bin %*u [%*u -    ... ]%s: %lu\n",
				       BIN_NUM_WIDTH,
				       i,
				       TIME_UNIT_WIDTH,
				       next_start,
				       time_unit,
				       bins[i]);
			} else if (i == 0) {
				printf("Bin %*u [%*u - %*u]%s: %lu\n",
				       BIN_NUM_WIDTH,
				       i,
				       TIME_UNIT_WIDTH,
				       next_start,
				       TIME_UNIT_WIDTH,
				       bin_width - 1,
				       time_unit,
				       bins[i]);
				next_start = bin_width;
				bin_width = cfg->bin1_width;
			} else {
				printf("Bin %*u [%*u - %*u]%s: %lu\n",
				       BIN_NUM_WIDTH,
				       i,
				       TIME_UNIT_WIDTH,
				       next_start,
				       TIME_UNIT_WIDTH,
				       next_start + bin_width - 1,
				       time_unit,
				       bins[i]);
				next_start += bin_width;
				bin_width *= mode_inc;
			}
		}
		printf("\n\n");
	}

stop_ctx:
	tmp_result = doca_telemetry_adp_retx_stop(adp_retx);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop context. error=%s", doca_error_get_name(tmp_result));
destroy_ctx:
	tmp_result = doca_telemetry_adp_retx_destroy(adp_retx);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy adp_retx context. error=%s", doca_error_get_name(tmp_result));
release_bins:
	free(bins);
close_dev:
	(void)doca_dev_close(dev);

	return result;
}
