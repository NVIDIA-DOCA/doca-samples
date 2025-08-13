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

#ifndef _PSP_GW_FLOWS_H_
#define _PSP_GW_FLOWS_H_

#include <netinet/in.h>
#include <string>
#include <unordered_map>

#include <rte_ether.h>

#include <doca_dev.h>
#include <doca_flow.h>

#include "psp_gw_config.h"

static const int NUM_OF_PSP_SYNDROMES = 2; // ICV Fail, Bad Trailer

struct psp_gw_app_config;

/**
 * @brief Maintains the state of the host PF
 */
struct psp_pf_dev {
	doca_dev *dev;
	uint16_t port_id;
	doca_flow_port *port_obj;

	rte_ether_addr src_mac;
	std::string src_mac_str;

	struct doca_flow_ip_addr src_pip; // Physical/Outer IP addr
	std::string src_pip_str;
};

/**
 * @brief describes a PSP tunnel connection to a single address
 *        on a peer.
 */
struct psp_session_t {
	rte_ether_addr dst_mac;

	struct doca_flow_ip_addr dst_pip; /* Physical/Outer IP addr */
	struct doca_flow_ip_addr dst_vip; /* Virtual/Inner dest IP addr */
	struct doca_flow_ip_addr src_vip; /* Virtual/Inner src IP addr */

	uint32_t spi_egress;  /* Security Parameter Index on the wire - host-to-net */
	uint32_t spi_ingress; /* Security Parameter Index on the wire - net-to-host */
	uint32_t crypto_id;   /* Internal shared-resource index */

	uint32_t psp_proto_ver; /* PSP protocol version used by this session */
	uint64_t vc;		/* Virtualization cookie, if enabled */

	doca_flow_pipe_entry *encap_encrypt_entry; /* DOCA Flow encap & encrypt entry */
	doca_flow_pipe_entry *acl_entry;	   /* DOC AFlow ACL entry */
	uint64_t pkt_count_egress;		   /* Count of encap_encrypt_entry */
	uint64_t pkt_count_ingress;		   /* Count of acl_entry */
};

/**
 * @brief The entity which owns all the doca flow shared
 *        resources and flow pipes (but not sessions).
 */
class PSP_GatewayFlows {
public:
	/**
	 * @brief Constructs the object. This operation cannot fail.
	 * @param [in] pf The Host PF object, already opened and probed,
	 *        but not started, by DOCA, of the device which sends
	 *        and receives encrypted packets
	 * @param [in] vf_dev The DOCA device representor device which sends
	 *        and received plaintext packets.
	 * @param [in] vf_port_id The port_id of the device which sends
	 *        and received plaintext packets.
	 */
	PSP_GatewayFlows(psp_pf_dev *pf, doca_dev_rep *vf_dev, uint16_t vf_port_id, psp_gw_app_config *app_config);

	/**
	 * Deallocates all associated DOCA objects.
	 * In case of failure, an error is logged and progress continues.
	 */
	virtual ~PSP_GatewayFlows(void);

	/**
	 * Exposes the host PF device. (Used by the benchmarking functions)
	 */
	psp_pf_dev *pf(void)
	{
		return pf_dev;
	}

	/**
	 * @brief Initialized the DOCA resources.
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t init(void);

	/**
	 * @brief Adds a flow pipe entry to perform encryption on a new flow
	 *        to the indicated peer.
	 * The caller is responsible for negotiating the SPI and key, and
	 * assigning a unique crypto_id.
	 *
	 * @session [in]: the session for which an encryption flow should be created
	 * @encrypt_key [in]: the encryption key to use for the session
	 * @queue_id [in]: the queue ID for to add the entry to
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t add_encrypt_entry(psp_session_t *session, const void *encrypt_key, uint16_t queue_id);

	/**
	 * @brief Adds an ingress ACL entry for the given session to accept
	 *        the combination of src_vip and SPI.
	 *
	 * @session [in]: the session for which an ingress ACL flow should be created
	 * @queue_id [in]: the queue ID for to add the entry to
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t add_ingress_acl_entry(psp_session_t *session, uint16_t queue_id);

	/**
	 * @brief Removes the indicated flow entry.
	 *
	 * @session [in]: The session whose associated flows should be removed
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t remove_encrypt_entry(psp_session_t *session);

	/**
	 * @brief Shows flow counters for pipes which have a fixed number of entries,
	 *        if any counter values have changed since the last invocation.
	 */
	void show_static_flow_counts(void);

	/**
	 * @brief Shows flow counters for the given tunnel, if they have changed
	 *        since the last invocation.
	 *
	 * @session_vips_pair [in]: the pair of VIPs which identify the session
	 * @session [in/out]: the object which holds the flow entries
	 */
	void show_session_flow_count(const session_key session_vips_pair, psp_session_t &session);

private:
	/**
	 * @brief Private structure used to display flow query results
	 */
	struct pipe_query;

	/**
	 * @brief Callback which is invoked to check the status of every entry
	 *        added to a flow pipe. See doca_flow_entry_process_cb.
	 *
	 * @entry [in]: The entry which was added/removed/updated
	 * @pipe_queue [in]: The index of the associated queue
	 * @status [in]: The result of the operation
	 * @op [in]: The type of the operation
	 * @user_ctx [in]: The argument supplied to add_entry, etc.
	 */
	static void check_for_valid_entry(doca_flow_pipe_entry *entry,
					  uint16_t pipe_queue,
					  enum doca_flow_entry_status status,
					  enum doca_flow_entry_op op,
					  void *user_ctx);

	/**
	 * @brief Starts the given port (with optional dev pointer) to create
	 *        a doca flow port.
	 *
	 * @port_id [in]: the numerical index of the port
	 * @port_dev [in]: the doca_dev returned from doca_dev_open()
	 * @port_rep [in]: the doca_dev_rep returned from doca_dev_rep_open()
	 * @port [out]: the resulting port object
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t start_port(uint16_t port_id, doca_dev *port_dev, doca_dev_rep *port_rep, doca_flow_port **port);

	/**
	 * @brief handles the initialization DOCA Flow
	 *
	 * @app_cfg [in]: the psp app configuration
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t init_doca_flow(const psp_gw_app_config *app_cfg);

	/**
	 * @brief initialization of entries status vector in app_cfg
	 *
	 * @app_cfg [in]: the psp app configuration
	 */
	void init_status(psp_gw_app_config *app_cfg);

	/**
	 * @brief handles the binding of the shared resources to ports
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t bind_shared_resources(void);

	/**
	 * @brief handles the setup of the packet mirroring shared resources
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t configure_mirrors(void);

	/**
	 * @brief wrapper for doca_flow_pipe_add_entry()
	 * Handles the call to process_entry and its callback for a single entry.
	 *
	 * @pipe_queue [in]: the queue index associated with the caller cpu core
	 * @pipe [in]: the pipe on which to add the entry
	 * @port [in]: the port which owns the pipe
	 * @match [in]: packet match criteria
	 * @actions [in]: packet mod actions
	 * @mon [in]: packet monitoring actions
	 * @fwd [in]: packet forwarding actions
	 * @entry [out]: the newly created flow entry
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t add_single_entry(uint16_t pipe_queue,
				      doca_flow_pipe *pipe,
				      doca_flow_port *port,
				      const doca_flow_match *match,
				      const doca_flow_actions *actions,
				      const doca_flow_monitor *mon,
				      const doca_flow_fwd *fwd,
				      doca_flow_pipe_entry **entry);

	/**
	 * Generates the outer/encap header contents for a given session for ipv6 tunnel encap
	 *
	 * @session [in]: the remote host mac/ip/etc. to encap
	 * @encap_data [out]: the actions.crypto_encap.encap_data to populate
	 */
	void format_encap_tunnel_data_ipv6(const psp_session_t *session, uint8_t *encap_data);

	/**
	 * Generates the outer/encap header contents for a given session for ipv4 tunnel encap
	 *
	 * @session [in]: the remote host mac/ip/etc. to encap
	 * @encap_data [out]: the actions.crypto_encap.encap_data to populate
	 */
	void format_encap_tunnel_data_ipv4(const psp_session_t *session, uint8_t *encap_data);

	/**
	 * Generates the outer/encap header contents for a given session for transport encap
	 *
	 * @session [in]: the remote host mac/ip/etc. to encap
	 * @encap_data [out]: the actions.crypto_encap.encap_data to populate
	 */
	void format_encap_transport_data(const psp_session_t *session, uint8_t *encap_data);

	/**
	 * Top-level pipe creation method
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t create_pipes(void);

	/**
	 * Creates the PSP decryption pipe.
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t ingress_decrypt_pipe_create(void);

	/**
	 * Creates the pipe to sample packets with the PSP.S bit set
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t ingress_sampling_pipe_create(void);

	/**
	 * Creates the pipe to only accept incoming packets from
	 * appropriate sources.
	 *
	 * @ipv4 [in]: if true match ipv4 address, else ipv6
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t ingress_acl_pipe_create(bool ipv4);

	/**
	 * Creates the pipe which counts the various syndrome types
	 * and drops the packets
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t syndrome_stats_pipe_create(void);

	/**
	 * Creates the pipe to trap outgoing packets to unregistered destinations
	 *
	 * @ipv4 [in]: if true match ipv4 address, else ipv6
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t egress_acl_pipe_create(bool ipv4);

	/**
	 * Creates the pipe that match ipv6 destination address in egress domain
	 * Write on meta data the hash of the source address
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t create_egress_dst_ip6_pipe(void);

	/**
	 * Creates the pipe that match ipv6 source address in ingress domain
	 * Write on meta data the hash of the destination address
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t create_ingress_src_ip6_pipe(void);

	/**
	 * Add entry to ipv6 destination address pipe
	 *
	 * @session [in]: the session for which an encryption flow should be created
	 * @dst_vip_id [in]: the hash of the destination vip to set in meta data
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t add_egress_dst_ip6_entry(psp_session_t *session, int dst_vip_id);

	/**
	 * Add entry to ipv6 source address pipe
	 *
	 * @session [in]: the session for which an decryption flow should be created
	 * @dst_vip_id [in]: the hash of the destination vip to set in meta data
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t add_ingress_src_ip6_entry(psp_session_t *session, int dst_vip_id);

	/**
	 * Creates the pipe to mark and randomly sample outgoing packets
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t egress_sampling_pipe_create(void);

	/**
	 * Creates the entry point to the CPU Rx queues
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t rss_pipe_create(void);

	/**
	 * @brief Creates the first pipe hit by packets arriving to
	 * the eswitch from either the uplink (wire) or the VF.
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t ingress_root_pipe_create(void);

	/**
	 * @brief Creates a pipe that classify if inner IP is ipv6 or ipv4 and based on that send to acl pipe
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t ingress_inner_classifier_pipe_create(void);

	/**
	 * @brief Creates a pipe to fwd packets to port
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t fwd_to_wire_pipe_create(void);

	/**
	 * @brief Creates a pipe that set pkt meta sample indicator and fwd to rss pipe
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t fwd_to_rss_pipe_create(void);

	/**
	 * @brief Creates a pipe that match on random and set sample bit in PSP header
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t set_sample_bit_pipe_create(void);

	/**
	 * @brief Creates a pipe whose only purpose is to relay
	 * flows from the egress domain to the secure-egress domain,
	 * and to relay injected ARP responses back to the VF.
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t empty_pipe_create(void);

	/**
	 * @brief Creates a pipe to fwd packets to port
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t empty_pipe_create_not_sampled(void);

	/**
	 * @brief Performs a flow query and logs the result.
	 *
	 * @query [in]: The pipe and/or entries to query
	 * @suppress_output [in]: Whether to log the query results or
	 * simply count them
	 * @return: A pair of counters (hits, misses)
	 */
	std::pair<uint64_t, uint64_t> perform_pipe_query(pipe_query *query, bool suppress_output);

	// Application state data:

	psp_gw_app_config *app_config{};

	psp_pf_dev *pf_dev{};

	uint16_t vf_port_id{UINT16_MAX};

	doca_dev_rep *vf_dev_rep{};

	doca_flow_port *vf_port{};

	bool sampling_enabled{false};

	std::vector<uint16_t> rss_queues;
	doca_flow_fwd fwd_changeable_rss{};
	doca_flow_fwd fwd_ipv4_rss{};
	doca_flow_fwd fwd_ipv6_rss{};

	// Pipe and pipe entry application state:

	// general pipes
	doca_flow_pipe *rss_pipe{};
	doca_flow_pipe *ingress_root_pipe{};

	// net-to-host pipes
	doca_flow_pipe *ingress_decrypt_pipe{};
	doca_flow_pipe *ingress_sampling_pipe{};
	doca_flow_pipe *ingress_inner_ip_classifier_pipe{};
	doca_flow_pipe *ingress_acl_ipv4_pipe{};
	doca_flow_pipe *ingress_acl_ipv6_pipe{};

	// host-to-net pipes
	doca_flow_pipe *egress_acl_ipv4_pipe{};
	doca_flow_pipe *egress_acl_ipv6_pipe{};
	doca_flow_pipe *egress_sampling_pipe{};
	doca_flow_pipe *egress_encrypt_pipe{};
	doca_flow_pipe *syndrome_stats_pipe{};
	doca_flow_pipe *empty_pipe{};
	doca_flow_pipe *empty_pipe_not_sampled{};
	doca_flow_pipe *fwd_to_wire_pipe{};
	doca_flow_pipe *fwd_to_rss_pipe{};
	doca_flow_pipe *set_sample_bit_pipe{};
	doca_flow_pipe *egress_dst_ip6_pipe{};
	doca_flow_pipe *ingress_src_ip6_pipe{};

	// static pipe entries
	doca_flow_pipe_entry *ipv4_rss_entry{};
	doca_flow_pipe_entry *ipv6_rss_entry{};
	doca_flow_pipe_entry *default_decrypt_entry{};
	doca_flow_pipe_entry *default_ingr_sampling_entry{};
	doca_flow_pipe_entry *egr_sampling_rss{};
	doca_flow_pipe_entry *egr_sampling_drop{};
	doca_flow_pipe_entry *default_ingr_acl_ipv4_entry{};
	doca_flow_pipe_entry *default_ingr_acl_ipv6_entry{};
	doca_flow_pipe_entry *ingress_ipv4_clasify_entry{};
	doca_flow_pipe_entry *ingress_ipv6_clasify_entry{};
	doca_flow_pipe_entry *root_jump_to_ingress_ipv6_entry{};
	doca_flow_pipe_entry *root_jump_to_ingress_ipv4_entry{};
	doca_flow_pipe_entry *root_jump_to_egress_ipv6_entry{};
	doca_flow_pipe_entry *root_jump_to_egress_ipv4_entry{};
	doca_flow_pipe_entry *vf_arp_to_rss{};
	doca_flow_pipe_entry *vf_ns_to_rss{};
	doca_flow_pipe_entry *vf_arp_to_wire{};
	doca_flow_pipe_entry *uplink_arp_to_vf{};
	doca_flow_pipe_entry *vf_ns_to_wire{};
	doca_flow_pipe_entry *uplink_ns_to_vf{};
	doca_flow_pipe_entry *syndrome_stats_entries[NUM_OF_PSP_SYNDROMES]{};
	doca_flow_pipe_entry *empty_pipe_entry{};
	doca_flow_pipe_entry *arp_empty_pipe_entry{};
	doca_flow_pipe_entry *ns_empty_pipe_entry{};
	doca_flow_pipe_entry *ipv4_empty_pipe_entry{};
	doca_flow_pipe_entry *ipv6_empty_pipe_entry{};
	doca_flow_pipe_entry *root_default_drop{};
	doca_flow_pipe_entry *fwd_to_wire_entry{};
	doca_flow_pipe_entry *fwd_ipv4_rss_entry{};
	doca_flow_pipe_entry *fwd_ipv6_rss_entry{};
	doca_flow_pipe_entry *set_sample_bit_entry{};

	// commonly used setting to enable per-entry counters
	struct doca_flow_monitor monitor_count {};

	// Shared resource IDs
	uint32_t mirror_res_id_ingress{1};
	uint32_t mirror_res_id_rss{2};
	uint32_t mirror_res_id_drop{3};
	uint32_t mirror_res_id_count{4};

	// Sum of all static pipe entries the last time
	// show_static_flow_counts() was invoked.
	uint64_t prev_static_flow_count{UINT64_MAX};
};

#endif /* _PSP_GW_FLOWS_H_ */
