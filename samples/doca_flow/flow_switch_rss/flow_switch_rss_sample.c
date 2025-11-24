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

#include <string.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_net.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_dev.h>

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_SWITCH_SWITCH_RSS);

enum ingress_vport_entry_type {
	INGRESS_TO_PORT0,
	INGRESS_TO_PORT1,
	INGRESS_TO_PORT2,
	INGRESS_TO_PORT_MAX,
};

enum switch_rss_basic_pipe_type {
	SWITCH_RSS_BASIC_PIPE_CONST_RSS,  /* Constant RSS fwd in pipe */
	SWITCH_RSS_BASIC_PIPE_DYN_RSS,	  /* Dynamic RSS fwd in entry */
	SWITCH_RSS_BASIC_PIPE_SHARED_RSS, /* null fwd with shared RSS in entry */
	SWITCH_RSS_BASIC_PIPE_MAX,
};

enum switch_rss_control_entry_type {
	SWITCH_RSS_CONTROL_IMM_RSS,
	SWITCH_RSS_CONTROL_SHARED_RSS,
	SWITCH_RSS_CONTROL_MAX,
};

enum ingress_root_entry_type {
	INGRESS_ROOT_TO_BASIC = SWITCH_RSS_BASIC_PIPE_MAX,
	INGRESS_ROOT_TO_CONTROL = INGRESS_ROOT_TO_BASIC + SWITCH_RSS_CONTROL_MAX,
	INGRESS_ROOT_TO_EGRESS = INGRESS_ROOT_TO_CONTROL * 2,
	INGRESS_ROOT_TO_VPORT = INGRESS_ROOT_TO_EGRESS + 1,
	INGRESS_ROOT_TO_MAX = INGRESS_ROOT_TO_VPORT,
};

enum switch_rss_pipe_dom_type {
	SWITCH_RSS_PIPE_DOM_INGRESS, /* Network to host direction pipe */
	SWITCH_RSS_PIPE_DOM_EGRESS,  /* Host to network direction pipe */
	SWITCH_RSS_PIPE_DOM_MAX,
};

#define NB_EGRESS_ENTRIES (SWITCH_RSS_BASIC_PIPE_MAX + SWITCH_RSS_CONTROL_MAX)

#define NB_INGRESS_ENTRIES INGRESS_ROOT_TO_MAX

#define NB_VPORT_ENTRIES INGRESS_TO_PORT_MAX

#define NB_TOTAL_ENTRIES \
	(1 + NB_INGRESS_ENTRIES + NB_EGRESS_ENTRIES + NB_VPORT_ENTRIES + \
	 (SWITCH_RSS_BASIC_PIPE_MAX + SWITCH_RSS_CONTROL_MAX) * SWITCH_RSS_PIPE_DOM_MAX)

#define MAX_PKTS 16

#define WAIT_SECS 15

static struct doca_flow_pipe *pipe_egress;
static struct doca_flow_pipe *pipe_ingress;
static struct doca_flow_pipe *pipe_vport;
static struct doca_flow_pipe *pipe_rss;

static struct doca_flow_pipe *pipe_basic_switch_rss[SWITCH_RSS_PIPE_DOM_MAX][SWITCH_RSS_BASIC_PIPE_MAX];
static struct doca_flow_pipe *pipe_control_switch_rss[SWITCH_RSS_PIPE_DOM_MAX];

static struct doca_flow_pipe_entry *entry_basic_switch_rss[SWITCH_RSS_PIPE_DOM_MAX][SWITCH_RSS_BASIC_PIPE_MAX];
static struct doca_flow_pipe_entry *entry_control_switch_rss[SWITCH_RSS_PIPE_DOM_MAX][SWITCH_RSS_CONTROL_MAX];
static struct doca_flow_pipe_entry *rss_entry;

static uint16_t basic_queue_map[SWITCH_RSS_PIPE_DOM_MAX][SWITCH_RSS_BASIC_PIPE_MAX] = {
	{
		0,
		1,
		2,
	},
	{
		3,
		4,
		5,
	},
};
static uint16_t control_queue_map[SWITCH_RSS_PIPE_DOM_MAX][SWITCH_RSS_CONTROL_MAX] = {
	{
		6,
		7,
	},
	{
		8,
		9,
	},
};

#define MAX_RSS_QUEUE 10

static uint32_t shared_rss_ids[MAX_RSS_QUEUE];

/* array for storing created egress entries */
static struct doca_flow_pipe_entry *egress_entries[NB_EGRESS_ENTRIES];

/* array for storing created ingress entries */
static struct doca_flow_pipe_entry *ingress_entries[NB_INGRESS_ENTRIES];

/* array for storing created ingress entries */
static struct doca_flow_pipe_entry *vport_entries[NB_VPORT_ENTRIES];

/*
 * Handle received traffic and check the pkt_meta value added by internal pipe.
 *
 * @port_id [in]: proxy port id
 * @nb_queues [in]: number of queues the sample has
 */
static void handle_rx_tx_pkts(uint32_t port_id, uint16_t nb_queues)
{
	int rc;
	uint32_t queue_id;
	uint32_t secs = WAIT_SECS;
	uint32_t nb_rx;
	uint32_t i;
	uint32_t sw_packet_type;
	struct rte_mbuf *mbufs[MAX_PKTS];

	rc = rte_flow_dynf_metadata_register();
	if (unlikely(rc)) {
		DOCA_LOG_ERR("Enable metadata failed");
		return;
	}

	while (secs--) {
		sleep(1);
		for (queue_id = 0; queue_id < nb_queues; queue_id++) {
			nb_rx = rte_eth_rx_burst(port_id, queue_id, mbufs, MAX_PKTS);
			for (i = 0; i < nb_rx; i++) {
				sw_packet_type = rte_net_get_ptype(mbufs[i], NULL, RTE_PTYPE_ALL_MASK);
				if (mbufs[i]->ol_flags & RTE_MBUF_F_RX_FDIR_ID)
					DOCA_LOG_INFO("The pkt meta:0x%x, src_q:%d, type:0x%x",
						      mbufs[i]->hash.fdir.hi,
						      queue_id,
						      sw_packet_type);
				else
					DOCA_LOG_INFO("The pkt src_q:%d, type: 0x%x\n", queue_id, sw_packet_type);
				rte_pktmbuf_free(mbufs[i]);
			}
		}
	}
}

/*
 * Create DOCA Flow pipe with 5 tuple match, and forward RSS
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_rss_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint16_t rss_queues[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	/* L3 match */
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RSS_META_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	/* RSS queue - send matched traffic to queue 0  */
	rss_queues[0] = 0;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry with example 5 tuple
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_rss_pipe_entry(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	result = doca_flow_pipe_add_entry(0, pipe, &match, 0, &actions, NULL, NULL, 0, status, &rss_entry);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow control pipe
 *
 * @port [in]: port of the pipe
 * @dir [in]: pipe direction
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_switch_rss_control_pipe(struct doca_flow_port *port,
						   enum switch_rss_pipe_dom_type dir,
						   struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "CONTROL_PIPE", DOCA_FLOW_PIPE_CONTROL, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (dir == SWITCH_RSS_PIPE_DOM_INGRESS) {
		result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	} else {
		result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe with 5 tuple match, and forward RSS
 *
 * @port [in]: port of the pipe
 * @dir [in]: pipe direction
 * @type [in]: pipe type
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_switch_rss_pipe(struct doca_flow_port *port,
					   enum switch_rss_pipe_dom_type dir,
					   enum switch_rss_basic_pipe_type type,
					   struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd *pfwd = &fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	uint16_t rss_queues[1];
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));
	memset(&monitor, 0, sizeof(monitor));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dst_ip = 0xffffffff;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "RSS_META_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	if (dir == SWITCH_RSS_PIPE_DOM_INGRESS) {
		result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	} else {
		result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	if (type == SWITCH_RSS_BASIC_PIPE_SHARED_RSS) {
		fwd.type = DOCA_FLOW_FWD_CHANGEABLE;
	} else if (type == SWITCH_RSS_BASIC_PIPE_DYN_RSS) {
		fwd.type = DOCA_FLOW_FWD_RSS;
		fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		fwd.rss.nr_queues = UINT32_MAX;
	} else {
		/* RSS queue - send matched traffic to queue 0  */
		rss_queues[0] = basic_queue_map[dir][type];
		fwd.type = DOCA_FLOW_FWD_RSS;
		fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		fwd.rss.queues_array = rss_queues;
		fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4;
		fwd.rss.nr_queues = 1;
	}

	result = doca_flow_pipe_create(pipe_cfg, pfwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe with 5 tuple match on the switch port.
 * Matched traffic will be forwarded to the port defined per entry.
 * Unmatched traffic will be dropped.
 *
 * @sw_port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_switch_egress_pipe(struct doca_flow_port *sw_port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	/* Source, destination IP addresses and source, destination TCP ports are defined per entry */
	match.outer.ip4.src_ip = 0xffffffff;

	fwd.type = DOCA_FLOW_FWD_PIPE;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, sw_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SWITCH_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_EGRESS_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
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
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe with 5 tuple match on the switch port.
 * Matched traffic will be forwarded to the port defined per entry.
 * Unmatched traffic will be dropped.
 *
 * @sw_port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_switch_ingress_pipe(struct doca_flow_port *sw_port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	/* Source, destination IP addresses and source, destination TCP ports are defined per entry */
	match.outer.ip4.src_ip = 0xffffffff;

	/* Port ID to forward to is defined per entry */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = NULL;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, sw_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SWITCH_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, INGRESS_ROOT_TO_MAX);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow pipe with 5 tuple match on the switch port.
 * Matched traffic will be forwarded to the port defined per entry.
 * Unmatched traffic will be dropped.
 *
 * @sw_port [in]: switch port
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_switch_vport_pipe(struct doca_flow_port *sw_port, struct doca_flow_pipe **pipe)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&pipe_cfg, 0, sizeof(pipe_cfg));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	/* Source IP addresses and source TCP ports are defined per entry */
	match.outer.ip4.dst_ip = 0xffffffff;

	/* Port ID to forward to is defined per entry */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 0xffff;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, sw_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "SWITCH_VPORT_PIPE", DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_VPORT_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add DOCA Flow pipe entry to the pipe
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_switch_egress_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	doca_error_t result;
	int entry_index = 0;
	doca_be32_t src_ip_addr;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	for (entry_index = 0; entry_index < NB_EGRESS_ENTRIES; entry_index++) {
		src_ip_addr = BE_IPV4_ADDR(1, 2, 3, 4 + INGRESS_ROOT_TO_CONTROL + entry_index);
		match.outer.ip4.src_ip = src_ip_addr;

		fwd.type = DOCA_FLOW_FWD_PIPE;
		if (entry_index < SWITCH_RSS_BASIC_PIPE_MAX)
			fwd.next_pipe = pipe_basic_switch_rss[SWITCH_RSS_PIPE_DOM_EGRESS][entry_index];
		else if (entry_index < INGRESS_ROOT_TO_CONTROL)
			fwd.next_pipe = pipe_control_switch_rss[SWITCH_RSS_PIPE_DOM_EGRESS];

		result = doca_flow_pipe_add_entry(0,
						  pipe,
						  &match,
						  0,
						  NULL,
						  NULL,
						  &fwd,
						  flags,
						  status,
						  &egress_entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry %d: %s", entry_index, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow pipe entry to the pipe
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_switch_ingress_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	doca_error_t result;
	int entry_index = 0;
	doca_be32_t src_ip_addr;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	for (entry_index = 0; entry_index < NB_INGRESS_ENTRIES; entry_index++) {
		src_ip_addr = BE_IPV4_ADDR(1, 2, 3, 4 + entry_index);
		match.outer.ip4.src_ip = src_ip_addr;

		fwd.type = DOCA_FLOW_FWD_PIPE;
		if (entry_index < INGRESS_ROOT_TO_BASIC)
			fwd.next_pipe = pipe_basic_switch_rss[SWITCH_RSS_PIPE_DOM_INGRESS][entry_index];
		else if (entry_index < INGRESS_ROOT_TO_CONTROL)
			fwd.next_pipe = pipe_control_switch_rss[SWITCH_RSS_PIPE_DOM_INGRESS];
		else if (entry_index < INGRESS_ROOT_TO_EGRESS)
			fwd.next_pipe = pipe_egress;
		else if (entry_index < INGRESS_ROOT_TO_VPORT)
			fwd.next_pipe = pipe_vport;
		result = doca_flow_pipe_add_entry(0,
						  pipe,
						  &match,
						  0,
						  NULL,
						  NULL,
						  &fwd,
						  flags,
						  status,
						  &ingress_entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry %d: %s", entry_index, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow pipe entry to the pipe
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_switch_vport_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	enum doca_flow_flags_type flags = DOCA_FLOW_WAIT_FOR_BATCH;
	doca_error_t result;
	int entry_index = 0;
	doca_be32_t dst_ip_addr;

	memset(&fwd, 0, sizeof(fwd));
	memset(&match, 0, sizeof(match));
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	for (entry_index = 0; entry_index < NB_VPORT_ENTRIES; entry_index++) {
		dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8 + entry_index);

		match.outer.ip4.dst_ip = dst_ip_addr;

		fwd.type = DOCA_FLOW_FWD_PORT;
		/* First port as wire to wire, second wire to VF */
		fwd.port_id = entry_index;

		result = doca_flow_pipe_add_entry(0,
						  pipe,
						  &match,
						  0,
						  NULL,
						  NULL,
						  &fwd,
						  flags,
						  status,
						  &vport_entries[entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow SWITCH RSS pipes match on the switch port.
 * Matched traffic will be forwarded to the RSS queue defined per entry.
 * Unmatched traffic will be dropped.
 *
 * @port [in]: switch port
 * @dir [in]: pipe direction
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static int create_switch_rss_pipes(struct doca_flow_port *port, enum switch_rss_pipe_dom_type dir)
{
	doca_error_t result;
	int i;

	result = create_switch_rss_control_pipe(port, dir, &pipe_control_switch_rss[dir]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create %d control pipe: %s", dir, doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < SWITCH_RSS_BASIC_PIPE_MAX; i++) {
		result = create_switch_rss_pipe(port, dir, i, &pipe_basic_switch_rss[dir][i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create %d basic pipe %d: %s", dir, i, doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Add DOCA Flow RSS pipe entry to the pipe
 *
 * @dir [in]: pipe direction
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static int add_switch_rss_pipe_entries(enum switch_rss_pipe_dom_type dir, struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd *efwd = &fwd;
	enum doca_flow_flags_type flags = 0; // DOCA_FLOW_WAIT_FOR_BATCH;
	doca_error_t result;
	int entry_index = 0;
	doca_be32_t dst_ip_addr;
	uint16_t rss_queues[1];
	struct doca_flow_monitor monitor;

	memset(&match, 0, sizeof(match));
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	for (entry_index = 0; entry_index < SWITCH_RSS_BASIC_PIPE_MAX; entry_index++) {
		dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8);
		match.outer.ip4.dst_ip = dst_ip_addr;

		memset(&fwd, 0, sizeof(fwd));
		if (entry_index == SWITCH_RSS_BASIC_PIPE_SHARED_RSS) {
			fwd.type = DOCA_FLOW_FWD_RSS;
			fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
			fwd.shared_rss_id = shared_rss_ids[basic_queue_map[dir][entry_index]];
		} else {
			rss_queues[0] = basic_queue_map[dir][entry_index];
			fwd.type = DOCA_FLOW_FWD_RSS;
			fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
			fwd.rss.queues_array = rss_queues;
			fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4;
			fwd.rss.nr_queues = 1;
		}

		result = doca_flow_pipe_add_entry(0,
						  pipe_basic_switch_rss[dir][entry_index],
						  &match,
						  0,
						  NULL,
						  NULL,
						  efwd,
						  flags,
						  status,
						  &entry_basic_switch_rss[dir][entry_index]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe %d entry: %s", entry_index, doca_error_get_descr(result));
			return result;
		}
	}

	for (entry_index = 0; entry_index < SWITCH_RSS_CONTROL_MAX; entry_index++) {
		dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8 + entry_index);
		match.outer.ip4.dst_ip = dst_ip_addr;

		memset(&fwd, 0, sizeof(fwd));
		memset(&monitor, 0, sizeof(monitor));
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		if (entry_index == SWITCH_RSS_CONTROL_IMM_RSS) {
			/* RSS queue - send matched traffic to queue 0	*/
			rss_queues[0] = control_queue_map[dir][entry_index];
			fwd.type = DOCA_FLOW_FWD_RSS;
			fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
			fwd.rss.queues_array = rss_queues;
			fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4;
			fwd.rss.nr_queues = 1;
		} else if (entry_index == SWITCH_RSS_CONTROL_SHARED_RSS) {
			fwd.type = DOCA_FLOW_FWD_RSS;
			fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_SHARED;
			fwd.shared_rss_id = shared_rss_ids[control_queue_map[dir][entry_index]];
		}
		result = doca_flow_pipe_control_add_entry(0,
							  entry_index,
							  pipe_control_switch_rss[dir],
							  &match,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  &monitor,
							  efwd,
							  status,
							  &entry_control_switch_rss[dir][entry_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add pipe entry: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Run flow_switch_rss sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @nb_ports [in]: number of ports the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_switch_rss(int nb_queues, int nb_ports, struct flow_switch_ctx *ctx)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	uint32_t actions_mem_size[nb_ports];
	struct doca_flow_resource_query query_stats;
	struct entries_status status;
	doca_error_t result;
	int entry_idx;
	uint32_t shared_rss_id;
	struct doca_flow_shared_resource_cfg cfg = {0};
	struct doca_flow_resource_rss_cfg rss_cfg = {0};
	const char *start_str;
	bool is_expert = ctx->is_expert;
	int i;
	uint16_t queues[1];

	memset(&status, 0, sizeof(status));
	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = 2 * NB_TOTAL_ENTRIES; /* counter per entry */
	resource.nr_rss = 10;
	/* Use isolated mode as we will create the RSS pipe later */
	if (is_expert)
		start_str = "switch,hws,isolated,hairpinq_num=4,expert";
	else
		start_str = "switch,hws,isolated,hairpinq_num=4";
	result = init_doca_flow(nb_queues, start_str, &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NB_TOTAL_ENTRIES));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	rss_cfg.outer_flags = DOCA_FLOW_RSS_IPV4;
	rss_cfg.nr_queues = 1;
	rss_cfg.queues_array = queues;
	cfg.rss_cfg = rss_cfg;
	/* config shared rss with dest */
	for (i = 0; i < MAX_RSS_QUEUE; i++) {
		queues[0] = i;
		result = doca_flow_port_shared_resource_get(doca_flow_port_switch_get(ports[0]),
							    DOCA_FLOW_SHARED_RESOURCE_RSS,
							    &shared_rss_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get shared resource RSS %d", i);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		result = doca_flow_port_shared_resource_set_cfg(doca_flow_port_switch_get(ports[0]),
								DOCA_FLOW_SHARED_RESOURCE_RSS,
								shared_rss_id,
								&cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to cfg shared rss %d", i);
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		shared_rss_ids[i] = i;
	}

	/* Create rss pipe and entry */
	result = create_rss_pipe(doca_flow_port_switch_get(ports[0]), &pipe_rss);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rss pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_rss_pipe_entry(pipe_rss, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* Create Newtowrk to host rss pipes */
	result = create_switch_rss_pipes(doca_flow_port_switch_get(ports[0]), SWITCH_RSS_PIPE_DOM_INGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rx rss pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_switch_rss_pipe_entries(SWITCH_RSS_PIPE_DOM_INGRESS, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add rx entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* Create bi-direction rss pipe */
	result = create_switch_rss_pipes(doca_flow_port_switch_get(ports[0]), SWITCH_RSS_PIPE_DOM_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create unified rss pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_switch_rss_pipe_entries(SWITCH_RSS_PIPE_DOM_EGRESS, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add unified entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* Create egress pipe and entries */
	result = create_switch_egress_pipe(doca_flow_port_switch_get(ports[0]), &pipe_egress);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create egress pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_switch_egress_pipe_entries(pipe_egress, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add egress_entries to the pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* Create vport pipe and entries */
	result = create_switch_vport_pipe(doca_flow_port_switch_get(ports[0]), &pipe_vport);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vport pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_switch_vport_pipe_entries(pipe_vport, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add vport_entries to the pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	/* Create ingress pipe and entries */
	result = create_switch_ingress_pipe(doca_flow_port_switch_get(ports[0]), &pipe_ingress);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ingress pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result = add_switch_ingress_pipe_entries(pipe_ingress, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add ingress_entries to the pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	result =
		doca_flow_entries_process(doca_flow_port_switch_get(ports[0]), 0, DEFAULT_TIMEOUT_US, NB_TOTAL_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process egress_entries: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	if (status.nb_processed != NB_TOTAL_ENTRIES || status.failure) {
		DOCA_LOG_ERR("Failed to process all entries %d", status.nb_processed);
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");

	handle_rx_tx_pkts(0, nb_queues);

	/* dump egress entries counters */
	for (entry_idx = 0; entry_idx < NB_EGRESS_ENTRIES; entry_idx++) {
		result = doca_flow_resource_query_entry(egress_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Egress Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	for (entry_idx = 0; entry_idx < NB_VPORT_ENTRIES; entry_idx++) {
		result = doca_flow_resource_query_entry(vport_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query vport pipe entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Vport Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	for (entry_idx = 0; entry_idx < NB_INGRESS_ENTRIES; entry_idx++) {
		result = doca_flow_resource_query_entry(ingress_entries[entry_idx], &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Ingress Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	for (entry_idx = 0; entry_idx < SWITCH_RSS_BASIC_PIPE_MAX; entry_idx++) {
		result = doca_flow_resource_query_entry(entry_basic_switch_rss[SWITCH_RSS_PIPE_DOM_INGRESS][entry_idx],
							&query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Rx Basic PIPE Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	for (entry_idx = 0; entry_idx < SWITCH_RSS_BASIC_PIPE_MAX; entry_idx++) {
		result = doca_flow_resource_query_entry(entry_basic_switch_rss[SWITCH_RSS_PIPE_DOM_EGRESS][entry_idx],
							&query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Unified Basic PIPE Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	for (entry_idx = 0; entry_idx < SWITCH_RSS_CONTROL_MAX; entry_idx++) {
		result = doca_flow_resource_query_entry(entry_control_switch_rss[SWITCH_RSS_PIPE_DOM_EGRESS][entry_idx],
							&query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Rx Control PIPE Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	for (entry_idx = 0; entry_idx < SWITCH_RSS_CONTROL_MAX; entry_idx++) {
		result = doca_flow_resource_query_entry(entry_control_switch_rss[SWITCH_RSS_PIPE_DOM_EGRESS][entry_idx],
							&query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			stop_doca_flow_ports(nb_ports, ports);
			doca_flow_destroy();
			return result;
		}
		DOCA_LOG_INFO("Unified Control PIPE Entry in index: %d", entry_idx);
		DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
		DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);
	}

	result = doca_flow_resource_query_entry(rss_entry, &query_stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}
	DOCA_LOG_INFO("Miss RSS Entry");
	DOCA_LOG_INFO("Total bytes: %ld", query_stats.counter.total_bytes);
	DOCA_LOG_INFO("Total packets: %ld", query_stats.counter.total_pkts);

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
