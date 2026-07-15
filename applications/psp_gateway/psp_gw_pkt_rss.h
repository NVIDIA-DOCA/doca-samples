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

#ifndef _PSP_GW_PKT_RSS_H
#define _PSP_GW_PKT_RSS_H

#include <stdint.h>

struct psp_gw_app_config;
class PSP_GatewayFlows;
class PSP_GatewayImpl;

/**
 * @brief The parameters needed by each L-Core's main loop.
 */
struct lcore_params {
	volatile bool *force_quit;   /* Indicates the application has been requested to quit */
	psp_gw_app_config *config;   /* Contains configuration information */
	psp_pf_dev *pf_dev;	     /* The PF device to poll */
	PSP_GatewayFlows *psp_flows; /* The DOCA Flow objects */
	PSP_GatewayImpl *psp_svc;    /* The RPC service which manages tunnels */
};

/**
 * @brief The entry point for each L-Core's main processing loop.
 * Each L-Core polls a different Rx queue on the Host PF(s).
 * If the packet indicates the need for a new tunnel to be
 * established, it will be passed to the psp_svc object.
 * Note multiple such packets may be received during the
 * creation of the tunnel; in any case, they will be resubmitted
 * to the encryption pipeline once the new flow has been created.
 *
 * @lcore_args [in]: a pointer to an lcore_params struct
 * @return: 0 on success (the main loop exited normally), negative value otherwise
 */
int lcore_pkt_proc_func(void *lcore_args);

/**
 * @brief Used by the psp_svc to re-inject a packet via the Host PF Tx queue after
 *        a new tunnel has been established.
 *
 * @packet [in]: the packet to submit into the egress pipeline
 * @port_id [in]: the port on which to send the packet, usually the host PF
 * @return: true if the packet was successfully sent, false if too many retries failed
 */
bool reinject_packet(struct rte_mbuf *packet, uint16_t port_id);

/**
 * @brief Used to reply to an ARP request.
 *
 * @mpool [in]: the mempool to allocate the response packet from
 * @port_id [in]: the port on which to send the ARP response
 * @queue_id [in]: the queue on which to send the ARP response
 * @port_src_mac [in]: the source mac address of the port
 * @request_pkt [in]: the ARP request packet
 * @arp_response_meta_flag [in]: the metadata flag to set on the ARP response
 * @return: number of ARP packets handled (currently only one packet is supported)
 */
uint16_t handle_arp(struct rte_mempool *mpool,
		    uint16_t port_id,
		    uint16_t queue_id,
		    rte_ether_addr *port_src_mac,
		    const struct rte_mbuf *request_pkt,
		    uint32_t arp_response_meta_flag);

/**
 * @brief Used to reply to a Neighbor Solicitation packet.
 *
 * @mpool [in]: the mempool to allocate the response packet from
 * @port_id [in]: the port on which to send the Neighbor Solicitation response
 * @queue_id [in]: the queue on which to send the Neighbor Solicitation response
 * @port_src_mac [in]: the source mac address of the port
 * @request_pkt [in]: the Neighbor Solicitation packet
 * @na_response_meta_flag [in]: the metadata flag to set on the Neighbor Solicitation response
 * @return: number of Neighbor Solicitation packets handled (currently only one packet is supported)
 */
uint16_t handle_neighbor_solicitation(struct rte_mempool *mpool,
				      uint16_t port_id,
				      uint16_t queue_id,
				      rte_ether_addr *port_src_mac,
				      const struct rte_mbuf *request_pkt,
				      uint32_t na_response_meta_flag);

#endif // _PSP_GW_PKT_RSS_H
