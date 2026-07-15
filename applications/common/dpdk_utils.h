/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef COMMON_DPDK_UTILS_H_
#define COMMON_DPDK_UTILS_H_

#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <rte_mbuf.h>
#include <rte_flow.h>

#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RX_RING_SIZE 1024    /* RX ring size */
#define TX_RING_SIZE 1024    /* TX ring size */
#define NUM_MBUFS (8 * 1024) /* Number of mbufs to be allocated in the mempool */
#define MBUF_CACHE_SIZE 250  /* mempool cache size */
#define MAC_ADDR_LEN 6	     /* MAC address length in bytes */
#define MAC_ADDR_STR_LEN 18  /* MAC address length in string format */
#define IPV6_ADDRESS_LEN 16  /* IPv6 address length */
#define MAC_STRING_TO_BIN_ARRAY(mac_str, mac_array) \
	do { \
		if (!sscanf(mac_str, \
			    "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", \
			    &mac_array[0], \
			    &mac_array[1], \
			    &mac_array[2], \
			    &mac_array[3], \
			    &mac_array[4], \
			    &mac_array[5])) \
			DOCA_LOG_ERR("Failed while parsing MAC address"); \
	} while (0) /* create source mac address */

#define BIN_ARRAY_TO_MAC_STRING(mac_array, mac_str) \
	do { \
		snprintf(mac_str, \
			 18, \
			 "%02x:%02x:%02x:%02x:%02x:%02x", \
			 mac_array[0], \
			 mac_array[1], \
			 mac_array[2], \
			 mac_array[3], \
			 mac_array[4], \
			 mac_array[5]); \
	} while (0)

struct doca_dev;
struct dpdk_mempool_shadow;
struct doca_buf_inventory;
struct doca_buf;

/* Port configuration */
struct application_port_config {
	int nb_ports;	      /* Set on init to 0 for don't care, required ports otherwise */
	uint16_t nb_queues;   /* Set on init to 0 for don't care, required minimum cores otherwise */
	uint16_t mbuf_size;   /* Set on init to 0 for don't care, packet mbuf size (including headroom) otherwise */
	uint64_t tx_offloads; /* Set on init to 0 for don't care, port tx offload flags otherwise */
	uint16_t enable_mbuf_metadata : 1; /* Set on init to 0 to disable, otherwise it will add meta to each mbuf */
	uint16_t rss_support : 1;	   /* Set on init to 0 for no RSS support, RSS support otherwise */
	uint16_t lpbk_support : 1;	   /* Enable loopback support */
	uint16_t switch_mode : 1;	   /* Set on init to 1 for switch mode */
};

/* DPDK configuration */
struct application_dpdk_config {
	struct application_port_config port_config; /* DPDK port configuration */
	bool reserve_main_thread;		    /* Reserve lcore for the main thread */
	struct rte_mempool *mbuf_pool;		    /* Will be filled by "dpdk_queues_and_ports_init".
						     * Memory pool that will be used by the DPDK ports
						     * for allocating rte_pktmbuf
						     */
};

/*
 * Destroy DPDK environment end close all opened devices.
 *
 * @nb_devs [in]: number of opened devices to close in this function.
 */
void dpdk_fini_with_devs(uint16_t nb_devs);

/*
 * Initialize DPDK environment
 *
 * @argc [in]: number of program command line arguments
 * @argv [in]: program command line arguments
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpdk_init(int argc, char **argv);

/*
 * Destroy DPDK environment
 */
void dpdk_fini(void);

/*
 * Initialize DPDK ports and queues
 *
 * @app_dpdk_config [in/out]: application DPDK configuration values
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpdk_queues_and_ports_init(struct application_dpdk_config *app_dpdk_config);

/*
 * Destroy DPDK ports and queues
 *
 * @app_dpdk_config [in]: application DPDK configuration values
 */
void dpdk_queues_and_ports_fini(struct application_dpdk_config *app_dpdk_config);

/*
 * Initialize a shadow of a DPDK memory pool the shadow will have all of DPDK's memory registered to a device
 *
 * @mbuf_pool [in]: the DPDK memory pool
 * @device [in]: DOCA device for memory registration
 * @return: dpdk_mempool_shadow with memory of the DPDK pool registered on success, NULL otherwise
 */
struct dpdk_mempool_shadow *dpdk_mempool_shadow_create(struct rte_mempool *mbuf_pool, struct doca_dev *device);

/*
 * Initialize a shadow of a DPDK memory pool the shadow will have all of RTE external memory
 *
 * @ext_mem [in]: Pointer to the array of structures describing the external memory for data buffers.
 * @ext_num [in]: Number of elements in the ext_mem array.
 * @device [in]: DOCA device for memory registration
 * @return: dpdk_mempool_shadow with memory of the DPDK pool registered on success, NULL otherwise
 */
struct dpdk_mempool_shadow *dpdk_mempool_shadow_create_extbuf(const struct rte_pktmbuf_extmem **ext_mem,
							      uint32_t ext_num,
							      struct doca_dev *device);

/*
 * Find the DOCA mmap instance that contains the requested address range then allocate DOCA buffer from it
 *
 * @mempool_shadow [in]: shadow of a DPDK memory pool
 * @inventory [in]: a DOCA buffer inventory used for allocating the buffer
 * @mem_range_start [in]: start address of memory range, must be within range of some 'rte_mbuf'
 * @mem_range_size [in]: the size of the memory range in bytes, must be within range of some 'rte_mbuf'
 * @out_buf [out]: DOCA buffer allocated with data pointing to the given range
 * @return: DOCA_SUCCESS on success, and doca_error_t otherwise
 */
doca_error_t dpdk_mempool_shadow_find_buf_by_data(struct dpdk_mempool_shadow *mempool_shadow,
						  struct doca_buf_inventory *inventory,
						  uintptr_t mem_range_start,
						  size_t mem_range_size,
						  struct doca_buf **out_buf);

/*
 * Destroy the DPDK memory pool shadow
 *
 * @mempool_shadow [in]: shadow of a DPDK memory pool to be destroyed
 * @Note: this should be done before destroying the DPDK memory pool
 */
void dpdk_mempool_shadow_destroy(struct dpdk_mempool_shadow *mempool_shadow);

/*
 * Print packet header information
 *
 * @packet [in]: packet mbuf
 * @l2 [in]: if true the function prints l2 header
 * @l3 [in]: if true the function prints l3 header
 * @l4 [in]: if true the function prints l4 header
 */
void print_header_info(const struct rte_mbuf *packet, const bool l2, const bool l3, const bool l4);

/*
 * Detect VF MAC address
 *
 * @vf_if_name [in]: VF interface name
 * @vf_mac_address [out]: VF mac address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t detect_vf_mac_address(char *vf_if_name, uint8_t *vf_mac_address);

/*
 * Detect VF IP addresses
 *
 * @vf_if_name [in]: VF interface name
 * @vf_ip_local_address [out]: VF IP local address
 * @vf_ip_global_address [out]: VF IP global address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t detect_vf_ip_address(char *vf_if_name, uint8_t *vf_ip_local_address, uint8_t *vf_ip_global_address);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMMON_DPDK_UTILS_H_ */
