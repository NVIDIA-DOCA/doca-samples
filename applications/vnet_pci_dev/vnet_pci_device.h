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

#ifndef VNET_PCI_DEVICE_H
#define VNET_PCI_DEVICE_H

#include <sys/queue.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include <doca_dev.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_devemu_pci_tlp.h>

#include "vnet_pcie_defs.h"
#include "vnet_virtio_types.h"
#include "pci_spec_tlp.h"
#include "vnet_recycle.h"

/**
 * @brief Initialize VNet PCI Device Framework
 * @param[in] dev DOCA device handle for the DPU
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t vnet_pci_dev_init(struct tlp_context *tlp_ctx);

/**
 * @brief Reset and cleanup VNet PCI Device Framework
 */
void vnet_pci_dev_reset(struct tlp_context *tlp_ctx);

/**
 * @brief Start VNet PCI Device TLP Channel
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t vnet_pci_dev_start(struct tlp_context *tlp_ctx);

/**
 * @brief Start VNet PCI Device TLP Channel from exported state (LU standby)
 * @param[in] tlp_ctx TLP context
 * @param[in] export_desc Export descriptor from active's channel export
 * @param[in] export_desc_len Length of export descriptor in bytes
 * @param[in] shm_dir_path Shared memory directory path for channel
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t vnet_pci_dev_start_from_export(struct tlp_context *tlp_ctx,
					    const void *export_desc,
					    size_t export_desc_len,
					    const char *shm_dir_path);

/**
 * @brief Stop VNet PCI Device TLP Channel
 */
void vnet_pci_dev_stop(struct tlp_context *tlp_ctx);

/**
 * @brief Get DOCA context for TLP channel
 * @return DOCA context pointer for progress engine integration
 */
struct doca_ctx *vnet_pci_dev_tlp_channel_ctx(struct tlp_context *tlp_ctx);

/**
 * @brief Pin the current thread as the time-sensitive main TLP progress thread
 */
void vnet_pci_device_pin_main_thread(void);

/**
 * @brief Configure optional explicit CPU affinity for VNet threads
 *
 * @param[in] tlp_core_idx TLP progress core, or -1 for automatic selection
 * @param[in] worker_core_idx PCI config worker core, or -1 for automatic selection
 * @param[in] mq_core_idx MQ start core, or -1 for automatic single-core selection
 */
void vnet_pci_device_configure_affinity(int tlp_core_idx, int worker_core_idx, int mq_core_idx);

/* Forward declaration for controller type */
struct vnet_pci_dev_controller;
struct vnet_pci_device;
struct vnet_admin_async_cmd;

/**
 * @brief Shutdown the PCI config workqueue
 *
 * Stops accepting new work items and waits for all pending work to complete.
 * Must be called before destroying any resources that work items may reference
 * (e.g., VirtIO devices with vqs arrays).
 */
void pci_cfg_workqueue_shutdown(void);

/**
 * @brief Submit controller cleanup to async workqueue (fire and forget)
 *
 * Uses pre-allocated pool - no memory allocation in hot path.
 * Silently drops if pool exhausted or workqueue shutting down.
 *
 * @param[in] controller Controller to cleanup
 * @param[in] destroy_engine true for app shutdown, false for RESET
 */
void pci_cfg_workqueue_submit_controller_cleanup(struct vnet_pci_dev_controller *controller, bool destroy_engine);

/**
 * @brief Submit one step of a PF's function recycle (fire and forget).
 *
 * Call only after vnet_recycle_needs_step() has granted the step slot. On false
 * the caller still owns that slot and must call vnet_recycle_release_step().
 *
 * @param[in] controller Controller whose PF is being recycled
 * @return true if the step was queued
 */
bool pci_cfg_workqueue_submit_recycle_step(struct vnet_pci_dev_controller *controller);

/**
 * @brief Submit one step of a VF's function recycle (fire and forget).
 *
 * The slot is identified by (PF, index) rather than by pointer: the VF slot array
 * is freed when SR-IOV is disabled, possibly while a step is queued. Same
 * step-slot contract as the PF version.
 *
 * @param[in] pf_endpoint Endpoint of the PF owning the VF
 * @param[in] slot_idx PF-relative VF slot index
 * @return true if the step was queued
 */
bool pci_cfg_workqueue_submit_vf_recycle_step(struct pci_device_config *pf_endpoint, uint16_t slot_idx);

/*
 * Host-visible register publication. A PF's header lives both in
 * pci_device_config::cfg_space_hdr and in the materialised dw_regs[] image the
 * host reads back; these setters make mutating and publishing one operation.
 */

/**
 * @brief Set the PCI Command register and publish it to the host-visible image.
 *
 * @param[in] dev VirtIO device owning the materialised config image
 * @param[in] ep Endpoint owning the header shadow (may be NULL)
 * @param[in] command New Command register value
 */
void vnet_pci_device_publish_command(struct vnet_pci_device *dev, struct pci_device_config *ep, uint16_t command);

/**
 * @brief Read the PCI Command register as the host currently sees it.
 *
 * @param[in] dev VirtIO device owning the materialised config image
 * @return The Command register value
 */
uint16_t vnet_pci_device_command_get(struct vnet_pci_device *dev);

/**
 * @brief Whether the host currently has Bus Master Enable set on this function.
 *
 * @param[in] dev VirtIO device owning the materialised config image
 * @return true if bus mastering is enabled
 */
bool vnet_pci_device_bus_master_is_set(struct vnet_pci_device *dev);

/**
 * @brief Return the function's host-visible config image to power-on defaults.
 *
 * Called on the reasons that reset the function architecturally (FLR, PERST#):
 * the host clears its own view of the Command register, BARs, MSI-X and the
 * SR-IOV capability, so the emulated image has to follow or it reads back stale.
 *
 * @param[in] dev VirtIO device owning the materialised config image
 * @param[in] ep Endpoint owning the header shadow (may be NULL)
 * @return true if this call took VF Enable down from set, which is the caller's
 *         cue to cascade the reset onto the VFs. A repeated reset finds VF Enable
 *         already clear and reports false.
 */
bool vnet_pci_device_publish_power_on_cfg(struct vnet_pci_device *dev, struct pci_device_config *ep);

/**
 * @brief Check that the published SR-IOV capability still matches its shadow.
 *
 * Debug self-check for the divergence class above: a mismatch means the host is
 * reading something other than what the application believes it published, which
 * is otherwise invisible from the DPU log.
 *
 * @param[in] dev VirtIO device owning the materialised config image
 * @param[in] ep Endpoint owning the SR-IOV shadow
 * @return true if the images agree, or if this endpoint has no SR-IOV
 */
bool vnet_pci_device_cfg_image_check(struct vnet_pci_device *dev, struct pci_device_config *ep);

/**
 * @brief Hide a PF's VFs after its reset cleared VF Enable.
 *
 * Call with the PF's endpoint_lock held for read.
 *
 * @param[in] ep Endpoint of the PF whose VFs are being hidden
 * @return true if the hide was queued; false leaves it owed to the caller
 */
bool vnet_pci_device_hide_pf_vfs(struct pci_device_config *ep);

/**
 * @brief Drop the host-visible reset hold, publishing device_status 0.
 *
 * @param[in] dev VirtIO device to release
 */
void vnet_pci_device_release_reset_hold(struct vnet_pci_device *dev);

/**
 * @brief Publish DEVICE_NEEDS_RESET so a waiting driver gives up and re-probes.
 *
 * Used when a recycle exhausts its retry budget. Without it the guest sits in
 * its FEATURES_OK wait until the virtio timeout fires, reporting a probe failure
 * with no indication the device asked to be reset.
 *
 * @param[in] dev VirtIO device to mark
 */
void vnet_pci_device_publish_needs_reset(struct vnet_pci_device *dev);

/**
 * @brief Submit engine start to async workqueue (fire and forget)
 * @param[in] controller VNet controller
 */
void pci_cfg_workqueue_submit_engine_start(struct vnet_pci_dev_controller *controller);

/**
 * @brief Submit VQs initialization to async workqueue (fire and forget)
 * @param[in] controller VNet controller
 */
void pci_cfg_workqueue_submit_initialize_vqs(struct vnet_pci_dev_controller *controller);

/**
 * @brief Submit IO context initialization to async workqueue (fire and forget)
 * @param[in] controller VNet controller
 */
void pci_cfg_workqueue_submit_initialize_io_context(struct vnet_pci_dev_controller *controller);

/**
 * @brief Submit start and enable to async workqueue (fire and forget)
 * @param[in] controller VNet controller
 * @param[in] dev VNet PCI device
 */
void pci_cfg_workqueue_submit_start_and_enable(struct vnet_pci_dev_controller *controller, struct vnet_pci_device *dev);

/**
 * @brief Submit stats list creation to async workqueue (fire and forget)
 * @param[in] controller VNet controller
 */
void pci_cfg_workqueue_submit_create_stats_list(struct vnet_pci_dev_controller *controller);

/**
 * @brief Log workqueue status for debugging
 * Logs current state of work pool and pending work count
 */
void pci_cfg_workqueue_log_status(void);

/**
 * @brief Set the worker PEs for async operations
 * Worker thread will use these PEs to drive DOCA async state machines.
 * Multi-endpoint mode creates per-controller worker PEs - we must progress
 * all of them to avoid stalling controllers 1..N.
 * @param[in] pes Array of worker progress engines (PE2s), one per controller
 * @param[in] count Number of worker PEs in the array
 */
void pci_cfg_workqueue_set_worker_pes(struct doca_pe **pes, uint32_t count);

/**
 * @brief Clear all worker PEs before destroying them
 * Must be called before destroying worker PEs to prevent use-after-free
 */
void pci_cfg_workqueue_clear_worker_pes(void);

/**
 * @brief Enable or disable worker-side diagnostics collection
 *
 * When enabled, the PCI config worker uses its idle timeout to run
 * stats/counters collection on one controller at a time instead of
 * keeping this work on the main PE1 progress loop.
 *
 * @param[in] tlp_ctx TLP context that owns the controllers to inspect
 * @param[in] enabled true to enable worker-side diagnostics, false to disable
 */
void pci_cfg_workqueue_set_diag_collection(struct tlp_context *tlp_ctx, bool enabled);

/**
 * @brief Send MSI via Memory Write TLP
 * @param[in] tlp_ctx TLP context
 * @param[in] dsp DSP device configuration
 * @return DOCA_SUCCESS on success, error otherwise
 */
doca_error_t send_msi_via_memory_write_tlp(struct tlp_context *tlp_ctx, struct pci_device_config *dsp);

/**
 * @brief VirtIO BAR Configuration
 *
 * Our VirtIO layout uses a single BAR for all capabilities.
 * Future enhancement could support multiple BARs for different device types.
 */
#define VNET_VIRTIO_BAR_ID 0	    /* Use BAR 0 for VirtIO capabilities */
#define VNET_VIRTIO_BAR_LOG_SIZE 15 /* 2^15 = 32KB - covers all VirtIO capabilities */

/**
 * @brief VNet MQ Configuration
 *
 * VNET_MAX_QUEUE_PAIRS: Default queue pairs if -q flag not specified
 * VNET_CTRL_MAX_QUEUES_PAIRS: Absolute hardware maximum (from vnet_ctrl library)
 * VNET_DEFAULT_QUEUE_PAIRS: Default active QPs for first-stage bring-up before MQ commands
 *
 * Runtime configuration: Set via -q flag (1 to VNET_CTRL_MAX_QUEUES_PAIRS)
 * VQ shadow array is now dynamically allocated based on actual num_queues
 */
#define VNET_MAX_QUEUE_PAIRS 8	       /* Default (not a hard limit!) */
#define VNET_CTRL_MAX_QUEUES_PAIRS 127 /* Absolute hardware max */
#define VNET_DEFAULT_QUEUE_PAIRS 1     /* Default active QPs for first-stage bring-up */

/* Computes the number of data VQs given the number of queue pairs.
 * Each queue pair consists of 2 data VQs (RX and TX). */
#define VNET_NUM_DATA_VQS(num_qp) ((num_qp)*2)

/* CVQ index calculation: max_queue_pairs * 2 (RX+TX per QP) */
#define VNET_CVQ_INDEX(max_qp) ((max_qp)*2)

/* Total VQs = all data queue pairs + 1 CVQ. The admin VQ follows the CVQ, so it
 * takes that index, and VQ_SLOTS is the full count: data pairs + CVQ + admin. */
#define VNET_TOTAL_VQS(max_qp) (VNET_CVQ_INDEX(max_qp) + 1)
#define VNET_ADMIN_VQ_INDEX(max_qp) VNET_TOTAL_VQS(max_qp)
#define VNET_TOTAL_VQ_SLOTS(max_qp, admin_qs) (VNET_TOTAL_VQS(max_qp) + (admin_qs))

#define VNET_DEFAULT_QUEUE_SIZE 1024 /* Default queue size */
#define VNET_MIN_QUEUE_SIZE 32	     /* Minimum queue size */
#define VNET_MAX_QUEUE_SIZE 4096     /* Maximum queue size */
#define VNET_VF_CVQ_MAX_MAC_ENTRIES 64
#define VNET_VF_CVQ_MAX_VLAN_ENTRIES 4096
#define VNET_VF_CVQ_VLAN_BITMAP_WORDS (VNET_VF_CVQ_MAX_VLAN_ENTRIES / 64)

/**
 * @brief VirtIO Device Memory Layout (BAR 0)
 *
 * All offsets and sizes for VirtIO capabilities within the device BAR.
 * Layout follows VirtIO specification recommendations for optimal access.
 */

/* Common PCI Configuration Space */
#define VNET_VIRTIO_PCI_CFG_OFFSET 0x0000 /* Start of BAR */
#define VNET_VIRTIO_PCI_CFG_LEN 0x100	  /* 256 bytes for common config */

/* Interrupt Status Register */
#define VNET_VIRTIO_ISR_CFG_OFFSET 0x100 /* After common config */
#define VNET_VIRTIO_ISR_CFG_LEN 0x1	 /* Single byte for ISR */

/* Device-Specific Configuration */
#define VNET_VIRTIO_DEV_CFG_OFFSET 0x200 /* Network device config area */
#define VNET_VIRTIO_DEV_CFG_LEN 0x100	 /* 256 bytes for device config */

/* MSI-X Configuration */
#define VNET_PCI_DEV_NUM_MSIX 256	     /* Number of MSI-X vectors supported */
#define VNET_PCI_DEV_NUM_DB 256		     /* Number of doorbells supported */
#define VNET_PCI_DEV_MAX_ADMIN_QUEUES 1	     /* Max Admin VQs per PF (backend-limited) */
#define VNET_VIRTIO_MSIX_TABLE_OFFSET 0x2000 /* MSI-X table location */
#define VNET_VIRTIO_MSIX_TABLE_LEN 0x1000    /* 4KB for MSI-X table */
#define VNET_VIRTIO_MSIX_PBA_OFFSET 0x3000   /* MSI-X Pending Bit Array */
#define VNET_VIRTIO_MSIX_PBA_LEN 0x1000	     /* 4KB for MSI-X PBA */

/* VirtQueue Doorbell Configuration */
#define VNET_VIRTIO_DB_STRIDE 3	     /* Doorbell stride: 2^3 = 8 bytes between doorbells */
#define VNET_VIRTIO_DB_OFFSET 0x4000 /* Doorbell area start */
#define VNET_VIRTIO_DB_LEN 0x1000    /* 4KB doorbell area */

struct vnet_pci_device;

typedef void (*vnet_pci_device_status_change_cb_t)(struct vnet_pci_device *dev, void *arg);

struct vnet_pci_device {
	LIST_ENTRY(vnet_pci_device) entry;
	bool is_enumerated;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	int pf_index;
	/* Meaningful only when pf_index < 0, i.e. this device is a VF. Carried on
	 * the device so a log site handed nothing else can still say which
	 * function it means. */
	uint32_t vf_index;
	/* Per-device previous status for state tracking */
	uint8_t prev_status;
	/* Incremented on reset to reject stale async work */
	atomic_uint reset_generation;

	/* Per-device cancellation flag for long-running worker operations.
	 * Set by the main thread (DEVICE_RESET path) BEFORE the per-device barrier wait.
	 * Checked by worker thread in loops (e.g., start_additional_queue_pairs) to bail out
	 * early, bounding the maximum barrier wait to one sub-operation (~25-50ms) instead
	 * of the full operation duration (seconds for 64-127 QPs). */
	atomic_bool cancel_in_progress;

	/* emulated device with complete VirtIO capability chain */
	struct pcie_virtio_dev pcie_dev;

	vnet_pci_device_status_change_cb_t pci_cfg_change_cb;
	void *cb_arg;

	/* selectors */
	uint64_t device_features;
	uint64_t driver_features;

	/* vq shadow array - dynamically allocated based on num_queues.
	 *
	 * The cross-thread race the lock prevents:
	 *   Writer: main TLP thread doing memset(dev->vqs, ...) inside
	 *           vnet_pci_device_queue_init() (called by the reset path
	 *           after the 20 ms timed wait expires).
	 *   Reader: worker thread iterating dev->vqs[] in start_vqs /
	 *           start_and_enable / start_additional_queue_pairs / vq_config.
	 *
	 * NOT a race: the queue_select / shadow update / DRIVER_OK save sites
	 * inside vnet_pci_device_pci_cfg_write32() also touch dev->vqs[], but
	 * they execute on the same main TLP thread as the reset path and are
	 * therefore mutually serialised by the single TLP execution context.
	 *
	 * vqs_mutex is held only across the memcpy/memset itself - never across
	 * a DOCA / FW round-trip. Worker readers must use
	 * vnet_pci_device_snapshot_vqs() / vnet_pci_device_snapshot_vq() to
	 * take a consistent local copy. */
	struct vnet_virtio_queue_config *vqs;
	uint16_t vqs_count;
	pthread_mutex_t vqs_mutex;
	uint16_t queue_size; /* Configured VirtQueue size (entries per queue) */

	/* virtio pci config - fixme: static assert */
	struct vnet_virtio_common_config pci_cfg;

	/* device configs - network device only */
	union {
		struct vnet_virtio_net_config vnet_cfg;
		/* placeholder for network device configuration */
		char dev_cfg[sizeof(struct vnet_virtio_net_config)];
	};
};

/* fixme: inheritance from the base ep */
LIST_HEAD(vnet_pci_device_list, vnet_pci_device);

struct doca_devemu_vnet_offload_engine;
struct doca_devemu_vnet_rx_vq;
struct doca_devemu_vnet_tx_vq;
struct doca_devemu_vnet_ctrl_vq;
struct doca_devemu_vnet_counters;
struct doca_devemu_pci_msix;
struct doca_devemu_vnet_io;

/*
 * VirtIO migration shadows only: the current vnet backend does not yet enforce
 * MAC/VLAN filter tables, but their CVQ state is retained for device-parts LM.
 */
struct vnet_vf_cvq_state {
	uint64_t vlan_bitmap[VNET_VF_CVQ_VLAN_BITMAP_WORDS];
	uint8_t macs[VNET_VF_CVQ_MAX_MAC_ENTRIES][ETH_ALEN];
	uint32_t mac_entries;
	uint16_t vlan_entries;
};

/* How far a VF's guest-reset datapath teardown has got. The host stays held
 * for as long as this is OWED or QUEUED. */
enum vnet_vf_teardown_state {
	VNET_VF_TEARDOWN_IDLE = 0, /* Nothing owed */
	VNET_VF_TEARDOWN_OWED,	   /* Needed, but not queued yet; the sweep retries */
	VNET_VF_TEARDOWN_QUEUED,   /* Work item in flight */
	VNET_VF_TEARDOWN_FAILED,   /* Teardown failed; host told via NEEDS_RESET */
};

/* Per-VF runtime slot. One per VF in struct vnet_sriov_pf_state::vf_slots[],
 * populated by vnet_sriov_discover_vf_reps() with the opened representor and
 * the VF's globally-unique index. Beyond the rep/tlp_dev plumbing, each slot
 * carries the per-VF datapath state (offload engine, VQs, control-VQ IO
 * context) that is brought up on the VF's own FEATURES_OK / DRIVER_OK MMIO
 * transitions, mirroring the PF controller. */
struct vnet_vf_slot {
	struct doca_dev_rep *rep;				/* Opened VF representor (NULL until discovered) */
	struct vnet_pci_device *vf_dev;				/* Per-VF emulated cfg-space (NULL until built) */
	struct doca_devemu_pci_tlp_dev *tlp_dev;		/* SDK tlp_dev (NULL until VFE 0->1 starts the slot) */
	struct doca_devemu_vnet_offload_engine *offload_engine; /* Per-VF datapath engine (created on FEATURES_OK) */
	struct doca_devemu_vnet_rx_vq **rx_vqs;			/* [max_queue_pairs] per-VF RX VQs */
	struct doca_devemu_vnet_tx_vq **tx_vqs;			/* [max_queue_pairs] per-VF TX VQs */
	struct doca_devemu_vnet_ctrl_vq *cvq;			/* per-VF control VQ */
	uint16_t max_queue_pairs;				/* size of rx_vqs/tx_vqs arrays */
	uint16_t vf_queue_pairs;				/* --vf-queue-pairs geometry (0 = inherit the PF's) */
	uint32_t vf_index;  /* Globally-unique VF id (pool base + PF-relative index) */
	atomic_bool active; /* answers host TLPs: VFE on, within NumVFs, has a device */
	/* A VF is reset by the host exactly as a PF is, and the firmware
	 * invalidates its crossed mkey just the same, so it runs the same
	 * recycle. See vnet_recycle.h. */
	struct vnet_recycle_state recycle;
	/* PF was restarted after this slot's tlp_dev started; restart it on the next VFE 0->1 */
	atomic_bool tlp_restart_owed;
	/* The guest reached FEATURES_OK while this VF was recycling, so bring-up
	 * was refused rather than queued against an engine that did not exist yet.
	 * The recycle owes it a bring-up on the way out; the guest will not re-ask. */
	atomic_bool deferred_setup_pending;
	/* The handshake the guest had reached, sampled before the release clears it.
	 * Bits are dropped as their work is queued, so a retry submits no more. */
	uint8_t deferred_setup_status;
	/* enum vnet_vf_teardown_state. The teardown runs off the TLP thread, so
	 * device_status is pinned at NEEDS_RESET while it is still owed. */
	atomic_uint teardown_state;
	/* A hold this slot owes the host back. Kept apart from the holders: a
	 * recycle and a teardown can hold the same VF, and neither releases for
	 * the other. */
	atomic_bool reset_hold_release_owed;
	atomic_bool engine_started;			 /* offload engine created+started on VF FEATURES_OK */
	atomic_bool vqs_created;			 /* per-VF VQ structures created */
	atomic_bool engine_enabled;			 /* VQs started + engine enabled on VF DRIVER_OK */
	struct doca_devemu_vnet_counters *vnet_counters; /* per-VF datapath counters (RX/TX visibility) */
	struct doca_devemu_pci_msix *vq_msix[2];	 /* armed per-VQ MSI-X on the VF endpoint (RX VQ0, TX VQ1) */

	/* Control-VQ / multi-queue state, mirroring the PF controller's MQ fields.
	 * These stay inert for a single-queue VF: mq_feature_negotiated only becomes
	 * true once the VF advertises and the guest negotiates F_CTRL_VQ + F_MQ, at
	 * which point the per-VF CVQ + IO-context paths are exercised. */
	struct doca_devemu_vnet_io *io_ctx; /* per-VF CVQ IO context (MQ only) */
	struct doca_pe *cvq_worker_pe;	    /* PF worker PE the io_ctx is attached to (for drain on teardown) */
	atomic_bool io_ctx_started;	    /* io_ctx doca_ctx_start() succeeded */
	atomic_bool cvq_bound;		    /* CVQ bound to io_ctx */
	bool mq_feature_negotiated;	    /* guest negotiated F_CTRL_VQ + F_MQ */
	_Atomic uint16_t num_active_qps;    /* currently started data queue pairs */
	struct vnet_vf_cvq_state cvq_state; /* app-owned CVQ MAC/VLAN shadow */
	pthread_rwlock_t admin_config_lock; /* serializes AQ snapshots/mutations with guest MMIO */
	bool admin_config_lock_initialized;
	atomic_uint_fast64_t admin_cache_generation;	 /* FLR invalidates capture; PF resources survive */
	atomic_uint_fast64_t admin_operation_generation; /* arbitrates admin work with VF reset */
	atomic_bool admin_mutating;			 /* one mutating admin command per VF */
	atomic_bool admin_stopped;			 /* device-parts LM mode */
	atomic_bool admin_stop_requested;		 /* explicit LM stop blocks guest datapath bring-up */
	atomic_bool admin_rebuild_required;		 /* SET retry must finish hardware rebuild */
};

/*
 * Whether this VF's datapath can still reach the rings the guest is resetting
 * away from. A failed teardown is not in flight: it is reported to the host
 * rather than held for.
 */
static inline bool vnet_vf_teardown_in_flight(const struct vnet_vf_slot *slot)
{
	unsigned int state = atomic_load(&slot->teardown_state);

	return state == VNET_VF_TEARDOWN_OWED || state == VNET_VF_TEARDOWN_QUEUED;
}

/*
 * Per-PF SR-IOV runtime state (up to 255 VFs via ARI).
 *
 * Allocated when SR-IOV is enabled for an EP (--total-vfs > 0) and hung off
 * pci_device_config::sriov. NULL when SR-IOV is disabled for that EP (the
 * default).
 *
 * The fields fall into three groups:
 *
 *   1. Static config (set once at PF create; never changes after):
 *      total_vf, first_vf_offset, vf_stride, vf_device_id, vf_num_msix,
 *      vf_num_db.
 *
 *   2. Host-writable shadow (updated by the SR-IOV cap cfg-write handler):
 *      control_shadow, status_shadow, num_vfs_shadow, system_pg_size,
 *      vf_bar[].
 *
 *   3. SDK-allocated runtime state (populated when the SDK creates the
 *      dynamic SR-IOV instance and enumerates VFs): first_vf_idx, vf_slots.
 *
 * The on-wire SR-IOV cap layout is documented in struct sriov_ext_cap_layout
 * (vnet_pcie_defs.h). This struct holds the semantic values; the cfg-space
 * reader synthesizes each on-wire DW from these fields.
 */
struct vnet_sriov_pf_state {
	/* Static config — set at PF create from CLI */
	uint16_t total_vf;	  /* TotalVFs / InitialVFs (1..64, per VNET_SRIOV_MAX_VFS_PER_PF) */
	uint16_t first_vf_offset; /* PCIe SR-IOV cap FirstVFOffset (= 1) */
	uint16_t vf_stride;	  /* PCIe SR-IOV cap VFStride (= 1) */
	uint16_t vf_device_id;	  /* PCIe SR-IOV cap VF Device ID */
	uint16_t vf_num_msix;	  /* Initial MSI-X per VF (derived from queue count) */
	uint16_t vf_num_db;	  /* Initial doorbells per VF (derived from queue count) */

	/* Host-writable shadow — updated by the SR-IOV cap cfg-write handler */
	uint16_t control_shadow; /* SR-IOV Control register (VF Enable, MSE, ...) */
	uint16_t status_shadow;	 /* SR-IOV Status register (mostly RO/W1C) */
	uint16_t num_vfs_shadow; /* Host-written NumVFs (clamped to total_vf) */
	uint32_t system_pg_size; /* Host-written System Page Size */
	uint32_t vf_bar[6];	 /* Host-written VF BAR registers (size-probe + base) */

	/* SDK-allocated runtime state */
	uint32_t first_vf_idx; /* Starting index in the global dynamic VF pool */
	/* True iff this process reserved this PF's VF-index range and must release it on
	 * destroy; false for LU-restored PFs that reattach without reserving. Explicit so
	 * the release does not hinge on vf_slots being populated. */
	bool owns_pool_range;
	/* True once this PF's VF MSI-X/doorbells sit in its dynamic pool, which happens
	 * once at bring-up, before the VF devices claim their own counts. */
	bool vf_res_pooled;
	struct vnet_vf_slot *vf_slots; /* Array of length total_vf; NULL until allocated */
};

enum vnet_virtio_device_types {
	VNET_VIRTIO_INVALID_DEVICE = 0,
	VNET_VIRTIO_NETWORK_DEVICE = 1,
};

struct vnet_pci_device_attrs {
	uint16_t virtio_type;
	uint64_t device_features;
	int num_queues;
	uint16_t admin_queue_num;
	uint16_t queue_size; /* VirtQueue size (entries per queue) */
	void *dev_cfg;
	vnet_pci_device_status_change_cb_t pci_cfg_change_cb;
	void *cb_arg;
};

/**
 * @brief Initialize VNet PCI Device Subsystem
 *
 * Creates and registers the PCI TLP device type for VirtIO network devices.
 * The PCI layout is preconfigured inside the vnet_pci_device and the
 * endpoint is plugged into the PCI switch provided by the firmware.
 * The endpoint will be visible upon next PCI enumeration (warm boot or
 * forced PCI rescan: echo 1 > /sys/bus/pci/rescan).
 *
 * @param[in] dev DOCA device handle
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t vnet_pci_device_init(struct tlp_context *tlp_ctx);

/**
 * @brief Populate per-PF cfg-space dw_regs[64..79] with the SR-IOV PCIe ext cap
 *
 * Synthesizes the 16 DWs of the SR-IOV cap from the per-PF shadow and writes
 * them into the VirtIO device's raw cfg-space array. Required because endpoint
 * cfg-reads (vnet_pci_dev_handle_cfg_read0) read dw_regs[] directly via
 * VIRTIO_DEV_CONFIG_READ — they bypass the handle_config_space_ext_caps_read
 * dispatcher, which only fires for non-endpoint cfg paths.
 *
 * Called once per PF at create_device time, after endpoint->sriov has been
 * allocated and populated with static fields from the CLI config. No-op when
 * either argument is NULL (caller guards against calling for SR-IOV-disabled
 * EPs by simply not calling).
 *
 * @param[in] virtio_dev Per-PF VirtIO device (host-visible cfg-space owner)
 * @param[in] sriov Per-PF SR-IOV shadow state (static + host-writable fields)
 */
void vnet_pci_device_sriov_populate_cfg(struct vnet_pci_device *virtio_dev, const struct vnet_sriov_pf_state *sriov);

/**
 * @brief Rebuild PF config/MMIO routing tables after Channel LU restore.
 *
 * Channel LU restores BDF and BAR state without replaying host enumeration TLPs.
 * This helper publishes the restored PF virtio devices into the same lookup
 * tables normal enumeration populates.
 *
 * @param[in] tlp_ctx TLP context with restored devs_config and virtio_dev state
 */
void vnet_pci_device_lu_restore_pf_routing(struct tlp_context *tlp_ctx);

/**
 * Build / tear down the per-VF emulated VirtIO config-space template.
 *
 * init_vf allocates a vnet_pci_device mirroring the PF's VirtIO interface with a
 * VF-specific Device ID (from the SR-IOV shadow), the PF queue geometry, and a
 * per-VF MAC; it sits in vf_slots[].vf_dev until a tlp_dev is bound. destroy_vf
 * frees it.
 */
doca_error_t vnet_pci_device_init_vf(struct tlp_context *tlp_ctx,
				     const struct pci_device_config *pf_endpoint,
				     uint32_t vf_index,
				     struct vnet_pci_device **out_vf_dev);
void vnet_pci_device_destroy_vf(struct vnet_pci_device *vf_dev);

/**
 * @brief Reset an SR-IOV VF's emulated common-config shadow to reset defaults.
 *
 * Called on VF datapath teardown (guest device reset, app exit) so a subsequent
 * guest re-probe re-runs the full status/feature/queue handshake. In particular
 * it clears each queue's stale queue_enable so the re-probe's queue_enable 0->1
 * transition re-applies the VQ configuration.
 *
 * @param[in,out] vf VF emulated device shadow. NULL is tolerated (no-op).
 */
void vnet_pci_device_reset_vf_shadow(struct vnet_pci_device *vf);

/**
 * @brief Create VirtIO network device instance
 *
 * Creates and initializes a new VirtIO network device with the specified attributes.
 * This includes creating device representor, TLP device, configuring VirtIO parameters,
 * and adding the device to the active devices list for enumeration.
 *
 * @param[in] attr Device attributes including device features, queue count, callbacks
 * @return Pointer to created device on success, NULL on error
 *
 * @note The device will be enumerated when the host performs PCIe discovery
 * @note Device must be destroyed with vnet_pci_device_destroy()
 */
doca_error_t vnet_pci_device_create(struct tlp_context *tlp_ctx, const struct vnet_pci_device_attrs *attr);

/**
 * @brief Destroy VirtIO network device instance
 *
 * Properly cleans up and destroys a VirtIO device instance, including stopping
 * the TLP device, destroying the device representor, and freeing allocated memory.
 * Removes the device from active/enumerated device lists.
 *
 * @param[in] dev VirtIO device to destroy (can be NULL)
 *
 * @note This function is safe to call with NULL pointer
 * @note All device resources are properly cleaned up
 */
void vnet_pci_device_destroy(struct vnet_pci_device *dev);

/**
 * @brief Get VirtIO common configuration structure
 * @param[in] dev VirtIO device
 * @return Pointer to common configuration
 */
struct vnet_virtio_common_config *vnet_pci_device_get_pci_cfg(struct vnet_pci_device *dev);

/**
 * @brief Get VirtIO queue configuration array
 *
 * NOTE: The returned pointer aliases the device's mutable shadow array. It is
 * NOT safe to dereference from a worker thread while the host can issue a
 * RESET (the TLP path may concurrently memset the array under vqs_mutex when
 * the 20 ms reset wait expires). Worker-thread callers must use
 * vnet_pci_device_snapshot_vqs() / vnet_pci_device_snapshot_vq() instead.
 *
 * @param[in] dev VirtIO device
 * @return Pointer to queue configuration array
 */
const struct vnet_virtio_queue_config *vnet_pci_device_get_virtq_pci_cfg(struct vnet_pci_device *dev);

/**
 * @brief Snapshot the entire VQ shadow array into caller-provided storage.
 *
 * Briefly takes dev->vqs_mutex around a memcpy. Callers MUST NOT call this
 * while holding any DOCA/FW round-trip in flight - the lock must not contend
 * with the TLP main thread for more than the memcpy duration.
 *
 * @param[in]  dev VirtIO device
 * @param[out] dst Destination buffer with room for at least dev->vqs_count entries
 */
void vnet_pci_device_snapshot_vqs(struct vnet_pci_device *dev, struct vnet_virtio_queue_config *dst);

/**
 * @brief Snapshot a single VQ entry into caller-provided storage.
 *
 * If vq_index is out of range the destination is zero-filled and the function
 * returns false.
 *
 * @param[in]  dev      VirtIO device
 * @param[in]  vq_index Queue index
 * @param[out] dst      Destination entry
 * @return true if vq_index was in range, false otherwise
 */
bool vnet_pci_device_snapshot_vq(struct vnet_pci_device *dev, uint16_t vq_index, struct vnet_virtio_queue_config *dst);

/**
 * @brief Get VirtIO network device configuration
 * @param[in] dev VirtIO device
 * @return Pointer to network device configuration
 */
const struct vnet_virtio_net_config *vnet_pci_device_get_vnet_dev_cfg(struct vnet_pci_device *dev);

/**
 * @brief Submit delayed device destroy to workqueue
 *
 * Used during hotplug removal to destroy device asynchronously after host
 * driver completes reset. This ensures host can read device_status=0 before
 * device is destroyed.
 *
 * @param[in] tlp_ctx TLP context
 * @param[in] endpoint Endpoint device configuration to destroy
 */
void pci_cfg_workqueue_submit_delayed_destroy(struct tlp_context *tlp_ctx, struct pci_device_config *endpoint);

/**
 * @brief Submit hotplug event to workqueue for async execution
 *
 * Queues device creation/destruction to the worker thread so the main thread
 * continues calling doca_pe_progress() without interruption. This prevents
 * PCIe Completion Timeouts that occur when create_device() blocks the main
 * thread for 350-400ms during back-to-back hotplug operations.
 *
 * @param[in] tlp_ctx TLP context
 * @param[in] dsp_index DSP slot index
 * @param[in] plug True for plug, false for unplug
 */
void pci_cfg_workqueue_submit_hotplug(struct tlp_context *tlp_ctx, uint32_t dsp_index, bool plug);

/**
 * @brief Spawn a serialized MQ resize worker for one controller
 *
 * The controller state guarantees that only one worker is submitted per
 * controller. Workers for different controllers can run in parallel.
 *
 * @param[in] controller VNet controller
 * @return DOCA_SUCCESS if the worker was created, error otherwise
 */
doca_error_t pci_cfg_workqueue_submit_mq_resize(struct vnet_pci_dev_controller *controller);

/**
 * @brief Submit speed change to async workqueue
 *
 * Queues a speed change operation to the worker thread. The worker will call
 * vnet_pci_dev_execute_speed_change() which performs link bounce + MSI-X raise.
 *
 * @param[in] controller VNet controller
 * @param[in] new_speed New speed in Mbps
 */
void pci_cfg_workqueue_submit_speed_change(struct vnet_pci_dev_controller *controller, uint32_t new_speed);

/**
 * @brief Submit an on-demand SF-info dump to the workqueue
 *
 * Queues the 'sfinfo' dump for PF 'ep_index' and its VFs on the worker thread,
 * off the PE progress loop.
 *
 * @param[in] tlp_ctx TLP context
 * @param[in] ep_index PF/endpoint index (0 .. num_ep-1)
 */
void pci_cfg_workqueue_submit_dump_sf_info(struct tlp_context *tlp_ctx, uint32_t ep_index);

/**
 * @brief Queue creation + start of a VF's datapath offload engine.
 *
 * Submitted from the VF MMIO common-config write path when the guest VF driver
 * sets the virtio FEATURES_OK status bit. Runs on the workqueue worker thread
 * because the engine create is a heavy call that must not block the TLP thread.
 * The worker re-validates the slot under pf_endpoint->endpoint_lock and is a
 * no-op when the slot's engine is already started.
 *
 * @param[in] tlp_ctx TLP context (MTU / MAC / queue config for the engine).
 * @param[in] pf_endpoint Owning PF endpoint; valid until the worker drains.
 * @param[in] slot_idx Index into pf_endpoint->sriov->vf_slots[]; < total_vf.
 */
bool pci_cfg_workqueue_submit_vf_engine_setup(struct tlp_context *tlp_ctx,
					      struct pci_device_config *pf_endpoint,
					      uint16_t slot_idx);

/**
 * @brief Queue application of one SR-IOV VF virtqueue's configuration.
 *
 * Submitted from the VF MMIO common-config write path when the guest enables a
 * VF queue (queue_enable 0->1, or addresses become complete). The worker
 * re-validates the slot under pf_endpoint->endpoint_lock and applies the
 * queue's descriptor/driver/device addresses to the backing VQ via the SDK.
 *
 * @param[in] tlp_ctx TLP context.
 * @param[in] pf_endpoint Owning PF endpoint; valid until the worker drains.
 * @param[in] slot_idx Index into pf_endpoint->sriov->vf_slots[]; < total_vf.
 * @param[in] vq_index VirtIO queue index to configure.
 */
void pci_cfg_workqueue_submit_vf_vq_config(struct tlp_context *tlp_ctx,
					   struct pci_device_config *pf_endpoint,
					   uint16_t slot_idx,
					   uint16_t vq_index);

/**
 * @brief Queue start + enable of an SR-IOV VF's offload engine.
 *
 * Submitted from the VF MMIO common-config write path on the VF's DRIVER_OK
 * transition. The worker re-validates the slot under pf_endpoint->endpoint_lock
 * and starts the VF's virtqueues + enables its offload engine.
 *
 * @param[in] pf_endpoint Owning PF endpoint; valid until the worker drains.
 * @param[in] slot_idx Index into pf_endpoint->sriov->vf_slots[]; < total_vf.
 */
bool pci_cfg_workqueue_submit_vf_engine_enable(struct pci_device_config *pf_endpoint, uint16_t slot_idx);

/**
 * @brief Queue teardown of an SR-IOV VF's offload engine on a guest device reset.
 *
 * Submitted from the VF MMIO common-config write path when the guest clears
 * device_status to 0 (device reset, e.g. driver unbind). The worker re-validates
 * the slot under pf_endpoint->endpoint_lock and destroys the VF's engine + VQs
 * so the next FEATURES_OK rebuilds a fresh datapath. The emulated shadow is
 * reset inline by the caller.
 *
 * @param[in] pf_endpoint Owning PF endpoint; valid until the worker drains.
 * @param[in] slot_idx Index into pf_endpoint->sriov->vf_slots[]; < total_vf.
 * @return true if the teardown was queued; false leaves it owed to the caller.
 */
bool pci_cfg_workqueue_submit_vf_datapath_reset(struct pci_device_config *pf_endpoint, uint16_t slot_idx);

/**
 * @brief Queue a retained mutating admin command on the PCI worker
 *
 * @param[in] cmd App-owned command context retained until worker completion
 * @return true when queued, false when the workqueue cannot accept the command
 */
bool pci_cfg_workqueue_submit_admin_cmd(struct vnet_admin_async_cmd *cmd);

/**
 * @brief Check whether the caller is the PCI configuration worker
 *
 * @return true when called by the workqueue worker thread
 */
bool pci_cfg_workqueue_is_worker_thread(void);

#endif
