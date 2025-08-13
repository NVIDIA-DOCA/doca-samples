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

#ifndef IPSEC_CTX_H_
#define IPSEC_CTX_H_

#include <doca_dev.h>
#include <doca_flow.h>

#include <dpdk_utils.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SOCKET_PATH_NAME (108)	    /* Maximum socket file name length */
#define MAX_FILE_NAME (255)		    /* Maximum file name length */
#define MAX_NB_RULES (1048576)		    /* Maximal number of rules == 2^20 */
#define DYN_RESERVED_RULES (1024)	    /* Reserved rules for dynamic rules */
#define MAX_KEY_LEN (32)		    /* Maximal GCM key size is 256bit==32B */
#define NUM_OF_SYNDROMES (4)		    /* Number of bad syndromes */
#define SW_WINDOW_SIZE 64		    /* The size of the replay window when anti replay is done by SW */
#define HW_WINDOW_SIZE 128		    /* The size of the replay window when anti replay is done by HW*/
#define MAX_NAME_LEN (20)		    /* Max pipe and entry name length */
#define MAX_ACTIONS_MEM_SIZE (8388608 * 64) /* 2^23 * size of max_entry */

/* SA attrs struct */
struct ipsec_security_gw_sa_attrs {
	enum doca_flow_crypto_key_type key_type; /* Key type */
	uint8_t enc_key_data[MAX_KEY_LEN];	 /* Policy encryption key */
	uint64_t iv;				 /* Policy IV */
	uint32_t salt;				 /* Key Salt */
	uint32_t lifetime_threshold;		 /* SA lifetime threshold */
	bool esn_en;				 /* If extended sn is enable*/
};

/* will hold an entry of a bad syndrome and its last counter */
struct bad_syndrome_entry {
	struct doca_flow_pipe_entry *entry; /* DOCA Flow entry */
	uint32_t previous_stats;	    /* last query stats */
};

/* struct to hold anti-replay state */
struct antireplay_state {
	uint32_t window_size; /* anti-replay window size */
	uint32_t end_win_sn;  /* end of window sequence number */
	uint64_t bitmap;      /* anti-replay bitmap - LSB is with lowest sequence number */
};

/* entry information struct */
struct security_gateway_entry_info {
	char name[MAX_NAME_LEN + 1];	    /* entry name */
	struct doca_flow_pipe_entry *entry; /* entry pointer */
	uint32_t prev_stats;		    /* prev stats */
};

/* decryption rule struct */
struct decrypt_rule {
	enum doca_flow_l3_type l3_type; /* IP type */
	union {
		doca_be32_t dst_ip4;	/* destination IPv4 */
		doca_be32_t dst_ip6[4]; /* destination IPv6 */
	};
	doca_be32_t esp_spi;				     /* ipsec session parameter index */
	enum doca_flow_l3_type inner_l3_type;		     /* inner IP type */
	struct ipsec_security_gw_sa_attrs sa_attrs;	     /* input SA attributes */
	struct bad_syndrome_entry entries[NUM_OF_SYNDROMES]; /* array of bad syndrome entries */
	struct antireplay_state antireplay_state;	     /* Antireplay state */
};

/* IPv4 addresses struct */
struct ipsec_security_gw_ip4 {
	doca_be32_t src_ip; /* source IPv4 */
	doca_be32_t dst_ip; /* destination IPv4 */
};

/* IPv6 addresses struct */
struct ipsec_security_gw_ip6 {
	doca_be32_t src_ip[4]; /* source IPv6 */
	doca_be32_t dst_ip[4]; /* destination IPv6 */
};

/* encryption rule struct */
struct encrypt_rule {
	enum doca_flow_l3_type l3_type;	     /* l3 type */
	enum doca_flow_l4_type_ext protocol; /* protocol */
	union {
		struct ipsec_security_gw_ip4 ip4; /* IPv4 addresses */
		struct ipsec_security_gw_ip6 ip6; /* IPv6 addresses */
	};
	int src_port;			      /* source port */
	int dst_port;			      /* destination port */
	enum doca_flow_l3_type encap_l3_type; /* encap l3 type */
	union {
		doca_be32_t encap_dst_ip4;    /* encap destination IPv4 */
		doca_be32_t encap_dst_ip6[4]; /* encap destination IPv6 */
	};
	doca_be32_t esp_spi;			    /* ipsec session parameter index */
	uint32_t current_sn;			    /* current sequence number */
	struct ipsec_security_gw_sa_attrs sa_attrs; /* input SA attributes */
};

/* pipe information struct */
struct security_gateway_pipe_info {
	char name[MAX_NAME_LEN + 1];			  /* pipe name */
	struct doca_flow_pipe *pipe;			  /* pipe pointer */
	uint32_t nb_entries;				  /* number of entries in pipe */
	struct security_gateway_entry_info *entries_info; /* entries info array */
};

/* all the pipes that is used for encrypt packets */
struct encrypt_pipes {
	struct security_gateway_pipe_info encrypt_root;		/* encrypt control pipe */
	struct security_gateway_pipe_info egress_ip_classifier; /* egress IP classifier */
	struct security_gateway_pipe_info ipv4_encrypt_pipe;	/* encryption action pipe for ipv4 traffic */
	struct security_gateway_pipe_info ipv6_encrypt_pipe;	/* encryption action pipe for ipv6 traffic */
	struct security_gateway_pipe_info ipv4_tcp_pipe;	/* 5-tuple ipv4 tcp match pipe */
	struct security_gateway_pipe_info ipv4_udp_pipe;	/* 5-tuple ipv4 udp match pipe */
	struct security_gateway_pipe_info ipv6_tcp_pipe;	/* 5-tuple ipv6 tcp match pipe */
	struct security_gateway_pipe_info ipv6_udp_pipe;	/* 5-tuple ipv6 udp match pipe */
	struct security_gateway_pipe_info ipv6_src_tcp_pipe;	/* src ipv6 tcp match pipe */
	struct security_gateway_pipe_info ipv6_src_udp_pipe;	/* src ipv6 udp match pipe */
	struct security_gateway_pipe_info vxlan_encap_pipe;	/* vxlan encap pipe */
	struct security_gateway_pipe_info marker_insert_pipe;	/* insert non-ESP marker pipe */
};

/* all the pipes that is used for decrypt packets */
struct decrypt_pipes {
	struct security_gateway_pipe_info decrypt_root;		 /* decrypt control pipe */
	struct security_gateway_pipe_info marker_remove_pipe;	 /* remove non-ESP marker pipe */
	struct security_gateway_pipe_info decrypt_ipv4_pipe;	 /* decrypt ipv4 pipe */
	struct security_gateway_pipe_info decrypt_ipv6_pipe;	 /* decrypt ipv6 pipe */
	struct security_gateway_pipe_info decap_pipe;		 /* decap ESP header pipe */
	struct security_gateway_pipe_info bad_syndrome_pipe;	 /* match on ipsec bad syndrome */
	struct security_gateway_pipe_info vxlan_decap_ipv4_pipe; /* decap vxlan tunnel inner ipv4 pipe */
	struct security_gateway_pipe_info vxlan_decap_ipv6_pipe; /* decap vxlan tunnel inner ipv6 pipe */
};

/* all the pipes that is ued for switch mode */
struct switch_pipes {
	struct security_gateway_pipe_info rss_pipe;	 /* RSS pipe */
	struct security_gateway_pipe_info pkt_meta_pipe; /* packet meta */
};

/* Application rules arrays {encryption, decryption}*/
struct ipsec_security_gw_rules {
	struct encrypt_rule *encrypt_rules; /* Encryption rules array */
	struct decrypt_rule *decrypt_rules; /* Decryption rules array */
	int nb_encrypt_rules;		    /* Number of encryption rules in array */
	int nb_decrypt_rules;		    /* Number of decryption rules in array */
	int nb_rules;			    /* Total number of rules, will be used to indicate
					     * which crypto index is the next one.
					     */
};

/* IPsec Security Gateway modes */
enum ipsec_security_gw_mode {
	IPSEC_SECURITY_GW_TUNNEL,	 /* ipsec tunnel mode */
	IPSEC_SECURITY_GW_TRANSPORT,	 /* ipsec transport mode */
	IPSEC_SECURITY_GW_UDP_TRANSPORT, /* ipsec transport mode over UDP */
};

/* IPsec Security Gateway flow modes */
enum ipsec_security_gw_flow_mode {
	IPSEC_SECURITY_GW_VNF,	  /* DOCA Flow vnf mode */
	IPSEC_SECURITY_GW_SWITCH, /* DOCA Flow switch mode */
};

/* IPsec Security Gateway ESP offload */
enum ipsec_security_gw_esp_offload {
	IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH,  /* HW offload for both encap and decap */
	IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP, /* HW offload for encap, decap in SW */
	IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP, /* HW offload for decap, encap in SW */
	IPSEC_SECURITY_GW_ESP_OFFLOAD_NONE,  /* encap and decap both done in SW */
};

/* IPsec Security Gateway perf mode */
enum ipsec_security_gw_perf {
	IPSEC_SECURITY_GW_PERF_NONE,	       /* avoid any performance measurement */
	IPSEC_SECURITY_GW_PERF_INSERTION_RATE, /* print insertion rate results */
	IPSEC_SECURITY_GW_PERF_BW,	       /* optimize the pipeline for bandwidth measure */
	IPSEC_SECURITY_GW_PERF_BOTH,	       /* both insertion rate measure and bw optimize */
};

/* IPsec Security Gateway forward bad syndrome type */
enum ipsec_security_gw_fwd_syndrome {
	IPSEC_SECURITY_GW_FWD_SYNDROME_DROP, /* drop bad syndrome packets */
	IPSEC_SECURITY_GW_FWD_SYNDROME_RSS,  /* forward bad syndrome packets to app */
};

/* IPsec Security Gateway device information */
struct ipsec_security_gw_dev_info {
	bool is_representor;		   /* true if device is representor */
	struct doca_dev *doca_dev;	   /* DOCA device */
	struct doca_dev_rep *doca_dev_rep; /* DOCA device representor */
};

/* IPsec Security Gateway DOCA objects */
struct ipsec_security_gw_doca_objects {
	struct ipsec_security_gw_dev_info secured_dev;	 /* DOCA device for secured network */
	struct ipsec_security_gw_dev_info unsecured_dev; /* DOCA device for unsecured network */
};

/* IPsec Security Gateway DOCA socket context */
struct ipsec_security_gw_socket_ctx {
	int fd;					/* Socket file descriptor */
	int connfd;				/* Connection file descriptor */
	char socket_path[MAX_SOCKET_PATH_NAME]; /* Socket file path */
	bool socket_conf;			/* If IPC mode is enabled */
};

/* IPsec Security Gateway configuration structure */
struct ipsec_security_gw_config {
	bool sw_sn_inc_enable;				  /* true for doing sn increment in software */
	bool sw_antireplay;				  /* true for doing anti-replay in software */
	bool debug_mode;				  /* run in debug mode */
	bool vxlan_encap;				  /* True for vxlan encap / decap */
	bool marker_encap;				  /* insert/remove non-ESP marker header */
	enum ipsec_security_gw_mode mode;		  /* application mode */
	enum ipsec_security_gw_flow_mode flow_mode;	  /* DOCA Flow mode */
	enum ipsec_security_gw_esp_offload offload;	  /* ESP offload */
	enum ipsec_security_gw_perf perf_measurement;	  /* performance measurement mode */
	enum ipsec_security_gw_fwd_syndrome syndrome_fwd; /* fwd type for bad syndrome packets */
	uint64_t sn_initial;				  /* set the initial sequence number */
	char json_path[MAX_FILE_NAME];			  /* Path to the JSON file with rules */
	struct rte_hash *ip6_table;			  /* IPV6 addresses hash table */
	struct application_dpdk_config *dpdk_config;	  /* DPDK configuration struct */
	struct decrypt_pipes decrypt_pipes;		  /* Decryption DOCA flow pipes */
	struct encrypt_pipes encrypt_pipes;		  /* Encryption DOCA flow pipes */
	struct switch_pipes switch_pipes;		  /* Encryption DOCA flow pipes */
	struct ipsec_security_gw_rules app_rules;	  /* Application encryption/decryption rules */
	struct ipsec_security_gw_doca_objects objects;	  /* Application DOCA objects */
	struct ipsec_security_gw_socket_ctx socket_ctx;	  /* Application DOCA socket context */
	uint8_t nb_cores;				  /* number of cores to DPDK -l flag */
	uint32_t vni;					  /* vni to use when vxlan encap is true */
	enum doca_flow_crypto_icv_len icv_length;	  /* Supported icv (Integrity Check Value) length */
	struct entries_status *secured_status;
	struct entries_status *unsecured_status;
};

/*
 * Open DOCA devices according to the pci-address input and probe dpdk ports
 *
 * @app_cfg [in/out]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_init_devices(struct ipsec_security_gw_config *app_cfg);

/*
 * Close DOCA devices
 *
 * @app_cfg [in]: application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t ipsec_security_gw_close_devices(const struct ipsec_security_gw_config *app_cfg);

/*
 * Get dpdk port ID and check if its encryption port or decryption, based on
 * user PCI input and DOCA device devinfo
 *
 * @app_cfg [in]: application configuration structure
 * @port_id [in]: port ID
 * @connected_dev [in]: doca device that connected to this port id
 * @idx [out]: index for ports array - 0 for secured network index and 1 for unsecured
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t find_port_action_type_vnf(const struct ipsec_security_gw_config *app_cfg,
				       int port_id,
				       struct doca_dev **connected_dev,
				       int *idx);

/*
 * Get dpdk port ID and check if its encryption port or decryption, by checking if the port is representor
 * representor port is the unsecured port
 *
 * @port_id [in]: port ID
 * @idx [out]: index for ports array - 0 for secured network index and 1 for unsecured
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t find_port_action_type_switch(int port_id, int *idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IPSEC_CTX_H_ */
