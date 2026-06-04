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

#ifndef VBLK_PCI_DEV_CORE_H
#define VBLK_PCI_DEV_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include <doca_error.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include "vblk_ctrl.h"
#include "pci_spec_tlp.h"

/* Max supported queues (HW supports 256, but limited to 255 to prevent MSIX sharing) */
#define VBLK_PCI_DEV_MAX_QUEUES 255
#define VBLK_PCI_DEV_MAX_CORES 16
#define VBLK_PCI_DEV_MAX_IO_CORES (VBLK_PCI_DEV_MAX_CORES - 1)

/* Default configuration values */
#define VBLK_PCI_DEV_DEFAULT_TLP_CORE_IDX 15
#define VBLK_PCI_DEV_DEFAULT_OFFLOAD_ENGINE_CORE_IDX 0
#define VBLK_PCI_DEV_DEFAULT_IO_CTX_MASK 0x00FFUL
#define VBLK_PCI_DEV_DEFAULT_NUM_QUEUES 255
#define VBLK_PCI_DEV_MAX_QUEUES_PER_EP 255
#define VBLK_PCI_DEV_DEFAULT_NUM_EP 1

/* Global force quit flag (defined in vblk_pci_dev.c) */
extern volatile bool force_quit;

/**
 * @brief Progress engine context for application threads
 *
 * Each IO context thread and the TLP thread has one PE context, containing
 * the DOCA progress engine, the thread handle, and initialization state.
 */
struct vblk_pci_dev_pe_context {
	struct doca_pe *pe; /* DOCA progress engine handle */
	pthread_t thread;   /* Thread handle for this PE context */
	bool initialized;   /* True when PE is created and ready */
};

struct vblk_mpool_set;

/*
 * Per-EP hotplug state for cross-thread coordination.
 * TLP thread sets plug/unplug_requested.
 * OE thread detects and executes ctrl lifecycle.
 * IO threads detect need_io_ctx and create IO ctx on their own thread.
 */
struct vblk_ep_hotplug_state {
	_Atomic bool plug_requested;
	_Atomic bool unplug_requested;
	_Atomic bool unplug_queued;
	_Atomic bool need_io_ctx;
	_Atomic bool need_io_ctx_shutdown;
	_Atomic uint8_t io_ctx_ready;
	_Atomic uint8_t io_ctx_shutdown_done;
	_Atomic bool enabled;
	_Atomic bool plug_msi_pending;	 /* Plug MSI send failed, retry on next OE cycle */
	_Atomic bool unplug_msi_pending; /* Unplug MSI send failed, retry on next OE cycle */
	_Atomic bool io_ctx_created[VBLK_PCI_DEV_MAX_IO_CORES];
};

/**
 * @brief Application resources structure
 *
 * Contains all runtime resources for the VirtIO Block PCI device application.
 * This structure aggregates device handles, PE contexts, and the block controller.
 * Mirrors vnet_pci_dev_resources for consistent architecture across VirtIO devices.
 */
struct vblk_pci_dev_resources {
	struct doca_dev *doca_dev;		    /* DOCA device handle */
	struct vblk_pci_dev_pe_context *io_pe_ctxs; /* Array of IO PE contexts (num_io_ctx) */
	struct vblk_pci_dev_pe_context tlp_pe_ctx;  /* TLP thread PE context */
	struct vblk_ctrl *vblk_ctrls;		    /* Per-EP VBlk controllers (num_ep) */
	struct vblk_tlp_context *tlp_ctx;	    /* TLP context for PCI event handling */
	struct vblk_mpool_set **shared_mpools;	    /* Per-IO-ctx shared memory pools */
	struct vblk_ep_hotplug_state *hp_states;    /* Per-EP hotplug state (num_ep) */
	uint32_t num_ep;			    /* Number of endpoints */
	uint8_t num_io_ctx;			    /* Number of IO contexts */
};

/**
 * @brief Application configuration structure
 *
 * Contains all configuration parameters for the VirtIO Block PCI device application.
 * This structure is populated from command-line arguments and provides immutable
 * configuration throughout the application lifecycle.
 */
struct vblk_pci_dev_config {
	char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* Emulation manager device name */
	uint32_t num_ep;				/* Number of endpoints (1-32) */
	uint16_t num_queues;				/* Number of virtio queues (1-255) */
	uint16_t seg_max;				/* Max segments per request (0 = default 1) */
	uint64_t io_ctx_mask;				/* IO contexts CPU mask */
	uint8_t tlp_core_idx;				/* TLP core index (not in io_ctx_mask) */
	uint8_t offload_engine_core_idx;		/* Offload engine core (in io_ctx_mask) */
	bool datapath_on_dpa;				/* Data path provider */
	uint32_t stats_ios_period;			/* Stats query period (0 = disabled) */
	bool hotplug_mode;				/* Hotplug mode (dynamic plug/unplug) */
	bool indirect_enabled;				/* Enable indirect descriptor feature */
};

/**
 * @brief Initialize application resources
 *
 * Allocates PE context arrays and per-EP arrays inside the caller-provided
 * resources structure. Must be called after num_io_ctx is determined from
 * configuration validation.
 *
 * @param[in] num_io_ctx Number of IO contexts
 * @param[in] num_ep Number of endpoints
 * @param[out] resources Resources structure to initialize (caller-allocated)
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
doca_error_t vblk_pci_dev_resources_init(uint8_t num_io_ctx, uint32_t num_ep, struct vblk_pci_dev_resources *resources);

/**
 * @brief Cleanup application resources
 *
 * Releases dynamically allocated arrays within the resources structure.
 *
 * @param[in] resources Resources to cleanup
 */
void vblk_pci_dev_resources_cleanup(struct vblk_pci_dev_resources *resources);

/**
 * @brief Initialize and run VirtIO Block PCI Device application
 *
 * Main entry point for VirtIO Block PCI device logic. Performs device discovery,
 * initializes DOCA framework, creates VirtIO block devices, and runs
 * the main event processing loop until termination is requested.
 *
 * @param[in] config Application configuration
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 *
 * @note This function blocks until force_quit is set to true
 * @note Proper cleanup is performed regardless of exit reason
 */
doca_error_t vblk_pci_dev_run(struct vblk_pci_dev_config *config);

#endif /* VBLK_PCI_DEV_CORE_H */
