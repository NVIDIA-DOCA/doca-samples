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

#ifndef DOCA_GPUNETIO_PACKETS_H
#define DOCA_GPUNETIO_PACKETS_H

#include "common.h"

#define TCP_PROTOCOL_ID 0x6
#define UDP_PROTOCOL_ID 0x11

#define DNS_POST 0x35
#define WHITESPACE_ASCII 0x20

/* ICMP packet types */
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8

enum tcp_flags {
	TCP_FLAG_FIN = (1 << 0),
	/* set tcp packet with Fin flag */
	TCP_FLAG_SYN = (1 << 1),
	/* set tcp packet with Syn flag */
	TCP_FLAG_RST = (1 << 2),
	/* set tcp packet with Rst flag */
	TCP_FLAG_PSH = (1 << 3),
	/* set tcp packet with Psh flag */
	TCP_FLAG_ACK = (1 << 4),
	/* set tcp packet with Ack flag */
	TCP_FLAG_URG = (1 << 5),
	/* set tcp packet with Urg flag */
	TCP_FLAG_ECE = (1 << 6),
	/* set tcp packet with ECE flag */
	TCP_FLAG_CWR = (1 << 7),
	/* set tcp packet with CQE flag */
};

struct ether_hdr {
	uint8_t d_addr_bytes[ETHER_ADDR_LEN]; /* Destination addr bytes in tx order */
	uint8_t s_addr_bytes[ETHER_ADDR_LEN]; /* Source addr bytes in tx order */
	uint16_t ether_type;		      /* Frame type */
} __attribute__((__packed__));

struct ipv4_hdr {
	union {
		uint8_t version_ihl; /**< version and header length */
		struct {
			uint8_t ihl : 4;     /**< header length */
			uint8_t version : 4; /**< version */
		};
	};
	uint8_t type_of_service;  /* type of service */
	uint16_t total_length;	  /* length of packet */
	uint16_t packet_id;	  /* packet ID */
	uint16_t fragment_offset; /* fragmentation offset */
	uint8_t time_to_live;	  /* time to live */
	uint8_t next_proto_id;	  /* protocol ID */
	uint16_t hdr_checksum;	  /* header checksum */
	uint32_t src_addr;	  /* source address */
	uint32_t dst_addr;	  /* destination address */
} __attribute__((__packed__));

struct tcp_hdr {
	uint16_t src_port; /* TCP source port */
	uint16_t dst_port; /* TCP destination port */
	uint32_t sent_seq; /* TX data sequence number */
	uint32_t recv_ack; /* RX data acknowledgment sequence number */
	union {
		uint8_t data_off;
		struct {
			uint8_t rsrv : 4;
			uint8_t dt_off : 4; /* Data offset */
		};
	};
	union {
		uint8_t tcp_flags; /* TCP flags */
		struct {
			uint8_t fin : 1;
			uint8_t syn : 1;
			uint8_t rst : 1;
			uint8_t psh : 1;
			uint8_t ack : 1;
			uint8_t urg : 1;
			uint8_t ecne : 1;
			uint8_t cwr : 1;
		};
	};
	uint16_t rx_win;  /* RX flow control window */
	uint16_t cksum;	  /* TCP checksum */
	uint16_t tcp_urp; /* TCP urgent pointer, if any */
} __attribute__((__packed__));

struct eth_ip_tcp_hdr {
	struct ether_hdr l2_hdr; /* Ethernet header */
	struct ipv4_hdr l3_hdr;	 /* IP header */
	struct tcp_hdr l4_hdr;	 /* TCP header */
} __attribute__((__packed__));

struct udp_hdr {
	uint16_t src_port;    /* UDP source port */
	uint16_t dst_port;    /* UDP destination port */
	uint16_t dgram_len;   /* UDP datagram length */
	uint16_t dgram_cksum; /* UDP datagram checksum */
} __attribute__((__packed__));

struct eth_ip_udp_hdr {
	struct ether_hdr l2_hdr; /* Ethernet header */
	struct ipv4_hdr l3_hdr;	 /* IP header */
	struct udp_hdr l4_hdr;	 /* UDP header */
} __attribute__((__packed__));

/**
 * ICMP
 */
struct icmp_hdr {
	uint8_t type;	 /* ICMP packet type */
	uint8_t code;	 /* ICMP packet code */
	uint16_t cksum;	 /* ICMP packet checksum */
	uint16_t ident;	 /* ICMP packet identifier */
	uint16_t seq_nb; /* ICMP packet sequence number */
} __attribute__((__packed__));

struct eth_ip_icmp_hdr {
	struct ether_hdr l2_hdr; /* Ethernet header */
	struct ipv4_hdr l3_hdr;	 /* IP header */
	struct icmp_hdr l4_hdr;	 /* ICMP header */
} __attribute__((__packed__));

#endif /* DOCA_GPUNETIO_PACKETS_H */
