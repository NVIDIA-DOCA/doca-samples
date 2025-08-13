/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <time.h>

#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_type.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include <doca_devemu_vfs.h>
#include <doca_devemu_vfs_type.h>
#include <doca_devemu_vfs_dev.h>

DOCA_LOG_REGISTER(DPU_DEVEMU_VFS_DEVICE_LIST);

struct program_state {
	struct doca_dev *dev;		       /**< Device that manage the emulated device */
	struct doca_devemu_vfs_type *vfs_type; /**< VFS Type of the emulated device */
};

/*
 * Cleanup state of the sample
 *
 * @state [in]: The state of the sample
 */
static void state_cleanup(struct program_state *state)
{
	doca_error_t res;

	if (state->dev != NULL) {
		res = doca_dev_close(state->dev);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(res));

		state->dev = NULL;
	}
}

/*
 * Check if the device's PCI address matches the provided PCI address
 * Supports both formats DOMAIN:BUS:DEVICE.FUNCTION or BUS:DEVICE.FUNCTION
 *
 * @devinfo [in]: The DOCA device information
 * @pci_address [in]: The PCI address to compare against
 * @return: true in case matches, false otherwise
 */
static bool device_match_pci_addr(const struct doca_devinfo *devinfo, const char *pci_address)
{
	uint8_t is_equal = 0;

	(void)doca_devinfo_is_equal_pci_addr(devinfo, pci_address, &is_equal);

	return is_equal == 1;
}

/*
 * Open a DOCA device according to a given PCI address
 * Picks device that has given PCI address and supports hotplug of the PCI type
 *
 * @pci_address [in]: PCI address
 * @dev [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t find_doca_device(const char *pci_address, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*dev = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		if (!device_match_pci_addr(dev_list[i], pci_address))
			continue;

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

/*
 * Convert PCI function type to string
 *
 * @pci_func_type [in]: PCI Function Type
 * @return: String for function type
 */
static const char *rep_func_type_to_string(enum doca_pci_func_type pci_func_type)
{
	switch (pci_func_type) {
	case DOCA_PCI_FUNC_TYPE_PF:
		return "PF";
	case DOCA_PCI_FUNC_TYPE_VF:
		return "VF";
	case DOCA_PCI_FUNC_TYPE_SF:
		return "SF";
	default:
		break;
	}

	return "Invalid pci function type";
}

/*
 * List the static and hotplug VirtioFS devices in system
 *
 * @vfs_type [in]: VFS Type to get the devices
 * @return: DOCA_SUCCESS on success and doca_error_t otherwise
 */
static doca_error_t list_emulated_devices(struct doca_devemu_vfs_type *vfs_type)
{
	doca_error_t result;
	uint32_t num_devices = 0;
	struct doca_devinfo_rep **dev_list_rep = NULL;
	char buf[DOCA_DEVINFO_REP_VUID_SIZE] = {};
	char pci_buf[DOCA_DEVINFO_REP_PCI_ADDR_SIZE] = {};
	enum doca_pci_func_type pci_func_type;
	uint32_t i;

	result = doca_devemu_pci_type_create_rep_list(doca_devemu_vfs_type_as_pci_type(vfs_type),
						      &dev_list_rep,
						      &num_devices);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get list: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Total Emulated Devices : %d\n", num_devices);
	for (i = 0; i < num_devices; i++) {
		result = doca_devinfo_rep_get_vuid(dev_list_rep[i], buf, DOCA_DEVINFO_REP_VUID_SIZE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get vuid: %s", doca_error_get_descr(result));
			return result;
		}
		result = doca_devinfo_rep_get_pci_addr_str(dev_list_rep[i], pci_buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get pci addr: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_devinfo_rep_get_pci_func_type(dev_list_rep[i], &pci_func_type);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get pci func type: %s", doca_error_get_descr(result));
			return result;
		}

		DOCA_LOG_INFO("Idx:%d, vuid:%s pci_addr:%s pci_func_type:%s",
			      i,
			      buf,
			      pci_buf,
			      rep_func_type_to_string(pci_func_type));
	}

	doca_devinfo_rep_destroy_list(dev_list_rep);

	return DOCA_SUCCESS;
}

/*
 * Sample program to list VirtioFS Static and Hotplug devices
 *
 * @pci_address [in]: Device PCI address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_vfs_device_list(const char *pci_address)
{
	doca_error_t result;
	struct program_state state = {0};

	result = find_doca_device(pci_address, &state.dev);
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	result = doca_devemu_vfs_find_default_vfs_type_by_dev(state.dev, &state.vfs_type);
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	DOCA_LOG_INFO("List Static VirtioFS Devices\n");

	result = list_emulated_devices(state.vfs_type);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to get list of emulated devices: %s", doca_error_get_descr(result));

	state_cleanup(&state);

	return result;
}
