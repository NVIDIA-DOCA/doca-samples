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

#ifndef NVME_PCI_COMMON_H_
#define NVME_PCI_COMMON_H_

#include <stdbool.h>

#include <doca_error.h>
#include <doca_argp.h>
#include <doca_log.h>
#include <doca_devemu_pci_type.h>
#include <doca_devemu_pci.h>
#include <doca_dpa.h>

#include "nvme_pci_type_config.h"

#define SPDK_APP_DEBUG

/* Function to check if a given device supports PCI emulation */
typedef doca_error_t (*emulation_supported_cb_t)(const struct doca_devinfo *,
						 const struct doca_devemu_pci_type *pci_type,
						 uint8_t *is_supported);

/*
 * Open a DOCA device according to a given device name
 * Picks a device that has given name and supports hotplug of the PCI type
 *
 * @dev_name [in]: device name
 * @pci_type [in]: The emulated PCI type
 * @has_support [in]: Method to check if device hsupports emulation
 * @dev [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t find_supported_device(const char *dev_name,
				   const struct doca_devemu_pci_type *pci_type,
				   emulation_supported_cb_t has_support,
				   struct doca_dev **dev);
/*
 * Open an emulated PCI device representor according to type and given VUID
 *
 * @pci_type [in]: The emulated PCI type
 * @vuid [in]: The VUID of the emulated device
 * @rep [out]: pointer to doca_dev_rep struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t find_emulated_device(struct doca_devemu_pci_type *pci_type, const char *vuid, struct doca_dev_rep **rep);

/*
 * Sets the PCI configurations of the type and then starts it
 * Once device is hotplugged the configurations will be visible to the Host as part of the
 * PCI configuration space of that device
 *
 * @pci_type [in]: The emulated PCI type
 * @dev [in]: The device that manages the PCI type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t configure_and_start_pci_type(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev);

/*
 * Creates a pci type, selects a device with the specified PCI device name and supports hotplug for the PCI type,
 * configures the PCI settings for the type, and then starts it.
 *
 * @dev_name [in]: The emulated device name
 * @pci_type [out]: The PCI type*
 * @dev [out]: The device that manages the PCI type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_find_start_pci_type(char *dev_name, struct doca_devemu_pci_type **pci_type, struct doca_dev **dev);

/*
 * Frees the given resources.
 *
 * @pci_type [in]: The PCI type*
 * @dev [in]: The device that manages the PCI type
 */
void cleanup_pci_resources(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev);

/*
 * Convert enum doca_devemu_pci_hotplug_state to string
 *
 * @hotplug_state [in]: The hotplug state to convert
 * @return: String representation of the hotplug state
 */
const char *hotplug_state_to_string(enum doca_devemu_pci_hotplug_state hotplug_state);

#ifdef SPDK_APP_DEBUG
/*
 * Create a string Hex dump representation of the given input buffer
 *
 * @data [in]: Pointer to the input buffer
 * @size [in]: Number of bytes to be analyzed
 * @return: pointer to the string representation, or NULL if an error was encountered
 */
char *hex_dump(const void *data, size_t size);
#endif // SPDK_APP_DEBUG

#endif // DEVEMU_PCI_COMMON_H_
