/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef VNET_PCI_DEV_CORE_H
#define VNET_PCI_DEV_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#include <doca_error.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include "vnet_pci_device.h"
#include "vnet_pci_dev_lu.h"
#include "vnet_virtio_types.h"

/* Opaque SDK virtqueue handle; full definition comes from the devemu headers
 * included by the .c files. */
struct doca_devemu_virtio_vq;

/* MAC address string length: "XX:XX:XX:XX:XX:XX\0" = 18 characters */
#define VNET_PCI_DEV_MAC_ADDR_STR_LEN 18
/** Maximum doca_pe_progress() iterations when draining pending completions */
#define VNET_PCI_DEV_PE_DRAIN_MAX_ITERATIONS 1000

/**
 * @brief Default VirtIO-net device features
 *
 * These features are advertised to the guest driver during feature negotiation.
 * Modify this macro to add/remove supported features without changing function code.
 *
 * Note: VIRTIO_F_VERSION_1, VIRTIO_F_ACCESS_PLATFORM, VIRTIO_NET_F_CTRL_VQ, and
 * VIRTIO_NET_F_MQ are always added by vnet_pci_device_init_internal().
 *
 * Supported features:
 *   - VIRTIO_NET_F_MAC: Device has given MAC address
 *   - VIRTIO_NET_F_STATUS: Link status available in config
 *   - VIRTIO_NET_F_CSUM: Host handles packets with partial checksum
 *   - VIRTIO_NET_F_MTU: Device maximum MTU reporting (required for jumbo frames)
 *   - VIRTIO_NET_F_SPEED_DUPLEX: Device reports speed and duplex in config
 *   - VIRTIO_NET_F_HOST_TSO4: Host can handle TSOv4 (TCP Segmentation Offload IPv4)
 *   - VIRTIO_NET_F_HOST_TSO6: Host can handle TSOv6 (TCP Segmentation Offload IPv6)
 */
#define VNET_PCI_DEV_DEFAULT_FEATURES \
	((1ULL << VIRTIO_NET_F_MAC) | (1ULL << VIRTIO_NET_F_STATUS) | (1ULL << VIRTIO_NET_F_CSUM) | \
	 (1ULL << VIRTIO_NET_F_MTU) | (1ULL << VIRTIO_NET_F_SPEED_DUPLEX) | (1ULL << VIRTIO_NET_F_HOST_TSO4) | \
	 (1ULL << VIRTIO_NET_F_HOST_TSO6))

/* Per-VF feature set, a subset of VNET_PCI_DEV_DEFAULT_FEATURES:
 *   - F_MAC: guest adopts the per-VF device-config MAC (the offload engine
 *     steers RX by it; without it inbound unicast is filtered out).
 *   - F_STATUS: guest reads link state from the VF device-config (reported
 *     link-up), so the netdev carrier reflects the device.
 *   - F_MTU: guest reads the VF max MTU from the device-config, so it clamps to
 *     the engine MTU instead of assuming an arbitrary maximum.
 *   - F_SPEED_DUPLEX: guest reads link speed/duplex, inherited from the owning
 *     PF (a VF shares the PF uplink).
 *   - F_CSUM + F_HOST_TSO4/TSO6: TX checksum and TCP segmentation offload,
 *     completed by the offload engine per-device (F_HOST_TSO* requires F_CSUM).
 *   - F_CTRL_VQ + F_MQ: control VQ to scale up to max_virtqueue_pairs.
 *   - VERSION_1 + ACCESS_PLATFORM: modern-virtio transport bits.
 * F_STATUS/F_MTU/F_SPEED_DUPLEX are served from the per-VF device-config
 * populated in vnet_pci_device_init_vf(). */
#define VNET_PCI_DEV_VF_DEFAULT_FEATURES \
	((1ULL << VNET_VIRTIO_F_VERSION_1) | (1ULL << VNET_VIRTIO_F_ACCESS_PLATFORM) | (1ULL << VIRTIO_NET_F_MAC) | \
	 (1ULL << VIRTIO_NET_F_STATUS) | (1ULL << VIRTIO_NET_F_MTU) | (1ULL << VIRTIO_NET_F_SPEED_DUPLEX) | \
	 (1ULL << VIRTIO_NET_F_CSUM) | (1ULL << VIRTIO_NET_F_HOST_TSO4) | (1ULL << VIRTIO_NET_F_HOST_TSO6) | \
	 (1ULL << VIRTIO_NET_F_CTRL_VQ) | (1ULL << VIRTIO_NET_F_MQ))

/* Forward declarations for VNet API structures */
struct doca_devemu_virtio_offload_engine;
struct doca_devemu_vnet_offload_engine;
struct doca_devemu_vnet_rx_vq;
struct doca_devemu_vnet_tx_vq;
struct doca_devemu_vnet_ctrl_vq;
struct doca_devemu_virtio_admin_vq;
struct doca_devemu_virtio_admin_req;
struct doca_devemu_vnet_io;
struct doca_devemu_vnet_counters;
struct doca_devemu_pci_msix;
struct doca_devemu_vnet_ctrl_req;

/**
 * @brief Debug state collection structure
 *
 * Encapsulates all state related to async debug state collection.
 * stats_in_progress and counters_in_progress are accessed from multiple threads
 * (main loop and cleanup) and must be atomic.
 */
#define VNET_STATS_COLLECTION_INTERVAL_SEC 10
#define VNET_STATS_FIRST_COLLECTION_DELAY_SEC 30

struct vnet_dbg_state {
	_Atomic time_t last_stats_time;	   /* Next eligible time for queue stats collection */
	_Atomic time_t last_counters_time; /* Last time virtqueue counters were collected (5-sec throttle) */
	atomic_bool stats_in_progress;	   /* True when async stats population is in progress */
	atomic_bool counters_in_progress;  /* True when counters collection is in progress */
};

/* Application resources */
struct vnet_pci_dev_resources {
	struct tlp_context *tlp_ctx;
	volatile bool *force_quit;
};

/**
 * @brief VirtIO queue statistics reference - bundled for atomic publication.
 *
 * Readers load the pointer atomically to get a consistent (state_list, list_len) pair.
 * Writers allocate a new struct, populate it, then atomically swap the pointer.
 */
struct vnet_stats_list_ref {
	struct doca_devemu_virtio_queue_dbg_state **state_list; /* Array of queue statistics */
	uint32_t list_len;					/* Number of statistics entries */
};

/**
 * @brief Deferred first-stage/MQ bring-up state.
 *
 * These fields are touched from PE1, PE2, and background MQ resize threads, so
 * keep them grouped and use atomics for cross-thread visibility.
 */
struct vnet_deferred_mq_state {
	atomic_bool initial_data_qps_deferred; /* True when first-stage bring-up starts CVQ only */
	atomic_bool first_ctrl_req_delay_done; /* True after initial-stage ctrl_req timing delay was applied */
	atomic_bool start_deferred;	       /* True when MQ resize is delayed until enumeration settles */
};

/**
 * @brief Per-controller MQ resize serialization state.
 *
 * One background worker converges the hardware-committed count in
 * controller->num_active_qps to desired_qps.
 */
struct vnet_mq_resize_state {
	pthread_mutex_t mutex;	  /* Guards desired_qps and worker_running */
	pthread_cond_t idle_cond; /* Broadcast when worker_running clears, so teardown can wait it out */
	uint16_t desired_qps;	  /* Queue pairs the host last asked for */
	bool worker_running;	  /* True from resize submit until the worker reports it stopped */
};

/**
 * @brief VNet controller structure
 *
 * Manages VNet offload engine with multi-queue support for hardware-accelerated
 * packet processing. Supports dynamic queue pair configuration via MQ commands.
 *
 * Key Design Principles:
 * - Multi-queue model: up to VNET_MAX_QUEUE_PAIRS data queue pairs
 * - Control VirtQueue (CVQ) is always present
 * - VQ arrays for scalable queue management
 * - Conditional CVQ creation based on feature negotiation (F_CTRL_VQ + F_MQ)
 * - Selective VQ start: only active QPs and CVQ which are started
 */
enum vnet_reset_status_state {
	VNET_RESET_STATUS_IDLE = 0,
	VNET_RESET_STATUS_HELD,
	VNET_RESET_STATUS_RELEASE_PENDING,
};

enum vnet_fatal_reason {
	VNET_FATAL_NONE = 0,  /* No fatal error is recorded */
	VNET_FATAL_MQ_RESIZE, /* Asynchronous MQ resize failed and requires a device reset */
};

/**
 * @brief Worker-reported fatal device error awaiting publication on PE1.
 *
 * latched and pending stay atomic so the PE1 progress loop and the ctrl_req path
 * can test them without locking. The descriptive fields are guarded by mutex,
 * which keeps a report from a worker thread indivisible with respect to the
 * reset-boundary clears issued by PE1 and by the cleanup thread.
 */
struct vnet_fatal_state {
	pthread_mutex_t mutex;	       /* Protects the descriptive fields */
	atomic_bool latched;	       /* Blocks new work until the device resets */
	atomic_bool pending;	       /* PE1 has not yet claimed the report */
	uint32_t generation;	       /* Device reset generation that owns this record */
	enum vnet_fatal_reason reason; /* Operation category that caused the fatal error */
	doca_error_t result;	       /* Underlying DOCA error returned by that operation */
};

struct vnet_pci_dev_controller {
	/* Links to existing components */
	_Atomic(struct vnet_pci_device *) virtio_device; /* Associated VirtIO device */
	struct tlp_context *tlp_ctx;			 /* Parent TLP context (PE, global lock, etc) */

	/* VNet offload engine */
	struct doca_devemu_vnet_offload_engine *offload_engine; /* Hardware offload engine */

	/* VQ arrays - dynamically allocated based on configured max_queue_pairs
	 * Each array has max_queue_pairs slots (allocated at controller creation)
	 * RX VQs: VQ0, VQ2, VQ4, ... (even indices)
	 * TX VQs: VQ1, VQ3, VQ5, ... (odd indices)
	 */
	struct doca_devemu_vnet_rx_vq **rx_vqs;	      /* Dynamic RX VQ pointer array */
	struct doca_devemu_vnet_tx_vq **tx_vqs;	      /* Dynamic TX VQ pointer array */
	struct doca_devemu_vnet_ctrl_vq *cvq;	      /* Control VQ */
	struct doca_devemu_virtio_admin_vq *admin_vq; /* Admin VQ */

	/* MQ state */
	uint16_t max_queue_pairs;	 /* Configured max QPs (size of rx_vqs/tx_vqs arrays) */
	_Atomic uint16_t num_active_qps; /* Hardware-committed active QPs */
	struct vnet_deferred_mq_state deferred_mq;
	struct vnet_mq_resize_state mq_resize;

	/* Feature negotiation */
	bool mq_feature_negotiated; /* True if F_CTRL_VQ && F_MQ negotiated */
	bool admin_vq_negotiated;   /* True if VIRTIO_F_ADMIN_VQ negotiated */
	uint16_t admin_queue_index;
	uint16_t admin_queue_num;

	/* IO context for CVQ command handling */
	struct doca_devemu_vnet_io *io_ctx; /* IO context for ctrl_req forwarding */
	bool io_ctx_started;		    /* True when io_ctx is started */

	/* Dual PE architecture:
	 * - PE1 (tlp_ctx->pe): Main thread, TLP handling - time critical
	 * - PE2 (worker_pe): Worker thread, heavy operations - can take time
	 * Worker thread drives its own PE, eliminating cross-thread PE dependency. */
	struct doca_pe *worker_pe; /* PE2: Worker thread drives this for async ops */

	/* State tracking - atomic for multi-thread access (main loop, PCI cfg workqueue) */
	atomic_bool offload_engine_started;	/* True when offload engine is started */
	atomic_bool vqs_initialized;		/* True when VQ structures are initialized */
	atomic_bool cvq_bound;			/* True when CVQ is bound to IO context */
	atomic_bool admin_vq_bound;		/* True when AQ is bound to IO context */
	atomic_bool engine_enabled;		/* True when engine (and VQs) are enabled */
	atomic_bool shutting_down;		/* True during final cleanup to prevent callback interference */
	atomic_bool stop_stats_collection;	/* True to stop stats collection (during cleanup or reset) */
	atomic_bool cleanup_running;		/* True during cleanup - FEATURES_OK/DRIVER_OK must wait */
	atomic_bool initialization_in_progress; /* True during FEATURES_OK async init - not yet ready for deferred MQ
						   release */
	atomic_bool deferred_init_pending;	/* True when FEATURES_OK was skipped, init needed after cleanup */
	struct vnet_fatal_state fatal_error;	/* Worker-reported error published by PE1 and latched until reset */
	atomic_uint reset_status_state;		/* enum vnet_reset_status_state */

	/* Config change MSI-X handle for raising config-change interrupts towards the host.
	 * Created on-demand when first config change needs it (e.g., speed change).
	 * Survives reset -- destroyed only on full teardown or if the host changes the vector. */
	struct doca_devemu_pci_msix *config_msix;
	uint16_t config_msix_vector_cached; /* vector used to create config_msix (for change detection) */

	/* VNET virtqueue traffic counters */
	struct doca_devemu_vnet_counters *vnet_counters; /* VNET virtqueue counters handle */

	/* VirtIO queue statistics - see struct vnet_stats_list_ref definition above.
	 * stats_ref_mutex serializes concurrent access between the PE1 stats
	 * collection thread (reader via trylock) and the workqueue thread that
	 * creates/swaps the stats list on DRIVER_OK.
	 * Contract: readers must hold the mutex for the entire duration they use
	 * stats_ref, because the writer swaps the pointer under the mutex but
	 * frees the old stats list after unlocking. */
	pthread_mutex_t stats_ref_mutex;		 /* Protects stats_ref read/swap */
	_Atomic(struct vnet_stats_list_ref *) stats_ref; /* Atomic pointer to stats list reference */

	/* Debug state collection (per-controller for multi-EP support) */
	struct vnet_dbg_state dbg_state; /* Debug state collection */

	/* Per-controller log files (avoids race condition with global file handles) */
	FILE *stats_log_file;	 /* Log file for VQ stats (/tmp/vnet_stats_pfX.log) */
	FILE *counters_log_file; /* Log file for VQ counters (/tmp/vnet_counters_pfX.log) */
};

struct vnet_vf_slot;

/**
 * @brief Create the per-VF datapath offload engine for an SR-IOV VF slot.
 *
 * Binds a vnet offload engine to the VF's own started tlp_dev (the SR-IOV
 * analog of the PF controller's engine), configures it (MTU, per-VF MAC,
 * queue count) and leaves it idle. The engine is started later on the VF's
 * own FEATURES_OK once the VF MMIO datapath bridge is in place. Idempotent:
 * a no-op when the slot already has an engine.
 *
 * @param[in] tlp_ctx TLP context (MTU / MAC base / queue config).
 * @param[in,out] slot VF slot with a started tlp_dev; slot->offload_engine is
 *                     set on success.
 * @return DOCA_SUCCESS or an SDK error (slot->offload_engine left NULL).
 */
doca_error_t vnet_pci_dev_vf_engine_create(struct tlp_context *tlp_ctx, struct vnet_vf_slot *slot);

/**
 * @brief Destroy the per-VF datapath offload engine for an SR-IOV VF slot.
 *
 * Must be called before the VF tlp_dev is stopped/destroyed: a live engine
 * holds a refcount on the tlp_dev endpoint. Idempotent: a no-op when the slot
 * has no engine.
 *
 * @param[in,out] slot VF slot; slot->offload_engine is cleared on success.
 * @return DOCA_SUCCESS or an SDK error.
 */
doca_error_t vnet_pci_dev_vf_engine_destroy(struct vnet_vf_slot *slot);

/**
 * @brief LU-safe VF datapath quiesce without destroying the offload engine object.
 *
 * Mirrors PF vnet_controller_cleanup(..., false): disable/stop datapath
 * resources before handover while keeping the stopped OE handle for final
 * active-side cleanup.
 *
 * @param[in,out] slot VF slot to quiesce.
 * @return DOCA_SUCCESS or an SDK error.
 */
doca_error_t vnet_pci_dev_vf_lu_quiesce(struct vnet_vf_slot *slot);

/**
 * @brief Create the per-VF datapath virtqueues (RX/TX pairs + control VQ).
 *
 * The SR-IOV analog of vnet_pci_dev_initialize_vqs(): once the VF's offload
 * engine is started (on the VF's FEATURES_OK), create the VQ structures so the
 * guest's per-queue configuration can be applied. Idempotent: a no-op when the
 * slot's VQs are already created.
 *
 * @param[in] tlp_ctx TLP context (provides max_queue_pairs).
 * @param[in,out] slot VF slot with a started engine; VQ handles are stored on
 *                     the slot on success.
 * @return DOCA_SUCCESS or an SDK error (any partial VQs are rolled back).
 */
doca_error_t vnet_pci_dev_vf_vqs_create(struct tlp_context *tlp_ctx, struct vnet_vf_slot *slot);

/**
 * Configure one VF data virtqueue (RX = even vq_index, TX = odd) from its
 * cfg-space shadow, applying set_conf when the queue is fully configured.
 *
 * Shared by the DRIVER_OK start path and the deferred per-queue worker so both
 * apply identical logic. Control VQs (vq_index >= 2 * max_queue_pairs) are not
 * handled here.
 *
 * @param[in] slot VF slot with created VQ handles.
 * @param[in] vq_index Data VQ index.
 * @param[out] out_vq Set to the configured VQ handle, or NULL when the queue is
 *                    not ready / out of range (caller skips).
 * @param[out] out_snap Optional; receives the applied snapshot for logging.
 * @return DOCA_SUCCESS (incl. the not-ready skip) or an SDK error from set_conf.
 */
doca_error_t vnet_pci_dev_vf_set_conf_data_vq(struct vnet_vf_slot *slot,
					      uint16_t vq_index,
					      struct doca_devemu_virtio_vq **out_vq,
					      struct vnet_virtio_queue_config *out_snap);

/**
 * @brief Destroy the per-VF datapath virtqueues.
 *
 * Must run before the VF engine is stopped/destroyed. Idempotent: a no-op when
 * the slot has no VQs.
 *
 * @param[in,out] slot VF slot; VQ handles/arrays are cleared on success.
 * @return DOCA_SUCCESS or the first SDK error encountered.
 */
doca_error_t vnet_pci_dev_vf_vqs_destroy(struct vnet_vf_slot *slot);

/**
 * @brief Configure and start active VF VQs without enabling the VF engine.
 *
 * Used by LU Phase 1 to prepare VF VQs while App_A still serves traffic.
 *
 * @param[in,out] slot VF slot with created VQ handles and restored config shadow.
 * @return DOCA_SUCCESS or an SDK error.
 */
doca_error_t vnet_pci_dev_vf_start_vqs(struct vnet_vf_slot *slot);

/**
 * @brief Enable a prepared VF offload engine.
 *
 * Used by LU Phase 2 after DEV_GO.
 *
 * @param[in,out] slot VF slot with VQs already started.
 * @return DOCA_SUCCESS or an SDK error.
 */
doca_error_t vnet_pci_dev_vf_enable_engine(struct vnet_vf_slot *slot);

/**
 * @brief Start the per-VF virtqueues and enable the VF offload engine.
 *
 * Called on the VF's DRIVER_OK transition (off the TLP thread, via the config
 * workqueue). Applies and starts each fully-configured VQ, then enables the
 * offload engine so the VF datapath becomes operational. Idempotent and a
 * no-op until the guest has set DRIVER_OK; on any failure the started VQs are
 * stopped again so the slot teardown stays well-defined.
 *
 * @param[in,out] slot VF slot; slot->engine_enabled is set on success.
 * @return DOCA_SUCCESS or an SDK error.
 */
doca_error_t vnet_pci_dev_vf_start_and_enable(struct vnet_vf_slot *slot);

/**
 * @brief Create + start the per-VF control-VQ IO context (multi-queue only).
 *
 * Mirror of vnet_pci_dev_initialize_io_context() for an SR-IOV VF: when the VF
 * has negotiated F_CTRL_VQ + F_MQ (slot->mq_feature_negotiated), creates a
 * DOCA vnet IO context from the VF's offload engine, attaches it to the owning
 * PF controller's worker PE (PE2), registers the VF ctrl_req handler, and
 * starts it. No-op when MQ was not negotiated or the io_ctx already exists.
 *
 * @param[in] tlp_ctx TLP context (resolves the owning PF controller's worker PE).
 * @param[in] pf_endpoint Owning PF endpoint.
 * @param[in,out] slot VF slot; slot->io_ctx / io_ctx_started set on success.
 * @return DOCA_SUCCESS (incl. the MQ-not-negotiated no-op) or an SDK error.
 */
doca_error_t vnet_pci_dev_vf_initialize_io_context(struct tlp_context *tlp_ctx,
						   struct pci_device_config *pf_endpoint,
						   struct vnet_vf_slot *slot);

/**
 * @brief Application configuration structure
 *
 * Contains all configuration parameters for the VirtIO Net PCI device application.
 * Supports multi-queue configuration aligned with VirtIO specification.
 * Total VQs are calculated as: max_queue_pairs * 2 + 1 (RX + TX per pair + CVQ)
 */
struct vnet_pci_dev_config {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE];  /* Device PCI address (e.g., "0000:03:00.0") */
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* IB device name (e.g., "mlx5_bond_0_pci_dev") */
	char mac_addr[VNET_PCI_DEV_MAC_ADDR_STR_LEN];  /* MAC address string (e.g., "52:54:00:12:34:56") */
	uint16_t mtu;				       /* MTU size (typically 1500) */
	uint32_t speed;				       /* Network link speed in Mbps */
	uint8_t duplex;				       /* Duplex mode: 0=half, 1=full */
	uint16_t max_queue_pairs;		       /* Maximum queue pairs (supported by the device) */
	uint16_t queue_size;			       /* VirtQueue size (entries per queue, must be power of 2) */
	uint16_t admin_queue_num;		       /* Admin queue count (0 disables AQ) */
	uint32_t num_ep;			       /* Number of endpoints to create */
	bool hotplug_mode;			       /* Hotplug mode: true for hotplug, false for static */
	enum vnet_lu_mode vnet_lu_mode;		       /* Live update mode */
	int tlp_core_idx;			       /* TLP progress core (-1 = auto) */
	int worker_core_idx;			       /* Worker core (-1 = auto) */
	int mq_core_idx;			       /* MQ start core (-1 = auto single core) */
	uint64_t opt_features;			       /* Optional Virtio feature bits (filtered at init) */

	/* SR-IOV (up to 255 VFs per PF via ARI). sriov_enabled is derived from
	 * --total-vfs > 0; per-VF MSI-X/doorbells are derived from the queue
	 * count, not configured. */
	bool sriov_enabled;	  /* derived: sriov_total_vfs > 0 */
	uint16_t sriov_total_vfs; /* --total-vfs: TotalVFs (0 = off; 1..64 enables, per VNET_SRIOV_MAX_VFS_PER_PF) */
};

/* SR-IOV limits and defaults. With ARI advertised the PF places VFs as ARI
 * functions 1..N on its own bus (Device=0, 8-bit Function), so the single-bus
 * ceiling is function 255 (up to 255 VFs per PF). The actual maximum is further
 * clamped at runtime to the device's reported max_total_vfs (vnet_sriov.c). */
#define VNET_SRIOV_MAX_TOTAL_VFS 255
#define VNET_SRIOV_DEFAULT_TOTAL_VFS 0 /* SR-IOV off unless --total-vfs > 0 */
/* Single-bus ARI ceiling: the highest ARI Function Number on one bus is 255, so
 * first_vf_offset + (total_vf - 1) * vf_stride must not exceed 255. */
#define VNET_SRIOV_MAX_ARI_FUNCTION 255
/* Supported per-PF VF cap (validated envelope). Tighter than the 255 ARI
 * ceiling so the device-wide VF datapath footprint fits the firmware
 * VQ/MSI-X/doorbell pool; see VNET_SRIOV_MAX_TOTAL_VF_VQS. */
#define VNET_SRIOV_MAX_VFS_PER_PF 64
#define VNET_SRIOV_VF_NUM_MSIX(max_qp) (VNET_TOTAL_VQS(max_qp) + 1)
#define VNET_SRIOV_VF_NUM_DB(max_qp) (VNET_TOTAL_VQS(max_qp) + 1)
/* Device-wide VF datapath VQ budget = the supported reference envelope
 * 4 PF x 64 VF x 8 QP (= 4 * 64 * VNET_TOTAL_VQS(8) VQs), a conservative
 * fraction of the firmware emulated-VQ pool for ample headroom. validate_caps()
 * rejects any num_ep/total_vfs/qp whose footprint
 * (num_ep * total_vfs * VNET_TOTAL_VQS(max_qp)) exceeds it. Device-wide ceiling,
 * not per-function: lighter geometries may trade PFs for QPs within the same
 * budget (e.g. 2 PF x 64 VF x 16 QP also fits). */
#define VNET_SRIOV_REF_MAX_PF 4
#define VNET_SRIOV_REF_MAX_QP 8
#define VNET_SRIOV_MAX_TOTAL_VF_VQS \
	(VNET_SRIOV_REF_MAX_PF * VNET_SRIOV_MAX_VFS_PER_PF * VNET_TOTAL_VQS(VNET_SRIOV_REF_MAX_QP))

/* Device-wide cap on the aggregate VF count (num_ep * total_vfs). The controller
 * backend reserves 256 of its per-function engine slots for VFs (see
 * VNET_CTRL_MAX_NUM_ENGINES), so at most 256 VFs run device-wide -- a limit the
 * VQ budget above misses when many PFs use few QPs. */
#define VNET_SRIOV_MAX_TOTAL_VFS_ALL_PF 256

/**
 * @brief Initialize VQs according to library's embedded design
 *
 * Creates embedded RX and TX VQs as required by the library's fixed 2-queue
 * architecture. VQ configuration will be handled automatically by the engine
 * when the VirtIO device becomes ready. Must be called AFTER engine start
 * and BEFORE engine enable.
 *
 * @param[in] controller VNet controller
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
doca_error_t vnet_pci_dev_initialize_vqs(struct vnet_pci_dev_controller *controller);

/**
 * @brief Apply negotiated Virtio feature subset onto the offload engine
 *
 * Enables/disables each optional feature bit on the engine.
 * Must be called before engine start.
 *
 * @param[in] virtio_engine Virtio offload engine
 * @param[in] dev VirtIO device with driver_features already set
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
doca_error_t vnet_pci_dev_apply_negotiated_features(struct doca_devemu_virtio_offload_engine *virtio_engine,
						    struct vnet_pci_device *dev);

/**
 * @brief Validate the negotiated feature set against the device and the VirtIO spec.
 *
 * Confirms that the driver did not negotiate features the device never offered,
 * and that all VirtIO spec inter-feature dependencies are satisfied.
 *
 * @param[in] dev VirtIO device carrying driver_features (negotiated) and device_features (offered).
 * @param[out] accepted Set to true if the negotiated feature set is acceptable, false otherwise.
 */
void vnet_pci_dev_validate_features(struct vnet_pci_device *dev, bool *accepted);

/**
 * @brief Enable engine after VQs are properly configured
 *
 * Enables the offload engine after VQs have been configured with host-provided
 * addresses. VQs will be implicitly started and enabled by the engine.
 *
 * @param[in] controller VNet controller
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
doca_error_t vnet_pci_dev_enable_engine(struct vnet_pci_dev_controller *controller);

/**
 * @brief Configure and start active VQs (vq_set_conf + vq_start) without enable.
 * Used by LU Phase 1 to prepare VQs while App_A still serves traffic.
 */
doca_error_t vnet_pci_dev_start_vqs(struct vnet_pci_dev_controller *controller, struct vnet_pci_device *dev);

/**
 * @brief Configure, start all active VQs and enable offload engine
 *
 * Calls vnet_pci_dev_start_vqs() followed by engine enable and counter creation.
 *
 * @param[in] controller VNet controller
 * @param[in] dev VNet PCI device
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
doca_error_t vnet_pci_dev_start_and_enable(struct vnet_pci_dev_controller *controller, struct vnet_pci_device *dev);

/**
 * @brief Handle VirtIO-net control virtqueue request
 *
 * @param[in] req ctrl_req object containing command data
 * @param[in] cls VirtIO control command class
 * @param[in] cmd VirtIO control command
 * @param[in] user_data User context (controller pointer)
 */
void vnet_pci_dev_ctrl_req_handler(struct doca_devemu_vnet_ctrl_req *req, uint8_t cls, uint8_t cmd, void *user_data);
void vnet_pci_dev_admin_req_handler(struct doca_devemu_virtio_admin_req *req,
				    uint16_t opcode,
				    uint16_t group_type,
				    uint64_t group_member_id,
				    void *user_data);

/**
 * @brief Apply VQ configuration to hardware
 *
 * This function applies VQ configuration (addresses, size, MSI-X vector) to the
 * underlying DOCA VQ handle using doca_devemu_virtio_vq_set_conf().
 *
 * @param[in] dev VirtIO device
 * @param[in] vq_index Virtual queue index to configure
 */
void vnet_pci_device_vq_config(struct vnet_pci_device *dev, uint16_t vq_index);

/**
 * @brief Cleanup VNet controller resources
 *
 * Performs proper DOCA API lifecycle cleanup:
 *   1. vq_disable() for each VQ (while engine ENABLED) - prevents blocking
 *   2. engine_disable() (fast - VQs already disabled)
 *   3. vq_stop() for each VQ (engine is DISABLED)
 *   4. vq_destroy() for each VQ
 *   5. engine_stop()
 *   6. engine_destroy() (only if destroy_engine is true)
 *
 * Unified cleanup function for VNet controller. Used for both device reset
 * (driver rebind) and application shutdown scenarios.
 *
 * @param[in] ctrl Controller to cleanup
 * @param[in] destroy_engine true for app shutdown, false for RESET (driver rebind)
 * @return DOCA_SUCCESS on success, error code on failure
 */
doca_error_t vnet_controller_cleanup(struct vnet_pci_dev_controller *ctrl, bool destroy_engine);

/**
 * @brief Initialize IO context for CVQ command handling
 *
 * Called during FEATURES_OK phase after VQs are initialized.
 *
 * @param[in] controller VNet controller
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
doca_error_t vnet_pci_dev_initialize_io_context(struct vnet_pci_dev_controller *controller);

/**
 * @brief Initialize and run VirtIO Net PCI Device application
 *
 * Main entry point for VirtIO Net PCI device logic. Performs device discovery,
 * initializes DOCA framework, creates VirtIO network devices, and runs
 * the main event processing loop until termination is requested.
 *
 * @param[in] config Application configuration containing PCI address, MAC, etc.
 * @param[in] force_quit Pointer to force quit flag for graceful shutdown
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 *
 * @note This function blocks until force_quit is set to true
 * @note Proper cleanup is performed regardless of exit reason
 * @note Created VirtIO device becomes visible after PCIe enumeration
 */
doca_error_t vnet_pci_dev_run(struct vnet_pci_dev_config *config, volatile bool *force_quit);

/**
 * @brief Destroy endpoint device during hotplug removal
 *
 * Destroys the VNet controller, TLP device, and representor for an endpoint.
 * Used by workqueue for delayed device destruction after host reset.
 *
 * @param[in] tlp_ctx TLP context
 * @param[in,out] endpoint Endpoint device configuration
 * @return DOCA_SUCCESS on success, error otherwise
 */
doca_error_t vnet_pci_dev_destroy_device(struct tlp_context *tlp_ctx, struct pci_device_config *endpoint);

/**
 * @brief Dump backing-SF representor info (vhca_id/ifindex/sf_index) for one PF and its VFs
 *
 * Serves the interactive 'sfinfo' command. Resolves each engine's SF representor
 * on demand; must run on the config worker (not the PE thread) since it performs
 * blocking control-plane queries.
 *
 * @param[in] tlp_ctx TLP context
 * @param[in] ep_index PF/endpoint index (0 .. num_ep-1)
 */
void vnet_dump_sf_info(struct tlp_context *tlp_ctx, uint32_t ep_index);

/**
 * @brief Execute hotplug event (device creation/destruction + slot status + MSI)
 *
 * Called from the worker thread to perform the full hotplug sequence without
 * blocking the main PE progress loop. This prevents PCIe Completion Timeouts
 * that occur when create_device() blocks the main thread for 350-400ms.
 *
 * @param[in] tlp_ctx TLP context
 * @param[in] dsp_index DSP slot index
 * @param[in] plug True for plug, false for unplug
 * @return DOCA_SUCCESS on success, error otherwise
 */
doca_error_t vnet_pci_dev_execute_hotplug(struct tlp_context *tlp_ctx, uint32_t dsp_index, bool plug);

/**
 * @brief Converge MQ queue pairs to the latest requested count
 *
 * Called from a joinable background thread after the CVQ request has been
 * accepted. Only one resize worker runs per controller, while different
 * controllers can resize in parallel.
 *
 * @param[in] controller VNet controller
 * @param[in] reset_generation Device reset generation captured at submission
 * @return DOCA_SUCCESS on success, error otherwise
 */
doca_error_t vnet_pci_dev_execute_mq_resize(struct vnet_pci_dev_controller *controller, uint32_t reset_generation);

/**
 * @brief Mark a submitted MQ resize worker as stopped
 *
 * Used when the thread is cancelled before entering the resize executor.
 *
 * @param[in] controller VNet controller
 */
void vnet_pci_dev_mq_resize_worker_stopped(struct vnet_pci_dev_controller *controller);

/**
 * @brief Report a fatal device error for publication on PE1
 *
 * Worker-side callers use this function to latch a fatal error. PE1 later
 * publishes DEVICE_NEEDS_RESET and notifies the host. The latch remains set
 * until the device is reset.
 *
 * @param[in] controller VNet controller
 * @param[in] reset_generation Device reset generation owning the error
 * @param[in] reason Fatal error reason
 * @param[in] result Error returned by the failed operation
 */
void vnet_controller_report_fatal_error(struct vnet_pci_dev_controller *controller,
					uint32_t reset_generation,
					enum vnet_fatal_reason reason,
					doca_error_t result);

/**
 * @brief Ensure config MSI-X is ready, creating or recreating as needed
 *
 * Compares the live config_msix_vector (written by the host driver) with the cached
 * value used to create the current MSI-X object. If the vector changed, destroys the
 * old MSI-X and creates a new one. If no MSI-X exists and the vector is valid, creates
 * one. Follows the same on-demand pattern as vBLK's vblk_pci_notify_host().
 *
 * @param[in] controller VNet controller
 * @return DOCA_SUCCESS on success, DOCA_ERROR_BAD_STATE if vector is 0xFFFF
 */
doca_error_t vnet_ensure_config_msix(struct vnet_pci_dev_controller *controller);

/**
 * @brief Execute speed change with config MSI-X notification to host
 *
 * Performs a spec-compliant speed change: link down -> update speed -> link up -> MSI-X.
 * Must be called from the worker thread (not the main PE progress thread).
 *
 * @param[in] controller VNet controller
 * @param[in] new_speed New speed in Mbps
 * @return DOCA_SUCCESS on success, DOCA_ERROR_BAD_STATE if config_msix not available
 */
doca_error_t vnet_pci_dev_execute_speed_change(struct vnet_pci_dev_controller *controller, uint32_t new_speed);

/**
 * @brief Collect stats/counters for one controller on worker-side idle time
 *
 * Called from the PCI config worker thread when it is otherwise idle so
 * diagnostics no longer run on the main PE1 progress loop.
 *
 * @param[in] controller VNet controller
 */
void vnet_pci_dev_worker_collect_diagnostics(struct vnet_pci_dev_controller *controller);

#endif
