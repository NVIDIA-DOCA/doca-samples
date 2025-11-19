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

#ifndef FLOW_ENCRYPT_H_
#define FLOW_ENCRYPT_H_

#include <rte_hash.h>

#include "flow_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Add encryption entry to the encrypt pipes:
 * - 5 tuple rule in the TCP / UDP pipe with specific set meta data value (shared obj ID)
 * - specific meta data match on encryption pipe (shared obj ID) with shared object ID in actions
 *
 * @rule [in]: rule to insert for encryption
 * @rule_id [in]: rule id for shared obj ID
 * @ports [in]: array of ports
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t add_encrypt_entry(struct encrypt_rule *rule,
			       int rule_id,
			       struct ipsec_security_gw_ports_map **ports,
			       struct ipsec_security_gw_config *app_cfg);

/*
 * Add encryption entries to the encrypt pipes:
 * - 5 tuple rule in the TCP / UDP pipe with specific set meta data value (shared obj ID)
 * - specific meta data match on encryption pipe (shared obj ID) with shared object ID in actions
 *
 * @app_cfg [in]: application configuration struct
 * @ports [in]: ports map
 * @queue_id [in]: queue id
 * @nb_rules [in]: number of encryption rules
 * @rule_offset [in]: offset of the rule in the rules array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t add_encrypt_entries(struct ipsec_security_gw_config *app_cfg,
				 struct ipsec_security_gw_ports_map *ports[],
				 uint16_t queue_id,
				 int nb_rules,
				 int rule_offset);

/*
 * Create encrypt pipe and entries according to the parsed rules
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @app_cfg [in]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_insert_encrypt_rules(struct ipsec_security_gw_ports_map *ports[],
						    struct ipsec_security_gw_config *app_cfg);

/*
 * Create encrypt egress pipes
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @app_cfg [in]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_create_encrypt_egress(struct ipsec_security_gw_ports_map *ports[],
						     struct ipsec_security_gw_config *app_cfg);

/*
 * Handling the new received packet - print packet source IP and send them to tx queues of second port
 *
 * @packet [in]: packet to parse
 * @ctx [in]: core context struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t handle_unsecured_packets_received(struct rte_mbuf **packet, struct ipsec_security_gw_core_ctx *ctx);

/*
 * Allocate encrypt IDs to the secure port
 *
 * @nb_rules [in]: number of decrypt rules
 * @port [in]: secure port pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t get_encrypt_ids(int nb_rules, struct doca_flow_port *port);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLOW_ENCRYPT_H_ */
