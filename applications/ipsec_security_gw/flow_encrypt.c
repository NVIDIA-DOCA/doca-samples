/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_log.h>
#include <doca_bitfield.h>

#include <pack.h>
#include <utils.h>

#include "doca_flow.h"
#include "flow_encrypt.h"

DOCA_LOG_REGISTER(IPSEC_SECURITY_GW::flow_encrypt);

#define ENCAP_DST_IP_IDX_IP4 30		  /* index in encap raw data for destination IPv4 */
#define ENCAP_DST_IP_IDX_IP6 38		  /* index in encap raw data for destination IPv4 */
#define ENCAP_IP_ID_IDX_IP4 18		  /* index in encap raw data for IPv4 ID */
#define ENCAP_IDX_SRC_MAC 6		  /* index in encap raw data for source mac */
#define ENCAP_DST_UDP_PORT_IDX 2	  /* index in encap raw data for UDP destination port */
#define ENCAP_ESP_SPI_IDX_TUNNEL_IP4 34	  /* index in encap raw data for esp SPI in IPv4 tunnel */
#define ENCAP_ESP_SPI_IDX_TUNNEL_IP6 54	  /* index in encap raw data for esp SPI in IPv6 tunnel */
#define ENCAP_ESP_SPI_IDX_TRANSPORT 0	  /* index in encap raw data for esp SPI in transport mode*/
#define ENCAP_ESP_SPI_IDX_UDP_TRANSPORT 8 /* index in encap raw data for esp SPI in transport over UDP mode*/
#define ENCAP_ESP_SN_IDX_TUNNEL_IP4 38	  /* index in encap raw data for esp SN in IPv4 tunnel */
#define ENCAP_ESP_SN_IDX_TUNNEL_IP6 58	  /* index in encap raw data for esp SN in IPv6 tunnel */
#define ENCAP_ESP_SN_IDX_TRANSPORT 4	  /* index in encap raw data for esp SN in transport mode*/
#define ENCAP_ESP_SN_IDX_UDP_TRANSPORT 12 /* index in encap raw data for esp SN in transport over UDP mode*/

#define ENCRYPT_IPV4_SEQ_IDX 0 /* IPv4 sequence index for ordered list */
#define ENCRYPT_IPV6_SEQ_IDX 1 /* IPv6 sequence index for ordered list */

#define ENCAP_MARKER_HEADER_SIZE 8 /* non-ESP marker header size */
#define PADDING_ALIGN 4		   /* padding alignment */

static const uint8_t esp_pad_bytes[15] = {
	1,
	2,
	3,
	4,
	5,
	6,
	7,
	8,
	9,
	10,
	11,
	12,
	13,
	14,
	15,
};

static uint16_t current_ip_id; /* Incremented for each new packet */

/*
 * Create reformat data for encapsulation in transport mode, and copy it to reformat_data pointer
 *
 * @rule [in]: current rule for encapsulation
 * @sw_sn_inc [in]: if true, sequence number will be incremented in software
 * @reformat_data [out]: pointer to created data
 * @reformat_data_sz [out]: data size
 */
static void create_transport_encap(struct encrypt_rule *rule,
				   bool sw_sn_inc,
				   uint8_t *reformat_data,
				   uint16_t *reformat_data_sz)
{
	uint8_t reformat_encap_data[16] = {
		0x00,
		0x00,
		0x00,
		0x00, /* SPI */
		0x00,
		0x00,
		0x00,
		0x00, /* SN */
		0x00,
		0x00,
		0x00,
		0x00, /* IV */
		0x00,
		0x00,
		0x00,
		0x00,
	};

	reformat_encap_data[ENCAP_ESP_SPI_IDX_TRANSPORT] = GET_BYTE(rule->esp_spi, 3);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TRANSPORT + 1] = GET_BYTE(rule->esp_spi, 2);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TRANSPORT + 2] = GET_BYTE(rule->esp_spi, 1);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TRANSPORT + 3] = GET_BYTE(rule->esp_spi, 0);

	if (sw_sn_inc == true) {
		reformat_encap_data[ENCAP_ESP_SN_IDX_TRANSPORT] = GET_BYTE(rule->current_sn, 3);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TRANSPORT + 1] = GET_BYTE(rule->current_sn, 2);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TRANSPORT + 2] = GET_BYTE(rule->current_sn, 1);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TRANSPORT + 3] = GET_BYTE(rule->current_sn, 0);
	}

	memcpy(reformat_data, reformat_encap_data, sizeof(reformat_encap_data));
	*reformat_data_sz = sizeof(reformat_encap_data);
}

/*
 * Create reformat data for encapsulation in UDP transport mode, and copy it to reformat_data pointer
 *
 * @rule [in]: current rule for encapsulation
 * @sw_sn_inc [in]: if true, sequence number will be incremented in software
 * @reformat_data [out]: pointer to created data
 * @reformat_data_sz [out]: data size
 */
static void create_udp_transport_encap(struct encrypt_rule *rule,
				       bool sw_sn_inc,
				       uint8_t *reformat_data,
				       uint16_t *reformat_data_sz)
{
	uint16_t udp_dst_port = 4500;
	uint8_t reformat_encap_data[24] = {
		0x30, 0x39, 0x00, 0x00, /* UDP src/dst */
		0x00, 0xa4, 0x00, 0x00, /* USD sum/len */
		0x00, 0x00, 0x00, 0x00, /* SPI */
		0x00, 0x00, 0x00, 0x00, /* SN */
		0x00, 0x00, 0x00, 0x00, /* IV */
		0x00, 0x00, 0x00, 0x00,
	};

	reformat_encap_data[ENCAP_ESP_SPI_IDX_UDP_TRANSPORT] = GET_BYTE(rule->esp_spi, 3);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_UDP_TRANSPORT + 1] = GET_BYTE(rule->esp_spi, 2);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_UDP_TRANSPORT + 2] = GET_BYTE(rule->esp_spi, 1);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_UDP_TRANSPORT + 3] = GET_BYTE(rule->esp_spi, 0);

	reformat_encap_data[ENCAP_DST_UDP_PORT_IDX] = GET_BYTE(udp_dst_port, 1);
	reformat_encap_data[ENCAP_DST_UDP_PORT_IDX + 1] = GET_BYTE(udp_dst_port, 0);

	if (sw_sn_inc == true) {
		reformat_encap_data[ENCAP_ESP_SN_IDX_UDP_TRANSPORT] = GET_BYTE(rule->current_sn, 3);
		reformat_encap_data[ENCAP_ESP_SN_IDX_UDP_TRANSPORT + 1] = GET_BYTE(rule->current_sn, 2);
		reformat_encap_data[ENCAP_ESP_SN_IDX_UDP_TRANSPORT + 2] = GET_BYTE(rule->current_sn, 1);
		reformat_encap_data[ENCAP_ESP_SN_IDX_UDP_TRANSPORT + 3] = GET_BYTE(rule->current_sn, 0);
	}

	memcpy(reformat_data, reformat_encap_data, sizeof(reformat_encap_data));
	*reformat_data_sz = sizeof(reformat_encap_data);
}

/*
 * Create reformat data for encapsulation IPV4 tunnel, and copy it to reformat_data pointer
 *
 * @rule [in]: current rule for encapsulation
 * @sw_sn_inc [in]: if true, sequence number will be incremented in software
 * @eth_header [in]: contains the src mac address
 * @reformat_data [out]: pointer to created data
 * @reformat_data_sz [out]: data size
 */
static void create_ipv4_tunnel_encap(struct encrypt_rule *rule,
				     bool sw_sn_inc,
				     struct doca_flow_header_eth *eth_header,
				     uint8_t *reformat_data,
				     uint16_t *reformat_data_sz)
{
	uint8_t reformat_encap_data[50] = {
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, /* mac_dst */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* mac_src */
		0x08, 0x00,			    /* mac_type */
		0x45, 0x00, 0x00, 0x00, 0x00, 0x00, /* IPv4 - Part 1 */
		0x00, 0x00, 0x00, 0x32, 0x00, 0x00, /* IPv4 - Part 2 */
		0x02, 0x02, 0x02, 0x02,		    /* IP src */
		0x00, 0x00, 0x00, 0x00,		    /* IP dst */
		0x00, 0x00, 0x00, 0x00,		    /* SPI */
		0x00, 0x00, 0x00, 0x00,		    /* SN */
		0x00, 0x00, 0x00, 0x00,		    /* IV */
		0x00, 0x00, 0x00, 0x00,
	};

	reformat_encap_data[ENCAP_IDX_SRC_MAC] = eth_header->src_mac[0];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 1] = eth_header->src_mac[1];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 2] = eth_header->src_mac[2];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 3] = eth_header->src_mac[3];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 4] = eth_header->src_mac[4];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 5] = eth_header->src_mac[5];

	/* dst IP was already converted to big endian */
	reformat_encap_data[ENCAP_DST_IP_IDX_IP4] = GET_BYTE(rule->encap_dst_ip4, 0);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP4 + 1] = GET_BYTE(rule->encap_dst_ip4, 1);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP4 + 2] = GET_BYTE(rule->encap_dst_ip4, 2);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP4 + 3] = GET_BYTE(rule->encap_dst_ip4, 3);

	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP4] = GET_BYTE(rule->esp_spi, 3);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP4 + 1] = GET_BYTE(rule->esp_spi, 2);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP4 + 2] = GET_BYTE(rule->esp_spi, 1);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP4 + 3] = GET_BYTE(rule->esp_spi, 0);

	reformat_encap_data[ENCAP_IP_ID_IDX_IP4] = GET_BYTE(current_ip_id, 1);
	reformat_encap_data[ENCAP_IP_ID_IDX_IP4 + 1] = GET_BYTE(current_ip_id, 0);
	++current_ip_id;

	if (sw_sn_inc == true) {
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP4] = GET_BYTE(rule->current_sn, 3);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP4 + 1] = GET_BYTE(rule->current_sn, 2);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP4 + 2] = GET_BYTE(rule->current_sn, 1);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP4 + 3] = GET_BYTE(rule->current_sn, 0);
	}

	memcpy(reformat_data, reformat_encap_data, sizeof(reformat_encap_data));
	*reformat_data_sz = sizeof(reformat_encap_data);
}

/*
 * Create reformat data for encapsulation IPV6 tunnel, and copy it to reformat_data pointer
 *
 * @rule [in]: current rule for encapsulation
 * @sw_sn_inc [in]: if true, sequence number will be incremented in software
 * @eth_header [in]: contains the src mac address
 * @reformat_data [out]: pointer to created data
 * @reformat_data_sz [out]: data size
 */
static void create_ipv6_tunnel_encap(struct encrypt_rule *rule,
				     bool sw_sn_inc,
				     struct doca_flow_header_eth *eth_header,
				     uint8_t *reformat_data,
				     uint16_t *reformat_data_sz)
{
	uint8_t reformat_encap_data[70] = {
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, /* mac_dst */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* mac_src */
		0x86, 0xdd,			    /* mac_type */
		0x60, 0x00, 0x00, 0x00,		    /* IPv6 - Part 1 */
		0x00, 0x00, 0x32, 0x40,		    /* IPv6 - Part 2 */
		0x02, 0x02, 0x02, 0x02,		    /* IP src */
		0x02, 0x02, 0x02, 0x02,		    /* IP src */
		0x02, 0x02, 0x02, 0x02,		    /* IP src */
		0x02, 0x02, 0x02, 0x02,		    /* IP src */
		0x01, 0x01, 0x01, 0x01,		    /* IP dst */
		0x01, 0x01, 0x01, 0x01,		    /* IP dst */
		0x01, 0x01, 0x01, 0x01,		    /* IP dst */
		0x01, 0x01, 0x01, 0x01,		    /* IP dst */
		0x00, 0x00, 0x00, 0x00,		    /* SPI */
		0x00, 0x00, 0x00, 0x00,		    /* SN */
		0x00, 0x00, 0x00, 0x00,		    /* IV */
		0x00, 0x00, 0x00, 0x00,
	};

	reformat_encap_data[ENCAP_IDX_SRC_MAC] = eth_header->src_mac[0];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 1] = eth_header->src_mac[1];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 2] = eth_header->src_mac[2];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 3] = eth_header->src_mac[3];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 4] = eth_header->src_mac[4];
	reformat_encap_data[ENCAP_IDX_SRC_MAC + 5] = eth_header->src_mac[5];

	/* dst IP was already converted to big endian */
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6] = GET_BYTE(rule->encap_dst_ip6[0], 0);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 1] = GET_BYTE(rule->encap_dst_ip6[0], 1);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 2] = GET_BYTE(rule->encap_dst_ip6[0], 2);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 3] = GET_BYTE(rule->encap_dst_ip6[0], 3);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 4] = GET_BYTE(rule->encap_dst_ip6[1], 0);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 5] = GET_BYTE(rule->encap_dst_ip6[1], 1);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 6] = GET_BYTE(rule->encap_dst_ip6[1], 2);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 7] = GET_BYTE(rule->encap_dst_ip6[1], 3);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 8] = GET_BYTE(rule->encap_dst_ip6[2], 0);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 9] = GET_BYTE(rule->encap_dst_ip6[2], 1);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 10] = GET_BYTE(rule->encap_dst_ip6[2], 2);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 11] = GET_BYTE(rule->encap_dst_ip6[2], 3);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 12] = GET_BYTE(rule->encap_dst_ip6[3], 0);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 13] = GET_BYTE(rule->encap_dst_ip6[3], 1);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 14] = GET_BYTE(rule->encap_dst_ip6[3], 2);
	reformat_encap_data[ENCAP_DST_IP_IDX_IP6 + 15] = GET_BYTE(rule->encap_dst_ip6[3], 3);

	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP6] = GET_BYTE(rule->esp_spi, 3);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP6 + 1] = GET_BYTE(rule->esp_spi, 2);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP6 + 2] = GET_BYTE(rule->esp_spi, 1);
	reformat_encap_data[ENCAP_ESP_SPI_IDX_TUNNEL_IP6 + 3] = GET_BYTE(rule->esp_spi, 0);

	if (sw_sn_inc == true) {
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP6] = GET_BYTE(rule->current_sn, 3);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP6 + 1] = GET_BYTE(rule->current_sn, 2);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP6 + 2] = GET_BYTE(rule->current_sn, 1);
		reformat_encap_data[ENCAP_ESP_SN_IDX_TUNNEL_IP6 + 3] = GET_BYTE(rule->current_sn, 0);
	}

	memcpy(reformat_data, reformat_encap_data, sizeof(reformat_encap_data));
	*reformat_data_sz = sizeof(reformat_encap_data);
}

/*
 * Create egress IP classifier to choose to which encrypt pipe to forward the packet
 *
 * @port [in]: port of the pipe
 * @is_root [in]: true for vnf mode
 * @debug_mode [in]: true for vnf mode
 * @encrypt_pipes [in]: all the encrypt pipes
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_egress_ip_classifier(struct doca_flow_port *port,
						bool is_root,
						bool debug_mode,
						struct encrypt_pipes *encrypt_pipes,
						struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct security_gateway_pipe_info *pipe = &encrypt_pipes->egress_ip_classifier;
	struct doca_flow_pipe_entry **entry = NULL;
	doca_error_t result;
	int num_of_entries = 2;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	match.parser_meta.outer_l3_type = UINT32_MAX;
	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "IP_CLASSIFIER_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create IP classifier pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	if (debug_mode) {
		pipe->entries_info =
			(struct security_gateway_entry_info *)calloc(2, sizeof(struct security_gateway_entry_info));
		if (pipe->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	fwd.next_pipe = encrypt_pipes->match_encrypt_ipv4_pipe.pipe;

	if (debug_mode) {
		snprintf(pipe->entries_info[pipe->nb_entries].name, MAX_NAME_LEN, "IPv4");
		entry = &pipe->entries_info[pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(0,
					  pipe->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_WAIT_FOR_BATCH,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ipv4 entry: %s", doca_error_get_descr(result));
		return result;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	fwd.next_pipe = encrypt_pipes->match_encrypt_ipv6_pipe.pipe;

	if (debug_mode) {
		snprintf(pipe->entries_info[pipe->nb_entries].name, MAX_NAME_LEN, "IPv6");
		entry = &pipe->entries_info[pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(0,
					  pipe->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ipv6 entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS)
		return result;
	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add vxlan encap pipe entry
 *
 * @port [in]: port of the pipe
 * @pipe [in]: pipe to add entry
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_vxlan_encap_pipe_entry(struct doca_flow_port *port,
					       struct security_gateway_pipe_info *pipe,
					       struct ipsec_security_gw_config *app_cfg)
{
	int num_of_entries = 1;
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct doca_flow_pipe_entry **entry = NULL;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	if (app_cfg->debug_mode) {
		pipe->entries_info =
			(struct security_gateway_entry_info *)calloc(1, sizeof(struct security_gateway_entry_info));
		if (pipe->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			return DOCA_ERROR_NO_MEMORY;
		}
		snprintf(pipe->entries_info[pipe->nb_entries].name, MAX_NAME_LEN, "vxlan_encap");
		entry = &pipe->entries_info[pipe->nb_entries++].entry;
	}

	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.encap_cfg.encap.outer.ip4.src_ip = BE_IPV4_ADDR(1, 2, 3, 4);
	actions.encap_cfg.encap.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	actions.encap_cfg.encap.outer.ip4.flags_fragment_offset = RTE_BE16(DOCA_FLOW_IP4_FLAG_DONT_FRAGMENT);
	actions.encap_cfg.encap.outer.ip4.ttl = 17;
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	actions.encap_cfg.encap.tun.vxlan_tun_id = DOCA_HTOBE32(app_cfg->vni);

	result = doca_flow_pipe_add_entry(0,
					  pipe->pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  NULL,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ipv4 entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS)
		return result;
	if (app_cfg->secured_status[0].nb_processed != num_of_entries || app_cfg->secured_status[0].failure)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

/*
 * Create vxlan encap pipe
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID to forward the packet to
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_vxlan_encap_pipe(struct doca_flow_port *port,
					    int port_id,
					    struct ipsec_security_gw_config *app_cfg)
{
	int nb_actions = 1;
	struct security_gateway_pipe_info *pipe = &app_cfg->encrypt_pipes.vxlan_encap_pipe;
	struct doca_flow_actions actions, *actions_arr[nb_actions];
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	actions.encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	actions.encap_cfg.is_l2 = true;
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.src_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	SET_MAC_ADDR(actions.encap_cfg.encap.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	actions.encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.encap_cfg.encap.outer.ip4.src_ip = 0xffffffff;
	actions.encap_cfg.encap.outer.ip4.dst_ip = 0xffffffff;
	actions.encap_cfg.encap.outer.ip4.ttl = 0xff;
	actions.encap_cfg.encap.outer.ip4.flags_fragment_offset = 0xffff;
	actions.encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	actions.encap_cfg.encap.outer.udp.l4_port.dst_port = RTE_BE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
	actions.encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	actions.encap_cfg.encap.tun.vxlan_tun_id = 0xffffffff;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "VXLAN_ENCAP_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, nb_actions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vxlan encap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return add_vxlan_encap_pipe_entry(port, pipe, app_cfg);

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create egress pipe to insert non-ESP marker header and forward to specified port
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID to forward the packet to
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_marker_encap_pipe(struct doca_flow_port *port,
					     uint16_t port_id,
					     struct ipsec_security_gw_config *app_cfg)
{
	int nb_actions = 1;
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, actions_arr[nb_actions];
	struct doca_flow_actions *actions_list[] = {&actions_arr[0]};
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct security_gateway_pipe_info *pipe_info = &app_cfg->encrypt_pipes.marker_insert_pipe;
	struct doca_flow_pipe_entry *entry = NULL;
	doca_error_t result;

	if (app_cfg->offload != IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH &&
	    app_cfg->offload != IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP) {
		return DOCA_SUCCESS;
	}

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));

	actions.has_crypto_encap = true;
	actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
	actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_NON_ESP_MARKER;

	actions_arr[0] = actions;
	memset(actions_arr[0].crypto_encap.encap_data, 0, ENCAP_MARKER_HEADER_SIZE);
	actions_arr[0].crypto_encap.data_size = ENCAP_MARKER_HEADER_SIZE;

	strcpy(pipe_info->name, "MARKER_ENCAP_PIPE");
	pipe_info->nb_entries = 0;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_info->name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_list, NULL, NULL, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create non-ESP marker encap pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(1, sizeof(struct security_gateway_entry_info));

		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
			goto destroy_pipe_cfg;
		}
	}

	result = doca_flow_pipe_add_entry(0,
					  pipe_info->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add non-ESP marker encap entry: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 1);
	if (result != DOCA_SUCCESS)
		goto destroy_pipe_cfg;

	if (app_cfg->secured_status[0].nb_processed != 1 || app_cfg->secured_status[0].failure)
		result = DOCA_ERROR_BAD_STATE;

	if (result == DOCA_SUCCESS && pipe_info->entries_info != NULL) {
		snprintf(pipe_info->entries_info[pipe_info->nb_entries].name, MAX_NAME_LEN, "marker_encap");
		pipe_info->entries_info[pipe_info->nb_entries++].entry = entry;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create ipsec encrypt pipe with shared IPSEC encryption object
 *
 * @port [in]: port of the pipe
 * @port_id [in]: port ID to forward the packet to
 * @expected_entries [in]: expected number of entries
 * @app_cfg [in]: application configuration struct
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_encrypt_pipe(struct doca_flow_port *port,
					      uint16_t port_id,
					      int expected_entries,
					      struct ipsec_security_gw_config *app_cfg,
					      struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_crypto crypto_actions_ipv4 = {};
	struct doca_flow_crypto crypto_actions_ipv6 = {};
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	const int nb_ordered_lists = 2;
	struct doca_flow_ordered_list *ordered_lists[nb_ordered_lists];
	struct doca_flow_ordered_list ordered_list_ipv4 = {};
	struct doca_flow_ordered_list ordered_list_ipv6 = {};
	struct doca_flow_ordered_list_element elements_ipv4[1] = {0};
	struct doca_flow_ordered_list_element elements_ipv6[1] = {0};
	doca_error_t result;

	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	crypto_actions_ipv4.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
	crypto_actions_ipv4.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
	crypto_actions_ipv4.crypto.crypto_id = UINT32_MAX;
	if (!app_cfg->sw_sn_inc_enable) {
		crypto_actions_ipv4.crypto.ipsec_sa.sn_en = !app_cfg->sw_sn_inc_enable;
	}

	if (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH ||
	    app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP) {
		crypto_actions_ipv4.has_crypto_encap = true;
		crypto_actions_ipv4.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
		crypto_actions_ipv4.crypto_encap.icv_size = get_icv_len_int(app_cfg->icv_length);

		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL) {
			crypto_actions_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_TUNNEL;
			crypto_actions_ipv4.crypto_encap.data_size = 50;
		} else if (app_cfg->mode == IPSEC_SECURITY_GW_TRANSPORT) {
			crypto_actions_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_OVER_IPV4;
			crypto_actions_ipv4.crypto_encap.data_size = 16;
		} else {
			crypto_actions_ipv4.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_UDP_ESP_OVER_IPV4;
			crypto_actions_ipv4.crypto_encap.data_size = 24;
		}
		memset(crypto_actions_ipv4.crypto_encap.encap_data, 0xff, crypto_actions_ipv4.crypto_encap.data_size);
	}

	crypto_actions_ipv6 = crypto_actions_ipv4;
	if (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH ||
	    app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP) {
		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL) {
			crypto_actions_ipv6.crypto_encap.data_size = 70;
		} else if (app_cfg->mode == IPSEC_SECURITY_GW_TRANSPORT) {
			crypto_actions_ipv6.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_OVER_IPV6;
		} else {
			crypto_actions_ipv6.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_UDP_ESP_OVER_IPV6;
		}
		memset(crypto_actions_ipv6.crypto_encap.encap_data, 0xff, crypto_actions_ipv6.crypto_encap.data_size);
	}

	/* Create 2 ordered lists (sequences) - one for IPv4, one for IPv6 */
	/* IPv4 sequence elements */
	elements_ipv4[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements_ipv4[0].crypto = &crypto_actions_ipv4;

	ordered_list_ipv4.idx = ENCRYPT_IPV4_SEQ_IDX;
	ordered_list_ipv4.size = 1;
	ordered_list_ipv4.elements = elements_ipv4;
	ordered_lists[0] = &ordered_list_ipv4;

	/* IPv6 sequence elements */
	elements_ipv6[0].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	elements_ipv6[0].crypto = &crypto_actions_ipv6;

	ordered_list_ipv6.idx = ENCRYPT_IPV6_SEQ_IDX;
	ordered_list_ipv6.size = 1;
	ordered_list_ipv6.elements = elements_ipv6;
	ordered_lists[1] = &ordered_list_ipv6;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "ENCRYPT_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, nb_ordered_lists);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg ordered_lists: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (!app_cfg->vxlan_encap) {
		if (app_cfg->marker_encap && app_cfg->encrypt_pipes.marker_insert_pipe.pipe != NULL) {
			fwd.type = DOCA_FLOW_FWD_PIPE;
			fwd.next_pipe = app_cfg->encrypt_pipes.marker_insert_pipe.pipe;
		} else {
			fwd.type = DOCA_FLOW_FWD_PORT;
			fwd.port_id = port_id;
		}
	} else {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = app_cfg->encrypt_pipes.vxlan_encap_pipe.pipe;
	}

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encrypt ordered list pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create ipsec encrypt match pipe with changeable meta data match that forwards to ordered list pipe
 *
 * @port [in]: port of the pipe
 * @expected_entries [in]: expected number of entries
 * @app_cfg [in]: application configuration struct
 * @l3_type [in]: DOCA_FLOW_L3_META_IPV4 / DOCA_FLOW_L3_META_IPV6
 * @ordered_list_pipe [in]: ordered list pipe to forward to
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_encrypt_match_pipe(struct doca_flow_port *port,
						    int expected_entries,
						    struct ipsec_security_gw_config *app_cfg,
						    enum doca_flow_l3_meta l3_type,
						    struct doca_flow_pipe *ordered_list_pipe,
						    struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_match match;
	struct doca_flow_match match_mask;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};

	memset(&match, 0, sizeof(match));
	memset(&match_mask, 0, sizeof(match_mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	meta.rule_id = -1;
	match_mask.meta.pkt_meta = DOCA_HTOBE32(meta.u32);
	match.meta.pkt_meta = 0xffffffff;
	match_mask.parser_meta.outer_l3_type = l3_type;
	match.parser_meta.outer_l3_type = l3_type;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "ENCRYPT_MATCH_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (app_cfg->debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = ordered_list_pipe;
	fwd.ordered_list_pipe.idx = 0xffffffff; /* changeable per entry */

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encrypt match pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (app_cfg->debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(expected_entries,
								     sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create pipe with source ipv6 match, and fwd to the hairpin pipe
 *
 * @port [in]: port of the pipe
 * @debug_mode [in]: true if running in debug mode
 * @expected_entries [in]: expected number of entries
 * @protocol_type [in]: DOCA_FLOW_L4_TYPE_EXT_TCP / DOCA_FLOW_L4_TYPE_EXT_UDP
 * @hairpin_pipe [in]: pipe to forward the packets
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_src_ip6_pipe(struct doca_flow_port *port,
					      bool debug_mode,
					      int expected_entries,
					      enum doca_flow_l4_type_ext protocol_type,
					      struct doca_flow_pipe *hairpin_pipe,
					      struct security_gateway_pipe_info *pipe_info)
{
	int nb_actions = 1;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_actions actions, *actions_arr[nb_actions];
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&actions, 0, sizeof(actions));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
	match.parser_meta.outer_l4_type = (protocol_type == DOCA_FLOW_L4_TYPE_EXT_TCP) ? DOCA_FLOW_L4_META_TCP :
											 DOCA_FLOW_L4_META_UDP;
	SET_IP6_ADDR(match.outer.ip6.src_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);

	actions.meta.u32[0] = UINT32_MAX;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "SRC_IP6_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, nb_actions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = hairpin_pipe;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create hairpin pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(expected_entries,
								     sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create pipe with 5 tuple match, changeable set meta, and fwd to the second port
 *
 * @port [in]: port of the pipe
 * @debug_mode [in]: true if running in debug mode
 * @expected_entries [in]: expected number of entries
 * @protocol_type [in]: DOCA_FLOW_L4_TYPE_EXT_TCP / DOCA_FLOW_L4_TYPE_EXT_UDP
 * @l3_type [in]: DOCA_FLOW_L3_TYPE_IP4 / DOCA_FLOW_L3_TYPE_IP6
 * @fwd [in]: pointer to forward struct
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_hairpin_pipe(struct doca_flow_port *port,
					      bool debug_mode,
					      int expected_entries,
					      enum doca_flow_l4_type_ext protocol_type,
					      enum doca_flow_l3_type l3_type,
					      struct doca_flow_fwd *fwd,
					      struct security_gateway_pipe_info *pipe_info)
{
	int nb_actions = 1;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, *actions_arr[nb_actions];
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&actions, 0, sizeof(actions));

	match.outer.l4_type_ext = protocol_type;
	match.outer.l3_type = l3_type;
	if (l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		match.outer.ip4.dst_ip = 0xffffffff;
		match.outer.ip4.src_ip = 0xffffffff;
	} else {
		match.meta.u32[0] = UINT32_MAX;
		SET_IP6_ADDR(match.outer.ip6.dst_ip, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
	}

	SET_L4_PORT(outer, src_port, 0xffff);
	SET_L4_PORT(outer, dst_port, 0xffff);

	actions.meta.pkt_meta = 0xffffffff;
	actions_arr[0] = &actions;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "HAIRPIN_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, expected_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, nb_actions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	if (debug_mode) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	result = doca_flow_pipe_create(pipe_cfg, fwd, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create hairpin pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(expected_entries,
								     sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create control pipe for unsecured port
 *
 * @port [in]: port of the pipe
 * @is_root [in]: true in vnf mode
 * @debug_mode [in]: true if running in debug mode
 * @pipe_info [out]: pipe info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_control_pipe(struct doca_flow_port *port,
					bool is_root,
					bool debug_mode,
					struct security_gateway_pipe_info *pipe_info)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "CONTROL_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, is_root);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &pipe_info->pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create control pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (debug_mode) {
		pipe_info->entries_info =
			(struct security_gateway_entry_info *)calloc(4, sizeof(struct security_gateway_entry_info));
		if (pipe_info->entries_info == NULL) {
			DOCA_LOG_ERR("Failed to allocate entries array");
			result = DOCA_ERROR_NO_MEMORY;
		}
	}

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Check if BW perf is enabled
 *
 * @app_cfg [in]: application configuration struct
 * @return: true if BW perf is enabled.
 */
static inline bool is_perf_bw(struct ipsec_security_gw_config *app_cfg)
{
	return (app_cfg->perf_measurement == IPSEC_SECURITY_GW_PERF_BW ||
		app_cfg->perf_measurement == IPSEC_SECURITY_GW_PERF_BOTH);
}

/*
 * Add control pipe entries
 * - entry that forwards IPv4 TCP traffic to IPv4 TCP pipe,
 * - entry that forwards IPv4 UDP traffic to IPv4 UDP pipe,
 * - entry that forwards IPv6 TCP traffic to source IPv6 TCP pipe,
 * - entry that forwards IPv6 UDP traffic to source IPv6 UDP pipe,
 * - entry with lower priority that drop the packets
 *
 * @control_pipe [in]: control pipe pointer
 * @pipes [in]: all the pipes to forward the packets to
 * @perf_bw [in]: true if perf mode includes bandwidth
 * @debug_mode [in]: true if running in debug mode
 * @is_root [in]: true in vnf mode
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_control_pipe_entries(struct security_gateway_pipe_info *control_pipe,
					     struct encrypt_pipes *pipes,
					     bool perf_bw,
					     bool debug_mode,
					     bool is_root)
{
	struct doca_flow_pipe_entry **entry = NULL;
	struct doca_flow_monitor monitor;
	struct doca_flow_monitor *monitor_ptr = NULL;
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;

	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	if (debug_mode && !is_root) {
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		monitor_ptr = &monitor;
	}

	if (is_root) {
		match.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.outer.ip4.next_proto = DOCA_FLOW_PROTO_TCP;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->ipv4_tcp_pipe.pipe;
	if (debug_mode) {
		snprintf(control_pipe->entries_info[control_pipe->nb_entries].name, MAX_NAME_LEN, "ipv4_tcp");
		entry = &control_pipe->entries_info[control_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  control_pipe->pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor_ptr,
						  &fwd,
						  NULL,
						  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP IPv4 control entry: %s", doca_error_get_descr(result));
		return result;
	}

	memset(&match, 0, sizeof(match));
	if (is_root) {
		match.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV4);
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.outer.ip4.next_proto = DOCA_FLOW_PROTO_UDP;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipes->ipv4_udp_pipe.pipe;
	if (debug_mode) {
		snprintf(control_pipe->entries_info[control_pipe->nb_entries].name, MAX_NAME_LEN, "ipv4_udp");
		entry = &control_pipe->entries_info[control_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  control_pipe->pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor_ptr,
						  &fwd,
						  NULL,
						  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP IPv4 control entry: %s", doca_error_get_descr(result));
		return result;
	}
	memset(&match, 0, sizeof(match));
	if (is_root) {
		match.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV6);
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		match.outer.ip6.next_proto = DOCA_FLOW_PROTO_TCP;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	if (!perf_bw)
		fwd.next_pipe = pipes->ipv6_src_tcp_pipe.pipe;
	else
		fwd.next_pipe = pipes->ipv6_tcp_pipe.pipe;
	if (debug_mode) {
		snprintf(control_pipe->entries_info[control_pipe->nb_entries].name, MAX_NAME_LEN, "ipv6_tcp");
		entry = &control_pipe->entries_info[control_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  control_pipe->pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor_ptr,
						  &fwd,
						  NULL,
						  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add TCP IPv6 control entry: %s", doca_error_get_descr(result));
		return result;
	}

	memset(&match, 0, sizeof(match));
	if (is_root) {
		match.outer.eth.type = rte_cpu_to_be_16(DOCA_FLOW_ETHER_TYPE_IPV6);
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
		match.outer.ip6.next_proto = DOCA_FLOW_PROTO_UDP;
	} else {
		match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
		match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	}

	fwd.type = DOCA_FLOW_FWD_PIPE;
	if (!perf_bw)
		fwd.next_pipe = pipes->ipv6_src_udp_pipe.pipe;
	else
		fwd.next_pipe = pipes->ipv6_udp_pipe.pipe;
	if (debug_mode) {
		snprintf(control_pipe->entries_info[control_pipe->nb_entries].name, MAX_NAME_LEN, "ipv6_udp");
		entry = &control_pipe->entries_info[control_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_control_add_entry(0,
						  0,
						  control_pipe->pipe,
						  &match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  monitor_ptr,
						  &fwd,
						  NULL,
						  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add UDP IPv6 control entry: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Update the crypto config for encrypt transport mode according to the rule
 *
 * @crypto_cfg [in]: shared object config
 * @rule [in]: encrypt rule
 */
static void create_ipsec_encrypt_shared_object_transport(struct doca_flow_crypto_encap_action *crypto_cfg,
							 struct encrypt_rule *rule)
{
	create_transport_encap(rule, false, crypto_cfg->encap_data, &crypto_cfg->data_size);
}

/*
 * Update the crypto config for encrypt transport over UDP mode according to the rule
 *
 * @crypto_cfg [in]: shared object config
 * @rule [in]: encrypt rule
 */
static void create_ipsec_encrypt_shared_object_transport_over_udp(struct doca_flow_crypto_encap_action *crypto_cfg,
								  struct encrypt_rule *rule)
{
	create_udp_transport_encap(rule, false, crypto_cfg->encap_data, &crypto_cfg->data_size);
}

/*
 * Update the crypto config for encrypt tunnel mode according to the rule
 *
 * @crypto_cfg [in]: shared object config
 * @rule [in]: encrypt rule
 * @eth_header [in]: contains the src mac address
 */
static void create_ipsec_encrypt_shared_object_tunnel(struct doca_flow_crypto_encap_action *crypto_cfg,
						      struct encrypt_rule *rule,
						      struct doca_flow_header_eth *eth_header)
{
	if (rule->encap_l3_type == DOCA_FLOW_L3_TYPE_IP4)
		create_ipv4_tunnel_encap(rule, false, eth_header, crypto_cfg->encap_data, &crypto_cfg->data_size);
	else
		create_ipv6_tunnel_encap(rule, false, eth_header, crypto_cfg->encap_data, &crypto_cfg->data_size);
}

/*
 * Config and bind shared IPSEC object for encryption
 *
 * @app_sa_attrs [in]: SA attributes
 * @app_cfg [in]: application configuration struct
 * @ipsec_id [in]: shared object ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipsec_encrypt_shared_object(struct doca_flow_port *secured_port,
						       struct ipsec_security_gw_sa_attrs *app_sa_attrs,
						       struct ipsec_security_gw_config *app_cfg,
						       uint32_t ipsec_id)
{
	struct doca_flow_shared_resource_cfg cfg;
	doca_error_t result;

	memset(&cfg, 0, sizeof(cfg));

	cfg.ipsec_sa_cfg.icv_len = app_cfg->icv_length;
	cfg.ipsec_sa_cfg.salt = app_sa_attrs->salt;
	cfg.ipsec_sa_cfg.implicit_iv = app_sa_attrs->iv;
	cfg.ipsec_sa_cfg.key_cfg.key_type = app_sa_attrs->key_type;
	cfg.ipsec_sa_cfg.key_cfg.key = (void *)&app_sa_attrs->enc_key_data;
	cfg.ipsec_sa_cfg.sn_initial = app_cfg->sn_initial;
	cfg.ipsec_sa_cfg.esn_en = app_sa_attrs->esn_en;
	if (!app_cfg->sw_sn_inc_enable) {
		cfg.ipsec_sa_cfg.sn_offload_type = DOCA_FLOW_CRYPTO_SN_OFFLOAD_INC;
		cfg.ipsec_sa_cfg.lifetime_threshold = app_sa_attrs->lifetime_threshold;
	}
	/* config ipsec object */
	result = doca_flow_port_shared_resource_set_cfg(secured_port,
							DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA,
							ipsec_id,
							&cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cfg shared ipsec object: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Get the relevant pipe for adding the rule
 *
 * @rule [in]: the rule that need to add
 * @pipes [in]: encrypt pipes struct
 * @src_ip6 [in]: true if we want to get the source ipv6 pipe
 * @pipe [out]: output pipe
 */
static void get_pipe_for_rule(struct encrypt_rule *rule,
			      struct encrypt_pipes *pipes,
			      bool src_ip6,
			      struct security_gateway_pipe_info **pipe)
{
	if (!src_ip6) {
		if (rule->l3_type == DOCA_FLOW_L3_TYPE_IP4) {
			if (rule->protocol == DOCA_FLOW_L4_TYPE_EXT_TCP)
				*pipe = &pipes->ipv4_tcp_pipe;
			else
				*pipe = &pipes->ipv4_udp_pipe;
		} else {
			if (rule->protocol == DOCA_FLOW_L4_TYPE_EXT_TCP)
				*pipe = &pipes->ipv6_tcp_pipe;
			else
				*pipe = &pipes->ipv6_udp_pipe;
		}
	} else {
		if (rule->protocol == DOCA_FLOW_L4_TYPE_EXT_TCP)
			*pipe = &pipes->ipv6_src_tcp_pipe;
		else
			*pipe = &pipes->ipv6_src_udp_pipe;
	}
}

/*
 * Add entry to source IPv6 pipe
 *
 * @port [in]: port of the pipe
 * @rule [in]: encrypt rule
 * @pipes [in]: encrypt pipes struct
 * @hairpin_status [in]: the entries status
 * @src_ip_id [in]: source IP unique ID
 * @queue_id [in]: queue ID to forward the packets to
 * @debug_mode [in]: true when running in debug mode
 * @i [in]: rule id
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_src_ip6_entry(struct doca_flow_port *port,
				      struct encrypt_rule *rule,
				      struct encrypt_pipes *pipes,
				      struct entries_status *hairpin_status,
				      uint32_t src_ip_id,
				      uint16_t queue_id,
				      bool debug_mode,
				      int i)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	enum doca_flow_flags_type flags;
	struct security_gateway_pipe_info *pipe;
	struct doca_flow_pipe_entry **entry = NULL;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	get_pipe_for_rule(rule, pipes, true, &pipe);

	memcpy(match.outer.ip6.src_ip, rule->ip6.src_ip, sizeof(rule->ip6.src_ip));
	actions.meta.u32[0] = DOCA_HTOBE32(src_ip_id);

	if (hairpin_status->entries_in_queue == QUEUE_DEPTH - 1)
		flags = DOCA_FLOW_NO_WAIT;
	else
		flags = DOCA_FLOW_WAIT_FOR_BATCH;

	/* add entry to hairpin pipe*/
	if (debug_mode) {
		snprintf(pipe->entries_info[pipe->nb_entries].name, MAX_NAME_LEN, "rule%d", i);
		entry = &pipe->entries_info[pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(queue_id,
					  pipe->pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  NULL,
					  flags,
					  hairpin_status,
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add hairpin pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	hairpin_status->entries_in_queue++;
	if (hairpin_status->entries_in_queue == QUEUE_DEPTH) {
		result = process_entries(port, hairpin_status, DEFAULT_TIMEOUT_US, queue_id);
		if (result != DOCA_SUCCESS)
			return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Add 5-tuple entries based on a rule
 * If ipv6 - add the source IP to different pipe
 *
 * @port [in]: port of the pipe
 * @rule [in]: encrypt rule
 * @app_cfg [in]: application configuration structure
 * @nb_rules [in]: number of encryption rules
 * @i [in]: rule index
 * @queue_id [in]: queue ID to forward the packets to
 * @hairpin_status [in]: the entries status
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_five_tuple_match_entry(struct doca_flow_port *port,
					       struct encrypt_rule *rule,
					       struct ipsec_security_gw_config *app_cfg,
					       int nb_rules,
					       int i,
					       uint16_t queue_id,
					       struct entries_status *hairpin_status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	struct security_gateway_pipe_info *pipe;
	enum doca_flow_flags_type flags;
	int src_ip_id = 0;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	struct doca_flow_pipe_entry **entry = NULL;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	if (rule->l3_type == DOCA_FLOW_L3_TYPE_IP6 && !is_perf_bw(app_cfg)) {
		src_ip_id = rte_hash_lookup(app_cfg->ip6_table, (void *)&rule->ip6.dst_ip);
		if (src_ip_id < 0) {
			DOCA_LOG_ERR("Failed to find source IP in table");
			return DOCA_ERROR_NOT_FOUND;
		}
		result = add_src_ip6_entry(port,
					   rule,
					   &app_cfg->encrypt_pipes,
					   hairpin_status,
					   src_ip_id,
					   queue_id,
					   app_cfg->debug_mode,
					   i);
		if (result != DOCA_SUCCESS)
			return result;
	}

	get_pipe_for_rule(rule, &app_cfg->encrypt_pipes, false, &pipe);

	match.outer.l4_type_ext = rule->protocol;
	SET_L4_PORT(outer, src_port, rte_cpu_to_be_16(rule->src_port));
	SET_L4_PORT(outer, dst_port, rte_cpu_to_be_16(rule->dst_port));

	if (rule->l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		match.outer.ip4.dst_ip = rule->ip4.dst_ip;
		match.outer.ip4.src_ip = rule->ip4.src_ip;
	} else {
		match.meta.u32[0] = DOCA_HTOBE32(src_ip_id);
		memcpy(match.outer.ip6.dst_ip, rule->ip6.dst_ip, sizeof(rule->ip6.dst_ip));
	}

	meta.encrypt = 1;
	meta.rule_id = i;
	actions.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	if (i == nb_rules - 1 || hairpin_status->entries_in_queue == QUEUE_DEPTH - 1)
		flags = DOCA_FLOW_NO_WAIT;
	else
		flags = DOCA_FLOW_WAIT_FOR_BATCH;

	/* add entry to hairpin pipe*/
	if (app_cfg->debug_mode) {
		snprintf(pipe->entries_info[pipe->nb_entries].name, MAX_NAME_LEN, "rule%d", i);
		entry = &pipe->entries_info[pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(queue_id,
					  pipe->pipe,
					  &match,
					  0,
					  &actions,
					  NULL,
					  NULL,
					  flags,
					  hairpin_status,
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add hairpin pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	hairpin_status->entries_in_queue++;
	if (hairpin_status->entries_in_queue == QUEUE_DEPTH) {
		result = process_entries(port, hairpin_status, DEFAULT_TIMEOUT_US, queue_id);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t add_encrypt_entry(struct encrypt_rule *rule,
			       int rule_id,
			       struct ipsec_security_gw_ports_map **ports,
			       struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_entry **entry = NULL;
	struct security_gateway_pipe_info *match_encrypt_pipe;
	struct doca_flow_port *secured_port = NULL;
	struct doca_flow_port *unsecured_port = NULL;
	union security_gateway_pkt_meta meta = {0};
	struct doca_flow_crypto crypto_actions = {};
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element element = {0};
	struct doca_flow_fwd fwd_to_ordered_list = {};
	doca_error_t result;
	uint32_t ipsec_id = IPSEC_ID(rule_id);
	uint32_t sequence_id = ENCRYPT_IPV4_SEQ_IDX;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		secured_port = doca_flow_port_switch_get(NULL);
		unsecured_port = doca_flow_port_switch_get(NULL);
	} else {
		secured_port = ports[SECURED_IDX]->port;
		unsecured_port = ports[UNSECURED_IDX]->port;
	}

	memset(&app_cfg->unsecured_status[0], 0, sizeof(app_cfg->unsecured_status[0]));
	memset(&app_cfg->secured_status[0], 0, sizeof(app_cfg->secured_status[0]));
	memset(&match, 0, sizeof(match));

	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL && (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_NONE ||
							  app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP)) {
		/* SW encap in tunnel mode */
		if (rule->encap_l3_type == DOCA_FLOW_L3_TYPE_IP4)
			match_encrypt_pipe = &app_cfg->encrypt_pipes.match_encrypt_ipv4_pipe;
		else
			match_encrypt_pipe = &app_cfg->encrypt_pipes.match_encrypt_ipv6_pipe;
	} else {
		if (rule->l3_type == DOCA_FLOW_L3_TYPE_IP4)
			match_encrypt_pipe = &app_cfg->encrypt_pipes.match_encrypt_ipv4_pipe;
		else
			match_encrypt_pipe = &app_cfg->encrypt_pipes.match_encrypt_ipv6_pipe;
	}
	/* add entry to hairpin pipe*/
	result = add_five_tuple_match_entry(unsecured_port,
					    rule,
					    app_cfg,
					    rule_id + 1,
					    rule_id,
					    0,
					    &app_cfg->unsecured_status[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	/* create ipsec shared object */
	result = create_ipsec_encrypt_shared_object(secured_port, &rule->sa_attrs, app_cfg, ipsec_id);
	if (result != DOCA_SUCCESS)
		return result;

	/* Add entry to ordered list pipe */
	crypto_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
	crypto_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
	crypto_actions.crypto.crypto_id = ipsec_id;
	if (!app_cfg->sw_sn_inc_enable) {
		crypto_actions.crypto.ipsec_sa.sn_en = !app_cfg->sw_sn_inc_enable;
	}

	if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL) {
		create_ipsec_encrypt_shared_object_tunnel(&crypto_actions.crypto_encap,
							  rule,
							  &ports[SECURED_IDX]->eth_header);
		sequence_id = rule->encap_l3_type == DOCA_FLOW_L3_TYPE_IP4 ? ENCRYPT_IPV4_SEQ_IDX :
									     ENCRYPT_IPV6_SEQ_IDX;
	} else if (app_cfg->mode == IPSEC_SECURITY_GW_TRANSPORT) {
		sequence_id = rule->l3_type == DOCA_FLOW_L3_TYPE_IP4 ? ENCRYPT_IPV4_SEQ_IDX : ENCRYPT_IPV6_SEQ_IDX;
		create_ipsec_encrypt_shared_object_transport(&crypto_actions.crypto_encap, rule);
	} else {
		sequence_id = rule->l3_type == DOCA_FLOW_L3_TYPE_IP4 ? ENCRYPT_IPV4_SEQ_IDX : ENCRYPT_IPV6_SEQ_IDX;
		create_ipsec_encrypt_shared_object_transport_over_udp(&crypto_actions.crypto_encap, rule);
	}

	element.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
	element.crypto = &crypto_actions;

	ordered_list.idx = sequence_id;
	ordered_list.size = 1;
	ordered_list.elements = &element;

	result = doca_flow_pipe_ordered_list_add_entry(0,
						       app_cfg->encrypt_pipes.encrypt_pipe.pipe,
						       rule_id,
						       &ordered_list,
						       NULL,
						       DOCA_FLOW_WAIT_FOR_BATCH,
						       &app_cfg->secured_status[0],
						       NULL);
	if (result != DOCA_SUCCESS)
		return result;
	app_cfg->secured_status[0].entries_in_queue += 1;

	/* Add entry to match pipe */
	meta.rule_id = rule_id;
	match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

	fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd_to_ordered_list.ordered_list_pipe.pipe = app_cfg->encrypt_pipes.encrypt_pipe.pipe;
	fwd_to_ordered_list.ordered_list_pipe.idx = rule_id;

	/* add entry to encrypt match pipe*/
	if (app_cfg->debug_mode) {
		snprintf(match_encrypt_pipe->entries_info[match_encrypt_pipe->nb_entries].name,
			 MAX_NAME_LEN,
			 "rule%d",
			 rule_id);
		entry = &match_encrypt_pipe->entries_info[match_encrypt_pipe->nb_entries++].entry;
	}
	result = doca_flow_pipe_add_entry(0,
					  match_encrypt_pipe->pipe,
					  &match,
					  0,
					  NULL,
					  NULL,
					  &fwd_to_ordered_list,
					  DOCA_FLOW_NO_WAIT,
					  &app_cfg->secured_status[0],
					  entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
		return result;
	}
	app_cfg->secured_status[0].entries_in_queue++;

	/* process the entries in the encryption pipe*/
	do {
		result = process_entries(secured_port, &app_cfg->secured_status[0], DEFAULT_TIMEOUT_US, 0);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->secured_status[0].entries_in_queue > 0);

	/* process the entries in the 5 tuple match pipes */
	do {
		result = process_entries(unsecured_port, &app_cfg->unsecured_status[0], DEFAULT_TIMEOUT_US, 0);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->unsecured_status[0].entries_in_queue > 0);
	return DOCA_SUCCESS;
}

doca_error_t get_encrypt_ids(int nb_rules, struct doca_flow_port *port)
{
	doca_error_t result;
	int i, array_len = nb_rules;

	if (array_len == 0)
		return DOCA_SUCCESS;

	for (i = 0; i < nb_rules; i++) {
		result = doca_flow_port_shared_resource_get(port, DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, &IPSEC_ID(i));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate encrypt IDs to the port");
			return result;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t add_encrypt_entries(struct ipsec_security_gw_config *app_cfg,
				 struct ipsec_security_gw_ports_map *ports[],
				 uint16_t queue_id,
				 int nb_rules,
				 int rule_offset)
{
	struct doca_flow_match match;
	struct doca_flow_pipe_entry **entry = NULL;
	struct security_gateway_pipe_info *match_encrypt_pipe;
	enum doca_flow_flags_type flags;
	struct doca_flow_port *secured_port = NULL;
	struct doca_flow_port *unsecured_port = NULL;
	struct doca_flow_crypto crypto_actions = {};
	struct doca_flow_ordered_list ordered_list = {};
	struct doca_flow_ordered_list_element element = {0};
	struct doca_flow_fwd fwd_to_ordered_list = {};
	int i, rule_id;
	doca_error_t result;
	union security_gateway_pkt_meta meta = {0};
	struct encrypt_rule *rules = app_cfg->app_rules.encrypt_rules;
	struct encrypt_pipes *pipes = &app_cfg->encrypt_pipes;
	uint32_t ipsec_id;
	uint32_t sequence_id = ENCRYPT_IPV4_SEQ_IDX;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		secured_port = doca_flow_port_switch_get(NULL);
		unsecured_port = doca_flow_port_switch_get(NULL);
	} else {
		secured_port = ports[SECURED_IDX]->port;
		unsecured_port = ports[UNSECURED_IDX]->port;
	}

	memset(&app_cfg->secured_status[queue_id], 0, sizeof(app_cfg->secured_status[queue_id]));
	memset(&app_cfg->unsecured_status[queue_id], 0, sizeof(app_cfg->unsecured_status[queue_id]));
	memset(&match, 0, sizeof(match));

	for (i = 0; i < nb_rules; i++) {
		rule_id = rule_offset + i;
		ipsec_id = IPSEC_ID(rule_id);
		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL &&
		    (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_NONE ||
		     app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP)) {
			/* SW encap in tunnel mode */
			if (rules[rule_id].encap_l3_type == DOCA_FLOW_L3_TYPE_IP4)
				match_encrypt_pipe = &pipes->match_encrypt_ipv4_pipe;
			else
				match_encrypt_pipe = &pipes->match_encrypt_ipv6_pipe;
		} else {
			if (rules[rule_id].l3_type == DOCA_FLOW_L3_TYPE_IP4)
				match_encrypt_pipe = &pipes->match_encrypt_ipv4_pipe;
			else
				match_encrypt_pipe = &pipes->match_encrypt_ipv6_pipe;
		}
		/* add entry to hairpin pipe*/
		result = add_five_tuple_match_entry(unsecured_port,
						    &rules[rule_id],
						    app_cfg,
						    nb_rules,
						    rule_id,
						    queue_id,
						    &app_cfg->unsecured_status[queue_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}

		/* create ipsec shared object */
		result = create_ipsec_encrypt_shared_object(secured_port, &rules[rule_id].sa_attrs, app_cfg, ipsec_id);
		if (result != DOCA_SUCCESS)
			return result;

		/* Add entry to ordered list pipe */
		crypto_actions.crypto.crypto_id = ipsec_id;

		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL) {
			sequence_id = rules[rule_id].encap_l3_type == DOCA_FLOW_L3_TYPE_IP4 ? ENCRYPT_IPV4_SEQ_IDX :
											      ENCRYPT_IPV6_SEQ_IDX;
			create_ipsec_encrypt_shared_object_tunnel(&crypto_actions.crypto_encap,
								  &rules[rule_id],
								  &ports[SECURED_IDX]->eth_header);
		} else if (app_cfg->mode == IPSEC_SECURITY_GW_TRANSPORT) {
			sequence_id = rules[rule_id].l3_type == DOCA_FLOW_L3_TYPE_IP4 ? ENCRYPT_IPV4_SEQ_IDX :
											ENCRYPT_IPV6_SEQ_IDX;
			create_ipsec_encrypt_shared_object_transport(&crypto_actions.crypto_encap, &rules[rule_id]);
		} else {
			sequence_id = rules[rule_id].l3_type == DOCA_FLOW_L3_TYPE_IP4 ? ENCRYPT_IPV4_SEQ_IDX :
											ENCRYPT_IPV6_SEQ_IDX;
			create_ipsec_encrypt_shared_object_transport_over_udp(&crypto_actions.crypto_encap,
									      &rules[rule_id]);
		}

		element.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_CRYPTO;
		element.crypto = &crypto_actions;

		ordered_list.idx = sequence_id;
		ordered_list.size = 1;
		ordered_list.elements = &element;

		result = doca_flow_pipe_ordered_list_add_entry(queue_id,
							       pipes->encrypt_pipe.pipe,
							       rule_id,
							       &ordered_list,
							       NULL,
							       DOCA_FLOW_WAIT_FOR_BATCH,
							       &app_cfg->secured_status[queue_id],
							       NULL);
		if (result != DOCA_SUCCESS)
			return result;
		app_cfg->secured_status[queue_id].entries_in_queue += 1;

		/* Add entry to match pipe */
		memset(&match, 0, sizeof(match));
		meta.rule_id = rule_id;
		match.meta.pkt_meta = DOCA_HTOBE32(meta.u32);

		fwd_to_ordered_list.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
		fwd_to_ordered_list.ordered_list_pipe.pipe = pipes->encrypt_pipe.pipe;
		fwd_to_ordered_list.ordered_list_pipe.idx = rule_id;

		if (rule_id == nb_rules - 1 || app_cfg->secured_status[queue_id].entries_in_queue == QUEUE_DEPTH - 1)
			flags = DOCA_FLOW_NO_WAIT;
		else
			flags = DOCA_FLOW_WAIT_FOR_BATCH;
		/* add entry to encrypt match pipe*/
		if (app_cfg->debug_mode) {
			snprintf(match_encrypt_pipe->entries_info[match_encrypt_pipe->nb_entries].name,
				 MAX_NAME_LEN,
				 "rule%d",
				 i);
			entry = &match_encrypt_pipe->entries_info[match_encrypt_pipe->nb_entries++].entry;
		}
		result = doca_flow_pipe_add_entry(queue_id,
						  match_encrypt_pipe->pipe,
						  &match,
						  0,
						  NULL,
						  NULL,
						  &fwd_to_ordered_list,
						  flags,
						  &app_cfg->secured_status[queue_id],
						  entry);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
		app_cfg->secured_status[queue_id].entries_in_queue++;
		if (app_cfg->secured_status[queue_id].entries_in_queue == QUEUE_DEPTH) {
			result = process_entries(secured_port,
						 &app_cfg->secured_status[queue_id],
						 DEFAULT_TIMEOUT_US,
						 queue_id);
			if (result != DOCA_SUCCESS)
				return result;
		}
	}
	/* process the entries in the encryption pipe*/
	do {
		result =
			process_entries(secured_port, &app_cfg->secured_status[queue_id], DEFAULT_TIMEOUT_US, queue_id);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->secured_status[queue_id].entries_in_queue > 0);

	/* process the entries in the 5 tuple match pipes */
	do {
		result = process_entries(unsecured_port,
					 &app_cfg->unsecured_status[queue_id],
					 DEFAULT_TIMEOUT_US,
					 queue_id);
		if (result != DOCA_SUCCESS)
			return result;
	} while (app_cfg->unsecured_status[queue_id].entries_in_queue > 0);
	return DOCA_SUCCESS;
}

doca_error_t ipsec_security_gw_create_encrypt_egress(struct ipsec_security_gw_ports_map *ports[],
						     struct ipsec_security_gw_config *app_cfg)
{
	struct doca_flow_port *secured_port = NULL;
	bool is_root;
	doca_error_t result;
	int expected_entries;

	if (app_cfg->socket_ctx.socket_conf)
		expected_entries = MAX_NB_RULES;
	else if (app_cfg->app_rules.nb_encrypt_rules > 0)
		expected_entries = app_cfg->app_rules.nb_encrypt_rules;
	else /* default value - no entries expected so putting a default value so that pipe creation won't fail */
		expected_entries = DEF_EXPECTED_ENTRIES;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		secured_port = ports[SECURED_IDX]->port;
		is_root = true;
	} else {
		secured_port = doca_flow_port_switch_get(NULL);
		is_root = false;
	}

	if (app_cfg->vxlan_encap) {
		if (app_cfg->marker_encap) {
			DOCA_LOG_ERR("Non-ESP marker is not supported over VXLAN encapsulation");
			return DOCA_ERROR_NOT_SUPPORTED;
		}

		snprintf(app_cfg->encrypt_pipes.vxlan_encap_pipe.name, MAX_NAME_LEN, "vxlan_encap");
		result = create_vxlan_encap_pipe(secured_port, ports[SECURED_IDX]->port_id, app_cfg);
		if (result != DOCA_SUCCESS)
			return result;
	} else if (app_cfg->marker_encap) {
		result = create_marker_encap_pipe(secured_port, ports[SECURED_IDX]->port_id, app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create non-ESP marker egress pipe: %s", doca_error_get_descr(result));
			return result;
		}
	}

	snprintf(app_cfg->encrypt_pipes.encrypt_pipe.name, MAX_NAME_LEN, "encrypt");
	result = create_ipsec_encrypt_pipe(secured_port,
					   ports[SECURED_IDX]->port_id,
					   expected_entries,
					   app_cfg,
					   &app_cfg->encrypt_pipes.encrypt_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	snprintf(app_cfg->encrypt_pipes.match_encrypt_ipv4_pipe.name, MAX_NAME_LEN, "IPv4_encrypt_match");
	result = create_ipsec_encrypt_match_pipe(secured_port,
						 expected_entries,
						 app_cfg,
						 DOCA_FLOW_L3_META_IPV4,
						 app_cfg->encrypt_pipes.encrypt_pipe.pipe,
						 &app_cfg->encrypt_pipes.match_encrypt_ipv4_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	snprintf(app_cfg->encrypt_pipes.match_encrypt_ipv6_pipe.name, MAX_NAME_LEN, "IPv6_encrypt_match");
	result = create_ipsec_encrypt_match_pipe(secured_port,
						 expected_entries,
						 app_cfg,
						 DOCA_FLOW_L3_META_IPV6,
						 app_cfg->encrypt_pipes.encrypt_pipe.pipe,
						 &app_cfg->encrypt_pipes.match_encrypt_ipv6_pipe);
	if (result != DOCA_SUCCESS)
		return result;

	snprintf(app_cfg->encrypt_pipes.egress_ip_classifier.name, MAX_NAME_LEN, "ip_classifier");
	result = create_egress_ip_classifier(secured_port,
					     is_root,
					     app_cfg->debug_mode,
					     &app_cfg->encrypt_pipes,
					     app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

doca_error_t ipsec_security_gw_insert_encrypt_rules(struct ipsec_security_gw_ports_map *ports[],
						    struct ipsec_security_gw_config *app_cfg)
{
	uint32_t nb_queues = app_cfg->dpdk_config->port_config.nb_queues;
	uint16_t rss_queues[nb_queues];
	uint32_t rss_flags;
	struct doca_flow_port *unsecured_port = NULL;
	struct doca_flow_fwd fwd;
	bool is_root;
	bool perf_bw;
	doca_error_t result;
	int expected_entries;

	if (app_cfg->socket_ctx.socket_conf)
		expected_entries = MAX_NB_RULES;
	else if (app_cfg->app_rules.nb_encrypt_rules > 0)
		expected_entries = app_cfg->app_rules.nb_encrypt_rules;
	else /* default value - no entries expected so putting a default value so that pipe creation won't fail */
		expected_entries = DEF_EXPECTED_ENTRIES;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		unsecured_port = ports[UNSECURED_IDX]->port;
		is_root = true;
	} else {
		unsecured_port = doca_flow_port_switch_get(NULL);
		is_root = false;
	}

	rss_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP;
	create_hairpin_pipe_fwd(app_cfg, ports[UNSECURED_IDX]->port_id, true, rss_queues, rss_flags, &fwd);

	DOCA_LOG_DBG("Creating IPv4 TCP hairpin pipe");
	snprintf(app_cfg->encrypt_pipes.ipv4_tcp_pipe.name, MAX_NAME_LEN, "IPv4_tcp_hairpin");
	result = create_ipsec_hairpin_pipe(unsecured_port,
					   app_cfg->debug_mode,
					   expected_entries,
					   DOCA_FLOW_L4_TYPE_EXT_TCP,
					   DOCA_FLOW_L3_TYPE_IP4,
					   &fwd,
					   &app_cfg->encrypt_pipes.ipv4_tcp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed create IPv4 TCP hairpin pipe");
		return result;
	}

	DOCA_LOG_DBG("Creating IPv4 UDP hairpin pipe");
	snprintf(app_cfg->encrypt_pipes.ipv4_udp_pipe.name, MAX_NAME_LEN, "IPv4_udp_hairpin");
	rss_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	create_hairpin_pipe_fwd(app_cfg, ports[UNSECURED_IDX]->port_id, true, rss_queues, rss_flags, &fwd);

	result = create_ipsec_hairpin_pipe(unsecured_port,
					   app_cfg->debug_mode,
					   expected_entries,
					   DOCA_FLOW_L4_TYPE_EXT_UDP,
					   DOCA_FLOW_L3_TYPE_IP4,
					   &fwd,
					   &app_cfg->encrypt_pipes.ipv4_udp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed create IPv4 UDP hairpin pipe");
		return result;
	}

	DOCA_LOG_DBG("Creating IPv6 TCP hairpin pipe");
	snprintf(app_cfg->encrypt_pipes.ipv6_tcp_pipe.name, MAX_NAME_LEN, "IPv6_tcp_hairpin");
	rss_flags = DOCA_FLOW_RSS_IPV6 | DOCA_FLOW_RSS_TCP;
	create_hairpin_pipe_fwd(app_cfg, ports[UNSECURED_IDX]->port_id, true, rss_queues, rss_flags, &fwd);

	result = create_ipsec_hairpin_pipe(unsecured_port,
					   app_cfg->debug_mode,
					   expected_entries,
					   DOCA_FLOW_L4_TYPE_EXT_TCP,
					   DOCA_FLOW_L3_TYPE_IP6,
					   &fwd,
					   &app_cfg->encrypt_pipes.ipv6_tcp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed create IPv6 TCP hairpin pipe");
		return result;
	}

	DOCA_LOG_DBG("Creating IPv6 UDP hairpin pipe");
	snprintf(app_cfg->encrypt_pipes.ipv6_udp_pipe.name, MAX_NAME_LEN, "IPv6_udp_hairpin");
	rss_flags = DOCA_FLOW_RSS_IPV6 | DOCA_FLOW_RSS_UDP;
	create_hairpin_pipe_fwd(app_cfg, ports[UNSECURED_IDX]->port_id, true, rss_queues, rss_flags, &fwd);

	result = create_ipsec_hairpin_pipe(unsecured_port,
					   app_cfg->debug_mode,
					   expected_entries,
					   DOCA_FLOW_L4_TYPE_EXT_UDP,
					   DOCA_FLOW_L3_TYPE_IP6,
					   &fwd,
					   &app_cfg->encrypt_pipes.ipv6_udp_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed create IPv6 UDP hairpin pipe");
		return result;
	}

	perf_bw = is_perf_bw(app_cfg);
	if (!perf_bw) {
		DOCA_LOG_DBG("Creating source IPv6 TCP hairpin pipe");
		snprintf(app_cfg->encrypt_pipes.ipv6_src_tcp_pipe.name, MAX_NAME_LEN, "IPv6_src_tcp");
		result = create_ipsec_src_ip6_pipe(unsecured_port,
						   app_cfg->debug_mode,
						   expected_entries,
						   DOCA_FLOW_L4_TYPE_EXT_TCP,
						   app_cfg->encrypt_pipes.ipv6_tcp_pipe.pipe,
						   &app_cfg->encrypt_pipes.ipv6_src_tcp_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create source ip6 TCP hairpin pipe");
			return result;
		}

		DOCA_LOG_DBG("Creating source IPv6 UDP hairpin pipe");
		snprintf(app_cfg->encrypt_pipes.ipv6_src_udp_pipe.name, MAX_NAME_LEN, "IPv6_src_udp");
		result = create_ipsec_src_ip6_pipe(unsecured_port,
						   app_cfg->debug_mode,
						   expected_entries,
						   DOCA_FLOW_L4_TYPE_EXT_UDP,
						   app_cfg->encrypt_pipes.ipv6_udp_pipe.pipe,
						   &app_cfg->encrypt_pipes.ipv6_src_udp_pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed create source ip6 UDP hairpin pipe");
			return result;
		}
	}
	DOCA_LOG_DBG("Creating control pipe");
	snprintf(app_cfg->encrypt_pipes.encrypt_root.name, MAX_NAME_LEN, "encrypt_root");
	result =
		create_control_pipe(unsecured_port, is_root, app_cfg->debug_mode, &app_cfg->encrypt_pipes.encrypt_root);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_DBG("Adding control entries");
	result = add_control_pipe_entries(&app_cfg->encrypt_pipes.encrypt_root,
					  &app_cfg->encrypt_pipes,
					  perf_bw,
					  app_cfg->debug_mode,
					  is_root);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Update mbuf with the new headers and trailer data for tunnel mode
 *
 * @m [in]: the mbuf to update
 * @ctx [in]: the security gateway context
 * @rule_idx [in]: the index of the rule to use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_packet_tunnel(struct rte_mbuf **m,
					  struct ipsec_security_gw_core_ctx *ctx,
					  uint32_t rule_idx)
{
	struct rte_ether_hdr *nh;
	struct rte_esp_tail *esp_tail;
	struct rte_ipv4_hdr *ipv4;
	struct rte_ipv6_hdr *ipv6;
	struct rte_mbuf *last_seg;
	struct encrypt_rule *rule = &ctx->encrypt_rules[rule_idx];
	uint32_t icv_len = get_icv_len_int(ctx->config->icv_length);
	bool sw_sn_inc = ctx->config->sw_sn_inc_enable;
	void *trailer_pointer;
	uint32_t payload_len, esp_len, encrypted_len, padding_len, trailer_len, padding_offset;
	uint16_t reformat_encap_data_len;

	if (rule->encap_l3_type == DOCA_FLOW_L3_TYPE_IP4)
		reformat_encap_data_len = 50;
	else
		reformat_encap_data_len = 70;

	/* remove trailing zeros */
	remove_ethernet_padding(m);

	/* in tunnel mode need to encrypt everything beside the eth header */
	payload_len = (*m)->pkt_len - sizeof(struct rte_ether_hdr);
	/* extra header space required */
	esp_len = reformat_encap_data_len - sizeof(struct rte_ether_hdr);

	encrypted_len = payload_len + (sizeof(struct rte_esp_tail));
	/* align payload to 4 bytes */
	encrypted_len = RTE_ALIGN_CEIL(encrypted_len, PADDING_ALIGN);

	padding_len = encrypted_len - payload_len;
	/* extra trailer space is required */
	trailer_len = padding_len + icv_len;

	/* append the needed space at the beginning of the packet */
	nh = (struct rte_ether_hdr *)(void *)rte_pktmbuf_prepend(*m, esp_len);
	if (nh == NULL)
		return DOCA_ERROR_NO_MEMORY;

	last_seg = rte_pktmbuf_lastseg(*m);

	/* append tail */
	padding_offset = last_seg->data_len;
	last_seg->data_len += trailer_len;
	(*m)->pkt_len += trailer_len;
	trailer_pointer = rte_pktmbuf_mtod_offset(last_seg, typeof(trailer_pointer), padding_offset);

	/* add the new IP and ESP headers */
	if (rule->encap_l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		create_ipv4_tunnel_encap(rule,
					 sw_sn_inc,
					 &ctx->ports[SECURED_IDX]->eth_header,
					 (void *)nh,
					 &reformat_encap_data_len);
		ipv4 = (void *)(nh + 1);
		ipv4->total_length = rte_cpu_to_be_16((*m)->pkt_len - sizeof(struct rte_ether_hdr));
		ipv4->hdr_checksum = 0;
		ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
	} else {
		create_ipv6_tunnel_encap(rule,
					 sw_sn_inc,
					 &ctx->ports[SECURED_IDX]->eth_header,
					 (void *)nh,
					 &reformat_encap_data_len);
		ipv6 = (void *)(nh + 1);
		ipv6->payload_len = rte_cpu_to_be_16((*m)->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(*ipv6));
	}

	padding_len -= sizeof(struct rte_esp_tail);

	/* add padding (if needed) */
	if (padding_len > 0)
		memcpy(trailer_pointer, esp_pad_bytes, RTE_MIN(padding_len, sizeof(esp_pad_bytes)));

	esp_tail = (struct rte_esp_tail *)(trailer_pointer + padding_len);
	esp_tail->pad_len = padding_len;
	/* set the next proto according to the original packet */
	if (rule->l3_type == DOCA_FLOW_L3_TYPE_IP4)
		esp_tail->next_proto = 4; /* ipv4 */
	else
		esp_tail->next_proto = 41; /* ipv6 */

	ctx->encrypt_rules[rule_idx].current_sn++;
	return DOCA_SUCCESS;
}

/*
 * Update mbuf with the new headers and trailer data for transport and udp transport mode
 *
 * @m [in]: the mbuf to update
 * @ctx [in]: the security gateway context
 * @rule_idx [in]: the index of the rule to use
 * @udp_transport [in]: true for UDP transport mode
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_packet_transport(struct rte_mbuf **m,
					     struct ipsec_security_gw_core_ctx *ctx,
					     uint32_t rule_idx,
					     bool udp_transport)
{
	struct rte_ether_hdr *oh, *nh;
	struct rte_esp_tail *esp_tail;
	struct rte_ipv4_hdr *ipv4;
	struct rte_ipv6_hdr *ipv6;
	struct rte_mbuf *last_seg;
	struct encrypt_rule *rule = &ctx->encrypt_rules[rule_idx];
	uint32_t icv_len = get_icv_len_int(ctx->config->icv_length);
	void *trailer_pointer;
	uint32_t payload_len, esp_len, encrypted_len, padding_len, trailer_len, padding_offset, l2_l3_len;
	uint16_t reformat_encap_data_len;
	int protocol, next_protocol = 0;
	bool sw_sn_inc = ctx->config->sw_sn_inc_enable;

	if (udp_transport) {
		reformat_encap_data_len = 24;
		protocol = IPPROTO_UDP;
	} else {
		reformat_encap_data_len = 16;
		protocol = IPPROTO_ESP;
	}

	/* remove trailing zeros */
	remove_ethernet_padding(m);

	/* get l2 and l3 headers length */
	oh = rte_pktmbuf_mtod(*m, struct rte_ether_hdr *);

	if (RTE_ETH_IS_IPV4_HDR((*m)->packet_type)) {
		ipv4 = (void *)(oh + 1);
		l2_l3_len = rte_ipv4_hdr_len(ipv4) + sizeof(struct rte_ether_hdr);
	} else
		l2_l3_len = sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_ether_hdr);

	/* in transport mode need to encrypt everything beside l2 and l3 headers*/
	payload_len = (*m)->pkt_len - l2_l3_len;
	/* extra header space required */
	esp_len = reformat_encap_data_len;

	encrypted_len = payload_len + (sizeof(struct rte_esp_tail));
	/* align payload to 4 bytes */
	encrypted_len = RTE_ALIGN_CEIL(encrypted_len, PADDING_ALIGN);

	padding_len = encrypted_len - payload_len;
	/* extra trailer space is required */
	trailer_len = padding_len + icv_len;

	nh = (struct rte_ether_hdr *)(void *)rte_pktmbuf_prepend(*m, esp_len);
	if (nh == NULL)
		return DOCA_ERROR_NO_MEMORY;

	last_seg = rte_pktmbuf_lastseg(*m);

	/* append tail */
	padding_offset = last_seg->data_len;
	last_seg->data_len += trailer_len;
	(*m)->pkt_len += trailer_len;
	trailer_pointer = rte_pktmbuf_mtod_offset(last_seg, typeof(trailer_pointer), padding_offset);

	/* move l2 and l3 to beginning of packet, and copy ESP header after */
	memmove(nh, oh, l2_l3_len);
	if (udp_transport)
		create_udp_transport_encap(rule, sw_sn_inc, ((void *)nh) + l2_l3_len, &reformat_encap_data_len);
	else
		create_transport_encap(rule, sw_sn_inc, ((void *)nh) + l2_l3_len, &reformat_encap_data_len);

	/* update next protocol to ESP/UDP, total length and checksum */
	if (RTE_ETH_IS_IPV4_HDR((*m)->packet_type)) {
		ipv4 = (void *)(nh + 1);
		next_protocol = ipv4->next_proto_id;
		ipv4->next_proto_id = protocol;
		ipv4->total_length = rte_cpu_to_be_16((*m)->pkt_len - sizeof(struct rte_ether_hdr));
		ipv4->hdr_checksum = 0;
		ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
		if (udp_transport) {
			struct rte_udp_hdr *udp = (void *)(ipv4 + 1);
			uint16_t udp_len = (*m)->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr);

			udp->dgram_len = rte_cpu_to_be_16(udp_len);
			udp->dgram_cksum = RTE_BE16(0);
		}
	} else if (RTE_ETH_IS_IPV6_HDR((*m)->packet_type)) {
		ipv6 = (void *)(nh + 1);
		next_protocol = ipv6->proto;
		ipv6->proto = protocol;
		ipv6->payload_len = rte_cpu_to_be_16((*m)->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(*ipv6));
		if (udp_transport) {
			struct rte_udp_hdr *udp = (void *)(ipv6 + 1);
			uint16_t udp_len = (*m)->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv6_hdr);

			udp->dgram_len = rte_cpu_to_be_16(udp_len);
			udp->dgram_cksum = RTE_BE16(0);
		}
	}

	padding_len -= sizeof(struct rte_esp_tail);

	/* add padding (if needed) */
	if (padding_len > 0)
		memcpy(trailer_pointer, esp_pad_bytes, RTE_MIN(padding_len, sizeof(esp_pad_bytes)));

	/* set the next proto according to the original packet */
	esp_tail = (struct rte_esp_tail *)(trailer_pointer + padding_len);
	esp_tail->pad_len = padding_len;
	esp_tail->next_proto = next_protocol;

	ctx->encrypt_rules[rule_idx].current_sn++;
	return DOCA_SUCCESS;
}

doca_error_t handle_unsecured_packets_received(struct rte_mbuf **packet, struct ipsec_security_gw_core_ctx *ctx)
{
	uint32_t pkt_meta;
	uint32_t rule_idx;
	doca_error_t result;

	pkt_meta = *RTE_FLOW_DYNF_METADATA(*packet);
	rule_idx = ((union security_gateway_pkt_meta)pkt_meta).rule_id;

	if (ctx->config->mode == IPSEC_SECURITY_GW_TRANSPORT)
		result = prepare_packet_transport(packet, ctx, rule_idx, false);
	else if (ctx->config->mode == IPSEC_SECURITY_GW_UDP_TRANSPORT)
		result = prepare_packet_transport(packet, ctx, rule_idx, true);
	else
		result = prepare_packet_tunnel(packet, ctx, rule_idx);
	if (result != DOCA_SUCCESS)
		return result;
	/* All SAs are created over the secured port, hence we should access them accordingly */
	return doca_flow_port_crypto_ipsec_update_sn(ctx->ports[SECURED_IDX]->port,
						     IPSEC_ID(rule_idx),
						     ctx->encrypt_rules[rule_idx].current_sn);
}
