/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

/* CT Firewall shared types, limits and module interfaces. */

#ifndef CT_FW_H_
#define CT_FW_H_

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rte_common.h>
#include <rte_ether.h>
#include <rte_mempool.h>

#include <doca_error.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>

#include <dpdk_utils.h>
#include <flow_common.h>
#include <flow_ct_common.h>
#include <flow_switch_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CT_FW_MAX_PORTS 2
/* Uplink i is DOCA Flow port id i. */
#define CT_FW_FIRST_UPLINK 0  /* switch manager (-a) */
#define CT_FW_SECOND_UPLINK 1 /* representor (-r) */
#define CT_FW_MAX_WORKERS 16  /* one worker per CT queue */
#define CT_FW_PACKET_BURST 64
/* Most packets left queued when an entry goes live or is re-enabled (both modes). */
#define CT_FW_BACKLOG_PKTS (RX_RING_SIZE + CT_FW_PACKET_BURST)
/* A segment that missed an entry just before it went live can reach the receive queue this much later (measured
 * up to about 40 us; an empirical margin, not a bound). */
#define CT_FW_BACKLOG_SETTLE_US 100
#define CT_FW_DEFAULT_CT_QUEUES 1
#define CT_FW_DEFAULT_POOL 8192	    /* --pool-size default */
#define CT_FW_DEFAULT_SESSIONS 8192 /* --sessions default */
/* Counter-pool limit: two counters per connection per family. */
#define CT_FW_MAX_SESSIONS 3800000
#define CT_FW_MAX_POOL CT_DEFAULT_MAX_ZONE_SESSIONS
#define CT_FW_MAX_COUNTING 4096 /* connections adopting or deferring their install */

#define CT_FW_MAX_ECHO 1024	 /* pings tracked under NAT */
#define CT_FW_ARP_PENDING 64	 /* ARP requests remembered under NAT, per table */
#define CT_FW_ARP_PENDING_S 3	 /* how long one waits for its reply */
#define CT_FW_MAX_SNAT_OWNERS 64 /* --snat client hosts, one per zone */

/* ICMPv4 types (RFC 792) the NAT path handles; older DPDK releases do not name them all. */
#define CT_FW_ICMP_ECHO_REPLY 0
#define CT_FW_ICMP_DEST_UNREACHABLE 3
#define CT_FW_ICMP_ECHO_REQUEST 8
#define CT_FW_ICMP_TTL_EXCEEDED 11
#define CT_FW_ICMP_PARAM_PROBLEM 12

/* Hardware and software idle limit, in seconds. */
#define CT_FW_DEFAULT_AGING_TIMEOUT_S 30
#define CT_FW_MAX_AGING_TIMEOUT_S UINT16_MAX
/* Idle limit of an unanswered connection attempt. */
#define CT_FW_UNCONFIRMED_TIMEOUT_S 5
/* An operation this old means the CT queue failed. */
#define CT_FW_OP_WATCHDOG_S 10
#define CT_FW_SWEEP_MIN_PER_ROUND 64 /* least slots owed per sweep round */
#define CT_FW_SWEEP_SLICE 1024u	     /* most slots examined per loop iteration */
#define CT_FW_SWEEP_INTERVAL_S 1     /* seconds between sweep rounds */
/* doca_flow_aging_handle limits per call and per round. */
#define CT_FW_AGING_QUOTA_US 1000
#define CT_FW_AGING_BURST 64
#define CT_FW_AGING_CALLS_PER_ROUND 16
#define CT_FW_AGING_CYCLE_DONE (-1)
#define CT_FW_BUILD_DRAIN_TRIES 5
#define CT_FW_BUILD_BATCH 64 /* entries submitted between drains */
/* Queue operations one packet can need. */
#define CT_FW_ENTRY_OPS 2u
#define CT_FW_CT_QUEUE_RESERVE (CT_FW_ENTRY_OPS * CT_FW_PACKET_BURST)
_Static_assert(CT_FW_AGING_BURST <= CT_FW_PACKET_BURST, "the queue reserve must cover a burst of aging removals");
_Static_assert(CT_FW_CT_QUEUE_RESERVE <= CT_DEFAULT_QUEUE_DEPTH, "the queue reserve must fit the CT operation queue");
#define CT_FW_RM_FLAGS DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT
/* No duplicate filter: prepare never finds an existing entry, in either mode. */
#define CT_FW_PREPARE_FLAGS DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS
_Static_assert((CT_FW_PREPARE_FLAGS &
		(DOCA_FLOW_CT_ENTRY_FLAGS_DUP_FILTER_ORIGIN | DOCA_FLOW_CT_ENTRY_FLAGS_DUP_FILTER_REPLY)) == 0,
	       "prepare never finds an existing entry without a duplicate filter");
/* An overflow falls to the sweep. */
#define CT_FW_MAX_PENDING_FREE (2 * (CT_FW_PACKET_BURST + CT_FW_AGING_BURST))
/* Window-validation operations outstanding per worker: the CT queue depth. */
#define CT_FW_WV_POOL_SIZE CT_DEFAULT_QUEUE_DEPTH
_Static_assert(CT_FW_WV_POOL_SIZE < UINT16_MAX, "a slot index must fit the connection's 16-bit field");
/* Slots an install may not take: installs are refused once 448 slots are in use, leaving these to re-enables. */
#define CT_FW_WV_REENABLE_RESERVE 64
_Static_assert(CT_FW_WV_REENABLE_RESERVE < CT_FW_WV_POOL_SIZE, "installs must keep some slots");
#define CT_FW_MAX_PENDING_REPLAY CT_FW_WV_POOL_SIZE
#define CT_FW_HASH_MIN_ENTRIES 8u /* smallest DPDK hash table */
#define CT_FW_DEFAULT_STATS_INTERVAL 5
#define CT_FW_MAX_STATS_INTERVAL 3600
#define CT_FW_MAX_ACK_WINDOW 7
#define CT_FW_MAX_OFFLOAD_AFTER INT32_MAX
#define CT_FW_MBUF_SIZE 10240 /* covers a 9000-byte MTU */
#define CT_FW_IPV4_ADDR_BYTES 4
#define CT_FW_IPV6_ADDR_BYTES 16
#define CT_FW_IPV6_VERSION 6 /* top 4 bits of vtc_flow */
#define CT_FW_VLAN_VID_BITS 12
#define CT_FW_VLAN_VID_MASK 0x0fffu /* PCP and DEI ignored */
#define CT_FW_VLAN_VID_MAX 4095	    /* highest VLAN ID */
#define CT_FW_MAX_VLAN_TAGS 2	    /* tags in a zone */
/* IPv4 fragment reassembly, per worker. */
#define CT_FW_FRAG_MAX_DGRAMS 64
#define CT_FW_FRAG_MAX_PER_DGRAM 8
#define CT_FW_FRAG_MAX_LEN 9000 /* reassembled IP length */
#define CT_FW_FRAG_TIMEOUT_S 1
#define CT_FW_FRAG_L4_TOTAL_UNKNOWN 0 /* no last fragment yet */

/* pkt_meta: [3:0] CT, [4] reinject, [6:5] port, [31:7] zone. */
#define CT_FW_META_REINJECT_BIT 4u
#define CT_FW_META_REINJECT_MASK (1u << CT_FW_META_REINJECT_BIT)
#define CT_FW_META_PORT_SHIFT 5u
#define CT_FW_META_PORT_MASK (0x3u << CT_FW_META_PORT_SHIFT)
/* Port code [6:5]: uplink + 1. */
#define CT_FW_PORT_CODE_NONE 0u
#define CT_FW_PORT_CODE(uplink) ((((uint32_t)(uplink) + 1u) << CT_FW_META_PORT_SHIFT) & CT_FW_META_PORT_MASK)
/* Codes 1 and 2 differ in both bits. */
#define CT_FW_PORT_CODE_OTHER(code) ((code) ^ CT_FW_META_PORT_MASK)
#define CT_FW_META_ZONE_SHIFT 7u
#define CT_FW_META_ZONE_MASK (0xFFFFFF80u)
#define CT_FW_MAX_ZONE_ID (CT_FW_META_ZONE_MASK >> CT_FW_META_ZONE_SHIFT)
/* Zone bits [23:12]: the inner VLAN ID. */
#define CT_FW_META_INNER_VID_SHIFT (CT_FW_META_ZONE_SHIFT + CT_FW_VLAN_VID_BITS)
#define CT_FW_META_INNER_VID_MASK (CT_FW_VLAN_VID_MASK << CT_FW_META_INNER_VID_SHIFT)
#define CT_FW_MAX_VLAN_PAIR ((CT_FW_VLAN_VID_MASK << CT_FW_VLAN_VID_BITS) | CT_FW_VLAN_VID_MASK)
_Static_assert(CT_FW_MAX_ZONE_ID >= CT_FW_MAX_VLAN_PAIR, "room for an outer and an inner VLAN ID");

/* Zone of a host-order pkt_meta value. */
static inline uint32_t ct_fw_meta_zone(uint32_t pkt_meta)
{
	return (pkt_meta & CT_FW_META_ZONE_MASK) >> CT_FW_META_ZONE_SHIFT;
}

/* NAT action templates, one per L4 type. */
#define CT_FW_ACT_IDX_TCP 0u
#define CT_FW_ACT_IDX_UDP 1u
#define CT_FW_ACT_NB 2u

/* All-ones in a template: the value is set per entry. */
#define CT_FW_ACT_PORT_CHANGEABLE UINT16_MAX
#define CT_FW_ACT_ADDR_CHANGEABLE UINT32_MAX
#define CT_FW_ACT_META_CHANGEABLE UINT32_MAX
#define CT_FW_MATCH_FIELD_CHANGEABLE UINT16_MAX
#define CT_FW_MATCH_META_CHANGEABLE UINT32_MAX

/* Syndrome: state [7:6], flags [1:0]. */
#define CT_FW_SYN_STATE_FLAGS_MASK \
	(DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_STATE_MASK | \
	 DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_BAD_PACKET | \
	 DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_STATE_CHANGE)
#define CT_FW_SYN_STATE_BAD_MASK \
	(DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_STATE_MASK | \
	 DOCA_FLOW_CT_TCP_WINDOW_VALIDATION_PKT_SYNDROME_BAD_PACKET)

enum ct_fw_family {
	CT_FW_FAMILY_IPV4 = 0,
	CT_FW_FAMILY_IPV6 = 1,
};
#define CT_FW_NB_FAMILIES 2

enum ct_fw_sync_mode {
	CT_FW_ASYNC = 0,
	CT_FW_SYNC = 1, /**< --sync */
};

/* --dnat / --snat; an all-zero address is not translated. */
struct ct_fw_nat {
	bool enabled;
	uint8_t new_src[CT_FW_IPV6_ADDR_BYTES]; /**< --snat, network order */
	uint8_t new_dst[CT_FW_IPV6_ADDR_BYTES]; /**< --dnat, network order */
};

/* True when the address is not all-zero. */
static inline bool ct_fw_nat_addr_set(const uint8_t *addr, enum ct_fw_family family)
{
	size_t len = (family == CT_FW_FAMILY_IPV6) ? CT_FW_IPV6_ADDR_BYTES : CT_FW_IPV4_ADDR_BYTES;
	size_t i;

	for (i = 0; i < len; i++)
		if (addr[i] != 0)
			return true;
	return false;
}

/* Hashed byte-wise: padding is explicit and zeroed. */
struct ct_fw_key {
	enum ct_fw_family family;
	uint8_t protocol;
	uint8_t pad[3];
	uint32_t zone;
	uint8_t src[16];
	uint8_t dst[16];
	uint16_t src_port; /**< host order */
	uint16_t dst_port; /**< host order */
};

#define CT_FW_WS_DISABLED 0xFu /* no window scale */

/* TCP header fields a profile is built from (host order). */
struct ct_fw_tcp_obs {
	uint32_t seq;
	uint32_t ack;
	uint16_t window;
	uint16_t payload_len;
	uint8_t flags;
	uint8_t ws;
	bool sack_seen;
};

enum ct_fw_conn_state {
	CT_FW_CONN_HALF_OPEN = 0, /**< SYN seen */
	CT_FW_CONN_COUNTING,	  /**< --offload-after: counting bytes */
	CT_FW_CONN_INSTALLING,	  /**< install in flight */
	CT_FW_CONN_ADOPTING,	  /**< adoption in flight */
	CT_FW_CONN_OFFLOADED,	  /**< live in hardware */
	CT_FW_CONN_ECHO,	  /**< ping under NAT, software only */
};

enum ct_fw_install_mode {
	CT_FW_INSTALL_HANDSHAKE = 0, /**< at the SYN-ACK */
	CT_FW_INSTALL_ADOPT,	     /**< mid-flow, liberal */
	CT_FW_INSTALL_THRESHOLD,     /**< after --offload-after bytes */
	CT_FW_INSTALL_UDP,	     /**< no profile */
};

#define CT_FW_COUNT_DIR_ORIGIN 0x1u
#define CT_FW_COUNT_DIR_REPLY 0x2u
#define CT_FW_COUNT_DIR_BOTH (CT_FW_COUNT_DIR_ORIGIN | CT_FW_COUNT_DIR_REPLY)

/* A failed install's entry still owed; the sweep retries. */
enum ct_fw_entry_owed {
	CT_FW_OWED_NONE = 0,
	CT_FW_OWED_ROLLBACK, /**< never added: rollback owed */
	CT_FW_OWED_REMOVE,   /**< added: removal owed */
};

/* Window-validation operation in flight. */
enum ct_fw_wv_step {
	CT_FW_WV_NONE = 0,
	CT_FW_WV_PENDING_ADD, /**< install's profile write */
	CT_FW_WV_QUERY,	      /**< re-enable query */
	CT_FW_WV_UPDATE,      /**< re-enable update */
	CT_FW_WV_ADD_OWED,    /**< add owed after its profile write: the CT queue had no room */
};

struct ct_fw_conn {
	struct ct_fw_key key;
	struct doca_flow_pipe_entry *entry;
	enum ct_fw_conn_state state;
	enum ct_fw_install_mode install_mode;
	uint16_t ct_queue;     /**< queue that allocated the entry */
	uint64_t bytes_origin; /**< --offload-after byte counts */
	uint64_t bytes_reply;
	uint8_t counted_dirs;	     /**< CT_FW_COUNT_DIR_* with a baseline */
	struct ct_fw_tcp_obs syn;    /**< origin baseline */
	struct ct_fw_tcp_obs synack; /**< reply baseline */
	uint32_t origin_port_code;   /**< ingress port code of the origin */
	uint64_t last_seen_tsc;	     /**< last software-handled packet */
	uint64_t op_tsc;	     /**< last install or re-enable submitted */
	uint64_t backlog_burst;	     /**< burst a re-enable or mid-stream install completed in */
	uint64_t backlog_rx;	     /**< packets received when it completed, then when it settled */
	uint64_t backlog_tsc;	     /**< timer cycles when it completed */
	bool backlog_settled;	     /**< CT_FW_BACKLOG_SETTLE_US passed since then */
	bool reenable_owed;	     /**< a disabled entry may not be re-enabled yet */
	bool wv_op_inflight;	     /**< window-validation operation or owed add outstanding */
	enum ct_fw_wv_step wv_step;
	uint16_t wv_buf;		  /**< wv_slots index of the outstanding operation, any step */
	bool reuse_pending;		  /**< a new SYN: remove the entry if the query finds it closed */
	bool removing;			  /**< async: removal posted; the slot is held until it completes */
	uint8_t fin_dirs;		  /**< sides closed by an in-sequence FIN */
	bool fin_seen;			  /**< any FIN: never installed */
	enum ct_fw_entry_owed entry_owed; /**< failed install's entry still owed back */
	bool remove_requested;		  /**< removal owed */
	struct rte_mbuf *held_clone;	  /**< held packet: reinject clone or SYN-ACK */
	bool held_forward;		  /**< held_clone is a SYN-ACK, forwarded once live */
	bool free_pending;		  /**< release queued */
	bool in_use;
};

struct ct_fw_config;
struct rte_hash;

/* Per-worker connection pool indexed by hash position. */
struct ct_fw_conn_table {
	struct rte_hash *hash;	     /**< key -> conns[] position */
	struct rte_hash *reply_hash; /**< NAT only: return tuple -> conns[] position */
	struct ct_fw_conn *conns;    /**< stable pool */
	uint32_t capacity;	     /**< conns[] length */
	uint32_t max_load;	     /**< connection limit */
	uint32_t count;
	const struct ct_fw_config *cfg;
	uint64_t full_log_tsc;	     /**< last table-full warning */
	uint64_t reply_full_log_tsc; /**< last reply-index-full warning */
	uint16_t worker_id;	     /**< owning worker, for the warning */
};

struct ct_fw_stats {
	uint64_t rx_packets;
	uint64_t tx_packets;
	uint64_t conns_installed;
	uint64_t conns_adopted;
	uint64_t conns_threshold_installed; /**< --offload-after installs */
	uint64_t conns_udp_installed;
	uint64_t conns_reenabled;    /**< window validation re-enabled */
	uint64_t conns_aged;	     /**< entries removed (reset or aging) */
	uint64_t conns_sw_expired;   /**< released by the idle sweep */
	uint64_t aged_rm_failed;     /**< aged entries not removed */
	uint64_t reinjections;	     /**< replay clones sent */
	uint64_t pool_exhausted;     /**< drops for lack of room */
	uint64_t frag_drops;	     /**< fragments dropped, each counted once */
	uint64_t frag_reassembled;   /**< fragmented datagrams forwarded */
	uint64_t state_drops;	     /**< TCP state drops */
	uint64_t wrong_uplink_drops; /**< tracked tuple seen on the other uplink */
	uint64_t reuse_drops;	     /**< SYNs for a tuple whose entry is not yet removed */
	uint64_t parse_drops;	     /**< unparsable frames */
	uint64_t other_forwarded;    /**< ARP, ICMP and ICMPv6 forwarded */
	uint64_t install_failures;   /**< installs and removals completed with a failure (not --sync profile writes) */
	uint64_t install_drops;	     /**< drops while installing or after a failed install */
	uint64_t nat_other_drops;    /**< untranslatable ARP/ICMP; a second --snat client */
	/* Hit counters of removed entries, per direction. */
	uint64_t conn_origin_pkts;
	uint64_t conn_origin_bytes;
	uint64_t conn_reply_pkts;
	uint64_t conn_reply_bytes;
	uint64_t install_retries; /**< installs whose add waited for CT queue room after the profile write */
	uint64_t wv_limit_drops;  /**< handshake SYN-ACKs dropped: window-validation operations at the limit */
	uint64_t wv_limit_defers; /**< mid-flow segments whose install was deferred, same limit */
	uint64_t sweep_lagged;	  /**< sweep rounds that started with slots still owed */
};

/* One writer per counter, so relaxed atomics suffice. */
#define CT_FW_STAT_INC(field) \
	__atomic_store_n(&(field), __atomic_load_n(&(field), __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED)
#define CT_FW_STAT_ADD(field, v) \
	__atomic_store_n(&(field), __atomic_load_n(&(field), __ATOMIC_RELAXED) + (v), __ATOMIC_RELAXED)

/* valid, state_change, invalid, disabled */
#define CT_FW_NB_SYNDROME_VERDICTS 4

struct ct_fw_pipes {
	struct doca_flow_pipe *ingress_root;
	struct doca_flow_pipe *zone;
	struct doca_flow_pipe *zone_inner;
	struct doca_flow_pipe *ct;
	struct doca_flow_pipe *syndrome_check;
	struct doca_flow_pipe *miss_handler;
	struct doca_flow_pipe *port_forward;
	struct doca_flow_pipe *rss_to_cpu;
	struct doca_flow_pipe *rss_v4;
	struct doca_flow_pipe *rss_v6;	     /**< NULL without IPv6 CT */
	struct doca_flow_pipe *rss_l3_split; /**< NULL without IPv6 CT */
	struct doca_flow_pipe *egress_root;
	struct doca_flow_pipe_entry *syn_entries[CT_FW_NB_SYNDROME_VERDICTS]; /**< per verdict */
	uint32_t nb_syn_entries;
	struct doca_flow_pipe_entry *fwd_entries[CT_FW_MAX_PORTS]; /**< per output port */
	uint32_t nb_fwd_entries;
	struct doca_flow_pipe_entry *egress_entries[CT_FW_MAX_PORTS]; /**< per destination */
	uint32_t nb_egress_entries;
	struct doca_flow_pipe_entry *egress_drop_entry; /**< transmit with no destination */
	struct doca_flow_pipe_entry *ingress_tcp_entry; /**< diagnostic */
	struct doca_flow_pipe_entry *reinject_drop_entry;
	struct doca_flow_pipe_entry *miss_mark_drop_entry;
	struct doca_flow_pipe_entry *ct_miss_entry;
	struct doca_flow_pipe_entry *ipv6_drop_entry;
};

struct ct_fw_ctx;
struct ct_fw_worker;

/* Inputs of one CT entry submit. */
struct ct_fw_submit_ctx {
	struct doca_flow_ct_match *match_origin;
	struct doca_flow_ct_match *match_reply;
	struct doca_flow_ct_actions *act_origin;
	struct doca_flow_ct_actions *act_reply;
	const struct doca_flow_ct_tcp_window_validation *prof; /**< NULL when window validation is off */
	uint32_t hash_origin;
	uint32_t hash_reply;
	uint32_t prepare_flags;
	uint32_t entry_flags;
	uint32_t timeout_s;
};

/* A window-validation operation: set at submit, status at completion. */
struct ct_fw_pending_wv {
	struct ct_fw_key key;
	struct doca_flow_pipe_entry *entry;
	enum ct_fw_wv_step step;
	doca_error_t status;
	struct doca_flow_ct_tcp_window_validation profile; /**< query output */
};

enum ct_fw_wv_slot_state {
	CT_FW_WV_SLOT_FREE = 0,
	CT_FW_WV_SLOT_INFLIGHT, /**< submitted, no completion yet */
	CT_FW_WV_SLOT_READY,	/**< completed; on wv_ready */
	CT_FW_WV_SLOT_ADD_OWED, /**< profile written, add refused for room; on add_owed */
	CT_FW_WV_SLOT_ORPHAN,	/**< operation of a released connection; freed by its completion */
};

/* Held from submission until the completion is consumed, so completions never outnumber slots. */
struct ct_fw_wv_slot {
	struct ct_fw_pending_wv rec;
	uint64_t submit_tsc; /**< for the slot watchdog */
	uint8_t state;	     /**< enum ct_fw_wv_slot_state */
	bool abandoned;	     /**< owner released while the slot is on a ring */
};

/* FIFO of slot indices; a slot is on at most one ring, so a failed push is a broken invariant and stops. */
struct ct_fw_slot_ring {
	uint16_t idx[CT_FW_WV_POOL_SIZE];
	uint16_t head;
	uint16_t count;
};

/* Next step after a window-validation completion. */
enum ct_fw_wv_action {
	CT_FW_WV_ACT_NONE = 0,
	CT_FW_WV_ACT_UPDATE,  /**< submit the re-enable update */
	CT_FW_WV_ACT_ADD,     /**< add the entry */
	CT_FW_WV_ACT_RELEASE, /**< give the install up */
	CT_FW_WV_ACT_REMOVE,  /**< closed entry, new SYN: remove it */
};

/* An IPv4 ARP request remembered under NAT. */
struct ct_fw_arp_pending {
	uint32_t zone;
	rte_be32_t client; /**< asking host */
	rte_be32_t asked;  /**< address it asked for */
	uint64_t tsc;	   /**< when it was asked; kept until CT_FW_ARP_PENDING_S after that */
	bool answered;	   /**< a reply was sent for it */
	bool in_use;
};

/* The --snat client host of one zone. */
struct ct_fw_snat_owner {
	uint32_t zone;
	struct rte_ether_addr mac;
	rte_be32_t ip;	     /**< target of the server's ARP for --snat: the owner's latest ARP */
	uint64_t ip_log_tsc; /**< last address-change log */
	bool in_use;
};

/* Address translation state of ARP and ICMP. */
struct ct_fw_nat_other {
	struct ct_fw_arp_pending pending[CT_FW_ARP_PENDING];   /**< client's requests under --dnat */
	struct ct_fw_arp_pending own_asked[CT_FW_ARP_PENDING]; /**< server side asking for a --snat client */
	struct ct_fw_snat_owner owner[CT_FW_MAX_SNAT_OWNERS];
	uint64_t refuse_log_tsc; /**< last refusal warning */
	uint64_t fail_log_tsc;	 /**< last resource-failure warning */
};

typedef doca_error_t (*ct_fw_submit_fn)(struct ct_fw_worker *worker,
					struct ct_fw_conn *conn,
					const struct ct_fw_submit_ctx *ctx);

/* Fragmented datagram key. */
struct ct_fw_frag_key {
	uint32_t zone;
	uint32_t port_code;
	uint32_t src; /**< network order */
	uint32_t dst; /**< network order */
	uint16_t ip_id;
	uint8_t proto;
	uint8_t pad; /**< zero */
};

/* One held fragment: L4 bytes [off, end). */
struct ct_fw_frag {
	struct rte_mbuf *m;
	uint16_t off;
	uint16_t end;
};

struct ct_fw_frag_dgram {
	struct ct_fw_frag_key key;
	uint64_t first_tsc;
	uint16_t l4_total; /**< CT_FW_FRAG_L4_TOTAL_UNKNOWN until the last fragment arrives */
	uint8_t nb;
	bool in_use;
	struct ct_fw_frag frag[CT_FW_FRAG_MAX_PER_DGRAM]; /**< sorted by off */
};

struct ct_fw_frag_table {
	struct ct_fw_frag_dgram d[CT_FW_FRAG_MAX_DGRAMS];
	uint32_t nb_in_use;
	uint64_t full_log_tsc; /**< last table-full warning */
	uint64_t cap_log_tsc;  /**< last too-many-fragments warning */
	uint64_t len_log_tsc;  /**< last too-long warning */
	uint64_t buf_log_tsc;  /**< last reassembly-buffer warning */
};

/* A fragment's checked header fields. */
struct ct_fw_frag_info {
	struct ct_fw_frag_key key;
	uint16_t off; /**< L4 byte offset */
	uint16_t end;
	bool last;	   /**< more-fragments flag clear */
	bool short_first;  /**< offset 0 without a whole L4 header */
	uint32_t over_len; /**< nonzero: the IP length past CT_FW_FRAG_MAX_LEN that refused it */
};

enum ct_fw_frag_result {
	CT_FW_FRAG_HELD = 0, /**< kept, datagram incomplete */
	CT_FW_FRAG_COMPLETE, /**< kept, datagram complete */
	CT_FW_FRAG_DROPPED,  /**< freed and counted */
};

struct ct_fw_worker {
	struct ct_fw_ctx *app;
	uint16_t worker_id;
	uint16_t lcore;
	uint16_t ct_queue; /**< nb_queues + worker_id */
	uint16_t rss_queue;
	struct ct_fw_conn_table table;
	struct entries_status ct_status;
	uint32_t ct_queue_room; /**< lower bound of free queue room */
	struct ct_fw_stats stats;
	uint32_t counting;	    /**< COUNTING connections */
	uint64_t counting_log_tsc;  /**< last counting-limit warning */
	uint64_t replay_log_tsc;    /**< last replay-list-full warning */
	uint64_t owed_rm_log_tsc;   /**< last owed-removal-list-full warning */
	uint64_t wv_limit_log_tsc;  /**< last window-validation-slots-at-limit warning */
	uint64_t sweep_lag_log_tsc; /**< last sweep-behind warning */
	uint64_t failure_log_tsc;   /**< last failed-completion warning */
	uint64_t offload_log_tsc;   /**< last failed-offload error */
	uint64_t query_log_tsc;	    /**< last failed re-enable query error */
	uint64_t reenable_log_tsc;  /**< last failed re-enable update error */
	uint64_t key_log_tsc;	    /**< last failed connection-key store */
	ct_fw_submit_fn submit;	    /**< sync or async install */
	uint64_t now_tsc;
	uint64_t rx_bursts;	     /**< receive bursts polled */
	uint64_t rx_drained;	     /**< last burst that emptied the receive queue */
	uint64_t rx_drained_tsc;     /**< timer cycles when that burst polled */
	uint64_t backlog_settle;     /**< CT_FW_BACKLOG_SETTLE_US in timer cycles */
	uint64_t sweep_deadline_tsc; /**< next sweep round */
	uint32_t sweep_cursor;
	uint32_t sweep_budget;				       /**< slots per round */
	uint32_t sweep_left;				       /**< slots still owed this round */
	struct ct_fw_key pending_free[CT_FW_MAX_PENDING_FREE]; /**< queued slot releases */
	uint32_t nb_pending_free;
	struct doca_flow_pipe_entry *owed_rm[CT_FW_MAX_PENDING_FREE]; /**< idle entries without a connection to remove
								       */
	uint32_t nb_owed_rm;
	struct doca_flow_pipe_entry *sync_entry; /**< --sync: entry whose completion is awaited */
	enum doca_flow_entry_op sync_op;	 /**< --sync: the awaited operation */
	bool sync_done;				 /**< --sync: it completed */
	bool sync_ok;				 /**< --sync: it completed successfully */
	struct ct_fw_wv_slot wv_slots[CT_FW_WV_POOL_SIZE];
	uint16_t wv_free[CT_FW_WV_POOL_SIZE]; /**< stack of free wv_slots indices */
	uint16_t nb_wv_free;
	uint16_t nb_wv_orphans;					   /**< ORPHAN slots */
	struct ct_fw_slot_ring wv_ready;			   /**< completions for the drain */
	struct ct_fw_slot_ring add_owed;			   /**< adds waiting for CT queue room, oldest first */
	struct ct_fw_key pending_replay[CT_FW_MAX_PENDING_REPLAY]; /**< clones to replay */
	uint32_t nb_pending_replay;
	struct ct_fw_frag_table frags;
	struct ct_fw_nat_other nat_other;
	uint32_t echo_conns; /**< ECHO connections */
} __rte_cache_aligned;

/* Command line, read-only after parse. */
struct ct_fw_config {
	enum ct_fw_sync_mode sync_mode;
	bool enable_window_validation;
	bool liberal_mode;			 /**< --liberal */
	uint8_t max_ack_window;			 /**< --max-ack-window */
	uint32_t offload_after;			 /**< --offload-after; 0 = at the SYN-ACK */
	struct ct_fw_nat nat[CT_FW_NB_FAMILIES]; /**< per address family */
	uint16_t nb_ct_queues;			 /**< worker count */
	uint32_t nb_sessions;			 /**< --sessions, per family */
	uint32_t pool_size;			 /**< --pool-size */
	uint32_t aging_timeout_s;		 /**< --aging-timeout */
	uint32_t stats_interval;		 /**< --stats-interval, 0 = off */
	struct flow_switch_ctx switch_ctx;
};

/* NAT applies only to connections opened from the first uplink. */
static inline const struct ct_fw_nat *ct_fw_nat_for(const struct ct_fw_config *cfg,
						    enum ct_fw_family family,
						    uint32_t origin_port_code)
{
	static const struct ct_fw_nat untranslated = {0};

	return origin_port_code == CT_FW_PORT_CODE(CT_FW_FIRST_UPLINK) ? &cfg->nat[family] : &untranslated;
}

/* The family --dnat/--snat translate; false without NAT. */
static inline bool ct_fw_nat_family(const struct ct_fw_config *cfg, enum ct_fw_family *family)
{
	if (cfg->nat[CT_FW_FAMILY_IPV4].enabled)
		*family = CT_FW_FAMILY_IPV4;
	else if (cfg->nat[CT_FW_FAMILY_IPV6].enabled)
		*family = CT_FW_FAMILY_IPV6;
	else
		return false;
	return true;
}

static inline const struct ct_fw_nat *ct_fw_conn_nat(const struct ct_fw_config *cfg, const struct ct_fw_conn *conn)
{
	return ct_fw_nat_for(cfg, conn->key.family, conn->origin_port_code);
}

/* An IPv4 --snat address applies to what enters on this uplink (the --snat owner is IPv4 only). */
static inline bool ct_fw_snat_enabled(const struct ct_fw_config *cfg, uint32_t port_code)
{
	return ct_fw_nat_addr_set(ct_fw_nat_for(cfg, CT_FW_FAMILY_IPV4, port_code)->new_src, CT_FW_FAMILY_IPV4);
}

struct ct_fw_ctx {
	struct ct_fw_config config;
	struct doca_flow_port *ports[CT_FW_MAX_PORTS];
	uint16_t nb_ports;
	uint16_t nb_queues; /**< DPDK queue count */
	struct ct_fw_pipes pipes;
	struct ct_fw_worker workers[CT_FW_MAX_WORKERS];
	uint16_t nb_workers;
	bool has_ipv6_ct;
	volatile sig_atomic_t *force_quit; /**< set by the signal handler */
	int failed;			   /**< a CT queue failed: workers stop; state kept until teardown */
};

doca_error_t ct_fw_register_params(void);
/* Each logs the flag on a reject. */
doca_error_t ct_fw_config_check_int_range(const char *name, int value, uint32_t min, uint32_t max);
/* --dnat (dst) or --snat address. */
doca_error_t ct_fw_config_parse_nat_addr(const char *name, const char *literal, bool dst, struct ct_fw_config *cfg);
/* Cross-flag checks. */
doca_error_t ct_fw_config_validate(const struct ct_fw_config *cfg);

doca_error_t ct_fw_pipeline_build(struct ct_fw_ctx *app);
void ct_fw_pipeline_destroy(struct ct_fw_ctx *app);

/* cfg must outlive the table. */
doca_error_t ct_fw_conn_table_init(struct ct_fw_conn_table *table, uint32_t capacity, const struct ct_fw_config *cfg);
void ct_fw_conn_table_fini(struct ct_fw_conn_table *table);
struct ct_fw_conn *ct_fw_conn_lookup(struct ct_fw_conn_table *table, const struct ct_fw_key *key);
/* An existing key is returned unchanged; a key with a queued release is reopened through ct_fw_conn_open(). */
struct ct_fw_conn *ct_fw_conn_insert(struct ct_fw_conn_table *table, const struct ct_fw_key *key);
void ct_fw_conn_remove(struct ct_fw_conn_table *table, struct ct_fw_conn *conn);
/* Connection of a translated return tuple, or NULL. */
struct ct_fw_conn *ct_fw_conn_lookup_reply(struct ct_fw_conn_table *table, const struct ct_fw_key *reply_key);
/* Index a translated return tuple; needs origin_port_code set. */
void ct_fw_conn_reply_index_add(struct ct_fw_conn_table *table, const struct ct_fw_conn *conn);
/* True when the return tuple is the plain reverse. */
bool ct_fw_conn_reply_is_plain_reverse(const struct ct_fw_conn_table *table, const struct ct_fw_conn *conn);
/* True while a failed install's entry is still owed back. */
static inline bool ct_fw_conn_retained(const struct ct_fw_conn *conn)
{
	return conn->entry_owed != CT_FW_OWED_NONE && conn->entry != NULL;
}

static inline uint32_t ct_fw_unconfirmed_timeout_s(uint32_t aging_timeout_s)
{
	return aging_timeout_s < CT_FW_UNCONFIRMED_TIMEOUT_S ? aging_timeout_s : CT_FW_UNCONFIRMED_TIMEOUT_S;
}

/* Slots per sweep round so a full pass ends within limit_s. */
static inline uint32_t ct_fw_sweep_budget(uint32_t capacity, uint32_t limit_s)
{
	return 2 * ((capacity + limit_s - 1) / limit_s);
}

/* Worker thread only; free_later is safe from a callback. */
void ct_fw_conn_free_later(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
void ct_fw_conn_release(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
void ct_fw_conn_drain_pending_free(struct ct_fw_worker *worker);
/* The connection of key, new or existing; a slot whose release is queued is cleaned up and reused in place. */
struct ct_fw_conn *ct_fw_conn_open(struct ct_fw_worker *worker, const struct ct_fw_key *key);
/* Return tuple of a connection opened with key. */
void ct_fw_conn_nat_reply_key(const struct ct_fw_config *cfg,
			      const struct ct_fw_key *key,
			      uint32_t origin_port_code,
			      struct ct_fw_key *rkey);
/* Sweep round start: check the slot watchdog, then owe one round's slots (carried debt counts in sweep_lagged). */
void ct_fw_conn_sweep_round(struct ct_fw_worker *worker, uint64_t now_tsc, uint64_t watchdog_cycles);
/* Examine up to CT_FW_SWEEP_SLICE owed slots: expire idle connections; retry owed entries. */
void ct_fw_conn_sweep(struct ct_fw_worker *worker,
		      uint64_t now_tsc,
		      uint64_t idle_cycles,
		      uint64_t unconfirmed_cycles,
		      uint64_t watchdog_cycles);
/* Return a failed install's entry; false while it is still owed. */
bool ct_fw_conn_return_entry(struct ct_fw_worker *worker, struct ct_fw_conn *conn, enum ct_fw_entry_owed owed);
/* False: the removal is owed (remove_requested) or, async, posted and not yet completed (removing). */
bool ct_fw_conn_teardown_entry(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
/* Async: hold the slot and its entry until the posted removal completes (watchdog-bounded). */
void ct_fw_conn_removal_posted(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
/* Remove the entry and add its hit counters once. */
doca_error_t ct_fw_rm_entry_counted(struct ct_fw_worker *worker, uint16_t queue, struct doca_flow_pipe_entry *entry);
void ct_fw_conn_release_clone(struct ct_fw_conn *conn);
/* Hand off the held packet once. */
struct rte_mbuf *ct_fw_conn_take_held(struct ct_fw_conn *conn);
/* Free the held packet; a SYN-ACK counts in install_drops. */
void ct_fw_conn_drop_held(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
/* Latch a failed CT queue and stop. */
doca_error_t ct_fw_fail_stop(struct ct_fw_worker *worker, const char *what, doca_error_t result);
static inline bool ct_fw_failed(const struct ct_fw_worker *worker)
{
	return __atomic_load_n(&worker->app->failed, __ATOMIC_RELAXED) != 0;
}
/* An entry went live: data and FIN already queued for software are still dropped. */
void ct_fw_backlog_arm(const struct ct_fw_worker *worker, struct ct_fw_conn *conn);
/* A re-enable landed: segments already queued for software are still dropped. */
void ct_fw_reenable_landed(const struct ct_fw_worker *worker, struct ct_fw_conn *conn);
/* Returns the next step of a completed window-validation operation. */
enum ct_fw_wv_action ct_fw_wv_apply(struct ct_fw_worker *worker,
				    struct ct_fw_conn *conn,
				    const struct ct_fw_pending_wv *rec,
				    struct doca_flow_ct_tcp_window_validation *prof);
/* One direction's addresses after NAT. */
void ct_fw_nat_translate(const struct ct_fw_config *cfg,
			 const struct ct_fw_conn *conn,
			 bool reply,
			 uint8_t out_src[CT_FW_IPV6_ADDR_BYTES],
			 uint8_t out_dst[CT_FW_IPV6_ADDR_BYTES]);

/* New payload bytes a segment adds; retransmissions add none. */
uint32_t ct_fw_obs_advance(struct ct_fw_tcp_obs *obs, const struct ct_fw_tcp_obs *seg, bool established);
/* True when the segment starts where the baseline ends. */
bool ct_fw_obs_is_next(const struct ct_fw_tcp_obs *obs, const struct ct_fw_tcp_obs *seg);
/* True when the install is due. */
bool ct_fw_conn_count_segment(struct ct_fw_conn *conn,
			      const struct ct_fw_tcp_obs *seg,
			      bool is_reply,
			      uint32_t offload_after);
/* True when both sides closed (LAST_ACK or TIME_WAIT). */
bool ct_fw_profile_closed(const struct doca_flow_ct_tcp_window_validation *prof);
/* End the step; its slot is freed when its completion arrives or is popped. */
void ct_fw_wv_abandon(struct ct_fw_worker *worker, struct ct_fw_conn *conn);

/* THRESHOLD and ADOPT seed from the last_dir_reply direction. */
void ct_fw_profile_build(const struct ct_fw_config *cfg,
			 const struct ct_fw_conn *conn,
			 enum ct_fw_install_mode mode,
			 bool last_dir_reply,
			 struct doca_flow_ct_tcp_window_validation *prof);

void ct_fw_build_match(const struct ct_fw_ctx *app,
		       const struct ct_fw_conn *conn,
		       struct doca_flow_ct_match *match_origin,
		       struct doca_flow_ct_match *match_reply);
/* Build the add-entry inputs; false when there are no actions. */
bool ct_fw_build_add_inputs(const struct ct_fw_ctx *app,
			    const struct ct_fw_conn *conn,
			    struct doca_flow_ct_match *match_origin,
			    struct doca_flow_ct_match *match_reply,
			    struct doca_flow_ct_actions *act_origin,
			    struct doca_flow_ct_actions *act_reply,
			    uint32_t *entry_flags);

doca_error_t ct_fw_submit_sync(struct ct_fw_worker *worker,
			       struct ct_fw_conn *conn,
			       const struct ct_fw_submit_ctx *ctx);
/* Leaves the connection HALF_OPEN; the caller drops the packet. */
doca_error_t ct_fw_submit_on_error(struct ct_fw_worker *worker, struct ct_fw_conn *conn, doca_error_t result);
/* Log a refused or failed install: an error unless the refusal is expected. */
void ct_fw_log_offload_failure(struct ct_fw_worker *worker, doca_error_t result);
/* Store the connection's key in its entry; false if the entry's private data cannot be read. */
bool ct_fw_conn_store_key(struct ct_fw_worker *worker, const struct ct_fw_conn *conn);
/* Log a failed re-enable query or update; AGAIN is retried quietly. */
void ct_fw_log_reenable_failure(struct ct_fw_worker *worker, bool query, doca_error_t result);
/* Count one CT operation that completed with a failure; rate-limited warning. */
void ct_fw_note_failed_completion(struct ct_fw_worker *worker);
/* True at most once a second per timestamp (rate-limits a warning). */
bool ct_fw_log_due(uint64_t now_tsc, uint64_t *last_tsc);
/* --sync: wait for this operation's completion; BAD_STATE if it failed, TIME_OUT once stopped. */
doca_error_t ct_fw_sync_wait(struct ct_fw_worker *worker,
			     struct doca_flow_pipe_entry *entry,
			     enum doca_flow_entry_op op);
/* Queue room accounting, worker thread only. */
void ct_fw_queue_poll(struct ct_fw_worker *worker);
bool ct_fw_queue_make_room(struct ct_fw_worker *worker);
static inline void ct_fw_queue_posted(struct ct_fw_worker *worker)
{
	worker->ct_queue_room = worker->ct_queue_room >= CT_FW_ENTRY_OPS ? worker->ct_queue_room - CT_FW_ENTRY_OPS : 0;
}
/* The add is posted once the profile write completes. */
doca_error_t ct_fw_submit_async(struct ct_fw_worker *worker,
				struct ct_fw_conn *conn,
				const struct ct_fw_submit_ctx *ctx);
/* ct_fw_cb_init must run before any entry is processed. */
void ct_fw_cb_init(struct ct_fw_ctx *app);
void ct_fw_entry_cb(struct doca_flow_pipe_entry *entry,
		    uint16_t pipe_queue,
		    enum doca_flow_entry_status status,
		    enum doca_flow_entry_op op,
		    void *user_ctx);
/* Records only; never submits. */
void ct_fw_async_wv_comp_cb(struct doca_flow_pipe *pipe,
			    uint16_t queue,
			    doca_error_t status,
			    struct doca_flow_pipe_entry *entry,
			    struct doca_flow_ct_tcp_window_validation *profile,
			    void *user_ctx);
/* reuse: a new SYN; remove the entry if the query finds it closed. */
void ct_fw_conn_reenable_async(struct ct_fw_worker *worker, struct ct_fw_conn *conn, bool reuse);
void ct_fw_worker_async_init(struct ct_fw_worker *worker);
/* A free slot; an install also leaves CT_FW_WV_REENABLE_RESERVE to re-enables. */
bool ct_fw_wv_slot_admit(const struct ct_fw_worker *worker, bool install);
/* A TCP install with window validation may start (always with --sync); a refusal counts in *refused and warns. */
bool ct_fw_install_admitted(struct ct_fw_worker *worker, uint64_t *refused);
/* Window-validation slots in use. */
static inline uint32_t ct_fw_wv_ops_in_use(const struct ct_fw_worker *worker)
{
	return CT_FW_WV_POOL_SIZE - worker->nb_wv_free;
}
/* Stop if a slot's operation is older than watchdog_cycles. */
void ct_fw_wv_slot_watchdog(struct ct_fw_worker *worker, uint64_t now_tsc, uint64_t watchdog_cycles);
/* Post the adds owed for CT queue room, oldest first; an RST cancels one. */
void ct_fw_retry_owed_adds(struct ct_fw_worker *worker);
/* Retry the removals owed for idle entries no connection owns (--sync waits for each). */
void ct_fw_retry_owed_rm(struct ct_fw_worker *worker);
void ct_fw_wv_drain(struct ct_fw_worker *worker);

doca_error_t ct_fw_conn_install(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
doca_error_t ct_fw_conn_install_udp(struct ct_fw_worker *worker, struct ct_fw_conn *conn);
/* --offload-after install (THRESHOLD or ADOPT). */
doca_error_t ct_fw_conn_install_midstream(struct ct_fw_worker *worker,
					  struct ct_fw_conn *conn,
					  enum ct_fw_install_mode mode,
					  bool last_dir_reply);
void ct_fw_conn_count_installed(struct ct_fw_worker *worker, const struct ct_fw_conn *conn);
/* --sync; queried is the profile already read, or NULL; rewritten set if re-enabled. */
doca_error_t ct_fw_conn_reenable(struct ct_fw_worker *worker,
				 struct ct_fw_conn *conn,
				 const struct doca_flow_ct_tcp_window_validation *queried,
				 bool *rewritten);
doca_error_t ct_fw_conn_query_profile(struct ct_fw_worker *worker,
				      struct ct_fw_conn *conn,
				      struct doca_flow_ct_tcp_window_validation *prof);

/* Timer intervals of the worker loop, in timer cycles. */
struct ct_fw_loop_cycles {
	uint64_t idle;	      /**< --aging-timeout */
	uint64_t unconfirmed; /**< unanswered connection attempts */
	uint64_t sweep;	      /**< between sweep rounds */
	uint64_t watchdog;    /**< an operation this old stops the application */
	uint64_t frag;	      /**< fragment reassembly timeout */
};

int ct_fw_worker_loop(void *arg);
/* After a receive burst: releases, completions, one sweep slice and, when due, the aging round; false once stopped. */
bool ct_fw_worker_service(struct ct_fw_worker *worker, const struct ct_fw_loop_cycles *cyc);
/* Process one received packet. */
void ct_fw_process_packet(struct ct_fw_worker *worker, struct rte_mbuf *m);
/* Act on queued completions. */
void ct_fw_drain_completions(struct ct_fw_worker *worker);
/* Rewrite the addresses and adjust the checksums; false if the packet is truncated or its protocol not translated. */
bool ct_fw_rewrite_addrs(struct rte_mbuf *m, const uint8_t *src, const uint8_t *dst);
/* RFC 1624 checksum update; a UDP zero is kept. */
uint16_t ct_fw_cksum_adjust_addr(uint16_t cksum,
				 const uint8_t *old_src,
				 const uint8_t *old_dst,
				 const uint8_t *new_src,
				 const uint8_t *new_dst,
				 size_t addr_len,
				 bool udp);
/* True for an IPv4 fragment. */
bool ct_fw_ipv4_is_fragment(uint16_t fragment_offset_be);

/* False: the fragment is refused on its own. */
bool ct_fw_frag_parse(const struct rte_mbuf *m, uint32_t port_code, struct ct_fw_frag_info *info);
/* Takes m; *dg is set when COMPLETE. */
enum ct_fw_frag_result ct_fw_frag_insert(struct ct_fw_frag_table *table,
					 const struct ct_fw_frag_info *info,
					 struct rte_mbuf *m,
					 uint64_t now_tsc,
					 uint64_t *drops,
					 struct ct_fw_frag_dgram **dg);
/* Free and count the datagram's fragments. */
void ct_fw_frag_drop(struct ct_fw_frag_table *table, struct ct_fw_frag_dgram *dg, uint64_t *drops);
/* End a datagram the caller took. */
void ct_fw_frag_done(struct ct_fw_frag_table *table, struct ct_fw_frag_dgram *dg);
/* Free every held fragment; worker stopped. */
void ct_fw_frag_flush(struct ct_fw_frag_table *table);
/* Drop datagrams older than timeout_cycles. */
uint32_t ct_fw_frag_expire(struct ct_fw_frag_table *table, uint64_t now_tsc, uint64_t timeout_cycles, uint64_t *drops);
/* The datagram as one packet, or NULL. */
struct rte_mbuf *ct_fw_frag_wrap(const struct ct_fw_frag_dgram *dg, struct rte_mempool *pool, uint32_t *over_len);

/* Handle an ARP/ICMPv4 frame (ct_fw_frame_is_other()) under IPv4 NAT; false if not translated. */
bool ct_fw_nat_other(struct ct_fw_worker *worker, struct rte_mbuf *m);
/* --snat client check of a new connection's first packet: false for a host other than the zone's owner, or a new
 * zone when every owner slot is taken. */
bool ct_fw_snat_owner_allowed(struct ct_fw_worker *worker,
			      uint32_t zone,
			      const struct rte_ether_addr *mac,
			      rte_be32_t src_ip);
/* The first admitted client owns --snat: the zone's owner, or NULL when every owner slot is taken. */
struct ct_fw_snat_owner *ct_fw_snat_owner_claim(struct ct_fw_worker *worker,
						uint32_t zone,
						const struct rte_ether_addr *mac,
						rte_be32_t src_ip);

/* A parsed punted frame (host order). */
struct ct_fw_parsed_pkt {
	struct ct_fw_key key; /**< as seen on this packet */
	uint8_t tcp_flags;
	uint32_t seq;
	uint32_t ack;
	uint16_t window;
	uint16_t payload_len;
	uint8_t ws;
	bool sack_seen;
	bool is_tcp;
	bool is_fragment;
	bool from_fragments; /**< reassembled from IPv4 fragments */
	bool valid;
};

/* False for a fragment, malformed or untracked frame. */
bool ct_fw_parse_packet(struct rte_mbuf *m, uint32_t zone, bool ipv6_ct, struct ct_fw_parsed_pkt *out);

/* How a packet ends; HELD and CONSUMED are not the caller's. */
enum ct_fw_outcome {
	CT_FW_FORWARD = 0,	    /**< translated if nat, then sent */
	CT_FW_FORWARD_UNTRANSLATED, /**< sent unchanged */
	CT_FW_DROP,		    /**< freed, drop_ctr counted */
	CT_FW_HELD,		    /**< held until the entry is live */
	CT_FW_CONSUMED,		    /**< already sent or freed */
};

/* A packet's verdict, carried out by ct_fw_emit(). */
struct ct_fw_verdict {
	enum ct_fw_outcome outcome;
	bool nat;			    /**< translate to src/dst */
	bool was_live;			    /**< entry live when judging began */
	uint8_t src[CT_FW_IPV6_ADDR_BYTES]; /**< FORWARD with nat: source address to write */
	uint8_t dst[CT_FW_IPV6_ADDR_BYTES]; /**< FORWARD with nat: destination address to write */
	uint64_t *drop_ctr;		    /**< DROP: counter to add to, or NULL */
};

/* Decide a parsed packet. */
void ct_fw_judge_packet(struct ct_fw_worker *worker,
			struct rte_mbuf *m,
			const struct ct_fw_parsed_pkt *pkt,
			struct ct_fw_verdict *v);
/* Carry out a verdict; every uplink-bound packet leaves here, except a clone replayed through CT. */
void ct_fw_emit(struct ct_fw_worker *worker, struct rte_mbuf *m, const struct ct_fw_verdict *v);
/* A bare SYN opens; an ACK without SYN, FIN or RST is adopted. */
bool ct_fw_tcp_opener_allowed(uint8_t tcp_flags, bool *is_new_syn);
/* SYN and ACK set, no FIN or RST. */
bool ct_fw_tcp_synack_allowed(uint8_t tcp_flags);
/* ARP, ICMP or ICMPv6 (not an IPv4 fragment). */
bool ct_fw_frame_is_other(const uint8_t *data, const uint8_t *data_end);
/* Accepts ISN + 1 up to ISN + 1 + the SYN's payload. */
bool ct_fw_synack_acks_syn(const struct ct_fw_tcp_obs *syn, uint32_t ack);
/* A reply RST must ACK the SYN; an opener's must be in range. */
bool ct_fw_halfopen_rst_ok(const struct ct_fw_tcp_obs *syn, const struct ct_fw_parsed_pkt *pkt, bool is_reply);
/* A same-ISN retransmission keeps the larger payload. */
void ct_fw_syn_refresh(struct ct_fw_tcp_obs *syn, const struct ct_fw_tcp_obs *seg);
/* L3 after at most two 802.1Q tags. */
bool ct_fw_eth_l3(const uint8_t *data, const uint8_t *data_end, uint16_t *ether_type, const uint8_t **l3);
/* Ingress uplink code stamped on a received frame. */
uint32_t ct_fw_mbuf_port_code(struct rte_mbuf *m);
/* Zone of a frame; false if its tags are unsupported. */
bool ct_fw_frame_zone(const uint8_t *data, const uint8_t *data_end, uint32_t *zone);
/* A queued release is a miss. */
struct ct_fw_conn *ct_fw_conn_find_bidir(struct ct_fw_conn_table *table,
					 const struct ct_fw_key *key,
					 uint32_t port_code,
					 bool *is_reply,
					 bool *wrong_uplink);

void ct_fw_stats_dump(const struct ct_fw_ctx *app);
void ct_fw_stats_accumulate(struct ct_fw_stats *total, const struct ct_fw_stats *w);
/* Main thread only. */
void ct_fw_stats_print_periodic(const struct ct_fw_ctx *app, uint64_t uptime_s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CT_FW_H_ */
