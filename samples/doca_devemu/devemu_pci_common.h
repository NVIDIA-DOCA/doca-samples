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

#ifndef DEVEMU_PCI_COMMON_H_
#define DEVEMU_PCI_COMMON_H_

#include <stdbool.h>

#include <doca_error.h>
#include <doca_argp.h>
#include <doca_log.h>
#include <doca_devemu_pci_type.h>
#include <doca_devemu_pci.h>
#include <doca_dpa.h>

#include "devemu_pci_type_config.h"

#define SLEEP_IN_MICROS (10) /* Sample the task every 10 microseconds */
#define SLEEP_IN_NANOS (SLEEP_IN_MICROS * 1000)

/* Function to check if a given device supports PCI emulation */
typedef doca_error_t (*emulation_supported_cb_t)(const struct doca_devinfo *,
						 const struct doca_devemu_pci_type *pci_type,
						 uint8_t *is_supported);

struct devemu_resources {
	struct doca_pe *pe;					/**< Progress engine to retrieve events */
	struct doca_dev *dev;					/**< Device that manage the emulated device */
	struct doca_devemu_pci_type *pci_type;			/**< The PCI type of the emulated device */
	struct doca_dev_rep *rep;				/**< Representor of the emulated device */
	struct doca_devemu_pci_dev *pci_dev;			/**< The emulated device management context */
	struct doca_ctx *ctx;					/**< DOCA context representation of pci_dev */
	enum doca_devemu_pci_hotplug_state hotplug_state;	/**< The hotplug state of emulated device */
	void *stateful_region_values;				/**< Buffer used to query stateful region values */
	struct doca_dpa *dpa;					/**< The DPA context */
	struct doca_dpa_thread *dpa_thread;			/**< DPA thread for receiving completions */
	struct doca_devemu_pci_db_completion *db_comp;		/**< The DPA DB completion context */
	doca_dpa_dev_devemu_pci_db_completion_t db_comp_handle; /**< The DPA DB completion context handle */
	uint16_t db_region_idx;					/**< The DB region index */
	uint32_t db_id;						/**< The DB ID */
	struct {
		struct doca_devemu_pci_msix *msix;	    /**< The device emulation MSI-X object */
		doca_dpa_dev_devemu_pci_msix_t msix_handle; /**< The DPA handle of the MSI-X object */
		struct doca_devemu_pci_db *db;		    /**< The device emulation DB object */
		doca_dpa_dev_devemu_pci_db_t db_handle;	    /**< The DPA handle of the DB object */
	} data_path;
	doca_error_t error; /**< Indicates an error that occurred during progress */
};

/*
 * Parse pci address taken from command line
 *
 * @addr [in]: Input parameter received from command line
 * @parsed_addr [out]: Used to store parsed address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t parse_pci_address(const char *addr, char *parsed_addr);

/*
 * Register PCI address command line parameter
 *
 * @pci_callback [in]: Callback called for parsing the PCI address command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_pci_address_param(doca_argp_param_cb_t pci_callback);

/*
 * Parse VUID taken from command line
 *
 * @vuid [in]: Input parameter received from command line
 * @parsed_vuid [out]: Used to store parsed VUID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t parse_vuid(const char *vuid, char *parsed_vuid);

/*
 * Register VUID command line parameter
 *
 * @description [in]: Description displayed in help message
 * @vuid_callback [in]: Callback called for parsing the VUID command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_vuid_param(const char *description, doca_argp_param_cb_t vuid_callback);

/*
 * Open a DOCA device according to a given PCI address
 * Picks device that has given PCI address and supports hotplug of the PCI type
 *
 * @pci_address [in]: PCI address
 * @pci_type [in]: The emulated PCI type
 * @has_support [in]: Method to check if device hsupports emulation
 * @dev [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t find_supported_device(const char *pci_address,
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
 * Cleanup resources of the sample
 *
 * @resources [in]: The resources of the sample
 * @destroy_rep [in]: Whether to destroy the representor or only close it
 */
void devemu_resources_cleanup(struct devemu_resources *resources, bool destroy_rep);

/*
 * Convert enum doca_devemu_pci_hotplug_state to string
 *
 * @hotplug_state [in]: The hotplug state to convert
 * @return: String representation of the hotplug state
 */
const char *hotplug_state_to_string(enum doca_devemu_pci_hotplug_state hotplug_state);

/*
 * Initialize a DPA context for given DPA application
 *
 * @resources [in]: The resources of the sample
 * @dpa_app [in]: The DPA application
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_dpa(struct devemu_resources *resources, struct doca_dpa_app *dpa_app);

#endif // DEVEMU_PCI_COMMON_H_
