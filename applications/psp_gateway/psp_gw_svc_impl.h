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

#ifndef _PSP_GW_SVC_H
#define _PSP_GW_SVC_H

#include <memory>
#include <map>

#include <doca_flow.h>

#include <psp_gateway.pb.h>
#include <psp_gateway.grpc.pb.h>
#include "psp_gw_config.h"
#include "psp_gw_flows.h"

struct psp_pf_dev;
struct doca_flow_crypto_psp_spi_key_bulk;

using psp_session_and_key_t = std::pair<psp_session_t *, void *>;

/**
 * @brief Implementation of the PSP_Gateway service.
 *
 * Manages the generation of PSP encryption keys, which
 * are supplied to a remote service to establish a tunnel
 * connection.
 *
 * As a server, listens to requests for new tunnels, generates
 * parameters for the requestor to send encrypted packets, and
 * creates the flows required to send encrypted packets back
 * to the requestor.
 *
 * As a client, generates parameters for a remote service to
 * send encrypted packets, and sends them as part of the request.
 */
class PSP_GatewayImpl : public psp_gateway::PSP_Gateway::Service {
public:
	static constexpr uint16_t DEFAULT_HTTP_PORT_NUM = 3000;

	/**
	 * @brief Constructs the object. This operation cannot fail.
	 *
	 * @param [in] psp_flows The object which manages the doca resources.
	 */
	PSP_GatewayImpl(psp_gw_app_config *config, PSP_GatewayFlows *psp_flows);

	/**
	 * @brief Requests that the recipient allocate multiple SPIs and encryption keys
	 * so that the initiator can begin sending encrypted traffic.
	 *
	 * @context [in]: grpc context
	 * @request [in]: request parameters
	 * @response [out]: requested outputs
	 * @return: Indicates success/failure of the request
	 */
	::grpc::Status RequestMultipleTunnelParams(::grpc::ServerContext *context,
						   const ::psp_gateway::MultiTunnelRequest *request,
						   ::psp_gateway::MultiTunnelResponse *response) override;

	/**
	 * @brief Requests that the recipient rotate the PSP master key.
	 *
	 * @context [in]: grpc context
	 * @request [in]: request parameters
	 * @response [out]: requested outputs
	 * @return: Indicates success/failure of the request
	 */
	::grpc::Status RequestKeyRotation(::grpc::ServerContext *context,
					  const ::psp_gateway::KeyRotationRequest *request,
					  ::psp_gateway::KeyRotationResponse *response) override;

	/**
	 * @brief Handles any "miss" packets received by RSS which indicate
	 *        a new tunnel connection is needed.
	 *
	 * @packet [in]: The packet received from RSS
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t handle_miss_packet(struct rte_mbuf *packet);

	/**
	 * @brief Displays the counters of all tunnel sessions that have
	 *        changed since the previous invocation.
	 *
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t show_flow_counts(void);

	/**
	 * @brief Attempt to establish tunnels to each of the passed peers.
	 * On success, a given peer is removed from the list so that this
	 * method can be called repeatedly with the same list.
	 *
	 * @peers [in/out]: the list of tunnels to try to establish
	 * @return: the number of peers successfully connected and removed from 'peers'
	 */
	size_t try_connect(std::vector<psp_gw_peer> &peers);

private:
	/**
	 * @brief Returns the number of bits of the key size as determined
	 * by the given PSP protocol version.
	 *
	 * @psp_proto_ver [in]: the PSP protocol version
	 * @return: 128 or 256 depending on the key type
	 */
	static uint32_t psp_version_to_key_length_bits(uint32_t psp_proto_ver)
	{
		return (psp_proto_ver == 0 || psp_proto_ver == 2) ? 128 : 256;
	}

	/**
	 * @brief Sends a request to the given peer
	 * The request includes the parameters required for
	 * traffic in the reverse direction (remote to local).
	 * An ACL is also provided for return traffic, if the
	 * local/remote virtual addresses are provided.
	 *
	 * @peer [in]: The peer to which we will create a tunnel
	 * @vip_pair [in]: The source and destination IP addresses of the traffic flow
	 * @supply_reverse_params [in]: Whether to include tunnel parameters for traffic
	 * returning to the sender of the request.
	 * @suppress_failure_msg [in]: Indicates we are okay with a failure to connect, such
	 * as during application startup.
	 * @has_remote [in]: true if remote_virt_ip was send to the function -
	 * when true, generate one pair of SPI and key and insert one rule
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t request_tunnel_to_host(struct psp_gw_peer *peer,
					    ip_pair *vip_pair,
					    bool supply_reverse_params,
					    bool suppress_failure_msg,
					    bool has_remote);

	/**
	 * @brief Returns a gRPC client for a given peer
	 * Note: this assumes only a single PSP app instance per peer
	 *
	 * @return: the gRPC stub associated with the given address
	 */
	::psp_gateway::PSP_Gateway::Stub *get_stub(const std::string &peer_ip);

	/**
	 * @brief Checks whether a peer has been configured to receive
	 * traffic to the given vip_pair
	 *
	 * @vip_pair [in]: The source and destination IP addresses of the traffic flow
	 * @return: the remote gateway host, if one exists
	 */
	psp_gw_peer *lookup_vip_pair(ip_pair &vip_pair);

	/**
	 * @brief Checks the list of supported versions in the request
	 *
	 * @request [in]: The request received over gRPC
	 * @return: the supported version number, or -1 if no acceptable
	 * version was requested.
	 */
	int select_psp_version(const ::psp_gateway::MultiTunnelRequest *request) const;

	/**
	 * @brief Checks whether the given PSP version is supported
	 *
	 * @psp_ver [in]: The requested PSP protocol version
	 * @return: True if the version is supported; false otherwise
	 */
	bool is_psp_ver_supported(uint32_t psp_ver) const
	{
		return SUPPORTED_PSP_VERSIONS.count(psp_ver) > 0;
	}

	/**
	 * @brief writes the new SPI/Key and all required PF attributes to a gRPC request object.
	 *
	 * @psp_ver [in]: The PSP version to use for encryption
	 * @key [in]: The key to use for encryption
	 * @spi [in]: The SPI to use for encryption
	 * @params [out]: The gRPC object to populate with the new SPI/key
	 */
	void fill_tunnel_params(int psp_ver, uint32_t *key, uint32_t spi, psp_gateway::TunnelParameters *params);

	/**
	 * @brief Generates new SPI/key pairs
	 *
	 * @key_len_bits [in]: 128 or 256
	 * @nr_keys_spis [in]: The number of SPIs/keys to generate
	 * @keys [out]: The generated keys
	 * @spis [out]: The generated SPIs
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t generate_keys_spis(uint32_t key_len_bits, uint32_t nr_keys_spis, uint32_t *keys, uint32_t *spis);

	/**
	 * @brief Adds encryption entries to pipeline according to sessions
	 *
	 * @new_sessions_keys [in]: The new sessions to create entries for
	 * @peer_svc_addr [in]: The peer to which we will create a tunnel
	 * @queue_id [in]: The queue ID for to add the entries to
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t add_encrypt_entries(std::vector<psp_session_and_key_t> &new_sessions_keys,
					 std::string peer_svc_addr,
					 uint16_t queue_id);
	/**
	 * @brief Prepares the session for the given peer virtual IP
	 *
	 * @peer_svc_addr [in]: The peer to which we will create a tunnel
	 * @vip_pair [in]: The source and destination IP addresses of the traffic flow
	 * @params [in]: The parameters for the tunnel
	 * @sessions_keys_prepared [out]: The session will be added to this vector
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t prepare_session(std::string peer_svc_addr,
				     ip_pair &vip_pair,
				     const psp_gateway::TunnelParameters &params,
				     std::vector<psp_session_and_key_t> &sessions_keys_prepared);

	/**
	 * @brief Dumps the hex bytes of the given PSP key
	 *
	 * @msg_prefix [in]: Prepend this string to the key log
	 * @key [in]: the bytes of the key object
	 * @key_size [in]: indicates whether the key is 128 or 256 bits
	 */
	void debug_key(const char *msg_prefix, const void *key, size_t key_size_bytes) const;

	/**
	 * @brief Determines the next available crypto_id at which to store the
	 * next PSP encryption key
	 *
	 * @return: The crypto_id to use for the PSP shared resource
	 */
	uint32_t next_crypto_id(void);

	// Application state data:

	psp_gw_app_config *config{};

	PSP_GatewayFlows *psp_flows{};

	psp_pf_dev *pf{};

	// Used to uniquely populate the request ID in each NewTunnelRequest message.
	uint64_t next_request_id{};

	// This flag will cause encryption keys to be logged to stderr, etc.
	const bool DEBUG_KEYS{false};

	// map each svc_addr to an RPC object
	std::map<std::string, std::unique_ptr<::psp_gateway::PSP_Gateway::Stub>> stubs;

	// map tuple of (src vip, dst vip) to an active session object
	std::map<session_key, psp_session_t> sessions;

	// Used to assign a unique shared-resource ID to each encryption flow.
	uint32_t next_crypto_id_ = 1;
};

#endif // _PSP_GW_SVC_H
