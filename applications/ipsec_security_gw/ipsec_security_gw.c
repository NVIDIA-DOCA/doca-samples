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
#include <signal.h>
#include <fcntl.h>

#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_flow_tune_server.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_dev.h>

#include <dpdk_utils.h>
#include <pack.h>

#include "config.h"
#include "flow_common.h"
#include "flow_decrypt.h"
#include "flow_encrypt.h"
#include "ipsec_ctx.h"
#include "policy.h"

DOCA_LOG_REGISTER(IPSEC_SECURITY_GW);

#define DEFAULT_NB_CORES 4		   /* Default number of running cores */
#define PACKET_BURST 32			   /* The number of packets in the rx queue */
#define NB_TX_BURST_TRIES 5		   /* Number of tries for sending batch of packets */
#define MIN_ENTRIES_PER_CORE 1024	   /* Minimum number of entries per core */
#define MAC_ADDRESS_SIZE 6		   /* Size of mac address */
#define ONE_SECOND_IN_MICROSECONDS 1000000 /* One second in microseconds */

/* Rule Inserter worker thread context struct */
struct multi_thread_insertion_ctx {
	struct ipsec_security_gw_config *app_cfg;   /* Application configuration struct */
	struct ipsec_security_gw_ports_map **ports; /* Application ports */
	int queue_id;				    /* Queue ID */
	int nb_encrypt_rules;			    /* Number of encryption rules */
	int nb_decrypt_rules;			    /* Number of decryption rules */
	int encrypt_rule_offset;		    /* Offset for encryption rules */
	int decrypt_rule_offset;		    /* Offset for decryption rules */
};

static bool force_quit; /* Set when signal is received */
static char *syndrome_list[NUM_OF_SYNDROMES] = {"Authentication failed",
						"Trailer length exceeded ESP payload",
						"Replay protection failed",
						"IPsec offload context reached its hard lifetime threshold"};

/*
 * Signals handler function to handle SIGINT and SIGTERM signals
 *
 * @signum [in]: signal number
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/*
 * Query all the entries in the pipe and print the changed entries
 *
 * @pipe [in]: pipe to query
 * @return: true if packet hit an entry in the pipe since last query
 */
static bool query_pipe_info(struct security_gateway_pipe_info *pipe)
{
	doca_error_t result;
	struct doca_flow_resource_query query_stats;
	uint64_t delta;
	uint32_t i;
	bool first = true;

	for (i = 0; i < pipe->nb_entries; i++) {
		result = doca_flow_resource_query_entry(pipe->entries_info[i].entry, &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			continue;
		}
		if (query_stats.counter.total_pkts != pipe->entries_info[i].prev_stats) {
			delta = query_stats.counter.total_pkts - pipe->entries_info[i].prev_stats;
			if (first) {
				printf("%s[%s:%ld", pipe->name, pipe->entries_info[i].name, delta);
				first = false;
			} else
				printf(",%s:%ld", pipe->entries_info[i].name, delta);
			pipe->entries_info[i].prev_stats = query_stats.counter.total_pkts;
		}
	}
	if (!first)
		printf("] ");
	return !first;
}
/*
 * Query all the entries of bad syndrome of a specific rule, print the delta from last query if different from 0
 *
 * @decrypt_rule [in]: the rule to query its entries
 */
static void query_bad_syndrome(struct decrypt_rule *decrypt_rule)
{
	doca_error_t result;
	struct doca_flow_resource_query query_stats;
	int i;

	for (i = 0; i < NUM_OF_SYNDROMES; i++) {
		result = doca_flow_resource_query_entry(decrypt_rule->entries[i].entry, &query_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query entry: %s", doca_error_get_descr(result));
			continue;
		}
		if (query_stats.counter.total_pkts != decrypt_rule->entries[i].previous_stats) {
			if (decrypt_rule->l3_type == DOCA_FLOW_L3_TYPE_IP4) {
				DOCA_LOG_INFO("Spi %d, IP %d.%d.%d.%d",
					      decrypt_rule->esp_spi,
					      (decrypt_rule->dst_ip4) & 0xFF,
					      (decrypt_rule->dst_ip4 >> 8) & 0xFF,
					      (decrypt_rule->dst_ip4 >> 16) & 0xFF,
					      (decrypt_rule->dst_ip4 >> 24) & 0xFF);
			} else {
				char ipinput[INET6_ADDRSTRLEN];

				inet_ntop(AF_INET6, &(decrypt_rule->dst_ip6), ipinput, INET6_ADDRSTRLEN);
				DOCA_LOG_INFO("Spi %d, IP %s", decrypt_rule->esp_spi, ipinput);
			}
			DOCA_LOG_INFO("Got bad syndrome: %s, number of hits since last dump: %ld",
				      syndrome_list[i],
				      query_stats.counter.total_pkts - decrypt_rule->entries[i].previous_stats);
			decrypt_rule->entries[i].previous_stats = query_stats.counter.total_pkts;
		}
	}
}

/*
 * Query all the encrypt pipes
 *
 * @app_cfg [in]: application configuration structure
 */
static void query_encrypt_pipes(struct ipsec_security_gw_config *app_cfg)
{
	bool changed = false;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		changed |= query_pipe_info(&app_cfg->encrypt_pipes.encrypt_root);
		changed |= query_pipe_info(&app_cfg->switch_pipes.pkt_meta_pipe);
	}
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv4_tcp_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv4_udp_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv6_tcp_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv6_udp_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv6_src_tcp_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv6_src_udp_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.egress_ip_classifier);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv4_encrypt_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.ipv6_encrypt_pipe);
	changed |= query_pipe_info(&app_cfg->encrypt_pipes.marker_insert_pipe);
	if (app_cfg->vxlan_encap)
		changed |= query_pipe_info(&app_cfg->encrypt_pipes.vxlan_encap_pipe);
	if (changed) {
		printf("\n");
		fflush(stdout);
	}
}

/*
 * Query all the decrypt pipes
 *
 * @app_cfg [in]: application configuration structure
 */
static void query_decrypt_pipes(struct ipsec_security_gw_config *app_cfg)
{
	bool changed = false;

	changed |= query_pipe_info(&app_cfg->decrypt_pipes.vxlan_decap_ipv4_pipe);
	changed |= query_pipe_info(&app_cfg->decrypt_pipes.vxlan_decap_ipv6_pipe);
	changed |= query_pipe_info(&app_cfg->decrypt_pipes.decrypt_ipv4_pipe);
	changed |= query_pipe_info(&app_cfg->decrypt_pipes.decrypt_ipv6_pipe);
	changed |= query_pipe_info(&app_cfg->decrypt_pipes.decap_pipe);
	changed |= query_pipe_info(&app_cfg->decrypt_pipes.marker_remove_pipe);
	if (changed) {
		printf("\n");
		fflush(stdout);
	}
}

/*
 * Query the entries that dropped packet with bad syndrome
 *
 * @args [in]: generic pointer to core context struct
 */
static void process_syndrome_packets(void *args)
{
	doca_error_t result;
	struct ipsec_security_gw_core_ctx *ctx = (struct ipsec_security_gw_core_ctx *)args;
	int i;
	uint64_t start_time, end_time;
	uint64_t delta;
	uint64_t cycle_time = ONE_SECOND_IN_MICROSECONDS;
	uint64_t max_timeout = (ctx->config->debug_mode ? 0.9 : 0.95) * ONE_SECOND_IN_MICROSECONDS;

	while (!force_quit) {
		start_time = rte_get_timer_cycles();
		result = doca_flow_crypto_ipsec_resource_handle(ctx->ports[SECURED_IDX]->port, max_timeout, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to handle IPSec resources: %s", doca_error_get_descr(result));
			/* Fallthrough - We want to continue the loop, regardless of the result */
		}
		if (ctx->config->debug_mode) {
			query_encrypt_pipes(ctx->config);
			query_decrypt_pipes(ctx->config);
			for (i = 0; i < ctx->config->app_rules.nb_decrypt_rules; i++)
				query_bad_syndrome(&ctx->config->app_rules.decrypt_rules[i]);
		}
		end_time = rte_get_timer_cycles();
		delta = (end_time - start_time) / (rte_get_timer_hz() / ONE_SECOND_IN_MICROSECONDS);
		if (delta < cycle_time) {
			usleep(cycle_time - delta);
		}
	}
	free(ctx);
}

/*
 * Return true if we run in debug mode and send the bad syndrome packets to RSS queue
 *
 * @app_cfg [in]: application configuration struct
 * @return: true if we send the bad syndrome packets to RSS
 */
static inline bool is_fwd_syndrome_rss(struct ipsec_security_gw_config *app_cfg)
{
	return (app_cfg->debug_mode && app_cfg->syndrome_fwd == IPSEC_SECURITY_GW_FWD_SYNDROME_RSS);
}

/*
 * Handling the new received packet, send to process encap or decap based on src port
 *
 * @port_id [in]: port ID
 * @nb_packets [in]: size of mbufs array
 * @packets [in]: array of packets
 * @ctx [in]: core context struct
 * @nb_processed_packets [out]: number of processed packets
 * @processed_packets [out]: array of processed packets
 * @unprocessed_packets [out]: array of unprocessed packets
 */
static void handle_packets_received(uint16_t port_id,
				    uint16_t nb_packets,
				    struct rte_mbuf **packets,
				    struct ipsec_security_gw_core_ctx *ctx,
				    uint16_t *nb_processed_packets,
				    struct rte_mbuf **processed_packets,
				    struct rte_mbuf **unprocessed_packets)
{
	uint32_t current_packet;
	uint32_t pkt_meta;
	bool encrypt;
	int unprocessed_packets_idx = 0;
	doca_error_t result;

	*nb_processed_packets = 0;

	for (current_packet = 0; current_packet < nb_packets; current_packet++) {
		if (ctx->config->flow_mode == IPSEC_SECURITY_GW_SWITCH) {
			pkt_meta = *RTE_FLOW_DYNF_METADATA(packets[current_packet]);
			encrypt = ((union security_gateway_pkt_meta)pkt_meta).encrypt;
			if (encrypt)
				port_id = ctx->ports[UNSECURED_IDX]->port_id;
			else
				port_id = ctx->ports[SECURED_IDX]->port_id;
		}
		if (port_id == (ctx->ports[UNSECURED_IDX])->port_id)
			result = handle_unsecured_packets_received(&packets[current_packet], ctx);
		else
			result = handle_secured_packets_received(&packets[current_packet],
								 is_fwd_syndrome_rss(ctx->config),
								 ctx);
		if (result != DOCA_SUCCESS)
			goto add_dropped;

		processed_packets[(*nb_processed_packets)++] = packets[current_packet];
		continue;

add_dropped:
		unprocessed_packets[unprocessed_packets_idx++] = packets[current_packet];
	}
}

/*
 * Receive the income packets from the RX queue process them, and send it to the TX queue in the second port
 *
 * @args [in]: generic pointer to core context struct
 */
static void process_queue_packets(void *args)
{
	uint16_t port_id;
	uint16_t nb_packets_received;
	uint16_t nb_processed_packets = 0;
	uint16_t nb_packets_to_drop;
	struct rte_mbuf *packets[PACKET_BURST];
	struct rte_mbuf *processed_packets[PACKET_BURST] = {0};
	struct rte_mbuf *packets_to_drop[PACKET_BURST] = {0};
	int nb_pkts;
	int num_of_tries = NB_TX_BURST_TRIES;
	struct ipsec_security_gw_core_ctx *ctx = (struct ipsec_security_gw_core_ctx *)args;
	uint16_t nb_ports = ctx->config->dpdk_config->port_config.nb_ports;
	uint16_t tx_port;

	DOCA_LOG_DBG("Core %u is receiving packets", rte_lcore_id());
	while (!force_quit) {
		for (port_id = 0; port_id < nb_ports; port_id++) {
			nb_packets_received = rte_eth_rx_burst(port_id, ctx->queue_id, packets, PACKET_BURST);
			if (nb_packets_received) {
				DOCA_LOG_TRC("Received %d packets from port %d on core %u",
					     nb_packets_received,
					     port_id,
					     rte_lcore_id());
				handle_packets_received(port_id,
							nb_packets_received,
							packets,
							ctx,
							&nb_processed_packets,
							processed_packets,
							packets_to_drop);
				nb_pkts = 0;
				if (ctx->config->flow_mode == IPSEC_SECURITY_GW_VNF) {
					tx_port = port_id ^ 1;
				} else {
					tx_port = port_id;
				}
				do {
					nb_pkts += rte_eth_tx_burst(tx_port,
								    ctx->queue_id,
								    processed_packets + nb_pkts,
								    nb_processed_packets - nb_pkts);
					num_of_tries--;
				} while (nb_processed_packets > nb_pkts && num_of_tries > 0);
				if (nb_processed_packets > nb_pkts)
					DOCA_LOG_WARN(
						"%d packets were dropped during the transmission to the next port",
						(nb_processed_packets - nb_pkts));
				nb_packets_to_drop = nb_packets_received - nb_processed_packets;
				if (nb_packets_to_drop > 0) {
					DOCA_LOG_WARN("%d packets were dropped during the processing",
						      nb_packets_to_drop);
					rte_pktmbuf_free_bulk(packets_to_drop, nb_packets_to_drop);
				}
			}
		}
	}
	free(ctx);
}

/*
 * Run on lcore 1 process_syndrome_packets() to query the bad syndrome entries
 *
 * @config [in]: application configuration struct
 * @ports [in]: application ports
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ipsec_security_gw_process_bad_packets(struct ipsec_security_gw_config *config,
							  struct ipsec_security_gw_ports_map *ports[])
{
	uint16_t lcore_index = 0;
	int current_lcore = 0;
	struct ipsec_security_gw_core_ctx *ctx;

	current_lcore = rte_get_next_lcore(current_lcore, true, false);

	ctx = (struct ipsec_security_gw_core_ctx *)malloc(sizeof(struct ipsec_security_gw_core_ctx));
	if (ctx == NULL) {
		DOCA_LOG_ERR("malloc() failed");
		return DOCA_ERROR_NO_MEMORY;
	}
	ctx->queue_id = lcore_index;
	ctx->config = config;
	ctx->encrypt_rules = config->app_rules.encrypt_rules;
	ctx->decrypt_rules = config->app_rules.decrypt_rules;
	ctx->nb_encrypt_rules = &config->app_rules.nb_encrypt_rules;
	ctx->ports = ports;

	if (rte_eal_remote_launch((void *)process_syndrome_packets, (void *)ctx, current_lcore) != 0) {
		DOCA_LOG_ERR("Remote launch failed");
		free(ctx);
		return DOCA_ERROR_DRIVER;
	}
	return DOCA_SUCCESS;
}

/*
 * Run on each lcore process_queue_packets() to receive and send packets in a loop
 *
 * @config [in]: application configuration struct
 * @ports [in]: application ports
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ipsec_security_gw_process_packets(struct ipsec_security_gw_config *config,
						      struct ipsec_security_gw_ports_map *ports[])
{
	uint16_t lcore_index = 0;
	int current_lcore = 0;
	struct ipsec_security_gw_core_ctx *ctx;
	int nb_queues = config->dpdk_config->port_config.nb_queues;

	while ((current_lcore < RTE_MAX_LCORE) && (lcore_index < nb_queues)) {
		current_lcore = rte_get_next_lcore(current_lcore, true, false);
		ctx = (struct ipsec_security_gw_core_ctx *)malloc(sizeof(struct ipsec_security_gw_core_ctx));
		if (ctx == NULL) {
			DOCA_LOG_ERR("malloc() failed");
			force_quit = true;
			return DOCA_ERROR_NO_MEMORY;
		}
		ctx->queue_id = lcore_index;
		ctx->config = config;
		ctx->encrypt_rules = config->app_rules.encrypt_rules;
		ctx->decrypt_rules = config->app_rules.decrypt_rules;
		ctx->nb_encrypt_rules = &config->app_rules.nb_encrypt_rules;
		ctx->ports = ports;

		/* Launch the worker to start process packets */
		if (lcore_index == 0) {
			/* lcore index 0 will not get regular packets to process */
			if (rte_eal_remote_launch((void *)process_syndrome_packets, (void *)ctx, current_lcore) != 0) {
				DOCA_LOG_ERR("Remote launch failed");
				free(ctx);
				force_quit = true;
				return DOCA_ERROR_DRIVER;
			}
		} else {
			if (rte_eal_remote_launch((void *)process_queue_packets, (void *)ctx, current_lcore) != 0) {
				DOCA_LOG_ERR("Remote launch failed");
				free(ctx);
				force_quit = true;
				return DOCA_ERROR_DRIVER;
			}
		}
		lcore_index++;
	}
	return DOCA_SUCCESS;
}

/*
 * Unpack external buffer for new policy
 *
 * @buf [in]: buffer to unpack
 * @nb_bytes [in]: buffer size
 * @policy [out]: policy pointer to store the unpacked values
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t unpack_policy_buffer(uint8_t *buf, uint32_t nb_bytes, struct ipsec_security_gw_ipsec_policy *policy)
{
	uint8_t *ptr = buf;

	policy->src_port = unpack_uint16(&ptr);
	policy->dst_port = unpack_uint16(&ptr);
	policy->l3_protocol = unpack_uint8(&ptr);
	policy->l4_protocol = unpack_uint8(&ptr);
	policy->outer_l3_protocol = unpack_uint8(&ptr);
	policy->policy_direction = unpack_uint8(&ptr);
	policy->policy_mode = unpack_uint8(&ptr);
	policy->esn = unpack_uint8(&ptr);
	policy->icv_length = unpack_uint8(&ptr);
	policy->key_type = unpack_uint8(&ptr);
	policy->spi = unpack_uint32(&ptr);
	policy->salt = unpack_uint32(&ptr);
	unpack_blob(&ptr, MAX_IP_ADDR_LEN + 1, (uint8_t *)policy->src_ip_addr);
	policy->src_ip_addr[MAX_IP_ADDR_LEN] = '\0';
	unpack_blob(&ptr, MAX_IP_ADDR_LEN + 1, (uint8_t *)policy->dst_ip_addr);
	policy->dst_ip_addr[MAX_IP_ADDR_LEN] = '\0';
	unpack_blob(&ptr, MAX_IP_ADDR_LEN + 1, (uint8_t *)policy->outer_src_ip);
	policy->outer_src_ip[MAX_IP_ADDR_LEN] = '\0';
	unpack_blob(&ptr, MAX_IP_ADDR_LEN + 1, (uint8_t *)policy->outer_dst_ip);
	policy->outer_dst_ip[MAX_IP_ADDR_LEN] = '\0';
	if (nb_bytes == POLICY_RECORD_MAX_SIZE)
		unpack_blob(&ptr, 32, (uint8_t *)policy->enc_key_data);
	else
		unpack_blob(&ptr, 16, (uint8_t *)policy->enc_key_data);
	return DOCA_SUCCESS;
}

/*
 * Read bytes_to_read from given socket
 *
 * @fd [in]: socket file descriptor
 * @bytes_to_read [in]: number of bytes to read
 * @buf [out]: store data from socket
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t fill_buffer_from_socket(int fd, size_t bytes_to_read, uint8_t *buf)
{
	ssize_t ret;
	size_t bytes_received = 0;

	do {
		ret = recv(fd, buf + bytes_received, bytes_to_read - bytes_received, 0);
		if (ret == -1) {
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				return DOCA_ERROR_AGAIN;
			else {
				DOCA_LOG_ERR("Failed to read from socket buffer [%s]", strerror(errno));
				return DOCA_ERROR_IO_FAILED;
			}
		}
		if (ret == 0)
			return DOCA_ERROR_AGAIN;
		bytes_received += ret;
	} while (bytes_received < bytes_to_read);

	return DOCA_SUCCESS;
}

/*
 * Read first 4 bytes from the socket to know policy length
 *
 * @app_cfg [in]: application configuration struct
 * @length [out]: policy length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t read_message_length(struct ipsec_security_gw_config *app_cfg, uint32_t *length)
{
	uint8_t buf[8] = {0};
	uint8_t *ptr = &buf[0];
	uint32_t policy_length;
	doca_error_t result;

	result = fill_buffer_from_socket(app_cfg->socket_ctx.connfd, sizeof(uint32_t), buf);
	if (result != DOCA_SUCCESS)
		return result;

	policy_length = unpack_uint32(&ptr);
	if (policy_length != POLICY_RECORD_MIN_SIZE && policy_length != POLICY_RECORD_MAX_SIZE) {
		DOCA_LOG_ERR("Wrong policy length [%u], should be [%u] or [%u]",
			     policy_length,
			     POLICY_RECORD_MIN_SIZE,
			     POLICY_RECORD_MAX_SIZE);
		return DOCA_ERROR_IO_FAILED;
	}

	*length = policy_length;
	return DOCA_SUCCESS;
}

/*
 * check if new policy where added to the socket and read it.
 *
 * @app_cfg [in]: application configuration struct
 * @policy [out]: policy structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t read_message_from_socket(struct ipsec_security_gw_config *app_cfg,
					     struct ipsec_security_gw_ipsec_policy *policy)
{
	uint8_t buffer[1024] = {0};
	uint32_t policy_length;
	doca_error_t result;

	result = read_message_length(app_cfg, &policy_length);
	if (result != DOCA_SUCCESS)
		return result;

	result = fill_buffer_from_socket(app_cfg->socket_ctx.connfd, policy_length, buffer);
	if (result != DOCA_SUCCESS)
		return result;

	return unpack_policy_buffer(buffer, policy_length, policy);
}

/*
 * Handle SW increment of SN - set the initial SN for each rule
 *
 * @app_cfg [in]: application configuration struct
 * @encrypt_rules [in]: encryption rules
 * @encrypt_array_size [in]: size of the encryption rules array
 */
static void sw_handling_sn_inc(struct ipsec_security_gw_config *app_cfg,
			       struct encrypt_rule *encrypt_rules,
			       int encrypt_array_size)
{
	int entry_idx;

	for (entry_idx = 0; entry_idx < encrypt_array_size; entry_idx++) {
		encrypt_rules[entry_idx].current_sn = (uint32_t)(app_cfg->sn_initial);
	}
}

/*
 * Handle SW anti-replay - set the anti-replay state for each rule
 *
 * @app_cfg [in]: application configuration struct
 * @decrypt_rules [in]: decryption rules
 * @decrypt_array_size [in]: size of the decryption rules array
 */
static void sw_handling_antireplay(struct ipsec_security_gw_config *app_cfg,
				   struct decrypt_rule *decrypt_rules,
				   int decrypt_array_size)
{
	int entry_idx;

	for (entry_idx = 0; entry_idx < decrypt_array_size; entry_idx++) {
		decrypt_rules[entry_idx].antireplay_state.window_size = SW_WINDOW_SIZE;
		decrypt_rules[entry_idx].antireplay_state.end_win_sn =
			(uint32_t)(app_cfg->sn_initial) + SW_WINDOW_SIZE - 1;
		decrypt_rules[entry_idx].antireplay_state.bitmap = 0;
	}
}

/*
 * Wait in a loop and process packets until receive signal
 *
 * @app_cfg [in]: application configuration struct
 * @ports [in]: application ports
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ipsec_security_gw_wait_for_traffic(struct ipsec_security_gw_config *app_cfg,
						       struct ipsec_security_gw_ports_map *ports[])
{
	doca_error_t result = DOCA_SUCCESS;
	struct ipsec_security_gw_ipsec_policy policy = {0};
	struct doca_flow_port *secured_port;
	struct encrypt_rule *enc_rule;
	struct decrypt_rule *dec_rule;
	int encrypt_array_size, decrypt_array_size;

	DOCA_LOG_INFO("Waiting for traffic, press Ctrl+C for termination");
	if (app_cfg->offload != IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH || is_fwd_syndrome_rss(app_cfg)) {
		encrypt_array_size = app_cfg->socket_ctx.socket_conf ? DYN_RESERVED_RULES :
								       app_cfg->app_rules.nb_encrypt_rules;
		decrypt_array_size = app_cfg->socket_ctx.socket_conf ? DYN_RESERVED_RULES :
								       app_cfg->app_rules.nb_decrypt_rules;
		if (app_cfg->sw_sn_inc_enable) {
			sw_handling_sn_inc(app_cfg, app_cfg->app_rules.encrypt_rules, encrypt_array_size);
		}
		if (app_cfg->sw_antireplay) {
			/* Create and allocate an anti-replay state for each entry */
			sw_handling_antireplay(app_cfg, app_cfg->app_rules.decrypt_rules, decrypt_array_size);
		}
		result = ipsec_security_gw_process_packets(app_cfg, ports);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process packets on all lcores");
			goto exit_failure;
		}
	} else {
		result = ipsec_security_gw_process_bad_packets(app_cfg, ports);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process packets");
			goto exit_failure;
		}
	}

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF)
		secured_port = ports[SECURED_IDX]->port;
	else
		secured_port = doca_flow_port_switch_get(NULL);

	while (!force_quit) {
		if (!app_cfg->socket_ctx.socket_conf) {
			sleep(1);
			continue;
		}

		memset(&policy, 0, sizeof(policy));
		result = read_message_from_socket(app_cfg, &policy);
		if (result != DOCA_SUCCESS) {
			if (result == DOCA_ERROR_AGAIN) {
				DOCA_LOG_DBG("No new IPsec policy, try again");
				sleep(1);
				continue;
			} else {
				DOCA_LOG_ERR("Failed to read new IPSEC policy [%s]", doca_error_get_descr(result));
				goto exit_failure;
			}
		}

		print_policy_attrs(&policy);

		if (policy.policy_direction == POLICY_DIR_OUT) {
			if (app_cfg->app_rules.nb_encrypt_rules >= MAX_NB_RULES) {
				DOCA_LOG_ERR("Can't receive more encryption policies, maximum size is [%d]",
					     MAX_NB_RULES);
				result = DOCA_ERROR_BAD_STATE;
				goto exit_failure;
			}
			/* Check if the array is full and reallocate memory of DYN_RESERVED_RULES blocks */
			if ((app_cfg->app_rules.nb_encrypt_rules != 0) &&
			    (app_cfg->app_rules.nb_encrypt_rules % DYN_RESERVED_RULES == 0)) {
				DOCA_LOG_DBG("Reallocating memory for new encryption rules");
				app_cfg->app_rules.encrypt_rules =
					realloc(app_cfg->app_rules.encrypt_rules,
						(app_cfg->app_rules.nb_encrypt_rules + DYN_RESERVED_RULES) *
							sizeof(struct encrypt_rule));
				if (app_cfg->app_rules.encrypt_rules == NULL) {
					DOCA_LOG_ERR("Failed to allocate memory for new encryption rule");
					result = DOCA_ERROR_NO_MEMORY;
					goto exit_failure;
				}

				if (app_cfg->sw_sn_inc_enable) {
					sw_handling_sn_inc(app_cfg,
							   app_cfg->app_rules.encrypt_rules +
								   app_cfg->app_rules.nb_encrypt_rules,
							   DYN_RESERVED_RULES);
				}
			}
			/* Get the next empty encryption rule for egress traffic */
			enc_rule = &app_cfg->app_rules.encrypt_rules[app_cfg->app_rules.nb_encrypt_rules];
			result = ipsec_security_gw_handle_encrypt_policy(app_cfg, ports, &policy, enc_rule);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to handle new encryption policy");
				goto exit_failure;
			}
		} else if (policy.policy_direction == POLICY_DIR_IN) {
			if (app_cfg->app_rules.nb_decrypt_rules >= MAX_NB_RULES) {
				DOCA_LOG_ERR("Can't receive more decryption policies, maximum size is [%d]",
					     MAX_NB_RULES);
				result = DOCA_ERROR_BAD_STATE;
				goto exit_failure;
			}
			/* Check if the array is full and reallocate memory of DYN_RESERVED_RULES blocks */
			if ((app_cfg->app_rules.nb_decrypt_rules != 0) &&
			    (app_cfg->app_rules.nb_decrypt_rules % DYN_RESERVED_RULES == 0)) {
				app_cfg->app_rules.decrypt_rules =
					realloc(app_cfg->app_rules.decrypt_rules,
						(app_cfg->app_rules.nb_decrypt_rules + DYN_RESERVED_RULES) *
							sizeof(struct decrypt_rule));
				if (app_cfg->app_rules.decrypt_rules == NULL) {
					DOCA_LOG_ERR("Failed to allocate memory for new decryption rule");
					result = DOCA_ERROR_NO_MEMORY;
					goto exit_failure;
				}
				if (app_cfg->sw_antireplay) {
					sw_handling_antireplay(app_cfg,
							       app_cfg->app_rules.decrypt_rules +
								       app_cfg->app_rules.nb_decrypt_rules,
							       DYN_RESERVED_RULES);
				}
			}
			/* Get the next empty decryption rule for ingress traffic */
			dec_rule = &app_cfg->app_rules.decrypt_rules[app_cfg->app_rules.nb_decrypt_rules];
			result = ipsec_security_gw_handle_decrypt_policy(app_cfg, secured_port, &policy, dec_rule);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to handle new decryption policy");
				goto exit_failure;
			}
		}
	}

exit_failure:
	force_quit = true;
	/* If SW offload is enabled, wait till threads finish */
	rte_eal_mp_wait_lcore();

	if (app_cfg->socket_ctx.socket_conf) {
		/* Close the connection */
		close(app_cfg->socket_ctx.connfd);
		close(app_cfg->socket_ctx.fd);

		/* Remove the socket file */
		unlink(app_cfg->socket_ctx.socket_path);
	}
	return result;
}

/*
 * Create socket connection, including opening new fd for socket, binding shared file, and listening for new connection
 *
 * @app_cfg [in/out]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_policy_socket(struct ipsec_security_gw_config *app_cfg)
{
	struct sockaddr_un addr;
	int fd, connfd = -1, flags;
	doca_error_t result;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	strlcpy(addr.sun_path, app_cfg->socket_ctx.socket_path, MAX_SOCKET_PATH_NAME);

	/* Create a Unix domain socket */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		DOCA_LOG_ERR("Failed to create new socket [%s]", strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}

	/* Set socket as non blocking */
	flags = fcntl(fd, F_GETFL, 0);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		DOCA_LOG_ERR("Failed to set socket as non blocking [%s]", strerror(errno));
		close(fd);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Bind the socket to a file path */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		DOCA_LOG_ERR("Failed to bind the socket with the file path [%s]", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto exit_failure;
	}

	/* Listen for incoming connections */
	if (listen(fd, 5) == -1) {
		DOCA_LOG_ERR("Failed to listen for incoming connection [%s]", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto exit_failure;
	}

	DOCA_LOG_DBG("Waiting for establishing new connection");

	/* Accept an incoming connection */
	while (!force_quit) {
		connfd = accept(fd, NULL, NULL);
		if (connfd == -1) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				sleep(1); /* No pending connections at the moment
					   * Wait for a short period and retry
					   */
				continue;
			}
			DOCA_LOG_ERR("Failed to accept incoming connection [%s]", strerror(errno));
			result = DOCA_ERROR_IO_FAILED;
			goto exit_failure;
		} else
			break;
	}

	if (connfd == -1)
		return DOCA_SUCCESS;

	/* Set socket as non blocking */
	flags = fcntl(connfd, F_GETFL, 0);
	if (fcntl(connfd, F_SETFL, flags | O_NONBLOCK) == -1) {
		DOCA_LOG_ERR("Failed to set connection socket as non blocking [%s]", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		close(connfd);
		goto exit_failure;
	}
	app_cfg->socket_ctx.connfd = connfd;
	app_cfg->socket_ctx.fd = fd;
	return DOCA_SUCCESS;

exit_failure:
	close(fd);
	/* Remove the socket file */
	unlink(app_cfg->socket_ctx.socket_path);
	return result;
}

/*
 * Worker function to insert rules on the queues
 *
 * @args [in]: generic pointer to multi_thread_insertion_ctx struct
 */
static void rule_inserter_worker(void *args)
{
	struct multi_thread_insertion_ctx *ctx = (struct multi_thread_insertion_ctx *)args;
	doca_error_t result;

	DOCA_LOG_DBG("Core %d is inserting on queue %d: %d encryption rules and %d decryption rules",
		     rte_lcore_id(),
		     ctx->queue_id,
		     ctx->nb_encrypt_rules,
		     ctx->nb_decrypt_rules);
	if (ctx->nb_encrypt_rules > 0) {
		result = add_encrypt_entries(ctx->app_cfg,
					     ctx->ports,
					     ctx->queue_id,
					     ctx->nb_encrypt_rules,
					     ctx->encrypt_rule_offset);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add encrypt entries");
			force_quit = true;
			return;
		}
	}
	if (ctx->nb_decrypt_rules > 0) {
		result = add_decrypt_entries(ctx->app_cfg,
					     ctx->ports[SECURED_IDX],
					     ctx->queue_id,
					     ctx->nb_decrypt_rules,
					     ctx->decrypt_rule_offset);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add decrypt entries");
			force_quit = true;
			return;
		}
	}
}

/*
 * Check if insertion rate measure is enabled
 *
 * @app_cfg [in]: application configuration struct
 * @return: true if BW perf is enabled.
 */
static inline bool is_insertion_rate(struct ipsec_security_gw_config *app_cfg)
{
	return (app_cfg->perf_measurement == IPSEC_SECURITY_GW_PERF_INSERTION_RATE ||
		app_cfg->perf_measurement == IPSEC_SECURITY_GW_PERF_BOTH);
}

/*
 * Run multi-thread insertion on the queues
 *
 * @app_cfg [in]: application configuration struct
 * @ports [in]: application ports
 * @nb_queues [in]: number of queues
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_multithread_insertion(struct ipsec_security_gw_config *app_cfg,
					      struct ipsec_security_gw_ports_map *ports[],
					      int nb_queues)
{
	uint16_t lcore_index;
	int current_lcore = 0;
	struct multi_thread_insertion_ctx *ctxs, *ctx;
	float encrypt_ratio, decrypt_ratio;
	int next_encrypt_rule_offset = 0, next_decrypt_rule_offset = 0;
	double start_time, end_time, total_time;
	double rules_per_sec;
	bool insertion_rate_print = is_insertion_rate(app_cfg);

	if (nb_queues < 1 || RTE_MAX_LCORE < nb_queues) {
		DOCA_LOG_ERR("Invalid number of queues");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (app_cfg->app_rules.nb_encrypt_rules == 0 && app_cfg->app_rules.nb_decrypt_rules == 0) {
		DOCA_LOG_WARN("No rules to insert");
		return DOCA_SUCCESS;
	}

	/* Calculate the number of queues to run - Don't create cores with less than MIN_ENTRIES_PER_CORE entries for
	 * encrypt/ decrypt */
	nb_queues = RTE_MIN(nb_queues,
			    RTE_MAX(RTE_MAX((int)(app_cfg->app_rules.nb_encrypt_rules / MIN_ENTRIES_PER_CORE),
					    (int)(app_cfg->app_rules.nb_decrypt_rules / MIN_ENTRIES_PER_CORE)),
				    1));

	DOCA_LOG_DBG("Running multi-thread insertion on %d queues", nb_queues);

	ctxs = (struct multi_thread_insertion_ctx *)malloc(sizeof(struct multi_thread_insertion_ctx) * nb_queues);
	if (ctxs == NULL) {
		DOCA_LOG_ERR("malloc() failed");
		return DOCA_ERROR_NO_MEMORY;
	}
	encrypt_ratio = (float)app_cfg->app_rules.nb_encrypt_rules / nb_queues;
	decrypt_ratio = (float)app_cfg->app_rules.nb_decrypt_rules / nb_queues;

	if (insertion_rate_print)
		start_time = rte_get_timer_cycles();
	for (lcore_index = 0; lcore_index < nb_queues; lcore_index++) {
		current_lcore = rte_get_next_lcore(current_lcore, true, false);
		if (current_lcore == RTE_MAX_LCORE) {
			DOCA_LOG_ERR("Not enough cores to run multi-thread insertion");
			free(ctxs);
			return DOCA_ERROR_INVALID_VALUE;
		}
		ctx = ctxs + lcore_index;

		ctx->app_cfg = app_cfg;
		ctx->ports = ports;
		ctx->queue_id = lcore_index;
		if (encrypt_ratio < 1) { /* If the number of rules is less than the number of queues */
			ctx->nb_encrypt_rules = lcore_index < app_cfg->app_rules.nb_encrypt_rules ? 1 : 0;
			ctx->encrypt_rule_offset = lcore_index;
		} else {
			ctx->encrypt_rule_offset = next_encrypt_rule_offset;
			next_encrypt_rule_offset = (lcore_index != (nb_queues - 1)) ?
							   (int)((lcore_index + 1) * encrypt_ratio) :
							   app_cfg->app_rules.nb_encrypt_rules;
			ctx->nb_encrypt_rules = next_encrypt_rule_offset - ctx->encrypt_rule_offset;
		}
		if (decrypt_ratio < 1) { /* If the number of rules is less than the number of queues */
			ctx->nb_decrypt_rules = lcore_index < app_cfg->app_rules.nb_decrypt_rules ? 1 : 0;
			ctx->decrypt_rule_offset = lcore_index;
		} else {
			ctx->decrypt_rule_offset = next_decrypt_rule_offset;
			next_decrypt_rule_offset = (lcore_index != (nb_queues - 1)) ?
							   (int)((lcore_index + 1) * decrypt_ratio) :
							   app_cfg->app_rules.nb_decrypt_rules;
			ctx->nb_decrypt_rules = next_decrypt_rule_offset - ctx->decrypt_rule_offset;
		}

		if (rte_eal_remote_launch((void *)rule_inserter_worker, (void *)ctx, current_lcore) != 0) {
			DOCA_LOG_ERR("Remote launch failed");
			free(ctxs);
			return DOCA_ERROR_DRIVER;
		}
	}

	/* Wait for all threads to finish */
	rte_eal_mp_wait_lcore();
	if (force_quit) {
		DOCA_LOG_ERR("Failed to insert entries");
		return DOCA_ERROR_BAD_STATE;
	}
	if (insertion_rate_print) {
		end_time = rte_get_timer_cycles();
		total_time = (end_time - start_time) / rte_get_timer_hz();
	}
	DOCA_LOG_DBG("All threads finished inserting rules");
	if (insertion_rate_print) {
		DOCA_LOG_INFO("Total insertion time for %d encryption rules and %d decryption rules: %f",
			      app_cfg->app_rules.nb_encrypt_rules,
			      app_cfg->app_rules.nb_decrypt_rules,
			      total_time);
		rules_per_sec =
			(app_cfg->app_rules.nb_encrypt_rules + app_cfg->app_rules.nb_decrypt_rules) / total_time;
		DOCA_LOG_INFO("Rules/Sec: %f", rules_per_sec);
	}

	free(ctxs);
	return DOCA_SUCCESS;
}

/*
 * IPsec Security Gateway application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	int ret, nb_ports = 2;
	int exit_status = EXIT_SUCCESS;
	struct ipsec_security_gw_ports_map *ports[nb_ports];
	struct ipsec_security_gw_config app_cfg = {0};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = nb_ports,
		.port_config.nb_queues = 2, /* This will be updated according to app_cfg.nb_cores in
					       dpdk_queues_and_ports_init() */
		.port_config.enable_mbuf_metadata = true,
		.reserve_main_thread = true,
	};
	char cores_str[10];
	char pid_str[50]; /* Buffer for "pid_" + process ID */
	snprintf(pid_str, sizeof(pid_str), "pid_%d", getpid());
	char *eal_param[7] = {"", "-a", "00:00.0", "-l", "", "--file-prefix", pid_str};
	struct doca_log_backend *sdk_log;

	app_cfg.dpdk_config = &dpdk_config;
	app_cfg.nb_cores = DEFAULT_NB_CORES;
	/* Per the RFC, the initial SN on the wire should be 1, so use it as the default value */
	app_cfg.sn_initial = 1;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	force_quit = false;

	/* Init ARGP interface and start parsing cmdline/json arguments */
	result = doca_argp_init(NULL, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}
	result = register_ipsec_security_gw_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application params: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	snprintf(cores_str, sizeof(cores_str), "0-%d", app_cfg.nb_cores - 1);
	eal_param[4] = cores_str;
	ret = rte_eal_init(7, eal_param);
	if (ret < 0) {
		DOCA_LOG_ERR("EAL initialization failed");
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = ipsec_security_gw_parse_config(&app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application json file: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto dpdk_destroy;
	}

	if (app_cfg.flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		dpdk_config.port_config.switch_mode = true;
	}

	result = ipsec_security_gw_init_devices(&app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA devices: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto config_destroy;
	}

	/* Update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update application ports and queues: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto device_cleanup;
	}

	result = ipsec_security_gw_init_doca_flow(&app_cfg, dpdk_config.port_config.nb_queues, ports);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow");
		exit_status = EXIT_FAILURE;
		goto dpdk_cleanup;
	}

	result = doca_devinfo_get_mac_addr(doca_dev_as_devinfo((app_cfg.objects.secured_dev.doca_dev)),
					   ports[SECURED_IDX]->eth_header.src_mac,
					   MAC_ADDRESS_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to get mac address from DOCA device, setting mac address to default");
		SET_MAC_ADDR(ports[SECURED_IDX]->eth_header.src_mac, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
	}

	if (app_cfg.flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		DOCA_LOG_WARN("switch mode can not set real source mac, setting mac address to default");
		SET_MAC_ADDR(ports[UNSECURED_IDX]->eth_header.src_mac, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
	}

	else {
		result = doca_devinfo_get_mac_addr(doca_dev_as_devinfo((app_cfg.objects.unsecured_dev.doca_dev)),
						   ports[UNSECURED_IDX]->eth_header.src_mac,
						   MAC_ADDRESS_SIZE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to get mac address from DOCA device, setting mac address to default");
			SET_MAC_ADDR(ports[UNSECURED_IDX]->eth_header.src_mac, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
		}
	}

	result = ipsec_security_gw_init_status(&app_cfg, dpdk_config.port_config.nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init status entries");
		exit_status = EXIT_FAILURE;
		goto doca_flow_cleanup;
	}

	if (app_cfg.flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		result = create_rss_pipe(&app_cfg,
					 ports[SECURED_IDX]->port,
					 dpdk_config.port_config.nb_queues,
					 &app_cfg.switch_pipes.rss_pipe.pipe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create encrypt egress pipes");
			exit_status = EXIT_FAILURE;
			goto doca_flow_cleanup;
		}
	}

	result = ipsec_security_gw_bind(ports, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind encrypt and decrypt rules");
		exit_status = EXIT_FAILURE;
		goto doca_flow_cleanup;
	}

	result = ipsec_security_gw_create_encrypt_egress(ports, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encrypt egress pipes");
		exit_status = EXIT_FAILURE;
		goto doca_flow_cleanup;
	}

	if (app_cfg.flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		result = create_switch_egress_root_pipes(ports, &app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create switch root pipe");
			exit_status = EXIT_FAILURE;
			goto doca_flow_cleanup;
		}
	}

	result = ipsec_security_gw_insert_encrypt_rules(ports, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create encrypt rules");
		exit_status = EXIT_FAILURE;
		goto doca_flow_cleanup;
	}

	result = ipsec_security_gw_insert_decrypt_rules(ports, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create decrypt rules");
		exit_status = EXIT_FAILURE;
		goto doca_flow_cleanup;
	}

	if (app_cfg.flow_mode == IPSEC_SECURITY_GW_SWITCH) {
		result = create_switch_ingress_root_pipes(ports, &app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create switch root pipe");
			exit_status = EXIT_FAILURE;
			goto doca_flow_cleanup;
		}
	}

	if (!app_cfg.socket_ctx.socket_conf) {
		result = run_multithread_insertion(&app_cfg, ports, app_cfg.dpdk_config->port_config.nb_queues);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to run multi-thread insertion");
			exit_status = EXIT_FAILURE;
			goto doca_flow_cleanup;
		}
	} else {
		app_cfg.app_rules.nb_encrypt_rules = 0;
		app_cfg.app_rules.nb_decrypt_rules = 0;
		result = create_policy_socket(&app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create policy socket");
			exit_status = EXIT_FAILURE;
			goto doca_flow_cleanup;
		}
	}

	result = ipsec_security_gw_wait_for_traffic(&app_cfg, ports);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error happened during waiting for new traffic");
		exit_status = EXIT_FAILURE;
		goto doca_flow_cleanup;
	}

doca_flow_cleanup:
	security_gateway_free_resources(&app_cfg);
	/* Flow cleanup */
	doca_flow_cleanup(nb_ports, ports);
	security_gateway_free_status_entries(&app_cfg);
	doca_flow_tune_server_destroy();
dpdk_cleanup:
	/* DPDK cleanup */
	dpdk_queues_and_ports_fini(&dpdk_config);
device_cleanup:
	ipsec_security_gw_close_devices(&app_cfg);
config_destroy:
	if (app_cfg.app_rules.encrypt_rules)
		free(app_cfg.app_rules.encrypt_rules);
	if (app_cfg.app_rules.decrypt_rules)
		free(app_cfg.app_rules.decrypt_rules);
dpdk_destroy:
	dpdk_fini();
	/* ARGP cleanup */
	doca_argp_destroy();
	return exit_status;
}
