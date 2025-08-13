/*
 * Copyright (c) 2025-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef IP_FRAG_DP_H_
#define IP_FRAG_DP_H_

#include <stdbool.h>

#include <doca_error.h>
#include <doca_flow.h>

#include <flow_common.h>

#include <dpdk_utils.h>

#define UNUSED(x) ((void)(x))

enum ip_frag_mode {
	IP_FRAG_MODE_BIDIR,
	IP_FRAG_MODE_MULTIPORT,
};

enum ip_frag_port {
	IP_FRAG_PORT_REASSEMBLE_0,
	IP_FRAG_PORT_FRAGMENT_0,
	IP_FRAG_PORT_REASSEMBLE_1,
	IP_FRAG_PORT_FRAGMENT_1,
	IP_FRAG_PORT_NUM,
};

enum ip_frag_rss_pipe {
	IP_FRAG_RSS_PIPE_IPV4,
	IP_FRAG_RSS_PIPE_IPV6,
	IP_FRAG_RSS_PIPE_NUM,
};

struct ip_frag_config {
	enum ip_frag_mode mode;		   /* Application mode */
	uint64_t mbuf_flag_outer_modified; /* RTE mbuf outer fragmentation flag mask */
	uint64_t mbuf_flag_inner_modified; /* RTE mbuf inner fragmentation flag mask */
	uint16_t mtu;			   /* MTU */
	bool mbuf_chain;		   /* Use chained mbuf optimization */
	bool hw_cksum;			   /* Use hardware checksum optimization */
	uint32_t frag_tbl_timeout;	   /* Fragmentation table timeout in ms */
	uint32_t frag_tbl_size;		   /* Fragmentation table size */
	struct flow_dev_ctx flow_devs;	   /* Flow device context */
};

struct ip_frag_pipe_cfg {
	uint16_t port_id;		    /* Port ID */
	struct doca_flow_port *port;	    /* DOCA Flow port */
	enum doca_flow_pipe_domain domain;  /* Domain (RX/TX) */
	char *name;			    /* Pipe name */
	bool is_root;			    /* Flag to indicate if the pipe is root */
	uint32_t num_entries;		    /* Number of entries */
	struct doca_flow_match *match;	    /* Match */
	struct doca_flow_match *match_mask; /* Match mask */
	struct doca_flow_fwd *fwd;	    /* Forward hit */
	struct doca_flow_fwd *fwd_miss;	    /* Forward miss */
};

struct ip_frag_ctx {
	uint16_t num_ports;						      /* Number of ports */
	uint16_t num_queues;						      /* Number of device queues */
	struct doca_flow_pipe *pipes[IP_FRAG_PORT_NUM][IP_FRAG_RSS_PIPE_NUM]; /* Pipes */
	struct doca_flow_port *ports[IP_FRAG_PORT_NUM];			      /* Ports */
	struct doca_dev *dev_arr[IP_FRAG_PORT_NUM];			      /* Devices array */
};

extern bool force_stop;

/*
 * IP fragmentation application logic
 *
 * @cfg [in]: application config
 * @dpdk_cfg [in]: application DPDK configuration values
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ip_frag(struct ip_frag_config *cfg, struct application_dpdk_config *dpdk_cfg);

#endif /* IP_FRAG_DP_H_ */
