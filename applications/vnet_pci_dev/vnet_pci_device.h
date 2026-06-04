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

#include <doca_dev.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_devemu_pci_tlp.h>

#include "vnet_pcie_defs.h"
#include "vnet_virtio_types.h"
#include "pci_spec_tlp.h"

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

/* CVQ index calculation: max_queue_pairs * 2 (RX+TX per QP) */
#define VNET_CVQ_INDEX(max_qp) ((max_qp)*2)

/* Total VQs = all data queue pairs + 1 CVQ */
#define VNET_TOTAL_VQS(max_qp) (VNET_CVQ_INDEX(max_qp) + 1)

#define VNET_DEFAULT_QUEUE_SIZE 1024 /* Default queue size */
#define VNET_MIN_QUEUE_SIZE 16	     /* Minimum queue size */
#define VNET_MAX_QUEUE_SIZE 4096     /* Maximum queue size */

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

	/* vq shadow array - dynamically allocated based on num_queues */
	struct vnet_virtio_queue_config *vqs;
	uint16_t vqs_count;
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

enum vnet_virtio_device_types {
	VNET_VIRTIO_INVALID_DEVICE = 0,
	VNET_VIRTIO_NETWORK_DEVICE = 1,
};

struct vnet_pci_device_attrs {
	uint16_t virtio_type;
	uint64_t device_features;
	int num_queues;
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
 * @param[in] dev VirtIO device
 * @return Pointer to queue configuration array
 */
const struct vnet_virtio_queue_config *vnet_pci_device_get_virtq_pci_cfg(struct vnet_pci_device *dev);

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
 * @brief Spawn a detached thread to start additional queue pairs
 *
 * Called after the CVQ ctrl_req has already been completed with VIRTIO_NET_OK.
 * Each device's start_additional_queue_pairs runs in its own detached thread,
 * enabling parallel QP startup across all devices (~8s each, in parallel).
 *
 * The caller MUST complete the ctrl_req BEFORE calling this function.
 *
 * @param[in] controller VNet controller
 * @param[in] old_qps Current number of active queue pairs
 * @param[in] new_qps Target number of active queue pairs
 */
void pci_cfg_workqueue_submit_mq_start_qps(struct vnet_pci_dev_controller *controller,
					   uint16_t old_qps,
					   uint16_t new_qps);

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

#endif
