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

#ifndef COMMON_FLOW_PARSER_H_
#define COMMON_FLOW_PARSER_H_

#include <doca_flow.h>

/*
 * Parse IPv4 string
 *
 * @str_ip [in]: String to parse
 * @ipv4_addr [out]: Big endian IPv4 address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t parse_ipv4_str(const char *str_ip, doca_be32_t *ipv4_addr);

/*
 * Parse network layer protocol
 *
 * @protocol_str [in]: String to parse
 * @protocol [out]: Protocol identifier number
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t parse_protocol_string(const char *protocol_str, enum doca_flow_l4_type_ext *protocol);

/*
 * Set match l4 protocol
 *
 * @format [in]: outer or inner match format
 * @protocol [in]: protocol
 */
void set_match_l4_protocol(struct doca_flow_header_format *format, uint8_t protocol);

/*
 * Set the function to be called once pipe create command is entered
 *
 * @action [in]: Function callback
 */
void set_pipe_create(void (*action)(struct doca_flow_pipe_cfg *cfg,
				    uint16_t port_id,
				    struct doca_flow_fwd *fwd,
				    uint64_t fw_pipe_id,
				    struct doca_flow_fwd *fwd_miss,
				    uint64_t fw_miss_pipe_id));

/*
 * Set the function to be called once add entry command is entered
 *
 * @action [in]: Function callback
 */
void set_pipe_add_entry(void (*action)(uint16_t pipe_queue,
				       uint64_t pipe_id,
				       struct doca_flow_match *match,
				       uint8_t action_idx,
				       struct doca_flow_actions *actions,
				       struct doca_flow_monitor *monitor,
				       struct doca_flow_fwd *fwd,
				       uint64_t fw_pipe_id,
				       uint32_t flags));

/*
 * Set the function to be called once add entry command is entered for the Firewall application
 *
 * @action [in]: Function callback
 */
void set_pipe_fw_add_entry(void (*action)(uint16_t port_id, struct doca_flow_match *match));

/*
 * Set the function to be called once pipe control add entry command is entered
 *
 * @action [in]: Function callback
 */
void set_pipe_control_add_entry(void (*action)(uint16_t pipe_queue,
					       uint8_t priority,
					       uint64_t pipe_id,
					       struct doca_flow_match *match,
					       struct doca_flow_match *match_mask,
					       struct doca_flow_fwd *fwd,
					       uint64_t fw_pipe_id));

/*
 * Set the function to be called once pipe destroy command is entered
 *
 * @action [in]: Function callback
 */
void set_pipe_destroy(void (*action)(uint64_t pipe_id));

/*
 * Set the function to be called once remove entry command is entered
 *
 * @action [in]: Function callback
 */
void set_pipe_rm_entry(void (*action)(uint16_t pipe_queue, uint64_t entry_id, uint32_t flags));

/*
 * Set the function to be called once FW remove entry command is entered
 *
 * @action [in]: Function callback
 */
void set_pipe_fw_rm_entry(void (*action)(uint64_t entry_id));

/*
 * Set the function to be called once pipes flush command is entered
 *
 * @action [in]: Function callback
 */
void set_port_pipes_flush(void (*action)(uint16_t port_id));

/*
 * Set the function to be called once set query command is entered
 *
 * @action [in]: Function callback
 */
void set_query(void (*action)(uint64_t entry_id, struct doca_flow_resource_query *states));

/*
 * Initialize parser and open the command line interface
 *
 * @shell_prompt [in]: String for the shell to prompt
 * @fw_subset [in]: Boolean to decide what CLI should be supported
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_parser_init(char *shell_prompt, bool fw_subset);

/*
 * Destroy flow parser structures
 */
void flow_parser_cleanup(void);

#endif /* COMMON_FLOW_PARSER_H_ */
