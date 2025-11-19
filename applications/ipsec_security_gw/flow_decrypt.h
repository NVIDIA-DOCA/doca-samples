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

#ifndef FLOW_DECRYPT_H_
#define FLOW_DECRYPT_H_

#include "flow_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Add decryption entries to the decrypt pipe
 *
 * @app_cfg [in]: application configuration struct
 * @port [in]: port of the entries
 * @queue_id [in]: queue id to insert the entries
 * @nb_rules [in]: number of rules to insert
 * @rule_offset [in]: offset of the rules in the rules array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t add_decrypt_entries(struct ipsec_security_gw_config *app_cfg,
				 struct ipsec_security_gw_ports_map *port,
				 uint16_t queue_id,
				 int nb_rules,
				 int rule_offset);

/*
 * Add decryption entry to the decrypt pipe
 *
 * @rule [in]: rule to insert for decryption
 * @rule_id [in]: rule id for crypto shared index
 * @port [in]: port of the entries
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t add_decrypt_entry(struct decrypt_rule *rule,
			       int rule_id,
			       struct doca_flow_port *port,
			       struct ipsec_security_gw_config *app_cfg);

/*
 * Create decrypt pipe and entries according to the parsed rules
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @app_cfg [in]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_insert_decrypt_rules(struct ipsec_security_gw_ports_map *ports[],
						    struct ipsec_security_gw_config *app_cfg);

/*
 * Handling the new received packets - decap packet and send them to tx queues of second port
 *
 * @packet [in]: packet to process
 * @bad_syndrome_check [in]: true if need to check bad syndrome in packet meta
 * @ctx [in]: core context struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t handle_secured_packets_received(struct rte_mbuf **packet,
					     bool bad_syndrome_check,
					     struct ipsec_security_gw_core_ctx *ctx);

/*
 * Allocate decrypt IDs to the secure port
 *
 * @nb_rules [in]: number of decrypt rules
 * @initial_id [in]: initial ID for the decrypt IDs (number of encrypt IDs)
 * @port [in]: secure port pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t get_decrypt_ids(int nb_rules, int initial_id, struct doca_flow_port *port);
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLOW_DECRYPT_H_ */
