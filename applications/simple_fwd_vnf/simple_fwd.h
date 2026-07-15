/*
 * Copyright (c) 2021 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef SIMPLE_FWD_H_
#define SIMPLE_FWD_H_

#include <stdint.h>
#include <stdbool.h>

#include <doca_flow.h>

#include "simple_fwd_pkt.h"
#include "simple_fwd_port.h"

#define SIMPLE_FWD_PORTS (2)	    /* Number of ports used by the application */
#define SIMPLE_FWD_MAX_FLOWS (8096) /* Maximum number of flows used/added by the application at a given time */

/* Application resources, such as flow table, pipes and hairpin peers */
struct simple_fwd_app {
	struct simple_fwd_ft *ft;			       /* Flow table, used for stprng flows */
	uint16_t hairpin_peer[SIMPLE_FWD_PORTS];	       /* Binded pair ports array*/
	struct doca_flow_port *ports[SIMPLE_FWD_PORTS];	       /* DOCA Flow ports array used by the application */
	struct doca_flow_pipe *pipe_vxlan[SIMPLE_FWD_PORTS];   /* VXLAN pipe of each port */
	struct doca_flow_pipe *pipe_gre[SIMPLE_FWD_PORTS];     /* GRE pipe of each port */
	struct doca_flow_pipe *pipe_gtp[SIMPLE_FWD_PORTS];     /* GTP pipe of each port */
	struct doca_flow_pipe *pipe_control[SIMPLE_FWD_PORTS]; /* control pipe of each port */
	struct doca_flow_pipe *pipe_hairpin[SIMPLE_FWD_PORTS]; /* hairpin pipe for non-VxLAN/GRE/GTP traffic */
	struct doca_flow_pipe *pipe_rss[SIMPLE_FWD_PORTS];     /* RSS pipe, matches every packet and forwards to SW */
	struct doca_flow_pipe *vxlan_encap_pipe[SIMPLE_FWD_PORTS]; /* vxlan encap pipe on the egress domain */
	uint16_t nb_queues;					   /* flow age query item buffer */
	struct doca_flow_aged_query *query_array[0];		   /* buffer for flow aged query items */
};

/* Simple FWD flow entry representation */
struct simple_fwd_pipe_entry {
	bool is_hw;			       /* Wether the entry in HW or not */
	uint64_t total_pkts;		       /* Total number of packets matched the flow */
	uint64_t total_bytes;		       /* Total number of bytes matched the flow */
	uint16_t pipe_queue;		       /* Pipe queue of the flow entry */
	struct doca_flow_pipe_entry *hw_entry; /* a pointer for the flow entry in hw */
};

/*
 * fills struct app_vnf with init/destroy, process and other needed pointer functions.
 *
 * @return: a pointer to struct app_vnf which contains all needed pointer functions
 */
struct app_vnf *simple_fwd_get_vnf(void);

#endif /* SIMPLE_FWD_H_ */
