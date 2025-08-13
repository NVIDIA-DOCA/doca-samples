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

#ifndef UPF_ACCEL_PIPELINE_H_
#define UPF_ACCEL_PIPELINE_H_

#include "upf_accel.h"

#define UPF_ACCEL_PACKET_BURST 128

#define UPF_ACCEL_LOG_MAX_NUM_QER 16
#define UPF_ACCEL_MAX_NUM_QER (1ul << UPF_ACCEL_LOG_MAX_NUM_QER)
#define UPF_ACCEL_LOG_MAX_NUM_URR 16
#define UPF_ACCEL_MAX_NUM_URR (1ul << UPF_ACCEL_LOG_MAX_NUM_URR)
#define UPF_ACCEL_LOG_MAX_NUM_VNIS 10
#define UPF_ACCEL_MAX_NUM_VNIS (1ul << UPF_ACCEL_LOG_MAX_NUM_VNIS)

#define UPF_ACCEL_VERSION_IHL_IPV4 0x4500
#define UPF_ACCEL_VERSION_IPV6 0x6000
#define UPF_ACCEL_ENCAP_TTL UINT8_MAX

#define UPF_ACCEL_QFI_NONE 0

/*
 * Extra space in the software hash table for flows that failed to be
 * accelerated due to lack of space in the hardware table.
 */
#define UPF_ACCEL_MAX_NUM_FAILED_ACCEL_PER_CORE (1 << 15)

/*
 * Adds a static table entry to a table
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @port_id [in]: Port ID.
 * @pipe_queue [in]: Queue identifier.
 * @pipe [in]: Pointer to pipe.
 * @match [in]: Pointer to match, indicate specific packet match information.
 * @actions [in]: Pointer to modify actions, indicate specific modify information.
 * @mon [in]: Pointer to monitor actions.
 * @fwd [in]: Pointer to fwd actions.
 * @flags [in]: Flow entry will be pushed to hw immediately or not. enum doca_flow_flags_type.
 * @usr_ctx [in]: Pointer to user context.
 * @entry [out]: Pipe entry handler on success.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t upf_accel_pipe_static_entry_add(struct upf_accel_ctx *upf_accel_ctx,
					     enum upf_accel_port port_id,
					     uint16_t pipe_queue,
					     struct doca_flow_pipe *pipe,
					     const struct doca_flow_match *match,
					     const struct doca_flow_actions *actions,
					     const struct doca_flow_monitor *mon,
					     const struct doca_flow_fwd *fwd,
					     uint32_t flags,
					     void *usr_ctx,
					     struct doca_flow_pipe_entry **entry);

/*
 * Create pipeline
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t upf_accel_pipeline_create(struct upf_accel_ctx *upf_accel_ctx);

#endif /* UPF_ACCEL_PIPELINE_H_ */
