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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <doca_log.h>

#include "eth_common.h"

DOCA_LOG_REGISTER(ETH::COMMON);

#define CACHE_LINE_SIZE 64 /* Cache-line size in bytes */

doca_error_t allocate_eth_core_resources(struct eth_core_config *cfg, struct eth_core_resources *resources)
{
	doca_error_t status;

	if (cfg == NULL || resources == NULL) {
		DOCA_LOG_ERR("Failed to allocate eth_core_resources: invalid parameters");
		return DOCA_ERROR_INVALID_VALUE;
	}

	memset(resources, 0, sizeof(*resources));

	status = open_doca_device_with_ibdev_name((const uint8_t *)(cfg->ibdev_name),
						  DOCA_DEVINFO_IBDEV_NAME_SIZE,
						  cfg->check_device,
						  &(resources->core_objs.dev));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate eth_core_resources: failed to open a device, err: %s",
			     doca_error_get_name(status));
		return status;
	}

	status = create_core_objects(&(resources->core_objs), cfg->inventory_num_elements);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate eth_core_resources: failed to create core objects, err: %s",
			     doca_error_get_name(status));
		(void)destroy_eth_core_resources(resources);
		return status;
	}

	resources->mem_addr = malloc(cfg->mmap_size + CACHE_LINE_SIZE);
	if (resources->mem_addr == NULL) {
		DOCA_LOG_ERR("Failed to allocate eth_core_resources: failed to allocate mmap memory");
		status = DOCA_ERROR_NO_MEMORY;
		(void)destroy_eth_core_resources(resources);
		return status;
	}

	resources->mmap_addr = (void *)align_up_uint64((uint64_t)resources->mem_addr, CACHE_LINE_SIZE);

	resources->mmap_size = cfg->mmap_size;

	status = doca_mmap_set_memrange(resources->core_objs.src_mmap, resources->mmap_addr, resources->mmap_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate eth_core_resources: failed to set mmap range, err: %s",
			     doca_error_get_name(status));
		(void)destroy_eth_core_resources(resources);
		return status;
	}

	status = doca_mmap_start(resources->core_objs.src_mmap);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate eth_core_resources: failed to start mmap, err: %s",
			     doca_error_get_name(status));
		(void)destroy_eth_core_resources(resources);
		return status;
	}

	return DOCA_SUCCESS;
}

doca_error_t destroy_eth_core_resources(struct eth_core_resources *resources)
{
	doca_error_t status;

	if (resources->mem_addr != NULL) {
		free(resources->mem_addr);
		resources->mem_addr = NULL;
	}

	if (resources->core_objs.pe != NULL) {
		status = destroy_core_objects(&(resources->core_objs));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy core objects, err: %s", doca_error_get_name(status));
			return status;
		}
		resources->core_objs.pe = NULL;
	}

	if (resources->core_objs.dev != NULL) {
		status = doca_dev_close(resources->core_objs.dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device, err: %s", doca_error_get_name(status));
			return status;
		}
		resources->core_objs.dev = NULL;
	}

	return DOCA_SUCCESS;
}

doca_error_t extract_ibdev_name(char *ibdev_name, char *ibdev_name_out)
{
	int len;

	if (ibdev_name == NULL || ibdev_name_out == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	len = strnlen(ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("IB device name exceeding the maximum size of %d", DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(ibdev_name_out, ibdev_name, len + 1);

	return DOCA_SUCCESS;
}

doca_error_t extract_mac_addr(char *mac_addr, uint8_t *mac_addr_out)
{
	int len, valid_size;

	if (mac_addr == NULL || mac_addr_out == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	valid_size = strlen("FF:FF:FF:FF:FF:FF");
	len = strnlen(mac_addr, valid_size + 1);
	if (len != valid_size) {
		DOCA_LOG_ERR("Invalid MAC address, it should be in the following format FF:FF:FF:FF:FF:FF");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (sscanf(mac_addr,
		   "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   &(mac_addr_out[0]),
		   &(mac_addr_out[1]),
		   &(mac_addr_out[2]),
		   &(mac_addr_out[3]),
		   &(mac_addr_out[4]),
		   &(mac_addr_out[5])) != DOCA_DEVINFO_MAC_ADDR_SIZE) {
		DOCA_LOG_ERR("Invalid MAC address, it should be in the following format FF:FF:FF:FF:FF:FF");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}
