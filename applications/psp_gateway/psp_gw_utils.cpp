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

#include <netinet/in.h>
#include <arpa/inet.h>

#include <rte_ether.h>

#include "psp_gw_utils.h"

std::string mac_to_string(const rte_ether_addr &mac_addr)
{
	std::string addr_str(RTE_ETHER_ADDR_FMT_SIZE, '\0');
	rte_ether_format_addr(addr_str.data(), RTE_ETHER_ADDR_FMT_SIZE, &mac_addr);
	addr_str.resize(strlen(addr_str.c_str()));
	return addr_str;
}

bool is_empty_mac_addr(const rte_ether_addr &addr)
{
	rte_ether_addr empty_ether_addr = {};
	return !memcmp(empty_ether_addr.addr_bytes, addr.addr_bytes, RTE_ETHER_ADDR_LEN);
}

std::string ipv4_to_string(rte_be32_t ipv4_addr)
{
	std::string addr_str(INET_ADDRSTRLEN, '\0');
	inet_ntop(AF_INET, &ipv4_addr, addr_str.data(), INET_ADDRSTRLEN);
	addr_str.resize(strlen(addr_str.c_str()));
	return addr_str;
}

std::string ipv6_to_string(const uint32_t ipv6_addr[])
{
	std::string addr_str(INET6_ADDRSTRLEN, '\0');
	inet_ntop(AF_INET6, ipv6_addr, addr_str.data(), INET6_ADDRSTRLEN);
	addr_str.resize(strlen(addr_str.c_str()));
	return addr_str;
}

std::string ip_to_string(const struct doca_flow_ip_addr &ip_addr)
{
	if (ip_addr.type == DOCA_FLOW_L3_TYPE_IP4)
		return ipv4_to_string(ip_addr.ipv4_addr);
	else if (ip_addr.type == DOCA_FLOW_L3_TYPE_IP6)
		return ipv6_to_string(ip_addr.ipv6_addr);
	return "Invalid IP type";
}

bool is_ip_equal(struct doca_flow_ip_addr *ip_a, struct doca_flow_ip_addr *ip_b)
{
	if (ip_a->type != ip_b->type)
		return false;
	if (ip_a->type == DOCA_FLOW_L3_TYPE_IP4)
		return ip_a->ipv4_addr == ip_b->ipv4_addr;
	for (int i = 0; i < 4; i++) {
		if (ip_a->ipv6_addr[i] != ip_b->ipv6_addr[i])
			return false;
	}
	return true;
}

doca_error_t parse_ip_addr(const std::string &ip_str,
			   doca_flow_l3_type enforce_l3_type,
			   struct doca_flow_ip_addr *ip_addr)
{
	memset(ip_addr, 0, sizeof(struct doca_flow_ip_addr));
	if (enforce_l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		ip_addr->type = DOCA_FLOW_L3_TYPE_IP4;
		if (inet_pton(AF_INET, ip_str.c_str(), &ip_addr->ipv4_addr) != 1)
			return DOCA_ERROR_INVALID_VALUE;
	} else if (enforce_l3_type == DOCA_FLOW_L3_TYPE_IP6) {
		ip_addr->type = DOCA_FLOW_L3_TYPE_IP6;
		if (inet_pton(AF_INET6, ip_str.c_str(), ip_addr->ipv6_addr) != 1)
			return DOCA_ERROR_INVALID_VALUE;
	} else {
		if (inet_pton(AF_INET, ip_str.c_str(), &ip_addr->ipv4_addr) == 1) {
			ip_addr->type = DOCA_FLOW_L3_TYPE_IP4;
		} else if (inet_pton(AF_INET6, ip_str.c_str(), ip_addr->ipv6_addr) == 1) {
			ip_addr->type = DOCA_FLOW_L3_TYPE_IP6;
		} else {
			return DOCA_ERROR_INVALID_VALUE;
		}
	}
	return DOCA_SUCCESS;
}

void copy_ip_addr(const struct doca_flow_ip_addr &src, struct doca_flow_ip_addr &dst)
{
	memset(&dst, 0, sizeof(struct doca_flow_ip_addr));
	if (src.type == DOCA_FLOW_L3_TYPE_IP4) {
		dst.type = DOCA_FLOW_L3_TYPE_IP4;
		dst.ipv4_addr = src.ipv4_addr;
	} else if (src.type == DOCA_FLOW_L3_TYPE_IP6) {
		dst.type = DOCA_FLOW_L3_TYPE_IP6;
		memcpy(dst.ipv6_addr, src.ipv6_addr, sizeof(src.ipv6_addr));
	} // else dst.type is already 0 == DOCA_FLOW_L3_TYPE_NONE
}

psp_gw_peer *lookup_vip_pair(std::vector<psp_gw_peer> *peers, ip_pair &vip_pair)
{
	for (auto &peer : *peers) {
		for (auto peer_vip_pair : peer.vip_pairs) {
			if (is_ip_equal(&peer_vip_pair.dst_vip, &vip_pair.dst_vip) &&
			    is_ip_equal(&peer_vip_pair.src_vip, &vip_pair.src_vip))
				return &peer;
		}
	}
	return nullptr;
}
