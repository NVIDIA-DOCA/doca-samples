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

#ifndef VNET_SRIOV_H
#define VNET_SRIOV_H

/*
 * SR-IOV orchestration helpers for the vNET PCI emulation app.
 *
 * Split of responsibilities:
 *   - vnet_pci_device.{c,h} : SR-IOV PCIe extended cap registration, on-wire DW
 *                             synthesis, and host-write decoding.
 *   - vnet_sriov.{c,h}      : SDK cap probing, the dynamic-VF index pool, PF
 *                             representor creation with a reserved VF range, VF
 *                             representor enumeration, and per-PF cleanup.
 *
 * All functions here are reached only when SR-IOV is enabled (--total-vfs > 0);
 * with SR-IOV disabled (the default) this module is unreferenced.
 */

#include <stdbool.h>
#include <stdint.h>

#include <doca_dev.h>
#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tlp_context;
struct pci_device_config;
struct vnet_sriov_pf_state;
struct vnet_vf_slot;

/*
 * SDK capabilities relevant to dynamic SR-IOV. Filled by vnet_sriov_probe_caps()
 * at startup and consumed by validate_caps() / create_pf_rep_ex().
 * dynamic_sriov_supported gates the numeric fields: when false they are
 * unspecified and the caller falls back to the non-rep_ex create path.
 */
struct vnet_sriov_caps {
	bool dynamic_sriov_supported;	  /* doca_devemu_pci_tlp_cap_type_is_dynamic_sriov_supported */
	uint16_t max_total_vfs;		  /* doca_devemu_pci_cap_get_max_total_vfs */
	uint16_t pool_size;		  /* doca_devemu_pci_cap_get_dynamic_vf_pool_size */
	uint16_t max_vf_stride;		  /* doca_devemu_pci_cap_get_max_vf_stride */
	uint16_t max_first_vf_offset;	  /* doca_devemu_pci_cap_get_max_first_vf_offset */
	uint16_t max_num_msix_per_device; /* doca_devemu_pci_tlp_cap_type_get_max_num_msix (0 = unknown) */
	uint16_t max_num_db_per_device;	  /* doca_devemu_pci_tlp_cap_type_get_max_num_db (0 = unknown) */
};

/**
 * Probe SDK dynamic-SR-IOV capabilities for a device.
 *
 * Returns DOCA_ERROR_NOT_SUPPORTED (with *caps zeroed) when the device does not
 * advertise dynamic SR-IOV or firmware has not provisioned a dynamic-VF pool
 * (pool_size == 0); callers treat that as "fall back to the non-rep_ex create
 * path".
 *
 * @param[in] devinfo Device info to query (from doca_dev_as_devinfo()).
 * @param[out] caps Filled on success; zeroed on error or when unavailable.
 * @return DOCA_SUCCESS, DOCA_ERROR_INVALID_VALUE, or DOCA_ERROR_NOT_SUPPORTED.
 */
doca_error_t vnet_sriov_probe_caps(const struct doca_devinfo *devinfo, struct vnet_sriov_caps *caps);

/**
 * Validate the CLI SR-IOV config against the probed caps.
 *
 * Always checks the per-PF VF range. The SDK-bound checks (TotalVFs vs
 * max_total_vfs, fixed VFStride/FirstVFOffset, per-VF MSI-X/doorbells vs the
 * per-function max, total VFs vs the dynamic-VF pool) run only when caps are
 * populated, so the framework behaves uniformly whether or not dynamic SR-IOV
 * is available.
 *
 * @param[in] tlp_ctx TLP context with the resolved SR-IOV config.
 * @param[in] caps Probed caps (may be all-zero).
 * @return DOCA_SUCCESS or DOCA_ERROR_INVALID_VALUE.
 */
doca_error_t vnet_sriov_validate_caps(const struct tlp_context *tlp_ctx, const struct vnet_sriov_caps *caps);

/**
 * Initialize the global VF index pool on the TLP context (resets the cursor).
 * pool_size == 0 means "no dynamic pool"; later alloc_range() calls then fail
 * with DOCA_ERROR_NO_MEMORY.
 */
void vnet_sriov_pool_init(struct tlp_context *tlp_ctx, uint16_t pool_size);

/**
 * Reserve a contiguous VF index range from the pool (cursor allocator).
 * Returns the first reserved index in *out_first_idx.
 *
 * @return DOCA_SUCCESS, DOCA_ERROR_INVALID_VALUE, or DOCA_ERROR_NO_MEMORY.
 */
doca_error_t vnet_sriov_pool_alloc_range(struct tlp_context *tlp_ctx, uint16_t count, uint32_t *out_first_idx);

/**
 * Release a previously-reserved VF index range. Releasing the most-recent range
 * rewinds the cursor (LIFO fast path); once all reserved ranges are released
 * the cursor resets to 0, so no indices are stranded regardless of release
 * order (e.g. multi-PF teardown).
 */
void vnet_sriov_pool_release_range(struct tlp_context *tlp_ctx, uint32_t first_idx, uint16_t count);

/**
 * Create the PF representor with a reserved VF range via the SDK's
 * dynamic-SR-IOV create_rep_ex surface. On success pf_endpoint->rep is bound to
 * the range [first_vf_idx, first_vf_idx + TotalVFs); on error the caller falls
 * back to doca_devemu_pci_type_create_rep().
 *
 * @param[in,out] tlp_ctx TLP context carrying the pci_type and CLI config.
 * @param[in] first_vf_idx First reserved VF index from pool_alloc_range().
 * @param[in,out] pf_endpoint Endpoint whose ->rep is set on success.
 * @return DOCA_SUCCESS or an SDK error.
 */
doca_error_t vnet_sriov_create_pf_rep_ex(struct tlp_context *tlp_ctx,
					 uint32_t first_vf_idx,
					 struct pci_device_config *pf_endpoint);

/**
 * Discover and open the VF representors the SDK provisioned for a PF.
 *
 * Walks the emulated-rep list, matches each entry's parent PF VUID against this
 * PF, and opens matches into a freshly-allocated sriov->vf_slots[total_vf]
 * (slot.rep, slot.vf_index). When no matching reps exist (no VF range was
 * reserved) vf_slots is left NULL and DOCA_SUCCESS is returned.
 *
 * @return DOCA_SUCCESS on enumeration; an error only on infrastructure failure.
 */
doca_error_t vnet_sriov_discover_vf_reps(struct tlp_context *tlp_ctx, struct pci_device_config *pf_endpoint);

/**
 * Close every VF rep opened by vnet_sriov_discover_vf_reps() and clear the
 * slot handles. Does not free the vf_slots array or release the pool range
 * (the caller owns those). Safe on a NULL sriov or NULL vf_slots.
 */
void vnet_sriov_close_vf_reps(struct vnet_sriov_pf_state *sriov);

/**
 * Bring up the SDK tlp_dev backing one VF slot (host VFE 0->1 fan-out).
 *
 * Creates and starts a tlp_dev on the slot's opened rep and flips slot->active.
 * Idempotent: returns DOCA_SUCCESS if already active, and DOCA_ERROR_NOT_FOUND
 * for a slot with no opened rep so the fan-out worker can iterate uniformly.
 * Heavy SDK call (~50-200ms); runs on the workqueue worker, not the TLP thread.
 */
doca_error_t vnet_sriov_start_vf_slot(struct tlp_context *tlp_ctx, struct vnet_vf_slot *slot);

/**
 * Stop and destroy the SDK tlp_dev backing one VF slot (reverse of
 * vnet_sriov_start_vf_slot()).
 *
 * Tears down the per-VF datapath engine, then stops + destroys slot->tlp_dev
 * and clears slot->active. Idempotent (DOCA_SUCCESS on an already-inactive
 * slot) and returns DOCA_ERROR_NOT_FOUND for a slot with no opened rep.
 */
doca_error_t vnet_sriov_stop_vf_slot(struct vnet_vf_slot *slot);

/**
 * Derive a per-VF MAC from the base MAC: base_low16 + num_ep + vf_index, folded
 * into the low 2 bytes (out_mac must be ETH_ALEN). Shared by the VF cfg-space
 * template and the VF offload engine so both present the identical MAC.
 */
void vnet_sriov_derive_vf_mac(const uint8_t *base_mac, uint16_t num_ep, uint32_t vf_index, uint8_t *out_mac);

#ifdef __cplusplus
}
#endif

#endif /* VNET_SRIOV_H */
