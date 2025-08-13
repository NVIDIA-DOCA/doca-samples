/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef UPF_ACCEL_H_
#define UPF_ACCEL_H_

#include <stdbool.h>

#include <rte_malloc.h>
#include <rte_hash.h>

#include <doca_flow.h>
#include <doca_flow_net.h>

#include <flow_common.h>
#include <packet_parser.h>

#define UPF_ACCEL_LOG_MAX_NUM_CONNECTIONS 19
#define UPF_ACCEL_MAX_NUM_CONNECTIONS (1ul << UPF_ACCEL_LOG_MAX_NUM_CONNECTIONS)

enum upf_accel_port {
	UPF_ACCEL_PORT0,
	UPF_ACCEL_PORT1,
	UPF_ACCEL_PORTS_MAX,
};

#define UPF_ACCEL_NUM_DOMAINS_PER_PORT 2
#define UPF_ACCEL_NUM_DOMAINS (UPF_ACCEL_PORTS_MAX * UPF_ACCEL_NUM_DOMAINS_PER_PORT)

#define UPF_ACCEL_PDR_STR_LEN 64
#define UPF_ACCEL_PDR_URRIDS_LEN 16
#define UPF_ACCEL_PDR_QERIDS_LEN 16
#define UPF_ACCEL_IP_STR_LEN 64

#define UPF_ACCEL_LOG_MAX_NUM_PDR 5
#define UPF_ACCEL_MAX_NUM_PDR (1ul << UPF_ACCEL_LOG_MAX_NUM_PDR)

#define UPF_ACCEL_NUM_QUOTA_COUNTERS_PER_PORT UPF_ACCEL_MAX_NUM_PDR

#define UPF_ACCEL_SRC_MAC \
	{ \
		0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 \
	}
#define UPF_ACCEL_DST_MAC \
	{ \
		0xde, 0xad, 0xbe, 0xef, 0x00, 0x02 \
	}

#define UPF_ACCEL_SRC_IP 0xc0a80101 // 192.168.1.1
#define UPF_ACCEL_DST_IP 0xc0a80201 // 192.168.2.1
#define UPF_ACCEL_SRC_IPV6 \
	{ \
		0x2801, 0xdb8, 0, 0, 0, 0, 0xc0a8, 0x0101 \
	}

#define UPF_ACCEL_NUM_BYTES_IPV6 16

#define UPF_ACCEL_LOG_MAX_PDR_NUM_RATE_METERS 2
#define UPF_ACCEL_MAX_PDR_NUM_RATE_METERS (1ul << UPF_ACCEL_LOG_MAX_PDR_NUM_RATE_METERS)

#define UPF_ACCEL_META_PKT_DIR_OFFSET (UPF_ACCEL_LOG_MAX_NUM_PDR)
#define UPF_ACCEL_META_PKT_DIR_UL (0x1 << UPF_ACCEL_META_PKT_DIR_OFFSET)
#define UPF_ACCEL_META_PKT_DIR_DL (0x2 << UPF_ACCEL_META_PKT_DIR_OFFSET)
#define UPF_ACCEL_META_PKT_DIR_MASK (0x3 << UPF_ACCEL_META_PKT_DIR_OFFSET)

#define UPF_ACCEL_HW_AGING_POLL_INTERVAL_SEC (1)
#define UPF_ACCEL_HW_AGING_TIME_DEFAULT_SEC (15)

#define UPF_ACCEL_SW_AGING_TIME_DEFAULT_SEC (15)

/*
 * Number of packets handled in SW before deciding to accelerate, example:
 * If UPF_ACCEL_DEFAULT_DPI_THRESHOLD = 2 then we handle 2 packets in SW,
 * the 2nd packet triggers a rule creation for the side it came from.
 * Must be positive (1 is for undelayed acceleration).
 */
#define UPF_ACCEL_DEFAULT_DPI_THRESHOLD 2

#define UPF_ACCEL_FIXED_PORT_NONE (-1)

#define UNUSED(x) ((void)(x))

extern volatile bool force_quit;

typedef enum upf_accel_port (*upf_accel_get_forwarding_port)(enum upf_accel_port port_id);

struct upf_accel_fp_data;

enum upf_accel_pdr_pdi_si {
	/* Only those two types are supported */
	UPF_ACCEL_PDR_PDI_SI_UL = 0, /* Source Interface - Uplink */
	UPF_ACCEL_PDR_PDI_SI_DL = 2, /* Source Interface - Downlink */
};

enum upf_accel_far_action {
	UPF_ACCEL_FAR_ACTION_DROP = 1,
	UPF_ACCEL_FAR_ACTION_FWD = 2,
};

enum upf_accel_pipe_drop_type {
	UPF_ACCEL_DROP_DBG,
	UPF_ACCEL_DROP_RATE,
	UPF_ACCEL_DROP_FILTER,
	UPF_ACCEL_DROP_NUM,
};

enum upf_accel_pipe_type {
	UPF_ACCEL_PIPE_RX_ROOT,
	UPF_ACCEL_PIPE_FAR,
	UPF_ACCEL_PIPE_RX_VXLAN_DECAP,
	UPF_ACCEL_PIPE_RX_DROPS_START,
	UPF_ACCEL_PIPE_RX_DROPS_END = UPF_ACCEL_PIPE_RX_DROPS_START + UPF_ACCEL_DROP_NUM,

	UPF_ACCEL_PIPE_TX_ROOT,
	UPF_ACCEL_PIPE_TX_COUNTER,
	UPF_ACCEL_PIPE_TX_VXLAN_ENCAP,
	UPF_ACCEL_PIPE_TX_DROPS_START,
	UPF_ACCEL_PIPE_TX_DROPS_END = UPF_ACCEL_PIPE_TX_DROPS_START + UPF_ACCEL_DROP_NUM,

	UPF_ACCEL_PIPE_TX_SHARED_METERS_START,
	UPF_ACCEL_PIPE_TX_SHARED_METERS_END = UPF_ACCEL_PIPE_TX_SHARED_METERS_START + UPF_ACCEL_MAX_PDR_NUM_RATE_METERS,
	UPF_ACCEL_PIPE_TX_COLOR_MATCH_START,
	UPF_ACCEL_PIPE_TX_COLOR_MATCH_END = UPF_ACCEL_PIPE_TX_COLOR_MATCH_START + UPF_ACCEL_MAX_PDR_NUM_RATE_METERS - 1,
	UPF_ACCEL_PIPE_TX_COLOR_MATCH_NO_MORE_METERS,

	UPF_ACCEL_PIPE_ULDL,
	UPF_ACCEL_PIPE_EXT_GTP,

	UPF_ACCEL_PIPE_8T_INNER_IP_TYPE,
	UPF_ACCEL_PIPE_8T_IPV4,
	UPF_ACCEL_PIPE_8T_IPV6,
	UPF_ACCEL_PIPE_8T_IPV6_EXT,

	UPF_ACCEL_PIPE_7T_INNER_IP_TYPE,
	UPF_ACCEL_PIPE_7T_IPV4,
	UPF_ACCEL_PIPE_7T_IPV6,
	UPF_ACCEL_PIPE_7T_IPV6_EXT,

	UPF_ACCEL_PIPE_UL_TO_SW_IPV4,
	UPF_ACCEL_PIPE_UL_TO_SW_IPV6,

	UPF_ACCEL_PIPE_5T_OUTER_IP_TYPE,
	UPF_ACCEL_PIPE_5T_IPV4,
	UPF_ACCEL_PIPE_5T_IPV6,
	UPF_ACCEL_PIPE_5T_IPV6_EXT,

	UPF_ACCEL_PIPE_DL_TO_SW_IPV4,
	UPF_ACCEL_PIPE_DL_TO_SW_IPV6,

	UPF_ACCEL_PIPE_DECAP,

	UPF_ACCEL_PIPE_NUM,
};

enum upf_accel_encap_action_type {
	UPF_ACCEL_ENCAP_ACTION_IPV4_4G,
	UPF_ACCEL_ENCAP_ACTION_IPV4_5G,
	UPF_ACCEL_ENCAP_ACTION_IPV6_4G,
	UPF_ACCEL_ENCAP_ACTION_IPV6_5G,
	UPF_ACCEL_ENCAP_ACTION_NONE,
	UPF_ACCEL_ENCAP_ACTION_NUM,
};

enum upf_accel_rule_type {
	UPF_ACCEL_RULE_DYNAMIC,
	UPF_ACCEL_RULE_STATIC,
	UPF_ACCEL_RULE_DYNAMIC_EXTENSION,
};

enum upf_accel_entry_state {
	UPF_ACCEL_ENTRY_STATE_NONE,		      /* Entry isn't active (not established yet or deleted) */
	UPF_ACCEL_ENTRY_STATE_PENDING,		      /* Entry is in the proccess of being created */
	UPF_ACCEL_ENTRY_STATE_UNACCELERATED,	      /* Entry not accelerated yet */
	UPF_ACCEL_ENTRY_STATE_PENDING_ACCELERATION,   /* Entry is in the proccess of HW acceleration */
	UPF_ACCEL_ENTRY_STATE_ACCELERATED,	      /* Entry is accelerated (by HW) */
	UPF_ACCEL_ENTRY_STATE_PENDING_DEACCELERATION, /* Flow is in the proccess of removal from acceleration */
};

struct app_shared_counter_ids {
	uint32_t *ids[UPF_ACCEL_PORTS_MAX]; /* Array of IDs per port */
	uint16_t cntr_0;		    /* Index of the first counter */
	size_t cntrs_num;		    /* Number of counters (in each port) */
};

union upf_accel_ip {
	uint32_t v4;			      /* IPV4 address */
	uint8_t v6[UPF_ACCEL_NUM_BYTES_IPV6]; /* IPV6 address */
} __rte_aligned(UPF_ACCEL_NUM_BYTES_IPV6);

struct upf_accel_ip_addr {
	union upf_accel_ip addr;	   /* IP address */
	union upf_accel_ip mask;	   /* IP address mask */
	enum doca_flow_l3_type ip_version; /* IP version */
};

struct upf_accel_ip_port_range {
	uint16_t from; /* Start of the range */
	uint16_t to;   /* End of the range*/
};

struct upf_accel_pdr {
	uint32_t id;						/* PDR ID */
	uint32_t farid;						/* FAR ID */
	uint32_t urrids_num;					/* Number of URR IDs */
	uint32_t urrids[UPF_ACCEL_PDR_URRIDS_LEN];		/* List of URR IDs */
	uint32_t qerids_num;					/* Number of QER IDs, equivalent to num of meters */
	uint32_t qerids[UPF_ACCEL_PDR_QERIDS_LEN];		/* List of QER IDs */
	enum upf_accel_pdr_pdi_si pdi_si;			/* PDI's source interface */
	uint32_t pdi_local_teid_start;				/* PDI's local TEID start */
	uint32_t pdi_local_teid_end;				/* PDI's local TEID end */
	struct upf_accel_ip_addr pdi_local_teid_ip;		/* PDI's local teid IP */
	struct upf_accel_ip_addr pdi_ueip;			/* PDI's UEIP */
	uint16_t pdi_sdf_proto;					/* PDI's SDF protocol */
	uint8_t pdi_qfi;					/* PDI's QFI */
	struct upf_accel_ip_addr pdi_sdf_from_ip;		/* PDI's SDF from IP */
	struct upf_accel_ip_port_range pdi_sdf_from_port_range; /* PDI's SDF from port range */
	struct upf_accel_ip_addr pdi_sdf_to_ip;			/* PDI's SDF to IP */
	struct upf_accel_ip_port_range pdi_sdf_to_port_range;	/* PDI's SDF to port range */
};

struct upf_accel_pdrs {
	size_t num_pdrs;		 /* Number of PDRs */
	struct upf_accel_pdr arr_pdrs[]; /* PDRs array */
};

struct upf_accel_far {
	uint32_t id;			   /* FAR ID */
	struct upf_accel_ip_addr fp_oh_ip; /* Forwardind policy outer header creation IP */
	uint32_t fp_oh_teid;		   /* Forwarding policy outer header creation teid */
};

struct upf_accel_fars {
	size_t num_fars;		 /* Number of FARs */
	struct upf_accel_far arr_fars[]; /* FARs array */
};

struct upf_accel_urr {
	uint32_t id;			    /* URR ID */
	uint64_t volume_quota_total_volume; /* Volume quota total volume */
};

struct upf_accel_urrs {
	size_t num_urrs;		 /* Number of URRs */
	struct upf_accel_urr arr_urrs[]; /* URRs array */
};

struct upf_accel_qer {
	uint32_t id;	     /* QER ID */
	uint8_t qfi;	     /* QFI */
	uint64_t mbr_dl_mbr; /* MBR downlink */
	uint64_t mbr_ul_mbr; /* MBR uplink */
};

struct upf_accel_qers {
	size_t num_qers;		 /* Number of QERs */
	struct upf_accel_qer arr_qers[]; /* QERs array */
};

struct upf_accel_vxlan {
	uint32_t id;			       /* VXLAN ID */
	uint32_t vni;			       /* VXLAN network identifier */
	uint8_t mac[DOCA_FLOW_ETHER_ADDR_LEN]; /* MAC address */
};

struct upf_accel_vxlans {
	size_t num_vxlans;		     /* Number of VXLANs */
	struct upf_accel_vxlan arr_vxlans[]; /* VXLANs array */
};

struct upf_accel_config {
	const char *smf_config_file_path;   /* Path to SMF configuration file */
	struct upf_accel_pdrs *pdrs;	    /* PDRs */
	struct upf_accel_fars *fars;	    /* FARs */
	struct upf_accel_urrs *urrs;	    /* URRs */
	struct upf_accel_qers *qers;	    /* QERs */
	const char *vxlan_config_file_path; /* Path to SMF configuration file */
	struct upf_accel_vxlans *vxlans;    /* VXLANs */
	uint32_t hw_aging_time_sec;	    /* Amount of seconds before deleting an accelerated flow */
	uint32_t sw_aging_time_sec;	    /* Amount of seconds before deleting an unaccelerated flow */
	uint32_t dpi_threshold;		    /* Number of packets handled in SW before deciding to accelerate */
	uint32_t fixed_port;		    /* UL port number in fixed port mode */
	struct flow_dev_ctx flow_devs;	    /* Flow device context */
};

struct upf_accel_match_tun {
	union upf_accel_ip te_ip; /* Source IP */
	uint32_t te_id;		  /* TEID */
	uint8_t qfi;		  /* QFI */
	uint8_t ip_version;	  /* IP version */
};
static_assert(sizeof(struct upf_accel_match_tun) == 32, "Unexpected tunnel key size");

struct upf_accel_match_5t {
	union upf_accel_ip ue_ip;     /* User equipment IP */
	union upf_accel_ip extern_ip; /* Extern IP */
	uint16_t ue_port;	      /* User equipment port */
	uint16_t extern_port;	      /* Extern port */
	uint8_t ip_version;	      /* IP version */
	uint8_t ip_proto;	      /* IP protocol */
};
static_assert(sizeof(struct upf_accel_match_5t) == 48, "Unexpected 5t key size");

struct upf_accel_match_8t {
	struct upf_accel_match_5t inner;  /* Inner match */
	struct upf_accel_match_tun outer; /* Outer (tunnel) match */
};
static_assert(sizeof(struct upf_accel_match_8t) == 80, "Unexpected 8t key size");

struct upf_accel_match_extension {
	uint8_t ue_ip[UPF_ACCEL_NUM_BYTES_IPV6]; /* IPV6 address */
	uint32_t te_id;				 /* TEID */
	uint8_t qfi;				 /* QFI */
};
static_assert(sizeof(struct upf_accel_match_extension) == 24, "Unexpected extension key size");

struct upf_accel_sw_aging_ll {
	int32_t head; /* Head of the SW aging linked list */
	int32_t tail; /* Tail of the SW aging linked list */
};

struct upf_accel_sw_aging_ll_node {
	int32_t prev;	    /* Index of the prev node in the SW aging linked list */
	int32_t next;	    /* Index of the next node in the SW aging linked list */
	uint64_t timestamp; /* Timestamp of the last packet received in this flow */
};

struct upf_accel_extension_entry_ctx {
	struct upf_accel_match_extension match; /* Extension entry match */
	struct doca_flow_pipe_entry *entry;	/* Flow accelerated entry */
	struct upf_accel_fp_data *fp_data;	/* Pointer to the data of the handling core */
	struct doca_flow_pipe_entry *del_next;	/* Entry to delete next */
	hash_sig_t hash;			/* RTE hash (aka signature) */
	uint32_t md_value;			/* Metadata value to set in the entry */
	uint32_t ref_cnt;			/* Reference count of entries pointed to by the extension entry */
	enum upf_accel_entry_state state;	/* Extension entry state */
};

struct upf_accel_entry_ctx;

struct upf_accel_flow_entry {
	struct doca_flow_pipe_entry *entry;		 /* Flow accelerated entry */
	struct upf_accel_entry_ctx *ext_entry;		 /* Extension entry context */
	struct upf_accel_sw_aging_ll_node sw_aging_node; /* SW Aging linked list node */
	enum upf_accel_entry_state state;		 /* Entry state */
};

struct upf_accel_dyn_entry_ctx {
	struct upf_accel_match_8t match;			  /* Connection match */
	uint64_t cnt_pkts[PARSER_PKT_TYPE_NUM];			  /* Packets counter */
	uint64_t cnt_bytes[PARSER_PKT_TYPE_NUM];		  /* Bytes counter */
	struct upf_accel_flow_entry entries[PARSER_PKT_TYPE_NUM]; /* Pipe entries (accelerated) / Linked list entries
								     (unaccelerated) */
	struct upf_accel_fp_data *fp_data;			  /* Pointer to the data of the handling core */
	uint32_t pdr_id[PARSER_PKT_TYPE_NUM];			  /* PDR ID */
	int32_t conn_idx;					  /* Position of the connection in the hash table */
	hash_sig_t hash;					  /* RTE hash (aka signature) */
	enum upf_accel_port port_id;				  /* Port ID of the offloaded entry */
};

struct upf_accel_static_entry_ctx {
	struct entries_status ctrl_status; /* Control status */
};

struct upf_accel_entry_ctx {
	enum upf_accel_rule_type type; /* Type of the entry */
	union {
		struct upf_accel_dyn_entry_ctx dyn_ctx;	      /* Dynamic entry context */
		struct upf_accel_static_entry_ctx static_ctx; /* Static entry context */
		struct upf_accel_extension_entry_ctx ext_ctx; /* Extension entry context */
	};
} __rte_aligned(RTE_CACHE_LINE_SIZE);

struct upf_accel_ctx {
	uint16_t num_ports;						       /* Number of ports */
	uint16_t num_queues;						       /* Number of device queues */
	struct flow_resources resource;					       /* Flow resources */
	uint32_t num_shared_resources[SHARED_RESOURCE_NUM_VALUES];	       /* Number of shared resources */
	const struct upf_accel_config *upf_accel_cfg;			       /* UPF Acceleration configuration */
	struct doca_flow_pipe *pipes[UPF_ACCEL_PORTS_MAX][UPF_ACCEL_PIPE_NUM]; /* Pipes */
	struct doca_flow_port *ports[UPF_ACCEL_PORTS_MAX];		       /* Ports */
	struct doca_dev *dev_arr[UPF_ACCEL_PORTS_MAX];			       /* Devices array */
	struct doca_flow_pipe_entry *smf_entries[UPF_ACCEL_MAX_NUM_PDR][UPF_ACCEL_PORTS_MAX]; /* Resulting hw entries */
	struct doca_flow_pipe_entry *drop_entries[UPF_ACCEL_DROP_NUM][UPF_ACCEL_NUM_DOMAINS]; /* Resulting hw Drops
												 entries */
	struct upf_accel_entry_ctx static_entry_ctx[UPF_ACCEL_PORTS_MAX]; /* Static entries contexs */
	uint32_t num_static_entries[UPF_ACCEL_PORTS_MAX];		  /* Number of static entries */
	upf_accel_get_forwarding_port get_fwd_port;			  /* Function pointer to get fwd port */
};

struct upf_accel_action_cfg {
	struct doca_flow_actions **action_list;		  /* List of actions */
	struct doca_flow_action_descs **action_desc_list; /* List of action descriptions */
	size_t num_actions;				  /* Number of actions */
};

struct upf_accel_pipe_cfg {
	enum upf_accel_port port_id;	     /* Port ID */
	struct doca_flow_port *port;	     /* Port */
	enum doca_flow_pipe_domain domain;   /* Domain (RX/TX) */
	char *name;			     /* Pipe name */
	bool is_root;			     /* Flag to indicate if the pipe is root */
	uint32_t num_entries;		     /* Number of entries */
	struct doca_flow_match *match;	     /* Match */
	struct doca_flow_match *match_mask;  /* Match mask */
	struct doca_flow_fwd *fwd;	     /* Forward hit */
	struct doca_flow_fwd *fwd_miss;	     /* Forward miss */
	struct doca_flow_monitor *mon;	     /* Monitor */
	struct upf_accel_action_cfg actions; /* UPF Acceleration actions */
};

struct upf_accel_entry_cfg {
	struct doca_flow_pipe *pipe;	   /* Pipe */
	struct doca_flow_match *match;	   /* Match */
	struct doca_flow_actions *action;  /* Action */
	struct doca_flow_monitor *mon;	   /* Monitor */
	struct doca_flow_fwd *fwd;	   /* Forward */
	enum upf_accel_port port_id;	   /* Port ID */
	enum doca_flow_pipe_domain domain; /* Domain (RX/TX) */
	uint32_t entry_idx;		   /* Entry index */
};

/*
 * Calculate index of a given drop pipe
 *
 * @pipe_cfg [in]: UPF Acceleration configuration
 * @drop_type [in]: the desired drop pipe type
 * @return: index of the desired pipe
 */
static inline uint8_t upf_accel_drop_idx_get(struct upf_accel_pipe_cfg *pipe_cfg,
					     enum upf_accel_pipe_drop_type drop_type)
{
	return (pipe_cfg->domain ? UPF_ACCEL_PIPE_TX_DROPS_START : UPF_ACCEL_PIPE_RX_DROPS_START) + drop_type;
}

/*
 * Calculate index of the given domain of the given port
 *
 * @port_id [in]: port id
 * @domain [in]: domain id
 * @return: index of the domain
 */
static inline uint8_t upf_accel_domain_idx_get(enum upf_accel_port port_id, uint8_t domain)
{
	return 2 * port_id + !!domain;
}

/*
 * Get the opposite port of a given port
 *
 * @port_id [in]: port ID
 * @return: the opposite port
 */
static inline enum upf_accel_port upf_accel_get_opposite_port(enum upf_accel_port port_id)
{
	const enum upf_accel_port opposite_port = port_id ^ 1;

	assert(opposite_port == UPF_ACCEL_PORT0 || opposite_port == UPF_ACCEL_PORT1);

	return opposite_port;
}

/*
 * SMF Config parsing & initialization
 *
 * @cfg [in]: UPF Acceleration configuration.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t upf_accel_smf_parse(struct upf_accel_config *cfg);

/*
 * VXLAN Config parsing & initialization
 *
 * @cfg [in]: UPF Acceleration onfiguration.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t upf_accel_vxlan_parse(struct upf_accel_config *cfg);

/*
 * Cleans SMF configuration logic
 *
 * @cfg [in]: UPF Acceleration configuration.
 */
void upf_accel_smf_cleanup(struct upf_accel_config *cfg);

/*
 * Cleans VXLAN configuration logic
 *
 * @cfg [in]: UPF Acceleration configuration.
 */
void upf_accel_vxlan_cleanup(struct upf_accel_config *cfg);

/*
 * Init the SW Aging doubly linked list head & tail
 *
 * @fp_data [in]: flow processing data
 * @pkt_type [in]: packet direction
 */
void upf_accel_sw_aging_ll_init(struct upf_accel_fp_data *fp_data, enum parser_pkt_type pkt_type);

#endif /* UPF_ACCEL_H_ */
