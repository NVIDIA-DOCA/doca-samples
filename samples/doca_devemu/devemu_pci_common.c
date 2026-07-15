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

#include "devemu_pci_common.h"

#include <doca_dev.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_COMMON);

doca_error_t parse_pci_address(const char *addr, char *parsed_addr)
{
	int addr_len = strnlen(addr, DOCA_DEVINFO_PCI_ADDR_SIZE) + 1;

	/* Check using > to make static code analysis satisfied */
	if (addr_len > DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (addr_len != DOCA_DEVINFO_PCI_ADDR_SIZE && addr_len != DOCA_DEVINFO_PCI_BDF_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address does not match supported formats: XXXX:XX:XX.X or XX:XX.X");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(parsed_addr, addr, addr_len);

	return DOCA_SUCCESS;
}

doca_error_t register_pci_address_param(doca_argp_param_cb_t pci_callback)
{
	struct doca_argp_param *param;
	doca_error_t result;

	/* Create and register PCI address param */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "p");
	doca_argp_param_set_long_name(param, "pci-addr");
	doca_argp_param_set_description(param, "The DOCA device PCI address. Format: XXXX:XX:XX.X or XX:XX.X");
	doca_argp_param_set_callback(param, pci_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t parse_vuid(const char *vuid, char *parsed_vuid)
{
	int vuid_len = strnlen(vuid, DOCA_DEVINFO_REP_VUID_SIZE) + 1;

	/* Check using > to make static code analysis satisfied */
	if (vuid_len > DOCA_DEVINFO_REP_VUID_SIZE) {
		DOCA_LOG_ERR("Entered device VUID exceeding the maximum size of %d", DOCA_DEVINFO_REP_VUID_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(parsed_vuid, vuid, vuid_len);

	return DOCA_SUCCESS;
}

doca_error_t register_vuid_param(const char *description, doca_argp_param_cb_t vuid_callback)
{
	struct doca_argp_param *param;
	doca_error_t result;

	/* Create and register VUID param */
	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "u");
	doca_argp_param_set_long_name(param, "vuid");
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, vuid_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t find_supported_device(const char *pci_address,
				   const struct doca_devemu_pci_type *pci_type,
				   emulation_supported_cb_t has_support,
				   struct doca_dev **dev)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t res;
	size_t i;
	uint8_t is_supported;
	uint8_t is_equal;

	/* Set default return value */
	*dev = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		if (doca_devinfo_is_equal_pci_addr(dev_list[i], pci_address, &is_equal) != DOCA_SUCCESS ||
		    is_equal == 0)
			continue;

		res = has_support(dev_list[i], pci_type, &is_supported);
		if (res != DOCA_SUCCESS)
			continue;

		if (is_supported == 0) {
			DOCA_LOG_WARN(
				"Found device with matching address, but does not have hotplug support. Make sure a physical function was provided, and running with root permission");
			continue;
		}

		res = doca_dev_open(dev_list[i], dev);
		if (res == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			return res;
		}
	}

	DOCA_LOG_WARN("Matching device not found");

	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t find_emulated_device(struct doca_devemu_pci_type *pci_type, const char *vuid, struct doca_dev_rep **rep)
{
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_devs;
	uint32_t dev_idx;
	char actual_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	doca_error_t res;

	res = doca_devemu_pci_type_create_rep_list(pci_type, &rep_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create list of emulated devices: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (dev_idx = 0; dev_idx < nb_devs; ++dev_idx) {
		res = doca_devinfo_rep_get_vuid(rep_list[dev_idx], actual_vuid, DOCA_DEVINFO_REP_VUID_SIZE);
		if (res != DOCA_SUCCESS || strncmp(actual_vuid, vuid, DOCA_DEVINFO_REP_VUID_SIZE) != 0)
			continue;

		res = doca_dev_rep_open(rep_list[dev_idx], rep);
		if (res == DOCA_SUCCESS) {
			doca_devinfo_rep_destroy_list(rep_list);
			return res;
		}
	}

	DOCA_LOG_ERR("Matching emulated device not found");

	doca_devinfo_rep_destroy_list(rep_list);
	return DOCA_ERROR_NOT_FOUND;
}

/*
 * Sets the PCI configurations of the type
 * Once device is hotplugged the configurations will be visible to the Host as part of the
 * PCI configuration space of that device
 *
 * @pci_type [in]: The emulated PCI type
 * @dev [in]: The device that manages the PCI type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_pci_type_configurations(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev)
{
	const struct bar_memory_layout_config *layout_config;
	const struct bar_db_region_config *db_config;
	const struct bar_region_config *region_config;
	int idx;
	doca_error_t res;

	res = doca_devemu_pci_type_set_dev(pci_type, dev);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set device for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_device_id(pci_type, PCI_TYPE_DEVICE_ID);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set device ID for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_vendor_id(pci_type, PCI_TYPE_VENDOR_ID);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set vendor ID for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_subsystem_id(pci_type, PCI_TYPE_SUBSYSTEM_ID);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set subsystem ID for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_subsystem_vendor_id(pci_type, PCI_TYPE_SUBSYSTEM_VENDOR_ID);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set subsystem vendor ID for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_revision_id(pci_type, PCI_TYPE_REVISION_ID);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set revision ID for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_class_code(pci_type, PCI_TYPE_CLASS_CODE);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set class code for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	res = doca_devemu_pci_type_set_num_msix(pci_type, PCI_TYPE_NUM_MSIX);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of MSI-X for PCI type: %s", doca_error_get_descr(res));
		return res;
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MEMORY_LAYOUT; ++idx) {
		layout_config = &layout_configs[idx];
		res = doca_devemu_pci_type_set_memory_bar_conf(pci_type,
							       layout_config->bar_id,
							       layout_config->log_size,
							       layout_config->memory_type,
							       layout_config->prefetchable);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set layout at index %d: %s", idx, doca_error_get_descr(res));
			return res;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_DB_REGIONS; ++idx) {
		db_config = &db_configs[idx];
		if (db_config->with_data)
			res = doca_devemu_pci_type_set_bar_db_region_by_data_conf(pci_type,
										  db_config->region.bar_id,
										  db_config->region.start_address,
										  db_config->region.size,
										  db_config->log_db_size,
										  db_config->db_id_msbyte,
										  db_config->db_id_lsbyte);
		else
			res = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(pci_type,
										    db_config->region.bar_id,
										    db_config->region.start_address,
										    db_config->region.size,
										    db_config->log_db_size,
										    db_config->log_db_stride_size);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set DB region at index %d: %s", idx, doca_error_get_descr(res));
			return res;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_TABLE_REGIONS; ++idx) {
		region_config = &msix_table_configs[idx];
		res = doca_devemu_pci_type_set_bar_msix_table_region_conf(pci_type,
									  region_config->bar_id,
									  region_config->start_address,
									  region_config->size);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X table region at index %d: %s",
				     idx,
				     doca_error_get_descr(res));
			return res;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_PBA_REGIONS; ++idx) {
		region_config = &msix_pba_configs[idx];
		res = doca_devemu_pci_type_set_bar_msix_pba_region_conf(pci_type,
									region_config->bar_id,
									region_config->start_address,
									region_config->size);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X pending bit array region at index %d: %s",
				     idx,
				     doca_error_get_descr(res));
			return res;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_STATEFUL_REGIONS; ++idx) {
		region_config = &stateful_configs[idx];
		res = doca_devemu_pci_type_set_bar_stateful_region_conf(pci_type,
									region_config->bar_id,
									region_config->start_address,
									region_config->size);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set Stateful region at index %d: %s", idx, doca_error_get_descr(res));
			return res;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t configure_and_start_pci_type(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev)
{
	doca_error_t result;

	result = set_pci_type_configurations(pci_type, dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_devemu_pci_type_start(pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

void devemu_resources_cleanup(struct devemu_resources *resources, bool destroy_rep)
{
	doca_error_t res;

	if (resources->data_path.msix != NULL) {
		res = doca_devemu_pci_msix_destroy(resources->data_path.msix);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Device Emulation MSI-X object: %s",
				     doca_error_get_descr(res));

		resources->data_path.msix = NULL;
	}

	if (resources->data_path.db != NULL) {
		res = doca_devemu_pci_db_stop(resources->data_path.db);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Device Emulation DB object: %s", doca_error_get_descr(res));
		res = doca_devemu_pci_db_destroy(resources->data_path.db);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Device Emulation DB object: %s",
				     doca_error_get_descr(res));

		resources->data_path.db = NULL;
	}

	if (resources->db_comp != NULL) {
		res = doca_devemu_pci_db_completion_stop(resources->db_comp);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Device Emulation DB completion context: %s",
				     doca_error_get_descr(res));
		res = doca_devemu_pci_db_completion_destroy(resources->db_comp);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Device Emulation DB completion context: %s",
				     doca_error_get_descr(res));

		resources->db_comp = NULL;
	}

	if (resources->dpa_thread) {
		res = doca_dpa_thread_destroy(resources->dpa_thread);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA DPA thread: %s", doca_error_get_descr(res));

		resources->dpa_thread = NULL;
	}

	if (resources->ctx != NULL) {
		res = doca_ctx_stop(resources->ctx);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated Device context: %s", doca_error_get_descr(res));

		resources->ctx = NULL;
	}

	if (resources->stateful_region_values != NULL)
		free(resources->stateful_region_values);

	if (resources->pci_dev != NULL) {
		res = doca_devemu_pci_dev_destroy(resources->pci_dev);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Emulated Device context: %s", doca_error_get_descr(res));

		resources->pci_dev = NULL;
	}

	if (resources->rep != NULL) {
		res = destroy_rep ? doca_devemu_pci_type_destroy_rep(resources->rep) :
				    doca_dev_rep_close(resources->rep);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA Emulated Device representor: %s", doca_error_get_descr(res));

		resources->rep = NULL;
	}

	if (resources->dpa != NULL) {
		res = doca_dpa_destroy(resources->dpa);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA DPA: %s", doca_error_get_descr(res));

		resources->dpa = NULL;
	}

	if (resources->pci_type != NULL) {
		res = doca_devemu_pci_type_stop(resources->pci_type);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated PCI Type: %s", doca_error_get_descr(res));
	}

	if (resources->pci_type != NULL) {
		res = doca_devemu_pci_type_destroy(resources->pci_type);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Emulated PCI Type: %s", doca_error_get_descr(res));

		resources->pci_type = NULL;
	}

	if (resources->pe != NULL) {
		res = doca_pe_destroy(resources->pe);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(res));

		resources->pe = NULL;
	}

	if (resources->dev != NULL) {
		res = doca_dev_close(resources->dev);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(res));

		resources->dev = NULL;
	}
}

const char *hotplug_state_to_string(enum doca_devemu_pci_hotplug_state hotplug_state)
{
	switch (hotplug_state) {
	case DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF:
		return "DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF";
	case DOCA_DEVEMU_PCI_HP_STATE_UNPLUG_IN_PROGRESS:
		return "DOCA_DEVEMU_PCI_HP_STATE_UNPLUG_IN_PROGRESS";
	case DOCA_DEVEMU_PCI_HP_STATE_PLUG_IN_PROGRESS:
		return "DOCA_DEVEMU_PCI_HP_STATE_PLUG_IN_PROGRESS";
	case DOCA_DEVEMU_PCI_HP_STATE_POWER_ON:
		return "DOCA_DEVEMU_PCI_HP_STATE_POWER_ON";
	default:
		return "UNKNOWN";
	}
}

doca_error_t init_dpa(struct devemu_resources *resources, struct doca_dpa_app *dpa_app)
{
	doca_error_t ret = doca_dpa_create(resources->dev, &resources->dpa);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA context");
		return ret;
	}

	ret = doca_dpa_set_app(resources->dpa, dpa_app);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA app");
		return ret;
	}

	ret = doca_dpa_start(resources->dpa);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DPA context");
		return ret;
	}

	return DOCA_SUCCESS;
}
