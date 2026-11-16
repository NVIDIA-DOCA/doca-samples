/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef FLOW_COMMON_H_
#define FLOW_COMMON_H_

#include <arpa/inet.h>

#include <doca_flow.h>

#include "ipsec_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QUEUE_DEPTH (512) /* DOCA Flow queue depth */
#define SECURED_IDX (0)	  /* Index for secured network port in ports array */
#define UNSECURED_IDX (1) /* Index for unsecured network port in ports array */
/* Default destination MAC used for ESP tunnel encap/decap when the user does not provide one */
#define DEFAULT_TUNNEL_DST_MAC \
	{ \
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff \
	}
#define PORT_ID(ports, idx) ((ports)[(idx)]->port_id)
#define SECURED_PORT_ID(ports) (PORT_ID(ports, SECURED_IDX))
#define UNSECURED_PORT_ID(ports) (PORT_ID(ports, UNSECURED_IDX))
#define DEFAULT_TIMEOUT_US (10000)  /* default timeout for processing entries */
#define DEF_EXPECTED_ENTRIES (1024) /* default expected entries in the pipe */
#define SET_L4_PORT(layer, port, value) \
	do { \
		if (match.layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_TCP) \
			match.layer.tcp.l4_port.port = (value); \
		else if (match.layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_UDP) \
			match.layer.udp.l4_port.port = (value); \
	} while (0) /* Set match l4 port */

#define BE_IPV4_ADDR(a, b, c, d) (RTE_BE32((a << 24) + (b << 16) + (c << 8) + d)) /* Big endian conversion */
#define SET_IP6_ADDR(addr, a, b, c, d) \
	do { \
		addr[0] = a; \
		addr[1] = b; \
		addr[2] = c; \
		addr[3] = d; \
	} while (0)
#define SET_MAC_ADDR(addr, a, b, c, d, e, f) \
	do { \
		addr[0] = a & 0xff; \
		addr[1] = b & 0xff; \
		addr[2] = c & 0xff; \
		addr[3] = d & 0xff; \
		addr[4] = e & 0xff; \
		addr[5] = f & 0xff; \
	} while (0) /* create source mac address */

#define IPSEC_ID(index) allocated_ipsec_id[index]

extern uint32_t *allocated_ipsec_id;

/* IPsec Security Gateway mapping between dpdk and doca flow port */
struct ipsec_security_gw_ports_map {
	struct doca_flow_port *port;		/* doca flow port pointer */
	int port_id;				/* dpdk port ID */
	struct doca_flow_header_eth eth_header; /* doca flow eth header */
};

/* user context struct that will be used in entries process callback */
struct entries_status {
	bool failure;	      /* will be set to true if some entry status will not be success */
	int nb_processed;     /* number of entries that was already processed */
	int entries_in_queue; /* number of entries in queue that is waiting to process */
};

/* core context struct */
struct ipsec_security_gw_core_ctx {
	uint16_t queue_id;			    /* core queue ID */
	struct ipsec_security_gw_config *config;    /* application configuration struct */
	struct encrypt_rule *encrypt_rules;	    /* encryption rules */
	struct decrypt_rule *decrypt_rules;	    /* decryption rules */
	int *nb_encrypt_rules;			    /* number of encryption rules */
	struct ipsec_security_gw_ports_map **ports; /* application ports */
};

/*
 * This union describes the meaning of each bit in "meta.pkt_meta"
 */
union security_gateway_pkt_meta {
	uint32_t u32;
	struct {
		uint32_t encrypt : 1;		  /* packet is on encrypt path */
		uint32_t decrypt : 1;		  /* packet is on decrypt path */
		uint32_t inner_ipv6 : 1;	  /* indicate if inner type is ipv6 for tunnel mode */
		uint32_t decrypt_syndrome : 2;	  /* decrypt syndrome, set in debug mode when fwd to app */
		uint32_t antireplay_syndrome : 2; /* anti-replay syndrome, set in debug mode when fwd to app*/
		uint32_t rsvd0 : 5;		  /* must be set to 0 */
		uint32_t rule_id : 20;		  /* indicate the rule ID */
	};
} __attribute__((__packed__));

/*
 * Initialized DOCA Flow library and start DOCA Flow ports
 *
 * @app_cfg [in]: application configuration structure
 * @nb_queues [in]: number of queues
 * @ports [out]: initialized DOCA Flow ports
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_init_doca_flow(const struct ipsec_security_gw_config *app_cfg,
					      int nb_queues,
					      struct ipsec_security_gw_ports_map *ports[]);

/*
 * Initialized status entries for each port
 *
 * @app_cfg [in]: application configuration structure
 * @nb_queues [in]: number of queues
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_init_status(struct ipsec_security_gw_config *app_cfg, int nb_queues);

/*
 * Allocate shared-resource IDs for encrypt and decrypt rules
 *
 * @ports [in]: initialized DOCA Flow ports
 * @app_cfg [in]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_ids_get(struct ipsec_security_gw_ports_map *ports[],
				       struct ipsec_security_gw_config *app_cfg);

/*
 * Destroy DOCA Flow resources
 *
 * @nb_ports [in]: number of ports to destroy
 * @ports [in]: initialized DOCA Flow ports
 */
void doca_flow_cleanup(int nb_ports, struct ipsec_security_gw_ports_map *ports[]);

/*
 * Process the added entries and check the status
 *
 * @port [in]: DOCA Flow port
 * @status [in]: the entries status struct that monitor the entries in this specific port
 * @timeout [in]: timeout for process entries
 * @pipe_queue [in]: pipe queue to process
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t process_entries(struct doca_flow_port *port,
			     struct entries_status *status,
			     int timeout,
			     uint16_t pipe_queue);

/*
 * create root pipe for ingress in switch mode that forward the packets based on the port_id
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_switch_ingress_root_pipes(struct ipsec_security_gw_ports_map *ports[],
					      struct ipsec_security_gw_config *app_cfg);

/*
 * create root pipe for egress in switch mode that forward the packets based on pkt meta
 *
 * @ports [in]: array of struct ipsec_security_gw_ports_map
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_switch_egress_root_pipes(struct ipsec_security_gw_ports_map *ports[],
					     struct ipsec_security_gw_config *app_cfg);

/*
 * Create RSS pipe that fwd the packets to hairpin queue
 *
 * @port [in]: port of the pipe
 * @nb_queues [in]: number of queues
 * @rss_pipe [out]: pointer to created pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_rss_pipe(struct ipsec_security_gw_config *app_cfg,
			     struct doca_flow_port *port,
			     uint16_t nb_queues,
			     struct doca_flow_pipe **rss_pipe);

/*
 * Create the DOCA Flow forward struct based on the running mode
 *
 * @app_cfg [in]: application configuration struct
 * @port_id [in]: port ID of the pipe
 * @encrypt [in]: true if direction is encrypt, false for decrypt
 * @rss_queues [in]: rss queues array to fill in case of sw forward
 * @rss_flags [in]: rss flags
 * @fwd [out]: the created forward struct
 */
void create_hairpin_pipe_fwd(struct ipsec_security_gw_config *app_cfg,
			     int port_id,
			     bool encrypt,
			     uint16_t *rss_queues,
			     uint32_t rss_flags,
			     struct doca_flow_fwd *fwd);

/*
 * Resolve the L2 headers used for ESP tunnel encap/decap.
 * For any address the user did not provide, fills the default: src = local port MAC,
 * dst = DEFAULT_TUNNEL_DST_MAC. Only relevant in tunnel mode; if MAC overrides are
 * provided outside tunnel mode this returns an error.
 *
 * @app_cfg [in/out]: application configuration struct (encap_eth/decap_eth are filled in)
 * @ports [in]: application ports map (used for the default src MACs)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_resolve_tunnel_l2(struct ipsec_security_gw_config *app_cfg,
						 struct ipsec_security_gw_ports_map *ports[]);

/*
 * Add an ARP handling entry to a control pipe according to the configured ARP mode.
 * The caller is responsible for only calling this when ARP handling is enabled
 * (arp_mode != IPSEC_SECURITY_GW_ARP_MODE_DEFAULT_MISS).
 *
 * @control_pipe [in]: control pipe to add the entry to
 * @fwd_port_id [in]: port to forward ARP packets to when mode is forward-port
 * @arp_mode [in]: configured ARP handling mode
 * @debug_mode [in]: true if running in debug mode
 * @monitor_ptr [in]: monitor with counter for debug, or NULL
 * @entry_name [in]: entry name used for debug statistics
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t add_arp_control_entry(struct security_gateway_pipe_info *control_pipe,
				   uint16_t fwd_port_id,
				   enum ipsec_security_gw_arp_mode arp_mode,
				   bool debug_mode,
				   struct doca_flow_monitor *monitor_ptr,
				   const char *entry_name);

/*
 * Remove trailing zeros from ipv4/ipv6 payload.
 * Trailing zeros are added to ipv4/ipv6 payload so that it's larger than the minimal ethernet frame size.
 *
 * @m [in]: the mbuf to update
 */
void remove_ethernet_padding(struct rte_mbuf **m);

/*
 * Convert icv length value to the correct enum doca_flow_crypto_icv_len
 *
 * @icv_len [in]: icv length value
 * @return: enum doca_flow_crypto_icv_len with the correct icv length
 */
uint32_t get_icv_len_int(enum doca_flow_crypto_icv_len icv_len);

/*
 * Release application allocated status entries
 *
 * @app_cfg [in]: application configuration struct
 */
void security_gateway_free_status_entries(struct ipsec_security_gw_config *app_cfg);

/*
 * Release application allocated resources
 *
 * @app_cfg [in]: application configuration struct
 */
void security_gateway_free_resources(struct ipsec_security_gw_config *app_cfg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLOW_COMMON_H_ */
