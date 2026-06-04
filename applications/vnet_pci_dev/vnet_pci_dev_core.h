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

/* Forward declarations for VNet API structures */
struct doca_devemu_virtio_offload_engine;
struct doca_devemu_vnet_offload_engine;
struct doca_devemu_vnet_rx_vq;
struct doca_devemu_vnet_tx_vq;
struct doca_devemu_vnet_ctrl_vq;
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
struct vnet_dbg_state {
	_Atomic time_t last_collection_time; /* Last time stats/counters were collected (5-sec throttle) */
	atomic_bool stats_in_progress;	     /* True when async stats population is in progress */
	atomic_bool counters_in_progress;    /* True when counters collection is in progress */
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
 * These fields are touched from PE1, PE2, and detached MQ start threads, so
 * keep them grouped and use atomics for cross-thread visibility.
 */
struct vnet_deferred_mq_state {
	atomic_bool initial_data_qps_deferred; /* True when first-stage bring-up starts CVQ only */
	atomic_bool first_ctrl_req_delay_done; /* True after initial-stage ctrl_req timing delay was applied */
	atomic_bool start_deferred;	       /* True when MQ expansion is delayed until enumeration settles */
	_Atomic uint16_t old_qps;	       /* Active QPs before deferred MQ expansion */
	_Atomic uint16_t new_qps;	       /* Target QPs requested by deferred MQ expansion */
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
	struct doca_devemu_vnet_rx_vq **rx_vqs; /* Dynamic RX VQ pointer array */
	struct doca_devemu_vnet_tx_vq **tx_vqs; /* Dynamic TX VQ pointer array */
	struct doca_devemu_vnet_ctrl_vq *cvq;	/* Control VQ */

	/* MQ state */
	uint16_t max_queue_pairs;	 /* Configured max QPs (size of rx_vqs/tx_vqs arrays) */
	_Atomic uint16_t num_active_qps; /* Currently active QPs (updated by MQ cmd) */
	struct vnet_deferred_mq_state deferred_mq;

	/* Feature negotiation */
	bool mq_feature_negotiated; /* True if F_CTRL_VQ && F_MQ negotiated */

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
	atomic_bool engine_enabled;		/* True when engine (and VQs) are enabled */
	atomic_bool shutting_down;		/* True during final cleanup to prevent callback interference */
	atomic_bool stop_stats_collection;	/* True to stop stats collection (during cleanup or reset) */
	atomic_bool cleanup_running;		/* True during cleanup - FEATURES_OK/DRIVER_OK must wait */
	atomic_bool initialization_in_progress; /* True during FEATURES_OK async init - not yet ready for deferred MQ
						   release */
	atomic_bool deferred_init_pending;	/* True when FEATURES_OK was skipped, init needed after cleanup */
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
	uint32_t num_ep;			       /* Number of endpoints to create */
	bool hotplug_mode;			       /* Hotplug mode: true for hotplug, false for static */
	enum vnet_lu_mode vnet_lu_mode;		       /* Live update mode */
	int tlp_core_idx;			       /* TLP progress core (-1 = auto) */
	int worker_core_idx;			       /* Worker core (-1 = auto) */
	int mq_core_idx;			       /* MQ start core (-1 = auto single core) */
};

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
 * @brief Execute MQ start QPs in a background thread
 *
 * Called from a detached thread to start additional queue pairs. The CVQ
 * ctrl_req has already been completed with VIRTIO_NET_OK before this is
 * called, so the host is not blocked waiting. This allows all devices to
 * start their QPs in parallel.
 *
 * @param[in] controller VNet controller
 * @param[in] old_qps Current number of active queue pairs
 * @param[in] new_qps Target number of active queue pairs
 * @return DOCA_SUCCESS on success, error otherwise
 */
doca_error_t vnet_pci_dev_execute_mq_start_qps(struct vnet_pci_dev_controller *controller,
					       uint16_t old_qps,
					       uint16_t new_qps);

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
