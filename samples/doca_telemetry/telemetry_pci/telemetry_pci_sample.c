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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <doca_telemetry_pci.h>

#include "common.h"
#include "telemetry_pci_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_PCI::SAMPLE);

static const char *doca_telemetry_pci_link_width_to_string(enum doca_telemetry_pci_link_width width)
{
	switch (width) {
	case DOCA_TELEMETRY_PCI_LINK_WIDTH_X1:
		return "x1";
	case DOCA_TELEMETRY_PCI_LINK_WIDTH_X2:
		return "x2";
	case DOCA_TELEMETRY_PCI_LINK_WIDTH_X4:
		return "x4";
	case DOCA_TELEMETRY_PCI_LINK_WIDTH_X8:
		return "x8";
	case DOCA_TELEMETRY_PCI_LINK_WIDTH_X16:
		return "x16";
	}

	return "Unknown";
}

static const char *doca_telemetry_pci_link_speed_to_string(enum doca_telemetry_pci_link_speed speed)
{
	switch (speed) {
	case DOCA_TELEMETRY_PCI_LINK_SPEED_2_5_G:
		return "2.5g";
	case DOCA_TELEMETRY_PCI_LINK_SPEED_5_G:
		return "5g";
	case DOCA_TELEMETRY_PCI_LINK_SPEED_8_G:
		return "8g";
	case DOCA_TELEMETRY_PCI_LINK_SPEED_16_G:
		return "16g";
	case DOCA_TELEMETRY_PCI_LINK_SPEED_32_G:
		return "32g";
	case DOCA_TELEMETRY_PCI_LINK_SPEED_32_G_PAM_4:
		return "32g(pam4)";
	}

	return "Unknown";
}

static const char *doca_telemetry_pci_data_size_to_string(enum doca_telemetry_pci_data_size size)
{
	switch (size) {
	case DOCA_TELEMETRY_PCI_DATA_SIZE_128B:
		return "128";
	case DOCA_TELEMETRY_PCI_DATA_SIZE_256B:
		return "256";
	case DOCA_TELEMETRY_PCI_DATA_SIZE_512B:
		return "512";
	case DOCA_TELEMETRY_PCI_DATA_SIZE_1024B:
		return "1024";
	case DOCA_TELEMETRY_PCI_DATA_SIZE_2048B:
		return "2048";
	case DOCA_TELEMETRY_PCI_DATA_SIZE_4096B:
		return "4096";
	}

	return "Unknown";
}

static const char *doca_telemetry_pci_power_status_to_string(enum doca_telemetry_pci_power_status status)
{
	switch (status) {
	case DOCA_TELEMETRY_PCI_POWER_STATUS_NO_VALUE:
		return "N/A";
	case DOCA_TELEMETRY_PCI_POWER_STATUS_OK:
		return "OK";
	case DOCA_TELEMETRY_PCI_POWER_STATUS_LOW_POWER:
		return "Low power";
	}

	return "Unknown";
}

static const char *doca_telemetry_pci_port_type_to_string(enum doca_telemetry_pci_port_type type)
{
	switch (type) {
	case DOCA_TELEMETRY_PCI_PORT_TYPE_PCIE_ENDPOINT:
		return "endpoint";
	case DOCA_TELEMETRY_PCI_PORT_TYPE_PCIE_ROOT_PORT:
		return "root port";
	case DOCA_TELEMETRY_PCI_PORT_TYPE_PCIE_UPSTREAM:
		return "upstream";
	case DOCA_TELEMETRY_PCI_PORT_TYPE_PCIE_DOWNSTREAM:
		return "downstream";
	}

	return "Unknown";
}

static const char *doca_telemetry_pci_lane_reversal_mode_to_string(enum doca_telemetry_pci_lane_reversal_mode mode)
{
	switch (mode) {
	case DOCA_TELEMETRY_PCI_LANE_REVERSAL_MODE_STRAIGHT:
		return "straight";
	case DOCA_TELEMETRY_PCI_LANE_REVERSAL_MODE_REVERSAL:
		return "reversed";
	}

	return "Unknown";
}

static void fetch_and_display_management_info(struct doca_devinfo *devinfo,
					      struct doca_telemetry_pci *pci,
					      struct doca_telemetry_pci_dpn dpn)
{
	doca_error_t result;
	struct doca_telemetry_pci_management_info data = {0};
	bool power_reporting_supported;
	bool link_peer_max_speed_supported;
	power_reporting_supported = doca_telemetry_pci_cap_management_info_power_reporting_is_supported(devinfo) ==
				    DOCA_SUCCESS;

	link_peer_max_speed_supported =
		doca_telemetry_pci_cap_management_info_link_peer_max_speed_is_supported(devinfo) == DOCA_SUCCESS;

	result = doca_telemetry_pci_read_management_info(pci, dpn, &data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read Managament info. Err: %s", doca_error_get_name(result));
		return;
	}

	printf("doca_telemetry_pci_management_info sub-caps:\n");
	printf("\tpower_reporting_supported: %u\n", power_reporting_supported);
	printf("\tlink_peer_max_speed_supported: %u\n", link_peer_max_speed_supported);
	printf("doca_telemetry_pci_management_info data:\n");
	printf("\tlink_width_enabled: %s\n", doca_telemetry_pci_link_width_to_string(data.link_width_enabled));
	printf("\tlink_speed_enabled: %s\n", doca_telemetry_pci_link_speed_to_string(data.link_speed_enabled));
	printf("\tlink_width_active: %s\n", doca_telemetry_pci_link_width_to_string(data.link_width_active));
	printf("\tlink_speed_active: %s\n", doca_telemetry_pci_link_speed_to_string(data.link_speed_active));
	printf("\tmax_read_request_size: %s\n", doca_telemetry_pci_data_size_to_string(data.max_read_request_size));
	printf("\tmax_payload_size: %s\n", doca_telemetry_pci_data_size_to_string(data.max_payload_size));
	printf("\tpwr_status: %s\n", doca_telemetry_pci_power_status_to_string(data.pwr_status));
	printf("\tport_type: %s\n", doca_telemetry_pci_port_type_to_string(data.port_type));
	printf("\tlane_reversal: %s\n", doca_telemetry_pci_lane_reversal_mode_to_string(data.lane_reversal));
	printf("\tlink_peer_max_speed: %s\n", doca_telemetry_pci_link_speed_to_string(data.link_peer_max_speed));
	printf("\tnum_of_pfs: %u\n", data.num_of_pfs);
	printf("\tnum_of_vfs: %u\n", data.num_of_vfs);
	printf("\tbdf0: %u\n", data.bdf0);
	printf("\tpci_power: %u\n", data.pci_power);
	printf("\tlane0_physical_position: %u\n", data.lane0_physical_position);
	printf("\tprecode_sup: %u\n", data.precode_sup);
	printf("\tprecode_active: %u\n", data.precode_active);
	printf("\tflit_sup: %u\n", data.flit_sup);
	printf("\tflit_active: %u\n", data.flit_active);
	printf("\tcorrectable_error_detected: %u\n", data.correctable_error_detected);
	printf("\tnon_fatal_error_detected: %u\n", data.non_fatal_error_detected);
	printf("\tfatal_error_detected: %u\n", data.fatal_error_detected);
	printf("\tunsupported_request_detected: %u\n", data.unsupported_request_detected);
	printf("\taux_power_detected: %u\n", data.aux_power_detected);
	printf("\ttransaction_pending: %u\n", data.transaction_pending);
}

static void fetch_and_display_perf_counters_1(struct doca_devinfo *devinfo,
					      struct doca_telemetry_pci *pci,
					      struct doca_telemetry_pci_dpn dpn)
{
	doca_error_t result;
	bool tx_overflow_supported;
	bool outbound_stalled_supported;
	bool fec_error_supported;
	bool fber_supported;
	struct doca_telemetry_pci_perf_counters_1 data = {0};

	tx_overflow_supported = doca_telemetry_pci_cap_perf_counters_1_tx_overflow_is_supported(devinfo) ==
				DOCA_SUCCESS;

	outbound_stalled_supported = doca_telemetry_pci_cap_perf_counters_1_outbound_stalled_is_supported(devinfo) ==
				     DOCA_SUCCESS;

	fec_error_supported = doca_telemetry_pci_cap_perf_counters_1_fec_error_is_supported(devinfo) == DOCA_SUCCESS;

	fber_supported = doca_telemetry_pci_cap_perf_counters_1_fber_is_supported(devinfo) == DOCA_SUCCESS;

	result = doca_telemetry_pci_read_perf_counters_1(pci, dpn, &data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read Perf counters group 1. Err: %s", doca_error_get_name(result));
		return;
	}

	printf("doca_telemetry_pci_perf_counters_1 sub-caps:\n");
	printf("\tx_overflow_supported: %u\n", tx_overflow_supported);
	printf("\toutbound_stalled_supported: %u\n", outbound_stalled_supported);
	printf("\tfec_error_counters_supported: %u\n", fec_error_supported);
	printf("\tfber_counter_supported: %u\n", fber_supported);
	printf("doca_telemetry_pci_perf_counters_1 data:\n");
	printf("\ttx_overflow_buffer_pkt: %lu\n", data.tx_overflow_buffer_pkt);
	printf("\ttx_overflow_buffer_marked_pkt: %lu\n", data.tx_overflow_buffer_marked_pkt);
	printf("\trx_errors: %u\n", data.rx_errors);
	printf("\ttx_errors: %u\n", data.tx_errors);
	printf("\tcrc_error_dllp: %u\n", data.crc_error_dllp);
	printf("\tcrc_error_tlp: %u\n", data.crc_error_tlp);
	printf("\toutbound_stalled_reads: %u\n", data.outbound_stalled_reads);
	printf("\toutbound_stalled_writes: %u\n", data.outbound_stalled_writes);
	printf("\toutbound_stalled_reads_events: %u\n", data.outbound_stalled_reads_events);
	printf("\toutbound_stalled_writes_events: %u\n", data.outbound_stalled_writes_events);
	printf("\tfec_correctable_error_counter: %u\n", data.fec_correctable_error_counter);
	printf("\tfec_uncorrectable_error_counter: %u\n", data.fec_uncorrectable_error_counter);
	printf("\tl0_to_recovery: %u\n", data.l0_to_recovery);
	printf("\teffective_ber: %ue%u\n", data.effective_ber_magnitude, data.effective_ber_coef);
	printf("\tfber: %ue%u\n", data.fber_magnitude, data.fber_coef);
}

static void fetch_and_display_latency_histogram(struct doca_devinfo *devinfo,
						struct doca_telemetry_pci *pci,
						struct doca_telemetry_pci_dpn dpn)
{
	(void)(devinfo);

	doca_error_t result;
	uint32_t bucket_count;
	uint32_t bucket_width;
	uint64_t *bucket_data;
	uint32_t bin_lower_bound;
	uint32_t bin_upper_bound;

	result = doca_telemetry_pci_get_latency_histogram_dimensions(pci, dpn, &bucket_count, &bucket_width);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read Latency histogram dimmensions. Err: %s", doca_error_get_name(result));
		return;
	}

	bucket_data = (uint64_t *)malloc(sizeof(uint64_t) * bucket_count);
	if (bucket_data == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory to hold histogram buckets data");
		return;
	}

	memset(bucket_data, 0, sizeof(uint64_t) * bucket_count);

	result = doca_telemetry_pci_read_latency_histogram(pci, dpn, bucket_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read Latency histogram. Err: %s", doca_error_get_name(result));
		free(bucket_data);
		return;
	}

	bin_lower_bound = 0;
	bin_upper_bound = bucket_width - 1;

	printf("doca_telemetry_pci latency histogram:\n");
	for (uint32_t ii = 0; ii != bucket_count; ++ii) {
		if (ii == (bucket_count - 1))
			printf("\t[%2u] : %uns+\n", ii, bin_lower_bound);
		else
			printf("\t[%2u] : %uns->%uns\n", ii, bin_lower_bound, bin_upper_bound);

		printf("\t%lu\n", bucket_data[ii]);

		bin_lower_bound += bucket_width;
		bin_upper_bound += bucket_width;
	}

	free(bucket_data);
}

doca_error_t telemetry_pci_sample_run(const struct telemetry_pci_sample_cfg *cfg)
{
	struct doca_telemetry_pci *pci;
	struct doca_dev *dev;
	doca_error_t result, feature;

	/* Open DOCA device based on the given PCI address */
	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device with error=%s", doca_error_get_name(result));
		return result;
	}

	/* Create telemetry context */
	result = doca_telemetry_pci_create(dev, &pci);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create telemetry pci context. Error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	/* Create telemetry context */
	result = doca_telemetry_pci_start(pci);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry pci context. Error=%s", doca_error_get_name(result));
		goto destroy_context;
	}

	/* Run each supported feature - if they are not supported print a log and continue */
	feature = doca_telemetry_pci_cap_management_info_is_supported(doca_dev_as_devinfo(dev));
	if (feature == DOCA_SUCCESS) {
		fetch_and_display_management_info(doca_dev_as_devinfo(dev), pci, cfg->dpn);
	} else {
		DOCA_LOG_INFO("doca_telemetry_pci management info is not supported on this device");
	}

	feature = doca_telemetry_pci_cap_perf_counters_1_is_supported(doca_dev_as_devinfo(dev));
	if (feature == DOCA_SUCCESS) {
		fetch_and_display_perf_counters_1(doca_dev_as_devinfo(dev), pci, cfg->dpn);
	} else {
		DOCA_LOG_INFO("doca_telemetry_pci performance counters group 1 is not supported on this device");
	}

	feature = doca_telemetry_pci_cap_latency_histogram_is_supported(doca_dev_as_devinfo(dev));
	if (feature == DOCA_SUCCESS) {
		fetch_and_display_latency_histogram(doca_dev_as_devinfo(dev), pci, cfg->dpn);
	} else {
		DOCA_LOG_INFO("doca_telemetry_pci latency histogram is not supported on this device");
	}

	(void)doca_telemetry_pci_stop(pci);
destroy_context:
	(void)doca_telemetry_pci_destroy(pci);
close_dev:
	doca_dev_close(dev);

	return result;
}
