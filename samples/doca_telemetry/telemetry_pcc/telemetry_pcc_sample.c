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
#include <doca_telemetry_pcc.h>
#include <errno.h>
#include <unistd.h>

#include "common.h"
#include "telemetry_pcc_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_PCC::SAMPLE);

/*
 * Get and print algo information for each slot from the telemetry context
 *
 * @pcc [in]: pcc telemetry context
 * @dev [in]: doca_device in use
 * @slots_populated [out]: bitmap of slots populated
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t get_algo_information(struct doca_telemetry_pcc *pcc,
					 struct doca_dev *dev,
					 uint32_t *slots_populated)
{
	uint32_t max_slot_info_len, max_slots, id, major, minor, i;
	char *algo_info;
	doca_error_t result;

	result = doca_telemetry_pcc_cap_get_max_algo_slots(doca_dev_as_devinfo(dev), &max_slots);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get number of slots: error=%s", doca_error_get_name(result));
		return result;
	}

	printf("\n*************************************\n");
	printf("%u slots detected on card:\n", max_slots);

	result = doca_telemetry_pcc_cap_get_max_algo_info_len(doca_dev_as_devinfo(dev), &max_slot_info_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max algo info from device: error=%s", doca_error_get_name(result));
		return result;
	}

	algo_info = (char *)malloc(sizeof(char) * max_slot_info_len);
	if (algo_info == NULL) {
		DOCA_LOG_ERR("Failed to allocate array for algo info");
		return DOCA_ERROR_NO_MEMORY;
	}

	for (i = 0; i < max_slots; i++) {
		result = doca_telemetry_pcc_get_algo_id(pcc, i, &id);
		/* Bad state indicates an empty slot */
		if (result == DOCA_ERROR_BAD_STATE) {
			printf("Slot %u: Empty\n", i);
			continue;
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get algo id: Error=%s", doca_error_get_name(result));
			goto free_algo_info;
		}

		result = doca_telemetry_pcc_get_algo_major_version(pcc, i, &major);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get algo major version: Error=%s", doca_error_get_name(result));
			goto free_algo_info;
		}

		result = doca_telemetry_pcc_get_algo_minor_version(pcc, i, &minor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get algo minor version: Error=%s", doca_error_get_name(result));
			goto free_algo_info;
		}

		result = doca_telemetry_pcc_get_algo_info(pcc, i, algo_info);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed get algo info on slot %u: error=%s", i, doca_error_get_name(result));
			goto free_algo_info;
		}

		printf("Slot %u: ID %04x, version %u.%02u - %s\n", i, id, major, minor, algo_info);
		*slots_populated |= (1 << i);
	}
	printf("*************************************\n");

	/* At this point the function has completed successfully even if result says BAD STATE */
	result = DOCA_SUCCESS;

free_algo_info:
	free(algo_info);

	return result;
}

/*
 * Get and print counters from an algo slot
 *
 * @pcc [in]: pcc telemetry context
 * @dev [in]: doca_device in use
 * @slot [in]: algo slot to parse
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t get_counter_information(struct doca_telemetry_pcc *pcc, struct doca_dev *dev, uint8_t slot)
{
	uint32_t max_info, max_counters, counters_populated, i;
	char *counter_info;
	uint32_t *counters;
	doca_error_t result;

	printf("-------------------------------------\n");

	result = doca_telemetry_pcc_cap_get_max_num_counters(doca_dev_as_devinfo(dev), &max_counters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max num counters: Error=%s", doca_error_get_name(result));
		return result;
	}

	counters = (uint32_t *)malloc(sizeof(uint32_t) * max_counters);
	if (counters == NULL) {
		DOCA_LOG_ERR("Failed to allocate array for counters");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_telemetry_pcc_get_counters(pcc, slot, &counters_populated, counters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get counters: Error=%s", doca_error_get_name(result));
		goto free_counters;
	}

	printf("Number of counters found is %u\n", counters_populated);

	result = doca_telemetry_pcc_cap_get_max_counter_info_len(doca_dev_as_devinfo(dev), &max_info);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get max counter info: Error=%s", doca_error_get_name(result));
		return result;
	}

	counter_info = (char *)malloc(sizeof(char) * max_info);
	if (counter_info == NULL) {
		DOCA_LOG_ERR("Failed to allocate array for counter info");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_counters;
	}

	for (i = 0; i < counters_populated; i++) {
		result = doca_telemetry_pcc_get_counter_info(pcc, slot, i, counter_info);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get counter info: Error=%s", doca_error_get_name(result));
			goto free_counter_info;
		}

		printf("Counter %u: %u  - %s\n", i, counters[i], counter_info);
	}
	printf("-------------------------------------\n");

free_counter_info:
	free(counter_info);
free_counters:
	free(counters);

	return result;
}

doca_error_t telemetry_pcc_sample_run(const struct telemetry_pcc_sample_cfg *cfg)
{
	uint32_t slots_populated, slot, i;
	struct doca_telemetry_pcc *pcc;
	uint8_t algo_en, counters_en;
	struct doca_dev *dev;
	doca_error_t result;

	/* Open DOCA device based on the given PCI address */
	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device with error=%s", doca_error_get_name(result));
		return result;
	}

	/* Check for telemetry support */
	result = doca_telemetry_pcc_cap_is_supported(doca_dev_as_devinfo(dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Device does not have PCC telemetry support. Error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	/* Create telemetry context */
	result = doca_telemetry_pcc_create(dev, &pcc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create telemetry pcc context. Error=%s", doca_error_get_name(result));
		goto close_dev;
	}

	/* Parse all the slots on the card for PCC algos */
	slots_populated = 0;
	result = get_algo_information(pcc, dev, &slots_populated);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed getting algo information. Error=%s", doca_error_get_name(result));
		goto destroy_pcc;
	}

	/* Start the context for counter extraction */
	result = doca_telemetry_pcc_start(pcc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry pcc context. Error=%s", doca_error_get_name(result));
		goto destroy_pcc;
	}

	printf("\nContext is running. Parsing for counters...\n\n");

	while ((i = ffs(slots_populated))) {
		/* FFS returns bit position but slot index starts at 0 so need to subtract 1 */
		slot = i - 1;

		result = doca_telemetry_pcc_get_algo_enable_status(pcc, slot, &algo_en, &counters_en);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed getting Enabled status of slot %u: Error=%s",
				     slot,
				     doca_error_get_name(result));
			goto stop_pcc;
		}

		printf("Slot %u: algo %s, counters %s\n",
		       slot,
		       algo_en ? "ENABLED" : "DISABLED",
		       counters_en ? "ENABLED" : "DISABLED");

		/* Mark slot as parsed */
		slots_populated &= ~(1 << slot);

		if (counters_en == 0)
			continue;

		result = get_counter_information(pcc, dev, (uint8_t)slot);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed get get counters on slot %u: Error=%s", slot, doca_error_get_name(result));
			goto stop_pcc;
		}
	}
	printf("\n");

stop_pcc:
	(void)doca_telemetry_pcc_stop(pcc);
destroy_pcc:
	(void)doca_telemetry_pcc_destroy(pcc);
close_dev:
	doca_dev_close(dev);

	return result;
}
