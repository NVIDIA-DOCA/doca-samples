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

#include "nvme_pci_common.h"

#include <doca_dev.h>

DOCA_LOG_REGISTER(NVME_PCI_COMMON);

doca_error_t find_supported_device(const char *dev_name,
				   const struct doca_devemu_pci_type *pci_type,
				   emulation_supported_cb_t has_support,
				   struct doca_dev **dev)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t res;
	size_t i;
	uint8_t is_supported;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};

	/* Set default return value */
	*dev = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get ibdev name for device: %s", doca_error_get_descr(res));
			doca_devinfo_destroy_list(dev_list);
			return res;
		}

		if (strncmp(dev_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == 0) {
			res = has_support(dev_list[i], pci_type, &is_supported);
			if (res != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to check the support capability: %s", doca_error_get_descr(res));
				doca_devinfo_destroy_list(dev_list);
				return res;
			}

			if (is_supported == 0) {
				DOCA_LOG_ERR(
					"Device doesn't support the hot plug capability. Make sure a physical function was provided, and running with root permission");
				doca_devinfo_destroy_list(dev_list);
				return DOCA_ERROR_NOT_SUPPORTED;
			}

			res = doca_dev_open(dev_list[i], dev);
			if (res != DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(res));
				return res;
			}

			doca_devinfo_destroy_list(dev_list);
			return DOCA_SUCCESS;
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
		doca_devinfo_rep_destroy_list(rep_list);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(res));
			return res;
		}
		return DOCA_SUCCESS;
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

doca_error_t create_find_start_pci_type(char *dev_name, struct doca_devemu_pci_type **pci_type, struct doca_dev **dev)
{
	doca_error_t ret;

	ret = doca_devemu_pci_type_create(NVME_TYPE_NAME, pci_type);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pci type");
		return ret;
	}

	ret = find_supported_device(dev_name, *pci_type, doca_devemu_pci_cap_type_is_hotplug_supported, dev);

	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to find supported device");
		cleanup_pci_resources(*pci_type, *dev);
		return ret;
	}

	ret = configure_and_start_pci_type(*pci_type, *dev);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure and start pci device");
		cleanup_pci_resources(*pci_type, *dev);
		return ret;
	}

	return DOCA_SUCCESS;
}

void cleanup_pci_resources(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev)
{
	doca_error_t ret;
	if (pci_type != NULL) {
		ret = doca_devemu_pci_type_stop(pci_type);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated PCI Type: %s", doca_error_get_descr(ret));

		ret = doca_devemu_pci_type_destroy(pci_type);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Emulated PCI Type: %s", doca_error_get_descr(ret));
	}

	if (dev != NULL) {
		ret = doca_dev_close(dev);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(ret));
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

#ifdef SPDK_APP_DEBUG
char *hex_dump(const void *data, size_t size)
{
	/*
	 * <offset>:     <Hex bytes: 1-8>        <Hex bytes: 9-16>         <Ascii>
	 * 00000000: 31 32 33 34 35 36 37 38  39 30 61 62 63 64 65 66  1234567890abcdef
	 *    8     2         8 * 3          1          8 * 3         1       16       1
	 */
	const size_t line_size = 8 + 2 + 8 * 3 + 1 + 8 * 3 + 1 + 16 + 1;
	size_t i, j, r, read_index;
	size_t num_lines, buffer_size;
	char *buffer, *write_head;
	unsigned char cur_char, printable;
	char ascii_line[17];
	const unsigned char *input_buffer;

	/* Allocate a dynamic buffer to hold the full result */
	num_lines = (size + 16 - 1) / 16;
	buffer_size = num_lines * line_size + 1;
	buffer = (char *)malloc(buffer_size);
	if (buffer == NULL)
		return NULL;
	write_head = buffer;
	input_buffer = data;
	read_index = 0;

	for (i = 0; i < num_lines; i++) {
		/* Offset */
		snprintf(write_head, buffer_size, "%08lX: ", i * 16);
		write_head += 8 + 2;
		buffer_size -= 8 + 2;
		/* Hex print - 2 chunks of 8 bytes */
		for (r = 0; r < 2; r++) {
			for (j = 0; j < 8; j++) {
				/* If there is content to print */
				if (read_index < size) {
					cur_char = input_buffer[read_index++];
					snprintf(write_head, buffer_size, "%02X ", cur_char);
					/* Printable chars go "as-is" */
					if (' ' <= cur_char && cur_char <= '~')
						printable = cur_char;
					/* Otherwise, use a '.' */
					else
						printable = '.';
					/* Else, just use spaces */
				} else {
					snprintf(write_head, buffer_size, "   ");
					printable = ' ';
				}
				ascii_line[r * 8 + j] = printable;
				write_head += 3;
				buffer_size -= 3;
			}
			/* Spacer between the 2 hex groups */
			snprintf(write_head, buffer_size, " ");
			write_head += 1;
			buffer_size -= 1;
		}
		/* Ascii print */
		ascii_line[16] = '\0';
		snprintf(write_head, buffer_size, "%s\n", ascii_line);
		write_head += 16 + 1;
		buffer_size -= 16 + 1;
	}
	/* No need for the last '\n' */
	write_head[-1] = '\0';
	return buffer;
}
#endif // SPDK_APP_DEBUG
