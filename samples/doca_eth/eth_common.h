/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef ETH_COMMON_H_
#define ETH_COMMON_H_

#include <unistd.h>

#include <doca_pe.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_buf_inventory.h>
#include <doca_error.h>

#include "common.h"

#define MAX_QUEUES_NUM 128 /* Max number of ETH RXQs the sample can share the memory pool between */

struct eth_core_resources {
	struct program_core_objects core_objs; /* DOCA core objects */
	void *mem_addr;			       /* Address of allocated memory for mmap (might not be aligned) */
	void *mmap_addr;		       /* Address of DOCA mmap start memory address (aligned address) */
	uint32_t mmap_size;		       /* DOCA memory map size */
};

struct eth_core_config {
	uint32_t mmap_size;	       /* Size of the memory map */
	size_t inventory_num_elements; /* Elements number of the buffer inventory */
	tasks_check check_device;      /* Function to check device capability */
	const char *ibdev_name;	       /* DOCA IB device name */
};

struct ether_hdr {
	uint8_t dst_addr[DOCA_DEVINFO_MAC_ADDR_SIZE]; /* Destination addr bytes in tx order */
	uint8_t src_addr[DOCA_DEVINFO_MAC_ADDR_SIZE]; /* Source addr bytes in tx order */
	uint16_t ether_type;			      /* Frame type */
} __attribute__((__packed__));

/*
 * Allocate ETH core resources
 *
 * @cfg [in]: Configuration parameters
 * @resources [out]: ETH core resources to allocate
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t allocate_eth_core_resources(struct eth_core_config *cfg, struct eth_core_resources *resources);

/*
 * Destroy ETH core resources
 *
 * @resources [in]: ETH core resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_eth_core_resources(struct eth_core_resources *resources);

/*
 * Extract IB device name after checking it's a valid IB device name
 *
 * @ibdev_name [in]: IB device name to check and extract from
 * @ibdev_name_out [out]: buffer to extract/copy IB device name to
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t extract_ibdev_name(char *ibdev_name, char *ibdev_name_out);

/*
 * Extract MAC address after checking it's a valid MAC address
 *
 * @mac_addr [in]: MAC address to check and extract from
 * @mac_addr_out [out]: uint8_t array to extract MAC address to
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t extract_mac_addr(char *mac_addr, uint8_t *mac_addr_out);

#endif /* ETH_COMMON_H_ */
