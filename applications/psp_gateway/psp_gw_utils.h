/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef _PSP_GW_UTILS_H_
#define _PSP_GW_UTILS_H_

#include <inttypes.h>
#include <string>

#include <rte_byteorder.h>
#include <rte_ether.h>

#include "doca_flow_net.h"
#include "psp_gw_config.h"

/* set IPv6 address in array */
#define SET_IP6_ADDR(addr, a, b, c, d) \
	do { \
		addr[0] = a; \
		addr[1] = b; \
		addr[2] = c; \
		addr[3] = d; \
	} while (0)

/**
 * @brief Converts a MAC/ethernet address to a C++ string
 */
std::string mac_to_string(const rte_ether_addr &mac_addr);

/**
 * @brief Tests whether a MAC address has been set (is non-zero)
 *
 * @addr [in]: the mac addr to test
 * @return: true if all bits are zero; false otherwise
 */
bool is_empty_mac_addr(const rte_ether_addr &addr);

/**
 * @brief Converts an IPv4 address to a C++ string
 */
std::string ipv4_to_string(rte_be32_t ipv4_addr);

/**
 * @brief Converts an IPv6 address to a C++ string
 */
std::string ipv6_to_string(const uint32_t ipv6_addr[]);

/**
 * @brief Converts a DOCA Flow IP address struct to a C++ string
 */
std::string ip_to_string(const struct doca_flow_ip_addr &ip_addr);

/**
 * @brief Compare DOCA Flow IP address struct, return true if addresses are equal
 *
 * @ip_a [in]: first IP address
 * @ip_b [in]: second IP address
 * @return: true if both addresses is the same
 */
bool is_ip_equal(struct doca_flow_ip_addr *ip_a, struct doca_flow_ip_addr *ip_b);

/**
 * @brief Parse an IP address string into a DOCA Flow IP address struct
 *
 * @ip_str [in]: the IP address string to parse
 * @enforce_l3_type [in]: the L3 type to enforce
 * @ip_addr [out]: the parsed IP address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t parse_ip_addr(const std::string &ip_str,
			   doca_flow_l3_type enforce_l3_type,
			   struct doca_flow_ip_addr *ip_addr);

/**
 * @brief Copy an IP address struct
 *
 * @src [in]: the source IP address
 * @dst [out]: the destination IP address
 */
void copy_ip_addr(const struct doca_flow_ip_addr &src, struct doca_flow_ip_addr &dst);

/**
 * @brief Search for a peer in a vector of peers that holds the same IP pair
 *
 * @peers [in]: vector of peers to search
 * @vip_pair [in]: IP pair to search for
 * @return: pointer to the peer if found, nullptr otherwise
 */
psp_gw_peer *lookup_vip_pair(std::vector<psp_gw_peer> *peers, ip_pair &vip_pair);

#endif /* _PSP_GW_UTILS_H_ */
