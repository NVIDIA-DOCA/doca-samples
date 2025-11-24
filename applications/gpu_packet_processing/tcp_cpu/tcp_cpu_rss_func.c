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

#include "tcp_cpu_rss_func.h"
#include "tcp_session_table.h"
#include "packets.h"

#define TCP_CPU_RSS_QUEUE_DEPTH 2048
#define TCP_PACKET_MAX_BURST_SIZE 128

DOCA_LOG_REGISTER(TCP_CPU_RSS);

/*
 * Extract the address of the Ethernet header contained in the
 * raw ethernet frame packet buffer if present; otherwise null.
 *
 * @packet [in]: Packet to extract Ethernet hdr
 * @return: ptr on success and NULL otherwise
 */
static struct ether_hdr *extract_eth_hdr(struct doca_buf *packet)
{
	void *pkt_data;
	struct ether_hdr *eth_hdr;

	(void)doca_buf_get_data(packet, &pkt_data);
	eth_hdr = (struct ether_hdr *)pkt_data;

	return eth_hdr;
}

/*
 * Extract the address of the IPv4 TCP header contained in the
 * raw ethernet frame packet buffer if present; otherwise null.
 *
 * @packet [in]: Packet to extract TCP hdr
 * @return: ptr on success and NULL otherwise
 */
static struct tcp_hdr *extract_tcp_hdr(struct doca_buf *packet)
{
	struct ether_hdr *eth_hdr = extract_eth_hdr(packet);

	if (((uint16_t)htons(eth_hdr->ether_type)) != DOCA_FLOW_ETHER_TYPE_IPV4) {
		DOCA_LOG_ERR("Expected ether_type 0x%x, got 0x%x",
			     DOCA_FLOW_ETHER_TYPE_IPV4,
			     ((uint16_t)htons(eth_hdr->ether_type)));
		return NULL;
	}

	struct ipv4_hdr *ipv4_hdr = (struct ipv4_hdr *)&eth_hdr[1];

	if (ipv4_hdr->next_proto_id != IPPROTO_TCP) {
		DOCA_LOG_ERR("Expected next_proto_id %d, got %d", IPPROTO_TCP, ipv4_hdr->next_proto_id);
		return NULL;
	}

	struct tcp_hdr *tcp_hdr = (struct tcp_hdr *)&ipv4_hdr[1];

	return tcp_hdr;
}

/*
 * Extract TCP session key
 *
 * @packet [in]: Extract session key from this packet
 * @return: object
 */
static struct tcp_session_key extract_session_key(struct doca_buf *packet)
{
	struct ether_hdr *eth_hdr = extract_eth_hdr(packet);
	struct ipv4_hdr *ipv4_hdr = (struct ipv4_hdr *)&eth_hdr[1];
	struct tcp_hdr *tcp_hdr = (struct tcp_hdr *)&ipv4_hdr[1];

	struct tcp_session_key key = {
		.src_addr = ipv4_hdr->src_addr,
		.dst_addr = ipv4_hdr->dst_addr,
		.src_port = tcp_hdr->src_port,
		.dst_port = tcp_hdr->dst_port,
	};

	return key;
}

/*
 * Create TCP session
 *
 * @queue_id [in]: DPDK queue id for TCP control packets
 * @pkt [in]: pkt triggering the TCP session creation
 * @port [in]: DOCA Flow port
 * @gpu_rss_pipe [in]: DOCA Flow GPU RSS pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_tcp_session(const uint16_t queue_id,
				       struct doca_buf *pkt,
				       struct doca_flow_port *port,
				       struct doca_flow_pipe *gpu_rss_pipe)
{
	int ret;
	struct tcp_session_entry *session_entry;

	session_entry = (struct tcp_session_entry *)calloc(1, sizeof(struct tcp_session_entry));
	if (!session_entry) {
		DOCA_LOG_ERR("Failed to allocate TCP session object");
		return DOCA_ERROR_NO_MEMORY;
	}
	session_entry->key = extract_session_key(pkt);
	enable_tcp_gpu_offload(port, queue_id, gpu_rss_pipe, session_entry);

	ret = rte_hash_add_key_data(tcp_session_table, &session_entry->key, session_entry);
	if (ret != 0) {
		DOCA_LOG_ERR("Couldn't add new has key data err %d", ret);
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy TCP session
 *
 * @queue_id [in]: DPDK queue id for TCP control packets
 * @pkt [in]: pkt triggering the TCP session destruction
 * @port [in]: DOCA Flow port
 */
static void destroy_tcp_session(const uint16_t queue_id, struct doca_buf *pkt, struct doca_flow_port *port)
{
	const struct tcp_session_key key = extract_session_key(pkt);
	struct tcp_session_entry *session_entry = NULL;

	if (rte_hash_lookup_data(tcp_session_table, &key, (void **)&session_entry) < 0 || !session_entry)
		return;

	disable_tcp_gpu_offload(port, queue_id, session_entry);

	rte_hash_del_key(tcp_session_table, &key);
	free(session_entry);
}

/*
 * Log TCP flags
 *
 * @packet [in]: Packet to report TCP flags
 * @flags [in]: TCP Flags
 */
static void log_tcp_flag(struct doca_buf *packet, const char *flags)
{
	struct ether_hdr *eth_hdr = extract_eth_hdr(packet);
	struct ipv4_hdr *ipv4_hdr = (struct ipv4_hdr *)&eth_hdr[1];
	struct tcp_hdr *tcp_hdr = (struct tcp_hdr *)&ipv4_hdr[1];
	char src_addr[INET_ADDRSTRLEN];
	char dst_addr[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &ipv4_hdr->src_addr, src_addr, INET_ADDRSTRLEN);
	inet_ntop(AF_INET, &ipv4_hdr->dst_addr, dst_addr, INET_ADDRSTRLEN);
	DOCA_LOG_INFO("Received %s for TCP %s:%d>%s:%d",
		      flags,
		      src_addr,
		      htons(tcp_hdr->src_port),
		      dst_addr,
		      htons(tcp_hdr->dst_port));
}

/*
 * Create TCP ACK packet
 *
 * @src_packet [in]: Src packet to use to create ACK packet
 * @tcp_ack_pkt_pool [in]: DOCA buf pool to create ACK packets
 * @return: ptr on success and NULL otherwise
 */
static struct doca_buf *create_ack_packet(struct doca_buf *src_packet, struct doca_buf_pool *tcp_ack_pkt_pool)
{
	doca_error_t result;
	uint32_t TCP_OPT_NOP_bytes = 1;
	uint32_t TCP_OPT_MSS_nbytes = 4;
	uint32_t TCP_OPT_WND_SCALE_nbytes = 3;
	uint32_t TCP_OPT_SACK_PERMITTED_nbytes = 2;
	uint32_t TCP_OPT_TIMESTAMP_nbytes = 10;
	uint16_t mss = 8192; /* pick something */
	size_t tcp_option_array_len = TCP_OPT_MSS_nbytes + TCP_OPT_SACK_PERMITTED_nbytes + TCP_OPT_TIMESTAMP_nbytes +
				      TCP_OPT_NOP_bytes + TCP_OPT_WND_SCALE_nbytes;

	struct ether_hdr *dst_eth_hdr;
	struct ipv4_hdr *dst_ipv4_hdr;
	struct tcp_hdr *dst_tcp_hdr;
	uint8_t *dst_tcp_opts;
	struct doca_buf *dst_packet;
	struct ether_hdr *src_eth_hdr = extract_eth_hdr(src_packet);
	struct ipv4_hdr *src_ipv4_hdr = (struct ipv4_hdr *)&src_eth_hdr[1];
	struct tcp_hdr *src_tcp_hdr = (struct tcp_hdr *)&src_ipv4_hdr[1];

	if (!src_tcp_hdr->syn) {
		/* Do not bother with TCP options unless responding to SYN */
		tcp_option_array_len = 0;
	}

	result = doca_buf_pool_buf_alloc(tcp_ack_pkt_pool, &dst_packet);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate TCP ACK packet (err=%s)", doca_error_get_name(result));
		return NULL;
	}

	result = doca_buf_set_data_len(dst_packet,
				       sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) +
					       tcp_option_array_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set data length for TCP ACK packet (err=%s)", doca_error_get_name(result));
		goto release_dst;
	}

	dst_eth_hdr = extract_eth_hdr(dst_packet);
	if (dst_eth_hdr == NULL)
		goto release_dst;

	/* Zero out the entire packet */
	memset(dst_eth_hdr,
	       0,
	       sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + tcp_option_array_len);

	dst_ipv4_hdr = (struct ipv4_hdr *)&dst_eth_hdr[1];
	dst_tcp_hdr = (struct tcp_hdr *)&dst_ipv4_hdr[1];
	dst_tcp_opts = (uint8_t *)&dst_tcp_hdr[1];

	memcpy(dst_eth_hdr->s_addr_bytes, src_eth_hdr->d_addr_bytes, sizeof(dst_eth_hdr->s_addr_bytes));
	memcpy(dst_eth_hdr->d_addr_bytes, src_eth_hdr->s_addr_bytes, sizeof(dst_eth_hdr->d_addr_bytes));
	dst_eth_hdr->ether_type = src_eth_hdr->ether_type;

	/* Reminder: double-check remaining ack fields */
	dst_ipv4_hdr->version = 4;
	dst_ipv4_hdr->ihl = 5;
	dst_ipv4_hdr->src_addr = src_ipv4_hdr->dst_addr;
	dst_ipv4_hdr->dst_addr = src_ipv4_hdr->src_addr;
	dst_ipv4_hdr->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + tcp_option_array_len);
	dst_ipv4_hdr->fragment_offset = htons(IPV4_HDR_DF_FLAG);
	dst_ipv4_hdr->time_to_live = 64;
	dst_ipv4_hdr->next_proto_id = IPPROTO_TCP;

	dst_tcp_hdr->src_port = src_tcp_hdr->dst_port;
	dst_tcp_hdr->dst_port = src_tcp_hdr->src_port;
	dst_tcp_hdr->recv_ack = htonl(htonl(src_tcp_hdr->sent_seq) + 1);
	dst_tcp_hdr->sent_seq = src_tcp_hdr->syn ? htonl(1000) : src_tcp_hdr->recv_ack;
	dst_tcp_hdr->rx_win = htons(60000);
	dst_tcp_hdr->dt_off = 5 + tcp_option_array_len / 4;

	if (!src_tcp_hdr->ack) {
		dst_tcp_hdr->syn = src_tcp_hdr->syn;
		dst_tcp_hdr->fin = src_tcp_hdr->fin;
	}
	dst_tcp_hdr->ack = 1;

	if (tcp_option_array_len) {
		uint8_t *mss_opt = dst_tcp_opts;
		uint8_t *sack_ok_opt = dst_tcp_opts + TCP_OPT_MSS_nbytes;
		uint8_t *ts_opt = sack_ok_opt + TCP_OPT_SACK_PERMITTED_nbytes;
		uint8_t *nop_opt = ts_opt + TCP_OPT_TIMESTAMP_nbytes;
		uint8_t *ws_opt = nop_opt + 1;
		time_t seconds = htonl(time(NULL));

		mss_opt[0] = TCP_OPT_MSS;
		mss_opt[1] = TCP_OPT_MSS_nbytes;
		mss_opt[2] = (uint8_t)(mss >> 8);
		mss_opt[3] = (uint8_t)mss;

		sack_ok_opt[0] = TCP_OPT_SACK_PERMITTED;
		sack_ok_opt[1] = TCP_OPT_SACK_PERMITTED_nbytes;

		ts_opt[0] = TCP_OPT_TIMESTAMP;
		ts_opt[1] = TCP_OPT_TIMESTAMP_nbytes;
		memcpy(ts_opt + 2, &seconds, 4);
		// ts_opt+6 (ECR) set below

		nop_opt[0] = TCP_OPT_NOP;

		ws_opt[0] = TCP_OPT_WND_SCALE;
		ws_opt[1] = TCP_OPT_WND_SCALE_nbytes;
		ws_opt[2] = 7; // pick a scale

		const uint8_t *src_tcp_option = (uint8_t *)&src_tcp_hdr[1];
		const uint8_t *src_tcp_options_end = src_tcp_option + 4 * src_tcp_hdr->data_off;
		uint32_t opt_len = 0;

		while (src_tcp_option < src_tcp_options_end) {
			DOCA_LOG_DBG("Processing TCP Option 0x%x", *src_tcp_option);
			switch (*src_tcp_option) {
			case TCP_OPT_END:
				src_tcp_option = src_tcp_options_end; // end loop
				break;
			case TCP_OPT_NOP:
				++src_tcp_option;
				break;
			case TCP_OPT_MSS:
				opt_len = *src_tcp_option;
				src_tcp_option += 4; // don't care
				break;
			case TCP_OPT_WND_SCALE:
				src_tcp_option += 3; // don't care
				break;
			case TCP_OPT_SACK_PERMITTED:
				src_tcp_option += 2; // don't care
				break;
			case TCP_OPT_SACK:
				opt_len = *src_tcp_option; // variable length; don't care
				src_tcp_option += opt_len;
				break;
			case TCP_OPT_TIMESTAMP: {
				const uint8_t *src_tsval = src_tcp_option + 2;
				uint8_t *dst_tsecr = ts_opt + 6;

				memcpy(dst_tsecr, src_tsval, 4);
				src_tcp_option += 10;
				break;
			}
			}
		}
	} /* tcp options */

	return dst_packet;

release_dst:
	doca_buf_dec_refcount(dst_packet, NULL);
	return NULL;
}

static void rxq_success_cb(struct doca_eth_rxq_event_batch_managed_recv *event_batch_managed_recv,
			   uint16_t events_number,
			   union doca_data event_batch_user_data,
			   doca_error_t status,
			   struct doca_buf **pkt_array)
{
	(void)status;
	union doca_data user_data;
	uint32_t num_tx_packets = 0;
	struct doca_task_batch *task_batch;
	union doca_data *task_batch_user_data_array;
	struct doca_buf **task_batch_packets_array;
	struct doca_buf *ack_buf = NULL;
	struct doca_eth_txq *eth_txq;
	struct doca_buf *tx_packets[TCP_PACKET_MAX_BURST_SIZE];
	struct doca_buf_pool *buf_pool;
	union doca_data dummy_user_data;
	doca_error_t result;
	uint16_t queue_id;
	struct rxq_tcp_queues *tcp_queues;
	struct doca_buf *pkt;
	struct tcp_hdr *tcp_hdr;
	struct doca_ctx *ctx = doca_eth_rxq_event_batch_managed_recv_get_ctx(event_batch_managed_recv);

	result = doca_ctx_get_user_data(ctx, &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Can't retrieve queue id from cb");
		DOCA_GPUNETIO_VOLATILE(force_quit) = true;
		return;
	}

	queue_id = (uint16_t)user_data.u64;
	tcp_queues = event_batch_user_data.ptr;
	eth_txq = tcp_queues->eth_txq_cpu_rss[queue_id];
	buf_pool = tcp_queues->txq_cpu_rss_buf_pool[queue_id];
	dummy_user_data.ptr = NULL;

	for (int i = 0; i < events_number; i++) {
		pkt = pkt_array[i];
		tcp_hdr = extract_tcp_hdr(pkt);

		if (!tcp_hdr) {
			DOCA_LOG_WARN("Not a TCP packet");
			continue;
		}

		if (!tcp_hdr->syn && !tcp_hdr->fin && !tcp_hdr->rst) {
			DOCA_LOG_WARN("Unexpected TCP packet flags: 0x%x, expected SYN/RST/FIN", tcp_hdr->tcp_flags);
			continue;
		}

		if (tcp_hdr->rst) {
			log_tcp_flag(pkt, "RST");
			destroy_tcp_session(queue_id, pkt, tcp_queues->port);
			continue; // Do not bother to ack
		} else if (tcp_hdr->fin) {
			log_tcp_flag(pkt, "FIN");
			destroy_tcp_session(queue_id, pkt, tcp_queues->port);
		} else if (tcp_hdr->syn) {
			log_tcp_flag(pkt, "SYN");
			result = create_tcp_session(queue_id, pkt, tcp_queues->port, tcp_queues->rxq_pipe_gpu);
			if (result != DOCA_SUCCESS)
				goto error;
		} else {
			DOCA_LOG_WARN("Unexpected TCP packet flags: 0x%x, expected SYN/RST/FIN", tcp_hdr->tcp_flags);
			continue;
		}

		ack_buf = create_ack_packet(pkt, buf_pool);
		if (ack_buf)
			tx_packets[num_tx_packets++] = ack_buf;
	}

	doca_eth_rxq_event_batch_managed_recv_pkt_array_free(pkt_array);

	if (num_tx_packets > 0) {
		result = doca_eth_txq_task_batch_send_allocate(eth_txq,
							       num_tx_packets,
							       dummy_user_data,
							       &task_batch_packets_array,
							       &task_batch_user_data_array,
							       &task_batch);
		if (result != DOCA_SUCCESS) {
			/* If packet rate is high, then it can happen that ETH TXQ is full and we can't allocate task
			 * batch, don't fail the application in this case */
			DOCA_LOG_WARN("Failed to allocate DOCA tx packets for CPU RSS on queue %u (err=%s)",
				      queue_id,
				      doca_error_get_name(result));
			for (uint32_t i = 0; i < num_tx_packets; i++) {
				doca_buf_dec_refcount(tx_packets[i], NULL);
			}
			return;
		}

		memcpy(task_batch_packets_array, tx_packets, num_tx_packets * sizeof(struct doca_buf *));

		result = doca_task_batch_submit(task_batch);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit DOCA tx task batch for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			doca_task_batch_free(task_batch);
			for (uint32_t i = 0; i < num_tx_packets; i++) {
				doca_buf_dec_refcount(tx_packets[i], NULL);
			}
			goto error;
		}
	}
	return;
error:
	DOCA_GPUNETIO_VOLATILE(force_quit) = true;
}

static void rxq_error_cb(struct doca_eth_rxq_event_batch_managed_recv *event_batch_managed_recv,
			 uint16_t events_number,
			 union doca_data event_batch_user_data,
			 doca_error_t status,
			 struct doca_buf **pkt_array)
{
	(void)event_batch_managed_recv;
	(void)events_number;
	(void)event_batch_user_data;
	(void)status;
	(void)pkt_array;

	DOCA_LOG_ERR("Error in rxq_success_cb");
	DOCA_GPUNETIO_VOLATILE(force_quit) = true;
}

static void txq_success_cb(struct doca_task_batch *task_batch,
			   uint16_t tasks_num,
			   union doca_data ctx_user_data,
			   union doca_data task_batch_user_data,
			   union doca_data *task_user_data_array,
			   struct doca_buf **pkt_array,
			   doca_error_t *status_array)
{
	(void)ctx_user_data;
	(void)task_batch_user_data;
	(void)task_user_data_array;
	(void)status_array;

	for (int i = 0; i < tasks_num; i++) {
		doca_buf_dec_refcount(pkt_array[i], NULL);
	}

	doca_task_batch_free(task_batch);
}

static void txq_error_cb(struct doca_task_batch *task_batch,
			 uint16_t tasks_num,
			 union doca_data ctx_user_data,
			 union doca_data task_batch_user_data,
			 union doca_data *task_user_data_array,
			 struct doca_buf **pkt_array,
			 doca_error_t *status_array)
{
	(void)ctx_user_data;
	(void)task_batch_user_data;
	(void)task_user_data_array;
	(void)status_array;

	DOCA_LOG_ERR("Error in txq_error_cb");

	for (int i = 0; i < tasks_num; i++) {
		doca_buf_dec_refcount(pkt_array[i], NULL);
	}

	doca_task_batch_free(task_batch);
	DOCA_GPUNETIO_VOLATILE(force_quit) = true;
}

doca_error_t create_pe_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id)
{
	return doca_pe_create(&(tcp_queues->cpu_rss_pe[queue_id]));
}

doca_error_t destroy_pe_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id)
{
	if (tcp_queues->cpu_rss_pe[queue_id]) {
		doca_error_t ret = doca_pe_destroy(tcp_queues->cpu_rss_pe[queue_id]);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy PE CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(ret));
			return ret;
		}

		tcp_queues->cpu_rss_pe[queue_id] = NULL;
	}

	return DOCA_SUCCESS;
}

/*
 * Allocate memory and create DOCA mmap with a specific size
 *
 * @dev [in]: DOCA device to register memory on
 * @mmap_size [in]: Mmap size to allocate
 * @mmap [out]: Mmap
 * @mmap_addr [out]: Mmap memory address
 * @return: DOCA error
 */

static doca_error_t create_cpu_mmap(struct doca_dev *dev, size_t mmap_size, struct doca_mmap **mmap, void **mmap_addr)
{
	doca_error_t result;

	*mmap_addr = calloc(mmap_size, 1);
	if (*mmap_addr == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for txq_cpu_rss_mmap_addr");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_create(mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA mmap for CPU RSS (err=%s)", doca_error_get_name(result));
		goto free_txq_cpu_rss_mmap_addr;
	}

	result = doca_mmap_add_dev(*mmap, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add dev to DOCA mmap for CPU RSS (err=%s)", doca_error_get_name(result));
		goto free_txq_cpu_rss_mmap;
	}

	result = doca_mmap_set_permissions(*mmap,
					   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_RELAXED_ORDERING);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set permissions for DOCA mmap for CPU RSS (err=%s)",
			     doca_error_get_name(result));
		goto free_txq_cpu_rss_mmap;
	}

	result = doca_mmap_set_memrange(*mmap, *mmap_addr, mmap_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memrange for DOCA mmap for CPU RSS (err=%s)", doca_error_get_name(result));
		goto free_txq_cpu_rss_mmap;
	}

	result = doca_mmap_start(*mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start DOCA mmap for CPU RSS (err=%s)", doca_error_get_name(result));
		goto free_txq_cpu_rss_mmap;
	}

	return DOCA_SUCCESS;
free_txq_cpu_rss_mmap:
	doca_mmap_destroy(*mmap);
free_txq_cpu_rss_mmap_addr:
	free(*mmap_addr);
	return result;
}

/*
 * Destroy DOCA mmap and free memory
 *
 * @mmap [in]: Mmap to destroy
 * @mmap_addr [in]: Mmap memory address to free
 * @return: DOCA error
 */
static doca_error_t free_cpu_mmap(struct doca_mmap *mmap, void *mmap_addr)
{
	doca_error_t result;

	result = doca_mmap_destroy(mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap for CPU RSS (err=%s)", doca_error_get_name(result));
		return result;
	}

	free(mmap_addr);

	return DOCA_SUCCESS;
}

/*
 * Create DOCA buf pool for a specific mmap
 *
 * @num_bufs [in]: Number of buffers
 * @buf_size [in]: Buffer size
 * @mmap [in]: Mmap to create buf pool on
 * @buf_pool [out]: Buf pool
 * @return: DOCA error
 */
static doca_error_t create_txq_cpu_rss_buf_pool(size_t num_bufs,
						size_t buf_size,
						struct doca_mmap *mmap,
						struct doca_buf_pool **buf_pool)
{
	doca_error_t result;
	result = doca_buf_pool_create(num_bufs, buf_size, mmap, buf_pool);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA buf pool for CPU RSS (err=%s)", doca_error_get_name(result));
		return result;
	}

	result = doca_buf_pool_start(*buf_pool);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA buf pool for CPU RSS (err=%s)", doca_error_get_name(result));
		doca_buf_pool_destroy(*buf_pool);
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy DOCA buf pool
 *
 * @buf_pool [in]: Buf pool to destroy
 * @return: DOCA error
 */
static doca_error_t free_txq_cpu_rss_buf_pool(struct doca_buf_pool *buf_pool)
{
	doca_error_t result;
	result = doca_buf_pool_stop(buf_pool);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA buf pool for CPU RSS (err=%s)", doca_error_get_name(result));
		return result;
	}

	result = doca_buf_pool_destroy(buf_pool);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA buf pool for CPU RSS (err=%s)", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t create_txq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id)
{
	doca_error_t result;
	struct doca_ctx *ctx;

	result = create_cpu_mmap(tcp_queues->ddev,
				 TCP_CPU_RSS_QUEUE_DEPTH * MAX_PKT_SIZE,
				 &(tcp_queues->txq_cpu_rss_mmap[queue_id]),
				 &(tcp_queues->txq_cpu_rss_mmap_addr[queue_id]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA mmap for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		return result;
	}

	result = create_txq_cpu_rss_buf_pool(TCP_CPU_RSS_QUEUE_DEPTH,
					     MAX_PKT_SIZE,
					     tcp_queues->txq_cpu_rss_mmap[queue_id],
					     &(tcp_queues->txq_cpu_rss_buf_pool[queue_id]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA buf pool for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_cpu_mmap;
	}

	result = doca_eth_txq_create(tcp_queues->ddev,
				     TCP_CPU_RSS_QUEUE_DEPTH,
				     &(tcp_queues->eth_txq_cpu_rss[queue_id]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA ETH TXQ for CPU RSS on queue %u", queue_id);
		goto destroy_cpu_pool;
	}

	result = doca_eth_txq_set_type(tcp_queues->eth_txq_cpu_rss[queue_id], DOCA_ETH_TXQ_TYPE_REGULAR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set type, err: %s", doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	result = doca_eth_txq_task_batch_send_set_conf(tcp_queues->eth_txq_cpu_rss[queue_id],
						       DOCA_TASK_BATCH_MAX_TASKS_NUMBER_128,
						       (TCP_CPU_RSS_QUEUE_DEPTH / 128) * 2,
						       txq_success_cb,
						       txq_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure send task batch, err: %s", doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	result = doca_eth_txq_set_l3_chksum_offload(tcp_queues->eth_txq_cpu_rss[queue_id], 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set L3 checksum offload, err: %s", doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	result = doca_eth_txq_set_l4_chksum_offload(tcp_queues->eth_txq_cpu_rss[queue_id], 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set L4 checksum offload, err: %s", doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	ctx = doca_eth_txq_as_doca_ctx(tcp_queues->eth_txq_cpu_rss[queue_id]);
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH TXQ context as DOCA context");
		goto destroy_eth_txq;
	}

	result = doca_pe_connect_ctx(tcp_queues->cpu_rss_pe[queue_id], ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect PE, err: %s", doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context, err: %s", doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	result = doca_eth_txq_apply_queue_id(tcp_queues->eth_txq_cpu_rss[queue_id], QUEUE_ID_TCP_CPU_0 + queue_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply queue ID for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_txq;
	}

	return DOCA_SUCCESS;

destroy_eth_txq:
	doca_eth_txq_destroy(tcp_queues->eth_txq_cpu_rss[queue_id]);
	tcp_queues->eth_txq_cpu_rss[queue_id] = NULL;
destroy_cpu_pool:
	free_txq_cpu_rss_buf_pool(tcp_queues->txq_cpu_rss_buf_pool[queue_id]);
	tcp_queues->txq_cpu_rss_buf_pool[queue_id] = NULL;
destroy_cpu_mmap:
	free_cpu_mmap(tcp_queues->txq_cpu_rss_mmap[queue_id], tcp_queues->txq_cpu_rss_mmap_addr[queue_id]);
	tcp_queues->txq_cpu_rss_mmap[queue_id] = NULL;
	tcp_queues->txq_cpu_rss_mmap_addr[queue_id] = NULL;
	return result;
}

/*
 * Drain DOCA TXQ CPU RSS tasks
 *
 * @ctx [in]: DOCA ethernet TXQ context
 * @pe [in]: DOCA PE associated to the TXQ
 * @return: DOCA error
 */
static doca_error_t drain_txq_cpu_rss_tasks(struct doca_ctx *ctx, struct doca_pe *pe)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_STOPPING;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 10 * 1000,
	};
	int tries = 1000;

	while (state != DOCA_CTX_STATE_IDLE && tries-- > 0) {
		if (doca_pe_progress(pe)) {
			doca_error_t result = doca_ctx_get_state(ctx, &state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get DOCA context state for context %p (err=%s)",
					     ctx,
					     doca_error_get_name(result));
				return result;
			}
			continue;
		}
		nanosleep(&ts, &ts);
	}

	return state != DOCA_CTX_STATE_IDLE ? DOCA_ERROR_TIME_OUT : DOCA_SUCCESS;
}

doca_error_t destroy_txq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id)
{
	doca_error_t result;
	struct doca_ctx *ctx;

	if (tcp_queues->eth_txq_cpu_rss[queue_id] != NULL) {
		ctx = doca_eth_txq_as_doca_ctx(tcp_queues->eth_txq_cpu_rss[queue_id]);
		result = doca_ctx_stop(ctx);
		/* In good flow, context can fail to stop due to tasks left to handle */
		if (result == DOCA_ERROR_IN_PROGRESS) {
			DOCA_LOG_INFO(
				"ETH TXQ context for CPU RSS on queue %u still has tasks to handle. Draining tasks",
				queue_id);
			result = drain_txq_cpu_rss_tasks(ctx, tcp_queues->cpu_rss_pe[queue_id]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to drain tasks for CPU RSS on queue %u (err=%s)",
					     queue_id,
					     doca_error_get_name(result));
				return result;
			}
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA context for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		result = doca_eth_txq_destroy(tcp_queues->eth_txq_cpu_rss[queue_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA ETH TXQ for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		tcp_queues->eth_txq_cpu_rss[queue_id] = NULL;
	}

	if (tcp_queues->txq_cpu_rss_buf_pool[queue_id] != NULL) {
		result = free_txq_cpu_rss_buf_pool(tcp_queues->txq_cpu_rss_buf_pool[queue_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free DOCA buf pool for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		tcp_queues->txq_cpu_rss_buf_pool[queue_id] = NULL;
	}

	if (tcp_queues->txq_cpu_rss_mmap[queue_id] != NULL) {
		result = free_cpu_mmap(tcp_queues->txq_cpu_rss_mmap[queue_id],
				       tcp_queues->txq_cpu_rss_mmap_addr[queue_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free DOCA mmap for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		tcp_queues->txq_cpu_rss_mmap[queue_id] = NULL;
		tcp_queues->txq_cpu_rss_mmap_addr[queue_id] = NULL;
	}

	return DOCA_SUCCESS;
}

doca_error_t create_rxq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id)
{
	doca_error_t result;
	struct doca_ctx *ctx;
	union doca_data user_data;
	uint32_t cyclic_buffer_size;

	result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
						       1000,
						       100,
						       MAX_PKT_SIZE,
						       TCP_CPU_RSS_QUEUE_DEPTH,
						       1,
						       0,
						       0,
						       &cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to estimate packet buffer size for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		return result;
	}

	result = create_cpu_mmap(tcp_queues->ddev,
				 cyclic_buffer_size,
				 &(tcp_queues->rxq_cpu_rss_mmap[queue_id]),
				 &(tcp_queues->rxq_cpu_rss_mmap_addr[queue_id]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA mmap for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		return result;
	}

	result = doca_eth_rxq_create(tcp_queues->ddev,
				     TCP_CPU_RSS_QUEUE_DEPTH,
				     MAX_PKT_SIZE,
				     &(tcp_queues->eth_rxq_cpu_rss[queue_id]));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA ETH RXQ for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_cpu_mmap;
	}

	result = doca_eth_rxq_set_type(tcp_queues->eth_rxq_cpu_rss[queue_id], DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set type for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_rxq;
	}

	result = doca_eth_rxq_set_pkt_buf(tcp_queues->eth_rxq_cpu_rss[queue_id],
					  tcp_queues->rxq_cpu_rss_mmap[queue_id],
					  0,
					  cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set packet buffer for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_rxq;
	}

	user_data.ptr = tcp_queues;
	result = doca_eth_rxq_event_batch_managed_recv_register(tcp_queues->eth_rxq_cpu_rss[queue_id],
								DOCA_EVENT_BATCH_EVENTS_NUMBER_128,
								DOCA_EVENT_BATCH_EVENTS_NUMBER_1,
								user_data,
								rxq_success_cb,
								rxq_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register managed receive event batch for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_rxq;
	}

	ctx = doca_eth_rxq_as_doca_ctx(tcp_queues->eth_rxq_cpu_rss[queue_id]);
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to retrieve DOCA ETH RXQ context as DOCA context for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_rxq;
	}

	result = doca_pe_connect_ctx(tcp_queues->cpu_rss_pe[queue_id], ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect PE for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_rxq;
	}

	user_data.u64 = (uint64_t)queue_id;
	result = doca_ctx_set_user_data(ctx, user_data);

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context for CPU RSS on queue %u (err=%s)",
			     queue_id,
			     doca_error_get_name(result));
		goto destroy_eth_rxq;
	}

	return DOCA_SUCCESS;

destroy_eth_rxq:
	doca_eth_rxq_destroy(tcp_queues->eth_rxq_cpu_rss[queue_id]);
	tcp_queues->eth_rxq_cpu_rss[queue_id] = NULL;
destroy_cpu_mmap:
	free_cpu_mmap(tcp_queues->rxq_cpu_rss_mmap[queue_id], tcp_queues->rxq_cpu_rss_mmap_addr[queue_id]);
	tcp_queues->rxq_cpu_rss_mmap[queue_id] = NULL;
	tcp_queues->rxq_cpu_rss_mmap_addr[queue_id] = NULL;
	return result;
}

doca_error_t destroy_rxq_cpu_rss(struct rxq_tcp_queues *tcp_queues, uint16_t queue_id)
{
	doca_error_t result;
	struct doca_ctx *ctx;

	if (tcp_queues->eth_rxq_cpu_rss[queue_id] != NULL) {
		ctx = doca_eth_rxq_as_doca_ctx(tcp_queues->eth_rxq_cpu_rss[queue_id]);
		result = doca_ctx_stop(ctx);
		/* In good flow, context always succeeds to be stopped immediately (no tasks to handle) */
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA context for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		result = doca_eth_rxq_destroy(tcp_queues->eth_rxq_cpu_rss[queue_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA ETH TXQ for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		tcp_queues->eth_rxq_cpu_rss[queue_id] = NULL;
	}

	if (tcp_queues->rxq_cpu_rss_mmap[queue_id] != NULL) {
		result = free_cpu_mmap(tcp_queues->rxq_cpu_rss_mmap[queue_id],
				       tcp_queues->rxq_cpu_rss_mmap_addr[queue_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free DOCA mmap for CPU RSS on queue %u (err=%s)",
				     queue_id,
				     doca_error_get_name(result));
			return result;
		}

		tcp_queues->rxq_cpu_rss_mmap[queue_id] = NULL;
		tcp_queues->rxq_cpu_rss_mmap_addr[queue_id] = NULL;
	}

	return DOCA_SUCCESS;
}

void *tcp_cpu_rss_func(void *targs)
{
	struct thread_args *targs_ = targs;
	struct rxq_tcp_queues *tcp_queues = targs_->args;
	uint16_t queue_id = targs_->queue_id;
	pthread_t self_id = pthread_self();

	if (tcp_queues == NULL) {
		DOCA_LOG_ERR("%s: 'tcp_queues argument cannot be NULL", __func__);
		goto error;
	}
	if (tcp_queues->port == NULL) {
		DOCA_LOG_ERR("%s: 'tcp_queues->port argument cannot be NULL", __func__);
		goto error;
	}
	if (tcp_queues->rxq_pipe_gpu == NULL) {
		DOCA_LOG_ERR("%s: 'tcp_queues->rxq_pipe_gpu argument cannot be NULL", __func__);
		goto error;
	}

	if (tcp_queues->eth_rxq_cpu_rss[queue_id] == NULL) {
		DOCA_LOG_ERR("%s: 'tcp_queues->eth_rxq_cpu_rss[%u] argument cannot be NULL", __func__, queue_id);
		goto error;
	}
	if (tcp_queues->eth_txq_cpu_rss[queue_id] == NULL) {
		DOCA_LOG_ERR("%s: 'tcp_queues->eth_txq_cpu_rss[%u] argument cannot be NULL", __func__, queue_id);
		goto error;
	}
	if (tcp_queues->cpu_rss_pe[queue_id] == NULL) {
		DOCA_LOG_ERR("%s: 'tcp_queues->cpu_rss_pe[%u] argument cannot be NULL", __func__, queue_id);
		goto error;
	}

	DOCA_LOG_INFO("Thread %lu is performing TCP SYN/FIN processing on queue %u", (unsigned long)self_id, queue_id);

	/* read global force_quit */
	while (DOCA_GPUNETIO_VOLATILE(force_quit) == false) {
		(void)doca_pe_progress(tcp_queues->cpu_rss_pe[queue_id]);
	}

	return NULL;

error:
	DOCA_GPUNETIO_VOLATILE(force_quit) = true;
	return NULL;
}
