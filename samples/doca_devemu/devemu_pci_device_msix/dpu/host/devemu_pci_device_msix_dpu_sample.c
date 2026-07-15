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

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_dpa.h>

#include <common.h>
#include <devemu_pci_common.h>

#define MSIX_TABLE_REGION_INDEX 0

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_MSIX_DPU);

/*
 * A struct that includes all needed info on registered kernels and is initialized during linkage by DPACC.
 * Variable name should be the token passed to DPACC with --app-name parameter.
 */
extern struct doca_dpa_app *devemu_pci_sample_app;

/**
 * MSI-X RPC declaration
 */
extern doca_dpa_func_t raise_msix_rpc;

/*
 * Create an MSI-X object for the given index
 *
 * @resources [in]: The sample resources
 * @msix_idx [in]: MSI-X vector index
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_msix_object(struct devemu_resources *resources, uint16_t msix_idx)
{
	doca_error_t result;

	const struct bar_region_config *msix_table_region = &msix_table_configs[MSIX_TABLE_REGION_INDEX];

	result = doca_devemu_pci_msix_create_on_dpa(resources->pci_dev,
						    msix_table_region->bar_id,
						    msix_table_region->start_address,
						    msix_idx,
						    /*dpa_user_data=*/0x0,
						    &resources->data_path.msix);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create MSI-X to be used in DPA");
		return result;
	}

	result = doca_devemu_pci_msix_get_dpa_handle(resources->data_path.msix, &resources->data_path.msix_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get MSI-X DPA handle");
		return result;
	}

	return result;
}

/*
 * Run DOCA Device Emulation MSI-X DPU sample
 *
 * @pci_address [in]: Device PCI address
 * @emulated_dev_vuid [in]: VUID of the emulated device
 * @msix_idx [in]: MSI-X vector index
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_msix_dpu(const char *pci_address, const char *emulated_dev_vuid, uint16_t msix_idx)
{
	doca_error_t result;
	struct devemu_resources resources = {0};
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = PCI_TYPE_NAME;
	bool destroy_rep = false;

	result = doca_pe_create(&resources.pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_type_create(pci_type_name, &resources.pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI type: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = find_supported_device(pci_address,
				       resources.pci_type,
				       doca_devemu_pci_cap_type_is_hotplug_supported,
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

	/* Initialize DPA context */
	result = init_dpa(&resources, devemu_pci_sample_app);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Find existing emulated device */
	result = find_emulated_device(resources.pci_type, emulated_dev_vuid, &resources.rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to find PCI emulated device representor: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Create emulated device context */
	result = doca_devemu_pci_dev_create(resources.pci_type, resources.rep, resources.pe, &resources.pci_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI emulated device context: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = doca_ctx_set_datapath_on_dpa(doca_devemu_pci_dev_as_ctx(resources.pci_dev), resources.dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PCI emulated device context datapath on DPA: %s",
			     doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = doca_ctx_start(doca_devemu_pci_dev_as_ctx(resources.pci_dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI emulated device context: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Defer assignment so that cleanup does not stop the context in case it was not started */
	resources.ctx = doca_devemu_pci_dev_as_ctx(resources.pci_dev);

	result = doca_devemu_pci_dev_get_hotplug_state(resources.pci_dev, &resources.hotplug_state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get hotplug state: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	if (resources.hotplug_state != DOCA_DEVEMU_PCI_HP_STATE_POWER_ON) {
		DOCA_LOG_ERR(
			"Expected hotplug state to be DOCA_DEVEMU_PCI_HP_STATE_POWER_ON instead current state is %s",
			hotplug_state_to_string(resources.hotplug_state));
		devemu_resources_cleanup(&resources, destroy_rep);
		return DOCA_ERROR_BAD_STATE;
	}

	result = create_msix_object(&resources, msix_idx);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	DOCA_LOG_INFO("Raising MSI-X at index %u", msix_idx);

	uint64_t rpc_ret;
	result = doca_dpa_rpc(resources.dpa, &raise_msix_rpc, &rpc_ret, resources.data_path.msix_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Raise MSI-X RPC execution failed");
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	DOCA_LOG_INFO("MSI-X raised successfully");

	/* Clean and destroy all relevant objects */
	devemu_resources_cleanup(&resources, destroy_rep);

	return result;
}
