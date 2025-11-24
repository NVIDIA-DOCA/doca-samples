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

#ifndef RMAX_COMMON_H_
#define RMAX_COMMON_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <doca_argp.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_rmax.h>

#include "common.h"

#define MAX_BUFFERS 2 /* max number of buffers used by the samples */

/*
 * Struct containing all the configurations that needed to set stream and flow parameters.
 */
struct rmax_stream_config {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* device PCI address */

	/* used when setting stream attributes */
	bool scatter_all;	/* scatter all packet data including network headers */
	bool order_by_rtp_seqn; /* enable packet ordering by RTP sequence number */
	uint16_t hdr_size;	/* header size */
	uint16_t data_size;	/* payload size */
	uint32_t num_elements;	/* number of elements in the stream buffer */

	/* used when setting flow attributes */
	struct in_addr dst_ip; /* destination IP address */
	struct in_addr src_ip; /* source IP address */
	uint16_t dst_port;     /* destination port */
};

/*
 * Struct containing state of a rivermax sample
 */
struct rmax_program_state {
	struct rmax_stream_config *config;	  /* pointer to the stream configurations */
	struct program_core_objects core_objects; /* the DOCA core objects */
	uint16_t stride_size[MAX_BUFFERS];	  /* the stride sizes queried from doca_rmax_in_stream */
	bool run_pe_progress;			  /* controls whether to keep progressing the PE */
	doca_error_t exit_status;		  /* reflects status of last event */
};

/*
 * Register the command line parameter for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_create_stream_params(void);

/*
 * Set DOCA Rivermax flow relevant parameters, such as src/dst port and IP addresses.
 *
 * @config [in]: ca configurations containing all parameters of the flow that needed to be set
 * @flow [in]: the flow to set its parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rmax_flow_set_attributes(struct rmax_stream_config *config, struct doca_rmax_flow *flow);

/*
 * Set DOCA Rivermax stream relevant parameters, such as buffer size, packet size.
 *
 * @stream [in]: the stream to set its attributes
 * @config [in]: configurations containing all parameters of the stream that needed to be set
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rmax_stream_set_attributes(struct doca_rmax_in_stream *stream, struct rmax_stream_config *config);

/*
 * Start the DOCA rivermax context.
 *
 * @state [in]: struct rmax_program_state containing all the program state
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rmax_stream_start(struct rmax_program_state *state);

/*
 * Allocate buffers for received data storage, query stride size.
 *
 * @state [in]: struct rmax_program_state containing all the program state
 * @stream [in]: the stream to set its attributes
 * @config [in]: configurations containing all parameters of the stream that needed to be set
 * @buffer [out]: the allocated stream buffer
 * @stride_size [out]: stride size of memory block
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rmax_stream_allocate_buf(struct rmax_program_state *state,
				      struct doca_rmax_in_stream *stream,
				      struct rmax_stream_config *config,
				      struct doca_buf **buffer,
				      uint16_t *stride_size);

/*
 * Clean all the sample resources
 *
 * @state [in]: struct rmax_program_state containing all the program state
 * @stream [in]: DOCA Rivermax stream to destroy
 * @flow [in]: rmax flow to destroy
 * @buf [in]: allocated DOCA buffer that should be destroyed
 */
void rmax_create_stream_cleanup(struct rmax_program_state *state,
				struct doca_rmax_in_stream *stream,
				struct doca_rmax_flow *flow,
				struct doca_buf *buf);

#endif /* RMAX_COMMON_H_ */
