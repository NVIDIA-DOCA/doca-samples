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

#ifndef DEVEMU_PCI_HOST_COMMON_H_
#define DEVEMU_PCI_HOST_COMMON_H_

#include <stdbool.h>

#include <doca_error.h>
#include <doca_argp.h>

#include "devemu_pci_type_config.h"

struct bar_mapped_region {
	uint64_t size; /**< The size of the mapped memory */
	uint8_t *mem;  /**< Pointer to the mapped memory */
};

struct mapped_memory {
	uint64_t size; /**< The size of the mapped memory */
	uint8_t *mem;  /**< Pointer to the mapped memory */
};

struct devemu_host_resources {
	int container_fd;			  /**< The fd of the VFIO container */
	int group_fd;				  /**< The fd of the VFIO group */
	int device_fd;				  /**< The fd of the VFIO device */
	struct bar_mapped_region stateful_region; /**< mapped memory of the stateful region */
	struct bar_mapped_region db_region;	  /**< mapped memory of the DB region */
	int *msix_vector_to_fd;			  /**< Map of MSI-X index to event fd */
	struct mapped_memory dma_mem;		  /**< mapped memory of for DMA operation */
};

/*
 * Initialize VFIO device
 *
 * @resources [in]: The sample resources
 * @vfio_group [in]: The VFIO group ID
 * @pci_address [in]: The PCI address of the VFIO device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_vfio_device(struct devemu_host_resources *resources, int vfio_group, const char *pci_address);

/*
 * Map bar region to virtual memory
 *
 * @resources [in]: The sample resources
 * @bar_region_config [in]: The bar region to map
 * @mapped_mem [out]: The virtual memory of the mapped region
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t map_bar_region_memory(struct devemu_host_resources *resources,
				   const struct bar_region_config *bar_region_config,
				   struct bar_mapped_region *mapped_mem);

/*
 * Cleanup resources of the sample
 *
 * @resources [in]: The resources of the sample
 */
void devemu_host_resources_cleanup(struct devemu_host_resources *resources);

/*
 * Parse pci address of emulated device taken from command line
 *
 * @addr [in]: Input parameter received from command line
 * @parsed_addr [out]: Used to store parsed address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t parse_emulated_pci_address(const char *addr, char *parsed_addr);

/*
 * Register PCI address command line parameter
 *
 * @pci_callback [in]: Callback called for parsing the PCI address command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_emulated_pci_address_param(doca_argp_param_cb_t pci_callback);

/*
 * Register VFIO group ID command line parameter
 *
 * @vfio_group_callback [in]: Callback called for parsing the VFIO group ID command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_vfio_group_param(doca_argp_param_cb_t vfio_group_callback);

/*
 * Register region index command line parameter
 *
 * @description [in]: Description displayed in help message
 * @region_callback [in]: Callback called for parsing the region index command line param
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_region_index_param(const char *description, doca_argp_param_cb_t region_callback);

#endif // DEVEMU_PCI_HOST_COMMON_H_
