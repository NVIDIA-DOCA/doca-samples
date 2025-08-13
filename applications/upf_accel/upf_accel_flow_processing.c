/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_gtp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include <doca_bitfield.h>
#include <doca_flow_net.h>
#include <doca_log.h>

#include "upf_accel.h"
#include "upf_accel_flow_processing.h"
#include "upf_accel_print_header.h"

#define UPF_ACCEL_MAX_PKT_BURST 32
/* Maximum DOCA Flow entries to age in an aging function call */
#define UPF_ACCEL_MAX_NUM_AGING (UPF_ACCEL_MAX_PKT_BURST * 2)
/* Maximum timeout for DOCA Flow handling and processing functions. 0 for no limit */
#define UPF_ACCEL_DOCA_FLOW_MAX_TIMEOUT_US (0)

struct upf_accel_fp_burst_ctx {
	struct rte_mbuf *rx_pkts[UPF_ACCEL_MAX_PKT_BURST];	     /* Rx packet burst */
	struct rte_mbuf *tx_pkts[UPF_ACCEL_MAX_PKT_BURST];	     /* Tx packet burst */
	struct upf_accel_match_8t *matches[UPF_ACCEL_MAX_PKT_BURST]; /* Packet n-tuple matches */
	struct upf_accel_entry_ctx *conns[UPF_ACCEL_MAX_PKT_BURST];  /* Connections matched to packet n-tuples */
	struct tun_parser_ctx parse_ctxs[UPF_ACCEL_MAX_PKT_BURST];   /* Packet n-tuple parser contexts */
	enum parser_pkt_type pkts_type[UPF_ACCEL_MAX_PKT_BURST];     /* Packet type (plain or tunneled) */
	uint16_t rx_pkts_cnt;					     /* Rx packet burst count */
	uint16_t tx_pkts_cnt;					     /* Tx packet burst count */
	bool pkts_drop[UPF_ACCEL_MAX_PKT_BURST];		     /* Packet processing drop indicator */
} __rte_aligned(RTE_CACHE_LINE_SIZE);

static_assert(UPF_ACCEL_MAX_PKT_BURST <= RTE_HASH_LOOKUP_BULK_MAX,
	      "Can't process a burst larger than RTE bulk size limit");

DOCA_LOG_REGISTER(UPF_ACCEL::FLOW_PROCESSING);

/*
 * Changes the state of an entry according to the state machine rules
 *
 * @current_state [in]: current state of entry
 * @new_state [in]: desired new state
 */
static void upf_accel_fp_entry_state_change(enum upf_accel_entry_state *current_state,
					    enum upf_accel_entry_state new_state)
{
	switch (new_state) {
	case UPF_ACCEL_ENTRY_STATE_NONE:
		switch (*current_state) {
		case UPF_ACCEL_ENTRY_STATE_PENDING:
		case UPF_ACCEL_ENTRY_STATE_UNACCELERATED:
		case UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION:
		case UPF_ACCEL_ENTRY_STATE_ACCELERATED:
		case UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION:
			break;
		default:
			DOCA_LOG_ERR("Invalid state transition from %d to NONE", *current_state);
			assert(0);
		}
		break;
	case UPF_ACCEL_ENTRY_STATE_PENDING:
		switch (*current_state) {
		case UPF_ACCEL_ENTRY_STATE_NONE:
			break;
		default:
			DOCA_LOG_ERR("Invalid state transition from %d to PENDING", *current_state);
			assert(0);
		}
		break;
	case UPF_ACCEL_ENTRY_STATE_UNACCELERATED:
		switch (*current_state) {
		case UPF_ACCEL_ENTRY_STATE_PENDING:
			break;
		default:
			DOCA_LOG_ERR("Invalid state transition from %d to UNACCELERATED", *current_state);
			assert(0);
		}
		break;
	case UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION:
		switch (*current_state) {
		case UPF_ACCEL_ENTRY_STATE_NONE:
		case UPF_ACCEL_ENTRY_STATE_PENDING:
		case UPF_ACCEL_ENTRY_STATE_UNACCELERATED:
			break;
		default:
			DOCA_LOG_ERR("Invalid state transition from %d to PENDING_ACCELERATION", *current_state);
			assert(0);
		}
		break;
	case UPF_ACCEL_ENTRY_STATE_ACCELERATED:
		switch (*current_state) {
		case UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION:
			break;
		default:
			DOCA_LOG_ERR("Invalid state transition from %d to ACCELERATED", *current_state);
			assert(0);
		}
		break;
	case UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION:
		switch (*current_state) {
		case UPF_ACCEL_ENTRY_STATE_ACCELERATED:
			break;
		default:
			DOCA_LOG_ERR("Invalid state transition from %d to PENDING_DEL", *current_state);
			assert(0);
		}
		break;
	}

	*current_state = new_state;
}

/*
 * Indicate an entry exists
 *
 * @state [in]: State of the entry
 * @return: true if the entry was initialized, false otherwise
 */
static inline bool upf_accel_fp_entry_is_alive(enum upf_accel_entry_state state)
{
	return state != UPF_ACCEL_ENTRY_STATE_NONE;
}

/*
 * Check if an entry is established
 *
 * @state [in]: State of the entry
 * @return: true if the entry was initialized and is active, false otherwise
 */
static inline bool upf_accel_fp_entry_is_established(enum upf_accel_entry_state state)
{
	return state == UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION ||
	       state == UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION || state == UPF_ACCEL_ENTRY_STATE_ACCELERATED;
}

/*
 * Get the opposite packet type
 *
 * @pkt_type [in]: packet type
 * @return: the opposite type of packet
 */
static enum parser_pkt_type upf_accel_fp_get_opposite_pkt_type(enum parser_pkt_type pkt_type)
{
	return (pkt_type == PARSER_PKT_TYPE_TUNNELED) ? PARSER_PKT_TYPE_PLAIN : PARSER_PKT_TYPE_TUNNELED;
}

/*
 * Delete flow
 *
 * Assign its status to NONE, and delete its context from the ht if the flow
 * in the second type is also NONE (or in error status)
 *
 * @fp_data [in]: flow processing data
 * @dyn_ctx [in]: dynamic entry context
 * @pkt_type [in]: packet type
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_delete_flow(struct upf_accel_fp_data *fp_data,
					     struct upf_accel_dyn_entry_ctx *dyn_ctx,
					     enum parser_pkt_type pkt_type)
{
	enum parser_pkt_type opposite_dir_type = upf_accel_fp_get_opposite_pkt_type(pkt_type);
	bool last_entry = !upf_accel_fp_entry_is_alive(dyn_ctx->entries[opposite_dir_type].state);

	upf_accel_fp_entry_state_change(&dyn_ctx->entries[pkt_type].state, UPF_ACCEL_ENTRY_STATE_NONE);

	if (!last_entry)
		return DOCA_SUCCESS;

	if (rte_hash_del_key_with_hash(fp_data->dyn_tbl, &dyn_ctx->match.inner, dyn_ctx->hash) < 0) {
		DOCA_LOG_WARN("Failed entry aging - hash free failed");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_DBG("Entry removed from hash table");
	return DOCA_SUCCESS;
}

void upf_accel_sw_aging_ll_init(struct upf_accel_fp_data *fp_data, enum parser_pkt_type pkt_type)
{
	struct upf_accel_sw_aging_ll *ll = &fp_data->sw_aging_ll[pkt_type];

	ll->head = UPF_ACCEL_SW_AGING_LL_INVALID_NODE;
	ll->tail = UPF_ACCEL_SW_AGING_LL_INVALID_NODE;
}

/*
 * Init a SW Aging doubly linked list node
 *
 * @conn [in]: connection descriptor
 * @pkt_type [in]: packet type
 */
static void upf_accel_sw_aging_ll_node_init(struct upf_accel_entry_ctx *conn, enum parser_pkt_type pkt_type)
{
	struct upf_accel_sw_aging_ll_node *node = &conn->dyn_ctx.entries[pkt_type].sw_aging_node;

	node->prev = UPF_ACCEL_SW_AGING_LL_INVALID_NODE;
	node->next = UPF_ACCEL_SW_AGING_LL_INVALID_NODE;
}

/*
 * Check if a SW aging linked list node is valid
 *
 * @node_idx [in]: Node index
 * @return: true if is valid, false otherwise
 */
static bool upf_accel_sw_aging_ll_node_is_valid(int32_t node_idx)
{
	return (node_idx != UPF_ACCEL_SW_AGING_LL_INVALID_NODE);
}

/*
 * Check if a flow exists as a node in the SW Aging linked list
 *
 * @fp_data [in]: flow processing data
 * @conn [in]: connection descriptor
 * @pkt_type [in]: packet type
 * @return: true if the node exist, false otherwise
 */
static bool upf_accel_sw_aging_ll_node_exist(struct upf_accel_fp_data *fp_data,
					     struct upf_accel_entry_ctx *conn,
					     enum parser_pkt_type pkt_type)
{
	struct upf_accel_sw_aging_ll_node *node = &conn->dyn_ctx.entries[pkt_type].sw_aging_node;
	int32_t conn_idx = conn->dyn_ctx.conn_idx;

	return upf_accel_sw_aging_ll_node_is_valid(node->prev) || upf_accel_sw_aging_ll_node_is_valid(node->next) ||
	       conn_idx == fp_data->sw_aging_ll[pkt_type].head || conn_idx == fp_data->sw_aging_ll[pkt_type].tail;
}

/*
 * Remove node from the SW Aging linked list
 *
 * @fp_data [in]: flow processing data
 * @conn [in]: connection descriptor
 * @pkt_type [in]: packet type
 * @return: true if the node existed, false otherwise
 */
static bool upf_accel_sw_aging_ll_node_remove(struct upf_accel_fp_data *fp_data,
					      struct upf_accel_entry_ctx *conn,
					      enum parser_pkt_type pkt_type)
{
	int32_t prev = conn->dyn_ctx.entries[pkt_type].sw_aging_node.prev;
	int32_t next = conn->dyn_ctx.entries[pkt_type].sw_aging_node.next;
	int32_t conn_idx = conn->dyn_ctx.conn_idx;
	struct upf_accel_entry_ctx *tmp_conn;

	if (!upf_accel_sw_aging_ll_node_exist(fp_data, conn, pkt_type))
		return false;

	if (upf_accel_sw_aging_ll_node_is_valid(prev)) {
		tmp_conn = &fp_data->dyn_tbl_data[prev];
		tmp_conn->dyn_ctx.entries[pkt_type].sw_aging_node.next = next;
	}
	if (upf_accel_sw_aging_ll_node_is_valid(next)) {
		tmp_conn = &fp_data->dyn_tbl_data[next];
		tmp_conn->dyn_ctx.entries[pkt_type].sw_aging_node.prev = prev;
	}
	if (fp_data->sw_aging_ll[pkt_type].tail == conn_idx)
		fp_data->sw_aging_ll[pkt_type].tail = prev;
	if (fp_data->sw_aging_ll[pkt_type].head == conn_idx)
		fp_data->sw_aging_ll[pkt_type].head = next;

	fp_data->unaccel_counters[pkt_type].current--;
	return true;
}

/*
 * Insert node to the head of the SW Aging linked list and update its timestamp
 *
 * @fp_data [in]: flow processing data
 * @conn [in]: connection descriptor
 * @pkt_type [in]: packet type
 */
static void upf_accel_sw_aging_ll_node_insert(struct upf_accel_fp_data *fp_data,
					      struct upf_accel_entry_ctx *conn,
					      enum parser_pkt_type pkt_type)
{
	struct upf_accel_sw_aging_ll_node *node = &conn->dyn_ctx.entries[pkt_type].sw_aging_node;
	int32_t old_head = fp_data->sw_aging_ll[pkt_type].head;
	int32_t conn_idx = conn->dyn_ctx.conn_idx;
	struct upf_accel_entry_ctx *tmp_conn;

	if (upf_accel_sw_aging_ll_node_is_valid(old_head)) {
		tmp_conn = &fp_data->dyn_tbl_data[old_head];
		tmp_conn->dyn_ctx.entries[pkt_type].sw_aging_node.prev = conn_idx;
	}

	node->prev = UPF_ACCEL_SW_AGING_LL_INVALID_NODE;
	node->next = old_head;
	node->timestamp = rte_rdtsc();

	fp_data->sw_aging_ll[pkt_type].head = conn_idx;
	if (!upf_accel_sw_aging_ll_node_is_valid(fp_data->sw_aging_ll[pkt_type].tail))
		fp_data->sw_aging_ll[pkt_type].tail = conn_idx;

	fp_data->unaccel_counters[pkt_type].current++;
}

/*
 * Move node to the head of the SW Aging linked list
 *
 * First, remove it from its current position (if exist)
 * and then insert it to the head.
 * If the node was not yet in the list, the 'total' counter is increased.
 *
 * @fp_data [in]: flow processing data
 * @conn [in]: connection descriptor
 * @pkt_type [in]: packet type
 */
static void upf_accel_sw_aging_ll_node_move_to_head(struct upf_accel_fp_data *fp_data,
						    struct upf_accel_entry_ctx *conn,
						    enum parser_pkt_type pkt_type)
{
	if (!upf_accel_sw_aging_ll_node_remove(fp_data, conn, pkt_type))
		fp_data->unaccel_counters[pkt_type].total++;

	upf_accel_sw_aging_ll_node_insert(fp_data, conn, pkt_type);
}

/*
 * Scans all the flows in the list, starting from the oldest (tail), and ages out
 * all those that have exceeded the aging period until it encounters a flow that
 * has not yet aged.
 *
 * @fp_data [in]: flow processing data
 * @pkt_type [in]: packet type
 */
static void upf_accel_sw_aging_ll_scan(struct upf_accel_fp_data *fp_data, enum parser_pkt_type pkt_type)
{
	uint64_t aging_period_tsc = fp_data->ctx->upf_accel_cfg->sw_aging_time_sec * rte_get_tsc_hz();
	int32_t curr = fp_data->sw_aging_ll[pkt_type].tail;
	struct upf_accel_sw_aging_ll_node *node;
	uint64_t tsc = rte_rdtsc();
	struct upf_accel_entry_ctx *conn;
	doca_error_t ret;

	while (upf_accel_sw_aging_ll_node_is_valid(curr)) {
		conn = &fp_data->dyn_tbl_data[curr];
		node = &conn->dyn_ctx.entries[pkt_type].sw_aging_node;

		if (tsc - node->timestamp < aging_period_tsc)
			break;

		ret = upf_accel_fp_delete_flow(fp_data, &conn->dyn_ctx, pkt_type);
		if (ret != DOCA_SUCCESS) {
			fp_data->unaccel_counters[pkt_type].aging_errors++;
			DOCA_LOG_ERR("Failed to delete unaccelerated flow");
		}

		fp_data->unaccel_counters[pkt_type].current--;
		curr = node->prev;
	}

	fp_data->sw_aging_ll[pkt_type].tail = curr;
}

/*
 * Check if masked IPV4 address is matching
 *
 * @masked [in]: struct of a masked IPV4 address.
 * @ipv4 [in]: ipv4 address to match
 * @return: true if the address matches, otherwise false.
 */
static inline bool ipv4_masked_is_matching(const struct upf_accel_ip_addr *masked, uint32_t ipv4)
{
	return masked->addr.v4 == (ipv4 & masked->mask.v4);
}

/*
 * Check if masked IPV6 address is matching
 *
 * @masked [in]: struct of a masked IPV6 address
 * @ipv6 [in]: ipv6 address to match
 * @return: true if the address matches, false otherwise
 */
static inline bool ipv6_masked_is_matching(const struct upf_accel_ip_addr *mask_ip,
					   uint8_t ipv6[UPF_ACCEL_NUM_BYTES_IPV6])
{
	union upf_accel_ip masked;
	uint8_t i;

	for (i = 0; i < UPF_ACCEL_NUM_BYTES_IPV6; ++i) {
		masked.v6[i] = ipv6[i] & mask_ip->mask.v6[i];
	}

	return memcmp(masked.v6, mask_ip->addr.v6, sizeof(masked.v6)) == 0;
}

/*
 * Check if masked IP address is matching
 *
 * @masked [in]: struct of a masked IP address.
 * @ip [in]: ip address to match
 * @return: true if the address matches, otherwise false.
 */
static inline bool ip_masked_is_matching(const struct upf_accel_ip_addr *masked, union upf_accel_ip ip)
{
	switch (masked->ip_version) {
	case DOCA_FLOW_L3_TYPE_IP4:
		return ipv4_masked_is_matching(masked, ip.v4);
	case DOCA_FLOW_L3_TYPE_IP6:
		return ipv6_masked_is_matching(masked, ip.v6);
	default:
		DOCA_LOG_ERR("Invalid IP version");
		assert(0);
	}

	return false;
}

/*
 * Parse 5 tuple data from a raw packet, without ethernet header.
 *
 * @parse_ctx [in]: pointer to the parser context
 * @src_ip [out]: pointer to store the source IP
 * @dst_ip [out]: pointer to store the destination IP
 * @src_port [out]: pointer to store the source port
 * @dst_port [out]: pointer to store the destination port
 * @ip_version [out]: pointer to store the IP version
 * @ip_proto [out]: pointer to store the IP protocol
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_5t_match(struct conn_parser_ctx *parse_ctx,
				       union upf_accel_ip *src_ip,
				       union upf_accel_ip *dst_ip,
				       uint16_t *src_port,
				       uint16_t *dst_port,
				       uint8_t *ip_version,
				       uint8_t *ip_proto)
{
	switch (parse_ctx->transport_ctx.proto) {
	case DOCA_FLOW_PROTO_TCP:
		*src_port = rte_be_to_cpu_16(parse_ctx->transport_ctx.tcp_hdr->src_port);
		*dst_port = rte_be_to_cpu_16(parse_ctx->transport_ctx.tcp_hdr->dst_port);
		break;
	case DOCA_FLOW_PROTO_UDP:
		*src_port = rte_be_to_cpu_16(parse_ctx->transport_ctx.udp_hdr->src_port);
		*dst_port = rte_be_to_cpu_16(parse_ctx->transport_ctx.udp_hdr->dst_port);
		break;
	default:
		DOCA_LOG_WARN("Unsupported L4 %d", parse_ctx->transport_ctx.proto);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	switch (parse_ctx->network_ctx.ip_version) {
	case DOCA_FLOW_PROTO_IPV4:
		src_ip->v4 = rte_be_to_cpu_32(parse_ctx->network_ctx.ipv4_hdr->src_addr);
		dst_ip->v4 = rte_be_to_cpu_32(parse_ctx->network_ctx.ipv4_hdr->dst_addr);
		*ip_version = DOCA_FLOW_L3_TYPE_IP4;
		break;
	case DOCA_FLOW_PROTO_IPV6:
		memcpy(src_ip->v6, parse_ctx->network_ctx.ipv6.hdr->src_addr, UPF_ACCEL_NUM_BYTES_IPV6);
		memcpy(dst_ip->v6, parse_ctx->network_ctx.ipv6.hdr->dst_addr, UPF_ACCEL_NUM_BYTES_IPV6);
		*ip_version = DOCA_FLOW_L3_TYPE_IP6;
		break;
	default:
		DOCA_LOG_WARN("Unsupported L3 %d", *ip_version);
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	*ip_proto = parse_ctx->transport_ctx.proto;

	return DOCA_SUCCESS;
}

/*
 * Create a GTPU match from a raw packet
 *
 * @tun_parse_ctx [in]: pointer to the parser context
 * @match [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_gtpu_match(struct tun_parser_ctx *tun_parse_ctx, struct upf_accel_match_8t *match)
{
	doca_error_t ret;

	if ((tun_parse_ctx->network_ctx.ip_version != DOCA_FLOW_PROTO_IPV4) ||
	    tun_parse_ctx->network_ctx.next_proto != DOCA_FLOW_PROTO_UDP ||
	    tun_parse_ctx->transport_ctx.proto != DOCA_FLOW_PROTO_UDP ||
	    rte_be_to_cpu_16(tun_parse_ctx->transport_ctx.udp_hdr->dst_port) != DOCA_FLOW_GTPU_DEFAULT_PORT) {
		DOCA_LOG_DBG("Only support GTPU encapsulation with UDP over IPv4 and GTPU-reserved destination port");
		return DOCA_ERROR_INVALID_VALUE;
	}

	ret = upf_accel_5t_match(&tun_parse_ctx->inner,
				 &match->inner.ue_ip,
				 &match->inner.extern_ip,
				 &match->inner.ue_port,
				 &match->inner.extern_port,
				 &match->inner.ip_version,
				 &match->inner.ip_proto);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed parsing GTPU PDU");
		return ret;
	}

	switch (tun_parse_ctx->network_ctx.ip_version) {
	case DOCA_FLOW_PROTO_IPV4:
		match->outer.te_ip.v4 = rte_be_to_cpu_32(tun_parse_ctx->network_ctx.ipv4_hdr->src_addr);
		match->outer.ip_version = DOCA_FLOW_L3_TYPE_IP4;
		break;
	case DOCA_FLOW_PROTO_IPV6:
		memcpy(match->outer.te_ip.v6, tun_parse_ctx->network_ctx.ipv6.hdr->src_addr, UPF_ACCEL_NUM_BYTES_IPV6);
		match->outer.ip_version = DOCA_FLOW_L3_TYPE_IP6;
		break;
	default:
		DOCA_LOG_WARN("Unsupported L3 %d", tun_parse_ctx->network_ctx.ip_version);
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	match->outer.te_id = rte_be_to_cpu_32(tun_parse_ctx->gtp_ctx.gtp_hdr->teid);
	if (tun_parse_ctx->gtp_ctx.ext_hdr)
		match->outer.qfi = tun_parse_ctx->gtp_ctx.ext_hdr->qfi;
	return DOCA_SUCCESS;
}

/*
 * Create a match from a raw packet, coming from the RAN side.
 *
 * @data [in]: pointer to the start of the data
 * @data_end [in]: pointer to the end of the data
 * @parse_ctx [out]: pointer to the parser context
 * @match [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_ran_match(uint8_t *data,
					uint8_t *data_end,
					struct tun_parser_ctx *parse_ctx,
					struct upf_accel_match_8t *match)
{
	doca_error_t ret;

	ret = tunnel_parse(data, data_end, parse_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;

	return upf_accel_gtpu_match(parse_ctx, match);
}

/*
 * Create a match from a raw packet, coming from the WAN side.
 *
 * @data [in]: pointer to the start of the data
 * @data_end [in]: pointer to the end of the data
 * @parse_ctx [out]: pointer to the parser context
 * @match [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_wan_match(uint8_t *data,
					uint8_t *data_end,
					struct conn_parser_ctx *parse_ctx,
					struct upf_accel_match_5t *match)
{
	doca_error_t ret;

	ret = plain_parse(data, data_end, parse_ctx);
	if (ret != DOCA_SUCCESS)
		return ret;

	return upf_accel_5t_match(parse_ctx,
				  &match->extern_ip,
				  &match->ue_ip,
				  &match->extern_port,
				  &match->ue_port,
				  &match->ip_version,
				  &match->ip_proto);
}

/*
 * Check if a given tunnel header matches the PDR properties.
 *
 * @pdr [in]: the PDR describing the match criteria
 * @match [in]: header to check
 * @return: true if the header matches, otherwise false.
 */
static bool upf_accel_pdr_tunnel_is_matching(const struct upf_accel_pdr *pdr, const struct upf_accel_match_tun *match)
{
	if (match->te_id < pdr->pdi_local_teid_start || match->te_id > pdr->pdi_local_teid_end)
		return false;

	if ((match->qfi || pdr->pdi_qfi) && pdr->pdi_qfi != match->qfi)
		return false;

	return (pdr->pdi_local_teid_ip.ip_version == match->ip_version) &&
	       ip_masked_is_matching(&pdr->pdi_local_teid_ip, match->te_ip);
}

/*
 * Check if a given 5 tuple header matches the PDR properties.
 *
 * @pdr [in]: the PDR describing the match criteria
 * @match [in]: header to check
 * @return: true if the header matches, otherwise false.
 */
static bool upf_accel_pdr_tuple_is_matching(const struct upf_accel_pdr *pdr, const struct upf_accel_match_5t *match)
{
	if (pdr->pdi_sdf_proto && match->ip_proto != pdr->pdi_sdf_proto)
		return false;

	if (match->ue_port < pdr->pdi_sdf_from_port_range.from || match->ue_port > pdr->pdi_sdf_from_port_range.to)
		return false;

	if (match->extern_port < pdr->pdi_sdf_to_port_range.from || match->extern_port > pdr->pdi_sdf_to_port_range.to)
		return false;

	if (pdr->pdi_sdf_from_ip.ip_version != DOCA_FLOW_L3_TYPE_NONE &&
	    pdr->pdi_sdf_from_ip.ip_version != match->ip_version &&
	    !ip_masked_is_matching(&pdr->pdi_sdf_from_ip, match->ue_ip))
		return false;

	if (pdr->pdi_sdf_to_ip.ip_version != DOCA_FLOW_L3_TYPE_NONE &&
	    pdr->pdi_sdf_to_ip.ip_version != match->ip_version &&
	    !ip_masked_is_matching(&pdr->pdi_sdf_to_ip, match->extern_ip))
		return false;

	return pdr->pdi_ueip.ip_version == match->ip_version && ip_masked_is_matching(&pdr->pdi_ueip, match->ue_ip);
}

/*
 * Lookup a PDR that matches a given 8 tuple, from RAN side
 *
 * @pdrs [in]: list of PDRs
 * @match [in]: header to check
 * @return: pointer to a matching PDR, or NULL if non found
 */
static const struct upf_accel_pdr *upf_accel_ran_pdr_lookup(const struct upf_accel_pdrs *pdrs,
							    const struct upf_accel_match_8t *match)
{
	const struct upf_accel_pdr *pdr;
	size_t i;

	for (i = 0; i < pdrs->num_pdrs; i++) {
		pdr = &pdrs->arr_pdrs[i];
		if (pdr->pdi_si != UPF_ACCEL_PDR_PDI_SI_UL)
			continue;

		if (!upf_accel_pdr_tunnel_is_matching(pdr, &match->outer))
			continue;
		if (!upf_accel_pdr_tuple_is_matching(pdr, &match->inner))
			continue;

		return pdr;
	}

	return NULL;
}

/*
 * Decap GTPU header of a packet inplace
 *
 * @pkt [in]: pointer to the pkt
 * @parse_ctx [in]: pointer to the parser context
 */
static void upf_accel_decap(struct rte_mbuf *pkt, struct tun_parser_ctx *parse_ctx)
{
	const uint8_t src_mac[] = UPF_ACCEL_SRC_MAC;
	const uint8_t dst_mac[] = UPF_ACCEL_DST_MAC;
	struct rte_ether_hdr *eth = NULL;

	rte_pktmbuf_adj(pkt, parse_ctx->len - parse_ctx->inner.len - sizeof(*eth));

	eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
	switch (parse_ctx->inner.network_ctx.ip_version) {
	case DOCA_FLOW_PROTO_IPV4:
		eth->ether_type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV4);
		break;
	case DOCA_FLOW_PROTO_IPV6:
		eth->ether_type = DOCA_HTOBE16(DOCA_FLOW_ETHER_TYPE_IPV6);
		break;
	default:
		DOCA_LOG_ERR("Invalid IP version");
		assert(0);
	}
	SET_MAC_ADDR(eth->src_addr.addr_bytes, src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
	SET_MAC_ADDR(eth->dst_addr.addr_bytes, dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);
}

/*
 * Lookup a PDR that matches a given 5 tuple, from WAN side
 *
 * @pdrs [in]: list of PDRs
 * @match [in]: header to check
 * @return: pointer to a matching PDR, or NULL if non found
 */
static const struct upf_accel_pdr *upf_accel_wan_pdr_lookup(const struct upf_accel_pdrs *pdrs,
							    const struct upf_accel_match_5t *match)
{
	const struct upf_accel_pdr *pdr;
	size_t i;

	for (i = 0; i < pdrs->num_pdrs; i++) {
		pdr = &pdrs->arr_pdrs[i];
		if (pdr->pdi_si != UPF_ACCEL_PDR_PDI_SI_DL)
			continue;

		if (!upf_accel_pdr_tuple_is_matching(pdr, match))
			continue;

		return pdr;
	}

	return NULL;
}

#define RTE_LOG_MAX_LCORE (31 - __builtin_clz(RTE_MAX_LCORE))
static_assert(UPF_ACCEL_LOG_MAX_NUM_CONNECTIONS + RTE_LOG_MAX_LCORE <= 32,
	      "Requried num of bits for match chain metadata is too large");
/*
 * Calculate metadata value for a connection's match chain
 *
 * @entry_idx [in]: the index of the entry
 * @queue_id [in]: queue id of the current worker core
 * @return: the results metadata value
 */
static inline uint32_t upf_accel_match_chain_md_calc(int32_t entry_idx, uint16_t queue_id)
{
	return ((uint32_t)queue_id << UPF_ACCEL_LOG_MAX_NUM_CONNECTIONS) | entry_idx;
}

/*
 * Accelerating 8T IPv4 flow.
 *
 * @ctx [in]: UPF Acceleration context.
 * @port_id [in]: port id to accelerate the flow on
 * @queue_id [in]: queue ID
 * @match [in]: software flow match
 * @pdr_id [in]: matching PDR id
 * @entry_ctx [in]: resulting DOCA flow entry accelerate context
 * @entry [out]: resulting DOCA flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_8t_ipv4_accel(struct upf_accel_ctx *ctx,
						 enum upf_accel_port port_id,
						 uint16_t queue_id,
						 struct upf_accel_match_8t *match,
						 uint32_t pdr_id,
						 struct upf_accel_entry_ctx *entry_ctx,
						 struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_actions actions = {.action_idx = 0};
	struct doca_flow_match hw_match = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t res;

	if (!!match->outer.qfi) {
		pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_8T_IPV4];
		hw_match.tun.gtp_ext_psc_qfi = match->outer.qfi;
	} else {
		pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_7T_IPV4];
	}

	hw_match.outer.ip4.src_ip = rte_cpu_to_be_32(match->outer.te_ip.v4);
	hw_match.tun.gtp_teid = rte_cpu_to_be_32(match->outer.te_id);
	hw_match.inner.ip4.src_ip = rte_cpu_to_be_32(match->inner.ue_ip.v4);
	hw_match.inner.ip4.dst_ip = rte_cpu_to_be_32(match->inner.extern_ip.v4);
	hw_match.inner.ip4.next_proto = match->inner.ip_proto;
	switch (match->inner.ip_proto) {
	case DOCA_FLOW_PROTO_TCP:
		hw_match.inner.tcp.l4_port.dst_port = rte_cpu_to_be_16(match->inner.extern_port);
		hw_match.inner.tcp.l4_port.src_port = rte_cpu_to_be_16(match->inner.ue_port);
		break;
	case DOCA_FLOW_PROTO_UDP:
		hw_match.inner.udp.l4_port.dst_port = rte_cpu_to_be_16(match->inner.extern_port);
		hw_match.inner.udp.l4_port.src_port = rte_cpu_to_be_16(match->inner.ue_port);
		break;
	default:
		DOCA_LOG_ERR("Invalid inner IP protocol: %d", match->inner.ip_proto);
		assert(0);
	}
	actions.meta.pkt_meta = DOCA_HTOBE32(pdr_id);

	res = doca_flow_pipe_add_entry(queue_id,
				       pipe,
				       &hw_match,
				       &actions,
				       NULL,
				       NULL,
				       DOCA_FLOW_WAIT_FOR_BATCH,
				       entry_ctx,
				       entry);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to add dynamic 8T IPv4 entry: %s", doca_error_get_descr(res));
		return res;
	}

	return res;
}

/*
 * Accelerating 8T IPv6 flow.
 *
 * @ctx [in]: UPF Acceleration context.
 * @port_id [in]: port id to accelerate the flow on
 * @queue_id [in]: queue ID
 * @match [in]: software flow match
 * @pdr_id [in]: matching PDR id
 * @metadata_value [in]: metadata value to match on
 * @entry_ctx [in]: resulting DOCA flow entry accelerate context
 * @entry [out]: resulting DOCA flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_8t_ipv6_accel(struct upf_accel_ctx *ctx,
						 enum upf_accel_port port_id,
						 uint16_t queue_id,
						 struct upf_accel_match_8t *match,
						 uint32_t pdr_id,
						 uint32_t metadata_value,
						 struct upf_accel_entry_ctx *entry_ctx,
						 struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_actions actions = {.action_idx = 0};
	struct doca_flow_match hw_match = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t res;

	pipe = match->outer.qfi ? ctx->pipes[port_id][UPF_ACCEL_PIPE_8T_IPV6] :
				  ctx->pipes[port_id][UPF_ACCEL_PIPE_7T_IPV6];

	hw_match.meta.pkt_meta = metadata_value;
	hw_match.outer.ip4.src_ip = rte_cpu_to_be_32(match->outer.te_ip.v4);
	memcpy(hw_match.inner.ip6.dst_ip, match->inner.extern_ip.v6, UPF_ACCEL_NUM_BYTES_IPV6);
	hw_match.inner.ip6.next_proto = match->inner.ip_proto;

	switch (match->inner.ip_proto) {
	case DOCA_FLOW_PROTO_TCP:
		hw_match.inner.tcp.l4_port.dst_port = rte_cpu_to_be_16(match->inner.extern_port);
		hw_match.inner.tcp.l4_port.src_port = rte_cpu_to_be_16(match->inner.ue_port);
		break;
	case DOCA_FLOW_PROTO_UDP:
		hw_match.inner.udp.l4_port.dst_port = rte_cpu_to_be_16(match->inner.extern_port);
		hw_match.inner.udp.l4_port.src_port = rte_cpu_to_be_16(match->inner.ue_port);
		break;
	default:
		DOCA_LOG_ERR("Invalid inner IP protocol: %d", match->inner.ip_proto);
		assert(0);
	}
	actions.meta.pkt_meta = DOCA_HTOBE32(pdr_id);

	res = doca_flow_pipe_add_entry(queue_id,
				       pipe,
				       &hw_match,
				       &actions,
				       NULL,
				       NULL,
				       DOCA_FLOW_WAIT_FOR_BATCH,
				       entry_ctx,
				       entry);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to add dynamic 8T IPv6 entry: %s", doca_error_get_descr(res));
		return res;
	}

	return res;
}

/*
 * Accelerating the extension entry of an 8T flow.
 *
 * @ctx [in]: UPF Acceleration context.
 * @port_id [in]: port id to accelerate the flow on
 * @queue_id [in]: queue ID
 * @match [in]: software flow match
 * @metadata_value [in]: metadata value to set
 * @entry_ctx [in]: resulting DOCA flow entry accelerate context
 * @entry [out]: resulting DOCA flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_8t_ipv6_ext_accel(struct upf_accel_ctx *ctx,
						     enum upf_accel_port port_id,
						     uint16_t queue_id,
						     struct upf_accel_match_extension *match,
						     uint32_t metadata_value,
						     struct upf_accel_entry_ctx *entry_ctx,
						     struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_actions actions = {.action_idx = 0};
	struct doca_flow_match hw_match = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t res;

	if (!!match->qfi) {
		pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_8T_IPV6_EXT];
		hw_match.tun.gtp_ext_psc_qfi = match->qfi & 0x3f;
	} else {
		pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_7T_IPV6_EXT];
	}

	hw_match.tun.gtp_teid = rte_cpu_to_be_32(match->te_id);
	memcpy(hw_match.inner.ip6.src_ip, match->ue_ip, UPF_ACCEL_NUM_BYTES_IPV6);
	actions.meta.pkt_meta = metadata_value;

	res = doca_flow_pipe_add_entry(queue_id,
				       pipe,
				       &hw_match,
				       &actions,
				       NULL,
				       NULL,
				       DOCA_FLOW_WAIT_FOR_BATCH,
				       entry_ctx,
				       entry);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to add dynamic 8T IPv6 extension entry: %s", doca_error_get_descr(res));
		return res;
	}

	return res;
}

/*
 * Accelerating 5T IPv4 flow.
 *
 * @ctx [in]: UPF Acceleration context.
 * @port_id [in]: port id to accelerate the flow on
 * @queue_id [in]: queue ID
 * @match [in]: software flow match
 * @pdr_id [in]: matching PDR id
 * @entry_ctx [in]: resulting DOCA flow entry accelerate context
 * @entry [out]: resulting DOCA flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_5t_ipv4_accel(struct upf_accel_ctx *ctx,
						 enum upf_accel_port port_id,
						 uint16_t queue_id,
						 struct upf_accel_match_5t *match,
						 uint32_t pdr_id,
						 struct upf_accel_entry_ctx *entry_ctx,
						 struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_actions actions = {.action_idx = 0};
	struct doca_flow_match hw_match = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t res;

	pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_5T_IPV4];
	hw_match.outer.ip4.dst_ip = rte_cpu_to_be_32(match->ue_ip.v4);
	hw_match.outer.ip4.src_ip = rte_cpu_to_be_32(match->extern_ip.v4);
	hw_match.outer.ip4.next_proto = match->ip_proto;
	switch (match->ip_proto) {
	case DOCA_FLOW_PROTO_TCP:
		hw_match.outer.tcp.l4_port.dst_port = rte_cpu_to_be_16(match->ue_port);
		hw_match.outer.tcp.l4_port.src_port = rte_cpu_to_be_16(match->extern_port);
		break;
	case DOCA_FLOW_PROTO_UDP:
		hw_match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(match->ue_port);
		hw_match.outer.udp.l4_port.src_port = rte_cpu_to_be_16(match->extern_port);
		break;
	default:
		DOCA_LOG_ERR("Invalid outer IP protocol: %d", match->ip_proto);
		assert(0);
	}
	actions.meta.pkt_meta = DOCA_HTOBE32(pdr_id);

	res = doca_flow_pipe_add_entry(queue_id,
				       pipe,
				       &hw_match,
				       &actions,
				       NULL,
				       NULL,
				       DOCA_FLOW_WAIT_FOR_BATCH,
				       entry_ctx,
				       entry);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to add dynamic 5T IPv4 entry: %s", doca_error_get_descr(res));
		return res;
	}

	return res;
}

/*
 * Accelerating 5T IPv6 flow.
 *
 * @ctx [in]: UPF Acceleration context.
 * @port_id [in]: port id to accelerate the flow on
 * @queue_id [in]: queue ID
 * @match [in]: software flow match
 * @pdr_id [in]: matching PDR id
 * @metadata_value [in]: metadata value to match on
 * @entry_ctx [in]: resulting DOCA flow entry accelerate context
 * @entry [out]: resulting DOCA flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_5t_ipv6_accel(struct upf_accel_ctx *ctx,
						 enum upf_accel_port port_id,
						 uint16_t queue_id,
						 struct upf_accel_match_5t *match,
						 uint32_t pdr_id,
						 uint32_t metadata_value,
						 struct upf_accel_entry_ctx *entry_ctx,
						 struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_actions actions = {.action_idx = 0};
	struct doca_flow_match hw_match = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t res;

	pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_5T_IPV6];
	hw_match.meta.pkt_meta = metadata_value;
	memcpy(hw_match.outer.ip6.src_ip, match->extern_ip.v6, UPF_ACCEL_NUM_BYTES_IPV6);
	hw_match.outer.ip6.next_proto = match->ip_proto;
	switch (match->ip_proto) {
	case DOCA_FLOW_PROTO_TCP:
		hw_match.outer.tcp.l4_port.dst_port = rte_cpu_to_be_16(match->ue_port);
		hw_match.outer.tcp.l4_port.src_port = rte_cpu_to_be_16(match->extern_port);
		break;
	case DOCA_FLOW_PROTO_UDP:
		hw_match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(match->ue_port);
		hw_match.outer.udp.l4_port.src_port = rte_cpu_to_be_16(match->extern_port);
		break;
	default:
		DOCA_LOG_ERR("Invalid outer IP protocol: %d", match->ip_proto);
		assert(0);
	}
	actions.meta.pkt_meta = DOCA_HTOBE32(pdr_id);

	res = doca_flow_pipe_add_entry(queue_id,
				       pipe,
				       &hw_match,
				       &actions,
				       NULL,
				       NULL,
				       DOCA_FLOW_WAIT_FOR_BATCH,
				       entry_ctx,
				       entry);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to add dynamic 5T IPv6 entry: %s", doca_error_get_descr(res));
		return res;
	}

	return res;
}

/*
 * Accelerating the extension entry of a 5T flow.
 *
 * @ctx [in]: UPF Acceleration context.
 * @port_id [in]: port id to accelerate the flow on
 * @queue_id [in]: queue ID
 * @match [in]: software flow match
 * @metadata_value [in]: metadata value to set
 * @entry_ctx [in]: resulting DOCA flow entry accelerate context
 * @entry [out]: resulting DOCA flow entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_5t_ipv6_ext_accel(struct upf_accel_ctx *ctx,
						     enum upf_accel_port port_id,
						     uint16_t queue_id,
						     struct upf_accel_match_extension *match,
						     uint32_t metadata_value,
						     struct upf_accel_entry_ctx *entry_ctx,
						     struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_actions actions = {.action_idx = 0};
	struct doca_flow_match hw_match = {0};
	struct doca_flow_pipe *pipe;
	doca_error_t res;

	pipe = ctx->pipes[port_id][UPF_ACCEL_PIPE_5T_IPV6_EXT];
	memcpy(hw_match.outer.ip6.dst_ip, match->ue_ip, UPF_ACCEL_NUM_BYTES_IPV6);
	actions.meta.pkt_meta = metadata_value;

	res = doca_flow_pipe_add_entry(queue_id,
				       pipe,
				       &hw_match,
				       &actions,
				       NULL,
				       NULL,
				       DOCA_FLOW_WAIT_FOR_BATCH,
				       entry_ctx,
				       entry);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to add dynamic 5T IPv6 extension entry: %s", doca_error_get_descr(res));
		return res;
	}

	return res;
}

/*
 * Parse the packet and fill in the match structure.
 *
 * @pkt_type [in]: packet type
 * @pkt [in]: pointer to the pkt
 * @match [in]: software flow match
 * @parse_ctx [out]: pointer to the parser context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_pkt_match(enum parser_pkt_type pkt_type,
					   struct rte_mbuf *pkt,
					   struct upf_accel_match_8t *match,
					   struct tun_parser_ctx *parse_ctx)
{
	uint8_t *data_beg = rte_pktmbuf_mtod(pkt, uint8_t *);
	uint8_t *data_end = data_beg + rte_pktmbuf_data_len(pkt);
	doca_error_t ret;

	memset(match, 0, sizeof(*match));

	if (pkt_type == PARSER_PKT_TYPE_TUNNELED) {
		ret = upf_accel_ran_match(data_beg, data_end, parse_ctx, match);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_DBG("Failed to parse RAN packet status %u", ret);
			return ret;
		}
	} else {
		ret = upf_accel_wan_match(data_beg, data_end, &parse_ctx->inner, &match->inner);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_DBG("Failed to parse WAN packet status %u", ret);
			return ret;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Fetch packet type from metadata.
 *
 * @pkt [in]: pointer to the pkt
 * @return: packet type (ran / wan)
 */
static enum parser_pkt_type upf_accel_fp_fetch_pkt_type(const struct rte_mbuf *pkt)
{
	const uint32_t md = *RTE_FLOW_DYNF_METADATA(pkt);
	const uint32_t dir = md & UPF_ACCEL_META_PKT_DIR_MASK;

	assert(dir == UPF_ACCEL_META_PKT_DIR_UL || dir == UPF_ACCEL_META_PKT_DIR_DL);

	return (dir == UPF_ACCEL_META_PKT_DIR_UL) ? PARSER_PKT_TYPE_TUNNELED : PARSER_PKT_TYPE_PLAIN;
}

/*
 * Parse the packet burst and fill in the match structures.
 *
 * @burst_ctx [in]: packet burst context
 * @match_mem [in]: array of match structure used to init burst_ctx->matches
 */
static void upf_accel_fp_pkts_match(struct upf_accel_fp_burst_ctx *burst_ctx, struct upf_accel_match_8t match_mem[])
{
	doca_error_t ret;
	uint16_t i;

	for (i = 0; i < burst_ctx->rx_pkts_cnt; i++) {
		burst_ctx->pkts_type[i] = upf_accel_fp_fetch_pkt_type(burst_ctx->rx_pkts[i]);
		burst_ctx->matches[i] = &match_mem[i];

		ret = upf_accel_fp_pkt_match(burst_ctx->pkts_type[i],
					     burst_ctx->rx_pkts[i],
					     burst_ctx->matches[i],
					     &burst_ctx->parse_ctxs[i]);
		if (ret != DOCA_SUCCESS)
			burst_ctx->pkts_drop[i] = true;
	}
}

/*
 * Compare outer headers (the part of 8/7tuple that is not included in 5tuple)
 *
 * @tunnel1 [in]: first tunnel match
 * @tunnel2 [in]: second tunnel match
 * @return: true if tunnels are equal, false otherwise
 */
static bool upf_accel_fp_tunnel_eq(struct upf_accel_match_tun *tunnel1, struct upf_accel_match_tun *tunnel2)
{
	bool ip_neq;

	switch (tunnel1->ip_version) {
	case DOCA_FLOW_L3_TYPE_IP6:
		ip_neq = memcmp(tunnel1->te_ip.v6, tunnel2->te_ip.v6, UPF_ACCEL_NUM_BYTES_IPV6);
		break;
	case DOCA_FLOW_L3_TYPE_IP4:
		ip_neq = tunnel1->te_ip.v4 != tunnel2->te_ip.v4;
		break;
	default:
		DOCA_LOG_ERR("Invalid IP version");
		assert(0);
		ip_neq = true;
	}

	return !ip_neq && tunnel1->ip_version == tunnel2->ip_version && tunnel1->te_id == tunnel2->te_id &&
	       tunnel1->qfi == tunnel2->qfi;
}

/*
 * PDR lookup according to 5T match
 *
 * @fp_data [in]: flow processing data
 * @pkt_type [in]: packet type
 * @match [in]: software flow match
 * @pdr_out [out]: pdr
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_pdr_lookup(struct upf_accel_fp_data *fp_data,
					    enum parser_pkt_type pkt_type,
					    struct upf_accel_match_8t *match,
					    const struct upf_accel_pdr **pdr_out)
{
	const struct upf_accel_pdr *pdr;

	pdr = pkt_type == PARSER_PKT_TYPE_TUNNELED ?
		      upf_accel_ran_pdr_lookup(fp_data->ctx->upf_accel_cfg->pdrs, match) :
		      upf_accel_wan_pdr_lookup(fp_data->ctx->upf_accel_cfg->pdrs, &match->inner);
	if (!pdr) {
		DOCA_LOG_DBG("Failed to lookup PDR for packet type %u", pkt_type);
		return DOCA_ERROR_NOT_FOUND;
	}
	*pdr_out = pdr;

	return DOCA_SUCCESS;
}

/*
 * Get existing or initialize a new instance of dynamic connection.
 *
 * @fp_data [in]: flow processing data
 * @rx_port_id [in]: receive port id
 * @pkt_type [in]: packet type
 * @match [in]: software flow match
 * @conn_idx [in]: existing connection position in dynamic table or a negative value
 * @conn_out [out]: resulting connection descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_conn_lookup(struct upf_accel_fp_data *fp_data,
					     enum upf_accel_port rx_port_id,
					     enum parser_pkt_type pkt_type,
					     struct upf_accel_match_8t *match,
					     int32_t conn_idx,
					     struct upf_accel_entry_ctx **conn_out)
{
	struct upf_accel_entry_ctx *conn;
	const struct upf_accel_pdr *pdr;
	doca_error_t result;
	hash_sig_t hash;

	if (conn_idx < 0) {
		result = upf_accel_fp_pdr_lookup(fp_data, pkt_type, match, &pdr);
		if (result != DOCA_SUCCESS)
			return result;

		hash = rte_hash_hash(fp_data->dyn_tbl, &match->inner);
		conn_idx = rte_hash_add_key_with_hash(fp_data->dyn_tbl, &match->inner, hash);
		if (conn_idx < 0) {
			fp_data->accel_failed_counters[pkt_type].accel_errors++;
			DOCA_LOG_DBG("Couldn't create flow, No space available in the ht, err %d", conn_idx);
			return DOCA_ERROR_FULL;
		}

		conn = &fp_data->dyn_tbl_data[conn_idx];
		memset(conn, 0, sizeof(*conn));
		conn->dyn_ctx.match = *match;
		conn->dyn_ctx.hash = hash;
		conn->dyn_ctx.port_id = rx_port_id;
		conn->dyn_ctx.pdr_id[pkt_type] = pdr->id;
		conn->dyn_ctx.fp_data = fp_data;
		conn->dyn_ctx.conn_idx = conn_idx;
		upf_accel_fp_entry_state_change(&conn->dyn_ctx.entries[pkt_type].state, UPF_ACCEL_ENTRY_STATE_PENDING);

		upf_accel_sw_aging_ll_node_init(conn, pkt_type);
	} else {
		conn = &fp_data->dyn_tbl_data[conn_idx];

		if (pkt_type == PARSER_PKT_TYPE_TUNNELED) {
			if (upf_accel_fp_entry_is_alive(conn->dyn_ctx.entries[pkt_type].state) &&
			    !upf_accel_fp_tunnel_eq(&match->outer, &conn->dyn_ctx.match.outer)) {
				DOCA_LOG_DBG("Detected packet with same 5tuple as connection with different tunnel");
				return DOCA_ERROR_ALREADY_EXIST;
			}
			conn->dyn_ctx.match.outer = match->outer;
		}

		/*
		 * The connection was already initialized by a packet arriving
		 * from the opposite type; now, we need to complete the
		 * initialization for the other type.
		 */
		if (!upf_accel_fp_entry_is_alive(conn->dyn_ctx.entries[pkt_type].state)) {
			result = upf_accel_fp_pdr_lookup(fp_data, pkt_type, match, &pdr);
			if (result != DOCA_SUCCESS)
				return result;

			upf_accel_fp_entry_state_change(&conn->dyn_ctx.entries[pkt_type].state,
							UPF_ACCEL_ENTRY_STATE_PENDING);
			conn->dyn_ctx.pdr_id[pkt_type] = pdr->id;
			upf_accel_sw_aging_ll_node_init(conn, pkt_type);
		}
	}

	*conn_out = conn;
	return DOCA_SUCCESS;
}

/*
 * Get existing or initialize a new instance of dynamic connection for every packet in the burst.
 *
 * @fp_data [in]: flow processing data
 * @rx_port_id [in]: receive port id
 * @burst_ctx [in]: packet burst context
 */
static void upf_accel_fp_conns_lookup(struct upf_accel_fp_data *fp_data,
				      enum upf_accel_port rx_port_id,
				      struct upf_accel_fp_burst_ctx *burst_ctx)
{
	int32_t conn_idxs[RTE_HASH_LOOKUP_BULK_MAX];
	doca_error_t ret;
	uint16_t i;
	int err;

	if (!burst_ctx->rx_pkts_cnt)
		return;

	err = rte_hash_lookup_bulk(fp_data->dyn_tbl,
				   (const void **)burst_ctx->matches,
				   burst_ctx->rx_pkts_cnt,
				   conn_idxs);
	assert(!err);
	UNUSED(err);

	for (i = 0; i < burst_ctx->rx_pkts_cnt; i++) {
		if (burst_ctx->pkts_drop[i])
			continue;

		ret = upf_accel_fp_conn_lookup(fp_data,
					       rx_port_id,
					       burst_ctx->pkts_type[i],
					       burst_ctx->matches[i],
					       conn_idxs[i],
					       &burst_ctx->conns[i]);
		if (ret != DOCA_SUCCESS)
			burst_ctx->pkts_drop[i] = true;
	}
}

/*
 * Extracts the relevant fields from an 8 tuple match structure to an extension match structure.
 *
 * @match [in]: Pointer to the 8 tuple match structure containing the inner and outer match fields.
 * @ext_match [out]: Pointer to the extension match structure to be populated with extracted values.
 */
static inline void upf_accel_fp_extension_match_extract(struct upf_accel_match_8t *match,
							struct upf_accel_match_extension *ext_match)
{
	memcpy(ext_match->ue_ip, match->inner.ue_ip.v6, UPF_ACCEL_NUM_BYTES_IPV6);
	ext_match->te_id = match->outer.te_id;
	ext_match->qfi = match->outer.qfi & 0x3f;
}

/*
 * Accelerate a unidirectional flow on a connection
 *
 * @fp_data [in]: flow processing data
 * @port_id [in]: port id
 * @pkt_type [in]: packet type
 * @match [in]: software flow match
 * @conn [in]: connection descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_flow_accel(struct upf_accel_fp_data *fp_data,
					    enum upf_accel_port port_id,
					    enum parser_pkt_type pkt_type,
					    struct upf_accel_match_8t *match,
					    struct upf_accel_entry_ctx *conn)
{
	struct upf_accel_dyn_entry_ctx *dyn_ctx = &conn->dyn_ctx;
	struct upf_accel_entry_ctx *ext_entry_ctx;
	struct upf_accel_match_extension ext_match;
	struct upf_accel_flow_entry *flow_entry;
	int32_t ext_entry_idx;
	doca_error_t ret;
	hash_sig_t hash;

	if (upf_accel_fp_entry_is_established(conn->dyn_ctx.entries[pkt_type].state))
		return DOCA_ERROR_ALREADY_EXIST;

	if (upf_accel_fp_entry_is_alive(conn->dyn_ctx.entries[pkt_type].state) &&
	    (conn->dyn_ctx.cnt_pkts[pkt_type] + 1) < fp_data->ctx->upf_accel_cfg->dpi_threshold) {
		upf_accel_fp_entry_state_change(&conn->dyn_ctx.entries[pkt_type].state,
						UPF_ACCEL_ENTRY_STATE_UNACCELERATED);
		return DOCA_SUCCESS;
	}

	flow_entry = &dyn_ctx->entries[pkt_type];

	if (match->inner.ip_version == DOCA_FLOW_L3_TYPE_IP6) {
		upf_accel_fp_extension_match_extract(match, &ext_match);
		ext_entry_idx = rte_hash_lookup(fp_data->dyn_ext_tbl, (const void *)&ext_match);
		if (ext_entry_idx < 0) {
			hash = rte_hash_hash(fp_data->dyn_ext_tbl, &ext_match);
			ext_entry_idx =
				rte_hash_add_key_with_hash(fp_data->dyn_ext_tbl, (const void *)&ext_match, hash);
			fp_data->dyn_ext_tbl_data[ext_entry_idx].ext_ctx.match = ext_match;
			fp_data->dyn_ext_tbl_data[ext_entry_idx].ext_ctx.fp_data = fp_data;
			fp_data->dyn_ext_tbl_data[ext_entry_idx].ext_ctx.hash = hash;
			fp_data->dyn_ext_tbl_data[ext_entry_idx].ext_ctx.md_value =
				upf_accel_match_chain_md_calc(ext_entry_idx, fp_data->queue_id);
		} else if (fp_data->dyn_ext_tbl_data[ext_entry_idx].ext_ctx.state ==
			   UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION) {
			upf_accel_fp_entry_state_change(&dyn_ctx->entries[pkt_type].state,
							UPF_ACCEL_ENTRY_STATE_UNACCELERATED);
			return DOCA_SUCCESS;
		}

		ext_entry_ctx = &fp_data->dyn_ext_tbl_data[ext_entry_idx];
		flow_entry->ext_entry = ext_entry_ctx;
		ext_entry_ctx->ext_ctx.ref_cnt++;

		ret = pkt_type == PARSER_PKT_TYPE_TUNNELED ?
			      upf_accel_pipe_8t_ipv6_accel(fp_data->ctx,
							   port_id,
							   fp_data->queue_id,
							   match,
							   dyn_ctx->pdr_id[pkt_type],
							   ext_entry_ctx->ext_ctx.md_value,
							   conn,
							   &flow_entry->entry) :
			      upf_accel_pipe_5t_ipv6_accel(fp_data->ctx,
							   port_id,
							   fp_data->queue_id,
							   &match->inner,
							   dyn_ctx->pdr_id[pkt_type],
							   ext_entry_ctx->ext_ctx.md_value,
							   conn,
							   &flow_entry->entry);
	} else {
		ret = pkt_type == PARSER_PKT_TYPE_TUNNELED ? upf_accel_pipe_8t_ipv4_accel(fp_data->ctx,
											  port_id,
											  fp_data->queue_id,
											  match,
											  dyn_ctx->pdr_id[pkt_type],
											  conn,
											  &flow_entry->entry) :
							     upf_accel_pipe_5t_ipv4_accel(fp_data->ctx,
											  port_id,
											  fp_data->queue_id,
											  &match->inner,
											  dyn_ctx->pdr_id[pkt_type],
											  conn,
											  &flow_entry->entry);
	}

	switch (ret) {
	case DOCA_SUCCESS:
		upf_accel_fp_entry_state_change(&conn->dyn_ctx.entries[pkt_type].state,
						UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION);
		upf_accel_sw_aging_ll_node_remove(fp_data, conn, pkt_type);
		break;
	default:
		fp_data->accel_failed_counters[pkt_type].current++;
		fp_data->accel_failed_counters[pkt_type].total++;
		DOCA_LOG_ERR("FATAL ERROR: unexpected failure to accelerate connection on port %u, type %u.",
			     port_id,
			     pkt_type);
		return DOCA_ERROR_FULL;
	}

	return DOCA_SUCCESS;
}

/*
 * Increase packet counter (bytes and number).
 *
 * @ctr [in]: counter to increase.
 * @pkt [in]: pointer to the pkt mbuf.
 */
static inline void upf_accel_packet_byte_counter_inc(struct upf_accel_packet_byte_counter *ctr, struct rte_mbuf *pkt)
{
	ctr->pkts++;
	ctr->bytes += rte_pktmbuf_pkt_len(pkt);
}

/*
 * Accelerate a unidirectional flow on every connection in the burst.
 *
 * @fp_data [in]: flow processing data
 * @rx_port_id [in]: incoming packet port id
 * @burst_ctx [in]: packet burst context
 */
static void upf_accel_fp_flows_accel(struct upf_accel_fp_data *fp_data,
				     enum upf_accel_port rx_port_id,
				     struct upf_accel_fp_burst_ctx *burst_ctx)
{
	enum parser_pkt_type pkt_type;
	struct upf_accel_entry_ctx *conn;
	struct rte_mbuf *pkt;
	doca_error_t ret;
	uint16_t i;

	for (i = 0; i < burst_ctx->rx_pkts_cnt; i++) {
		if (burst_ctx->pkts_drop[i])
			continue;

		conn = burst_ctx->conns[i];
		pkt = burst_ctx->rx_pkts[i];
		pkt_type = burst_ctx->pkts_type[i];

		ret = upf_accel_fp_flow_accel(fp_data, rx_port_id, pkt_type, burst_ctx->matches[i], conn);
		if (ret == DOCA_ERROR_ALREADY_EXIST)
			DOCA_LOG_DBG("Got a sneaker packet on core %u, port %u, type %u.",
				     fp_data->queue_id,
				     rx_port_id,
				     pkt_type);

		if (conn->dyn_ctx.cnt_pkts[pkt_type])
			upf_accel_packet_byte_counter_inc(&fp_data->sw_counters.ex_conn, pkt);
		else
			upf_accel_packet_byte_counter_inc(&fp_data->sw_counters.new_conn, pkt);

		conn->dyn_ctx.cnt_pkts[pkt_type]++;
		conn->dyn_ctx.cnt_bytes[pkt_type] += rte_pktmbuf_pkt_len(pkt);
	}
}

/*
 * Drop the packet and increase error counters.
 *
 * @fp_data [in]: flow processing data
 * @pkt [in]: packet to drop
 */
static void upf_accel_fp_pkt_err_drop(struct upf_accel_fp_data *fp_data, struct rte_mbuf *pkt)
{
	upf_accel_packet_byte_counter_inc(&fp_data->sw_counters.err, pkt);
	rte_pktmbuf_free(pkt);
}

/*
 * Set metadata to a packet, in place
 *
 * @pkt [in]: pointer to the pkt
 * @md [in]: metadata to set
 */
static void upf_accel_md_set(struct rte_mbuf *pkt, uint32_t md)
{
	assert(md < UPF_ACCEL_MAX_NUM_PDR);
	*RTE_FLOW_DYNF_METADATA(pkt) = md;
	pkt->ol_flags |= RTE_MBUF_DYNFLAG_TX_METADATA;
}

/*
 * Check if the flow is not in an accelerated status
 *
 * @pkt_type [in]: packet type
 * @conn [in]: connection descriptor
 * @return: true if it`s unaccelerated, false otherwise
 */
static bool is_flow_unaccelerated(enum parser_pkt_type pkt_type, struct upf_accel_entry_ctx *conn)
{
	return (conn->dyn_ctx.entries[pkt_type].state == UPF_ACCEL_ENTRY_STATE_UNACCELERATED);
}

/*
 * Perform miscellaneous post processing operations on the burst. (packet metadata, decap, send)
 *
 * @fp_data [in]: flow processing data
 * @burst_ctx [in]: packet burst context
 */
static void upf_accel_fp_burst_postprocess(struct upf_accel_fp_data *fp_data, struct upf_accel_fp_burst_ctx *burst_ctx)
{
	struct upf_accel_entry_ctx *conn;
	enum parser_pkt_type pkt_type;
	struct rte_mbuf *pkt;
	uint16_t i;

	for (i = 0; i < burst_ctx->rx_pkts_cnt; i++) {
		pkt = burst_ctx->rx_pkts[i];
		conn = burst_ctx->conns[i];
		pkt_type = burst_ctx->pkts_type[i];

		if (burst_ctx->pkts_drop[i]) {
			upf_accel_fp_pkt_err_drop(fp_data, pkt);
			continue;
		}

#ifdef DOCA_DEBUG
		upf_accel_print_header_info(pkt, burst_ctx->matches[i], pkt_type);
#endif

		if (pkt_type == PARSER_PKT_TYPE_TUNNELED)
			upf_accel_decap(pkt, &burst_ctx->parse_ctxs[i]);

		if (is_flow_unaccelerated(pkt_type, conn))
			upf_accel_sw_aging_ll_node_move_to_head(fp_data, conn, pkt_type);

		upf_accel_md_set(pkt, conn->dyn_ctx.pdr_id[pkt_type]);
		burst_ctx->tx_pkts[burst_ctx->tx_pkts_cnt++] = burst_ctx->rx_pkts[i];
	}
}

/*
 * Initialize aging infrastructure.
 *
 * @fp_data [in]: flow processing data
 */
static void upf_accel_aging_init(struct upf_accel_fp_data *fp_data)
{
	enum upf_accel_port port_id;

	for (port_id = 0; port_id < fp_data->ctx->num_ports; port_id++)
		fp_data->last_hw_aging_tsc[port_id] = rte_rdtsc();
}

/*
 * If aging is in progress or the aging period timeout has expired, poll for up to UPF_ACCEL_MAX_NUM_AGING entries. Note
 * that this function doesn't process any potential resulting flow removals and relies on the caller to do it
 * afterwards, potentially combining it with entry additions/removal scheduled by other sources.
 *
 * @fp_data [in]: flow processing data
 * @port_id [in]: port_id of the port to remove the aged flows from
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t upf_accel_hw_aging_poll(struct upf_accel_fp_data *fp_data, enum upf_accel_port port_id)
{
	uint64_t aging_interval_tsc = UPF_ACCEL_HW_AGING_POLL_INTERVAL_SEC * rte_get_tsc_hz();
	struct upf_accel_ctx *ctx = fp_data->ctx;
	int num_aged_entries;

	if (rte_rdtsc() - fp_data->last_hw_aging_tsc[port_id] >= aging_interval_tsc) {
		if (fp_data->hw_aging_in_progress[port_id])
			DOCA_LOG_ERR("In core %d port %d: couldn't finish HW aging, continuing...",
				     fp_data->queue_id,
				     port_id);
		fp_data->hw_aging_in_progress[port_id] = true;
		fp_data->last_hw_aging_tsc[port_id] = rte_rdtsc();
	}
	if (!fp_data->hw_aging_in_progress[port_id])
		return DOCA_SUCCESS;

	num_aged_entries = doca_flow_aging_handle(ctx->ports[port_id],
						  fp_data->queue_id,
						  UPF_ACCEL_DOCA_FLOW_MAX_TIMEOUT_US,
						  UPF_ACCEL_MAX_NUM_AGING);
	if (num_aged_entries == -1)
		fp_data->hw_aging_in_progress[port_id] = false;
	else
		DOCA_LOG_DBG("In core %d port %d: num of aged entries: %d",
			     fp_data->queue_id,
			     port_id,
			     num_aged_entries);

	return DOCA_SUCCESS;
}

/*
 * Run one iteration of flow processing for specific port
 *
 * Receive burst of packets on rx port, process them accelerating any rules, if
 * necessary, and send the packets out to tx port.
 *
 * @fp_data [in]: flow processing data
 * @rx_port_id [in]: receive port id
 * @tx_port_id [in]: send port id
 */
static void upf_accel_fp_run_port(struct upf_accel_fp_data *fp_data,
				  enum upf_accel_port rx_port_id,
				  enum upf_accel_port tx_port_id)
{
	struct upf_accel_match_8t match_mem[UPF_ACCEL_MAX_PKT_BURST];
	struct upf_accel_fp_burst_ctx burst_ctx = {
		.pkts_drop = {0},
	};
	doca_error_t ret;
	uint16_t sent;

	burst_ctx.rx_pkts_cnt =
		rte_eth_rx_burst(rx_port_id, fp_data->queue_id, burst_ctx.rx_pkts, UPF_ACCEL_MAX_PKT_BURST);

	upf_accel_fp_pkts_match(&burst_ctx, match_mem);
	upf_accel_fp_conns_lookup(fp_data, rx_port_id, &burst_ctx);
	upf_accel_fp_flows_accel(fp_data, rx_port_id, &burst_ctx);
	upf_accel_fp_burst_postprocess(fp_data, &burst_ctx);

	ret = upf_accel_hw_aging_poll(fp_data, rx_port_id);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to execute HW aging poll on port %hu: %s", rx_port_id, doca_error_get_descr(ret));

	ret = doca_flow_entries_process(fp_data->ctx->ports[rx_port_id],
					fp_data->queue_id,
					UPF_ACCEL_DOCA_FLOW_MAX_TIMEOUT_US,
					0);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process flow entries on port %hu: %s", rx_port_id, doca_error_get_descr(ret));

	sent = rte_eth_tx_burst(tx_port_id, fp_data->queue_id, burst_ctx.tx_pkts, burst_ctx.tx_pkts_cnt);
	if (unlikely(sent < burst_ctx.tx_pkts_cnt)) {
		DOCA_LOG_WARN("Failed to send packets to port id %hu queue %hu", tx_port_id, fp_data->queue_id);

		do {
			upf_accel_fp_pkt_err_drop(fp_data, burst_ctx.tx_pkts[sent]);
		} while (++sent < burst_ctx.tx_pkts_cnt);
	}
}

/*
 * Run one iteration of flow processing.
 *
 * @fp_data [in]: flow processing data
 */
static void upf_accel_fp_run(struct upf_accel_fp_data *fp_data)
{
	enum upf_accel_port port_id;

	for (port_id = 0; port_id < fp_data->ctx->num_ports; port_id++)
		upf_accel_fp_run_port(fp_data, port_id, fp_data->ctx->get_fwd_port(port_id));
}

/*
 * Check and handles (if exceeds) a quota for pdr
 *
 * @ctx [in]: UPF Acceleration context
 * @pdr_id [in]: pdr ID
 * @query [in]: quota counter query
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_exceeds_quota_for_pdr(struct upf_accel_ctx *ctx,
						 uint16_t pdr_id,
						 struct doca_flow_resource_query *query)
{
	struct upf_accel_pdr *pdr = &ctx->upf_accel_cfg->pdrs->arr_pdrs[pdr_id];
	struct upf_accel_urr *urr;
	uint32_t i;

	for (i = 0; i < pdr->urrids_num; ++i) {
		urr = &ctx->upf_accel_cfg->urrs->arr_urrs[pdr->urrids[i]];
		if (query->counter.total_bytes >= urr->volume_quota_total_volume) {
			/*
			 * Quota exceeded.
			 * This code part should be implemented once a definition
			 * of how to react is complete.
			 */
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Iterates through all quota counters and handles if exceeds
 *
 * @fp_data [in]: flow processing data
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_exceeds_quotas(struct upf_accel_fp_data *fp_data)
{
	struct app_shared_counter_ids *shared_counter_ids = &fp_data->quota_cntrs;
	struct doca_flow_resource_query query_results_array[UPF_ACCEL_MAX_NUM_PDR];
	size_t cntrs_num = shared_counter_ids->cntrs_num;
	enum upf_accel_port port_id;
	doca_error_t result;
	uint16_t pdr_id;
	uint32_t i;

	if (!cntrs_num)
		return DOCA_SUCCESS;

	for (port_id = 0; port_id < fp_data->ctx->num_ports; ++port_id) {
		result = doca_flow_shared_resources_query(DOCA_FLOW_SHARED_RESOURCE_COUNTER,
							  shared_counter_ids->ids[port_id],
							  query_results_array,
							  cntrs_num);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entries: %s", doca_error_get_descr(result));
			return result;
		}

		for (i = 0; i < cntrs_num; ++i) {
			pdr_id = shared_counter_ids->cntr_0 + i;

			result = handle_exceeds_quota_for_pdr(fp_data->ctx, pdr_id, &query_results_array[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to handle quota for pdr %d: %s",
					     pdr_id,
					     doca_error_get_descr(result));
				return result;
			}
		}
	}

	return DOCA_SUCCESS;
}

void upf_accel_fp_loop(struct upf_accel_fp_data *fp_data)
{
	doca_error_t result;

	upf_accel_aging_init(fp_data);

	while (!force_quit) {
		upf_accel_fp_run(fp_data);

		upf_accel_sw_aging_ll_scan(fp_data, PARSER_PKT_TYPE_TUNNELED);
		upf_accel_sw_aging_ll_scan(fp_data, PARSER_PKT_TYPE_PLAIN);

		result = handle_exceeds_quotas(fp_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to handle expired quotas: %s", doca_error_get_descr(result));
			return;
		}
	}
}

/*
 * Deletes an extension entry
 *
 * @fp_data [in]: flow processing data
 * @ext_entry [in]: extension entry to delete
 */
static doca_error_t upf_accel_fp_delete_extension_entry(struct upf_accel_fp_data *fp_data,
							struct upf_accel_extension_entry_ctx *ext_entry)
{
	upf_accel_fp_entry_state_change(&ext_entry->state, UPF_ACCEL_ENTRY_STATE_NONE);
	if (rte_hash_del_key_with_hash(fp_data->dyn_ext_tbl, &ext_entry->match, ext_entry->hash) < 0) {
		DOCA_LOG_WARN("Failed entry aging - hash free failed");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Get packet type of a callback entry
 *
 * @dyn_ctx [in]: dynamic entry context
 * @entry [in]: DOCA Flow entry pointer
 * @return: packet type
 */
static enum parser_pkt_type upf_accel_dyn_entry_cb_get_pkt_type(struct upf_accel_dyn_entry_ctx *dyn_ctx,
								struct doca_flow_pipe_entry *entry)
{
	if (entry == dyn_ctx->entries[PARSER_PKT_TYPE_TUNNELED].entry) {
		return PARSER_PKT_TYPE_TUNNELED;
	} else if (entry == dyn_ctx->entries[PARSER_PKT_TYPE_PLAIN].entry) {
		return PARSER_PKT_TYPE_PLAIN;
	} else {
		DOCA_LOG_ERR("Unexpected entry");
		return PARSER_PKT_TYPE_UNKNOWN;
	}
}

/*
 * Dynamic entry add op
 * Changes the entry state to UPF_ACCEL_ENTRY_STATE_ACCELERATED.
 * If an extension entry is needed and doesn't exist - offloads the preceding extension entry.
 *
 * @entry_ctx [in]: entry context
 * @status [in]: DOCA Flow entry status
 * @pkt_type [in]: direction of the packet
 */
static void upf_accel_dyn_entry_cb_add(struct upf_accel_entry_ctx *entry_ctx,
				       enum doca_flow_entry_status status,
				       enum parser_pkt_type pkt_type)
{
	struct upf_accel_dyn_entry_ctx *dyn_ctx = &entry_ctx->dyn_ctx;
	struct upf_accel_flow_entry *flow_entry = &dyn_ctx->entries[pkt_type];
	struct upf_accel_entry_ctx *ext_entry_ctx = flow_entry->ext_entry;
	struct upf_accel_fp_data *fp_data = dyn_ctx->fp_data;
	struct upf_accel_fp_accel_counters *accel_counters = &fp_data->accel_counters[pkt_type];
	doca_error_t ret;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		accel_counters->accel_errors++;
		fp_data->accel_failed_counters[pkt_type].current++;
		fp_data->accel_failed_counters[pkt_type].total++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration on port %u, type %u caused by failure to process dynamic entry add",
			dyn_ctx->port_id,
			pkt_type);
	}

	upf_accel_fp_entry_state_change(&dyn_ctx->entries[pkt_type].state, UPF_ACCEL_ENTRY_STATE_ACCELERATED);
	accel_counters->current++;
	accel_counters->total++;

	if (ext_entry_ctx != NULL && !upf_accel_fp_entry_is_alive(ext_entry_ctx->ext_ctx.state)) {
		upf_accel_fp_entry_state_change(&ext_entry_ctx->ext_ctx.state,
						UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION);
		ret = pkt_type == PARSER_PKT_TYPE_TUNNELED ?
			      upf_accel_pipe_8t_ipv6_ext_accel(fp_data->ctx,
							       dyn_ctx->port_id,
							       fp_data->queue_id,
							       &ext_entry_ctx->ext_ctx.match,
							       ext_entry_ctx->ext_ctx.md_value,
							       ext_entry_ctx,
							       &ext_entry_ctx->ext_ctx.entry) :
			      upf_accel_pipe_5t_ipv6_ext_accel(fp_data->ctx,
							       dyn_ctx->port_id,
							       fp_data->queue_id,
							       &ext_entry_ctx->ext_ctx.match,
							       ext_entry_ctx->ext_ctx.md_value,
							       ext_entry_ctx,
							       &ext_entry_ctx->ext_ctx.entry);
		if (ret != DOCA_SUCCESS) {
			accel_counters->accel_errors++;
			DOCA_LOG_ERR(
				"FATAL ERROR: Unexpected failure in flow acceleration on port %u, flow type %u caused by failure to add extension entry",
				dyn_ctx->port_id,
				pkt_type);
		}
	}
}

/*
 * Dynamic entry aging op
 * Removes DOCA Flow entry, and sets status to UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION
 * If there is an extension entry with a reference count of 0 - sets its state to
 * UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION and removes it.
 *
 * @entry_ctx [in]: entry context
 * @entry [in]: DOCA Flow entry pointer
 * @status [in]: DOCA Flow entry status
 * @pipe_queue [in]: queue identifier
 * @pkt_type [in]: Direction of the packet
 */
static void upf_accel_dyn_entry_cb_age(struct upf_accel_entry_ctx *entry_ctx,
				       struct doca_flow_pipe_entry *entry,
				       enum doca_flow_entry_status status,
				       uint16_t pipe_queue,
				       enum parser_pkt_type pkt_type)
{
	struct upf_accel_dyn_entry_ctx *dyn_ctx = &entry_ctx->dyn_ctx;
	struct upf_accel_fp_accel_counters *accel_counters = &dyn_ctx->fp_data->accel_counters[pkt_type];
	struct upf_accel_entry_ctx *ext_entry_ctx = dyn_ctx->entries[pkt_type].ext_entry;
	struct doca_flow_pipe_entry *entry_to_delete = entry;
	doca_error_t ret;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		accel_counters->aging_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration on port %u, type %u caused by failure to process dynamic entry aging",
			dyn_ctx->port_id,
			pkt_type);
	}

	upf_accel_fp_entry_state_change(&dyn_ctx->entries[pkt_type].state,
					UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION);

	assert(ext_entry_ctx == NULL || ext_entry_ctx->ext_ctx.ref_cnt != 0);
	if (ext_entry_ctx != NULL && --ext_entry_ctx->ext_ctx.ref_cnt == 0) {
		upf_accel_fp_entry_state_change(&ext_entry_ctx->ext_ctx.state,
						UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION);
		ext_entry_ctx->ext_ctx.del_next = entry_to_delete;
		entry_to_delete = ext_entry_ctx->ext_ctx.entry;
	}

	ret = doca_flow_pipe_remove_entry(pipe_queue, DOCA_FLOW_WAIT_FOR_BATCH, entry_to_delete);
	if (ret != DOCA_SUCCESS) {
		accel_counters->aging_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration on port %u, type %u caused by failure to delete aged entry",
			dyn_ctx->port_id,
			pkt_type);
	} else {
		DOCA_LOG_DBG("Entry aged out of pipe");
	}
}

/*
 * Dynamic entry delete op
 * Deletes the flow if needed.
 *
 * @entry_ctx [in]: entry context
 * @status [in]: DOCA Flow entry status
 * @pkt_type [in]: direction of the packet
 */
static void upf_accel_dyn_entry_cb_del(struct upf_accel_entry_ctx *entry_ctx,
				       enum doca_flow_entry_status status,
				       enum parser_pkt_type pkt_type)
{
	struct upf_accel_dyn_entry_ctx *dyn_ctx = &entry_ctx->dyn_ctx;
	struct upf_accel_fp_data *fp_data = dyn_ctx->fp_data;
	struct upf_accel_fp_accel_counters *accel_counters = &fp_data->accel_counters[pkt_type];
	doca_error_t ret;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		accel_counters->del_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration on port %u, type %u caused by failure to process dynamic entry delete",
			dyn_ctx->port_id,
			pkt_type);
	}

	dyn_ctx->entries[pkt_type].ext_entry = NULL;
	ret = upf_accel_fp_delete_flow(fp_data, dyn_ctx, pkt_type);
	if (ret != DOCA_SUCCESS) {
		accel_counters->del_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration on port %u, flow type %u caused by failure to delete hash table entry",
			dyn_ctx->port_id,
			pkt_type);
	} else {
		accel_counters->current--;
		DOCA_LOG_DBG("Entry removed out of pipe");
	}
}

/*
 * Dynamic entry processing handler
 *
 * @entry_ctx [in]: entry context
 * @entry [in]: DOCA Flow entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 */
static void upf_accel_dyn_entry_cb(struct upf_accel_entry_ctx *entry_ctx,
				   struct doca_flow_pipe_entry *entry,
				   uint16_t pipe_queue,
				   enum doca_flow_entry_status status,
				   enum doca_flow_entry_op op)
{
	enum parser_pkt_type pkt_type;

	pkt_type = upf_accel_dyn_entry_cb_get_pkt_type(&entry_ctx->dyn_ctx, entry);
	if (pkt_type == PARSER_PKT_TYPE_UNKNOWN) {
		DOCA_LOG_ERR("Failed to get packet type");
		return;
	}

	switch (op) {
	case DOCA_FLOW_ENTRY_OP_ADD:
		upf_accel_dyn_entry_cb_add(entry_ctx, status, pkt_type);
		break;
	case DOCA_FLOW_ENTRY_OP_AGED:
		upf_accel_dyn_entry_cb_age(entry_ctx, entry, status, pipe_queue, pkt_type);
		break;
	case DOCA_FLOW_ENTRY_OP_DEL:
		upf_accel_dyn_entry_cb_del(entry_ctx, status, pkt_type);
		break;
	default:
		DOCA_LOG_ERR("UPF dynamic entry cb - bad op %u", op);
		return;
	}
}

/*
 * Extension entry add op
 * Changes the entry state to UPF_ACCEL_ENTRY_STATE_ACCELERATED.
 *
 * @entry_ctx [in]: entry context
 * @status [in]: DOCA Flow entry status
 */
static void upf_accel_ext_entry_cb_add(struct upf_accel_entry_ctx *entry_ctx, enum doca_flow_entry_status status)
{
	struct upf_accel_extension_entry_ctx *ext_entry = &entry_ctx->ext_ctx;
	enum parser_pkt_type pkt_type = ext_entry->match.te_id ? PARSER_PKT_TYPE_TUNNELED : PARSER_PKT_TYPE_PLAIN;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		ext_entry->fp_data->accel_counters[pkt_type].accel_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration, type %u caused by failure to process extension entry add",
			pkt_type);
	}

	upf_accel_fp_entry_state_change(&ext_entry->state, UPF_ACCEL_ENTRY_STATE_ACCELERATED);
}

/*
 * Extension entry delete op
 * Deletes the extension entry context and deletes the subsequent entry accelerted entry.
 *
 * @entry_ctx [in]: entry context
 * @status [in]: DOCA Flow entry status
 * @pipe_queue [in]: queue identifier
 */
static void upf_accel_ext_entry_cb_del(struct upf_accel_entry_ctx *entry_ctx,
				       enum doca_flow_entry_status status,
				       uint16_t pipe_queue)
{
	struct upf_accel_extension_entry_ctx *ext_entry = &entry_ctx->ext_ctx;
	enum parser_pkt_type pkt_type = ext_entry->match.te_id ? PARSER_PKT_TYPE_TUNNELED : PARSER_PKT_TYPE_PLAIN;
	struct doca_flow_pipe_entry *entry_to_delete = ext_entry->del_next;
	struct upf_accel_fp_data *fp_data = ext_entry->fp_data;
	doca_error_t ret;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
		fp_data->accel_counters[pkt_type].del_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration, type %u caused by failure to process extension entry delete",
			pkt_type);
	}

	ret = upf_accel_fp_delete_extension_entry(fp_data, ext_entry);
	if (ret == DOCA_SUCCESS) {
		ret = doca_flow_pipe_remove_entry(pipe_queue, DOCA_FLOW_WAIT_FOR_BATCH, entry_to_delete);
		DOCA_LOG_DBG("Entry removed out of pipe");
	} else {
		fp_data->accel_counters[pkt_type].del_errors++;
		DOCA_LOG_ERR(
			"FATAL ERROR: Unexpected failure in flow acceleration, type %u caused by failure to delete dynamic entry",
			pkt_type);
	}
}

/*
 * Extension entry processing handler
 *
 * @entry_ctx [in]: entry context
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 */
static void upf_accel_ext_entry_cb(struct upf_accel_entry_ctx *entry_ctx,
				   uint16_t pipe_queue,
				   enum doca_flow_entry_status status,
				   enum doca_flow_entry_op op)
{
	switch (op) {
	case DOCA_FLOW_ENTRY_OP_ADD:
		upf_accel_ext_entry_cb_add(entry_ctx, status);
		break;
	case DOCA_FLOW_ENTRY_OP_DEL:
		upf_accel_ext_entry_cb_del(entry_ctx, status, pipe_queue);
		break;
	default:
		DOCA_LOG_ERR("UPF extension entry cb - bad op %u", op);
		return;
	}
}

/*
 * Static entry processing handler
 *
 * @static_ctx [in]: static entry context
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 */
static void upf_accel_static_entry_cb(struct upf_accel_static_entry_ctx *static_ctx,
				      enum doca_flow_entry_status status,
				      enum doca_flow_entry_op op)
{
	struct entries_status *entries_status = &static_ctx->ctrl_status;

	switch (op) {
	case DOCA_FLOW_ENTRY_OP_ADD:
	case DOCA_FLOW_ENTRY_OP_DEL:
		if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
			entries_status->failure = true; /* Set failure to true if processing failed */
		entries_status->nb_processed++;
		break;
	default:
		DOCA_LOG_ERR("UPF static entry cb - bad op %u", op);
		return;
	}
}

void upf_accel_check_for_valid_entry_aging(struct doca_flow_pipe_entry *entry,
					   uint16_t pipe_queue,
					   enum doca_flow_entry_status status,
					   enum doca_flow_entry_op op,
					   void *user_ctx)
{
	struct upf_accel_entry_ctx *entry_ctx = (struct upf_accel_entry_ctx *)user_ctx;

	if (entry_ctx == NULL)
		return;

	if (likely(entry_ctx->type == UPF_ACCEL_RULE_DYNAMIC))
		upf_accel_dyn_entry_cb(entry_ctx, entry, pipe_queue, status, op);
	else if (likely(entry_ctx->type == UPF_ACCEL_RULE_DYNAMIC_EXTENSION))
		upf_accel_ext_entry_cb(entry_ctx, pipe_queue, status, op);
	else
		upf_accel_static_entry_cb(&entry_ctx->static_ctx, status, op);
}
