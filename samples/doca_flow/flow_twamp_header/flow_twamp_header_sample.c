/*
 * Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>
#include <sys/time.h>

#include <rte_ethdev.h>
#include <rte_net.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_bitfield.h>
#include <doca_flow_definitions.h>
#include <doca_flow_custom_header.h>
#include <doca_flow_custom_header_graph.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_TWAMP_HEADER);

#define PACKET_BURST 128 /* The number of packets in the rx queue */
#define WAIT_SECS 10	 /* number of seconds to wait packets */
#define UDP_DATA_MAX 24	 /* number of UDP payload bytes to print */

#define HEADER_DATA 0
#define HEADER_MAX 1
#define SAMPLER_MAX 3
#define ARC_IN_MAX 1

#define PIPE_ROOT 0
#define PIPE_UDP_INGRESS 1
#define PIPE_UDP_EGRESS 2
#define PIPE_MAX 3

#define UDP_DPORT_NUM 7777
#define SLEEP_SECS 1

#define VALID_BIT_SEQ_NUM 0
#define VALID_BIT_TIMESTAMP_LO 1
#define VALID_BIT_TIMESTAMP_HI 2

#define MAX_TWAMP_HDR_TO_SAMPLE_BYTES 96
#define TWAMP_HDR_OFFSET 42

struct application_header_config {
	struct doca_flow_custom_header *header;
	struct doca_flow_custom_header_sampler *sampler[SAMPLER_MAX];
	struct doca_flow_custom_header_graph_arc *arc_in[ARC_IN_MAX];
	const char *action_str[SAMPLER_MAX];
	const char *desc_str[SAMPLER_MAX];
};

struct application_graph_config {
	struct doca_flow_custom_header_graph *graph;
	struct application_header_config app_header[HEADER_MAX];
};

struct application_graph_config app_graph_config = {0};

struct twamp_hdr {
	uint32_t seq_number;	 /* Sequence Number */
	uint32_t ts_hi;		 /* Timestamp seconds (NTP/OWAMP format) */
	uint32_t ts_lo;		 /* Timestamp fractional seconds */
	uint16_t error_estimate; /* Error Estimate */
	uint8_t mbz;		 /* Must Be Zero */
	uint8_t ttl;		 /* Sender TTL / Hop Limit */
} __attribute__((packed, aligned(8)));

struct custom_match {
	struct doca_flow_match base_match;
	struct twamp_hdr hdr;
	uint8_t valid_bit[8];
};

struct custom_actions {
	struct doca_flow_actions base_actions;
	struct twamp_hdr hdr;
};

struct process_packet_context {
	int nb_ports;
};

static uint64_t g_rx_ts_mask;
static int g_rx_ts_field_offset = -1;

static void flow_custom_header_destroy_node(struct application_header_config *header);

static int rx_timestamp_init(void)
{
	if (rte_mbuf_dyn_rx_timestamp_register(&g_rx_ts_field_offset, &g_rx_ts_mask) < 0) {
		DOCA_LOG_ERR("Failed to register Rx timestamp dynfield");
		return -1;
	}

	return 0;
}

static inline uint64_t rx_mbuf_get_timestamp(const struct rte_mbuf *m)
{
	if ((m->ol_flags & g_rx_ts_mask) == 0)
		return 0;

	return *RTE_MBUF_DYNFIELD(m, g_rx_ts_field_offset, rte_mbuf_timestamp_t *);
}

static void dump_payload_and_cqe_ts(struct rte_mbuf *m)
{
#define SEQ_NUM_BYTES 4
#define TIMESTAMP_SIZE_BYTES 8
	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
	struct twamp_hdr *hdr;
	uint64_t ts_in_payload;
	uint64_t raw_ts, sec, nsec;
	uint64_t timestamp_in_cqe;

	if (rte_pktmbuf_pkt_len(m) >= (TWAMP_HDR_OFFSET + SEQ_NUM_BYTES + TIMESTAMP_SIZE_BYTES)) {
		hdr = (struct twamp_hdr *)(data + TWAMP_HDR_OFFSET);
		raw_ts = rx_mbuf_get_timestamp(m);
		sec = raw_ts / 1000000000L;
		timestamp_in_cqe = sec << 32;
		nsec = raw_ts % 1000000000L;
		timestamp_in_cqe = timestamp_in_cqe | nsec;
		ts_in_payload = ((uint64_t)DOCA_BETOH32(hdr->ts_hi) << 32) | DOCA_BETOH32(hdr->ts_lo);
		DOCA_LOG_INFO("Length = %u, Timestamp in CQE =0x%lx, Timestamp in payload 0x%lx",
			      rte_pktmbuf_pkt_len(m),
			      timestamp_in_cqe,
			      ts_in_payload);
	}
}

static void process_packets(int ingress_port)
{
	struct rte_mbuf *packets[PACKET_BURST];
	int queue_index = 0;
	int nb_packets;
	int i;

	nb_packets = rte_eth_rx_burst(ingress_port, queue_index, packets, PACKET_BURST);

	/* Print received packets data and TS in cqe */
	for (i = 0; i < nb_packets; i++) {
		dump_payload_and_cqe_ts(packets[i]);
		rte_pktmbuf_free(packets[i]);
	}
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
		return DOCA_ERROR_INVALID_VALUE;
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
		       offsetof(struct custom_match, hdr.seq_number),
		       offsetof(struct custom_actions, hdr.seq_number),
		       sizeof(((struct custom_match){}).hdr.seq_number),
		       VALID_BIT_SEQ_NUM);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for hdr.seq_num: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_DATA].sampler[1],
		       &app_graph_config.app_header[HEADER_DATA].action_str[1],
		       &app_graph_config.app_header[HEADER_DATA].desc_str[1],
		       offsetof(struct custom_match, hdr.ts_hi),
		       offsetof(struct custom_actions, hdr.ts_hi),
		       sizeof(((struct custom_match){}).hdr.ts_hi),
		       VALID_BIT_TIMESTAMP_HI);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for hdr.ts_hi: %s", doca_error_get_name(err));
		goto destroy_defs;
	}

	err = add_defs(*defs,
		       app_graph_config.app_header[HEADER_DATA].sampler[2],
		       &app_graph_config.app_header[HEADER_DATA].action_str[2],
		       &app_graph_config.app_header[HEADER_DATA].desc_str[2],
		       offsetof(struct custom_match, hdr.ts_lo),
		       offsetof(struct custom_actions, hdr.ts_lo),
		       sizeof(((struct custom_match){}).hdr.ts_lo),
		       VALID_BIT_TIMESTAMP_LO);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Definitions add field error for hdr.ts_lo: %s", doca_error_get_name(err));
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

	err = doca_flow_custom_header_create(&hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating custom UDP data header (%s)", doca_error_get_descr(err));
		return err;
	}

	if (hdr->header == NULL) {
		DOCA_LOG_ERR("Failed creating custom UDP data header - NULL returned");
		err = DOCA_ERROR_UNKNOWN;
		goto exit;
	}

	err = doca_flow_custom_header_length_field_set_fixed_length(hdr->header, MAX_TWAMP_HDR_TO_SAMPLE_BYTES);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting fixed length UDP data header (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[0], 0);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[0],
							 sizeof(((struct custom_match){}).hdr.seq_number) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[0],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of length UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	/* New fields */
	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[1]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating data sampler1 (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[1], 32);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of data sampler1 (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[1],
							 sizeof(((struct custom_match){}).hdr.ts_hi) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of data sampler1 (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[1],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of length UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_create(hdr->header, &hdr->sampler[2]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating data sampler2 (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_offset(hdr->sampler[2], 64);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting offset of data sampler2 (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_length(hdr->sampler[2],
							 sizeof(((struct custom_match){}).hdr.ts_lo) * 8);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting length of data sampler2 (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_sampler_set_tunnel_mode(hdr->sampler[2],
							      DOCA_FLOW_CUSTOM_HEADER_SAMPLER_TUNNEL_MODE_OUTER);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed setting tunnel mode of length UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_graph_arc_create(app_graph_config.graph, &hdr->arc_in[0]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc creation for UDP data sampler (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_graph_arc_set_src(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_UDP,
							UDP_DPORT_NUM,
							NULL);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting src node for UDP data header (%s)", doca_error_get_descr(err));
		goto exit;
	}

	err = doca_flow_custom_header_graph_arc_set_dst(hdr->arc_in[0],
							DOCA_FLOW_CUSTOM_HEADER_GRAPH_NODE_FLEX,
							hdr->header);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed arc setting dst node for UDP data header (%s)", doca_error_get_descr(err));
		goto exit;
	}

	return DOCA_SUCCESS;
exit:
	flow_custom_header_destroy_node(&app_graph_config.app_header[HEADER_DATA]);
	return err;
}

/*
 * Initialize custom headers and graphs
 */
doca_error_t flow_custom_header_init(struct application_graph_config **graph_config)
{
	doca_error_t err;

	err = doca_flow_custom_header_graph_create(&app_graph_config.graph);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed creating custom header graph(%s)", doca_error_get_descr(err));
		return err;
	}

	err = flow_custom_header_init_data(&app_graph_config.app_header[HEADER_DATA]);
	if (DOCA_IS_ERROR(err)) {
		DOCA_LOG_ERR("Failed init custom header UDP data(%s)", doca_error_get_descr(err));
		goto exit;
	}

	*graph_config = &app_graph_config;

	return DOCA_SUCCESS;
exit:
	err = doca_flow_custom_header_graph_destroy(app_graph_config.graph);
	if (DOCA_IS_ERROR(err))
		DOCA_LOG_ERR("Failed to destroy parse graph (%s)", doca_error_get_descr(err));
	return err;
}

/*
 * Configure graphs on all devices
 */
doca_error_t flow_custom_header_bind(struct application_graph_config *graph_config, struct flow_dev_ctx *flow_dev_ctx)
{
	doca_error_t err;
	int i;

	for (i = 0; i < flow_dev_ctx->nb_devs; i++) {
		err = doca_flow_custom_header_graph_bind(graph_config->graph, flow_dev_ctx->devs_manager[i].doca_dev);
		if (DOCA_IS_ERROR(err)) {
			DOCA_LOG_ERR("Failed to bind parse graph to device (%s)", doca_error_get_descr(err));
			err = doca_flow_custom_header_graph_unbind(graph_config->graph);
			if (DOCA_IS_ERROR(err))
				DOCA_LOG_ERR("Failed to unbind parse graph (%s)", doca_error_get_descr(err));
			break;
		}
	}

	return err;
};

/*
 * Destroy custom headeron all devices
 */
void flow_custom_header_unbind(struct application_graph_config *graph_config)
{
	doca_error_t err;

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

	/* Destroy samplers */
	for (i = 0; i < SAMPLER_MAX; i++) {
		if (header->sampler[i] == NULL)
			break;

		err = doca_flow_custom_header_sampler_destroy(header->sampler[i]);
		if (DOCA_IS_ERROR(err))
			DOCA_LOG_ERR("Failed to destroy sampler arc %u (%s)", i, doca_error_get_descr(err));
	}

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

	flow_custom_header_destroy_node(&graph_config->app_header[HEADER_DATA]);
	err = doca_flow_custom_header_graph_destroy(graph_config->graph);
	if (DOCA_IS_ERROR(err))
		DOCA_LOG_ERR("Failed to destroy parse graph (%s)", doca_error_get_descr(err));
}

/*
 * Create DOCA Flow pipe with UDP destination port match, and forward to the custom header pipes
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @port_id [in]: port id
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
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create pipe %s:", doca_error_get_descr(result));
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

	match->outer.udp.l4_port.dst_port = DOCA_HTOBE16(UDP_DPORT_NUM);
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = pipe[PIPE_UDP_INGRESS];
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
 * @port_id [in]: port id
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_twamp_hdr_ingress_match_pipe(struct doca_flow_port *port,
							struct doca_flow_pipe **pipe,
							int port_id)
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
	enum doca_rss_type ip_rss_flag = port_id == 0 ? DOCA_FLOW_RSS_IPV4_SRC : DOCA_FLOW_RSS_IPV4_DST;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	/* set mask value */
	actions->meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = actions;

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

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

	/* set meta value */
	actions->meta.pkt_meta = DOCA_HTOBE32(0x00010000);

	result = doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

static doca_error_t create_twamp_hdr_egress_match_pipe(struct doca_flow_port *port,
						       struct doca_flow_pipe **pipe,
						       int port_id)
{
#define NB_ACTION_DESC (2)
#define NB_ACTIONS_ARR (1)
	struct custom_match app_match;
	struct custom_actions app_actions;
	struct doca_flow_match *match = &app_match.base_match;
	struct doca_flow_actions *actions = &app_actions.base_actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_action_descs descs;
	struct doca_flow_action_descs *descs_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_desc desc_array[NB_ACTION_DESC] = {0};
	doca_error_t result;

	memset(&app_match, 0, sizeof(app_match));
	memset(&app_actions, 0, sizeof(app_actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	match->parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match->parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	app_match.valid_bit[VALID_BIT_TIMESTAMP_LO] = 0xFF;
	app_match.hdr.ts_lo = 0xffffffff;

	actions->meta.pkt_meta = UINT32_MAX;
	actions_arr[0] = actions;

	descs_arr[0] = &descs;
	descs.nb_action_desc = 0;
	descs.desc_array = desc_array;

	/* Copy lower 32 bits (bits 31-0) of UTC timestamp to packet payload
	 * pointed by desc_str[1].
	 */
	desc_array[0].type = DOCA_FLOW_ACTION_COPY;
	desc_array[0].field_op.src.field_string = "parser_meta.utc.time";
	desc_array[0].field_op.src.bit_offset = 32;
	desc_array[0].field_op.dst.field_string = app_graph_config.app_header[HEADER_DATA].desc_str[1];
	desc_array[0].field_op.dst.bit_offset = 0;
	desc_array[0].field_op.width = 32;

	/* Copy upper 32 bits (bits 63-32) of UTC timestamp to packet payload
	 * pointed by desc_str[2].
	 */
	desc_array[1].type = DOCA_FLOW_ACTION_COPY;
	desc_array[1].field_op.src.field_string = "parser_meta.utc.time";
	desc_array[1].field_op.src.bit_offset = 0;
	desc_array[1].field_op.dst.field_string = app_graph_config.app_header[HEADER_DATA].desc_str[2];
	desc_array[1].field_op.dst.bit_offset = 0;
	desc_array[1].field_op.width = 32;
	descs.nb_action_desc = 2;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "EGRESS_PIPE", DOCA_FLOW_PIPE_BASIC, true);
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

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed creating pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

static doca_error_t add_egress_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
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
	app_match.hdr.ts_lo = DOCA_HTOBE32(0xcafecafe);
	app_match.valid_bit[VALID_BIT_TIMESTAMP_LO] = 0x01;

	/* set meta value */
	actions->meta.pkt_meta = DOCA_HTOBE32(0x00010000);

	result = doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, NULL, NULL, 0, status, &entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Post a UDP with correct UDP port# and valid data to match
 */
static doca_error_t post_twamp_packet(uint32_t seq_num)
{
#define PAYLOAD_SIZE 68
#define MBUF_POOL_SIZE 8192
#define PORT_ID 0
#define NUM_PACKETS 4

	uint16_t ether_hdr_size = sizeof(struct rte_ether_hdr);
	uint16_t ip_hdr_size = sizeof(struct rte_ipv4_hdr);
	uint16_t udp_hdr_size = sizeof(struct rte_udp_hdr);
	uint16_t payload_size = PAYLOAD_SIZE;
	uint16_t total_pkt_size = ether_hdr_size + ip_hdr_size + udp_hdr_size + payload_size;
	struct rte_mempool *mbuf_pool = NULL;
	doca_error_t status = DOCA_SUCCESS;
	struct rte_mbuf *mbuf = NULL;
	struct twamp_hdr *twamp = NULL;
	uint16_t num_tx;
	char *pkt_data;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ip_hdr;
	struct rte_udp_hdr *udp_hdr;
	char *payload;

	/* Match on UDP_DPORT and this value to even allow UTC copy */
	const uint8_t ts_lo[] = {0xca, 0xfe, 0xca, 0xfe};
	const uint16_t ts_lo_len = sizeof(ts_lo);

	/* Create a memory pool for packet buffers */
	mbuf_pool = rte_pktmbuf_pool_create("UDP TEST", MBUF_POOL_SIZE, 0, 0, 1514, rte_eth_dev_socket_id(0));
	if (!mbuf_pool) {
		DOCA_LOG_ERR("Failed to allocate pkt mempool");
		status = DOCA_ERROR_NO_MEMORY;
		goto end;
	}

	/* Allocate an mbuf */
	mbuf = rte_pktmbuf_alloc(mbuf_pool);
	if (mbuf == NULL) {
		DOCA_LOG_ERR("Failed to allocate membuf");
		status = DOCA_ERROR_NO_MEMORY;
		goto mbuf_pool_free;
	}

	mbuf->data_len = total_pkt_size;
	mbuf->pkt_len = total_pkt_size;

	pkt_data = rte_pktmbuf_mtod(mbuf, char *);
	eth_hdr = (struct rte_ether_hdr *)pkt_data;

	/* Format ETH, IP AND UDP Headers */
	rte_ether_addr_copy(&((struct rte_ether_addr){{0xca, 0xfe, 0x01, 0xca, 0xfe, 0x02}}), &eth_hdr->src_addr);
	rte_ether_addr_copy(&((struct rte_ether_addr){{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}), &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	ip_hdr = (struct rte_ipv4_hdr *)(pkt_data + ether_hdr_size);
	ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;
	ip_hdr->type_of_service = 0;
	ip_hdr->total_length = rte_cpu_to_be_16(ip_hdr_size + udp_hdr_size + payload_size);
	ip_hdr->packet_id = 0;
	ip_hdr->fragment_offset = 0;
	ip_hdr->time_to_live = 64;
	ip_hdr->next_proto_id = IPPROTO_UDP;
	ip_hdr->src_addr = rte_cpu_to_be_32(0xC0A80001);
	ip_hdr->dst_addr = rte_cpu_to_be_32(0xC0A80002);
	ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

	udp_hdr = (struct rte_udp_hdr *)(pkt_data + ether_hdr_size + ip_hdr_size);
	udp_hdr->src_port = rte_cpu_to_be_16(1234);
	udp_hdr->dst_port = rte_cpu_to_be_16(UDP_DPORT_NUM);
	udp_hdr->dgram_len = rte_cpu_to_be_16(udp_hdr_size + payload_size);

	payload = pkt_data + ether_hdr_size + ip_hdr_size + udp_hdr_size;
	memset(payload, 0, payload_size);
	twamp = (struct twamp_hdr *)payload;
	twamp->seq_number = seq_num;
	rte_memcpy(&twamp->ts_lo, ts_lo, ts_lo_len);
	udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);

	/* Send 1 packet */
	num_tx = rte_eth_tx_burst(PORT_ID, 0, &mbuf, 1);
	if (num_tx != 1) {
		status = DOCA_ERROR_IO_FAILED;
		DOCA_LOG_ERR("Failed to send UDP packet");
		rte_pktmbuf_free(mbuf);
	}

mbuf_pool_free:
	rte_mempool_free(mbuf_pool);
end:
	return status;
}

static void flow_twamp_packets_process(void *context)
{
	struct process_packet_context *user_ctx = (struct process_packet_context *)context;
	int port_id;

	for (port_id = 0; port_id < user_ctx->nb_ports; port_id++)
		process_packets(port_id);
}

/*
 * Run flow_custom_header sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_twamp_header(int nb_queues)
{
	struct doca_flow_definitions *defs = NULL;
	const int nb_ports = 1;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct process_packet_context wait_ctx = {.nb_ports = nb_ports};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_pipe *pipe[PIPE_MAX] = {NULL};
	struct entries_status status;
	int num_of_entries = 2;
	uint32_t secs = WAIT_SECS;
	doca_error_t result;
	int port_id;

	if (rx_timestamp_init() != 0)
		DOCA_LOG_ERR("Failed to initialize Rx timestamp");

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

		result = create_twamp_hdr_ingress_match_pipe(ports[port_id], &pipe[PIPE_UDP_INGRESS], port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create data header pipe: %s", doca_error_get_descr(result));
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

		result = add_data_pipe_entry(pipe[PIPE_UDP_INGRESS], &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add data pipe entry: %s", doca_error_get_descr(result));
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

		result = create_twamp_hdr_egress_match_pipe(ports[port_id], &pipe[PIPE_UDP_EGRESS], port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create egress: %s", doca_error_get_descr(result));
			goto stop;
		}

		result = add_egress_pipe_entry(pipe[PIPE_UDP_EGRESS], &status);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
			goto stop;
		}
	}

	DOCA_LOG_INFO("Post UDP packets and Wait few seconds for packets to arrive");
	while (secs--) {
		DOCA_LOG_ERR("Post TWAMP pkt# %d", WAIT_SECS - secs);
		result = post_twamp_packet(WAIT_SECS - secs);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_INFO("Failed to post TWAMP packet %d: %s",
				      WAIT_SECS - secs,
				      doca_error_get_descr(result));
			break;
		}
		sleep(SLEEP_SECS);
	}
	flow_wait_for_packets(SLEEP_SECS, flow_twamp_packets_process, &wait_ctx);

stop:
	stop_doca_flow_ports(nb_ports, ports);
exit:
	doca_flow_destroy();
	doca_flow_definitions_destroy(defs);
	return result;
}
