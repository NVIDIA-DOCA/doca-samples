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

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_mmap.h>
#include <doca_buf_inventory.h>

#include "dpdk_utils.h"
#include "utils.h"

DOCA_LOG_REGISTER(NUTILS);

#define RSS_KEY_LEN 40

#ifndef IPv6_BYTES
#define IPv6_BYTES_FMT \
	"%02x%02x:%02x%02x:%02x%02x:%02x%02x:" \
	"%02x%02x:%02x%02x:%02x%02x:%02x%02x"
#define IPv6_BYTES(addr) \
	addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7], addr[8], addr[9], addr[10], addr[11], \
		addr[12], addr[13], addr[14], addr[15]
#endif

#define AUX_DEV_IDENTIFIER_PREFIX "auxiliary:mlx5_core.sf."
#define AUX_DEV_IDENTIFIER_MAX_SIZE (32)
#define DEV_IDENTIFIER_MAX_SIZE (MAX(AUX_DEV_IDENTIFIER_MAX_SIZE, DOCA_DEVINFO_PCI_ADDR_SIZE))

struct dpdk_mempool_shadow {
	struct doca_dev *device;     /* DOCA device used to register memory */
	struct doca_mmap **mmap_arr; /* DOCA mmap array that has mapped the packet buffers */
	uint32_t nb_mmaps;	     /* Number of elements in mmap_arr */
};

/*
 * Creates a new mempool in memory to hold the mbufs
 *
 * @total_nb_mbufs [in]: the number of elements in the mbuf pool
 * @mbuf_size [in]: the size of each mbuf, including headroom
 * @mbuf_pool [out]: the allocated pool
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t allocate_mempool(const uint32_t total_nb_mbufs,
				     const uint32_t mbuf_size,
				     struct rte_mempool **mbuf_pool)
{
	*mbuf_pool =
		rte_pktmbuf_pool_create("MBUF_POOL", total_nb_mbufs, MBUF_CACHE_SIZE, 0, mbuf_size, rte_socket_id());
	if (*mbuf_pool == NULL) {
		DOCA_LOG_ERR("Cannot allocate mbuf pool");
		return DOCA_ERROR_DRIVER;
	}
	return DOCA_SUCCESS;
}

/*
 * Initialize all the port resources
 *
 * @mbuf_pool [in]: packet mbuf pool
 * @port [in]: the port ID
 * @app_config [in]: application DPDK configuration values
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t port_init(struct rte_mempool *mbuf_pool, uint8_t port, struct application_dpdk_config *app_config)
{
	int ret = 0;
	int symmetric_hash_key_length = RSS_KEY_LEN;
	const uint16_t rx_rings = app_config->port_config.nb_queues;
	const uint16_t tx_rings = app_config->port_config.nb_queues;
	const uint16_t rss_support = !!(app_config->port_config.rss_support && (app_config->port_config.nb_queues > 1));
	uint16_t q;
	struct rte_ether_addr addr;
	struct rte_eth_dev_info dev_info;
	uint8_t symmetric_hash_key[RSS_KEY_LEN] = {
		0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	};
	const struct rte_eth_conf port_conf_default = {
		.lpbk_mode = app_config->port_config.lpbk_support,
		.rx_adv_conf =
			{
				.rss_conf =
					{
						.rss_key_len = symmetric_hash_key_length,
						.rss_key = symmetric_hash_key,
						.rss_hf = (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP),
					},
			},
	};
	struct rte_eth_conf port_conf = port_conf_default;

	ret = rte_eth_dev_info_get(port, &dev_info);
	if (ret < 0) {
		DOCA_LOG_ERR("Failed getting device (port %u) info, error=%s", port, strerror(-ret));
		return DOCA_ERROR_DRIVER;
	}
	if (*dev_info.dev_flags & RTE_ETH_DEV_REPRESENTOR && app_config->port_config.switch_mode) {
		DOCA_LOG_INFO("Skip represent port %d init in switch mode", port);
		return DOCA_SUCCESS;
	}

	port_conf.rxmode.mq_mode = rss_support ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE;
	port_conf.txmode.offloads = app_config->port_config.tx_offloads;

	/* Configure the Ethernet device */
	ret = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (ret < 0) {
		DOCA_LOG_ERR("Failed to configure the ethernet device - (%d)", ret);
		return DOCA_ERROR_DRIVER;
	}
	if (port_conf_default.rx_adv_conf.rss_conf.rss_hf != port_conf.rx_adv_conf.rss_conf.rss_hf) {
		DOCA_LOG_DBG("Port %u modified RSS hash function based on hardware support, requested:%#" PRIx64
			     " configured:%#" PRIx64 "",
			     port,
			     port_conf_default.rx_adv_conf.rss_conf.rss_hf,
			     port_conf.rx_adv_conf.rss_conf.rss_hf);
	}

	/* Enable RX in promiscuous mode for the Ethernet device */
	ret = rte_eth_promiscuous_enable(port);
	if (ret < 0) {
		DOCA_LOG_ERR("Failed to Enable RX in promiscuous mode - (%d)", ret);
		return DOCA_ERROR_DRIVER;
	}

	/* Allocate and set up RX queues according to number of cores per Ethernet port */
	for (q = 0; q < rx_rings; q++) {
		ret = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (ret < 0) {
			DOCA_LOG_ERR("Failed to set up RX queues - (%d)", ret);
			return DOCA_ERROR_DRIVER;
		}
	}

	/* Allocate and set up TX queues according to number of cores per Ethernet port */
	for (q = 0; q < tx_rings; q++) {
		ret = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE, rte_eth_dev_socket_id(port), NULL);
		if (ret < 0) {
			DOCA_LOG_ERR("Failed to set up TX queues - (%d)", ret);
			return DOCA_ERROR_DRIVER;
		}
	}

	/* Start the Ethernet port */
	ret = rte_eth_dev_start(port);
	if (ret < 0) {
		DOCA_LOG_ERR("Cannot start port %" PRIu8 ", ret=%d", port, ret);
		return DOCA_ERROR_DRIVER;
	}

	/* Display the port MAC address */
	rte_eth_macaddr_get(port, &addr);
	DOCA_LOG_DBG("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "",
		     (unsigned int)port,
		     addr.addr_bytes[0],
		     addr.addr_bytes[1],
		     addr.addr_bytes[2],
		     addr.addr_bytes[3],
		     addr.addr_bytes[4],
		     addr.addr_bytes[5]);

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	if (rte_eth_dev_socket_id(port) > 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id()) {
		DOCA_LOG_WARN("Port %u is on remote NUMA node to polling thread", port);
		DOCA_LOG_WARN("\tPerformance will not be optimal");
	}
	return DOCA_SUCCESS;
}

/*
 * Destroy all DPDK ports
 *
 * @app_dpdk_config [in]: application DPDK configuration values
 * @nb_ports [in]: number of ports to destroy
 */
static void dpdk_ports_fini(struct application_dpdk_config *app_dpdk_config, uint16_t nb_ports)
{
	int result;
	int port_id;

	for (port_id = nb_ports; port_id >= 0; port_id--) {
		if (!rte_eth_dev_is_valid_port(port_id))
			continue;
		result = rte_eth_dev_stop(port_id);
		if (result != 0)
			DOCA_LOG_ERR("rte_eth_dev_stop(): err=%d, port=%u", result, port_id);

		result = rte_eth_dev_close(port_id);
		if (result != 0)
			DOCA_LOG_ERR("rte_eth_dev_close(): err=%d, port=%u", result, port_id);
	}

	/* Free the memory pool used by the ports for rte_pktmbufs */
	if (app_dpdk_config->mbuf_pool != NULL)
		rte_mempool_free(app_dpdk_config->mbuf_pool);
}

/*
 * Initialize all DPDK ports
 *
 * @app_config [in]: application DPDK configuration values
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpdk_ports_init(struct application_dpdk_config *app_config)
{
	doca_error_t result;
	int ret;
	uint16_t port_id;
	uint16_t n;
	const uint16_t nb_ports = app_config->port_config.nb_ports;
	const uint32_t total_nb_mbufs = app_config->port_config.nb_queues * nb_ports * NUM_MBUFS;
	const uint32_t mbuf_size = app_config->port_config.mbuf_size ? app_config->port_config.mbuf_size :
								       RTE_MBUF_DEFAULT_BUF_SIZE;

	/* Initialize mbufs mempool */
	result = allocate_mempool(total_nb_mbufs, mbuf_size, &app_config->mbuf_pool);
	if (result != DOCA_SUCCESS)
		return result;

	/*
	 * Enable metadata to be delivered to application in the packets mbuf, the metadata is user configurable,
	 * with DOCA Flow offering a metadata scheme
	 */
	if (app_config->port_config.enable_mbuf_metadata) {
		ret = rte_flow_dynf_metadata_register();
		if (ret < 0) {
			DOCA_LOG_ERR("Metadata register failed, ret=%d", ret);
			return DOCA_ERROR_DRIVER;
		}
	}

	for (port_id = 0, n = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
		if (!rte_eth_dev_is_valid_port(port_id))
			continue;
		result = port_init(app_config->mbuf_pool, port_id, app_config);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Cannot init port %" PRIu8, port_id);
			dpdk_ports_fini(app_config, port_id);
			return result;
		}
		if (++n >= nb_ports)
			break;
	}
	return DOCA_SUCCESS;
}

doca_error_t dpdk_queues_and_ports_init(struct application_dpdk_config *app_dpdk_config)
{
	doca_error_t result;
	int ret = 0;

	/* Check that DPDK enabled the required ports to send/receive on */
	ret = rte_eth_dev_count_avail();
	if (app_dpdk_config->port_config.nb_ports > 0 && ret < app_dpdk_config->port_config.nb_ports) {
		DOCA_LOG_ERR("Application will only function with %u ports, num_of_ports=%d",
			     app_dpdk_config->port_config.nb_ports,
			     ret);
		return DOCA_ERROR_DRIVER;
	}

	/* Check for available logical cores */
	ret = rte_lcore_count();
	if (app_dpdk_config->port_config.nb_queues > 0 && ret < app_dpdk_config->port_config.nb_queues) {
		DOCA_LOG_ERR("At least %u cores are needed for the application to run, available_cores=%d",
			     app_dpdk_config->port_config.nb_queues,
			     ret);
		return DOCA_ERROR_DRIVER;
	}
	app_dpdk_config->port_config.nb_queues = ret;

	if (app_dpdk_config->reserve_main_thread)
		app_dpdk_config->port_config.nb_queues -= 1;

	if (app_dpdk_config->port_config.nb_ports > 0) {
		result = dpdk_ports_init(app_dpdk_config);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Ports allocation failed");
			return result;
		}
	}

	return DOCA_SUCCESS;
}

void dpdk_queues_and_ports_fini(struct application_dpdk_config *app_dpdk_config)
{
	dpdk_ports_fini(app_dpdk_config, RTE_MAX_ETHPORTS);
}

/*
 * Print ether address
 *
 * @dmac [in]: destination mac address
 * @smac [in]: source mac address
 * @ethertype [in]: eth type
 */
static void print_ether_addr(const struct rte_ether_addr *dmac,
			     const struct rte_ether_addr *smac,
			     const uint32_t ethertype)
{
	char dmac_buf[RTE_ETHER_ADDR_FMT_SIZE];
	char smac_buf[RTE_ETHER_ADDR_FMT_SIZE];

	rte_ether_format_addr(dmac_buf, RTE_ETHER_ADDR_FMT_SIZE, dmac);
	rte_ether_format_addr(smac_buf, RTE_ETHER_ADDR_FMT_SIZE, smac);
	DOCA_LOG_DBG("DMAC=%s, SMAC=%s, ether_type=0x%04x", dmac_buf, smac_buf, ethertype);
}

/*
 * Print L2 header
 *
 * @packet [in]: packet mbuf
 */
static void print_l2_header(const struct rte_mbuf *packet)
{
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);

	print_ether_addr(&eth_hdr->dst_addr, &eth_hdr->src_addr, htonl(eth_hdr->ether_type) >> 16);
}

/*
 * Print IPV4 address
 *
 * @dip [in]: destination IP address
 * @sip [in]: source IP address
 * @packet_type [in]: packet type
 */
static void print_ipv4_addr(const rte_be32_t dip, const rte_be32_t sip, const char *packet_type)
{
	DOCA_LOG_DBG("DIP=%d.%d.%d.%d, SIP=%d.%d.%d.%d, %s",
		     (dip & 0xff000000) >> 24,
		     (dip & 0x00ff0000) >> 16,
		     (dip & 0x0000ff00) >> 8,
		     (dip & 0x000000ff),
		     (sip & 0xff000000) >> 24,
		     (sip & 0x00ff0000) >> 16,
		     (sip & 0x0000ff00) >> 8,
		     (sip & 0x000000ff),
		     packet_type);
}

/*
 * Print IPV6 address
 *
 * @dst_addr [in]: destination IP address
 * @src_addr [in]: source IP address
 * @packet_type [in]: packet type
 */
static void print_ipv6_addr(const uint8_t dst_addr[16], const uint8_t src_addr[16], const char *packet_type)
{
	DOCA_LOG_DBG("DIPv6=" IPv6_BYTES_FMT ", SIPv6=" IPv6_BYTES_FMT ", %s",
		     IPv6_BYTES(dst_addr),
		     IPv6_BYTES(src_addr),
		     packet_type);
}

/*
 * Print L3 header
 *
 * @packet [in]: packet mbuf
 */
static void print_l3_header(const struct rte_mbuf *packet)
{
	if (RTE_ETH_IS_IPV4_HDR(packet->packet_type)) {
		struct rte_ipv4_hdr *ipv4_hdr =
			rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));

		print_ipv4_addr(htonl(ipv4_hdr->dst_addr),
				htonl(ipv4_hdr->src_addr),
				rte_get_ptype_l4_name(packet->packet_type));
	} else if (RTE_ETH_IS_IPV6_HDR(packet->packet_type)) {
		struct rte_ipv6_hdr *ipv6_hdr =
			rte_pktmbuf_mtod_offset(packet, struct rte_ipv6_hdr *, sizeof(struct rte_ether_hdr));

		print_ipv6_addr(ipv6_hdr->dst_addr, ipv6_hdr->src_addr, rte_get_ptype_l4_name(packet->packet_type));
	}
}

/*
 * Print L4 header
 *
 * @packet [in]: packet mbuf
 */
static void print_l4_header(const struct rte_mbuf *packet)
{
	uint8_t *l4_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	const struct rte_tcp_hdr *tcp_hdr;
	const struct rte_udp_hdr *udp_hdr;

	if (!RTE_ETH_IS_IPV4_HDR(packet->packet_type))
		return;

	ipv4_hdr = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
	l4_hdr = (typeof(l4_hdr))ipv4_hdr + rte_ipv4_hdr_len(ipv4_hdr);

	switch (ipv4_hdr->next_proto_id) {
	case IPPROTO_UDP:
		udp_hdr = (typeof(udp_hdr))l4_hdr;
		DOCA_LOG_DBG("UDP- DPORT %u, SPORT %u",
			     rte_be_to_cpu_16(udp_hdr->dst_port),
			     rte_be_to_cpu_16(udp_hdr->src_port));
		break;

	case IPPROTO_TCP:
		tcp_hdr = (typeof(tcp_hdr))l4_hdr;
		DOCA_LOG_DBG("TCP- DPORT %u, SPORT %u",
			     rte_be_to_cpu_16(tcp_hdr->dst_port),
			     rte_be_to_cpu_16(tcp_hdr->src_port));
		break;

	default:
		DOCA_LOG_DBG("Unsupported L4 protocol!");
	}
}

/*
 * Callback which creates a DOCA mmap out of a RTE chunk
 * This callback can be used with 'rte_mempool_mem_iter()'
 *
 * @mp [in]: The RTE memory pool
 * @opaque [in]: Opaque representing pointer to 'struct dpdk_mempool_shadow'
 * @memhdr [in]: Header describing the RTE chunk
 * @mem_idx [in]: Index of the RTE chunk
 */
static void dpdk_chunk_to_mmap_cb(struct rte_mempool *mp,
				  void *opaque,
				  struct rte_mempool_memhdr *memhdr,
				  unsigned int mem_idx)
{
	(void)mp;
	(void)mem_idx;

	struct dpdk_mempool_shadow *mempool_shadow = opaque;
	struct doca_mmap *new_mmap;
	doca_error_t result;

	result = doca_mmap_create(&new_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create mmap");
		return;
	}

	result = doca_mmap_set_memrange(new_mmap, memhdr->addr, memhdr->len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range of memory map (input): %s", doca_error_get_descr(result));
		doca_mmap_destroy(new_mmap);
		return;
	}

	result = doca_mmap_add_dev(new_mmap, mempool_shadow->device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to mmap: %s", doca_error_get_descr(result));
		doca_mmap_destroy(new_mmap);
		return;
	}

	result = doca_mmap_start(new_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start memory map: %s", doca_error_get_descr(result));
		doca_mmap_destroy(new_mmap);
		return;
	}

	mempool_shadow->mmap_arr[mempool_shadow->nb_mmaps++] = new_mmap;
}

struct dpdk_mempool_shadow *dpdk_mempool_shadow_create(struct rte_mempool *mbuf_pool, struct doca_dev *device)
{
	uint32_t nb_iterated_chunks, nb_chunks;
	struct dpdk_mempool_shadow *mempool_shadow = rte_zmalloc(NULL, sizeof(*mempool_shadow), 0);

	if (mempool_shadow == NULL) {
		DOCA_LOG_ERR("Dynamic allocation failed");
		return NULL;
	}
	mempool_shadow->device = device;

	nb_chunks = mbuf_pool->nb_mem_chunks;
	mempool_shadow->mmap_arr = (struct doca_mmap **)rte_zmalloc(NULL, sizeof(struct doca_mmap *) * nb_chunks, 0);
	if (mempool_shadow->mmap_arr == NULL) {
		DOCA_LOG_ERR("Dynamic allocation failed");
		dpdk_mempool_shadow_destroy(mempool_shadow);
		return NULL;
	}

	/* Register each chunk in the mempool to an mmap */
	nb_iterated_chunks = rte_mempool_mem_iter(mbuf_pool, dpdk_chunk_to_mmap_cb, mempool_shadow);
	if (nb_iterated_chunks != mempool_shadow->nb_mmaps) {
		dpdk_mempool_shadow_destroy(mempool_shadow);
		return NULL;
	}

	return mempool_shadow;
}

/*
 * Function which creates a DOCA mmap out of a RTE external chunk.
 * This function will work with any chunk, not only RTE one.
 *
 * @mempool_shadow [in]: Pointer to 'struct dpdk_mempool_shadow'
 * @chunk [in]: Contiguous memory used as external chunk to DPDK
 * @len [in]: The length of chunk in bytes
 * @return: DOCA_SUCCESS on success, and doca_error_t otherwise
 */
static doca_error_t dpdk_ext_chunk_to_mmap(struct dpdk_mempool_shadow *mempool_shadow, void *chunk, size_t len)
{
	struct doca_mmap *new_mmap;
	doca_error_t result;

	result = doca_mmap_create(&new_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create mmap");
		return result;
	}

	result = doca_mmap_set_memrange(new_mmap, chunk, len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range of memory map (input): %s", doca_error_get_descr(result));
		doca_mmap_destroy(new_mmap);
		return result;
	}

	result = doca_mmap_add_dev(new_mmap, mempool_shadow->device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to mmap: %s", doca_error_get_descr(result));
		doca_mmap_destroy(new_mmap);
		return result;
	}

	result = doca_mmap_start(new_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start memory map: %s", doca_error_get_descr(result));
		doca_mmap_destroy(new_mmap);
		return result;
	}

	mempool_shadow->mmap_arr[mempool_shadow->nb_mmaps++] = new_mmap;

	return DOCA_SUCCESS;
}

struct dpdk_mempool_shadow *dpdk_mempool_shadow_create_extbuf(const struct rte_pktmbuf_extmem **ext_mem,
							      uint32_t ext_num,
							      struct doca_dev *device)
{
	doca_error_t result;
	uint32_t i;
	struct dpdk_mempool_shadow *mempool_shadow = rte_zmalloc(NULL, sizeof(*mempool_shadow), 0);

	if (mempool_shadow == NULL) {
		DOCA_LOG_ERR("Dynamic allocation failed");
		return NULL;
	}
	mempool_shadow->device = device;

	mempool_shadow->mmap_arr = (struct doca_mmap **)rte_zmalloc(NULL, sizeof(struct doca_mmap *) * ext_num, 0);
	if (mempool_shadow->mmap_arr == NULL) {
		DOCA_LOG_ERR("Dynamic allocation failed");
		dpdk_mempool_shadow_destroy(mempool_shadow);
		return NULL;
	}

	/* Register each chunk in the mempool to an mmap */
	for (i = 0; i < ext_num; ++i) {
		result = dpdk_ext_chunk_to_mmap(mempool_shadow, ext_mem[i]->buf_ptr, ext_mem[i]->buf_len);
		if (result != DOCA_SUCCESS) {
			dpdk_mempool_shadow_destroy(mempool_shadow);
			return NULL;
		}
	}

	return mempool_shadow;
}

void dpdk_mempool_shadow_destroy(struct dpdk_mempool_shadow *mempool_shadow)
{
	uint32_t mmap_idx;

	if (mempool_shadow->mmap_arr != NULL) {
		for (mmap_idx = 0; mmap_idx < mempool_shadow->nb_mmaps; mmap_idx++)
			doca_mmap_destroy(mempool_shadow->mmap_arr[mmap_idx]);
		rte_free(mempool_shadow->mmap_arr);
	}
	rte_free(mempool_shadow);
}

doca_error_t dpdk_mempool_shadow_find_buf_by_data(struct dpdk_mempool_shadow *mempool_shadow,
						  struct doca_buf_inventory *inventory,
						  uintptr_t mem_range_start,
						  size_t mem_range_size,
						  struct doca_buf **out_buf)
{
	uintptr_t mmap_begin;
	size_t mmap_len;
	uint32_t mmap_idx;

	/* For most cases, only single mmap exists */
	for (mmap_idx = 0; mmap_idx < mempool_shadow->nb_mmaps; mmap_idx++) {
		struct doca_mmap *mmap = mempool_shadow->mmap_arr[mmap_idx];

		doca_mmap_get_memrange(mmap, (void **)&mmap_begin, &mmap_len);
		/* Following checks that memory range is within the mmap while avoiding integer overflow */
		if (mmap_begin <= mem_range_start && mem_range_start < mmap_begin + mmap_len &&
		    mem_range_size <= mmap_begin + mmap_len - mem_range_start)
			return doca_buf_inventory_buf_get_by_data(inventory,
								  mmap,
								  (void *)mem_range_start,
								  mem_range_size,
								  out_buf);
	}

	return DOCA_ERROR_NOT_FOUND;
}

void print_header_info(const struct rte_mbuf *packet, const bool l2, const bool l3, const bool l4)
{
	if (l2)
		print_l2_header(packet);
	if (l3)
		print_l3_header(packet);
	if (l4)
		print_l4_header(packet);
}

void dpdk_fini_with_devs(uint16_t nb_devs)
{
	struct doca_dev *dev;
	doca_error_t result;
	int i;

	dpdk_fini();
	for (i = 0; i < nb_devs; i++) {
		result = doca_dpdk_port_as_dev(i, &dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to get device for closing port %d: %s", i, doca_error_get_descr(result));

		result = doca_dev_close(dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to close device for port %d: %s", i, doca_error_get_descr(result));
	}
}

doca_error_t dpdk_init(int argc, char **argv)
{
	int result;

	result = rte_eal_init(argc, argv);
	if (result < 0) {
		DOCA_LOG_ERR("EAL initialization failed");
		return DOCA_ERROR_DRIVER;
	}
	return DOCA_SUCCESS;
}

void dpdk_fini(void)
{
	int result;

	result = rte_eal_cleanup();
	if (result < 0) {
		DOCA_LOG_ERR("rte_eal_cleanup() failed, error=%d", result);
		return;
	}

	DOCA_LOG_DBG("DPDK fini is done");
}

doca_error_t detect_vf_mac_address(char *vf_if_name, uint8_t *vf_mac_address)
{
	struct ifreq ifr;
	int sockfd;
	char mac_addr_parsed[MAC_ADDR_STR_LEN];

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		DOCA_LOG_ERR("Socket creation failed");
		return DOCA_ERROR_UNKNOWN;
	}

	strncpy(ifr.ifr_name, vf_if_name, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) == -1) {
		DOCA_LOG_ERR("ioctl(SIOCGIFHWADDR) failed");
		close(sockfd);
		return DOCA_ERROR_UNKNOWN;
	}

	memcpy(vf_mac_address, ifr.ifr_hwaddr.sa_data, MAC_ADDR_LEN);
	BIN_ARRAY_TO_MAC_STRING(vf_mac_address, mac_addr_parsed);
	DOCA_LOG_INFO("VF %s was detected to have the following MAC address: %s", vf_if_name, mac_addr_parsed);
	close(sockfd);
	return DOCA_SUCCESS;
}

doca_error_t detect_vf_ip_address(char *vf_if_name, uint8_t *vf_ip_local_address, uint8_t *vf_ip_global_address)
{
	struct ifaddrs *ifap, *ifa;
	struct sockaddr_in6 *sa;
	char addr[INET6_ADDRSTRLEN];

	if (getifaddrs(&ifap) == -1) {
		DOCA_LOG_ERR("getifaddrs failed");
		return DOCA_ERROR_UNEXPECTED;
	}

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;

		if (strcmp(ifa->ifa_name, vf_if_name) != 0)
			continue;

		sa = (struct sockaddr_in6 *)ifa->ifa_addr;
		inet_ntop(AF_INET6, &sa->sin6_addr, addr, sizeof(addr));
		// Detect if the IP address is a local address by checking the first two bytes e.g.
		// fe80::1234:5678:9abc:def0
		if (sa->sin6_addr.s6_addr[0] == 0xfe && (sa->sin6_addr.s6_addr[1] & 0xc0) == 0x80) {
			if (inet_pton(AF_INET6, addr, vf_ip_local_address) != 1) {
				DOCA_LOG_ERR("Failed to parse VF IPv6 local addr: %s", addr);
				freeifaddrs(ifap);
				return DOCA_ERROR_BAD_CONFIG;
			}
			DOCA_LOG_INFO("Local IP6 address %s for VF %s", addr, vf_if_name);
		} else {
			if (inet_pton(AF_INET6, addr, vf_ip_global_address) != 1) {
				DOCA_LOG_ERR("Failed to parse VF IPv6 global addr: %s", addr);
				freeifaddrs(ifap);
				return DOCA_ERROR_BAD_CONFIG;
			}
			DOCA_LOG_INFO("Global IP6 address %s for VF %s", addr, vf_if_name);
		}
	}

	freeifaddrs(ifap);
	return DOCA_SUCCESS;
}