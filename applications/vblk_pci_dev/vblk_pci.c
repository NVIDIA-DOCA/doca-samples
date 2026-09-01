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

#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <endian.h>

#include <linux/virtio_pci.h>
#include <linux/virtio_config.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_devemu_pci_type.h>
#include <doca_devemu_vblk_type.h>
#include <doca_devemu_pci_info.h>

#include "vblk_pci.h"
#include "pci_spec_tlp.h"
#include "vblk_pci_dev_core.h"

DOCA_LOG_REGISTER(VBLK_PCI);

/* Shutdown flag owned by the app core (declared in vblk_pci_dev_core.h, which
 * is included above). The TLP thread uses it to request a clean process
 * shutdown on an unrecoverable SR-IOV bring-up failure; the OE thread uses
 * the same flag for datapath-build failures. */

static struct vblk_tlp_context g_tlp_ctx;
static struct pcie_virtio_dev virtio_dev;

/* Round-level guard for PERST# handling: set on assert, cleared on deassert.
 * Drops repeat asserts inside the same round so firmware event retries do
 * not restart a recycle that is still in flight. */
static _Atomic bool g_perst_round_in_progress;

/* All complete_*() calls run on the TLP thread by design: doca_ctx is not
 * thread-safe and the doca lib's internal req_pool.acquire() happens BEFORE
 * the event callback fires, so no app-side lock can cover it anyway. The OE
 * thread never completes here — plug/unplug MSI is queued via
 * plug_msi_pending / unplug_msi_pending / cmd_completed_msi_pending and
 * consumed by TLP (see vblk_pci_retry_hotplug_msi). */
static inline void vblk_complete_tlp(struct doca_devemu_pci_tlp_channel_req *req,
				     uint8_t has_cpl,
				     struct doca_devemu_pci_tlp_dev *pci_dev)
{
	doca_devemu_pci_tlp_channel_req_complete_tlp(req, has_cpl, pci_dev);
}

static inline void vblk_complete_acg(struct doca_devemu_pci_tlp_channel_req *req,
				     uint16_t acg_buf_len,
				     enum doca_devemu_pci_tlp_channel_req_acg_comp_opmode opmode)
{
	doca_devemu_pci_tlp_channel_req_complete_acg(req, acg_buf_len, opmode);
}

static inline void vblk_complete_pci_event(struct doca_devemu_pci_tlp_channel_req *req)
{
	doca_devemu_pci_tlp_channel_req_complete_pci_event(req);
}

/* DW index of express slot_control+slot_status in PCI config space */
#define EXPRESS_SLOT_CTRL_STS_DW ((TLP_PCI_CAP_OFFSET_EXPRESS + 24) / 4)
#define PCI_STD_HEADER_REG_NUM (PCI_STD_HEADER_SIZEOF / sizeof(uint32_t))
#define PCIE_EXT_CAP_FIRST_REG_NUM (PCI_CFG_SPACE_SIZE / sizeof(uint32_t))
#define PCIE_EXT_CAP_END_REG_NUM (PCI_CFG_SPACE_EXP_SIZE / sizeof(uint32_t))

enum {
	VBLK_SRIOV_FIRST_VF_OFFSET = 1,
	VBLK_SRIOV_VF_STRIDE = 1,
};

static bool vblk_pci_is_ext_cfg_reg(const uint32_t reg)
{
	return reg >= PCIE_EXT_CAP_FIRST_REG_NUM && reg < PCIE_EXT_CAP_END_REG_NUM;
}

static bool vblk_pci_is_sriov_ext_cap_reg(const uint32_t reg)
{
	return reg >= PCIE_EXT_CAP_FIRST_REG_NUM &&
	       reg < PCIE_EXT_CAP_FIRST_REG_NUM + (PCI_EXT_CAP_SRIOV_SIZEOF / sizeof(uint32_t));
}

static bool vblk_pci_is_vf(const struct pci_device_config *dc)
{
	return dc->sriov_vf != NULL;
}

static uint32_t vblk_pci_ep_index(const struct pci_device_config *ep)
{
	return (uint32_t)(ep - &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx)]);
}

static bool vblk_pci_has_sriov_ext_cap_reg(const struct pci_device_config *dc, const uint32_t reg)
{
	return dc->is_endpoint && !vblk_pci_is_vf(dc) && dc->sriov != NULL && vblk_pci_is_sriov_ext_cap_reg(reg);
}

/*
 * SR-IOV Supported Page Sizes is a bitmap: bit N means 4 KB * 2^N.
 * vblk only supports the base 4 KB page size, so expose bit 0 only.
 */
#define VBLK_SRIOV_SUPPORTED_PAGE_SIZE_MASK (1u << 0)

_Static_assert(VBLK_SRIOV_FIRST_VF_OFFSET + (PCI_FUNC(UINT8_MAX) - 1) * VBLK_SRIOV_VF_STRIDE <= PCI_FUNC(UINT8_MAX),
	       "no-ARI SR-IOV VF functions must fit in the conventional PCI function number range");

static doca_error_t vblk_pci_sriov_reset_vf_slot(struct vblk_sriov_vf_slot *slot);
static void vblk_pci_sriov_pool_cleanup(void)
{
	free(g_tlp_ctx.sriov_pool.in_use);
	g_tlp_ctx.sriov_pool.in_use = NULL;
	g_tlp_ctx.sriov_pool.pool_size = 0;
}

static doca_error_t vblk_pci_sriov_pool_init(uint16_t pool_size)
{
	vblk_pci_sriov_pool_cleanup();
	if (pool_size == 0)
		return DOCA_SUCCESS;

	g_tlp_ctx.sriov_pool.in_use = calloc(pool_size, sizeof(*g_tlp_ctx.sriov_pool.in_use));
	if (g_tlp_ctx.sriov_pool.in_use == NULL)
		return DOCA_ERROR_NO_MEMORY;

	g_tlp_ctx.sriov_pool.pool_size = pool_size;
	return DOCA_SUCCESS;
}

static doca_error_t vblk_pci_sriov_pool_alloc(uint16_t count, uint32_t *first_idx)
{
	uint32_t start;

	if (first_idx == NULL || count == 0)
		return DOCA_ERROR_INVALID_VALUE;
	if (g_tlp_ctx.sriov_pool.in_use == NULL)
		return DOCA_ERROR_NO_MEMORY;

	for (start = 0; start + count <= g_tlp_ctx.sriov_pool.pool_size; start++) {
		uint16_t i;
		bool free_range = true;

		for (i = 0; i < count; i++) {
			if (g_tlp_ctx.sriov_pool.in_use[start + i] != 0) {
				free_range = false;
				start += i;
				break;
			}
		}
		if (!free_range)
			continue;

		for (i = 0; i < count; i++)
			g_tlp_ctx.sriov_pool.in_use[start + i] = 1;
		*first_idx = start;
		return DOCA_SUCCESS;
	}

	return DOCA_ERROR_NO_MEMORY;
}

static void vblk_pci_sriov_pool_release(uint32_t first_idx, uint16_t count)
{
	uint16_t i;

	if (count == 0)
		return;

	if (g_tlp_ctx.sriov_pool.in_use == NULL || first_idx + count > g_tlp_ctx.sriov_pool.pool_size) {
		DOCA_LOG_WARN("SR-IOV: invalid dynamic VF range release [%u, %u), pool_size=%u",
			      first_idx,
			      first_idx + count,
			      g_tlp_ctx.sriov_pool.pool_size);
		return;
	}

	for (i = 0; i < count; i++)
		g_tlp_ctx.sriov_pool.in_use[first_idx + i] = 0;
}

static doca_error_t vblk_pci_sriov_probe_caps(const struct doca_devinfo *devinfo)
{
	uint8_t supported = 0;
	doca_error_t result;

	if (devinfo == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	memset(&g_tlp_ctx.sriov_caps, 0, sizeof(g_tlp_ctx.sriov_caps));
	vblk_pci_sriov_pool_cleanup();

	result = doca_devemu_pci_tlp_cap_type_is_dynamic_sriov_supported(devinfo, &supported);
	if (result != DOCA_SUCCESS || supported == 0)
		return result == DOCA_SUCCESS ? DOCA_ERROR_NOT_SUPPORTED : result;

	g_tlp_ctx.sriov_caps.dynamic_sriov_supported = true;

	result = doca_devemu_pci_cap_get_max_total_vfs(devinfo, &g_tlp_ctx.sriov_caps.max_total_vfs);
	if (result != DOCA_SUCCESS)
		goto reset_caps;
	result = doca_devemu_pci_cap_get_dynamic_vf_pool_size(devinfo, &g_tlp_ctx.sriov_caps.dynamic_vf_pool_size);
	if (result != DOCA_SUCCESS)
		goto reset_caps;
	result = doca_devemu_pci_cap_get_max_vf_stride(devinfo, &g_tlp_ctx.sriov_caps.max_vf_stride);
	if (result != DOCA_SUCCESS)
		goto reset_caps;
	result = doca_devemu_pci_cap_get_max_first_vf_offset(devinfo, &g_tlp_ctx.sriov_caps.max_first_vf_offset);
	if (result != DOCA_SUCCESS)
		goto reset_caps;

	result = vblk_pci_sriov_pool_init(g_tlp_ctx.sriov_caps.dynamic_vf_pool_size);
	if (result != DOCA_SUCCESS)
		goto reset_caps;
	DOCA_LOG_INFO("SR-IOV dynamic caps: max_total_vfs=%u pool_size=%u max_vf_stride=%u max_first_vf_offset=%u",
		      g_tlp_ctx.sriov_caps.max_total_vfs,
		      g_tlp_ctx.sriov_caps.dynamic_vf_pool_size,
		      g_tlp_ctx.sriov_caps.max_vf_stride,
		      g_tlp_ctx.sriov_caps.max_first_vf_offset);
	return DOCA_SUCCESS;

reset_caps:
	memset(&g_tlp_ctx.sriov_caps, 0, sizeof(g_tlp_ctx.sriov_caps));
	vblk_pci_sriov_pool_cleanup();
	return result;
}

static doca_error_t vblk_pci_sriov_validate_caps(const struct doca_devinfo *devinfo, uint32_t num_ep)
{
	struct doca_devemu_pci_resources *resources = NULL;
	uint64_t requested_functions;
	uint64_t requested_msix;
	uint64_t requested_db;
	uint64_t requested_vfs;
	uint32_t max_pf_devices = 0;
	uint32_t avail_msix = 0;
	uint32_t avail_db = 0;
	uint16_t max_msix = 0;
	uint16_t max_db = 0;
	doca_error_t result;

	if (devinfo == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	result = doca_devemu_pci_tlp_cap_get_max_pf_devices(devinfo, &max_pf_devices);
	if (result != DOCA_SUCCESS)
		return result;
	if (num_ep > max_pf_devices) {
		DOCA_LOG_ERR("SR-IOV requested %u PFs, but the device supports only %u TLP PF devices",
			     num_ep,
			     max_pf_devices);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	if (g_tlp_ctx.sriov_total_vfs > g_tlp_ctx.sriov_caps.max_total_vfs) {
		DOCA_LOG_ERR("SR-IOV TotalVFs=%u exceeds device maximum %u",
			     g_tlp_ctx.sriov_total_vfs,
			     g_tlp_ctx.sriov_caps.max_total_vfs);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (g_tlp_ctx.sriov_caps.max_vf_stride < VBLK_SRIOV_VF_STRIDE) {
		DOCA_LOG_ERR("SR-IOV max_vf_stride=%u below required stride %u",
			     g_tlp_ctx.sriov_caps.max_vf_stride,
			     VBLK_SRIOV_VF_STRIDE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (g_tlp_ctx.sriov_caps.max_first_vf_offset < VBLK_SRIOV_FIRST_VF_OFFSET) {
		DOCA_LOG_ERR("SR-IOV max_first_vf_offset=%u below required offset %u",
			     g_tlp_ctx.sriov_caps.max_first_vf_offset,
			     VBLK_SRIOV_FIRST_VF_OFFSET);
		return DOCA_ERROR_INVALID_VALUE;
	}

	requested_vfs = (uint64_t)num_ep * g_tlp_ctx.sriov_total_vfs;
	if (requested_vfs > g_tlp_ctx.sriov_caps.dynamic_vf_pool_size) {
		DOCA_LOG_ERR("SR-IOV requested %llu dynamic VFs across %u PFs, but the device's dynamic "
			     "VF pool is only %u; every VF needs a representor and there is no "
			     "non-rep_ex fallback datapath",
			     (unsigned long long)requested_vfs,
			     num_ep,
			     g_tlp_ctx.sriov_caps.dynamic_vf_pool_size);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	result = doca_devemu_pci_tlp_cap_type_get_max_num_msix(devinfo, &max_msix);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_pci_tlp_cap_type_get_max_num_db(devinfo, &max_db);
	if (result != DOCA_SUCCESS)
		return result;
	if (g_tlp_ctx.sriov_num_vf_msix > max_msix || g_tlp_ctx.sriov_num_vf_db > max_db) {
		DOCA_LOG_ERR("SR-IOV VF resources exceed TLP per-device caps: vf_msix=%u/%u vf_db=%u/%u",
			     g_tlp_ctx.sriov_num_vf_msix,
			     max_msix,
			     g_tlp_ctx.sriov_num_vf_db,
			     max_db);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_devemu_pci_get_available_resources(devinfo, &resources);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_pci_resources_get_num_msix(resources, &avail_msix);
	if (result != DOCA_SUCCESS)
		goto release_resources;
	result = doca_devemu_pci_resources_get_num_db(resources, &avail_db);
	if (result != DOCA_SUCCESS)
		goto release_resources;

	requested_functions = (uint64_t)num_ep * ((uint64_t)g_tlp_ctx.sriov_total_vfs + 1);
	requested_msix = requested_functions * g_tlp_ctx.sriov_num_vf_msix;
	requested_db = requested_functions * g_tlp_ctx.sriov_num_vf_db;
	if (requested_msix > avail_msix || requested_db > avail_db) {
		DOCA_LOG_ERR("SR-IOV resources exceed available pool: functions=%llu msix=%llu/%u db=%llu/%u",
			     (unsigned long long)requested_functions,
			     (unsigned long long)requested_msix,
			     avail_msix,
			     (unsigned long long)requested_db,
			     avail_db);
		result = DOCA_ERROR_NOT_SUPPORTED;
	}

release_resources:
	(void)doca_devemu_pci_release_resources(resources);
	return result;
}

static void vblk_pci_sriov_release_dynamic_range(struct pci_device_config *ep)
{
	if (ep == NULL || !ep->sriov_dynamic_vf_range_owned)
		return;

	vblk_pci_sriov_pool_release(ep->sriov_dynamic_first_vf_idx, g_tlp_ctx.sriov_total_vfs);
	ep->sriov_dynamic_first_vf_idx = 0;
	ep->sriov_dynamic_vf_range_owned = false;
}

static void bdf_map_remove_entry(struct vblk_tlp_context *ctx, struct bdf_map_entry *entry)
{
	struct bdf_map_entry **pp = &ctx->bdf_map[entry->key % BDF_MAP_SIZE];

	while (*pp) {
		if (*pp == entry) {
			*pp = entry->next;
			entry->next = NULL;
			return;
		}
		pp = &(*pp)->next;
	}
}

static void bdf_map_update(struct vblk_tlp_context *ctx, uint32_t tlp_type, uint16_t bdf, struct pci_device_config *cfg)
{
	uint32_t key = (tlp_type << 16) | bdf;
	uint32_t h = key % BDF_MAP_SIZE;
	uint32_t idx = (uint32_t)(cfg - ctx->devs_config);
	struct bdf_map_entry *e = &ctx->bdf_entries[idx];

	if (e->dev_cfg == cfg && e->key == key)
		return;
	if (e->dev_cfg != NULL)
		bdf_map_remove_entry(ctx, e);
	e->key = key;
	e->dev_cfg = cfg;
	e->next = ctx->bdf_map[h];
	ctx->bdf_map[h] = e;
}

static struct pci_device_config *bdf_map_lookup(struct vblk_tlp_context *ctx, uint32_t tlp_type, uint16_t bdf)
{
	uint32_t key = (tlp_type << 16) | bdf;
	struct bdf_map_entry *e = ctx->bdf_map[key % BDF_MAP_SIZE];

	for (; e; e = e->next)
		if (e->key == key)
			return e->dev_cfg;
	return NULL;
}

static void bdf_map_remove(struct vblk_tlp_context *ctx, uint32_t tlp_type, uint16_t bdf)
{
	uint32_t key = (tlp_type << 16) | bdf;
	struct bdf_map_entry **pp = &ctx->bdf_map[key % BDF_MAP_SIZE];

	while (*pp) {
		if ((*pp)->key == key) {
			struct bdf_map_entry *e = *pp;

			*pp = e->next;
			e->key = 0;
			e->dev_cfg = NULL;
			e->next = NULL;
			return;
		}
		pp = &(*pp)->next;
	}
}

static bool vblk_pci_sriov_is_vf_enabled(const struct vblk_sriov_pf_state *sriov, uint32_t vf_index)
{
	return (sriov->control_shadow & PCI_SRIOV_CTRL_VFE) != 0 && vf_index < sriov->num_vfs_shadow;
}

static void vblk_pci_sriov_sync_vf_presence(struct pci_device_config *ep, bool pf_present)
{
	struct vblk_sriov_pf_state *sriov = ep->sriov;
	uint32_t i;

	if (sriov == NULL)
		return;

	for (i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
		struct pci_device_config *vf = slot->cfg;
		bool vf_present;

		if (vf == NULL || slot->vf_dev == NULL)
			continue;

		vf_present = pf_present && vblk_pci_sriov_is_vf_enabled(sriov, i);
		vf->device_present = vf_present;
		vf->rep = vf_present ? slot->rep : NULL;
		vf->tlp_dev = vf_present ? slot->vf_dev->pci_tlp_dev : NULL;

		if (!vf_present && vf->is_bdf_set) {
			bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0, vf->bdf);
			bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1, vf->bdf);
			vf->is_bdf_set = false;
		}
	}
}

static void vblk_pci_virtio_dev_queue_init(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	int i;

	pci_cfg->queue_select = 0;

	memset(dev->vqs, 0, sizeof(dev->vqs));
	for (i = 0; i < pci_cfg->num_queues; i++) {
		dev->vqs[i].queue_size = 256;
		/* RO for driver: queue_notify_off; provide a unique notify offset per queue (VirtIO PCI notify cap) */
		dev->vqs[i].queue_notify_off = (uint16_t)i;
		/* Default: no MSI-X vector assigned */
		dev->vqs[i].queue_msix_vector = 0xFFFF;
		/* Disabled by default */
		dev->vqs[i].queue_enable = 0;
	}

	/* Default: no MSI-X vector assigned for config interrupts */
	pci_cfg->config_msix_vector = VIRTIO_MSI_NO_VECTOR;

	memcpy(&pci_cfg->vq, dev->vqs, sizeof(pci_cfg->vq));
}

static doca_error_t vblk_pci_virtio_dev_init_vf(const struct vblk_pci_virtio_dev *pf_dev,
						const struct pci_device_config *pf_endpoint,
						struct vblk_sriov_vf_slot *slot)
{
	const struct vblk_sriov_pf_state *sriov;
	struct vblk_pci_virtio_dev *vf_dev;
	uint16_t vf_num_queues;

	if (pf_dev == NULL || pf_endpoint == NULL || pf_endpoint->sriov == NULL || slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	/* slot->rep is the VF's own SDK representor. Every enabled VF has one -
	 * SR-IOV init aborts if a VF representor is missing (vblk_pci_create_pf_rep
	 * / discover_vf_reps) - so there is no software / PF-shared TLP device fallback;
	 * this just builds the VF's emulated config-space + device-config image. */
	if (slot->vf_dev != NULL)
		return DOCA_ERROR_INVALID_VALUE;

	sriov = pf_endpoint->sriov;
	// vf_num_db = vf_num_queue = pf_num_db = pf_num_queue
	vf_num_queues = sriov->vf_num_db;
	if (vf_num_queues == 0 || vf_num_queues > VBLK_PCI_VIRTIO_MAX_QUEUES) {
		DOCA_LOG_ERR("SR-IOV: invalid VF queue count %u", vf_num_queues);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (sriov->vf_num_msix == 0) {
		DOCA_LOG_ERR("SR-IOV: invalid VF MSI-X count 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	vf_dev = calloc(1, sizeof(*vf_dev));
	if (vf_dev == NULL)
		return DOCA_ERROR_NO_MEMORY;

	vf_dev->dev_rep = slot->rep;
	vf_dev->ep_index = vblk_pci_ep_index(pf_endpoint);
	/* The per-VF PCI config-space template is initialized once in the slot. */
	vf_dev->pcie_dev = slot->pcie_dev;

	vf_dev->device_features = pf_dev->device_features & ~(1ULL << VIRTIO_F_SR_IOV);
	vf_dev->pci_cfg.num_queues = vf_num_queues;
	vf_dev->pci_cfg.device_feature = vf_dev->device_features & 0xFFFFFFFF;
	vf_dev->pci_cfg.device_status = 0;
	vf_dev->vblk_cfg = pf_dev->vblk_cfg;
	vf_dev->vblk_cfg.num_queues = vf_num_queues;
	vblk_pci_virtio_dev_queue_init(vf_dev);

	slot->vf_dev = vf_dev;
	DOCA_LOG_INFO("SR-IOV: PF[%u] VF[%u] virtio template initialized: queues=%u, msix=%u",
		      pf_dev->ep_index,
		      slot->dynamic_vf_index,
		      vf_num_queues,
		      sriov->vf_num_msix);
	return DOCA_SUCCESS;
}

static void vblk_pci_virtio_dev_destroy_vf(struct vblk_pci_virtio_dev *vf_dev)
{
	if (vf_dev == NULL)
		return;
	free(vf_dev);
}

static doca_error_t vblk_pci_sriov_create_pf_rep_ex(uint32_t ep_index, struct pci_device_config *pf_endpoint)
{
	struct doca_devemu_pci_rep_init_attr *init_attr = NULL;
	uint32_t first_vf_idx;
	doca_error_t result;
	doca_error_t destroy_result;

	if (pf_endpoint == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (!g_tlp_ctx.sriov_caps.dynamic_sriov_supported)
		return DOCA_ERROR_NOT_SUPPORTED;

	result = vblk_pci_sriov_pool_alloc(g_tlp_ctx.sriov_total_vfs, &first_vf_idx);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_devemu_pci_rep_init_attr_create(&init_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to allocate PF rep init attr for EP[%u]: %s",
			     ep_index,
			     doca_error_get_descr(result));
		vblk_pci_sriov_pool_release(first_vf_idx, g_tlp_ctx.sriov_total_vfs);
		return result;
	}

	result = doca_devemu_pci_rep_init_attr_set_total_vfs(init_attr, g_tlp_ctx.sriov_total_vfs);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_first_dynamic_vf_idx(init_attr, first_vf_idx);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_vf_stride(init_attr, (uint16_t)VBLK_SRIOV_VF_STRIDE);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_first_vf_offset(init_attr, (uint16_t)VBLK_SRIOV_FIRST_VF_OFFSET);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_vf_num_msix(init_attr, g_tlp_ctx.sriov_num_vf_msix);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;
	result = doca_devemu_pci_rep_init_attr_set_vf_num_db(init_attr, g_tlp_ctx.sriov_num_vf_db);
	if (result != DOCA_SUCCESS)
		goto destroy_attr;

	result = doca_devemu_pci_type_create_rep_ex(g_tlp_ctx.pci_type, init_attr, &pf_endpoint->rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: create_rep_ex failed for EP[%u] (total_vfs=%u, first_vf_idx=%u): %s",
			     ep_index,
			     g_tlp_ctx.sriov_total_vfs,
			     first_vf_idx,
			     doca_error_get_descr(result));
		pf_endpoint->rep = NULL;
		goto destroy_attr;
	}

	DOCA_LOG_INFO("SR-IOV: EP[%u] PF representor created with VF range [%u, %u) (vf_msix=%u, vf_db=%u)",
		      ep_index,
		      first_vf_idx,
		      first_vf_idx + g_tlp_ctx.sriov_total_vfs,
		      g_tlp_ctx.sriov_num_vf_msix,
		      g_tlp_ctx.sriov_num_vf_db);
	pf_endpoint->sriov_dynamic_first_vf_idx = first_vf_idx;
	pf_endpoint->sriov_dynamic_vf_range_owned = true;

destroy_attr:
	destroy_result = doca_devemu_pci_rep_init_attr_destroy(init_attr);
	if (destroy_result != DOCA_SUCCESS)
		DOCA_LOG_WARN("SR-IOV: failed to destroy PF rep init attr for EP[%u]: %s",
			      ep_index,
			      doca_error_get_descr(destroy_result));
	if (result != DOCA_SUCCESS)
		vblk_pci_sriov_pool_release(first_vf_idx, g_tlp_ctx.sriov_total_vfs);
	return result;
}

static doca_error_t vblk_pci_create_pf_rep(uint32_t ep_index, struct pci_device_config *pf_endpoint)
{
	doca_error_t result;

	if (pf_endpoint == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	pf_endpoint->sriov_dynamic_first_vf_idx = 0;
	pf_endpoint->sriov_dynamic_vf_range_owned = false;
	if (!g_tlp_ctx.sriov_enabled)
		return doca_devemu_pci_type_create_rep(g_tlp_ctx.pci_type, &pf_endpoint->rep);

	/* SR-IOV requires dynamic VF backing: create_rep_ex provisions each VF's
	 * representor and MSI-X/DB resources. There is no working non-rep_ex
	 * fallback datapath for VFs, so a failure here must abort init (the caller
	 * cleans up and returns) rather than fall back to a plain PF rep whose VFs
	 * would enumerate but never service MMIO/datapath. */
	result = vblk_pci_sriov_create_pf_rep_ex(ep_index, pf_endpoint);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("SR-IOV: dynamic PF rep creation failed for EP[%u] (%s); SR-IOV needs "
			     "rep_ex VF backing and has no non-rep_ex fallback - aborting",
			     ep_index,
			     doca_error_get_descr(result));
	return result;
}

static void vblk_pci_destroy_pf_rep(struct pci_device_config *ep)
{
	doca_error_t result;

	if (ep == NULL || ep->rep == NULL)
		return;

	result = doca_devemu_pci_type_destroy_rep(ep->rep);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy PCI type rep: %s", doca_error_get_descr(result));
	ep->rep = NULL;
	vblk_pci_sriov_release_dynamic_range(ep);
}

/* Stop a started TLP device in place; no-op when already stopped so callers
 * stay re-entrant. device_present is deliberately left to the caller. */
static doca_error_t vblk_pci_tlp_dev_suspend(struct doca_devemu_pci_tlp_dev *tlp_dev, const char *name, uint32_t idx)
{
	uint8_t started = 0;
	doca_error_t result;

	result = doca_devemu_pci_tlp_dev_is_started(tlp_dev, &started);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("%s[%u]: failed to query TLP started state: %s", name, idx, doca_error_get_descr(result));
		return result;
	}
	if (!started)
		return DOCA_SUCCESS;

	result = doca_devemu_pci_tlp_dev_stop(tlp_dev);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_ERR("%s[%u]: failed to suspend TLP device: %s", name, idx, doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("%s[%u]: TLP device suspended", name, idx);
	return DOCA_SUCCESS;
}

/* Restart a suspended TLP device on its retained handle; no-op when already
 * started. Failure is expected while FW is still invalidating the mkey; the
 * caller retries until it succeeds. */
static doca_error_t vblk_pci_tlp_dev_resume(struct doca_devemu_pci_tlp_dev *tlp_dev, const char *name, uint32_t idx)
{
	uint8_t started = 0;
	doca_error_t result;

	result = doca_devemu_pci_tlp_dev_is_started(tlp_dev, &started);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("%s[%u]: failed to query TLP started state: %s", name, idx, doca_error_get_descr(result));
		return result;
	}
	if (started)
		return DOCA_SUCCESS;

	result = doca_devemu_pci_tlp_dev_start(tlp_dev);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_DBG("%s[%u]: failed to resume TLP device: %s", name, idx, doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("%s[%u]: TLP device resumed", name, idx);
	return DOCA_SUCCESS;
}

static doca_error_t vblk_pci_sriov_start_vf_slot(struct vblk_sriov_vf_slot *slot, uint32_t ep_index)
{
	struct doca_devemu_pci_tlp_dev *tlp_dev;
	doca_error_t result;

	if (slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (slot->rep == NULL || slot->vf_dev == NULL)
		return DOCA_ERROR_NOT_FOUND;
	if (slot->vf_dev->pci_tlp_dev != NULL)
		return DOCA_SUCCESS;

	result = doca_devemu_pci_tlp_dev_create(g_tlp_ctx.pci_type, slot->rep, &tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to create EP[%u] VF[%u] TLP device: %s",
			     ep_index,
			     slot->dynamic_vf_index,
			     doca_error_get_descr(result));
		return result;
	}

	/* Per-VF MSI-X/DB resources are provisioned by the SDK as part of
	 * create_rep_ex (rep_init_attr vf_num_msix / vf_num_db); re-sizing the VF
	 * endpoint here makes the FW reject tlp_dev_start (BAD_PARAM), so the VF is
	 * started directly. */
	slot->vf_dev->pci_tlp_dev = tlp_dev;
	result = vblk_pci_sriov_vf_tlp_dev_start(slot);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to start EP[%u] VF[%u] TLP device: %s",
			     ep_index,
			     slot->dynamic_vf_index,
			     doca_error_get_descr(result));
		slot->vf_dev->pci_tlp_dev = NULL;
		(void)doca_devemu_pci_tlp_dev_destroy(tlp_dev);
		return result;
	}

	if (slot->cfg != NULL) {
		slot->cfg->tlp_dev = tlp_dev;
		slot->cfg->rep = slot->rep;
	}
	DOCA_LOG_INFO("SR-IOV: EP[%u] VF[%u] TLP device started", ep_index, slot->dynamic_vf_index);
	return DOCA_SUCCESS;
}

static void vblk_pci_sriov_destroy_vf_tlp_dev(struct vblk_sriov_vf_slot *slot)
{
	struct doca_devemu_pci_tlp_dev *tlp_dev;
	doca_error_t result;

	if (slot == NULL || slot->vf_dev == NULL)
		return;

	tlp_dev = slot->vf_dev->pci_tlp_dev;
	if (tlp_dev == NULL)
		return;

	result = vblk_pci_sriov_vf_tlp_dev_stop(slot);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("SR-IOV: destroying VF[%u] TLP device after stop failure", slot->dynamic_vf_index);
	result = doca_devemu_pci_tlp_dev_destroy(tlp_dev);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("SR-IOV: failed to destroy VF[%u] TLP device: %s",
			      slot->dynamic_vf_index,
			      doca_error_get_descr(result));

	if (slot->cfg != NULL && slot->cfg->tlp_dev == tlp_dev)
		slot->cfg->tlp_dev = NULL;
	slot->vf_dev->pci_tlp_dev = NULL;
}

static doca_error_t vblk_pci_sriov_start_vfs(struct pci_device_config *ep)
{
	struct vblk_sriov_pf_state *sriov;
	doca_error_t result;
	uint16_t total;
	uint16_t i;

	if (ep == NULL || ep->sriov == NULL || ep->sriov->vf_slots == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	sriov = ep->sriov;
	total = sriov->total_vfs;
	for (i = 0; i < total; i++) {
		result = vblk_pci_sriov_start_vf_slot(&sriov->vf_slots[i], vblk_pci_ep_index(ep));

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("SR-IOV: VF[%u] TLP start failed during EP[%u] init: %s",
				     sriov->vf_slots[i].dynamic_vf_index,
				     vblk_pci_ep_index(ep),
				     doca_error_get_descr(result));
			return result;
		}
	}

	vblk_pci_sriov_sync_vf_presence(ep, ep->device_present);
	DOCA_LOG_INFO("SR-IOV init EP[%u]: %u VF TLP devices started", vblk_pci_ep_index(ep), total);
	return DOCA_SUCCESS;
}

static void vblk_pci_sriov_close_vf_reps(struct vblk_sriov_pf_state *sriov)
{
	uint32_t i;

	if (sriov == NULL || sriov->vf_slots == NULL)
		return;

	for (i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];

		/* The OE thread must tear down the datapath before PF teardown destroys
		 * the VF TLP device it references. vf_ctrl is OE-owned and cannot be
		 * freed here; skip this VF rather than racing the OE engine. */
		if (slot->vf_ctrl != NULL) {
			DOCA_LOG_ERR("SR-IOV: VF[%u] datapath still live at rep close - skipping VF TLP/rep cleanup",
				     slot->dynamic_vf_index);
			continue;
		}
		vblk_pci_sriov_destroy_vf_tlp_dev(slot);
		if (slot->vf_dev != NULL) {
			vblk_pci_virtio_dev_destroy_vf(slot->vf_dev);
			slot->vf_dev = NULL;
		}
		if (slot->rep != NULL) {
			doca_error_t result = doca_dev_rep_close(slot->rep);

			if (result != DOCA_SUCCESS)
				DOCA_LOG_WARN("SR-IOV: failed to close VF rep[%u]: %s",
					      i,
					      doca_error_get_descr(result));
			if (slot->cfg != NULL && slot->cfg->rep == slot->rep)
				slot->cfg->rep = NULL;
		}
	}
}

static doca_error_t vblk_pci_sriov_discover_vf_reps(struct pci_device_config *ep,
						    const struct vblk_pci_virtio_dev *pf_dev)
{
	struct doca_devinfo_rep **rep_list = NULL;
	struct doca_devinfo_rep *pf_info;
	struct vblk_sriov_pf_state *sriov;
	char pf_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	uint32_t nb_reps = 0;
	uint16_t opened = 0;
	doca_error_t result;
	uint32_t i;

	if (ep == NULL || ep->sriov == NULL || ep->rep == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (!ep->sriov_dynamic_vf_range_owned)
		return DOCA_SUCCESS;

	sriov = ep->sriov;
	pf_info = doca_dev_rep_as_devinfo(ep->rep);
	if (pf_info == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	result = doca_devinfo_rep_get_vuid(pf_info, pf_vuid, sizeof(pf_vuid));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: failed to read PF VUID for EP[%u]: %s",
			     vblk_pci_ep_index(ep),
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_devinfo_rep_create_list(g_tlp_ctx.dev, DOCA_DEVINFO_REP_FILTER_EMULATED, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		/* The PF owns a dynamic VF range (create_rep_ex succeeded) but its VF
		 * representors cannot be enumerated, so the VFs cannot be backed.
		 * NOT_SUPPORTED is fatal too: returning SUCCESS here would leave every
		 * VF rep-less (present but unable to start a datapath). */
		if (result == DOCA_ERROR_NOT_SUPPORTED)
			DOCA_LOG_ERR("SR-IOV: emulated rep list unsupported for EP[%u]; cannot back VF reps",
				     vblk_pci_ep_index(ep));
		else
			DOCA_LOG_ERR("SR-IOV: failed to create emulated rep list: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < nb_reps; i++) {
		enum doca_pci_func_type func_type;
		char candidate_pf_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
		uint32_t vf_index;

		if (doca_devinfo_rep_get_pci_func_type(rep_list[i], &func_type) != DOCA_SUCCESS ||
		    func_type != DOCA_PCI_FUNC_TYPE_VF)
			continue;
		if (doca_devinfo_rep_get_pf_vuid(rep_list[i], candidate_pf_vuid, sizeof(candidate_pf_vuid)) !=
		    DOCA_SUCCESS)
			continue;
		if (strncmp(candidate_pf_vuid, pf_vuid, sizeof(pf_vuid)) != 0)
			continue;
		if (doca_devinfo_rep_get_vf_index(rep_list[i], &vf_index) != DOCA_SUCCESS)
			continue;
		/* doca_devinfo_rep_get_vf_index() returns a PF-local VF index
		 * (0..TotalVFs-1): emulated VFs are enumerated per PF and the
		 * representor's vf_index is the per-PF function index, not a global
		 * dynamic-pool index. The pf_vuid match above already restricts the
		 * list to this PF's VFs, so index the slot array directly by the
		 * PF-local vf_index. Filtering against sriov_dynamic_first_vf_idx (the
		 * PF's global pool offset) would wrongly reject every VF of any PF
		 * whose offset is nonzero, breaking multi-PF SR-IOV. */
		if (vf_index >= sriov->total_vfs)
			continue;

		if (sriov->vf_slots[vf_index].rep != NULL)
			continue;

		result = doca_dev_rep_open(rep_list[i], &sriov->vf_slots[vf_index].rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("SR-IOV: failed to open VF rep index %u: %s",
				     vf_index,
				     doca_error_get_descr(result));
			(void)doca_devinfo_rep_destroy_list(rep_list);
			vblk_pci_sriov_close_vf_reps(sriov);
			return result;
		}
		result = vblk_pci_virtio_dev_init_vf(pf_dev, ep, &sriov->vf_slots[vf_index]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("SR-IOV: failed to initialize VF[%u] virtio template: %s",
				     vf_index,
				     doca_error_get_descr(result));
			(void)doca_dev_rep_close(sriov->vf_slots[vf_index].rep);
			sriov->vf_slots[vf_index].rep = NULL;
			(void)doca_devinfo_rep_destroy_list(rep_list);
			vblk_pci_sriov_close_vf_reps(sriov);
			return result;
		}
		opened++;
	}

	(void)doca_devinfo_rep_destroy_list(rep_list);
	if (opened != sriov->total_vfs) {
		/* Every requested VF must have its own representor; a partial set would
		 * leave some VFs rep-less (present but unable to start a datapath). Fail
		 * init rather than silently expose unbacked VFs. */
		DOCA_LOG_ERR("SR-IOV: opened only %u/%u VF reps for EP[%u]; cannot back all VFs",
			     opened,
			     sriov->total_vfs,
			     vblk_pci_ep_index(ep));
		vblk_pci_sriov_close_vf_reps(sriov);
		return DOCA_ERROR_NOT_FOUND;
	}
	DOCA_LOG_INFO("SR-IOV: opened %u VF reps for EP[%u]", opened, vblk_pci_ep_index(ep));
	return DOCA_SUCCESS;
}

/*
 * Bridge config space read/write.
 * Type1 header per-register handlers with correct RO/RW/W1C semantics.
 */

/* Command register RW bits */
#define CMD_MEM_SPACE_EN 0x0002
#define CMD_BUS_MASTER_EN 0x0004
#define CMD_PERR_EN 0x0040
#define CMD_SERR_EN 0x0100
#define CMD_INT_DISABLE 0x0400

/* Status register W1C bits */
#define STS_MASTER_PERR 0x0100
#define STS_SIG_TA 0x0800
#define STS_RCV_TA 0x1000
#define STS_RCV_MA 0x2000
#define STS_SIG_SERR 0x4000
#define STS_DET_PERR 0x8000

static uint32_t bridge_hdr_read(struct pci_device_config *dc, uint32_t reg)
{
	struct pci_cfg_type1_header *h = &dc->cfg_space_hdr.type1;

	switch (reg) {
	case 0:
		return (uint32_t)h->vendor_id | ((uint32_t)h->device_id << 16);
	case 1:
		return (uint32_t)h->command | ((uint32_t)h->status << 16);
	case 2:
		return (uint32_t)h->revision_id | ((uint32_t)h->class_code << 8);
	case 3:
		return (uint32_t)h->cache_line_size | ((uint32_t)h->latency_timer << 8) |
		       ((uint32_t)h->header_type << 16) | ((uint32_t)h->bist << 24);
	case 4:
		return 0;
	case 5:
		return 0;
	case 6:
		return (uint32_t)h->primary_bus | ((uint32_t)h->secondary_bus << 8) |
		       ((uint32_t)h->subordinate_bus << 16) | ((uint32_t)h->secondary_latency << 24);
	case 7:
		return (uint32_t)h->io_base | ((uint32_t)h->io_limit << 8) | ((uint32_t)h->secondary_status << 16);
	case 8:
		return (uint32_t)(h->memory_base & 0xFFF0) | ((uint32_t)(h->memory_limit & 0xFFF0) << 16);
	case 9:
		return (uint32_t)(h->pre_memory_base & 0xFFF0) | ((uint32_t)(h->pre_memory_limit & 0xFFF0) << 16) |
		       0x00010001;
	case 10:
		return h->pre_memory_base_upper_32bit;
	case 11:
		return h->pre_memory_limit_upper_32bit;
	case 12:
		return (uint32_t)h->io_base_upper_16bit | ((uint32_t)h->io_limit_upper_16bit << 16);
	case 13:
		return (uint32_t)h->cap_ptr;
	case 14:
		return 0;
	case 15:
		return (uint32_t)h->interrupt_line | ((uint32_t)h->interrupt_pin << 8) |
		       ((uint32_t)h->bridge_control << 16);
	default:
		return 0;
	}
}

static void bridge_hdr_write_reg1(struct pci_device_config *dc, uint32_t data, uint32_t mask)
{
	uint16_t wd_cmd = data & 0xFFFF;
	uint16_t wd_sts = (data >> 16) & 0xFFFF;
	uint16_t m_cmd = mask & 0xFFFF;
	uint16_t m_sts = (mask >> 16) & 0xFFFF;
	uint16_t cmd_rw_bits[] = {CMD_MEM_SPACE_EN, CMD_BUS_MASTER_EN, CMD_PERR_EN, CMD_SERR_EN, CMD_INT_DISABLE};
	uint16_t sts_w1c_bits[] = {STS_MASTER_PERR, STS_SIG_TA, STS_RCV_TA, STS_RCV_MA, STS_SIG_SERR, STS_DET_PERR};
	uint32_t i;

	for (i = 0; i < sizeof(cmd_rw_bits) / sizeof(cmd_rw_bits[0]); i++) {
		if (m_cmd & cmd_rw_bits[i]) {
			if (wd_cmd & cmd_rw_bits[i])
				dc->cfg_space_hdr.type1.command |= cmd_rw_bits[i];
			else
				dc->cfg_space_hdr.type1.command &= ~cmd_rw_bits[i];
		}
	}
	for (i = 0; i < sizeof(sts_w1c_bits) / sizeof(sts_w1c_bits[0]); i++) {
		if ((m_sts & sts_w1c_bits[i]) && (wd_sts & sts_w1c_bits[i]))
			dc->cfg_space_hdr.type1.status &= ~sts_w1c_bits[i];
	}
}

static void bridge_hdr_write_reg8(struct pci_device_config *dc, uint32_t data, uint32_t mask)
{
	if (mask & 0xFFFF)
		dc->cfg_space_hdr.type1.memory_base = data & 0xFFF0;
	if (mask & 0xFFFF0000)
		dc->cfg_space_hdr.type1.memory_limit = (data >> 16) & 0xFFF0;
}

static void bridge_hdr_write_reg9(struct pci_device_config *dc, uint32_t data, uint32_t mask)
{
	if (mask & 0xFFFF)
		dc->cfg_space_hdr.type1.pre_memory_base = data & 0xFFF0;
	if (mask & 0xFFFF0000)
		dc->cfg_space_hdr.type1.pre_memory_limit = (data >> 16) & 0xFFF0;
}

static void bridge_hdr_write_reg15(struct pci_device_config *dc, uint32_t data, uint32_t mask)
{
	if (mask & 0xFF)
		dc->cfg_space_hdr.type1.interrupt_line = data & 0xFF;
	if (mask & 0xFFFF0000)
		dc->cfg_space_hdr.type1.bridge_control = (data >> 16) & 0xFFFF;
}

static bool is_dsp(const struct pci_device_config *dc)
{
	uint32_t idx = (uint32_t)(dc - g_tlp_ctx.devs_config);

	return idx >= FIRST_DSP_IDX(&g_tlp_ctx) && idx < FIRST_PF_IDX(&g_tlp_ctx);
}

static void bridge_handle_reg6_write(struct pci_device_config *dc, uint32_t data, uint32_t mask)
{
	uint8_t old_sec = dc->cfg_space_hdr.type1.secondary_bus;

	if (mask & 0xFF)
		dc->cfg_space_hdr.type1.primary_bus = data & 0xFF;
	if (mask & 0xFF00)
		dc->cfg_space_hdr.type1.secondary_bus = (data >> 8) & 0xFF;
	if (mask & 0xFF0000)
		dc->cfg_space_hdr.type1.subordinate_bus = (data >> 16) & 0xFF;
	if (mask & 0xFF000000)
		dc->cfg_space_hdr.type1.secondary_latency = (data >> 24) & 0xFF;

	if (is_dsp(dc) && old_sec != dc->cfg_space_hdr.type1.secondary_bus && old_sec != 0) {
		uint32_t dsp_i = (uint32_t)(dc - &g_tlp_ctx.devs_config[FIRST_DSP_IDX(&g_tlp_ctx)]);
		struct pci_device_config *ep = &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + dsp_i];

		if (ep->is_bdf_set) {
			bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1, ep->bdf);
			ep->is_bdf_set = false;
		}
	}

	if (dc->cfg_space_hdr.type1.secondary_bus != 0 && dc->cfg_space_hdr.type1.secondary_bus != 0xFF)
		DOCA_LOG_INFO("Bridge %04x bus: P=%02x S=%02x Sub=%02x",
			      dc->bdf,
			      dc->cfg_space_hdr.type1.primary_bus,
			      dc->cfg_space_hdr.type1.secondary_bus,
			      dc->cfg_space_hdr.type1.subordinate_bus);
}

static void bridge_hdr_write(struct pci_device_config *dc,
			     uint32_t reg,
			     uint32_t data,
			     uint32_t mask,
			     struct vblk_tlp_context *ctx)
{
	(void)ctx;
	switch (reg) {
	case 0:
		break;
	case 1:
		bridge_hdr_write_reg1(dc, data, mask);
		break;
	case 2:
		break;
	case 3:
		if (mask & 0xFF)
			dc->cfg_space_hdr.type1.cache_line_size = data & 0xFF;
		if (mask & 0xFF00)
			dc->cfg_space_hdr.type1.latency_timer = (data >> 8) & 0xFF;
		break;
	case 4:
		break;
	case 5:
		break;
	case 6:
		bridge_handle_reg6_write(dc, data, mask);
		break;
	case 7:
		break;
	case 8:
		bridge_hdr_write_reg8(dc, data, mask);
		break;
	case 9:
		bridge_hdr_write_reg9(dc, data, mask);
		break;
	case 10:
		if (mask)
			dc->cfg_space_hdr.type1.pre_memory_base_upper_32bit = data;
		break;
	case 11:
		if (mask)
			dc->cfg_space_hdr.type1.pre_memory_limit_upper_32bit = data;
		break;
	case 12:
		break;
	case 13:
		break;
	case 14:
		break;
	case 15:
		bridge_hdr_write_reg15(dc, data, mask);
		break;
	default:
		break;
	}
}

static uint32_t bridge_cap_read(struct pci_device_config *dc, uint32_t reg)
{
	uint32_t *d;

	if (reg >= TLP_CAP_PM_REG_NUM && reg < TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW) {
		d = (uint32_t *)&dc->caps.pm;
		return d[reg - TLP_CAP_PM_REG_NUM];
	}
	if (reg >= TLP_CAP_EXPRESS_REG_NUM && reg < TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW) {
		d = (uint32_t *)&dc->caps.express;
		return d[reg - TLP_CAP_EXPRESS_REG_NUM];
	}
	if (reg >= TLP_CAP_MSI_REG_NUM && reg < TLP_CAP_MSI_REG_NUM + TLP_CAP_MSI_LEN_DW) {
		d = (uint32_t *)&dc->caps.msi;
		return d[reg - TLP_CAP_MSI_REG_NUM];
	}
	if (reg >= TLP_CAP_MSIX_REG_NUM && reg < TLP_CAP_MSIX_REG_NUM + TLP_CAP_MSIX_LEN_DW) {
		d = (uint32_t *)&dc->caps.msix;
		return d[reg - TLP_CAP_MSIX_REG_NUM];
	}
	if (reg >= TLP_CAP_VPD_REG_NUM && reg < TLP_CAP_VPD_REG_NUM + TLP_CAP_VPD_LEN_DW) {
		d = (uint32_t *)&dc->caps.vpd;
		return d[reg - TLP_CAP_VPD_REG_NUM];
	}
	return 0;
}

/*
 * Returns true if slot_control changed (caller should trigger command_completed).
 */
static bool bridge_cap_write(struct pci_device_config *dc, uint32_t reg, uint32_t data, uint32_t mask)
{
	uint32_t *d;

	if (reg >= TLP_CAP_EXPRESS_REG_NUM && reg < TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW) {
		uint32_t idx = reg - TLP_CAP_EXPRESS_REG_NUM;

		d = (uint32_t *)&dc->caps.express;
		/* Express DW4: link_control (RW, low 16) + link_status (RO/W1C, high 16). */
		if (idx == 4) {
			if (mask & 0xFFFF) {
				uint16_t old_ctrl = dc->caps.express.link_control;
				uint16_t new_ctrl = (old_ctrl & ~(mask & 0xFFFF)) | (data & mask & 0xFFFF);

				dc->caps.express.link_control = new_ctrl;
			}
			if (mask & 0xFFFF0000) {
				uint16_t w1c = (data >> 16) & (mask >> 16) & LINK_STS_W1C;

				dc->caps.express.link_status &= ~w1c;
			}
			return false;
		}

		/* slot_control(RW) + slot_status(W1C) at Express DW offset 6 */
		if (idx == 6) {
			bool ctrl_changed = false;

			/* Lower 16 bits: slot_control (RW) */
			if (mask & 0xFFFF) {
				uint16_t old_ctrl = dc->caps.express.slot_control;
				uint16_t new_ctrl = (old_ctrl & ~(mask & 0xFFFF)) | (data & mask & 0xFFFF);

				dc->caps.express.slot_control = new_ctrl;

				if (is_dsp(dc)) {
					uint16_t cur_sts =
						__atomic_load_n(&dc->caps.express.slot_status, __ATOMIC_ACQUIRE);
					bool pds = (cur_sts & SLOT_STS_PRESENCE_DETECT_STATE) != 0;
					bool old_pwr_off = (old_ctrl & SLOT_CTRL_POWER_CONTROLLER) != 0;
					bool new_pwr_off = (new_ctrl & SLOT_CTRL_POWER_CONTROLLER) != 0;
					uint32_t di =
						(uint32_t)(dc - &g_tlp_ctx.devs_config[FIRST_DSP_IDX(&g_tlp_ctx)]);

					if (old_pwr_off && !new_pwr_off && pds) {
						/* Model link training on DSP slot power-on: expose negotiated
						 * width/speed plus DLActive. Express DW4 preserves those RO
						 * bits against host Link Status writes. */
						dc->caps.express.link_status = LINK_STS_TRAINED | LINK_STS_DL_ACTIVE;
						DOCA_LOG_INFO("Power ON: link trained (DLActive set)");
						if (di < g_tlp_ctx.num_ep)
							g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + di]
								.host_power_on_done = true;
					} else if (!old_pwr_off && new_pwr_off) {
						dc->caps.express.link_status &= ~LINK_STS_DL_ACTIVE;
						/* Empty the slot when the host powers it off: in this
						 * emulated hotplug model power-off == device removed, so
						 * drop Presence Detect State. Without this PDS stays set
						 * after the first plug forever, so a later plug presents
						 * no clean PDS 0->1 transition and the host powers the
						 * slot on then immediately back off (re-plug fails). */
						dc->caps.express.slot_status &=
							(uint16_t)~SLOT_STS_PRESENCE_DETECT_STATE;
						DOCA_LOG_INFO("Power OFF: DLActive cleared, slot emptied (PDS=0)");
						if (di < g_tlp_ctx.num_ep) {
							g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + di]
								.host_power_off = true;
							g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + di]
								.host_power_on_done = false;
						}
					}
					ctrl_changed = (new_ctrl != old_ctrl);
				}
			}

			/* Upper 16 bits: slot_status (W1C)
			 * slot_status writer is now TLP-only (bridge_cap_write W1C here +
			 * vblk_pci_trigger_hotplug OR from vblk_pci_retry_hotplug_msi),
			 * so the atomic RMW is defensive rather than strictly required. */
			if (mask & 0xFFFF0000) {
				uint16_t w1c = (data >> 16) & (mask >> 16);
				__atomic_fetch_and(&dc->caps.express.slot_status, ~w1c, __ATOMIC_ACQ_REL);
			}

			return ctrl_changed;
		}
		/* DW 2 = dev_control(RW) + dev_status(W1C bits 0-3).
		 * DW 4 (link_control/link_status) is handled above.
		 * DW 1/3/5 are RO capability registers.
		 * Without per-DW W1C handling, a host write that clears LBMS would
		 * also zero Speed/Width/DLLLA and break link train on the next replug. */
		if (idx == 2) {
			if (mask & 0xFFFF)
				dc->caps.express.dev_control = (dc->caps.express.dev_control & ~(mask & 0xFFFF)) |
							       (data & mask & 0xFFFF);
			if (mask & 0xFFFF0000)
				dc->caps.express.dev_status &= ~(((data >> 16) & (mask >> 16)) & DEV_STS_W1C_MASK);
			return false;
		}
		if (idx == 1 || idx == 3 || idx == 5)
			return false;
		d[idx] = (d[idx] & ~mask) | (data & mask);
	} else if (reg >= TLP_CAP_MSI_REG_NUM && reg < TLP_CAP_MSI_REG_NUM + TLP_CAP_MSI_LEN_DW) {
		d = (uint32_t *)&dc->caps.msi;
		uint32_t idx = reg - TLP_CAP_MSI_REG_NUM;
		d[idx] = (d[idx] & ~mask) | (data & mask);
	}
	return false;
}

static uint32_t bridge_cfg_read32(struct pci_device_config *dc, uint32_t reg)
{
	return (reg < PCI_STD_HEADER_REG_NUM) ? bridge_hdr_read(dc, reg) : bridge_cap_read(dc, reg);
}

static bool vblk_pci_has_acs_ext_cap_reg(const struct pci_device_config *dc, uint32_t reg)
{
	return is_dsp(dc) && reg >= PCIE_EXT_CAP_FIRST_REG_NUM &&
	       reg < PCIE_EXT_CAP_FIRST_REG_NUM + (PCI_ACS_EGRESS_CTL_V / sizeof(uint32_t));
}

static uint32_t vblk_pci_acs_ext_cap_read_dw(uint32_t cap_dw)
{
	switch (cap_dw) {
	case 0:
		return (uint32_t)PCI_EXT_CAP_ID_ACS | (1u << 16);
	case PCI_ACS_CAP / sizeof(uint32_t):
		/* Force linux hard-wired enable. */
		return 0;
	default:
		return 0;
	}
}

static uint32_t vblk_pci_sriov_ext_cap_read_dw(uint32_t cap_dw, const struct vblk_sriov_pf_state *s)
{
	const uint32_t vf_bar_dw = PCI_SRIOV_BAR / sizeof(uint32_t);

	/* PCI_SRIOV_BAR..VF_BAR5: PF-owned SR-IOV VF BAR shadow registers. */
	if (cap_dw >= vf_bar_dw && cap_dw < vf_bar_dw + PCI_SRIOV_NUM_BARS)
		return s->vf_bar[cap_dw - vf_bar_dw];

	switch (cap_dw) {
	case 0:
		/* Extended capability header: SR-IOV, version 1, no next extended capability. */
		return (uint32_t)PCI_EXT_CAP_ID_SRIOV | (1u << 16);
	case PCI_SRIOV_CAP / sizeof(uint32_t):
		/* SR-IOV Capabilities: VF migration and migration interrupt are not supported. */
		return 0;
	case PCI_SRIOV_CTRL / sizeof(uint32_t):
		/* SR-IOV Control (low 16b): host-controlled VFE/MSE, ARI stays clear.
		 * Status (high 16b) is RO 0 - VF migration unsupported. */
		return (uint32_t)s->control_shadow;
	case PCI_SRIOV_INITIAL_VF / sizeof(uint32_t):
		/* InitialVFs/TotalVFs: both are the configured no-ARI VF count. */
		return (uint32_t)s->total_vfs | ((uint32_t)s->total_vfs << 16);
	case PCI_SRIOV_NUM_VF / sizeof(uint32_t):
		/* NumVFs/Function Dependency Link: host-selected VF count; no dependency link. */
		return (uint32_t)s->num_vfs_shadow;
	case PCI_SRIOV_VF_OFFSET / sizeof(uint32_t):
		/* FirstVFOffset/VFStride: no-ARI VFs occupy consecutive functions after the PF. */
		return (uint32_t)s->first_vf_offset | ((uint32_t)s->vf_stride << 16);
	case PCI_SRIOV_VF_DID / sizeof(uint32_t):
		/* VFDeviceID is in the upper half of this DW and matches the virtio-blk PF ID. */
		return (uint32_t)s->vf_device_id << 16;
	case PCI_SRIOV_SUP_PGSIZE / sizeof(uint32_t):
		/* Supported Page Sizes: bit 0 only, meaning 4 KB. */
		return VBLK_SRIOV_SUPPORTED_PAGE_SIZE_MASK;
	case PCI_SRIOV_SYS_PGSIZE / sizeof(uint32_t):
		/* System Page Size: default 4 KB selection, host-writable while VFE is clear. */
		return s->system_pg_size;
	case PCI_SRIOV_VFM / sizeof(uint32_t):
	default:
		/* VF migration state array and any reserved holes are not implemented. */
		return 0;
	}
}

static void vblk_pci_sriov_vf_bar_write(struct vblk_sriov_pf_state *sriov, uint32_t bar_idx, uint32_t value)
{
	const uint32_t bar0_type = PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH;
	const uint64_t bar_size_mask = ~((1ULL << VBLK_PCI_VIRTIO_BAR_LOG_SIZE) - 1ULL);

	if (bar_idx == 0) {
		if (value == 0xFFFFFFFFu)
			sriov->vf_bar[0] = ((uint32_t)bar_size_mask & (uint32_t)PCI_BASE_ADDRESS_MEM_MASK) | bar0_type;
		else
			sriov->vf_bar[0] = (value & (uint32_t)PCI_BASE_ADDRESS_MEM_MASK) | bar0_type;
		return;
	}

	if (bar_idx == 1) {
		if (value == 0xFFFFFFFFu)
			sriov->vf_bar[1] = (uint32_t)(bar_size_mask >> 32);
		else
			sriov->vf_bar[1] = value;
		return;
	}

	sriov->vf_bar[bar_idx] = 0;
}

static void vblk_pci_sriov_cap_write_dw(struct pci_device_config *dc,
					struct vblk_sriov_pf_state *sriov,
					uint32_t cap_dw,
					uint32_t new_value)
{
	const uint32_t vf_bar_dw = PCI_SRIOV_BAR / sizeof(uint32_t);
	const uint16_t ctrl_rw_mask = PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE;
	const uint16_t unsupported_ctrl_mask = PCI_SRIOV_CTRL_VFM | PCI_SRIOV_CTRL_INTR | PCI_SRIOV_CTRL_ARI;

	if (cap_dw >= vf_bar_dw && cap_dw < vf_bar_dw + PCI_SRIOV_NUM_BARS) {
		uint32_t bar_idx = cap_dw - vf_bar_dw;
		bool vf_decode_enabled = (sriov->control_shadow & (PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE)) ==
					 (PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE);

		if (vf_decode_enabled && new_value != sriov->vf_bar[bar_idx]) {
			DOCA_LOG_WARN("SR-IOV VF BAR%u write rejected while VFE|MSE=1 for EP[%u] from %#x -> %#x",
				      bar_idx,
				      vblk_pci_ep_index(dc),
				      sriov->vf_bar[bar_idx],
				      new_value);
			return;
		}
		vblk_pci_sriov_vf_bar_write(sriov, bar_idx, new_value);
		return;
	}

	switch (cap_dw) {
	case 0: /* Extended capability header: RO. */
	case PCI_SRIOV_CAP / sizeof(uint32_t):
	case PCI_SRIOV_INITIAL_VF / sizeof(uint32_t):
	case PCI_SRIOV_VF_OFFSET / sizeof(uint32_t):
	case PCI_SRIOV_VF_DID / sizeof(uint32_t):
	case PCI_SRIOV_SUP_PGSIZE / sizeof(uint32_t):
	case PCI_SRIOV_VFM / sizeof(uint32_t):
		return;
	case PCI_SRIOV_CTRL / sizeof(uint32_t): {
		uint16_t new_ctrl = (uint16_t)(new_value & 0xFFFFu);
		uint16_t old_ctrl = sriov->control_shadow;
		uint16_t merged_ctrl = (uint16_t)((old_ctrl & ~ctrl_rw_mask) | (new_ctrl & ctrl_rw_mask));

		if (new_ctrl & unsupported_ctrl_mask) {
			DOCA_LOG_DBG("SR-IOV control write ignored unsupported bits 0x%04x for EP[%u]",
				     new_ctrl & unsupported_ctrl_mask,
				     vblk_pci_ep_index(dc));
		}
		if ((old_ctrl ^ merged_ctrl) & PCI_SRIOV_CTRL_VFE) {
			DOCA_LOG_INFO("SR-IOV VFE EP[%u]: %u -> %u (NumVFs=%u, TotalVFs=%u)",
				      vblk_pci_ep_index(dc),
				      !!(old_ctrl & PCI_SRIOV_CTRL_VFE),
				      !!(merged_ctrl & PCI_SRIOV_CTRL_VFE),
				      sriov->num_vfs_shadow,
				      sriov->total_vfs);
		}
		if ((old_ctrl ^ merged_ctrl) & PCI_SRIOV_CTRL_MSE) {
			DOCA_LOG_INFO("SR-IOV VF MSE EP[%u]: %u -> %u",
				      vblk_pci_ep_index(dc),
				      !!(old_ctrl & PCI_SRIOV_CTRL_MSE),
				      !!(merged_ctrl & PCI_SRIOV_CTRL_MSE));
		}
		sriov->control_shadow = merged_ctrl;
		return;
	}
	case PCI_SRIOV_NUM_VF / sizeof(uint32_t): {
		uint16_t requested = (uint16_t)(new_value & 0xFFFFu);
		uint16_t clamped = requested > sriov->total_vfs ? sriov->total_vfs : requested;

		/* NumVFs is read-only while VF Enable is set. The host must clear VFE
		 * before changing NumVFs. */
		if (sriov->control_shadow & PCI_SRIOV_CTRL_VFE) {
			if (clamped != sriov->num_vfs_shadow)
				DOCA_LOG_WARN(
					"SR-IOV NumVFs write rejected while VFE=1 for EP[%u] (current=%u requested=%u)",
					vblk_pci_ep_index(dc),
					sriov->num_vfs_shadow,
					requested);
			return;
		}
		if (clamped != requested) {
			DOCA_LOG_INFO("SR-IOV NumVFs EP[%u]: requested %u clamped to %u (TotalVFs=%u)",
				      vblk_pci_ep_index(dc),
				      requested,
				      clamped,
				      sriov->total_vfs);
		}
		if (clamped != sriov->num_vfs_shadow) {
			DOCA_LOG_INFO("SR-IOV NumVFs EP[%u]: %u -> %u",
				      vblk_pci_ep_index(dc),
				      sriov->num_vfs_shadow,
				      clamped);
		}
		sriov->num_vfs_shadow = clamped;
		/* Function Dependency Link is RO=0; no dependencies are modeled. */
		return;
	}
	case PCI_SRIOV_SYS_PGSIZE / sizeof(uint32_t): {
		uint32_t requested = new_value;

		if ((sriov->control_shadow & PCI_SRIOV_CTRL_VFE) && requested != sriov->system_pg_size) {
			DOCA_LOG_WARN(
				"SR-IOV SystemPageSize write rejected while VFE=1 for EP[%u] (current=0x%08x requested=0x%08x)",
				vblk_pci_ep_index(dc),
				sriov->system_pg_size,
				requested);
			return;
		}
		if (requested != VBLK_SRIOV_SUPPORTED_PAGE_SIZE_MASK) {
			DOCA_LOG_WARN("SR-IOV SystemPageSize rejected for EP[%u]: requested=0x%08x supported=0x%08x",
				      vblk_pci_ep_index(dc),
				      requested,
				      VBLK_SRIOV_SUPPORTED_PAGE_SIZE_MASK);
			return;
		}
		if (requested != sriov->system_pg_size) {
			DOCA_LOG_INFO("SR-IOV SystemPageSize EP[%u]: 0x%08x -> 0x%08x",
				      vblk_pci_ep_index(dc),
				      sriov->system_pg_size,
				      requested);
		}
		sriov->system_pg_size = requested;
		return;
	}
	default:
		/* Reserved holes are read-only. */
		return;
	}
}

static inline void vblk_pci_sriov_ext_cap_write(uint32_t reg, uint32_t data, uint32_t mask, struct pci_device_config *dc)
{
	uint32_t cap_dw = reg - PCIE_EXT_CAP_FIRST_REG_NUM;
	uint32_t old_value = vblk_pci_sriov_ext_cap_read_dw(cap_dw, dc->sriov);
	uint32_t requested_value = (old_value & ~mask) | (data & mask);

	vblk_pci_sriov_cap_write_dw(dc, dc->sriov, cap_dw, requested_value);
	vblk_pci_sriov_sync_vf_presence(dc, dc->device_present);
	DOCA_LOG_DBG("SR-IOV cap write EP[%u]: cap_dw=%u mask=0x%08x data=0x%08x old=0x%08x requested=0x%08x",
		     vblk_pci_ep_index(dc),
		     cap_dw,
		     mask,
		     data,
		     old_value,
		     requested_value);
}

/*
 * PCI Switch Bridge Initialization
 */

/*
 * Bridge initialization
 */

static void init_type1_header(struct pci_device_config *dc)
{
	struct pci_cfg_type1_header *h = &dc->cfg_space_hdr.type1;

	dc->is_bridge = true;
	dc->is_endpoint = false;
	dc->is_dummy = false;
	h->vendor_id = TLP_PCI_TYPE_VENDOR_ID;
	h->device_id = TLP_PCI_TYPE_BRIDGE_DEVICE_ID;
	h->command = 0x0400;
	h->status = 0x0010;
	h->class_code = TLP_PCI_CLASS_CODE_BRIDGE;
	h->revision_id = TLP_PCI_TYPE_REVISION_ID;
	h->header_type = HEADER_TYPE_BRIDGE;
	h->pre_memory_base = 0x0001;
	h->pre_memory_limit = 0x0001;
	h->cap_ptr = TLP_PCI_CAP_OFFSET_EXPRESS;
}

static void init_usp_capabilities(struct pci_device_config *dc)
{
	/* Express → PM → end */
	dc->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dc->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dc->caps.express.pcie_cap_register = TLP_PCIE_CAP_REG(TLP_PCIE_PORT_TYPE_UPSTREAM, false);
	dc->caps.express.dev_capabilities = 0x00008001;
	dc->caps.express.link_capabilities = LINK_CAP_INIT(0);
	dc->caps.express.link_status = LINK_STS_TRAINED;

	dc->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dc->caps.pm.next_cap_ptr = 0x00;
	dc->caps.pm.pmc = 0xC803;
	dc->caps.pm.pmcsr = 0x0008;

	memset(&dc->caps.msi, 0, sizeof(dc->caps.msi));
	memset(&dc->caps.vpd, 0, sizeof(dc->caps.vpd));
}

static void init_dsp_capabilities(struct pci_device_config *dc, uint32_t slot_number, bool hp)
{
	/* Express → MSI → PM → end */
	dc->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dc->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_MSI;
	dc->caps.express.pcie_cap_register = TLP_PCIE_CAP_REG(TLP_PCIE_PORT_TYPE_DOWNSTREAM, hp);
	dc->caps.express.dev_capabilities = 0x00008001;
	dc->caps.express.link_capabilities = LINK_CAP_INIT(1);
	dc->caps.express.link_status = LINK_STS_TRAINED;
	dc->caps.express.slot_capabilities = 0;
	dc->caps.express.slot_control = 0;
	dc->caps.express.slot_status = 0;

	if (hp) {
		dc->caps.express.slot_capabilities =
			(SLOT_CAP_PWR_CTRL_PRESENT | SLOT_CAP_HP_SURPRISE | SLOT_CAP_HP_CAPABLE |
			 (TLP_BRIDGE_SLOT_PWR_LIMIT << SLOT_CAP_PWR_LIMIT_VALUE_SHIFT) |
			 (slot_number << SLOT_CAP_PHYS_SLOT_NUM_SHIFT));
		dc->caps.express.slot_control = SLOT_CTRL_POWER_CONTROLLER;
	} else {
		dc->caps.express.link_status |= LINK_STS_DL_ACTIVE;
	}

	dc->caps.msi.cap_id = TLP_PCI_CAP_ID_MSI;
	dc->caps.msi.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dc->caps.msi.message_control = 0x0080;

	dc->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dc->caps.pm.next_cap_ptr = 0x00;
	dc->caps.pm.pmc = 0xC803;
	dc->caps.pm.pmcsr = 0x0008;

	memset(&dc->caps.vpd, 0, sizeof(dc->caps.vpd));
}

static void init_usp_bridge(struct vblk_tlp_context *ctx)
{
	struct pci_device_config *usp = &ctx->devs_config[USP_IDX(ctx)];

	init_type1_header(usp);
	init_usp_capabilities(usp);
	usp->device = 0x00;
	usp->function = 0x00;
	DOCA_LOG_INFO("Initialized USP bridge (idx %d)", USP_IDX(ctx));
}

static void init_dsp_bridge(struct vblk_tlp_context *ctx, uint32_t i, bool hp)
{
	struct pci_device_config *dsp = &ctx->devs_config[FIRST_DSP_IDX(ctx) + i];

	init_type1_header(dsp);
	/* Physical Slot Number = 0 forces Linux to use PCI geographic naming */
	init_dsp_capabilities(dsp, 0, hp);
	dsp->device = i;
	dsp->function = 0x00;
	DOCA_LOG_INFO("Initialized DSP[%u] (hotplug=%d)", i, hp);
}

static doca_error_t vblk_pci_switch_init(struct vblk_tlp_context *ctx, uint32_t num_ep, bool hotplug)
{
	uint32_t num_sriov_vfs;
	uint32_t i;

	ctx->num_ep = num_ep;
	ctx->num_dsp = num_ep;
	ctx->num_bridges = MAX_NUM_USP + num_ep;
	num_sriov_vfs = NUM_SRIOV_VFS(ctx);
	ctx->num_devices = ctx->num_bridges + num_ep + num_sriov_vfs + DUMMY_DEV_NUM;
	ctx->hotplug_mode = hotplug;

	ctx->devs_config = calloc(ctx->num_devices, sizeof(*ctx->devs_config));
	if (!ctx->devs_config)
		return DOCA_ERROR_NO_MEMORY;
	ctx->bdf_entries = calloc(ctx->num_devices, sizeof(*ctx->bdf_entries));
	if (!ctx->bdf_entries) {
		free(ctx->devs_config);
		return DOCA_ERROR_NO_MEMORY;
	}
	ctx->virtio_devs = calloc(num_ep, sizeof(*ctx->virtio_devs));
	if (!ctx->virtio_devs) {
		free(ctx->bdf_entries);
		free(ctx->devs_config);
		return DOCA_ERROR_NO_MEMORY;
	}
	memset(ctx->bdf_map, 0, sizeof(ctx->bdf_map));

	init_usp_bridge(ctx);
	for (i = 0; i < num_ep; i++)
		init_dsp_bridge(ctx, i, hotplug);
	for (i = 0; i < num_ep; i++) {
		struct pci_device_config *ep = &ctx->devs_config[FIRST_PF_IDX(ctx) + i];

		memset(ep, 0, sizeof(*ep));
		ep->is_endpoint = true;
		ep->cfg_space_hdr.type0.vendor_id = TLP_PCI_TYPE_VENDOR_ID;
		ep->cfg_space_hdr.type0.device_id = TLP_PCI_TYPE_ENDPOINT_DEVICE_ID;
		ep->device_present = false;
		ep->pending_unplug = false;
		ep->host_power_off = false;
		ep->host_power_on_done = false;
	}
	for (i = 0; i < num_sriov_vfs; i++) {
		struct pci_device_config *vf = &ctx->devs_config[FIRST_VF_IDX(ctx) + i];

		memset(vf, 0, sizeof(*vf));
		vf->is_endpoint = true;
		/* Vendor/device IDs are resolved from the initialized PF config. */
		vf->device_present = false;
	}
	/* Dummy device */
	struct pci_device_config *d = &ctx->devs_config[DUMMY_DEV_IDX(ctx)];

	memset(d, 0, sizeof(*d));
	d->is_dummy = true;
	d->cfg_space_hdr.type0.vendor_id = 0xFFFF;
	d->cfg_space_hdr.type0.device_id = 0xFFFF;

	if (ctx->sriov_enabled) {
		DOCA_LOG_INFO("PCI Switch: %u USP + %u DSP + %u PF + %u VF + 1 dummy = %u",
			      MAX_NUM_USP,
			      num_ep,
			      num_ep,
			      num_sriov_vfs,
			      ctx->num_devices);
	} else {
		DOCA_LOG_INFO("PCI Switch: %u USP + %u DSP + %u PF + 1 dummy = %u",
			      MAX_NUM_USP,
			      num_ep,
			      num_ep,
			      ctx->num_devices);
	}
	return DOCA_SUCCESS;
}

/************************************************************************
 ******      Modular VirtIO PCIe Device Configuration Functions     ******
 ************************************************************************/

/**
 * @brief Setup PCI configuration header
 *
 * Configures the standard PCI Type 0 configuration header with VirtIO block
 * device values including vendor ID, device class, and capabilities pointer.
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_pci_header(struct pcie_virtio_dev *dev)
{
	struct pci_config_header_type0 *regs = &dev->cfg.regs;

	regs->vendor_id = VBLK_PCI_VIRTIO_VENDOR_ID;
	regs->subsystem_vendor_id = VBLK_PCI_VIRTIO_VENDOR_ID;
	regs->device_id = 0;		    /* Replaced by real device id at init */
	regs->status = PCI_STATUS_CAP_LIST; /* Cap ptr is valid */
	regs->class_code = PCI_CLASS_STORAGE;
	regs->subclass = PCI_SUBCLASS_STORAGE_NVM;
	regs->revision_id = 1;

	/* Link PCIe Express capability into the chain (required to support FLR) */
	regs->capabilities_ptr = offsetof(struct pcie_virtio_dev, cfg.pcie_cap);

	regs->base_address[VBLK_PCI_VIRTIO_BAR_ID] = PCI_BASE_ADDRESS_MEM_PREFETCH | PCI_BASE_ADDRESS_MEM_TYPE_64;
}

/**
 * @brief Setup PCIe Express capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_pcie_capability(struct pcie_virtio_dev *dev)
{
	struct pcie_capability *pcie_cap = &dev->cfg.pcie_cap;

	pcie_cap->cap.cap_id = PCI_CAP_ID_EXP;
	pcie_cap->pcie_cap = (PCI_EXP_TYPE_ENDPOINT << 4) | PCIE_CAP_VERSION_2;
	pcie_cap->dev_cap = PCI_EXP_DEVCAP_FLR | PCIE_DEVCAP_MPSS_256B;

	pcie_cap->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.msix_cap);
}

/**
 * @brief Setup MSI-X capability
 *
 * Configures MSI-X capability with table and PBA locations within the BAR.
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_msix_capability(struct pcie_virtio_dev *dev)
{
	struct msix_capability *msix_cap = &dev->cfg.msix_cap;

	msix_cap->cap.cap_id = PCI_CAP_ID_MSIX;
	msix_cap->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.common_cfg);
	msix_cap->msgctl = VBLK_PCI_MAX_NUM_MSIX - 1; /* MSI-X Disabled by default | Table Size (zero based) */
	/* Note: lower bits are masked and not shifted */
	msix_cap->table_offset = VBLK_PCI_VIRTIO_MSIX_TABLE_OFFSET | VBLK_PCI_VIRTIO_BAR_ID;
	msix_cap->pba_offset = VBLK_PCI_VIRTIO_MSIX_PBA_OFFSET | VBLK_PCI_VIRTIO_BAR_ID;
}

/**
 * @brief Setup VirtIO Common Configuration capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_common_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *common_cfg = &dev->cfg.common_cfg;

	common_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	common_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.notify_cfg);
	common_cfg->cap_len = sizeof(struct virtio_pci_capability);
	common_cfg->cfg_type = VIRTIO_PCI_CAP_COMMON_CFG;
	common_cfg->bar = 0;
	common_cfg->offset = 0;
	common_cfg->length = VBLK_PCI_VIRTIO_PCI_CFG_LEN;
}

/**
 * @brief Setup VirtIO Notification capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_notify_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_notify_capability *notify_cfg = &dev->cfg.notify_cfg;

	notify_cfg->base.cap.cap_id = PCI_CAP_ID_VNDR;
	notify_cfg->base.cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.isr_cfg);
	notify_cfg->base.cap_len = sizeof(struct virtio_pci_notify_capability);
	notify_cfg->base.cfg_type = VIRTIO_PCI_CAP_NOTIFY_CFG;
	notify_cfg->base.bar = 0;
	notify_cfg->base.offset = VBLK_PCI_VIRTIO_DB_OFFSET;
	notify_cfg->base.length = VBLK_PCI_VIRTIO_DB_LEN;
	notify_cfg->notify_off_multiplier = VBLK_PCI_VIRTIO_DB_NOTIFY_BY_OFFSET << VBLK_PCI_VIRTIO_DB_STRIDE;
}

/**
 * @brief Setup VirtIO ISR Status capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_isr_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *isr_cfg = &dev->cfg.isr_cfg;

	isr_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	isr_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.device_cfg);
	isr_cfg->cap_len = sizeof(struct virtio_pci_capability);
	isr_cfg->cfg_type = VIRTIO_PCI_CAP_ISR_CFG;
	isr_cfg->bar = 0;
	isr_cfg->offset = VBLK_PCI_VIRTIO_ISR_CFG_OFFSET;
	isr_cfg->length = VBLK_PCI_VIRTIO_ISR_CFG_LEN;
}

/**
 * @brief Setup VirtIO Device Configuration capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_device_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *device_cfg = &dev->cfg.device_cfg;

	device_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	device_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.pci_cfg);
	device_cfg->cap_len = sizeof(struct virtio_pci_capability);
	device_cfg->cfg_type = VIRTIO_PCI_CAP_DEVICE_CFG;
	device_cfg->bar = 0;
	device_cfg->offset = VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
	device_cfg->length = VBLK_PCI_VIRTIO_DEV_CFG_LEN;
}

/**
 * @brief Setup VirtIO PCI Configuration Access capability (end of chain)
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_pci_cfg_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_cfg_capability *pci_cfg = &dev->cfg.pci_cfg;

	/* Must be present, but not fully supported.
	 * Spec 4.1.4.9: cap.bar, cap.length, cap.offset and pci_cfg_data
	 * are read-write (RW) for the driver. */
	pci_cfg->base.cap.cap_id = PCI_CAP_ID_VNDR;
	pci_cfg->base.cap.next_ptr = 0; /* End of capability chain */
	pci_cfg->base.cap_len = sizeof(struct virtio_pci_cfg_capability);
	pci_cfg->base.cfg_type = VIRTIO_PCI_CAP_PCI_CFG;
	pci_cfg->base.bar = 0;
	pci_cfg->base.offset = 0;
	pci_cfg->base.length = 0;
}

/**
 * @brief Setup BAR memory mapping configuration
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_bar_mapping(struct pcie_virtio_dev *dev)
{
	dev->bar64_map[VBLK_PCI_VIRTIO_BAR_ID].log_size = VBLK_PCI_VIRTIO_BAR_LOG_SIZE;
}

/**
 * @brief Set default values for VirtIO block device configuration
 *
 * Creates a complete VirtIO device configuration with all required capabilities
 * configured according to VirtIO specification. The configuration provides default
 * values that may be overridden by vblk_pci_fixup_virtio_dev() at runtime.
 *
 * @return Complete VirtIO device configuration ready for use
 */
static struct pcie_virtio_dev vblk_pci_set_default_values(void)
{
	struct pcie_virtio_dev dev = {0};

	vblk_pci_setup_pci_header(&dev);
	vblk_pci_setup_pcie_capability(&dev);
	vblk_pci_setup_msix_capability(&dev);
	vblk_pci_setup_virtio_common_capability(&dev);
	vblk_pci_setup_virtio_notify_capability(&dev);
	vblk_pci_setup_virtio_isr_capability(&dev);
	vblk_pci_setup_virtio_device_capability(&dev);
	vblk_pci_setup_virtio_pci_cfg_capability(&dev);
	vblk_pci_setup_bar_mapping(&dev);

	return dev;
}

static doca_error_t vblk_pci_sriov_reset_vf_slot(struct vblk_sriov_vf_slot *slot)
{
	struct vblk_pci_virtio_dev *vf_dev;

	if (slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (slot->rep == NULL || slot->vf_dev == NULL)
		return DOCA_ERROR_NOT_FOUND;
	if (slot->vf_dev->pci_tlp_dev == NULL)
		return DOCA_SUCCESS;

	/* PCIe FLR resets the VF function. Scrub the guest-writable VF config
	 * shadow on the TLP thread, which owns slot->pcie_dev, so reads during
	 * the FLR window reflect reset state. Keep RO/HwInit fields intact. */
	slot->pcie_dev.cfg.regs.command &= (uint16_t) ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	slot->pcie_dev.cfg.msix_cap.msgctl &= (uint16_t) ~(PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);

	vf_dev = slot->vf_dev;
	vf_dev->pci_cfg.device_status = 0;
	vblk_pci_virtio_dev_reset(vf_dev);
	atomic_store_explicit(&vf_dev->flr_kickoff, false, memory_order_release);
	atomic_store_explicit(&vf_dev->flr_in_progress, true, memory_order_release);
	if (!vblk_pci_virtio_cfg_change_notify(vf_dev))
		atomic_store_explicit(&vf_dev->flr_in_progress, false, memory_order_release);

	DOCA_LOG_INFO("SR-IOV: VF[%u] FLR reset VF PCI config and started VF datapath recycle", slot->dynamic_vf_index);
	return DOCA_SUCCESS;
}

doca_error_t vblk_pci_sriov_vf_tlp_dev_stop(struct vblk_sriov_vf_slot *slot)
{
	doca_error_t result;

	if (slot == NULL || slot->vf_dev == NULL || slot->vf_dev->pci_tlp_dev == NULL)
		return DOCA_SUCCESS;

	result = vblk_pci_tlp_dev_suspend(slot->vf_dev->pci_tlp_dev, "SR-IOV: VF", slot->dynamic_vf_index);
	if (result != DOCA_SUCCESS)
		return result;

	/* Removal semantics on top of the shared suspend: the VF leaves the bus. */
	slot->tlp_started = false;
	if (slot->cfg != NULL)
		slot->cfg->device_present = false;
	return DOCA_SUCCESS;
}

doca_error_t vblk_pci_sriov_vf_tlp_dev_suspend(struct vblk_sriov_vf_slot *slot)
{
	if (slot == NULL || slot->vf_dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Unlike vf_tlp_dev_stop, keep device_present and tlp_started: a suspended
	 * VF stays on the bus and still answers config cycles. */
	return vblk_pci_tlp_dev_suspend(slot->vf_dev->pci_tlp_dev, "SR-IOV: VF", slot->dynamic_vf_index);
}

doca_error_t vblk_pci_sriov_vf_tlp_dev_resume(struct vblk_sriov_vf_slot *slot)
{
	if (slot == NULL || slot->vf_dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	return vblk_pci_tlp_dev_resume(slot->vf_dev->pci_tlp_dev, "SR-IOV: VF", slot->dynamic_vf_index);
}

doca_error_t vblk_pci_sriov_vf_tlp_dev_start(struct vblk_sriov_vf_slot *slot)
{
	doca_error_t result;

	if (slot == NULL || slot->vf_dev == NULL || slot->vf_dev->pci_tlp_dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	result = vblk_pci_tlp_dev_resume(slot->vf_dev->pci_tlp_dev, "SR-IOV: VF", slot->dynamic_vf_index);
	if (result != DOCA_SUCCESS)
		return result;

	/* Started semantics on top of the shared resume: the slot lifecycle mark. */
	slot->tlp_started = true;
	return DOCA_SUCCESS;
}

doca_error_t vblk_pci_pf_tlp_dev_suspend(struct vblk_pci_virtio_dev *dev)
{
	if (dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* device_present stays set: the suspended PF still answers config cycles. */
	return vblk_pci_tlp_dev_suspend(dev->pci_tlp_dev, "PF EP", dev->ep_index);
}

doca_error_t vblk_pci_pf_tlp_dev_resume(struct vblk_pci_virtio_dev *dev)
{
	if (dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	return vblk_pci_tlp_dev_resume(dev->pci_tlp_dev, "PF EP", dev->ep_index);
}

static void vblk_pci_sriov_set_vf_datapath_wanted(struct pci_device_config *ep, bool want_up)
{
	struct vblk_sriov_pf_state *sriov;
	uint16_t total;
	uint16_t changed = 0;
	uint16_t i;

	if (ep == NULL || ep->sriov == NULL || ep->sriov->vf_slots == NULL)
		return;

	sriov = ep->sriov;
	total = want_up ? sriov->num_vfs_shadow : sriov->total_vfs;
	if (total > sriov->total_vfs)
		total = sriov->total_vfs;

	for (i = 0; i < total; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];

		if (want_up && (slot->vf_dev == NULL || slot->vf_dev->pci_tlp_dev == NULL)) {
			DOCA_LOG_ERR("SR-IOV: VF[%u] datapath requested before init-time TLP device is ready",
				     slot->dynamic_vf_index);
			force_quit = true;
			return;
		}
		if (atomic_exchange_explicit(&slot->dp_want_up, want_up, memory_order_acq_rel) != want_up)
			changed++;
	}

	if (changed == 0) {
		DOCA_LOG_INFO("SR-IOV VFE EP[%u]: VF offload intent unchanged (%s) for %u VF(s)",
			      vblk_pci_ep_index(ep),
			      want_up ? "enable" : "disable",
			      total);
	} else {
		DOCA_LOG_INFO("SR-IOV VFE EP[%u]: VF offload intent changed to %s for %u/%u VF(s)",
			      vblk_pci_ep_index(ep),
			      want_up ? "enable" : "disable",
			      changed,
			      total);
	}
}

static void vblk_pci_sriov_handle_pf_flr(struct pci_device_config *ep)
{
	struct vblk_sriov_pf_state *sriov;

	if (ep == NULL || ep->sriov == NULL)
		return;

	sriov = ep->sriov;
	if (sriov->total_vfs == 0)
		return;

	/* A PF FLR resets the PF's SR-IOV capability in hardware. Model that by
	 * resetting the control word (VFE|MSE), NumVFs, and System Page Size to their
	 * power-on (disabled) state. Linux does not remove VF devices across a PF reset:
	 * pci_reset_function() saves the PF config, including the SR-IOV cap, before
	 * the FLR and restores it afterwards via pci_restore_state() ->
	 * pci_restore_iov_state() -> sriov_restore_state(). The restore re-writes
	 * NumVFs while VFE is still 0, then re-writes VFE|MSE.
	 *
	 * Clearing VFE gates VF config/MMIO and disables VF offload intent through
	 * the same path as a host VFE 1->0 write. Keep VF TLP devices, vf_ctrl
	 * objects, VF virtio shadows, and config callbacks alive; Linux restores the
	 * PF SR-IOV cap and re-writes VFE|MSE, which re-enables VF offload through
	 * the normal VFE 0->1 path. */
	sriov->control_shadow &= (uint16_t) ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE);
	sriov->num_vfs_shadow = 0;
	sriov->system_pg_size = VBLK_SRIOV_SUPPORTED_PAGE_SIZE_MASK;
	/* Keep VF BAR shadows; Linux restores VFE over the existing VF resources. */
	vblk_pci_sriov_set_vf_datapath_wanted(ep, false);

	DOCA_LOG_INFO("SR-IOV: PF[%u] FLR reset SR-IOV cap (VFE/MSE/NumVFs cleared); "
		      "VF offload engines remain disabled until Linux restore re-enables VFE",
		      vblk_pci_ep_index(ep));
}

void vblk_pci_virtio_dev_reset(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;

	dev->driver_features = pci_cfg->driver_feature = 0;
	pci_cfg->driver_feature_select = pci_cfg->device_feature_select = 0;
	/* Keep device_feature aligned with device_feature_select (reset sets select=0 => low 32b features) */
	pci_cfg->device_feature = dev->device_features & 0xFFFFFFFF;

	vblk_pci_virtio_dev_queue_init(dev);
}

void vblk_pci_virtio_cfg_change_set(struct vblk_pci_virtio_dev *dev, pci_cfg_change_cb_t cb, void *arg)
{
	dev->cb_arg = arg;
	atomic_store_explicit(&dev->pci_cfg_change_cb, cb, memory_order_release);
}

bool vblk_pci_virtio_cfg_change_notify(struct vblk_pci_virtio_dev *dev)
{
	pci_cfg_change_cb_t cb;

	cb = atomic_load_explicit(&dev->pci_cfg_change_cb, memory_order_acquire);
	if (cb == NULL)
		return false;

	cb(dev, dev->cb_arg);
	return true;
}

void vblk_pci_tlp_poll(void)
{
	uint32_t i;

	for (i = 0; i < g_tlp_ctx.num_ep; i++) {
		struct vblk_pci_virtio_dev *d = g_tlp_ctx.virtio_devs[i];

		if (d && d->tlp_poll_cb)
			d->tlp_poll_cb(d->cb_arg);
	}
}

void vblk_pci_notify_host(void)
{
	struct vblk_pci_virtio_dev *dev = g_tlp_ctx.virtio_devs[0];
	doca_error_t err;

	if (dev == NULL)
		return;

	if (dev->msix && dev->config_msix_vector != dev->pci_cfg.config_msix_vector) {
		if (DOCA_SUCCESS != doca_devemu_pci_msix_destroy(dev->msix)) {
			DOCA_LOG_ERR("Failed to destroy msix: %p", dev->msix);
			return;
		}
		dev->msix = NULL;
	}
	if (!dev->msix && dev->pci_cfg.config_msix_vector != VIRTIO_MSI_NO_VECTOR) {
		err = doca_devemu_pci_ep_create_msix(doca_devemu_pci_tlp_dev_as_ep(dev->pci_tlp_dev),
						     VBLK_PCI_VIRTIO_BAR_ID,
						     VBLK_PCI_VIRTIO_MSIX_TABLE_OFFSET,
						     dev->pci_cfg.config_msix_vector,
						     &dev->msix);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create the EP msix %d : %s",
				     dev->pci_cfg.config_msix_vector,
				     doca_error_get_descr(err));
			return;
		}
		dev->config_msix_vector = dev->pci_cfg.config_msix_vector;
	}
	if (dev->msix) {
		doca_devemu_pci_msix_raise(dev->msix);
	} else {
		DOCA_LOG_INFO("Skip triggering host MSIX interrupt with vector %#x", dev->pci_cfg.config_msix_vector);
	}
}

static doca_error_t vblk_pci_virtio_dev_init(struct vblk_pci_virtio_dev *dev, const struct vblk_pci_virtio_attrs *attr)
{
	doca_error_t err;
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	const uint16_t num_queues = attr->num_queues;

	/* Only virtio block device is implemented */
	if (attr->device_type != VBLK_PCI_DEVICE_TYPE_VBLK) {
		DOCA_LOG_ERR("unsupported virtio device type %d", attr->device_type);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Validate num_queues before using it for resource allocation */
	if (num_queues > VBLK_PCI_VIRTIO_MAX_QUEUES) {
		DOCA_LOG_ERR("Invalid number of queues %d > %d", num_queues, VBLK_PCI_VIRTIO_MAX_QUEUES);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* num_msix = min(num_queues + 1, max) - allows VQs to share MSIX when queues > max */
	const uint16_t num_msix = (num_queues + 1 > VBLK_PCI_VIRTIO_MAX_NUM_MSIX) ? VBLK_PCI_VIRTIO_MAX_NUM_MSIX :
										    num_queues + 1;

	err = doca_devemu_pci_ep_set_num_msix(doca_devemu_pci_tlp_dev_as_ep(dev->pci_tlp_dev), num_msix);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set num_msix=%u: %s", num_msix, doca_error_get_descr(err));
		return err;
	}

	err = doca_devemu_pci_ep_set_num_db(doca_devemu_pci_tlp_dev_as_ep(dev->pci_tlp_dev), num_queues);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set num_db=%u: %s", num_queues, doca_error_get_descr(err));
		return err;
	}
	DOCA_LOG_INFO("EP[%u]: num_msix=%u, num_db=%u", dev->ep_index, num_msix, num_queues);

	dev->pcie_dev = virtio_dev;
	dev->pcie_dev.cfg.msix_cap.msgctl = num_msix - 1; /* zero based */
	DOCA_LOG_INFO("Endpoint msgctl=%u (table_size=%u)", dev->pcie_dev.cfg.msix_cap.msgctl, num_msix);

	/* 4.1.2.1 Device Requirements: PCI Device Discovery
	 * Devices MUST either have the PCI Device ID calculated by adding 0x1040 to the Virtio Device ID
	 */
	dev->pcie_dev.cfg.regs.device_id = VBLK_PCI_VIRTIO_NON_TRANSITION_DEV_ID_BASE + attr->device_type;
	dev->pcie_dev.cfg.regs.subsystem_id = attr->device_type;

	pci_cfg->num_queues = attr->num_queues;

	dev->device_features = attr->device_features | (1ULL << VBLK_PCI_VIRTIO_F_VERSION_1) |
			       (1ULL << VBLK_PCI_VIRTIO_F_ACCESS_PLATFORM);
	if (g_tlp_ctx.sriov_enabled)
		dev->device_features |= (1ULL << VIRTIO_F_SR_IOV);

	pci_cfg->device_feature = dev->device_features & 0xFFFFFFFF;
	pci_cfg->device_status = VBLK_PCI_VIRTIO_DEVICE_STATUS_NEEDS_RESET;

	if (attr->dev_cfg) {
		memcpy(&dev->vblk_cfg, attr->dev_cfg, sizeof(dev->vblk_cfg));
		if (dev->vblk_cfg.num_queues > pci_cfg->num_queues) {
			DOCA_LOG_ERR("Invalid number of the device queues: dev_cfg %d > pci_cfg %d",
				     dev->vblk_cfg.num_queues,
				     pci_cfg->num_queues);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	vblk_pci_virtio_dev_queue_init(dev);

	dev->tlp_poll_cb = attr->tlp_poll_cb;
	vblk_pci_virtio_cfg_change_set(dev, attr->pci_cfg_change_cb, attr->cb_arg);
	return DOCA_SUCCESS;
}

/**
 * @brief Fixup VirtIO device configuration with runtime PCI type info
 *
 * Queries the PCI type for actual BAR layout, regions, and doorbell configuration
 * and updates the default VirtIO device configuration accordingly.
 *
 * @param[in] virtio_type PCI device type to query
 * @param[in,out] dev VirtIO device configuration to fixup
 * @return DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t vblk_pci_fixup_virtio_dev(struct doca_devemu_pci_type *virtio_type, struct pcie_virtio_dev *dev)
{
	doca_error_t err, ret = DOCA_SUCCESS;
	uint32_t i, num_bars, num_regions;
	struct doca_devemu_pci_bar_info **bar_list;
	struct doca_devemu_pci_transaction_region_info **mmio_list;
	struct doca_devemu_pci_db_region_by_data_info **db_list;
	struct doca_devemu_pci_msix_pba_region_info **pba_list;
	struct doca_devemu_pci_msix_table_region_info **msix_list;

	/* to improve debugability it is better to do full pci type scan and
	 * not to stop on non fatal errors
	 */
	err = doca_devemu_pci_type_create_bar_info_list(virtio_type, &bar_list, &num_bars);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio bar list");
		return err;
	}

	for (i = 0; i < num_bars; i++) {
		uint8_t id, log_sz, is_prefetch;
		enum doca_devemu_pci_bar_mem_type mem_type;
		uint32_t val = 0;

		doca_devemu_pci_bar_info_get_bar_id(bar_list[i], &id);
		doca_devemu_pci_bar_info_get_log_sz(bar_list[i], &log_sz);
		doca_devemu_pci_bar_info_get_mem_type(bar_list[i], &mem_type);
		doca_devemu_pci_bar_info_get_prefetchable(bar_list[i], &is_prefetch);

		DOCA_LOG_INFO("bar%d log %d mem_type %d prefetch %d", id, log_sz, mem_type, is_prefetch);
		if (is_prefetch)
			val |= PCI_BASE_ADDRESS_MEM_PREFETCH;
		if (mem_type == DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT) {
			val |= PCI_BASE_ADDRESS_MEM_TYPE_64;
			dev->bar64_map[id].log_size = log_sz;
			dev->cfg.regs.base_address[id] = val;
		} else if (mem_type == DOCA_DEVEMU_PCI_BAR_MEM_TYPE_1_MB) {
			DOCA_LOG_ERR("bar memtype 1_MB is not supported");
			goto free_bar_list;
		} else {
			if (log_sz) {
				DOCA_LOG_ERR("32bit bars are not supported");
				goto free_bar_list;
			}
		}
	}
	doca_devemu_pci_type_destroy_bar_info_list(bar_list);

	err = doca_devemu_pci_type_create_transaction_region_info_list(virtio_type, &mmio_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio MMIO TLP region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one MMIO TLP region");
		ret = DOCA_ERROR_NOT_SUPPORTED;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one MMIO TLP region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_transaction_region_info_get_bar_id(mmio_list[i], &id);
		doca_devemu_pci_transaction_region_info_get_start_addr(mmio_list[i], &start_addr);
		doca_devemu_pci_transaction_region_info_get_size(mmio_list[i], &size);

		DOCA_LOG_INFO("mmio_tlp:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		/* region size must be big enough for our desired virtio bar config */
		if (size < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + VBLK_PCI_VIRTIO_DEV_CFG_LEN) {
			DOCA_LOG_ERR("region size is too small");
			ret = DOCA_ERROR_INVALID_VALUE;
		}

		dev->cfg.common_cfg.bar = id;
		dev->cfg.common_cfg.offset = start_addr;

		dev->cfg.isr_cfg.bar = id;
		dev->cfg.isr_cfg.offset = start_addr + VBLK_PCI_VIRTIO_ISR_CFG_OFFSET;

		dev->cfg.device_cfg.bar = id;
		dev->cfg.device_cfg.offset = start_addr + VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;

		dev->cfg.pci_cfg.base.bar = id;
	}

	doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list);

	err = doca_devemu_pci_type_create_db_region_by_data_info_list(virtio_type, &db_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio db region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one doorbell region");
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one doorbell region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_db_region_by_data_info_get_bar_id(db_list[i], &id);
		doca_devemu_pci_db_region_by_data_info_get_start_addr(db_list[i], &start_addr);
		doca_devemu_pci_db_region_by_data_info_get_size(db_list[i], &size);

		DOCA_LOG_INFO("db_region:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		/* Assume stride and doorbell size are standard */
		dev->cfg.notify_cfg.base.bar = id;
		dev->cfg.notify_cfg.base.offset = start_addr;
		dev->cfg.notify_cfg.base.length = size;
	}

	doca_devemu_pci_type_destroy_db_region_by_data_info_list(db_list);

	err = doca_devemu_pci_type_create_msix_table_region_info_list(virtio_type, &msix_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio MSIX region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one MSIX region");
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one MSIX region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_msix_table_region_info_get_bar_id(msix_list[i], &id);
		doca_devemu_pci_msix_table_region_info_get_start_addr(msix_list[i], &start_addr);
		doca_devemu_pci_msix_table_region_info_get_size(msix_list[i], &size);

		DOCA_LOG_INFO("msix_region:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		dev->cfg.msix_cap.table_offset = start_addr | id;
	}

	doca_devemu_pci_type_destroy_msix_table_region_info_list(msix_list);

	err = doca_devemu_pci_type_create_msix_pba_region_info_list(virtio_type, &pba_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio PBA region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one PBA region");
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one PBA region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_msix_pba_region_info_get_bar_id(pba_list[i], &id);
		doca_devemu_pci_msix_pba_region_info_get_start_addr(pba_list[i], &start_addr);
		doca_devemu_pci_msix_pba_region_info_get_size(pba_list[i], &size);

		DOCA_LOG_INFO("pba_region:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		dev->cfg.msix_cap.pba_offset = start_addr | id;
	}

	doca_devemu_pci_type_destroy_msix_pba_region_info_list(pba_list);

	return ret;

free_bar_list:
	doca_devemu_pci_type_destroy_bar_info_list(bar_list);
	return DOCA_ERROR_INVALID_VALUE;
}

static doca_error_t vblk_pci_virtio_block_init(struct doca_dev *dev)
{
	doca_error_t err;

	err = doca_devemu_vblk_pci_tlp_type_create("vblk_pci_vblk", &g_tlp_ctx.pci_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio block tlp device type");
		return err;
	}

	err = doca_devemu_pci_type_set_dev(g_tlp_ctx.pci_type, dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach device to virtio pci type");
		goto config_failed;
	}

	err = doca_devemu_pci_tlp_type_set_pci_cap_conf(g_tlp_ctx.pci_type,
							PCI_CAP_ID_EXP,
							offsetof(struct pcie_virtio_dev, cfg.pcie_cap),
							sizeof(struct pcie_capability));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set PCI capability configuration");
		goto config_failed;
	}

	err = doca_devemu_pci_tlp_type_set_pci_cap_conf(g_tlp_ctx.pci_type,
							PCI_CAP_ID_MSIX,
							offsetof(struct pcie_virtio_dev, cfg.msix_cap),
							sizeof(struct msix_capability));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set MSI-X capability configuration: %s", doca_error_get_descr(err));
		goto config_failed;
	}

	if (g_tlp_ctx.sriov_enabled) {
		err = doca_devemu_pci_tlp_type_set_pcie_cap_conf(g_tlp_ctx.pci_type,
								 PCI_EXT_CAP_ID_SRIOV,
								 PCI_CFG_SPACE_SIZE,
								 PCI_EXT_CAP_SRIOV_SIZEOF);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set SR-IOV extended capability configuration: %s",
				     doca_error_get_descr(err));
			goto config_failed;
		}
		DOCA_LOG_INFO("SR-IOV extended capability declared at byte offset 0x%x", PCI_CFG_SPACE_SIZE);
	}

	err = doca_devemu_pci_type_set_num_msix(g_tlp_ctx.pci_type, VBLK_PCI_VIRTIO_MAX_NUM_MSIX);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of MSIX interrupts");
		goto config_failed;
	}

	err = doca_devemu_pci_type_set_num_db(g_tlp_ctx.pci_type, VBLK_PCI_VIRTIO_MAX_NUM_DB);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of doorbell registers");
		goto config_failed;
	}
	DOCA_LOG_INFO("Type: num_msix=%u, num_db=%u", VBLK_PCI_VIRTIO_MAX_NUM_MSIX, VBLK_PCI_VIRTIO_MAX_NUM_DB);

	err = doca_devemu_pci_type_start(g_tlp_ctx.pci_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start virtio block pci type");
		goto config_failed;
	}

	DOCA_LOG_INFO("Created Virtio Block pci device type");

	/* Create default device configuration and fixup with runtime PCI type info */
	virtio_dev = vblk_pci_set_default_values();
	err = vblk_pci_fixup_virtio_dev(g_tlp_ctx.pci_type, &virtio_dev);
	if (err != DOCA_SUCCESS)
		goto fixup_failed;

	return DOCA_SUCCESS;

fixup_failed:
	if (DOCA_SUCCESS != doca_devemu_pci_type_stop(g_tlp_ctx.pci_type)) {
		DOCA_LOG_ERR("Failed to stop pci_type: %p", g_tlp_ctx.pci_type);
	}
config_failed:
	if (DOCA_SUCCESS != doca_devemu_pci_type_destroy(g_tlp_ctx.pci_type)) {
		DOCA_LOG_ERR("Failed to destroy pci_type: %p", g_tlp_ctx.pci_type);
	}
	g_tlp_ctx.pci_type = NULL;
	return err;
}

static void vblk_pci_sriov_free_pf_state(struct pci_device_config *ep)
{
	struct vblk_sriov_pf_state *sriov = ep->sriov;
	uint32_t i;

	if (sriov == NULL)
		return;

	vblk_pci_sriov_sync_vf_presence(ep, false);
	vblk_pci_sriov_close_vf_reps(sriov);
	for (i = 0; i < sriov->total_vfs; i++) {
		struct pci_device_config *vf = sriov->vf_slots[i].cfg;

		if (vf != NULL)
			vf->sriov_vf = NULL;
	}
	free(sriov->vf_slots);
	free(sriov);
	ep->sriov = NULL;
}

static doca_error_t vblk_pci_sriov_init_pf_state(uint32_t ep_index,
						 struct pci_device_config *ep,
						 const struct vblk_pci_virtio_dev *pf_dev)
{
	struct vblk_sriov_pf_state *sriov;
	const struct pcie_virtio_dev *pf_pcie_dev;
	doca_error_t result;
	uint32_t i;
	uint32_t first_vf_idx;

	if (!g_tlp_ctx.sriov_enabled)
		return DOCA_SUCCESS;
	if (pf_dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	/* TotalVFs is already validated (1..PCI_FUNC(UINT8_MAX)) by the CLI callback
	 * and vblk_pci_init; this path only runs with sriov_enabled, so it is in range. */
	if (ep->sriov != NULL)
		return DOCA_SUCCESS;

	sriov = calloc(1, sizeof(*sriov));
	if (sriov == NULL)
		return DOCA_ERROR_NO_MEMORY;
	sriov->vf_slots = calloc(g_tlp_ctx.sriov_total_vfs, sizeof(*sriov->vf_slots));
	if (sriov->vf_slots == NULL) {
		free(sriov);
		return DOCA_ERROR_NO_MEMORY;
	}

	pf_pcie_dev = &pf_dev->pcie_dev;
	sriov->total_vfs = g_tlp_ctx.sriov_total_vfs;
	sriov->first_vf_offset = VBLK_SRIOV_FIRST_VF_OFFSET;
	sriov->vf_stride = VBLK_SRIOV_VF_STRIDE;
	sriov->vf_device_id = pf_pcie_dev->cfg.regs.device_id;
	sriov->vf_num_msix = g_tlp_ctx.sriov_num_vf_msix;
	sriov->vf_num_db = g_tlp_ctx.sriov_num_vf_db;
	sriov->control_shadow = 0;
	sriov->num_vfs_shadow = 0;
	sriov->system_pg_size = VBLK_SRIOV_SUPPORTED_PAGE_SIZE_MASK; /* Default and only supported page size is 4 KB. */
	sriov->vf_bar[0] = PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH;
	first_vf_idx = ep_index * sriov->total_vfs;

	for (i = 0; i < sriov->total_vfs; i++) {
		struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];
		struct pci_device_config *vf = &g_tlp_ctx.devs_config[FIRST_VF_IDX(&g_tlp_ctx) + first_vf_idx + i];

		slot->cfg = vf;
		slot->pcie_dev = *pf_pcie_dev;
		/* The PF SR-IOV VF BARs advertise the per-VF MMIO window; the VF template starts unassigned. */
		memset(slot->pcie_dev.cfg.regs.base_address, 0, sizeof(slot->pcie_dev.cfg.regs.base_address));
		memset(slot->pcie_dev.bar64_map, 0, sizeof(slot->pcie_dev.bar64_map));
		slot->pcie_dev.cfg.regs.device_id = sriov->vf_device_id;
		slot->pcie_dev.cfg.regs.subsystem_id = VBLK_PCI_DEVICE_TYPE_VBLK;
		slot->pcie_dev.cfg.msix_cap.msgctl = sriov->vf_num_msix - 1;
		slot->dynamic_vf_index = ep->sriov_dynamic_first_vf_idx + i;

		vf->sriov_vf = slot;
	}

	ep->sriov = sriov;
	if (ep->sriov_dynamic_vf_range_owned) {
		result = vblk_pci_sriov_discover_vf_reps(ep, pf_dev);

		if (result != DOCA_SUCCESS) {
			vblk_pci_sriov_free_pf_state(ep);
			return result;
		}
	}
	result = vblk_pci_sriov_start_vfs(ep);
	if (result != DOCA_SUCCESS) {
		for (i = 0; i < sriov->total_vfs; i++) {
			struct vblk_sriov_vf_slot *slot = &sriov->vf_slots[i];

			vblk_pci_sriov_destroy_vf_tlp_dev(slot);
		}
		vblk_pci_sriov_free_pf_state(ep);
		return result;
	}
	DOCA_LOG_INFO("EP[%u] SR-IOV VF cfg-read state initialized: total_vfs=%u, vf_device_id=0x%04x, vf_msix=%u",
		      ep_index,
		      sriov->total_vfs,
		      sriov->vf_device_id,
		      sriov->vf_num_msix);
	return DOCA_SUCCESS;
}

static inline uint64_t vblk_pci_bar_offset(struct vblk_pci_virtio_dev *dev, const uint64_t addr)
{
	return addr - (dev->pcie_dev.cfg.regs.base_address64[VBLK_PCI_VIRTIO_BAR_ID] & ~0xF);
}

static uint32_t vblk_pci_virtio_mmio_read32(struct vblk_pci_virtio_dev *dev, const uint64_t addr)
{
	uint64_t offset = vblk_pci_bar_offset(dev, addr);
	uint32_t *base;

	DOCA_LOG_TRC("mmio read offset 0x%lx", offset);

	/* cast is needed to avoid always true comparison warning when defined PCI_CFG_OFFSET is 0 */
	if ((int)offset >= (int)VBLK_PCI_VIRTIO_PCI_CFG_OFFSET &&
	    offset < VBLK_PCI_VIRTIO_PCI_CFG_OFFSET + sizeof(dev->pci_cfg)) {
		offset -= VBLK_PCI_VIRTIO_PCI_CFG_OFFSET;
		base = (uint32_t *)&dev->pci_cfg;
	} else if (offset >= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->dev_cfg)) {
		offset -= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
		base = (uint32_t *)&dev->dev_cfg;
	} else {
		DOCA_LOG_ERR("mmio read - unsupported virtio bar offset 0x%lx", offset);
		return 0xFAFAFAFA;
	}
	return base[offset / 4];
}

static inline uint32_t vblk_pci_feature_select(int n, uint64_t ftr)
{
	if (n > 1)
		return 0;

	return n ? ftr >> 32 : ftr & 0xFFFFFFFF;
}

/* rw pointer to the current pci configuration */
struct vblk_pci_virtio_pci_common_cfg *vblk_pci_virtio_get_pci_cfg(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	uint32_t *p = (uint32_t *)&dev->driver_features;

	/* sync features and queue values */

	if (pci_cfg->driver_feature_select <= 1)
		p[pci_cfg->driver_feature_select] = pci_cfg->driver_feature;

	return pci_cfg;
}

/* ro queues */
const struct vblk_pci_virtq_pci_cfg *vblk_pci_virtio_get_virtq_pci_cfg(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;

	/* sync queues */
	if (pci_cfg->queue_select < pci_cfg->num_queues && pci_cfg->queue_select < VBLK_PCI_VIRTIO_MAX_QUEUES) {
		/* coverity[OVERRUN] */
		memcpy(&dev->vqs[pci_cfg->queue_select], &pci_cfg->vq, sizeof(pci_cfg->vq));
	}

	return dev->vqs;
}

/* rw device config */
const struct vblk_pci_virtio_blk_config *vblk_pci_virtio_get_vblk_dev_cfg(struct vblk_pci_virtio_dev *dev)
{
	return &dev->vblk_cfg;
}

void vblk_pci_set_capacity(uint64_t capacity_bytes)
{
	struct vblk_pci_virtio_dev *dev = g_tlp_ctx.virtio_devs[0];

	if (dev == NULL) {
		DOCA_LOG_ERR("Cannot set capacity: device not initialized");
		return;
	}
	dev->vblk_cfg.capacity = capacity_bytes / 512;
	DOCA_LOG_INFO("Block device capacity updated: %lu bytes (%lu sectors)", capacity_bytes, dev->vblk_cfg.capacity);
}

/* cfg write notify scheduling for vblk_pci_handle_mmio_write() */
enum vblk_cfg_wr_notify_sched {
	VBLK_CFG_WR_NO_NOTIFY = 0,
	VBLK_CFG_WR_NOTIFY_AFTER_COMPLETE = 1,	/* DRIVER_OK / normal: free TLP channel first */
	VBLK_CFG_WR_NOTIFY_BEFORE_COMPLETE = 2, /* status->0 reset: publish 0x40 before host NP-reads 0 */
};

/*
 * Apply a virtio common-cfg dword write to the shadow.
 * Returns VBLK_CFG_WR_* describing when cfg_change_notify must run relative to
 * complete_tlp (see vblk_pci_handle_mmio_write).
 */
static enum vblk_cfg_wr_notify_sched vblk_pci_virtio_pci_cfg_write32(struct vblk_pci_virtio_dev *dev,
								     const uint64_t offset,
								     const uint32_t val,
								     const uint32_t wr_mask)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	struct vblk_pci_virtio_pci_common_cfg prev_cfg;
	uint32_t *base = (uint32_t *)pci_cfg;
	const uint64_t status_offset = offsetof(struct vblk_pci_virtio_pci_common_cfg, device_status);
	bool device_status_write = false;

	/* save everything to handle ro fields and selectors */
	memcpy(&prev_cfg, pci_cfg, sizeof(prev_cfg));

	const uint32_t dw_offset = offset / 4;
	const uint32_t orig_val = base[dw_offset];

	if (offset <= status_offset && status_offset < offset + sizeof(uint32_t)) {
		uint32_t status_mask = 0xffu << ((status_offset - offset) * 8);

		device_status_write = (wr_mask & status_mask) != 0;
	}

	base[dw_offset] = (orig_val & ~wr_mask) | (val & wr_mask);
	DOCA_LOG_TRC("pci_cfg+%ld: 0x%x <- 0x%x (val 0x%x & mask 0x%x)",
		     offset,
		     orig_val,
		     base[dw_offset],
		     val,
		     wr_mask);

	/* check RO fields */
	if (prev_cfg.device_feature != pci_cfg->device_feature || prev_cfg.num_queues != pci_cfg->num_queues ||
	    prev_cfg.config_generation != pci_cfg->config_generation ||
	    prev_cfg.admin_queue_index != pci_cfg->admin_queue_index ||
	    prev_cfg.admin_queue_num != pci_cfg->admin_queue_num ||
	    prev_cfg.vq.queue_notify_off != pci_cfg->vq.queue_notify_off ||
	    prev_cfg.vq.queue_notif_config_data != pci_cfg->vq.queue_notif_config_data) {
		DOCA_LOG_ERR("pci_cfg: attempt to overwrite RO field: ROLLBACK");
		goto rollback;
	}

	if (prev_cfg.device_feature_select != pci_cfg->device_feature_select) {
		DOCA_LOG_TRC("dev_ftr select: %d -> %d",
			     prev_cfg.device_feature_select,
			     pci_cfg->device_feature_select);
		pci_cfg->device_feature = vblk_pci_feature_select(pci_cfg->device_feature_select, dev->device_features);
	}

	if (prev_cfg.driver_feature_select != pci_cfg->driver_feature_select) {
		DOCA_LOG_TRC("drv_ftr select: %d -> %d",
			     prev_cfg.driver_feature_select,
			     pci_cfg->driver_feature_select);
		uint32_t *p = (uint32_t *)&dev->driver_features;
		if (prev_cfg.driver_feature_select <= 1)
			p[prev_cfg.driver_feature_select] = pci_cfg->driver_feature;

		pci_cfg->driver_feature = vblk_pci_feature_select(pci_cfg->driver_feature_select, dev->driver_features);

		if ((dev->device_features & (1ULL << VBLK_PCI_VIRTIO_F_INDIRECT_DESC)) &&
		    (dev->driver_features & (1ULL << VBLK_PCI_VIRTIO_F_INDIRECT_DESC)))
			DOCA_LOG_INFO("Indirect descriptor feature negotiated (device and driver)");
	}

	if (prev_cfg.queue_select != pci_cfg->queue_select) {
		DOCA_LOG_TRC("queue select: %d -> %d", prev_cfg.queue_select, pci_cfg->queue_select);

		if (prev_cfg.queue_select < pci_cfg->num_queues && prev_cfg.queue_select < VBLK_PCI_VIRTIO_MAX_QUEUES) {
			/* coverity[OVERRUN] */
			memcpy(&dev->vqs[prev_cfg.queue_select], &pci_cfg->vq, sizeof(pci_cfg->vq));
		}

		/* 4.1.4.3.1 Device Requirements: Common configuration structure layout
		 * The device MUST present a 0 in queue_size if the virtqueue corresponding to the current queue_select
		 * is unavailable.
		 */
		if (pci_cfg->queue_select < prev_cfg.num_queues && pci_cfg->queue_select < VBLK_PCI_VIRTIO_MAX_QUEUES) {
			/* coverity[OVERRUN] */
			memcpy(&pci_cfg->vq, &dev->vqs[pci_cfg->queue_select], sizeof(pci_cfg->vq));
		} else {
			memset(&pci_cfg->vq, 0, sizeof(pci_cfg->vq));
		}
	}

	/* Helpful debug: log when the driver enables/disables the currently selected queue */
	if (prev_cfg.queue_select == pci_cfg->queue_select && prev_cfg.vq.queue_enable != pci_cfg->vq.queue_enable) {
		DOCA_LOG_TRC("queue_enable qid=%u %u -> %u",
			     pci_cfg->queue_select,
			     prev_cfg.vq.queue_enable,
			     pci_cfg->vq.queue_enable);
	}

	/*
	 * If FLR is in progress, the device is already on its way to reset state.
	 * A Linux reset write of device_status=0 must still read back NEEDS_RESET
	 * until the async FLR tail has rebound io_ctxs. Nonzero restore writes are
	 * preserved so the tail can replay the callback if Linux already reached
	 * DRIVER_OK. Do not re-run vblk_pci_virtio_dev_reset or invoke
	 * virtio_ctrl_change_cb here. */
	if (atomic_load_explicit(&dev->flr_in_progress, memory_order_acquire)) {
		if (device_status_write && pci_cfg->device_status == 0)
			pci_cfg->device_status = VBLK_PCI_VIRTIO_DEVICE_STATUS_NEEDS_RESET;
		DOCA_LOG_DBG("EP[%u]: ignoring common_cfg write during FLR cleanup", dev->ep_index);
		return VBLK_CFG_WR_NO_NOTIFY;
	}

	/* Reset PCI config before notifying the application layer, so
	 * callback sees clean MSI-X/feature/queue state. Check original
	 * write value (not post-callback) since callback may modify status. */
	if (prev_cfg.device_status != 0 && pci_cfg->device_status == 0) {
		DOCA_LOG_INFO("device status: %d -> %d", prev_cfg.device_status, pci_cfg->device_status);
		vblk_pci_virtio_dev_reset(dev);
		/* Reset must notify before complete_tlp so host does not NP-read a
		 * transient status=0 and re-init while VQs are still live (0x40 hold). */
		return VBLK_CFG_WR_NOTIFY_BEFORE_COMPLETE;
	} else if (prev_cfg.device_status != pci_cfg->device_status) {
		DOCA_LOG_INFO("device status: %d -> %d", prev_cfg.device_status, pci_cfg->device_status);
	}

	return VBLK_CFG_WR_NOTIFY_AFTER_COMPLETE;

rollback:
	memcpy(pci_cfg, &prev_cfg, sizeof(prev_cfg));
	return VBLK_CFG_WR_NO_NOTIFY;
}

static void vblk_pci_virtio_dev_cfg_write32(struct vblk_pci_virtio_dev *virtio_dev,
					    const uint64_t offset,
					    const uint32_t val,
					    const uint32_t wr_mask)
{
	uint32_t *base = (uint32_t *)&virtio_dev->dev_cfg;
	const uint32_t orig_val = base[offset / 4];

	/* RO for the virtio block with the exception of writeback field when
	 * VIRTIO_BLK_F_CONFIG_WCE is negotiated.
	 * SPEC 5.2.5 Device Initialization
	 */
	DOCA_LOG_WARN("write to the READ-ONLY field dev_cfg+%ld: 0x%x <- 0x%x (val 0x%x & mask 0x%x)",
		      offset,
		      orig_val,
		      (orig_val & ~wr_mask) | (val & wr_mask),
		      val,
		      wr_mask);
}

/* Returns VBLK_CFG_WR_* for notify scheduling relative to TLP complete. */
static enum vblk_cfg_wr_notify_sched vblk_pci_virtio_mmio_write32(struct vblk_pci_virtio_dev *dev,
								  const uint64_t addr,
								  const uint32_t val,
								  const uint32_t wr_mask)
{
	/* implement actual mmio write */
	uint64_t offset = vblk_pci_bar_offset(dev, addr);

	DOCA_LOG_TRC("mmio write offset 0x%lx", offset);

	/* cast is needed to avoid always true comparison warning when defined PCI_CFG_OFFSET is 0 */
	if ((int)offset >= (int)VBLK_PCI_VIRTIO_PCI_CFG_OFFSET &&
	    offset < VBLK_PCI_VIRTIO_PCI_CFG_OFFSET + sizeof(dev->pci_cfg)) {
		offset -= VBLK_PCI_VIRTIO_PCI_CFG_OFFSET;
		return vblk_pci_virtio_pci_cfg_write32(dev, offset, val, wr_mask);
	} else if (offset >= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->dev_cfg)) {
		offset -= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
		vblk_pci_virtio_dev_cfg_write32(dev, offset, val, wr_mask);
		return VBLK_CFG_WR_NO_NOTIFY;
	}

	DOCA_LOG_ERR("mmio write drop - unsupported virtio bar offset 0x%lx", offset);
	return VBLK_CFG_WR_NO_NOTIFY;
}

static struct vblk_pci_virtio_dev *vblk_pci_dev_find_by_addr(const uint64_t addr)
{
	uint32_t i;

	for (i = 0; i < g_tlp_ctx.num_ep; i++) {
		struct vblk_pci_virtio_dev *dev = g_tlp_ctx.virtio_devs[i];

		if (!dev)
			continue;

		/* in generally we have to match all bars, but for the virtio we only need one */
		const int log_size = dev->pcie_dev.bar64_map[VBLK_PCI_VIRTIO_BAR_ID].log_size;
		/* clear type bits */
		const uint64_t bar_base_addr = dev->pcie_dev.cfg.regs.base_address64[VBLK_PCI_VIRTIO_BAR_ID] & ~0xF;

		if (addr >= bar_base_addr && addr < bar_base_addr + (1ULL << log_size))
			return dev;
	}
	return NULL;
}

/*
 * Route a memory TLP to the SR-IOV VF whose BAR0 window contains 'addr'.
 *
 * Standard SR-IOV: the host programs a single VF-BAR aperture base in the PF's
 * SR-IOV capability; VF<n> owns [base + n*size, base + (n+1)*size) (no-ARI
 * stride 1, size = the PF BAR0 window). A VF's own config BARs are RO/0, so VF
 * memory TLPs are resolved through the PF SR-IOV VF-BAR aperture instead of
 * vblk_pci_dev_find_by_addr(). The matched vf_dev is stamped with the window
 * base (for the offset calc), the VF routing ID (completer), and the VF's own
 * TLP device. Only a present VF whose memory decode is enabled (device_present +
 * the PF SR-IOV MSE bit) is served. Returns NULL when no present VF owns the
 * address.
 */
static struct vblk_pci_virtio_dev *vblk_pci_sriov_find_vf_dev_by_mmio_addr(uint64_t addr)
{
	const uint32_t pf = FIRST_PF_IDX(&g_tlp_ctx);
	const uint64_t window = 1ULL << VBLK_PCI_VIRTIO_BAR_LOG_SIZE;
	uint32_t i;
	uint16_t s;

	for (i = 0; i < g_tlp_ctx.num_ep; i++) {
		struct pci_device_config *ep = &g_tlp_ctx.devs_config[pf + i];
		struct vblk_sriov_pf_state *sriov = ep->sriov;
		struct vblk_sriov_vf_slot *slot;
		uint64_t aperture, base;

		if (sriov == NULL || sriov->vf_slots == NULL)
			continue;
		aperture = ((uint64_t)sriov->vf_bar[1] << 32) | (sriov->vf_bar[0] & ~0xFULL);
		if (aperture == 0)
			continue;
		/* The total_vfs VF windows are contiguous from the aperture (base +
		 * vf_index * size), so index the target VF directly instead of scanning
		 * every slot. */
		if (addr < aperture || addr >= aperture + (uint64_t)sriov->total_vfs * window)
			continue; /* addr is not in this PF's VF aperture */
		s = (uint16_t)((addr - aperture) >> VBLK_PCI_VIRTIO_BAR_LOG_SIZE);
		slot = &sriov->vf_slots[s];
		base = aperture + (uint64_t)s * window;

		/*
		 * addr targets VF s's BAR window. The VF TLP device stays alive
		 * until PF teardown, so a memory request can still reach SW after
		 * the VF is disabled. Serve only a present VF whose memory decode
		 * is enabled: slot->cfg->device_present
		 * (PF-present + VFE + vf_index<NumVFs, maintained by
		 * sync_vf_presence) and the PF SR-IOV VF Memory Space Enable bit.
		 * Per PCIe/SR-IOV, VF memory decode is gated by the PF's MSE alone -
		 * VFs do not implement their own Command Memory Space Enable bit
		 * (Linux marks VFs no_command_memory), so it is not part of the gate.
		 * Anything else is a stray access to a disabled VF: log it and
		 * return NULL so the caller completes with Unsupported Request.
		 */
		if (!slot->cfg->device_present || (sriov->control_shadow & PCI_SRIOV_CTRL_MSE) == 0) {
			DOCA_LOG_ERR(
				"SR-IOV: stray MMIO addr 0x%lx to not-present/decode-disabled VF[%u] - returning UR",
				addr,
				slot->dynamic_vf_index);
			return NULL;
		}

		slot->vf_dev->pcie_dev.cfg.regs.base_address64[VBLK_PCI_VIRTIO_BAR_ID] = base;
		slot->vf_dev->bus = slot->cfg->bus;
		slot->vf_dev->device = slot->cfg->device;
		slot->vf_dev->function = slot->cfg->function;
		return slot->vf_dev;
	}
	return NULL;
}

/*
 * Topology-aware device lookup.
 *
 * Type0 CFG -> USP (device=0, function=0 on physical bus)
 * Type1 CFG -> DSP bridges (on USP's secondary_bus, device=0..N-1)
 *           -> PFs (on each DSP's secondary_bus, device=0 function=0)
 *           -> VFs (no ARI: function 1..TotalVFs under the same PF device)
 */
static struct pci_device_config *find_device_by_bdf(uint8_t bus, uint8_t device, uint8_t function, uint8_t tlp_type)
{
	struct vblk_tlp_context *ctx = &g_tlp_ctx;
	uint16_t bdf = BDF(bus, device, function);
	struct pci_device_config *dc;
	uint32_t i;

	dc = bdf_map_lookup(ctx, tlp_type, bdf);
	if (dc)
		return dc;

	if (tlp_type == TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0) {
		/* Type0: USP is the only device on the physical bus (device=0, fn=0) */
		if (device == 0 && function == 0) {
			dc = &ctx->devs_config[USP_IDX(ctx)];
			bdf_map_update(ctx, tlp_type, bdf, dc);
			return dc;
		}
	} else if (tlp_type == TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1) {
		uint8_t usp_sec = ctx->devs_config[USP_IDX(ctx)].cfg_space_hdr.type1.secondary_bus;

		/* DSP bridges sit on USP's secondary bus */
		if (usp_sec != 0 && bus == usp_sec && function == 0) {
			for (i = 0; i < ctx->num_dsp; i++) {
				if (device == i) {
					dc = &ctx->devs_config[FIRST_DSP_IDX(ctx) + i];
					bdf_map_update(ctx, tlp_type, bdf, dc);
					return dc;
				}
			}
		}

		/* PFs and no-ARI VFs sit on each DSP's secondary bus (device=0). */
		for (i = 0; i < ctx->num_ep; i++) {
			struct pci_device_config *ep = &ctx->devs_config[FIRST_PF_IDX(ctx) + i];
			uint8_t dsp_sec = ctx->devs_config[FIRST_DSP_IDX(ctx) + i].cfg_space_hdr.type1.secondary_bus;

			if (dsp_sec != 0 && bus == dsp_sec && device == 0) {
				if (function == 0 && ep->device_present) {
					bdf_map_update(ctx, tlp_type, bdf, ep);
					return ep;
				}
				if (function > 0 && ep->sriov != NULL) {
					struct vblk_sriov_pf_state *sriov = ep->sriov;
					struct pci_device_config *vf;
					uint32_t vf_function_offset;
					uint32_t vf_index;

					if (function < sriov->first_vf_offset || sriov->vf_stride == 0)
						continue;
					vf_function_offset = function - sriov->first_vf_offset;
					if (vf_function_offset % sriov->vf_stride != 0)
						continue;
					vf_index = vf_function_offset / sriov->vf_stride;
					if (vf_index < sriov->total_vfs)
						vf = sriov->vf_slots[vf_index].cfg;
					else
						vf = NULL;
					if (vf != NULL && vf->device_present) {
						dc = vf;
						bdf_map_update(ctx, tlp_type, bdf, dc);
						return dc;
					}
				}
			}
		}
	}

	return &ctx->devs_config[DUMMY_DEV_IDX(ctx)];
}

static struct pci_device_config *find_target_cfg_device(void *hdr, uint8_t tlp_type)
{
	uint8_t bus = GET_TLP_REQ_BUS(hdr);
	uint8_t device = GET_TLP_REQ_DEVICE(hdr);
	uint8_t function = GET_TLP_REQ_FUNCTION(hdr);
	uint16_t bdf = BDF(bus, device, function);
	struct pci_device_config *dc;

	dc = find_device_by_bdf(bus, device, function, tlp_type);

	if (dc->is_dummy)
		return dc;

	if (!dc->is_bdf_set) {
		const char *kind = dc->is_bridge ? "bridge" : (vblk_pci_is_vf(dc) ? "vf" : "endpoint");

		dc->bus = bus;
		dc->device = device;
		dc->function = function;
		dc->bdf = bdf;
		dc->completer_id = bdf;
		dc->is_bdf_set = true;
		DOCA_LOG_INFO("Device BDF set: %02x:%02x.%x (%s)", bus, device, function, kind);
		/* Sync virtio_dev BDF for endpoints */
		if (dc->is_endpoint && !vblk_pci_is_vf(dc)) {
			uint32_t idx = (uint32_t)(dc - &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx)]);
			if (idx < g_tlp_ctx.num_ep && g_tlp_ctx.virtio_devs[idx]) {
				g_tlp_ctx.virtio_devs[idx]->bus = bus;
				g_tlp_ctx.virtio_devs[idx]->device = device;
				g_tlp_ctx.virtio_devs[idx]->function = function;
				g_tlp_ctx.virtio_devs[idx]->is_enumerated = true;
			}
		}
	} else if (dc->bdf != bdf) {
		DOCA_LOG_DBG("BDF renumbered: %04x -> %04x", dc->bdf, bdf);
		bdf_map_remove(&g_tlp_ctx, tlp_type, dc->bdf);
		dc->bdf = bdf;
		dc->bus = bus;
		dc->device = device;
		dc->function = function;
		dc->completer_id = bdf;
		bdf_map_update(&g_tlp_ctx, tlp_type, bdf, dc);
	}

	return dc;
}

static struct vblk_pci_virtio_dev *virtio_dev_for_ep(struct pci_device_config *dc)
{
	uint32_t pf = FIRST_PF_IDX(&g_tlp_ctx);
	uint32_t idx;

	if (dc->is_dummy || !dc->is_endpoint)
		return NULL;
	idx = (uint32_t)(dc - &g_tlp_ctx.devs_config[pf]);
	return (idx < g_tlp_ctx.num_ep) ? g_tlp_ctx.virtio_devs[idx] : NULL;
}

/*
 * TLP completion helpers
 */

static void vblk_pci_tlp_cpl_prep_dw0(struct doca_devemu_pci_tlp_channel_req *tlp_req, int cmpl_fmt, int cmpl_length)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	/* DW0: fmt[31:29], type[28:24], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), cmpl_fmt, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), TLP_TYPE_COMPLETION, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), GET_TLP_REQ_TAG9(tlp_req_header), &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), GET_TLP_REQ_TAG8(tlp_req_header), &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), cmpl_length, &header_dw[0]);
}

static void vblk_pci_tlp_cpl_prep_dw1_2(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					const uint32_t completer_id,
					const int cmpl_status,
					const int byte_count)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	/* DW1: completer_id[31:16], cmpl_status[15:13], byte_cnt[11:0] (set to 0x04) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), completer_id, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), cmpl_status, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &header_dw[1]);

	/* DW2: requester_id[31:16], tag[15:8] (set to 0), lower_addr[6:0] (set to 0) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), GET_TLP_REQ_REQ_ID(tlp_req_header), &header_dw[2]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), GET_TLP_REQ_TAG(tlp_req_header), &header_dw[2]);
}

static void vblk_pci_tlp_cfg_cpl_prep(struct doca_devemu_pci_tlp_channel_req *tlp_req,
				      int cmpl_fmt,
				      int cmpl_status,
				      int cmpl_length)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	memset(header_dw, 0, 3 * sizeof(uint32_t));

	vblk_pci_tlp_cpl_prep_dw0(tlp_req, cmpl_fmt, cmpl_length);

	/* cfg requests are always bdf routed */
	uint16_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint16_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint16_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);
	uint16_t completer_id = (bus << 8) | (device << 3) | function;

	/* SPEC:
	 * - For Memory Read Completions, Byte Count[11:0] is set according to the rules in § Section
	 * 2.3.1.1
	 * - For AtomicOp Completions, the Byte Count value must equal the associated AtomicOp
	 *   operand size in bytes
	 * - For all other types of Completions, the Byte Count value must be 4
	 */
	vblk_pci_tlp_cpl_prep_dw1_2(tlp_req, completer_id, cmpl_status, 4);
}

static void vblk_pci_tlp_mmio_cpl_prep(struct vblk_pci_virtio_dev *virtio_dev,
				       struct doca_devemu_pci_tlp_channel_req *tlp_req,
				       int cmpl_fmt,
				       int cmpl_status,
				       int cmpl_length,
				       int byte_count)
{
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	memset(header_dw, 0, 3 * sizeof(uint32_t));
	vblk_pci_tlp_cpl_prep_dw0(tlp_req, cmpl_fmt, cmpl_length);

	const uint16_t completer_id = (virtio_dev->bus << 8) | (virtio_dev->device << 3) | virtio_dev->function;

	vblk_pci_tlp_cpl_prep_dw1_2(tlp_req, completer_id, cmpl_status, byte_count);
}

/*
 * Answer a Memory-Read whose target address has no live device mapping (EP just
 * unplugged, or BAR not yet programmed during a concurrent plug). The emulator
 * firmware asserts that every Memory-Read completion is 3DW_WITH_DATA
 * (fwassert 0x8A07) and panics on a data-less Cpl/UR, so return all-ones with
 * data, mirroring the PCIe semantics of reading a non-present device.
 * 'virtio_dev' may be NULL when no BAR matched; the completer_id is then unknown
 * (a Memory TLP carries an address, not a BDF) and is reported as 0.
 */
static void vblk_pci_tlp_mmio_read_all_ones_cpl(struct vblk_pci_virtio_dev *virtio_dev,
						struct doca_devemu_pci_tlp_channel_req *tlp_req,
						uint32_t length)
{
	uint32_t *cpl_header = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);
	uint32_t *cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	uint32_t dw = (length == 1 || length == 2) ? length : 1;
	uint16_t completer_id = 0;

	if (virtio_dev)
		completer_id = (virtio_dev->bus << 8) | (virtio_dev->device << 3) | virtio_dev->function;

	memset(cpl_header, 0, 3 * sizeof(uint32_t));
	for (uint32_t i = 0; i < dw; i++)
		cpl_data[i] = 0xFFFFFFFF;
	vblk_pci_tlp_cpl_prep_dw0(tlp_req, TLP_FMT_3DW_WITH_DATA, dw);
	vblk_pci_tlp_cpl_prep_dw1_2(tlp_req, completer_id, TLP_CPL_STATUS_SC, 4 * dw);
}

static uint32_t vblk_pci_bit_enable_to_mask(const uint32_t be_mask)
{
	uint32_t bit_mask = 0;

	if (be_mask & 0x1)
		bit_mask = 0xFFu;

	if (be_mask & (0x1 << 1))
		bit_mask |= (0xFFu << 8);

	if (be_mask & (0x1 << 2))
		bit_mask |= (0xFFu << 16);

	if (be_mask & (0x1 << 3))
		bit_mask |= (0xFFu << 24);

	return bit_mask;
}

static uint32_t vblk_pci_ext_reg_wr_mask(struct vblk_pci_virtio_dev *virtio_dev, const uint32_t ext_reg_num)
{
	struct pcie_raw_cfg *pcie_dev = TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev);

	switch (ext_reg_num) {
	/* Registers that can be treated as RO */
	case 0: /* vendor_id, device id - HwInit */
	case 2: /* revision_id HwInit, (prog_if, subclass, class_code) - RO */
	case 3:
		/*
		 * cache_line_size - RO, see comment
		 * SPEC: 7.5.1.1.7 Cache Line Size Register (Offset 0Ch)
		 * This read-write register is implemented for legacy compatibility purposes but has no
		 * effect on any PCI Express device behavior
		 *
		 * latency_timer - RO, see comment
		 * SPEC: 7.5.1.1.8 Latency Timer Register (Offset 0Dh)
		 * Its functionality does not apply to PCI Express.
		 * This register must be hardwired to 00h.
		 *
		 * header_type - RO
		 * bist - RO (not capable)
		 */
	case 10: /* cardbus_cis
		  * SPEC: 7.5.1.2.2 Cardbus CIS Pointer Register (Offset 28h)
		  * This register does not apply to PCI Express and must be hardwired
		  * to Zero
		  */
	case 11: /* subsystem_vendor_id, subsystem_id - RO */
	case 12: /* rom_address - not supported, RO, set to 0 */
	case 13: /* capabilities_ptr, HwInit */
	case 14: /* reserved */
	case 15: /* interrupt_line - RO, 0,
		  * SPEC: 7.5.1.1.12 Interrupt Line Register (Offset 3Ch)
		  * If Interrupt Pin Register is 00h, this register is
		  * permitted to be hardwired to 0b.
		  *
		  * interrupt_pin - RO 0
		  * SPEC: 7.5.1.1.13 Interrupt Pin Register (Offset 3Dh)
		  * A value of 00h indicates that the Function uses no legacy interrupt Message(s)
		  *
		  * min_gnt, max_lat - RO 0
		  * SPEC: 7.5.1.2.5 Min_Gnt Register/Max_Lat Register (Offset 3Eh/3Fh)
		  * These registers do not apply to PCI Express and must be hardwired to Zero.
		  */
		return 0;
	}

	/* BARs
	 * note that we only support 64 bit bars
	 */
	if (ext_reg_num >= 4 && ext_reg_num < 10) {
		const int bar64_id = (ext_reg_num - 4) / 2;

		if (pcie_dev->bar64_map[bar64_id].log_size == 0)
			return 0;

		if ((ext_reg_num - 4) % 2)
			return 0xFFFFFFFF;

		/* SPEC: Base Address Registers (Offset 10h - 24h)
		 * Bits 3-0 are  read-only.
		 */
		return ~((1 << pcie_dev->bar64_map[bar64_id].log_size) - 1) & (~0xF);
	}

	/* Status & command registers */
	if (ext_reg_num == 1) {
		/* Command reg RO and bits that can be assumed RO:
		 * SPEC: 7.5.1.1.3 Command Register (Offset 04h)
		 * 0 - PCI_COMMAND_IO
		 * This bit is permitted to be hardwired to Zero if a Function does not support I/O Space accesses
		 *
		 * 3 - PCI_COMMAND_SPECIAL
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 4 - PCI_COMMAND_INVALIDATE
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 5 - PCI_COMMAND_VGA_PALETTE
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 6 - PCI_COMMAND_PARITY
		 * An RCiEP that is not associated with a Root Complex Event Collector is permitted to hardwire this bit
		 * to 0b.
		 *
		 * 7 - PCI_COMMAND_WAIT
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 8 - PCI_COMMAND_SERR
		 * An RCiEP that is not associated with a Root Complex Event Collector is permitted to hardwire this bit
		 * to
		 *
		 * 9 - PCI_COMMAND_FAST_BACK
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 10 - PCI_COMMAND_INTX_DISABLE
		 * For Functions with a Type 0 Configuration Space Header that do not generate INTx interrupts,
		 * this bit is optional. If not implemented, this bit must be hardwired to 0b
		 *
		 * 11 - 15 - PCI_COMMAND_RES_BIT_MASK
		 */
		const uint16_t command_ro_mask = PCI_COMMAND_IO | PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
						 PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_PARITY | PCI_COMMAND_WAIT |
						 PCI_COMMAND_SERR | PCI_COMMAND_FAST_BACK | PCI_COMMAND_INTX_DISABLE |
						 PCI_COMMAND_RES_BIT_MASK;

		/* Status reg RO and bits that can be assumed RO:
		 * 7.5.1.1.4 Status Register (Offset 06h)
		 * 0 - PCI_STATUS_IMM_READY
		 * This optional bit, when Set, indicates the Function is guaranteed to be ready to
		 * successfully complete valid Configuration Requests at any time.
		 *
		 * 1 - 2 reserved
		 *
		 * 3 - PCI_STATUS_INTERRUPT
		 * Functions that do not generate INTx interrupts are permitted to hardwire this bit to 0b.
		 *
		 * 4 - PCI_STATUS_CAP_LIST
		 *
		 * 5 - PCI_STATUS_66MHZ
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 6 - reserved
		 *
		 * 7 - PCI_STATUS_FAST_BACK
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 8 - PCI_STATUS_PARITY RW1C but we can treat it as RO because we do not report parity errors
		 * If the Parity Error Response bit is 0b, this bit is never Set.
		 *
		 * 9 - 10 PCI_STATUS_DEVSEL_MASK
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 11 - PCI_STATUS_SIG_TARGET_ABORT RW1C can treat as RO
		 * Functions with a Type 0 Configuration Space Header that do not signal Completer Abort are permitted
		 * to hardwire this bit to 0b.
		 *
		 * 12 - PCI_STATUS_REC_TARGET_ABORT RW1C
		 * RO because functionality is not implemented
		 *
		 * 13 - PCI_STATUS_REC_MASTER_ABORT RW1C
		 * RO because functionality is not implemented
		 *
		 * 14 - PCI_STATUS_SIG_SYSTEM_ERROR RW1C, RO because SERR always 0
		 *
		 * This bit is Set when a Function sends an ERR_FATAL or ERR_NONFATAL Message, and the SERR# Enable
		 * bit in the Command Register is 1b
		 *
		 * Functions with a Type 0 Configuration Space Header that do not send ERR_FATAL or ERR_NONFATAL
		 * Messages are permitted to hardwire this bit to 0b
		 *
		 * 15 - PCI_STATUS_DETECTED_PARITY
		 * RO because functionality is not implemented
		 *
		 */
		/* bottom line we can treat status register as RO */
		const uint16_t status_ro_mask = 0xFFFF;

		return ~(((uint32_t)status_ro_mask << 16) | (uint32_t)command_ro_mask);
	}

	if (ext_reg_num == offsetof(struct pcie_virtio_dev, cfg.pcie_cap) / 4 ||
	    ext_reg_num == offsetof(struct pcie_virtio_dev, cfg.pcie_cap.dev_cap) / 4) {
		/* PCIe cap_id + next_ptr + pcie_cap register: RO
		 *     dev_cap : RO
		 */
		return 0;
	}

	if (ext_reg_num == offsetof(struct pcie_virtio_dev, cfg.pcie_cap.dev_ctrl) / 4) {
		/* PCIe dev_ctrl[15:0] + dev_status[31:16]
		 *     FLR bit (15), MPS field (7:5) and MRRS field (14:12) are RW so the
		 *     host can program payload/read-request sizes; dev_status stays RO.
		 */
		return (uint32_t)(PCI_EXP_DEVCTL_BCR_FLR | PCI_EXP_DEVCTL_PAYLOAD | PCI_EXP_DEVCTL_READRQ);
	}

	/* everything else is writable */
	return 0xFFFFFFFF;
}

/*
 * Type0 CFG Handlers (endpoints)
 */

static doca_error_t send_msi_via_memory_write_tlp(struct pci_device_config *dsp);

static void vblk_pci_cfg_read_all_ones_cpl(struct doca_devemu_pci_tlp_channel_req *tlp_req, uint32_t be)
{
	uint32_t data = 0xFFFFFFFF & vblk_pci_bit_enable_to_mask(be);
	void *cd = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);

	memcpy(cd, &data, sizeof(data));
	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);
	vblk_complete_tlp(tlp_req, 1, NULL);
}

/*
 * Unified config read handler for both Type0 and Type1.
 * Type0 -> USP (bridge)
 * Type1 -> DSP bridges or endpoints (via topology routing)
 */
static void vblk_pci_handle_cfg_read(struct doca_devemu_pci_tlp_channel_req *tlp_req, uint8_t tlp_type)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t reg = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	uint32_t be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	struct pci_device_config *dc;
	uint32_t data;
	void *cd;

	dc = find_target_cfg_device(tlp_req_header, tlp_type);

	if (dc->is_dummy) {
		/* Avoid surfacing UR as a host fatal error when pciehp probes an empty slot. */
		vblk_pci_cfg_read_all_ones_cpl(tlp_req, be);
		return;
	}

	if (dc->is_bridge) {
		if (vblk_pci_has_acs_ext_cap_reg(dc, reg)) {
			data = vblk_pci_acs_ext_cap_read_dw(reg - PCIE_EXT_CAP_FIRST_REG_NUM);
		} else if (vblk_pci_is_ext_cfg_reg(reg)) {
			data = 0;
		} else {
			data = bridge_cfg_read32(dc, reg);
		}
		data &= vblk_pci_bit_enable_to_mask(be);
		cd = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
		memcpy(cd, &data, 4);
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);
		vblk_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	/* Endpoint config read */
	if (vblk_pci_is_vf(dc)) {
		/* The VF's TLP device is created at SR-IOV init and stays alive across
		 * VFE disable, so a config request can still reach SW after the host
		 * clears VFE. A VF that is not present (VFE=0 or vf_index>=NumVFs) does
		 * not exist on the bus - complete with Unsupported Request, matching
		 * PCIe/SR-IOV (requests to a disabled VF receive UR). */
		if (!dc->device_present) {
			DOCA_LOG_ERR("SR-IOV: config read (reg dw %u) to not-present VF[%u] - returning UR",
				     reg,
				     dc->sriov_vf->dynamic_vf_index);
			vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
			doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
			return;
		}
		if (vblk_pci_is_ext_cfg_reg(reg))
			data = 0;
		else
			data = pcie_config_read(TO_PCIE_RAW_CFG(&dc->sriov_vf->pcie_dev), reg);
		data &= vblk_pci_bit_enable_to_mask(be);
		cd = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
		memcpy(cd, &data, 4);
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
		return;
	}

	struct vblk_pci_virtio_dev *virtio_dev = virtio_dev_for_ep(dc);
	if (!virtio_dev) {
		/* The EP was unplugged while the host still probes it; return an absent-device value. */
		vblk_pci_cfg_read_all_ones_cpl(tlp_req, be);
		return;
	}
	if (vblk_pci_has_sriov_ext_cap_reg(dc, reg)) {
		data = vblk_pci_sriov_ext_cap_read_dw(reg - PCIE_EXT_CAP_FIRST_REG_NUM, dc->sriov);
	} else if (vblk_pci_is_ext_cfg_reg(reg)) {
		data = 0;
	} else {
		data = pcie_config_read(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), reg);
	}
	data &= vblk_pci_bit_enable_to_mask(be);
	cd = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(cd, &data, 4);
	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);
	vblk_complete_tlp(tlp_req, 1, dc->tlp_dev);
}

/*
 * Unified config write handler for both Type0 and Type1.
 */
static void vblk_pci_handle_cfg_write(struct doca_devemu_pci_tlp_channel_req *tlp_req, uint8_t tlp_type)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	const uint32_t *td = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);
	uint32_t reg = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	uint32_t be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	struct pci_device_config *dc;
	uint32_t m;
	bool handle_flr_event = false;

	dc = find_target_cfg_device(tlp_req_header, tlp_type);

	if (dc->is_dummy) {
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
		vblk_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	m = vblk_pci_bit_enable_to_mask(be);

	if (dc->is_bridge) {
		bool slot_ctrl_changed = false;

		if (vblk_pci_is_ext_cfg_reg(reg)) {
			/* Unsupported extended config space is read-zero/write-ignore. */
		} else if (reg < PCI_STD_HEADER_REG_NUM) {
			bridge_hdr_write(dc, reg, td[0], m, &g_tlp_ctx);
		} else {
			slot_ctrl_changed = bridge_cap_write(dc, reg, td[0], m);
		}

		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
		vblk_complete_tlp(tlp_req, 1, NULL);

		/* Trigger Command Completed only when slot_control actually changed on a DSP.
		 * slot_status writes are TLP-only now (see the note in bridge_cap_write above); the
		 * atomic RMW is kept as a defensive guard. */
		if (slot_ctrl_changed && is_dsp(dc)) {
			__atomic_fetch_or(&dc->caps.express.slot_status, SLOT_STS_CMD_COMPLETED, __ATOMIC_RELEASE);
			DOCA_LOG_DBG("Command Completed: ctrl=0x%04x sts=0x%04x",
				     dc->caps.express.slot_control,
				     __atomic_load_n(&dc->caps.express.slot_status, __ATOMIC_ACQUIRE));
			/* Host pciehp blocks on Command Completed for each Slot Control write.
			 * If no ACG credit is available the MSI is deferred and retried on the
			 * TLP thread; otherwise the slot stalls for the full 1s timeout. */
			if (dc->caps.express.slot_control & SLOT_CTRL_CMD_COMPLETED_INT_EN) {
				if (send_msi_via_memory_write_tlp(dc) == DOCA_SUCCESS)
					__atomic_store_n(&dc->cmd_completed_msi_pending, false, __ATOMIC_RELEASE);
				else
					__atomic_store_n(&dc->cmd_completed_msi_pending, true, __ATOMIC_RELEASE);
			}
		}
		return;
	}

	/* VF config writes update the VF config shadow for guest-owned
	 * Command/MSI-X bits and handle VF FLR. */
	if (vblk_pci_is_vf(dc)) {
		struct vblk_sriov_vf_slot *slot = dc->sriov_vf;
		struct pcie_raw_cfg *vf_cfg = TO_PCIE_RAW_CFG(&slot->pcie_dev);
		const uint32_t cmd_reg = offsetof(struct pcie_virtio_dev, cfg.regs.command) / 4;
		const uint32_t msix_reg = offsetof(struct pcie_virtio_dev, cfg.msix_cap) / 4;
		const uint32_t dev_ctrl_reg = offsetof(struct pcie_virtio_dev, cfg.pcie_cap.dev_ctrl) / 4;
		bool handle_vf_flr = false;

		/* Not present (VFE=0 or vf_index>=NumVFs): the VF does not exist on the
		 * bus, so its config writes are dropped with Unsupported Request -
		 * matching the read path and PCIe/SR-IOV. The VF TLP device stays alive
		 * across VFE disable, so such a write can still reach SW. */
		if (!dc->device_present) {
			DOCA_LOG_ERR("SR-IOV: config write (reg dw %u) to not-present VF[%u] - returning UR",
				     reg,
				     slot->dynamic_vf_index);
			vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
			doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
			return;
		}

		/* Type-0 config write: commit the standard RW bits into the VF's
		 * emulated config image - the same image cfg reads serve from
		 * (vblk_pci_handle_cfg_read reads pcie_config_read on slot->pcie_dev). */
		if (reg == cmd_reg) {
			/* Command/Status dword. Only the two Command RW bits the VF
			 * datapath needs are writable: Memory Space Enable (decode the VF
			 * BAR) and Bus Master Enable (the device needs BME to issue the
			 * upstream MSI write that signals completions to the guest). Mask
			 * out every other Command bit and the entire Status word (upper
			 * 16b, RO/RW1C) so a wide dword write can't clobber them. This is a
			 * deliberately narrower allow-list than the PF's
			 * vblk_pci_ext_reg_wr_mask (a VF needs only MEM+BME), so it is
			 * hardcoded here rather than reusing that helper. */
			const uint32_t cmd_rw_mask = (uint32_t)(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
			uint32_t wr_mask = m & cmd_rw_mask;
			uint32_t cur = pcie_config_read(vf_cfg, reg);

			pcie_config_write(vf_cfg, reg, (cur & ~wr_mask) | (td[0] & wr_mask));
		} else if (reg == msix_reg) {
			/* MSI-X capability control (msgctl in the upper 16b of this dword):
			 * only MSI-X Enable and Function Mask are RW; the rest is RO.
			 * Without Enable the VF stays MSI-X-disabled and completions never
			 * interrupt the guest (I/O reaches the engine but the guest never
			 * sees it done). */
			const uint32_t msix_rw_mask = (uint32_t)(PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL) << 16;
			uint32_t cur = pcie_config_read(vf_cfg, reg);
			uint32_t rw_mask = m & msix_rw_mask;

			pcie_config_write(vf_cfg, reg, (cur & ~rw_mask) | (td[0] & rw_mask));
		} else if (reg == dev_ctrl_reg && (td[0] & m & PCI_EXP_DEVCTL_BCR_FLR) != 0) {
			DOCA_LOG_INFO("SR-IOV: VF[%u] FLR initiated via PCIe Device Control register",
				      slot->dynamic_vf_index);
			handle_vf_flr = true;
		}

		if (handle_vf_flr) {
			doca_error_t result = vblk_pci_sriov_reset_vf_slot(slot);

			if (result != DOCA_SUCCESS)
				DOCA_LOG_WARN("SR-IOV: VF[%u] FLR reset failed: %s",
					      slot->dynamic_vf_index,
					      doca_error_get_descr(result));
		}
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
		return;
	}

	struct vblk_pci_virtio_dev *virtio_dev = virtio_dev_for_ep(dc);
	if (!virtio_dev) {
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
		vblk_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	if (vblk_pci_has_sriov_ext_cap_reg(dc, reg)) {
		bool vfe_was_enabled = (dc->sriov->control_shadow & PCI_SRIOV_CTRL_VFE) != 0;
		bool vfe_is_enabled;

		vblk_pci_sriov_ext_cap_write(reg, td[0], m, dc);
		vfe_is_enabled = (dc->sriov->control_shadow & PCI_SRIOV_CTRL_VFE) != 0;
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
		if (!vfe_was_enabled && vfe_is_enabled)
			vblk_pci_sriov_set_vf_datapath_wanted(dc, true);
		else if (vfe_was_enabled && !vfe_is_enabled)
			vblk_pci_sriov_set_vf_datapath_wanted(dc, false);
		return;
	} else if (vblk_pci_is_ext_cfg_reg(reg)) {
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
		return;
	}

	m &= vblk_pci_ext_reg_wr_mask(virtio_dev, reg);
	uint32_t rd = pcie_config_read(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), reg);
	uint32_t wr = (rd & ~m) | (td[0] & m);

	if (reg == offsetof(struct pcie_virtio_dev, cfg.pcie_cap.dev_ctrl) / 4) {
		if (wr & PCI_EXP_DEVCTL_BCR_FLR) {
			DOCA_LOG_INFO("FLR initiated via PCIe Device Control register");
			wr &= ~PCI_EXP_DEVCTL_BCR_FLR;
			handle_flr_event = true;
		}
	}

	pcie_config_write(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), reg, wr);

	/* For a supported PF FLR, run the synchronous portion (mark device as in-progress and
	 * kick off the virtio reset callback that puts device_status into
	 * NEEDS_RESET and transitions all VQs to UNBINDING/STOPPING) BEFORE
	 * sending the SC.
	 *
	 * The asynchronous portion (VQ teardown by OE thread, io_ctx recycle,
	 * engine re-enable) still completes after the SC is sent. */
	if (handle_flr_event) {
		if (dc->sriov != NULL && dc->sriov->total_vfs != 0) {
			DOCA_LOG_WARN("PF[%u] FLR with SR-IOV is unsupported; ignoring the request",
				      virtio_dev->ep_index);
		} else if (virtio_dev->flr_in_progress) {
			/* Back-to-back FLR from the host while a prior FLR is
			 * still being cleaned up. The completion is still sent
			 * below so the host does not stall; the second FLR
			 * coalesces into the in-flight one. */
			DOCA_LOG_WARN("virtio_dev[%d] FLR in progress", virtio_dev->ep_index);
		} else {
			/* No-op for a non-SR-IOV PF (self-guards on ep->sriov / total_vfs). */
			vblk_pci_sriov_handle_pf_flr(dc);
			/* FLR returns the function to its reset state. Scrub the
			 * guest-writable config bits and rebuild the virtio
			 * common-cfg shadow on the TLP thread, which owns them.
			 * Without this a later re-probe still reads the previous
			 * driver's queue_enable/queue_desc and rejects the device.
			 * Mirrors vblk_pci_sriov_reset_vf_slot() for the VF. */
			virtio_dev->pcie_dev.cfg.regs.command &= (uint16_t) ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
			virtio_dev->pcie_dev.cfg.msix_cap.msgctl &=
				(uint16_t) ~(PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);
			vblk_pci_virtio_dev_reset(virtio_dev);
			virtio_dev->flr_in_progress = true;
			(void)vblk_pci_virtio_cfg_change_notify(virtio_dev);
		}
	}

	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
	vblk_complete_tlp(tlp_req, 1, dc->tlp_dev);
}

/*
 * MMIO Handlers
 */

static void vblk_pci_handle_mmio_read(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	const uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);

	/* lookup device by address */
	uint32_t *dw_header = (uint32_t *)tlp_req_header, *tlp_cpl_data;
	uint64_t addr;
	struct vblk_pci_virtio_dev *virtio_dev;
	uint32_t dw_mask, bit_en_mask;
	int byte_count;

	if (fmt == TLP_FMT_4DW_NO_DATA)
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
	else
		addr = be32toh(dw_header[2]) & ~0x3;

	DOCA_LOG_TRC("MMIO read 0x%lx len %d", addr, length);

	/* requests are routed by the mmio address */
	virtio_dev = vblk_pci_dev_find_by_addr(addr);
	if (!virtio_dev)
		virtio_dev = vblk_pci_sriov_find_vf_dev_by_mmio_addr(addr);
	if (!virtio_dev) {
		/* No live device at this address (EP unplugged, or BAR not yet programmed
		 * during a concurrent plug). A data-less UR would trip the emulator firmware
		 * Memory-Read assert (fwassert 0x8A07); answer all-ones instead. */
		DOCA_LOG_DBG("MMIO read 0x%lx has no device mapping, returning all-ones", addr);
		vblk_pci_tlp_mmio_read_all_ones_cpl(NULL, tlp_req, length);
		vblk_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	/* at the moment we only support 1 & 2 dwords reads */
	if (length > 2) {
		DOCA_LOG_WARN("unsupported read length %u, returning all-ones", length);
		goto read_all_ones;
	}

	if (length == 0) {
		DOCA_LOG_DBG("zero-length read, returning all-ones");
		goto read_all_ones;
	}

	tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	byte_count = 4 * length;

	bit_en_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
	byte_count -= (4 - __builtin_popcount(bit_en_mask));
	tlp_cpl_data[0] = (vblk_pci_virtio_mmio_read32(virtio_dev, addr) & dw_mask);

	DOCA_LOG_TRC("MMIO read result: 0x%lx result 0x%x mask 0x%x", addr, tlp_cpl_data[0], dw_mask);

	if (length == 2) {
		bit_en_mask = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
		dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
		byte_count -= (4 - __builtin_popcount(bit_en_mask));
		tlp_cpl_data[1] = (vblk_pci_virtio_mmio_read32(virtio_dev, addr + 4) & dw_mask);
	}

	vblk_pci_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, length, byte_count);
	vblk_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

read_all_ones:
	vblk_pci_tlp_mmio_read_all_ones_cpl(virtio_dev, tlp_req, length);
	vblk_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
}

static void vblk_pci_handle_mmio_write(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t *tlp_req_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);	/* data */
	const uint32_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	const uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);

	/* lookup device by address */
	uint32_t *dw_header = (uint32_t *)tlp_req_header;
	uint64_t addr;
	struct vblk_pci_virtio_dev *virtio_dev;
	enum vblk_cfg_wr_notify_sched notify_sched = VBLK_CFG_WR_NO_NOTIFY;

	if (fmt == TLP_FMT_4DW_WITH_DATA)
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
	else
		addr = be32toh(dw_header[2]) & ~0x3;

	DOCA_LOG_TRC("MMIO write 0x%lx len %d data1 0x%0x", addr, length, length > 0 ? tlp_req_data[0] : 0);

	/* requests are routed by the mmio address */
	virtio_dev = vblk_pci_dev_find_by_addr(addr);
	if (!virtio_dev)
		virtio_dev = vblk_pci_sriov_find_vf_dev_by_mmio_addr(addr);
	if (!virtio_dev) {
		DOCA_LOG_ERR("fatal invalid mmio address 0x%lx", addr);
		vblk_complete_tlp(tlp_req, 0, NULL);
		return;
	}

	/* at the moment we only support 1 & 2 dwords writes */
	if (length > 2) {
		DOCA_LOG_ERR("unsupported write length %d", length);
		goto done;
	}

	if (length == 0) {
		DOCA_LOG_ERR("zero write %d", length);
		goto done;
	}

	uint32_t dw_mask;
	uint32_t bit_en_mask;
	enum vblk_cfg_wr_notify_sched wr_sched;

	/*
	 * Shadow is always updated before complete. Notify scheduling:
	 *  - status->0 reset: notify BEFORE complete so virtio_ctrl_change_cb can
	 *    hold NEEDS_RESET (0x40) before host NP-reads a transient 0 and re-inits.
	 *  - DRIVER_OK / other cfg: complete BEFORE notify so the TLP channel is not
	 *    held across heavy ctrl work (NP timeout under many VFs).
	 */
	bit_en_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
	wr_sched = vblk_pci_virtio_mmio_write32(virtio_dev, addr, tlp_req_data[0], dw_mask);
	if (wr_sched > notify_sched)
		notify_sched = wr_sched;

	if (length == 2) {
		bit_en_mask = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
		dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
		wr_sched = vblk_pci_virtio_mmio_write32(virtio_dev, addr + 4, tlp_req_data[1], dw_mask);
		if (wr_sched > notify_sched)
			notify_sched = wr_sched;
	}

done:
	if (notify_sched == VBLK_CFG_WR_NOTIFY_BEFORE_COMPLETE)
		(void)vblk_pci_virtio_cfg_change_notify(virtio_dev);
	vblk_complete_tlp(tlp_req, 0, virtio_dev->pci_tlp_dev);
	if (notify_sched == VBLK_CFG_WR_NOTIFY_AFTER_COMPLETE)
		(void)vblk_pci_virtio_cfg_change_notify(virtio_dev);
}

/*
 * Recycle one PF EP on PERST# assert by driving the same reset sequence as
 * vblk_pci_sriov_reset_vf_slot() applies to a VF: the OE thread handles the
 * io_ctx destroy/rebuild (including the application payload dma_ctx and the
 * SDK-internal metadata dma_ctx) through the existing post-FLR chain.
 */
static void vblk_pci_perst_recycle_ep(struct vblk_pci_virtio_dev *dev)
{
	if (dev == NULL)
		return;

	if (atomic_load_explicit(&dev->flr_in_progress, memory_order_acquire)) {
		DOCA_LOG_INFO("PERST: EP[%u] FLR already in progress, skipping", dev->ep_index);
		return;
	}

	dev->pci_cfg.device_status = 0;
	vblk_pci_virtio_dev_reset(dev);
	atomic_store_explicit(&dev->flr_kickoff, false, memory_order_release);
	atomic_store_explicit(&dev->flr_in_progress, true, memory_order_release);
	if (!vblk_pci_virtio_cfg_change_notify(dev)) {
		atomic_store_explicit(&dev->flr_in_progress, false, memory_order_release);
		DOCA_LOG_WARN("PERST: EP[%u] cfg change cb not registered, cannot recycle", dev->ep_index);
		return;
	}

	DOCA_LOG_INFO("PERST: EP[%u] triggered controller recycle", dev->ep_index);
}

/*
 * PCI Event and TLP Dispatch
 */
static void vblk_pci_handle_pci_event(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	enum doca_devemu_pci_tlp_channel_req_pci_event_opmode pci_event_opmode =
		doca_devemu_pci_tlp_channel_req_get_pci_event_opmode(tlp_req);
	switch (pci_event_opmode) {
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_ASSERT:
		if (atomic_exchange_explicit(&g_perst_round_in_progress, true, memory_order_acq_rel)) {
			DOCA_LOG_INFO("PERST# re-asserted while previous round still in progress, skipping");
			break;
		}
		DOCA_LOG_INFO("PERST# is asserted (enters reset) - recycling %u EP controller(s)", g_tlp_ctx.num_ep);
		for (uint32_t i = 0; i < g_tlp_ctx.num_ep; i++)
			vblk_pci_perst_recycle_ep(g_tlp_ctx.virtio_devs[i]);
		break;
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_DEASSERT:
		DOCA_LOG_INFO("PERST# is deasserted (released from reset)");
		atomic_store_explicit(&g_perst_round_in_progress, false, memory_order_release);
		break;
	default:
		DOCA_LOG_WARN("Unknown PCI_EVENT opmode: %d", pci_event_opmode);
		break;
	}

	/* Complete the PCI_EVENT request */
	vblk_complete_pci_event(tlp_req);
}

/* ACG-flow diagnostics. Counters increment on every event so the running totals
 * can be cross-checked against per-event logs. With firmware max_acg=1 the
 * enqueue/consume rate is bounded by the host's PCIe acceptance rate, so the
 * INFO volume is comparable to the existing "MSI sent" lines. */
static _Atomic uint64_t g_acg_total_enqueued;
static _Atomic uint64_t g_acg_total_consumed;
static _Atomic uint64_t g_acg_total_starved;
static _Atomic uint64_t g_acg_total_flushed;

static void vblk_pci_ev_cb(struct doca_devemu_pci_tlp_channel *channel,
			   struct doca_devemu_pci_tlp_channel_req *tlp_req,
			   void *req_user_data)
{
	enum doca_devemu_pci_tlp_channel_req_opcode opcode = doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req);
	enum tlp_req_type type;

	(void)channel;
	(void)req_user_data;

	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_ACG) {
		uint16_t count;
		uint16_t device_max;
		bool flush;

		/* acg_queue is TLP-thread private: producer (this callback) and
		 * consumers (send_msi_via_memory_write_tlp, vblk_pci_stop) all run
		 * on the TLP thread, so no lock is needed. */
		flush = (g_tlp_ctx.acg_queue == NULL || g_tlp_ctx.acg_queue_count >= g_tlp_ctx.acg_queue_size);
		if (!flush) {
			g_tlp_ctx.acg_queue[g_tlp_ctx.acg_queue_tail] = tlp_req;
			g_tlp_ctx.acg_queue_tail = (g_tlp_ctx.acg_queue_tail + 1) % g_tlp_ctx.acg_queue_size;
			g_tlp_ctx.acg_queue_count++;
		}
		count = g_tlp_ctx.acg_queue_count;
		device_max = g_tlp_ctx.acg_device_max;

		if (flush) {
			uint64_t flushed_total =
				atomic_fetch_add_explicit(&g_acg_total_flushed, 1, memory_order_relaxed) + 1;
			/* Should not happen with VBLK_ACG_QUEUE_BUFFER_SIZE buffer. If it does,
			 * firmware is wildly over-granting (or buffer is somehow undersized). */
			DOCA_LOG_ERR("ACG flushed (saturated at %u entries) total_flushed=%lu",
				     g_tlp_ctx.acg_queue_size,
				     flushed_total);
			vblk_complete_acg(tlp_req, 0, DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		} else {
			uint64_t enq_total =
				atomic_fetch_add_explicit(&g_acg_total_enqueued, 1, memory_order_relaxed) + 1;
			if (count > device_max) {
				/* Firmware briefly held more ACGs outstanding than its reported max.
				 * Buffer absorbed it; just record the race for diagnostics. */
				DOCA_LOG_WARN(
					"ACG enqueued (%u/%u) - outstanding %u exceeds firmware max %u, total_enq=%lu",
					count,
					g_tlp_ctx.acg_queue_size,
					count,
					device_max,
					enq_total);
			} else {
				DOCA_LOG_INFO("ACG enqueued (%u/%u) total_enq=%lu",
					      count,
					      g_tlp_ctx.acg_queue_size,
					      enq_total);
			}
		}
		return;
	}

	if (doca_unlikely(opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_PCI_EVENT)) {
		vblk_pci_handle_pci_event(tlp_req);
		return;
	}

	type = tlp_req_get_type(tlp_req);
	switch (type) {
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_0:
		vblk_pci_handle_cfg_read(tlp_req, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0);
		return;
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0:
		vblk_pci_handle_cfg_write(tlp_req, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0);
		return;
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_1:
		vblk_pci_handle_cfg_read(tlp_req, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1);
		return;
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1:
		vblk_pci_handle_cfg_write(tlp_req, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1);
		return;
	case TLP_REQ_TYPE_MEMORY_READ:
		vblk_pci_handle_mmio_read(tlp_req);
		return;
	case TLP_REQ_TYPE_MEMORY_WRITE:
		vblk_pci_handle_mmio_write(tlp_req);
		return;
	default:
		DOCA_LOG_ERR("TLP type %d is not supported", type);
		vblk_complete_tlp(tlp_req, 0, NULL);
	}
}

/* Over-provisioned ACG ring buffer size. Firmware reports a small max_acg
 * (typically 1) which means at most that many ACG credits are in flight at any
 * time. Using size == max_acg makes the producer take the FLUSH path whenever a
 * new credit arrives while we are still draining the previous one (a frequent
 * race under 32-EP hotplug stress). FLUSH returns the credit to the firmware,
 * but observation shows that under sustained stress this eventually starves the
 * firmware ACG flow and MSI sends stop succeeding altogether. Over-provisioning
 * the buffer keeps every credit queued for the consumer, so the FLUSH path is
 * only hit if the buffer truly fills (which would itself indicate a firmware
 * issue worth logging). */
#define VBLK_ACG_QUEUE_BUFFER_SIZE 256

static doca_error_t init_acg_queue(void)
{
	uint16_t max_acg;
	doca_error_t err;

	err = doca_devemu_pci_tlp_cap_get_max_acg(doca_dev_as_devinfo(g_tlp_ctx.dev), &max_acg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query max ACG: %s", doca_error_get_descr(err));
		return err;
	}
	if (max_acg == 0) {
		DOCA_LOG_WARN("Device reports 0 ACG credits - MSI disabled");
		return DOCA_SUCCESS;
	}
	g_tlp_ctx.acg_device_max = max_acg;
	g_tlp_ctx.acg_queue_size = VBLK_ACG_QUEUE_BUFFER_SIZE;
	g_tlp_ctx.acg_queue = calloc(g_tlp_ctx.acg_queue_size, sizeof(struct doca_devemu_pci_tlp_channel_req *));
	if (g_tlp_ctx.acg_queue == NULL)
		return DOCA_ERROR_NO_MEMORY;
	DOCA_LOG_INFO("ACG queue initialized: device_max=%u buffer=%u", max_acg, g_tlp_ctx.acg_queue_size);
	return DOCA_SUCCESS;
}

static doca_error_t vblk_pci_ev_channel_init(struct doca_dev *dev)
{
	doca_error_t err;

	err = doca_devemu_pci_tlp_channel_create(dev, &g_tlp_ctx.tlp_channel);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create even channel");
		return err;
	}

	err = doca_devemu_pci_tlp_channel_event_req_register(g_tlp_ctx.tlp_channel, vblk_pci_ev_cb);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register event callback");
		doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
		g_tlp_ctx.tlp_channel = NULL;
		return err;
	}

	if (g_tlp_ctx.hotplug_mode) {
		err = doca_devemu_pci_tlp_channel_set_acg_enabled(g_tlp_ctx.tlp_channel, 1);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to enable ACG: %s", doca_error_get_descr(err));
			doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
			g_tlp_ctx.tlp_channel = NULL;
			return err;
		}
		DOCA_LOG_INFO("ACG enabled for MSI interrupts");
	}
	return DOCA_SUCCESS;
}

struct doca_ctx *vblk_pci_ev_channel_ctx(void)
{
	return doca_devemu_pci_tlp_channel_as_ctx(g_tlp_ctx.tlp_channel);
}

doca_error_t vblk_pci_tlp_start(void)
{
	doca_error_t err;

	err = doca_ctx_start(vblk_pci_ev_channel_ctx());
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start vblk_pci event channel: %s", doca_error_get_descr(err));
		return err;
	}

	/* The single-USP topology (1 USP + N DSP + N EP) requires
	 * exactly one physical NV switch TLP downstream port.
	 * N (emulated endpoints from -n) is unaffected by this check. */
	uint8_t num_nv_switch_tlp_dsp = 0;

	err = doca_devemu_pci_tlp_channel_get_num_dsp(g_tlp_ctx.tlp_channel, &num_nv_switch_tlp_dsp);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get num_dsp: %s", doca_error_get_descr(err));
		goto stop;
	}

	if (num_nv_switch_tlp_dsp != 1) {
		DOCA_LOG_ERR("Expected 1 NV switch TLP DSP, got %u; "
			     "use mlxconfig to set TLP ports to 1",
			     num_nv_switch_tlp_dsp);
		err = DOCA_ERROR_NOT_SUPPORTED;
		goto stop;
	}

	return DOCA_SUCCESS;

stop:
	doca_ctx_stop(vblk_pci_ev_channel_ctx());
	return err;
}

void vblk_pci_stop(void)
{
	doca_error_t err;

	err = doca_ctx_stop(vblk_pci_ev_channel_ctx());
	if (err != DOCA_SUCCESS && err != DOCA_ERROR_IN_PROGRESS)
		DOCA_LOG_ERR("Failed to stop vblk_pci event channel: %s", doca_error_get_descr(err));

	/* Flush pending ACG credits after stopping TLP channel. All acg_queue
	 * accessors are TLP-thread only, so no lock is needed here. */
	while (g_tlp_ctx.acg_queue && g_tlp_ctx.acg_queue_count > 0) {
		struct doca_devemu_pci_tlp_channel_req *acg = g_tlp_ctx.acg_queue[g_tlp_ctx.acg_queue_head];

		g_tlp_ctx.acg_queue_head = (g_tlp_ctx.acg_queue_head + 1) % g_tlp_ctx.acg_queue_size;
		g_tlp_ctx.acg_queue_count--;
		doca_devemu_pci_tlp_channel_req_complete_acg(acg,
							     0,
							     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
	}

	free(g_tlp_ctx.acg_queue);
	g_tlp_ctx.acg_queue = NULL;
}

static void vblk_pci_virtio_reset(void)
{
	if (g_tlp_ctx.pci_type) {
		if (DOCA_SUCCESS != doca_devemu_pci_type_stop(g_tlp_ctx.pci_type)) {
			DOCA_LOG_ERR("Failed to stop pci_type: %p", g_tlp_ctx.pci_type);
		}
		if (DOCA_SUCCESS != doca_devemu_pci_type_destroy(g_tlp_ctx.pci_type)) {
			DOCA_LOG_ERR("Failed to destroy pci_type: %p", g_tlp_ctx.pci_type);
		}
		g_tlp_ctx.pci_type = NULL;
	}
}

void vblk_pci_reset(void)
{
	doca_error_t result;
	uint32_t i, pf;

	if (g_tlp_ctx.virtio_devs) {
		for (i = 0; i < g_tlp_ctx.num_ep; i++)
			if (g_tlp_ctx.virtio_devs[i])
				vblk_pci_virtio_dev_destroy(g_tlp_ctx.virtio_devs[i]);
	}
	if (g_tlp_ctx.devs_config) {
		pf = FIRST_PF_IDX(&g_tlp_ctx);
		for (i = 0; i < g_tlp_ctx.num_ep; i++) {
			struct pci_device_config *ep = &g_tlp_ctx.devs_config[pf + i];

			if (ep->tlp_dev && ep->device_present) {
				vblk_pci_sriov_sync_vf_presence(ep, false);
				result = doca_devemu_pci_tlp_dev_stop(ep->tlp_dev);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
				ep->device_present = false;
			}
			vblk_pci_sriov_free_pf_state(ep);
			if (ep->tlp_dev) {
				result = doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to destroy TLP device: %s", doca_error_get_descr(result));
				ep->tlp_dev = NULL;
			}
			vblk_pci_destroy_pf_rep(ep);
		}
	}
	vblk_pci_virtio_reset();
	if (g_tlp_ctx.tlp_channel) {
		doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
		g_tlp_ctx.tlp_channel = NULL;
	}
	free(g_tlp_ctx.virtio_devs);
	free(g_tlp_ctx.bdf_entries);
	free(g_tlp_ctx.devs_config);
	vblk_pci_sriov_pool_cleanup();
	memset(&g_tlp_ctx, 0, sizeof(g_tlp_ctx));
}

doca_error_t vblk_pci_init(const struct vblk_pci_init_cfg *cfg)
{
	doca_error_t err;
	uint16_t n;

	if (cfg == NULL || cfg->dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	if (cfg->sriov_enabled) {
		/* TotalVFs range is validated by the --total-vfs CLI callback. */
		if (cfg->sriov_num_vf_msix > VBLK_PCI_VIRTIO_MAX_NUM_MSIX ||
		    cfg->sriov_num_vf_db > VBLK_PCI_VIRTIO_MAX_NUM_DB) {
			DOCA_LOG_ERR("Invalid SR-IOV VF resources: vf_msix=%u (max %u) vf_db=%u (max %u)",
				     cfg->sriov_num_vf_msix,
				     VBLK_PCI_VIRTIO_MAX_NUM_MSIX,
				     cfg->sriov_num_vf_db,
				     VBLK_PCI_VIRTIO_MAX_NUM_DB);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	memset(&g_tlp_ctx, 0, sizeof(g_tlp_ctx));
	g_tlp_ctx.dev = cfg->dev;
	g_tlp_ctx.unplug_active = -1;
	g_tlp_ctx.sriov_enabled = cfg->sriov_enabled;
	g_tlp_ctx.sriov_total_vfs = cfg->sriov_total_vfs;
	g_tlp_ctx.sriov_num_vf_msix = cfg->sriov_num_vf_msix;
	g_tlp_ctx.sriov_num_vf_db = cfg->sriov_num_vf_db;

	err = doca_devemu_pci_tlp_cap_get_max_types(doca_dev_as_devinfo(cfg->dev), &n);
	if (err != DOCA_SUCCESS || n == 0) {
		DOCA_LOG_ERR("No TLP emulation support");
		return err != DOCA_SUCCESS ? err : DOCA_ERROR_NOT_SUPPORTED;
	}
	DOCA_LOG_INFO("device supports %d pci tlp types", n);

	if (cfg->sriov_enabled) {
		err = vblk_pci_sriov_probe_caps(doca_dev_as_devinfo(cfg->dev));

		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("SR-IOV dynamic VF cap probe failed (%s); SR-IOV requires dynamic "
				     "VF backing (rep_ex) and has no non-rep_ex fallback datapath",
				     doca_error_get_descr(err));
			return err;
		}

		err = vblk_pci_sriov_validate_caps(doca_dev_as_devinfo(cfg->dev), cfg->num_ep);
		if (err != DOCA_SUCCESS) {
			vblk_pci_sriov_pool_cleanup();
			return err;
		}
	}

	err = vblk_pci_switch_init(&g_tlp_ctx, cfg->num_ep, cfg->hotplug_mode);
	if (err != DOCA_SUCCESS) {
		vblk_pci_sriov_pool_cleanup();
		return err;
	}

	err = vblk_pci_ev_channel_init(cfg->dev);
	if (err != DOCA_SUCCESS)
		goto fail;

	if (cfg->hotplug_mode) {
		err = init_acg_queue();
		if (err != DOCA_SUCCESS) {
			doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
			goto fail;
		}
	}

	err = vblk_pci_virtio_block_init(cfg->dev);
	if (err != DOCA_SUCCESS) {
		free(g_tlp_ctx.acg_queue);
		doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
		goto fail;
	}
	DOCA_LOG_INFO("vblk_pci initialized: num_ep=%u hotplug=%d", cfg->num_ep, cfg->hotplug_mode);
	return DOCA_SUCCESS;
fail:
	free(g_tlp_ctx.virtio_devs);
	free(g_tlp_ctx.bdf_entries);
	free(g_tlp_ctx.devs_config);
	vblk_pci_sriov_pool_cleanup();
	memset(&g_tlp_ctx, 0, sizeof(g_tlp_ctx));
	return err;
}

/*
 * Endpoint Device Management
 */

doca_error_t vblk_pci_create_device(uint32_t ep_index)
{
	struct pci_device_config *ep;
	doca_error_t err;

	if (ep_index >= g_tlp_ctx.num_ep) {
		DOCA_LOG_ERR("'ep_index' %u out of range (max %u)", ep_index, g_tlp_ctx.num_ep);
		return DOCA_ERROR_INVALID_VALUE;
	}
	ep = &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + ep_index];
	if (ep->rep || ep->tlp_dev) {
		DOCA_LOG_ERR("Endpoint %u already has TLP resources", ep_index);
		return DOCA_ERROR_ALREADY_EXIST;
	}
	err = vblk_pci_create_pf_rep(ep_index, ep);
	if (err != DOCA_SUCCESS)
		return err;
	err = doca_devemu_pci_tlp_dev_create(g_tlp_ctx.pci_type, ep->rep, &ep->tlp_dev);
	if (err != DOCA_SUCCESS) {
		vblk_pci_destroy_pf_rep(ep);
		return err;
	}
	err = doca_devemu_pci_tlp_dev_start(ep->tlp_dev);
	if (err != DOCA_SUCCESS) {
		doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
		ep->tlp_dev = NULL;
		vblk_pci_destroy_pf_rep(ep);
		return err;
	}
	ep->device_present = true;
	vblk_pci_sriov_sync_vf_presence(ep, true);
	DOCA_LOG_INFO("Created device for endpoint %u", ep_index);
	return DOCA_SUCCESS;
}

/*
 * Acknowledge host's vp_reset before destroying the virtio device on unplug.
 * The host's virtblk_remove path writes device_status=0 and then msleep-polls
 * until it reads 0. If we destroy first, no one updates device_status and the
 * host poll never returns; the pciehp irq thread holding the slot mutex then
 * blocks forever, wedging all other pciehp threads (whole hotplug subsystem
 * hangs). Force device_status to 0 here so the host's poll exits promptly.
 */
void vblk_pci_ack_host_reset(uint32_t ep_index)
{
	struct vblk_pci_virtio_dev *dev;
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg;

	if (ep_index >= g_tlp_ctx.num_ep)
		return;
	dev = g_tlp_ctx.virtio_devs[ep_index];
	if (dev == NULL)
		return;
	pci_cfg = vblk_pci_virtio_get_pci_cfg(dev);
	if (pci_cfg != NULL)
		pci_cfg->device_status = 0;
}

void vblk_pci_destroy_device(uint32_t ep_index)
{
	struct pci_device_config *ep;
	doca_error_t result;

	if (ep_index >= g_tlp_ctx.num_ep)
		return;
	ep = &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + ep_index];

	if (g_tlp_ctx.virtio_devs[ep_index]) {
		vblk_pci_virtio_dev_destroy(g_tlp_ctx.virtio_devs[ep_index]);
		g_tlp_ctx.virtio_devs[ep_index] = NULL;
	}
	if (ep->tlp_dev) {
		if (ep->device_present) {
			vblk_pci_sriov_sync_vf_presence(ep, false);
			result = doca_devemu_pci_tlp_dev_stop(ep->tlp_dev);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
		}
		ep->device_present = false;
		vblk_pci_sriov_free_pf_state(ep);
		result = doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy TLP device: %s", doca_error_get_descr(result));
		ep->tlp_dev = NULL;
	} else {
		ep->device_present = false;
		vblk_pci_sriov_free_pf_state(ep);
	}
	vblk_pci_destroy_pf_rep(ep);
	if (ep->is_bdf_set) {
		bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0, ep->bdf);
		bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1, ep->bdf);
		ep->is_bdf_set = false;
	}
	DOCA_LOG_INFO("Destroyed endpoint %u", ep_index);
}

static size_t set_memory_write_tlp_header(void *buf, uint64_t msi_addr, uint16_t msi_data, uint16_t req_bdf)
{
	uint32_t *dw = (uint32_t *)buf;
	bool use_64 = (msi_addr >> 32) != 0;
	size_t ndw = use_64 ? 5 : 4;

	memset(buf, 0, ndw * sizeof(uint32_t));

	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), use_64 ? MEM_WR_FMT_4DW_W_DATA : MEM_WR_FMT_3DW_W_DATA, &dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), MEM_WR_TYPE, &dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), 1, &dw[0]);

	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), req_bdf, &dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(3, 0), 0xF, &dw[1]);

	if (use_64) {
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 0), (uint32_t)(msi_addr >> 32), &dw[2]);
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 2), (uint32_t)(msi_addr >> 2), &dw[3]);
		dw[4] = msi_data;
	} else {
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 2), (uint32_t)(msi_addr >> 2), &dw[2]);
		dw[3] = msi_data;
	}
	return ndw * sizeof(uint32_t);
}

static doca_error_t send_msi_via_memory_write_tlp(struct pci_device_config *dsp)
{
	struct doca_devemu_pci_tlp_channel_req *acg;
	void *acg_buf;

	if (!(dsp->caps.msi.message_control & 0x0001)) {
		DOCA_LOG_DBG("MSI not enabled on DSP %04x", dsp->bdf);
		return DOCA_ERROR_BAD_STATE;
	}

	uint64_t msi_addr = ((uint64_t)dsp->caps.msi.message_address_high << 32) | dsp->caps.msi.message_address_low;
	if (msi_addr == 0) {
		DOCA_LOG_DBG("MSI address not configured on DSP %04x", dsp->bdf);
		return DOCA_ERROR_BAD_STATE;
	}

	uint16_t msi_data = dsp->caps.msi.message_data;
	uint16_t dsp_bdf = BDF(dsp->cfg_space_hdr.type1.primary_bus, dsp->device, dsp->function);

	/* acg_queue is TLP-thread private (see vblk_pci_ev_cb comment); no lock. */
	if (g_tlp_ctx.acg_queue == NULL || g_tlp_ctx.acg_queue_count == 0) {
		uint64_t starved = atomic_fetch_add_explicit(&g_acg_total_starved, 1, memory_order_relaxed) + 1;
		/* Host-driven Slot Control writes can trigger this at high rate;
		 * g_acg_total_starved already tracks it, keep the log at DBG to
		 * avoid flooding. */
		DOCA_LOG_DBG("ACG starved on send total_starved=%lu", starved);
		return DOCA_ERROR_AGAIN;
	}

	acg = g_tlp_ctx.acg_queue[g_tlp_ctx.acg_queue_head];
	g_tlp_ctx.acg_queue_head = (g_tlp_ctx.acg_queue_head + 1) % g_tlp_ctx.acg_queue_size;
	g_tlp_ctx.acg_queue_count--;
	uint16_t remaining = g_tlp_ctx.acg_queue_count;

	uint64_t cons_total = atomic_fetch_add_explicit(&g_acg_total_consumed, 1, memory_order_relaxed) + 1;
	DOCA_LOG_INFO("ACG consumed for MSI, queue=%u total_cons=%lu", remaining, cons_total);

	acg_buf = doca_devemu_pci_tlp_channel_req_get_acg_buf(acg);
	if (acg_buf == NULL) {
		DOCA_LOG_ERR("Failed to get ACG buffer");
		vblk_complete_acg(acg, 0, DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		return DOCA_ERROR_DRIVER;
	}

	size_t tlp_size = set_memory_write_tlp_header(acg_buf, msi_addr, msi_data, dsp_bdf);

	vblk_complete_acg(acg, tlp_size, DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_MMIO_WRITE);
	DOCA_LOG_INFO("MSI sent: DSP %04x addr=0x%lx data=0x%x", dsp_bdf, msi_addr, msi_data);
	return DOCA_SUCCESS;
}

/*
 * Send an ABP + slot-status update and the corresponding hotplug MSI.
 *
 * Must be called on the TLP thread only. The MSI eventually calls
 * doca_devemu_pci_tlp_channel_req_complete_acg(), which drives the lib's
 * req_pool.release(); the matching acquire() runs on the TLP thread inside
 * doca_pe_progress()'s ev_cb dispatch. doca ctx is thread-unsafe by design,
 * so mixing threads there corrupts the LIFO pool (root cause of the
 * "Request opcode is not ACG" abort). OE queues plug/unplug MSI via the
 * plug_msi_pending / unplug_msi_pending flags, and vblk_pci_retry_hotplug_msi
 * (below) is the only caller on the hotplug path.
 */
doca_error_t vblk_pci_trigger_hotplug(uint32_t dsp_index, bool plug)
{
	struct pci_device_config *dsp;

	if (!g_tlp_ctx.hotplug_mode) {
		DOCA_LOG_ERR("hotplug trigger called in static mode");
		return DOCA_ERROR_BAD_STATE;
	}
	if (dsp_index >= g_tlp_ctx.num_dsp) {
		DOCA_LOG_ERR("'dsp_index' %u out of range", dsp_index);
		return DOCA_ERROR_INVALID_VALUE;
	}
	dsp = &g_tlp_ctx.devs_config[FIRST_DSP_IDX(&g_tlp_ctx) + dsp_index];

	if (!(dsp->caps.express.slot_control & SLOT_CTRL_HP_INT_EN)) {
		/* Host has not (yet) enabled hotplug IRQ on this slot, or has actively
		 * disabled it (e.g. bridge remove). Caller is expected to treat
		 * NOT_SUPPORTED as a terminal condition for this slot and stop retrying. */
		DOCA_LOG_DBG("HP interrupt not enabled for DSP[%u]", dsp_index);
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	if (g_tlp_ctx.acg_queue_count == 0) {
		uint64_t starved = atomic_fetch_add_explicit(&g_acg_total_starved, 1, memory_order_relaxed) + 1;
		DOCA_LOG_WARN("ACG starved (fast path) total_starved=%lu", starved);
		return DOCA_ERROR_AGAIN;
	}

	/*
	 * ABP (Attention Button Pressed) mode:
	 * - Plug: ABP + PDC + PDS. Do NOT set DLActive (host sets it on Power ON)
	 * - Unplug: ABP only. Keep PDS=1, let host handle removal via Power OFF
	 * - DLActive transitions are handled in bridge_cap_write when host writes slot_control
	 *
	 * slot_status writer is now single-threaded (TLP): bridge_cap_write W1C
	 * and this RMW both run on the TLP thread. Keeping the __atomic_fetch_or
	 * remains cheap and future-proofs the field against a non-TLP writer.
	 */
	__atomic_fetch_or(&dsp->caps.express.slot_status, SLOT_STS_ATTN_BTN_PRESSED, __ATOMIC_RELEASE);

	if (plug) {
		__atomic_fetch_or(&dsp->caps.express.slot_status,
				  SLOT_STS_PRESENCE_DETECT_CHANGED | SLOT_STS_PRESENCE_DETECT_STATE,
				  __ATOMIC_RELEASE);
		DOCA_LOG_INFO("Hotplug PLUG DSP[%u]: ABP=1 PDC=1 PDS=1 (waiting for host Power ON)", dsp_index);
	} else {
		DOCA_LOG_INFO("Hotplug UNPLUG DSP[%u]: ABP=1 (waiting for host Power OFF)", dsp_index);
	}

	doca_error_t ret = send_msi_via_memory_write_tlp(dsp);
	if (ret == DOCA_SUCCESS)
		DOCA_LOG_INFO("MSI interrupt sent for DSP[%u]", dsp_index);
	else
		DOCA_LOG_ERR("MSI send for DSP[%u]: %s", dsp_index, doca_error_get_descr(ret));
	return ret;
}

/*
 * Retry Command Completed MSIs that were deferred because no ACG credit was
 * available. Host pciehp blocks on Command Completed for every Slot Control
 * write, so a dropped MSI stalls that slot for the full 1s timeout and, under
 * 32-EP stress, cascades into the whole pciehp subsystem wedging. Runs on the
 * TLP thread where ACG credits are replenished, so a freed credit is reused at
 * once. Stops at the first credit-less DSP and resumes on the next poll.
 */
void vblk_pci_retry_cmd_completed_msi(void)
{
	uint32_t i;

	if (!g_tlp_ctx.hotplug_mode)
		return;

	for (i = 0; i < g_tlp_ctx.num_dsp; i++) {
		struct pci_device_config *dsp = &g_tlp_ctx.devs_config[FIRST_DSP_IDX(&g_tlp_ctx) + i];

		if (!__atomic_load_n(&dsp->cmd_completed_msi_pending, __ATOMIC_ACQUIRE))
			continue;
		if (g_tlp_ctx.acg_queue_count == 0)
			break;
		if (send_msi_via_memory_write_tlp(dsp) != DOCA_SUCCESS)
			break;
		__atomic_store_n(&dsp->cmd_completed_msi_pending, false, __ATOMIC_RELEASE);
	}
}

/*
 * Consume plug/unplug MSI requests queued by the OE thread. Runs on the TLP
 * thread so that vblk_pci_trigger_hotplug -> send_msi_via_memory_write_tlp
 * -> complete_acg (and the lib req_pool.release() it triggers) happens on
 * the same thread that owns the doca ctx and drives req_pool.acquire() via
 * doca_pe_progress(). That single-thread invariant is what lets us drop the
 * lib-side req_pool spinlock: doca ctx is thread-unsafe by contract, so all
 * complete_*() paths must originate from the ctx's PE thread.
 *
 * MSI failure handling mirrors vblk_pci_retry_cmd_completed_msi():
 *   - ACG credit exhaustion: stop iterating and retry on the next TLP tick.
 *   - NOT_SUPPORTED (host disabled HP IRQ on this slot, typically bridge
 *     remove): drop the pending flag to avoid a tight retry loop.
 *   - other errors: leave pending flag set, log and let TLP retry next tick.
 * On success we set 'enabled' for plug and clear 'unplug_requested' for
 * unplug, keeping the FSM semantics OE previously enforced synchronously.
 */
void vblk_pci_retry_hotplug_msi(struct vblk_ep_hotplug_state *hp, uint32_t num_ep)
{
	if (!g_tlp_ctx.hotplug_mode || hp == NULL)
		return;

	for (uint32_t ep = 0; ep < num_ep; ep++) {
		if (g_tlp_ctx.acg_queue_count == 0)
			break;

		if (__atomic_load_n(&hp[ep].plug_msi_pending, __ATOMIC_ACQUIRE)) {
			doca_error_t err = vblk_pci_trigger_hotplug(ep, true);

			if (err == DOCA_SUCCESS) {
				__atomic_store_n(&hp[ep].enabled, true, __ATOMIC_RELEASE);
				__atomic_store_n(&hp[ep].plug_msi_pending, false, __ATOMIC_RELEASE);
			} else if (err == DOCA_ERROR_NOT_SUPPORTED) {
				__atomic_store_n(&hp[ep].plug_msi_pending, false, __ATOMIC_RELEASE);
				DOCA_LOG_WARN("TLP: EP %u plug MSI giving up — HP IRQ disabled by host", ep);
			} else if (err == DOCA_ERROR_AGAIN) {
				break;
			}
		}

		if (g_tlp_ctx.acg_queue_count == 0)
			break;

		if (__atomic_load_n(&hp[ep].unplug_msi_pending, __ATOMIC_ACQUIRE)) {
			doca_error_t err = vblk_pci_trigger_hotplug(ep, false);

			if (err == DOCA_SUCCESS) {
				__atomic_store_n(&hp[ep].unplug_msi_pending, false, __ATOMIC_RELEASE);
				__atomic_store_n(&hp[ep].unplug_requested, false, __ATOMIC_RELEASE);
			} else if (err == DOCA_ERROR_NOT_SUPPORTED) {
				__atomic_store_n(&hp[ep].unplug_msi_pending, false, __ATOMIC_RELEASE);
				__atomic_store_n(&hp[ep].unplug_requested, false, __ATOMIC_RELEASE);
				DOCA_LOG_WARN("TLP: EP %u unplug MSI giving up — HP IRQ disabled by host", ep);
			} else if (err == DOCA_ERROR_AGAIN) {
				break;
			}
		}
	}
}

/*
 * VirtIO Device Create / Destroy
 */

struct vblk_pci_virtio_dev *vblk_pci_virtio_dev_create(const struct vblk_pci_virtio_attrs *attr)
{
	uint32_t ei = attr->ep_index;
	struct pci_device_config *ep;
	struct vblk_pci_virtio_dev *dev;
	doca_error_t err;
	bool created_resources = false;

	if (ei >= g_tlp_ctx.num_ep) {
		DOCA_LOG_ERR("'ep_index' %u out of range", ei);
		return NULL;
	}
	if (g_tlp_ctx.virtio_devs[ei]) {
		DOCA_LOG_ERR("virtio_dev already exists for EP[%u]", ei);
		return NULL;
	}
	ep = &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + ei];
	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	dev->ep_index = ei;

	if (ep->rep && ep->tlp_dev) {
		dev->dev_rep = ep->rep;
		dev->pci_tlp_dev = ep->tlp_dev;
	} else {
		err = vblk_pci_create_pf_rep(ei, ep);
		if (err != DOCA_SUCCESS) {
			free(dev);
			return NULL;
		}
		dev->dev_rep = ep->rep;
		err = doca_devemu_pci_tlp_dev_create(g_tlp_ctx.pci_type, dev->dev_rep, &dev->pci_tlp_dev);
		if (err != DOCA_SUCCESS) {
			vblk_pci_destroy_pf_rep(ep);
			free(dev);
			return NULL;
		}
		ep->rep = dev->dev_rep;
		ep->tlp_dev = dev->pci_tlp_dev;
		created_resources = true;
	}
	err = vblk_pci_virtio_dev_init(dev, attr);
	if (err != DOCA_SUCCESS)
		goto fail;

	err = vblk_pci_sriov_init_pf_state(ei, ep, dev);
	if (err != DOCA_SUCCESS)
		goto fail;

	if (!ep->device_present) {
		err = doca_devemu_pci_tlp_dev_start(dev->pci_tlp_dev);
		if (err != DOCA_SUCCESS)
			goto fail;
		ep->device_present = true;
	}
	vblk_pci_sriov_sync_vf_presence(ep, true);
	g_tlp_ctx.virtio_devs[ei] = dev;
	DOCA_LOG_INFO("Created virtio_dev for EP[%u]", ei);
	return dev;
fail:
	vblk_pci_sriov_free_pf_state(ep);
	if (created_resources) {
		doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
		ep->tlp_dev = NULL;
		vblk_pci_destroy_pf_rep(ep);
	}
	free(dev);
	return NULL;
}

void vblk_pci_virtio_dev_destroy(struct vblk_pci_virtio_dev *dev)
{
	uint32_t ei = dev->ep_index;
	struct pci_device_config *ep;
	doca_error_t result;

	if (ei < g_tlp_ctx.num_ep)
		g_tlp_ctx.virtio_devs[ei] = NULL;

	ep = &g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + ei];
	if (dev->is_enumerated && ep->is_bdf_set) {
		bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0, ep->bdf);
		bdf_map_remove(&g_tlp_ctx, TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1, ep->bdf);
		ep->is_bdf_set = false;
	}
	if (dev->msix) {
		result = doca_devemu_pci_msix_destroy(dev->msix);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy MSI-X: %s", doca_error_get_descr(result));
		dev->msix = NULL;
	}
	if (dev->pci_tlp_dev && ep->device_present) {
		vblk_pci_sriov_sync_vf_presence(ep, false);
		result = doca_devemu_pci_tlp_dev_stop(dev->pci_tlp_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
		ep->device_present = false;
	}
	vblk_pci_sriov_free_pf_state(ep);
	free(dev);
}

struct vblk_tlp_context *vblk_pci_get_tlp_ctx(void)
{
	return &g_tlp_ctx;
}
