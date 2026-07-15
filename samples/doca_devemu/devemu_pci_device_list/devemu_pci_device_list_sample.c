/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <devemu_pci_common.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(DPU_DEVEMU_PCI_DEVICE_LIST);

/*
 * Run DOCA Device Emulation List sample
 *
 * @pci_address [in]: Device PCI address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_list(const char *pci_address)
{
	doca_error_t result;
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_reps;
	uint32_t rep_idx;
	char rep_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	char rep_pci[DOCA_DEVINFO_REP_PCI_ADDR_SIZE];
	struct devemu_resources resources = {0};
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = PCI_TYPE_NAME;
	bool destroy_rep = false;

	result = doca_devemu_pci_type_create(pci_type_name, &resources.pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI type: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = find_supported_device(pci_address,
				       resources.pci_type,
				       doca_devemu_pci_cap_type_is_mgmt_supported,
				       &resources.dev);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Set PCIe configuration space values */
	result = configure_and_start_pci_type(resources.pci_type, resources.dev);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Get the list of emulated device representors that match this type */
	result = doca_devemu_pci_type_create_rep_list(resources.pci_type, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create list of emulated device representors: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	DOCA_LOG_INFO("Found a total of %u emulated devices", nb_reps);

	for (rep_idx = 0; rep_idx < nb_reps; ++rep_idx) {
		DOCA_LOG_INFO("Emulated PCI device representor:");
		result = doca_devinfo_rep_get_vuid(rep_list[rep_idx], rep_vuid, DOCA_DEVINFO_REP_VUID_SIZE);
		DOCA_LOG_INFO("\tVUID:\t%s", result == DOCA_SUCCESS ? rep_vuid : doca_error_get_name(result));
		result = doca_devinfo_rep_get_pci_addr_str(rep_list[rep_idx], rep_pci);
		DOCA_LOG_INFO("\tPCI:\t%s", result == DOCA_SUCCESS ? rep_pci : doca_error_get_name(result));
	}

	doca_devinfo_rep_destroy_list(rep_list);

	/* Clean and destroy all relevant objects */
	devemu_resources_cleanup(&resources, destroy_rep);

	return result;
}
