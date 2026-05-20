/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <string.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_net.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_bitfield.h>
#include <doca_flow_definitions.h>
#include <doca_flow_custom_header.h>
#include <doca_flow_custom_header_graph.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_CUSTOM_HEADER);

#define PACKET_BURST 128 /* The number of packets in the rx queue */
#define WAITING_TIME 15	 /* The number of seconds app waits for traffic to come */
#define UDP_DATA_MAX 24	 /* number of UDP payload bytes to print */
#define NB_ACTION_DESC 1

#define HEADER_DATA 0
#define HEADER_TCP 1
#define HEADER_TLV 2
#define HEADER_MAX 3
#define SAMPLER_MAX 2
#define ARC_IN_MAX 1
#define ARC_OUT_MAX 1

#define PIPE_ROOT 0
#define PIPE_DATA 1
#define PIPE_TCP 2
#define PIPE_TLV 3
#define PIPE_MAX 4

#define UDP_PORT_DATA 4500
#define UDP_PORT_TCP 4501
#define UDP_PORT_TLV 4502

#define VALID_BIT_UDP_DATA32 0
#define VALID_BIT_UDP_DATA64 1
#define VALID_BIT_TCP_SPORT 2
#define VALID_BIT_TCP_DPORT 3
#define VALID_BIT_TLV_OPT_0 4
#define VALID_BIT_TLV_OPT_1 5

struct application_header_config {
	struct doca_flow_custom_header *header;
	struct doca_flow_custom_header_sampler *sampler[SAMPLER_MAX];
	struct doca_flow_custom_header_graph_arc *arc_in[ARC_IN_MAX];
	struct doca_flow_custom_header_graph_arc *arc_out[ARC_OUT_MAX];
	const char *action_str[SAMPLER_MAX];
	const char *desc_str[SAMPLER_MAX];
};

struct application_graph_config {
	struct doca_flow_custom_header_graph *graph;
	struct application_header_config app_header[HEADER_MAX];
};

struct application_graph_config app_graph_config = {0};

struct custom_match {
	struct doca_flow_match base_match;
	doca_be32_t udp_data32;
	doca_be64_t udp_data64;
	doca_be16_t tcp_sport;
	doca_be16_t tcp_dport;
	doca_be32_t tlv_option[2];
	uint8_t valid_bit[8];
};

struct custom_actions {
	struct doca_flow_actions base_actions;
	doca_be32_t udp_data32;
	doca_be64_t udp_data64;
	doca_be16_t tcp_sport;
	doca_be16_t tcp_dport;
	doca_be32_t tlv_option[2];
};

struct process_packet_context {
	int nb_ports;
};

static void dump_packet(struct rte_mbuf *m)
{
	struct rte_net_hdr_lens hdr_lens;
	uint32_t pkt_type, meta = 0;
	char hex[UDP_DATA_MAX * 4];

	memset(&hdr_lens, 0, sizeof(hdr_lens));
	memset(&hex, 0, sizeof(hex));
	pkt_type = rte_net_get_ptype(m, &hdr_lens, RTE_PTYPE_ALL_MASK);

	if (pkt_type & RTE_PTYPE_L4_UDP) {
		uint32_t len, off, i = 0;
		uint8_t *p;

		off = hdr_lens.l2_len + hdr_lens.l3_len + hdr_lens.l4_len;
		len = rte_pktmbuf_data_len(m);
		if (len > off && hdr_lens.l4_len >= sizeof(struct rte_udp_hdr)) {
			struct rte_udp_hdr *udp_hdr;
			uint32_t udp_len;

			len = len - off;
			p = rte_pktmbuf_mtod_offset(m, uint8_t *, off);
			udp_hdr = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *, off - hdr_lens.l4_len);
			udp_len = DOCA_BETOH16(udp_hdr->dgram_len);
			if (udp_len >= sizeof(struct rte_udp_hdr))
				udp_len -= sizeof(struct rte_udp_hdr);
			else
				udp_len = 0;

			if (len > udp_len)
				len = udp_len;
			if (len > UDP_DATA_MAX)
				len = UDP_DATA_MAX;
			while (i < len) {
				snprintf(hex + i * 3, 4, " %02X", p[i]);
				i++;
			}
		}
	}

	if (rte_flow_dynf_metadata_avail())
		meta = *RTE_FLOW_DYNF_METADATA(m);

	if (hex[0])
		DOCA_LOG_INFO("Packet %08X meta %X data%s", pkt_type, meta, hex);
	else
		DOCA_LOG_INFO("Packet %08X meta %X", pkt_type, meta);
}

/*
 * Dequeue packets from DPDK queues
 *
 * @ingress_port [in]: port id for dequeue packets
 */
static void process_packets(int ingress_port)
{
	struct rte_mbuf *packets[PACKET_BURST];
	int queue_index = 0;
	int nb_packets;
	int i;

	nb_packets = rte_eth_rx_burst(ingress_port, queue_index, packets, PACKET_BURST);

	/* Print received packets meta data */
	for (i = 0; i < nb_packets; i++) {
		dump_packet(packets[i]);
		rte_pktmbuf_free(packets[i]);
	}
}

static void flow_custom_header_process_packets(void *context)
{
	struct process_packet_context *user_ctx = (struct process_packet_context *)context;
	int port_id;

	for (port_id = 0; port_id < user_ctx->nb_ports; port_id++)
		process_packets(port_id);
}

static doca_error_t add_defs(struct doca_flow_definitions *defs,
			     struct doca_flow_custom_header_sampler *sampler,
			     const char **action_str,
			     const char **desc_str,
			     size_t match_offset,
			     size_t action_offset,
			     size_t size,
			     uint32_t valid_n)
{
	const char *str_data = NULL;
	const char *str_valid = NULL;
	doca_error_t err;

	err = doca_flow_custom_header_sampler_get_match_definition(sampler, &str_data);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed to get data opcode string: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_get_valid_bit_definition(sampler, &str_valid);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed to get valid bit opcode string %u: %s", valid_n, doca_error_get_name(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_get_action_definition(sampler, action_str);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed to get action opcode string %u: %s", valid_n, doca_error_get_name(err));
		return err;
	}

	if (strncmp(*action_str, "actions.packet.", sizeof("actions.packet.") - 1)) {
		DOCA_LOG_ERR("Failed to recognize action opcode string: %s", *action_str);
		return -EINVAL;
	}
	*desc_str = *action_str + sizeof("actions.packet.") - 1;

	err = doca_flow_definitions_add_field(defs, str_data, match_offset, size);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add data field error: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_flow_definitions_add_field(defs,
					      str_valid,
					      offsetof(struct custom_match, valid_bit[valid_n]),
					      sizeof(((struct custom_match){}).valid_bit[valid_n]));
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add valid bit field error %u: %s", valid_n, doca_error_get_name(err));
		return err;
	}

	err = doca_flow_definitions_add_field(defs, *action_str, action_offset, size);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add action field error: %s", doca_error_get_name(err));
		return err;
	}

	return DOCA_SUCCESS;
}

doca_error_t init_defs(struct doca_flow_definitions **defs)
{
	struct doca_flow_definitions_cfg *defs_cfg;
	doca_error_t err;

	err = doca_flow_definitions_cfg_create(&defs_cfg);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed to create defs cfg: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_flow_definitions_create(defs_cfg, defs);
	(void)doca_flow_definitions_cfg_destroy(defs_cfg);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions creation error: %s", doca_error_get_name(err));
		return err;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_DATA].sampler[0],
		       &app_graph_config.app_header[HEADER_DATA].action_str[0],
		       &app_graph_config.app_header[HEADER_DATA].desc_str[0],
		       offsetof(struct custom_match, udp_data32),
		       offsetof(struct custom_actions, udp_data32),
		       sizeof(((struct custom_match){}).udp_data32),
		       VALID_BIT_UDP_DATA32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for udp_data32: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_DATA].sampler[1],
		       &app_graph_config.app_header[HEADER_DATA].action_str[1],
		       &app_graph_config.app_header[HEADER_DATA].desc_str[1],
		       offsetof(struct custom_match, udp_data64),
		       offsetof(struct custom_actions, udp_data64),
		       sizeof(((struct custom_match){}).udp_data64),
		       VALID_BIT_UDP_DATA64);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for udp_data64: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_TCP].sampler[0],
		       &app_graph_config.app_header[HEADER_TCP].action_str[0],
		       &app_graph_config.app_header[HEADER_TCP].desc_str[0],
		       offsetof(struct custom_match, tcp_sport),
		       offsetof(struct custom_actions, tcp_sport),
		       sizeof(((struct custom_match){}).tcp_sport),
		       VALID_BIT_TCP_SPORT);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for tcp_sport: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_TCP].sampler[1],
		       &app_graph_config.app_header[HEADER_TCP].action_str[1],
		       &app_graph_config.app_header[HEADER_TCP].desc_str[1],
		       offsetof(struct custom_match, tcp_dport),
		       offsetof(struct custom_actions, tcp_dport),
		       sizeof(((struct custom_match){}).tcp_dport),
		       VALID_BIT_TCP_DPORT);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for tcp_dport: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_TLV].sampler[0],
		       &app_graph_config.app_header[HEADER_TLV].action_str[0],
		       &app_graph_config.app_header[HEADER_TLV].desc_str[0],
		       offsetof(struct custom_match, tlv_option[0]),
		       offsetof(struct custom_actions, tlv_option[0]),
		       sizeof(((struct custom_match){}).tlv_option[0]),
		       VALID_BIT_TLV_OPT_0);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for tlv option 0: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_TLV].sampler[1],
		       &app_graph_config.app_header[HEADER_TLV].action_str[1],
		       &app_graph_config.app_header[HEADER_TLV].desc_str[1],
		       offsetof(struct custom_match, tlv_option[1]),
		       offsetof(struct custom_actions, tlv_option[1]),
		       sizeof(((struct custom_match){}).tlv_option[1]),
		       VALID_BIT_TLV_OPT_1);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for tlv option 1: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	return DOCA_SUCCESS;

destroy_defs:
	doca_flow_definitions_destroy(*defs);

	return err;
}

doca_error_t flow_custom_header_init_data(struct application_header_config *hdr)
{
	doca_error_t err;
	/*
	 * Custom header represents single dword, following UDP, with dport=UDP_PORT_DATA
	 * with fixed header length
	 *    0                   1                   2                   3
	 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                       UDP data 32 bits                        |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                  UDP data 64 bits - high dword                |
	 * |                  UDP data 64 bits - low dword                 |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	err = doca_flow_custom_header_create(&hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating custom UDP data header (%s)", doca_error_get_descr(err));
		return err;
	}

	if (hdr->header == NULL) {
		DOCA_LOG_ERR("Failed creating custom UDP data header - NULL returned");
		return DOCA_ERROR_UNKNOWN;
	}

	err = doca_flow_custom_header_length_field_set_fixed_length(
		hdr->header,
		sizeof(((struct custom_match){}).udp_data32) * 8 + sizeof(((struct custom_match){}).udp_data64) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting fixed length UDP data header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[0], 0);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[0],
							 sizeof(((struct custom_match){}).udp_data32) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[0],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of length UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[1]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[1], 32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[1],
							 sizeof(((struct custom_match){}).udp_data64) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[1],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of length UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_create(app_graph_config.graph, &hdr->arc_in[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc creation for UDP data sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_set_src(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_UDP,
							UDP_PORT_DATA,
							NULL);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting src node for UDP data header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_set_dst(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_FLEX,
							hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting dst node for UDP data header (%s)", doca_error_get_descr(err));
		return err;
	}

	return DOCA_SUCCESS;
}

doca_error_t flow_custom_header_init_tcp(struct application_header_config *hdr)
{
	doca_error_t err;

	/*
	 * Custom header represents TCP protocol, following UDP, with dport=UDP_PORT_TCP
	 * with header length field
	 *    0                   1                   2                   3
	 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |          Source Port          |       Destination Port        |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                        Sequence Number                        |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                    Acknowledgment Number                      |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |  Data |       |C|E|U|A|P|R|S|F|                               |
	 * | Offset| Rsrvd |W|C|R|C|S|S|Y|I|            Window             |
	 * |       |       |R|E|G|K|H|T|N|N|                               |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |           Checksum            |         Urgent Pointer        |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                           [Options]                           |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	err = doca_flow_custom_header_create(&hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating custom pseudo TCP header (%s)", doca_error_get_descr(err));
		return err;
	}

	if (hdr->header == NULL) {
		DOCA_LOG_ERR("Failed creating custom pseudo TCP - NULL returned");
		return DOCA_ERROR_UNKNOWN;
	}

	err = doca_flow_custom_header_length_field_set_fixed_length(hdr->header, 0);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting fixed length pseudo TCP (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_length_field_set_offset(hdr->header, 96);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting pseudo TCP length field offset (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_length_field_set_length(hdr->header, 4);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting pseudo TCP length field width (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_length_field_set_multiplier(hdr->header, 4);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting pseudo TCP length field multiplier (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating pseudo TCP sport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[0], 0);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of pseudo TCP sport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[0],
							 sizeof(((struct custom_match){}).tcp_sport) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of pseudo TCP sport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[0],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of pseudo TCP sport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[1]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating pseudo TCP dport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[1], 2 * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of pseudo TCP dport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[1],
							 sizeof(((struct custom_match){}).tcp_dport) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of pseudo TCP dport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[1],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of pseudo TCP dport sampler (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_create(app_graph_config.graph, &hdr->arc_in[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc creation for TCP dport header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_set_src(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_UDP,
							UDP_PORT_TCP,
							NULL);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting src node for pseudo TCP header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_set_dst(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_FLEX,
							hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting dst node for pseudo TCP header (%s)", doca_error_get_descr(err));
		return err;
	}

	return DOCA_SUCCESS;
}

doca_error_t flow_custom_header_init_tlv(struct application_header_config *hdr)
{
	doca_error_t err;
	/*
	 * Custom header represents GENEVE protocol, following UDP, with dport=UDP_PORT_TLV
	 *
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *    |Ver|  Opt len  |O|C|   Rsvd.   |         Protocol type         |
	 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *    |        Virtual network identifier (VNI)       |     Rsvd.     |
	 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	err = doca_flow_custom_header_create(&hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating custom TLV option header (%s)", doca_error_get_descr(err));
		return err;
	}

	if (hdr->header == NULL) {
		DOCA_LOG_ERR("Failed creating custom TLV option - NULL returned");
		return DOCA_ERROR_UNKNOWN;
	}

	err = doca_flow_custom_header_length_field_set_fixed_length(hdr->header, 8 * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting fixed length TLV option header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_length_field_set_offset(hdr->header, 2);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option header length field offset (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_length_field_set_length(hdr->header, 6);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option header length field width (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_length_field_set_multiplier(hdr->header, 4);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option header length field multiplier (%s)",
			     doca_error_get_descr(err));
		return err;
	}
	/*
	 * Custom header TLV option format follows GENEVE protocol
	 *     0                   1                   2                   3
	 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |  Option class=0x0132 (Google) |    Type=01    |R|R|R|  Len=1  |
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 * |                     Network cookie                    |R|R|T|D|
	 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	err = doca_flow_custom_header_tlv_options_set_fixed_length(hdr->header, 4 * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option fixed length (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_tlv_options_set_offset(hdr->header, 8 * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option offset (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_tlv_options_type_set_offset(hdr->header, 16);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option type offset (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_tlv_options_type_set_length(hdr->header, 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option type length (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_tlv_options_length_set_offset(hdr->header, 27);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option length offset (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_tlv_options_length_set_length(hdr->header, 5);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option length length (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_tlv_options_length_set_multiplier(hdr->header, 4);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting TLV option length multiplier (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating TLV option sampler 0(%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[0], 32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of TLV option sampler 0 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[0], 32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of TLV option sampler 0 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[0],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of TLV option sampler 0 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_enable_is_options(hdr->sampler[0], 1);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting is_option of TLV option sampler 0 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[1]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating TLV option sampler 1 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[1], 32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset TLV option sampler 1 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[1], 32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of TLV option sampler 1 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[1],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of TLV option sampler 1 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_sampler_enable_is_options(hdr->sampler[1], 2);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting is_option of TLV option sampler 1 (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_create(app_graph_config.graph, &hdr->arc_in[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc creation for TLV option header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_set_src(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_UDP,
							UDP_PORT_TLV,
							NULL);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting src node for TLV option header (%s)", doca_error_get_descr(err));
		return err;
	}

	err = doca_flow_custom_header_graph_arc_set_dst(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_FLEX,
							hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting dst node for TLV option header (%s)", doca_error_get_descr(err));
		return err;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize custom headers and graphs
 */
doca_error_t flow_custom_header_init(struct application_graph_config **graph_config)
{
	doca_error_t err;

	/* Set graph_config early so cleanup can work even if init fails partway */
	*graph_config = &app_graph_config;

	err = doca_flow_custom_header_graph_create(&app_graph_config.graph);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating custom header graph(%s)", doca_error_get_descr(err));
		return err;
	}

	err = flow_custom_header_init_data(&app_graph_config.app_header[HEADER_DATA]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed init custom header UDP data(%s)", doca_error_get_descr(err));
		return err;
	}

	err = flow_custom_header_init_tcp(&app_graph_config.app_header[HEADER_TCP]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed init custom header pseudo TCP(%s)", doca_error_get_descr(err));
		return err;
	}

	err = flow_custom_header_init_tlv(&app_graph_config.app_header[HEADER_TLV]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed init custom header TLV options(%s)", doca_error_get_descr(err));
		return err;
	}

	return DOCA_SUCCESS;
}

/*
 * Configure graphs on all devices
 */
doca_error_t flow_custom_header_bind(struct application_graph_config *graph_config, struct flow_dev_ctx *flow_dev_ctx)
{
	doca_error_t err = DOCA_SUCCESS;
	int i;

	for (i = 0; i < flow_dev_ctx->nb_devs; i++) {
		err = doca_flow_custom_header_graph_bind(graph_config->graph, flow_dev_ctx->devs_manager[i].doca_dev);
		if (DOCA_IS_ERROR(err)) {
			DOCA_LOG_ERR("Failed to bind parse graph to device (%s)", doca_error_get_descr(err));
			break;
		}
	}

	return err;
}

/*
 * Destroy custom headeron all devices
 */
void flow_custom_header_unbind(struct application_graph_config *graph_config)
{
	doca_error_t err;

	if (graph_config == NULL || graph_config->graph == NULL)
		return;

	err = doca_flow_custom_header_graph_unbind(graph_config->graph);
	if (DOCA_IS_ERROR(err))
		DOCA_LOG_ERR("Failed to unbind parse graph (%s)", doca_error_get_descr(err));
}

/*
 * Destroy single custom header
 */
static void flow_custom_header_destroy_node(struct application_header_config *header)
{
	doca_error_t err;
	int i;

	/* Destroy linking arcs first */
	for (i = 0; i < ARC_IN_MAX; i++) {
		if (header->arc_in[i] == NULL)
			break;

		err = doca_flow_custom_header_graph_arc_destroy(header->arc_in[i]);
		if (DOCA_IS_ERROR(err))
			DOCA_LOG_ERR("Failed to destroy input arc %u (%s)", i, doca_error_get_descr(err));
	}

	for (i = 0; i < ARC_OUT_MAX; i++) {
		if (header->arc_out[i] == NULL)
			break;

		err = doca_flow_custom_header_graph_arc_destroy(header->arc_out[i]);
		if (DOCA_IS_ERROR(err))
			DOCA_LOG_ERR("Failed to destroy output arc %u (%s)", i, doca_error_get_descr(err));
	}

	/* Destroy samplers */
	for (i = 0; i < SAMPLER_MAX; i++) {
		if (header->sampler[i] == NULL)
			break;

		err = doca_flow_custom_header_sampler_destroy(header->sampler[i]);
		if (DOCA_IS_ERROR(err))
			DOCA_LOG_ERR("Failed to destroy sampler arc %u (%s)", i, doca_error_get_descr(err));
	}

	if (header->header == NULL)
		return;

	err = doca_flow_custom_header_destroy(header->header);
	if (DOCA_IS_ERROR(err))
		DOCA_LOG_ERR("Failed to destroy custom header (%s)", doca_error_get_descr(err));
}

/*
 * Cleanup custom header
 */
void flow_custom_header_destroy(struct application_graph_config *graph_config)
{
	doca_error_t err;

	if (graph_config == NULL)
		return;

	flow_custom_header_destroy_node(&graph_config->app_header[HEADER_DATA]);
	flow_custom_header_destroy_node(&graph_config->app_header[HEADER_TCP]);
	flow_custom_header_destroy_node(&graph_config->app_header[HEADER_TLV]);

	if (graph_config->graph == NULL)
		return;

	err = doca_flow_custom_header_graph_destroy(graph_config->graph);
	if (DOCA_IS_ERROR(err))
		DOCA_LOG_ERR("Failed to destroy parse graph (%s)", doca_error_get_descr(err));
}

/*
 * Create DOCA Flow pipe with UDP destination port match, and forward to the custom header pipes
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_root_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipes)
{
	struct doca_flow_pipe **pipe = &pipes[PIPE_ROOT];
	struct custom_match app_match;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&app_match, 0, sizeof(app_match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* 5 tuple match */
	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match->outer.udp.l4_port.dst_port = 0xffff;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "ROOT_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* fwd pipe is per entry */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = NULL;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to select the custom header pipes
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_root_pipe_entries(struct doca_flow_pipe **pipe, struct entries_status *status)
{
	struct custom_match app_match;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&app_match, 0, sizeof(app_match));
	memset(&fwd, 0, sizeof(fwd));

	match->outer.udp.l4_port.dst_port = DOCA_HTOBE16(UDP_PORT_DATA);
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipe[PIPE_DATA];
	result = doca_flow_pipe_basic_add_entry(0, pipe[PIPE_ROOT], match, 0, NULL, NULL, &fwd, 0, status, &entry);
	if (result)
		return result;

	match->outer.udp.l4_port.dst_port = DOCA_HTOBE16(UDP_PORT_TCP);
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipe[PIPE_TCP];
	result = doca_flow_pipe_basic_add_entry(0, pipe[PIPE_ROOT], match, 0, NULL, NULL, &fwd, 0, status, &entry);
	if (result)
		return result;

	match->outer.udp.l4_port.dst_port = DOCA_HTOBE16(UDP_PORT_TLV);
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipe[PIPE_TLV];
	result = doca_flow_pipe_basic_add_entry(0, pipe[PIPE_ROOT], match, 0, NULL, NULL, &fwd, 0, status, &entry);
	if (result)
		return result;

	return result;
}

/*
 * Create DOCA Flow pipe with match on custom header for UDP data
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_data_header_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct custom_match app_match;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_action_descs descs;
	struct doca_flow_action_descs *descs_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_desc desc_array[NB_ACTION_DESC] = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint16_t rss_queues[1];
	doca_error_t result;
	enum doca_rss_type ip_rss_flag = DOCA_FLOW_RSS_IPV4_SRC;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* set mask value */
	actions->meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = actions;
	app_actions.udp_data32 = UINT32_MAX;

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	app_match.valid_bit[VALID_BIT_UDP_DATA32] = 0xFF;
	app_match.udp_data32 = 0xffffffff;

	descs_arr[0] = &descs;
	descs.nb_action_desc = 0;
	descs.desc_array = desc_array;

	desc_array[0].type = DOCA_FLOW_ACTION_COPY;
	desc_array[0].field_op.src.field_string = "parser_meta.utc.time";
	desc_array[0].field_op.src.bit_offset = 0;
	desc_array[0].field_op.dst.field_string = app_graph_config.app_header[HEADER_DATA].desc_str[1];
	desc_array[0].field_op.dst.bit_offset = 0;
	desc_array[0].field_op.width = 64;
	descs.nb_action_desc++;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RSS_DATA_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, descs_arr, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* RSS queue - send matched traffic to queue 0  */
	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.inner_flags = ip_rss_flag;
	fwd.rss.nr_queues = 1;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to match on UDP data
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_data_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct custom_match app_match;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	app_match.udp_data32 = DOCA_HTOBE32(0x00000001);
	app_match.valid_bit[VALID_BIT_UDP_DATA32] = 0x01;
	app_actions.udp_data32 = DOCA_HTOBE32(0x01020304);

	/* set meta value */
	actions->meta.pkt_meta = DOCA_HTOBE32(0x00010000);

	result = doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe with match on custom header for pseudo TCP dport
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_tcp_header_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct custom_match app_match;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint16_t rss_queues[1];
	doca_error_t result;
	enum doca_rss_type ip_rss_flag = DOCA_FLOW_RSS_IPV4_SRC;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* set mask value */
	actions->meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = actions;

	/* 5 tuple match */
	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	app_match.tcp_dport = 0xffff;
	app_match.tcp_sport = 0xffff;
	app_match.valid_bit[VALID_BIT_TCP_DPORT] = 0x01;
	app_match.valid_bit[VALID_BIT_TCP_SPORT] = 0x01;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RSS_DATA_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* RSS queue - send matched traffic to queue 0  */
	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.inner_flags = ip_rss_flag;
	fwd.rss.nr_queues = 1;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to match on pseudo TCP dport
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_tcp_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct custom_match app_match;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	app_match.tcp_dport = DOCA_HTOBE16(0x12);
	app_match.tcp_sport = DOCA_HTOBE16(0x34);
	app_match.valid_bit[VALID_BIT_TCP_DPORT] = 0x01;
	app_match.valid_bit[VALID_BIT_TCP_SPORT] = 0x01;

	/* set meta value */
	actions->meta.pkt_meta = DOCA_HTOBE32(0x00020000);

	result = doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow pipe with match on custom header for pseudo GENEVE option
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_tlv_header_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct custom_match app_match;
	struct custom_match app_mask;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_match *mask = &app_mask.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint16_t rss_queues[1];
	doca_error_t result;
	enum doca_rss_type ip_rss_flag = DOCA_FLOW_RSS_IPV4_SRC;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_mask, 0, sizeof(app_mask));
	memset(&app_actions, 0, sizeof(app_actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* set mask value */
	actions->meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = actions;

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	app_match.tlv_option[0] = DOCA_HTOBE32(0xFFFFFFFF);
	app_match.valid_bit[VALID_BIT_TLV_OPT_0] = 0x01;

	mask->parser_meta.outer_l4_type = -1;
	mask->parser_meta.outer_l3_type = -1;
	app_mask.tlv_option[0] = DOCA_HTOBE32(0xFFFFFFFF);
	app_mask.valid_bit[VALID_BIT_TLV_OPT_0] = 0x01;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RSS_DATA_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, match, mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* RSS queue - send matched traffic to queue 0  */
	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.inner_flags = ip_rss_flag;
	fwd.rss.nr_queues = 1;

	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entries to match on pseudo GENEVE option
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_tlv_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct custom_match app_match;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions;
	struct doca_flow_pipe_entry *entry;
	doca_error_t result;
	int v;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

	/* install multiple flows with different match and meta values */
	for (v = 0; v < 16; v++) {
		app_match.tlv_option[0] = DOCA_HTOBE32(0x12345670 + v);
		actions->meta.pkt_meta = DOCA_HTOBE32(0x30000 + v);

		result = doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, NULL, NULL, 0, status, &entry);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_custom_header sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_custom_header(int nb_queues)
{
	struct doca_flow_definitions *defs = NULL;
	const int nb_ports = 1;
	struct process_packet_context wait_ctx = {.nb_ports = nb_ports};
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipe[PIPE_MAX] = {NULL};
	struct entries_status status;
	int num_of_entries = 6;
	doca_error_t result;
	int port_id;

	result = init_defs(&defs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA Flow definitionss: %s", doca_error_get_name(result));
		return result;
	}

	result = init_doca_flow_with_defs(nb_queues, "vnf,hws", &resource, nr_shared_resources, defs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow with definitions: %s", doca_error_get_descr(result));
		goto exit;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto exit;
	}

	for (port_id = 0; port_id < nb_ports; port_id++) {
		memset(&status, 0, sizeof(status));

		result = create_data_header_pipe(ports[port_id], &pipe[PIPE_DATA]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create data header pipe: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = create_tcp_header_pipe(ports[port_id], &pipe[PIPE_TCP]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create tcp header pipe: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = create_tlv_header_pipe(ports[port_id], &pipe[PIPE_TLV]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create tlv option pipe: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = create_root_pipe(ports[port_id], pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = add_root_pipe_entries(pipe, &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add root pipe entries: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = add_data_pipe_entry(pipe[PIPE_DATA], &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add data pipe entry: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = add_tcp_pipe_entry(pipe[PIPE_TCP], &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add tcp pipe entry: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = add_tlv_pipe_entry(pipe[PIPE_TLV], &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add tlv pipe entry: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries: %s", doca_error_get_descr(result));
			goto stop;
		}

		if (status.nb_processed != num_of_entries || status.failure) {
			DOCA_LOG_ERR("Failed to process entries");
			result = DOCA_ERROR_BAD_STATE;
			goto stop;
		}
	}

	/* Process packets and show statistics at specified interval by -i parameter. */
	flow_wait_for_packets(WAITING_TIME, flow_custom_header_process_packets, &wait_ctx);

stop:
	stop_doca_flow_ports(nb_ports, ports);
exit:
	doca_flow_destroy();
	doca_flow_definitions_destroy(defs);
	return result;
}
