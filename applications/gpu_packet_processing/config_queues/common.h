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

#ifndef DOCA_GPU_PACKET_PROCESSING_H
#define DOCA_GPU_PACKET_PROCESSING_H

#include "defines.h"
#include <doca_eth_txq_gpu_data_path.h>

extern bool force_quit;

/* Application configuration structure */
struct app_gpu_cfg {
	char gpu_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* GPU PCIe address */
	char nic_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Network card PCIe address */
	uint8_t queue_num;				/* Number of GPU receive queues */
	bool http_server;				/* Enable GPU HTTP server */
};

/* Application TCP receive queues objects */
struct rxq_tcp_queues {
	struct doca_gpu *gpu_dev; /* GPUNetio handler associated to queues */
	struct doca_dev *ddev;	  /* DOCA device handler associated to queues */

	uint16_t numq;					  /* Number of queues processed in the GPU */
	uint16_t numq_cpu_rss;				  /* Number of queues processed in the CPU */
	struct doca_eth_rxq *eth_rxq_cpu_rss[MAX_QUEUES]; /* DOCA Ethernet receive queue CPU RSS handler */
	struct doca_mmap *rxq_cpu_rss_mmap[MAX_QUEUES];	  /* DOCA mmap to receive packet with DOCA Ethernet CPU queue */
	void *rxq_cpu_rss_mmap_addr[MAX_QUEUES];	  /* DOCA mmap CPU memory address */
	struct doca_eth_txq *eth_txq_cpu_rss[MAX_QUEUES]; /* DOCA Ethernet transmit queue CPU RSS handler */
	struct doca_mmap *txq_cpu_rss_mmap[MAX_QUEUES];	  /* DOCA mmap to send packet with DOCA Ethernet CPU queue */
	void *txq_cpu_rss_mmap_addr[MAX_QUEUES];	  /* DOCA mmap CPU memory address */
	struct doca_buf_pool *txq_cpu_rss_buf_pool[MAX_QUEUES]; /* DOCA buf pool to send packet with DOCA Ethernet CPU
								   queue */
	struct doca_pe *cpu_rss_pe[MAX_QUEUES];			/* DOCA PE associated to queues */
	struct doca_ctx *eth_rxq_ctx[MAX_QUEUES];		/* DOCA Ethernet receive queue context */
	struct doca_eth_rxq *eth_rxq_cpu[MAX_QUEUES];		/* DOCA Ethernet receive queue CPU handler */
	struct doca_gpu_eth_rxq *eth_rxq_gpu[MAX_QUEUES];	/* DOCA Ethernet receive queue GPU handler */
	struct doca_mmap *pkt_buff_mmap[MAX_QUEUES]; /* DOCA mmap to receive packet with DOCA Ethernet queue */
	void *gpu_pkt_addr[MAX_QUEUES];		     /* DOCA mmap GPU memory address */
	int dmabuf_fd[MAX_QUEUES];		     /* GPU memory dmabuf file descriptor */

	struct doca_flow_port *port;		    /* DOCA Flow port */
	struct doca_flow_pipe *rxq_pipe_gpu;	    /* DOCA Flow pipe for GPU queues */
	struct doca_flow_pipe *rxq_pipe_cpu;	    /* DOCA Flow pipe for CPU queues */
	struct doca_flow_pipe_entry *cpu_rss_entry; /* DOCA Flow RSS entry for CPU queues */

	uint16_t nums;					     /* Number of semaphores items */
	struct doca_gpu_semaphore *sem_cpu[MAX_QUEUES];	     /* One semaphore per queue to report stats, CPU handler*/
	struct doca_gpu_semaphore_gpu *sem_gpu[MAX_QUEUES];  /* One semaphore per queue to report stats, GPU handler*/
	struct doca_gpu_semaphore *sem_http_cpu[MAX_QUEUES]; /* One semaphore per queue to report HTTP info, CPU
								handler*/
	struct doca_gpu_semaphore_gpu *sem_http_gpu[MAX_QUEUES]; /* One semaphore per queue to report HTTP info, GPU
								    handler*/
};

/* Application UDP receive queues objects */
struct rxq_udp_queues {
	struct doca_gpu *gpu_dev; /* GPUNetio handler associated to queues*/
	struct doca_dev *ddev;	  /* DOCA device handler associated to queues */

	uint16_t numq;					  /* Number of queues */
	struct doca_ctx *eth_rxq_ctx[MAX_QUEUES];	  /* DOCA Ethernet receive queue context */
	struct doca_eth_rxq *eth_rxq_cpu[MAX_QUEUES];	  /* DOCA Ethernet receive queue CPU handler */
	struct doca_gpu_eth_rxq *eth_rxq_gpu[MAX_QUEUES]; /* DOCA Ethernet receive queue GPU handler */
	struct doca_mmap *pkt_buff_mmap[MAX_QUEUES];	  /* DOCA mmap to receive packet with DOCA Ethernet queue */
	void *gpu_pkt_addr[MAX_QUEUES];			  /* DOCA mmap GPU memory address */
	int dmabuf_fd[MAX_QUEUES];			  /* GPU memory dmabuf file descriptor */

	struct doca_flow_port *port;			    /* DOCA Flow port */
	struct doca_flow_pipe *rxq_pipe;		    /* DOCA Flow receive pipe */
	struct doca_flow_pipe *root_pipe;		    /* DOCA Flow root pipe */
	struct doca_flow_pipe_entry *root_udp_entry;	    /* DOCA Flow root entry */
	struct doca_flow_pipe_entry *root_tcp_entry_gpu;    /* DOCA Flow root entry */
	struct doca_flow_pipe_entry *root_tcp_entry_cpu[3]; /* DOCA Flow root entry */
	struct doca_flow_pipe_entry *root_icmp_entry_gpu;   /* DOCA Flow root entry */

	uint16_t nums;					    /* Number of semaphores items */
	struct doca_gpu_semaphore *sem_cpu[MAX_QUEUES];	    /* One semaphore per queue, CPU handler*/
	struct doca_gpu_semaphore_gpu *sem_gpu[MAX_QUEUES]; /* One semaphore per queue, GPU handler*/
};

/* Application ICMP receive queues objects */
struct rxq_icmp_queues {
	struct doca_gpu *gpu_dev; /* GPUNetio handler associated to queues*/
	struct doca_dev *ddev;	  /* DOCA device handler associated to queues */

	uint16_t numq;					  /* Number of queues */
	struct doca_ctx *eth_rxq_ctx[MAX_QUEUES];	  /* DOCA Ethernet receive queue context */
	struct doca_eth_rxq *eth_rxq_cpu[MAX_QUEUES];	  /* DOCA Ethernet receive queue CPU handler */
	struct doca_gpu_eth_rxq *eth_rxq_gpu[MAX_QUEUES]; /* DOCA Ethernet receive queue GPU handler */
	struct doca_mmap *pkt_buff_mmap[MAX_QUEUES];	  /* DOCA mmap to receive packet with DOCA Ethernet queue */
	void *gpu_pkt_addr[MAX_QUEUES];			  /* DOCA mmap GPU memory address */
	int dmabuf_fd[MAX_QUEUES];			  /* GPU memory dmabuf file descriptor */

	struct doca_flow_port *port;	 /* DOCA Flow port */
	struct doca_flow_pipe *rxq_pipe; /* DOCA Flow receive pipe */

	struct doca_ctx *eth_txq_ctx[MAX_QUEUES];	  /* DOCA Ethernet send queue context */
	struct doca_eth_txq *eth_txq_cpu[MAX_QUEUES];	  /* DOCA Ethernet send queue CPU handler */
	struct doca_gpu_eth_txq *eth_txq_gpu[MAX_QUEUES]; /* DOCA Ethernet send queue GPU handler */
};

/* Tx buffer, used to send HTTP responses */
struct tx_buf {
	struct doca_gpu *gpu_dev;	      /* GPU device */
	struct doca_dev *ddev;		      /* Network DOCA device */
	uint32_t num_packets;		      /* Number of packets in the buffer */
	uint32_t max_pkt_sz;		      /* Max size of each packet in the buffer */
	uint32_t pkt_nbytes;		      /* Effective bytes in each packet */
	uint8_t *gpu_pkt_addr;		      /* GPU memory address of the buffer */
	struct doca_mmap *mmap;		      /* DOCA mmap around GPU memory buffer for the DOCA device */
	struct doca_buf_arr *buf_arr;	      /* DOCA buffer array object around GPU memory buffer */
	struct doca_gpu_buf_arr *buf_arr_gpu; /* DOCA buffer array GPU handle */
	int dmabuf_fd;			      /* GPU memory dmabuf file descriptor */
};

/* Application GPU HTTP server send queues objects */
struct txq_http_queues {
	struct doca_gpu *gpu_dev;			  /* GPUNetio handler associated to queues*/
	struct doca_dev *ddev;				  /* DOCA device handler associated to queues */
	struct doca_ctx *eth_txq_ctx[MAX_QUEUES];	  /* DOCA Ethernet send queue context */
	struct doca_eth_txq *eth_txq_cpu[MAX_QUEUES];	  /* DOCA Ethernet send queue CPU handler */
	struct doca_gpu_eth_txq *eth_txq_gpu[MAX_QUEUES]; /* DOCA Ethernet send queue GPU handler */

	struct tx_buf buf_page_index;	  /* GPU memory buffer for HTTP index page */
	struct tx_buf buf_page_contacts;  /* GPU memory buffer for HTTP contacts page */
	struct tx_buf buf_page_not_found; /* GPU memory buffer for HTTP not found page */
};

/* TCP statistics reported by GPU filters */
struct stats_tcp {
	uint32_t http;	    /* Generic HTTP packet */
	uint32_t http_head; /* HTTP HEAD packet */
	uint32_t http_get;  /* HTTP GET packet */
	uint32_t http_post; /* HTTP POST packet */
	uint32_t tcp_syn;   /* TCP with SYN flag */
	uint32_t tcp_fin;   /* TCP with FIN flag */
	uint32_t tcp_ack;   /* TCP with ACK flag */
	uint32_t others;    /* Other TCP packets */
	uint32_t total;	    /* Total TCP packets */
};

/* UDP statistics reported by GPU filters */
struct stats_udp {
	uint64_t dns;	 /* DNS packet */
	uint64_t others; /* Other UDP packets */
	uint64_t total;	 /* Total UDP packets */
};

/* HTTP GET packet info used to reply with HTTP response */
struct info_http {
	uint8_t eth_src_addr_bytes[ETHER_ADDR_LEN]; /* Source addr bytes in tx order */
	uint8_t eth_dst_addr_bytes[ETHER_ADDR_LEN]; /* Destination addr bytes in tx order */
	uint32_t ip_src_addr;			    /* IP source address */
	uint32_t ip_dst_addr;			    /* IP destination address */
	uint16_t ip_total_length;		    /* IP destination address */
	uint16_t tcp_src_port;			    /* TCP source port */
	uint16_t tcp_dst_port;			    /* TCP destination port */
	uint8_t tcp_dt_off;			    /* Data offset */
	uint32_t tcp_sent_seq;			    /* TCP TX data sequence number */
	uint32_t tcp_recv_ack;			    /* TCP RX data acknowledgment sequence number */
	enum http_page_get page;		    /* HTTP page requested */
};

/* Defined in tcp_session_table.h */
struct tcp_session_entry;

struct thread_args {
	uint16_t queue_id;
	void *args;
};

/*
 * Register application command line parameters.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_application_params(void);

/*
 * Initialize a DOCA network device.
 *
 * @nic_pcie_addr [in]: Network card PCIe address
 * @ddev [out]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_doca_device(char *nic_pcie_addr, struct doca_dev **ddev);

/*
 * Initialize DOCA Flow.
 *
 * @dev [in]: DOCA device
 * @port_id [in]: DOCA device DPDK port id
 * @rxq_num [in]: Receive queue number
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
struct doca_flow_port *init_doca_flow(struct doca_dev *dev, uint16_t port_id, uint8_t rxq_num);

/*
 * Create DOCA Flow pipeline for UDP packets.
 *
 * @udp_queues [in]: Application UDP queues
 * @port [in]: DOCA Flow port associated to the DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_udp_pipe(struct rxq_udp_queues *udp_queues, struct doca_flow_port *port);

/*
 * Create DOCA Flow pipeline for TCP control packets on CPU.
 *
 * @tcp_queues [in]: Application TCP queues
 * @port [in]: DOCA Flow port associated to the DOCA device
 * @connection_based_flows [in]: TCP connection mode is enabled or not
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_tcp_cpu_pipe(struct rxq_tcp_queues *tcp_queues, struct doca_flow_port *port);

/*
 * Create DOCA Flow pipeline for TCP packets on GPU.
 *
 * @tcp_queues [in]: Application TCP queues
 * @port [in]: DOCA Flow port associated to the DOCA device
 * @connection_based_flows [in]: TCP connection mode is enabled or not
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_tcp_gpu_pipe(struct rxq_tcp_queues *tcp_queues,
				 struct doca_flow_port *port,
				 bool connection_based_flows);

/*
 * Create DOCA Flow pipeline for ICMP packets on GPU.
 *
 * @icmp_queues [in]: Application ICMP queues
 * @port [in]: DOCA Flow port associated to the DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_icmp_gpu_pipe(struct rxq_icmp_queues *icmp_queues, struct doca_flow_port *port);

/*
 * Create DOCA Flow root pipeline to distinguish UDP and TCP.
 *
 * @udp_queues [in]: Application UDP queues
 * @tcp_queues [in]: Application TCP queues
 * @icmp_queues [in]: Application ICMP queues
 * @port [in]: DOCA Flow port associated to the DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_root_pipe(struct rxq_udp_queues *udp_queues,
			      struct rxq_tcp_queues *tcp_queues,
			      struct rxq_icmp_queues *icmp_queues,
			      struct doca_flow_port *port);

/*
 * Destroy DOCA Flow.
 *
 * @port_df [in]: DOCA flow port handler
 * @icmp_queues [in]: Application ICMP queues
 * @udp_queues [in]: Application UDP queues
 * @tcp_queues [in]: Application TCP queues
 * @http_server [in]: HTTP server enabled or not
 * @http_queues [in]: Application HTTP queues
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_flow_queue(struct doca_flow_port *port_df,
				struct rxq_icmp_queues *icmp_queues,
				struct rxq_udp_queues *udp_queues,
				struct rxq_tcp_queues *tcp_queues,
				bool http_server,
				struct txq_http_queues *http_queues);

/*
 * Enable TCP data traffic to GPU once TCP connection is established.
 *
 * @port [in]: DOCA flow port
 * @queue_id [in]: GPU queue id
 * @gpu_rss_pipe [in]: GPU DOCA Flow RSS pipe
 * @session_entry [in]: TCP session
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t enable_tcp_gpu_offload(struct doca_flow_port *port,
				    uint16_t queue_id,
				    struct doca_flow_pipe *gpu_rss_pipe,
				    struct tcp_session_entry *session_entry);

/*
 * Disable TCP data traffic to GPU once TCP connection is closed.
 *
 * @port [in]: DOCA flow port
 * @queue_id [in]: GPU queue id
 * @session_entry [in]: TCP session
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t disable_tcp_gpu_offload(struct doca_flow_port *port,
				     uint16_t queue_id,
				     struct tcp_session_entry *session_entry);

/*
 * Create TCP and HTTP server queues
 *
 * @tcp_queues [in]: TCP queues handler
 * @df_port [in]: DOCA flow port
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA network device
 * @queue_num [in]: Number of queues to create
 * @sem_num [in]: Number of semaphores to create
 * @http_server [in]: Enable HTTP server
 * @http_queues [in]: HTTP TXQ queues handler
 * @pe [in]: DOCA PE to associated to HTTP send queue
 * @event_error_send_packet_cb [in]: DOCA PE callback in case of send packet error event
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_tcp_queues(struct rxq_tcp_queues *tcp_queues,
			       struct doca_flow_port *df_port,
			       struct doca_gpu *gpu_dev,
			       struct doca_dev *ddev,
			       uint32_t queue_num,
			       uint32_t sem_num,
			       bool http_server,
			       struct txq_http_queues *http_queues,
			       struct doca_pe *pe,
			       doca_eth_txq_gpu_event_error_send_packet_cb_t event_error_send_packet_cb);

/*
 * Destroy TCP and HTTP server queues
 *
 * @tcp_queues [in]: TCP queues handler
 * @http_server [in]: Enable HTTP server
 * @http_queues [in]: HTTP TXQ queues handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_tcp_queues(struct rxq_tcp_queues *tcp_queues,
				bool http_server,
				struct txq_http_queues *http_queues);

/*
 * Create UDP queues
 *
 * @udp_queues [in]: UDP queues handler
 * @df_port [in]: DOCA flow port
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA network device
 * @queue_num [in]: Number of queues to create
 * @sem_num [in]: Number of semaphores to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_udp_queues(struct rxq_udp_queues *udp_queues,
			       struct doca_flow_port *df_port,
			       struct doca_gpu *gpu_dev,
			       struct doca_dev *ddev,
			       uint32_t queue_num,
			       uint32_t sem_num);

/*
 * Destroy UDP queues
 *
 * @udp_queues [in]: UDP queues handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_udp_queues(struct rxq_udp_queues *udp_queues);

/*
 * Create ICMP queues
 *
 * @icmp_queues [in]: UDP queues handler
 * @df_port [in]: DOCA flow port
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA network device
 * @queue_num [in]: Number of queues to create
 * @pe [in]: DOCA PE to associated to ICMP send queue
 * @event_error_send_packet_cb [in]: DOCA PE callback in case of send packet error event
 * @event_notify_send_packet_cb [in]: DOCA PE callback in case of send packet debug event
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_icmp_queues(struct rxq_icmp_queues *icmp_queues,
				struct doca_flow_port *df_port,
				struct doca_gpu *gpu_dev,
				struct doca_dev *ddev,
				uint32_t queue_num,
				struct doca_pe *pe,
				doca_eth_txq_gpu_event_error_send_packet_cb_t event_error_send_packet_cb,
				doca_eth_txq_gpu_event_notify_send_packet_cb_t event_notify_send_packet_cb);

/*
 * Destroy ICMP queues
 *
 * @icmp_queues [in]: ICMP queues handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_icmp_queues(struct rxq_icmp_queues *icmp_queues);

/*
 * Create TX buf for HTTP response
 *
 * @buf [in]: TX buf to create
 * @gpu_dev [in]: DOCA GPUNetIO handler
 * @ddev [in]: DOCA device network card handler
 * @num_packets [in]: Number of packets in the doca_buf_arr of the txbuf
 * @max_pkt_sz [in]: Max packet size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_tx_buf(struct tx_buf *buf,
			   struct doca_gpu *gpu_dev,
			   struct doca_dev *ddev,
			   uint32_t num_packets,
			   uint32_t max_pkt_sz);

/*
 * Destroy TX buf
 *
 * @buf [in]: TX buf to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_tx_buf(struct tx_buf *buf);

/*
 * Pre-prepare TX buf filling default values in GPU memory
 *
 * @buf [in]: TX buf to create
 * @page_type [in]: type of page payload to write in every buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t prepare_tx_buf(struct tx_buf *buf, enum http_page_get page_type);

/*
 * DOCA PE callback to be invoked if any Eth Txq get an error
 * sending packets.
 *
 * @event_error [in]: DOCA PE event error handler
 * @event_user_data [in]: custom user data set at registration time
 */
void error_send_packet_cb(struct doca_eth_txq_gpu_event_error_send_packet *event_error,
			  union doca_data event_user_data);

/*
 * DOCA PE callback to be invoked on ICMP Eth Txq to get the debug info
 * when sending packets
 *
 * @event_notify [in]: DOCA PE event debug handler
 * @event_user_data [in]: custom user data set at registration time
 */
void debug_send_packet_icmp_cb(struct doca_eth_txq_gpu_event_notify_send_packet *event_notify,
			       union doca_data event_user_data);

#if __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel to specifically receive TCP packets.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @exit_cond [in]: exit condition set by the CPU to notify the kernel it has to quit
 * @tcp_queues [in]: list of ethernet + flow queues to use to receive TCP traffic
 * @http_server [in]: GPU HTTP server mode enabled
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_receive_tcp(cudaStream_t stream,
				uint32_t *exit_cond,
				struct rxq_tcp_queues *tcp_queues,
				bool http_server);

/*
 * Launch a CUDA kernel to specifically receive UDP packets.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @exit_cond [in]: exit condition set by the CPU to notify the kernel it has to quit
 * @udp_queues [in]: list of ethernet + flow queues to use to receive TCP traffic
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_receive_udp(cudaStream_t stream, uint32_t *exit_cond, struct rxq_udp_queues *udp_queues);

/*
 * Launch a CUDA kernel to specifically receive ICMP packets.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @exit_cond [in]: exit condition set by the CPU to notify the kernel it has to quit
 * @icmp_queues [in]: list of ethernet + flow queues to use to receive ICMP traffic
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_receive_icmp(cudaStream_t stream, uint32_t *exit_cond, struct rxq_icmp_queues *icmp_queues);

/*
 * Launch a CUDA kernel to act as HTTP server
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @exit_cond [in]: exit condition set by the CPU to notify the kernel it has to quit
 * @tcp_queues [in]: TCP queues with HTTP info semaphore
 * @http_queues [in]: HTTP TXQ queues
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_http_server(cudaStream_t stream,
				uint32_t *exit_cond,
				struct rxq_tcp_queues *tcp_queues,
				struct txq_http_queues *http_queues);

#if __cplusplus
}
#endif

#endif
