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

#include <inttypes.h>

#include <doca_apsh.h>
#include <doca_log.h>

#include "apsh_common.h"

DOCA_LOG_REGISTER(INTERFACES_GET);
#define BUFFER_SIZE 2048

/*
 * Calls the DOCA APSH API function that matches this sample name and prints the result
 *
 * @dma_device_name [in]: IBDEV Name of the device to use for DMA
 * @pci_vuid [in]: VUID of the device exposed to the target system
 * @os_type [in]: Indicates the OS type of the target system
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t interfaces_get(const char *dma_device_name,
			    const char *pci_vuid,
			    enum doca_apsh_system_os os_type,
			    const char *mem_region,
			    const char *os_symbols)
{
	doca_error_t result;
	int num_interfaces, i;
	struct doca_apsh_ctx *apsh_ctx;
	struct doca_apsh_system *sys;
	struct doca_apsh_interface **interfaces;

	/* Init */
	result = init_doca_apsh(dma_device_name, &apsh_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init the DOCA APSH lib");
		return result;
	}
	DOCA_LOG_INFO("DOCA APSH lib context init successful");

	result = init_doca_apsh_system(apsh_ctx, os_type, os_symbols, mem_region, pci_vuid, &sys);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init the system context");
		return result;
	}
	DOCA_LOG_INFO("DOCA APSH system context created");

	result = doca_apsh_interfaces_get(sys, &interfaces, &num_interfaces);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the interfaces list");
		cleanup_doca_apsh(apsh_ctx, sys);
		return result;
	}
	DOCA_LOG_INFO("Successfully performed %s. Host system contains %d interfaces", __func__, num_interfaces);
	DOCA_LOG_INFO("Interfaces of system:");
	for (i = 0; i < num_interfaces; i++) {
		char buffer[BUFFER_SIZE];
		int offset = 0;
		offset += snprintf(buffer + offset,
				   BUFFER_SIZE - offset,
				   "Interface number %d\nname: %s\n",
				   i,
				   doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_NAME));
		uint32_t ipv4_arr_size =
			doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_IPV4_ARR_SIZE);
		if (ipv4_arr_size > 0) {
			offset += snprintf(buffer + offset, BUFFER_SIZE - offset, "IPV4:\n");
			char **ipv4_addr_arr =
				doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_IPV4_ARR);
			unsigned char *ipv4_prefixlen_arr =
				doca_apsh_interface_info_get(interfaces[i],
							     DOCA_APSH_LINUX_INTERFACE_IPV4_PREFIX_LEN_ARR);
			// prints only first 5 IPV4 addresses
			for (uint32_t j = 0; j < ipv4_arr_size && j < 5; j++) {
				offset += snprintf(buffer + offset,
						   BUFFER_SIZE - offset,
						   "%s,  prefixlen: %u\n",
						   ipv4_addr_arr[j],
						   ipv4_prefixlen_arr[j]);
			}
		}
		uint32_t ipv6_arr_size =
			doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_IPV6_ARR_SIZE);
		if (ipv6_arr_size > 0) {
			offset += snprintf(buffer + offset, BUFFER_SIZE - offset, "IPV6:\n");
			char **ipv6_addr_arr =
				doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_IPV6_ARR);
			uint32_t *ipv6_prefixlen_arr =
				doca_apsh_interface_info_get(interfaces[i],
							     DOCA_APSH_LINUX_INTERFACE_IPV6_PREFIX_LEN_ARR);
			// prints only first 5 IPV6 addresses
			for (uint32_t j = 0; j < ipv6_arr_size && j < 5; j++) {
				offset += snprintf(buffer + offset,
						   BUFFER_SIZE - offset,
						   "%s,  prefixlen: %u\n",
						   ipv6_addr_arr[j],
						   ipv6_prefixlen_arr[j]);
			}
		}
		uint32_t mac_arr_size =
			doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_MAC_ARR_SIZE);
		if (mac_arr_size > 0) {
			offset += snprintf(buffer + offset, BUFFER_SIZE - offset, "MAC address:\n");
			char **mac_addr_arr =
				doca_apsh_interface_info_get(interfaces[i], DOCA_APSH_LINUX_INTERFACE_MAC_ARR);
			// prints only first 5 MAC addresses
			for (uint32_t j = 0; j < mac_arr_size && j < 5; j++) {
				offset += snprintf(buffer + offset, BUFFER_SIZE - offset, "%s\n", mac_addr_arr[j]);
			}
		}
		DOCA_LOG_INFO("%s", buffer);
	}
	doca_apsh_interfaces_free(interfaces);
	cleanup_doca_apsh(apsh_ctx, sys);
	return DOCA_SUCCESS;
}