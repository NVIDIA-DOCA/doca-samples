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

#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>

#include <common.h>
#include <devemu_pci_host_common.h>
#include <devemu_pci_type_config.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_TLP_HANDLER_HOST::SAMPLE);

/*
 * Run DOCA Device Emulation TLP Handler Host sample
 *
 * @pci_address [in]: Emulated device PCI address
 * @vfio_group [in]: VFIO group ID
 * @region_index [in]: The index of the transaction region
 * @write_data [in]: The data to write to transaction region
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_tlp_handler_host(const char *pci_address,
						int vfio_group,
						int region_index,
						const char *write_data)
{
	doca_error_t result;
	struct devemu_host_resources resources = {0};

	resources.container_fd = -1;
	resources.group_fd = -1;
	resources.device_fd = -1;

	const struct bar_region_config *transaction_config = &transaction_configs[region_index];
	size_t data_len = strnlen(write_data, PCI_TYPE_MAX_TRANSACTION_REGION_SIZE);
	if (data_len > transaction_config->size) {
		DOCA_LOG_ERR("Write data size of %zuB exceeds region size of %luB", data_len, transaction_config->size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = init_vfio_device(&resources, vfio_group, pci_address);
	if (result != DOCA_SUCCESS) {
		devemu_host_resources_cleanup(&resources);
		return result;
	}

	result = map_bar_region_memory(&resources, transaction_config, &resources.transaction_region);
	if (result != DOCA_SUCCESS) {
		devemu_host_resources_cleanup(&resources);
		return result;
	}

	struct bar_mapped_region *transaction_region = &resources.transaction_region;
	if (data_len == 0) {
		// Read data
		char *dump = hex_dump(transaction_region->mem, transaction_config->size);
		DOCA_LOG_INFO("Reading transaction region at bar %u start address %zu size %zuB:\n%s",
			      transaction_config->bar_id,
			      transaction_config->start_address,
			      transaction_config->size,
			      dump);
		free(dump);
	} else {
		// Write data
		DOCA_LOG_INFO("Writing to transaction region at bar %u start address %zu size %zuB:\n",
			      transaction_config->bar_id,
			      transaction_config->start_address,
			      data_len);
		memcpy(transaction_region->mem, write_data, data_len);
	}

	return DOCA_SUCCESS;
}
