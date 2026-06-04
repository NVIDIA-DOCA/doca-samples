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
#include <string.h>
#include <stdlib.h>
#include <endian.h>

#include <linux/virtio_pci.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_devemu_vblk_type.h>
#include <doca_devemu_pci_info.h>

#include "vblk_pci.h"
#include "pci_spec_tlp.h"

DOCA_LOG_REGISTER(VBLK_PCI);

static struct vblk_tlp_context g_tlp_ctx;
static struct pcie_virtio_dev virtio_dev;

/* DW index of express slot_control+slot_status in PCI config space */
#define EXPRESS_SLOT_CTRL_STS_DW ((TLP_PCI_CAP_OFFSET_EXPRESS + 24) / 4)

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

static bool is_dsp(struct pci_device_config *dc)
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
		/* slot_control(RW) + slot_status(W1C) at Express DW offset 6 */
		if (idx == 6) {
			bool ctrl_changed = false;

			/* Lower 16 bits: slot_control (RW) */
			if (mask & 0xFFFF) {
				uint16_t old_ctrl = dc->caps.express.slot_control;
				uint16_t new_ctrl = (old_ctrl & ~(mask & 0xFFFF)) | (data & mask & 0xFFFF);

				dc->caps.express.slot_control = new_ctrl;

				if (dc->is_bridge) {
					bool pds = (dc->caps.express.slot_status & SLOT_STS_PRESENCE_DETECT_STATE) != 0;
					bool old_pwr_off = (old_ctrl & SLOT_CTRL_POWER_CONTROLLER) != 0;
					bool new_pwr_off = (new_ctrl & SLOT_CTRL_POWER_CONTROLLER) != 0;

					if (old_pwr_off && !new_pwr_off && pds) {
						dc->caps.express.link_status |= LINK_STS_DL_ACTIVE;
						DOCA_LOG_INFO("Power ON: DLActive set");
					} else if (!old_pwr_off && new_pwr_off) {
						dc->caps.express.link_status &= ~LINK_STS_DL_ACTIVE;
						DOCA_LOG_INFO("Power OFF: DLActive cleared");
						if (is_dsp(dc)) {
							uint32_t di =
								(uint32_t)(dc - &g_tlp_ctx.devs_config[FIRST_DSP_IDX(
											&g_tlp_ctx)]);

							if (di < g_tlp_ctx.num_ep)
								g_tlp_ctx.devs_config[FIRST_PF_IDX(&g_tlp_ctx) + di]
									.host_power_off = true;
						}
					}
					ctrl_changed = (new_ctrl != old_ctrl);
				}
			}

			/* Upper 16 bits: slot_status (W1C) */
			if (mask & 0xFFFF0000) {
				uint16_t w1c = (data >> 16) & (mask >> 16);
				dc->caps.express.slot_status &= ~w1c;
			}

			return ctrl_changed;
		}
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
	return (reg < 16) ? bridge_hdr_read(dc, reg) : bridge_cap_read(dc, reg);
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
	dc->caps.express.pcie_cap_register = 0x0052;
	dc->caps.express.dev_capabilities = 0x00008001;
	dc->caps.express.link_capabilities = 0x00500104;
	dc->caps.express.link_status = 0x1104;

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
	dc->caps.express.pcie_cap_register = 0x0162;
	dc->caps.express.dev_capabilities = 0x00008001;
	dc->caps.express.link_capabilities = 0x01500104;
	dc->caps.express.link_status = 0x1104;
	dc->caps.express.slot_capabilities = (SLOT_CAP_PWR_CTRL_PRESENT | SLOT_CAP_HP_SURPRISE | SLOT_CAP_HP_CAPABLE |
					      (TLP_BRIDGE_SLOT_PWR_LIMIT << SLOT_CAP_PWR_LIMIT_VALUE_SHIFT) |
					      (slot_number << SLOT_CAP_PHYS_SLOT_NUM_SHIFT));
	if (hp) {
		dc->caps.express.slot_control = SLOT_CTRL_POWER_CONTROLLER;
		dc->caps.express.slot_status = 0x0000;
	} else {
		dc->caps.express.slot_control = 0;
		dc->caps.express.slot_status = SLOT_STS_PRESENCE_DETECT_STATE;
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
	uint32_t i;

	ctx->num_ep = num_ep;
	ctx->num_dsp = num_ep;
	ctx->num_bridges = MAX_NUM_USP + num_ep;
	ctx->num_devices = ctx->num_bridges + num_ep + DUMMY_DEV_NUM;
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
	}
	/* Dummy device */
	struct pci_device_config *d = &ctx->devs_config[DUMMY_DEV_IDX(ctx)];

	memset(d, 0, sizeof(*d));
	d->is_dummy = true;
	d->cfg_space_hdr.type0.vendor_id = 0xFFFF;
	d->cfg_space_hdr.type0.device_id = 0xFFFF;

	DOCA_LOG_INFO("PCI Switch: %u USP + %u DSP + %u EP + 1 dummy = %u",
		      MAX_NUM_USP,
		      num_ep,
		      num_ep,
		      ctx->num_devices);
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
	/* Skip pcie cap which is currently not fully supported */
	regs->capabilities_ptr = offsetof(struct pcie_virtio_dev, cfg.msix_cap);
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

void vblk_pci_virtio_dev_reset(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;

	dev->driver_features = pci_cfg->driver_feature = 0;
	pci_cfg->driver_feature_select = pci_cfg->device_feature_select = 0;
	/* Keep device_feature aligned with device_feature_select (reset sets select=0 => low 32b features) */
	pci_cfg->device_feature = dev->device_features & 0xFFFFFFFF;

	vblk_pci_virtio_dev_queue_init(dev);
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

	dev->pci_cfg_change_cb = attr->pci_cfg_change_cb;
	dev->tlp_poll_cb = attr->tlp_poll_cb;
	dev->cb_arg = attr->cb_arg;
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

static void vblk_pci_virtio_pci_cfg_write32(struct vblk_pci_virtio_dev *dev,
					    const uint64_t offset,
					    const uint32_t val,
					    const uint32_t wr_mask)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	struct vblk_pci_virtio_pci_common_cfg prev_cfg;
	uint32_t *base = (uint32_t *)pci_cfg;

	/* save everything to handle ro fields and selectors */
	memcpy(&prev_cfg, pci_cfg, sizeof(prev_cfg));

	const uint32_t dw_offset = offset / 4;
	const uint32_t orig_val = base[dw_offset];

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

	/* Reset PCI config before notifying the application layer, so
	 * callback sees clean MSI-X/feature/queue state. Check original
	 * write value (not post-callback) since callback may modify status. */
	if (prev_cfg.device_status != 0 && pci_cfg->device_status == 0) {
		DOCA_LOG_INFO("device status: %d -> %d", prev_cfg.device_status, pci_cfg->device_status);
		vblk_pci_virtio_dev_reset(dev);
	} else if (prev_cfg.device_status != pci_cfg->device_status) {
		DOCA_LOG_INFO("device status: %d -> %d", prev_cfg.device_status, pci_cfg->device_status);
	}

	if (dev->pci_cfg_change_cb)
		dev->pci_cfg_change_cb(dev, dev->cb_arg);

	return;

rollback:
	memcpy(pci_cfg, &prev_cfg, sizeof(prev_cfg));
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

static void vblk_pci_virtio_mmio_write32(struct vblk_pci_virtio_dev *dev,
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
		vblk_pci_virtio_pci_cfg_write32(dev, offset, val, wr_mask);
	} else if (offset >= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->dev_cfg)) {
		offset -= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
		vblk_pci_virtio_dev_cfg_write32(dev, offset, val, wr_mask);
	} else {
		DOCA_LOG_ERR("mmio write drop - unsupported virtio bar offset 0x%lx", offset);
		return;
	}

	/* consider: raise bar change event so that app can address it */
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
 * Topology-aware device lookup.
 *
 * Type0 CFG → USP (device=0, function=0 on physical bus)
 * Type1 CFG → DSP bridges (on USP's secondary_bus, device=0..N-1)
 *           → Endpoints (on each DSP's secondary_bus, device=0 function=0)
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

		/* Endpoints sit on each DSP's secondary bus (device=0, fn=0) */
		for (i = 0; i < ctx->num_ep; i++) {
			struct pci_device_config *ep = &ctx->devs_config[FIRST_PF_IDX(ctx) + i];
			uint8_t dsp_sec = ctx->devs_config[FIRST_DSP_IDX(ctx) + i].cfg_space_hdr.type1.secondary_bus;

			if (dsp_sec != 0 && bus == dsp_sec && device == 0 && function == 0 && ep->device_present) {
				bdf_map_update(ctx, tlp_type, bdf, ep);
				return ep;
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
		dc->bus = bus;
		dc->device = device;
		dc->function = function;
		dc->bdf = bdf;
		dc->completer_id = bdf;
		dc->is_bdf_set = true;
		DOCA_LOG_INFO("Device BDF set: %02x:%02x.%x (%s%s)",
			      bus,
			      device,
			      function,
			      dc->is_bridge ? "bridge" : "",
			      dc->is_endpoint ? "endpoint" : "");
		/* Sync virtio_dev BDF for endpoints */
		if (dc->is_endpoint) {
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
		 * 10 - PCI_COMMAND_INT_DISABLE
		 * For Functions with a Type 0 Configuration Space Header that do not generate INTx interrupts,
		 * this bit is optional. If not implemented, this bit must be hardwired to 0b
		 *
		 * 11 - 15 - PCI_COMMAND_RES_BIT_MASK
		 */
		const uint16_t command_ro_mask = PCI_COMMAND_IO | PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
						 PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_PARITY | PCI_COMMAND_WAIT |
						 PCI_COMMAND_SERR | PCI_COMMAND_FAST_BACK | PCI_COMMAND_INT_DISABLE |
						 PCI_COMMAND_RES_BIT_MASK;

		/* Status reg RO and bits that can be assumed RO:
		 * 7.5.1.1.4 Status Register (Offset 06h)
		 * 0 - PCI_STATUS_READY
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

	/* everything else is writable */
	return 0xFFFFFFFF;
}

/*
 * Type0 CFG Handlers (endpoints)
 */

static doca_error_t send_msi_via_memory_write_tlp(struct pci_device_config *dsp);

/*
 * Unified config read handler for both Type0 and Type1.
 * Type0 → USP (bridge)
 * Type1 → DSP bridges or endpoints (via topology routing)
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
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	if (dc->is_bridge) {
		data = bridge_cfg_read32(dc, reg) & vblk_pci_bit_enable_to_mask(be);
		cd = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
		memcpy(cd, &data, 4);
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	/* Endpoint config read */
	struct vblk_pci_virtio_dev *virtio_dev = virtio_dev_for_ep(dc);

	if (!virtio_dev) {
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}
	data = pcie_config_read(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), reg) & vblk_pci_bit_enable_to_mask(be);
	cd = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(cd, &data, 4);
	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
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

	dc = find_target_cfg_device(tlp_req_header, tlp_type);

	if (dc->is_dummy) {
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	m = vblk_pci_bit_enable_to_mask(be);

	if (dc->is_bridge) {
		bool slot_ctrl_changed = false;

		if (reg < 16)
			bridge_hdr_write(dc, reg, td[0], m, &g_tlp_ctx);
		else
			slot_ctrl_changed = bridge_cap_write(dc, reg, td[0], m);

		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);

		/* Trigger Command Completed only when slot_control actually changed on a DSP */
		if (slot_ctrl_changed && is_dsp(dc)) {
			dc->caps.express.slot_status |= SLOT_STS_CMD_COMPLETED;
			DOCA_LOG_DBG("Command Completed: ctrl=0x%04x sts=0x%04x",
				     dc->caps.express.slot_control,
				     dc->caps.express.slot_status);
			if (dc->caps.express.slot_control & SLOT_CTRL_CMD_COMPLETED_INT_EN)
				send_msi_via_memory_write_tlp(dc);
		}
		return;
	}

	/* Endpoint config write */
	struct vblk_pci_virtio_dev *virtio_dev = virtio_dev_for_ep(dc);

	if (!virtio_dev) {
		vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}
	m &= vblk_pci_ext_reg_wr_mask(virtio_dev, reg);
	uint32_t rd = pcie_config_read(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), reg);
	pcie_config_write(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), reg, (rd & ~m) | (td[0] & m));
	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dc->tlp_dev);
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
	if (!virtio_dev) {
		DOCA_LOG_ERR("fatal invalid mmio address 0x%lx", addr);
		/* Memory TLP DW2 contains address, not BDF — cannot use vblk_pci_tlp_cfg_cpl_prep.
		 * Use completer_id=0 for unmapped address UR. */
		uint32_t *cpl = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

		memset(cpl, 0, 3 * sizeof(uint32_t));
		vblk_pci_tlp_cpl_prep_dw0(tlp_req, TLP_FMT_3DW_NO_DATA, 0);
		vblk_pci_tlp_cpl_prep_dw1_2(tlp_req, 0, TLP_CPL_STATUS_UR, 4);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	/* at the moment we only support 1 & 2 dwords reads */
	if (length > 2) {
		DOCA_LOG_ERR("unsupported read length %d", length);
		goto fatal_error;
	}

	if (length == 0) {
		DOCA_LOG_DBG("zero read %d", length);
		goto zero_read;
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
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

fatal_error:
	vblk_pci_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0, 4);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

zero_read:
	vblk_pci_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0, 0);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
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

	if (fmt == TLP_FMT_4DW_WITH_DATA)
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
	else
		addr = be32toh(dw_header[2]) & ~0x3;

	DOCA_LOG_TRC("MMIO write 0x%lx len %d data1 0x%0x", addr, length, length > 0 ? tlp_req_data[0] : 0);

	/* requests are routed by the mmio address */
	virtio_dev = vblk_pci_dev_find_by_addr(addr);
	if (!virtio_dev) {
		DOCA_LOG_ERR("fatal invalid mmio address 0x%lx", addr);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, NULL);
		return;
	}

	/* at the moment we only support 1 & 2 dwords writes */
	if (length > 2) {
		DOCA_LOG_ERR("unsupported write length %d", length);
		goto done;
	}

	if (length == 0) {
		DOCA_LOG_ERR("zero read %d", length);
		goto done;
	}

	uint32_t dw_mask;
	uint32_t bit_en_mask;

	bit_en_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
	vblk_pci_virtio_mmio_write32(virtio_dev, addr, tlp_req_data[0], dw_mask);

	if (length == 2) {
		bit_en_mask = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
		dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
		vblk_pci_virtio_mmio_write32(virtio_dev, addr + 4, tlp_req_data[1], dw_mask);
	}

done:
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, virtio_dev->pci_tlp_dev);
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
		DOCA_LOG_INFO("PERST# is asserted (enters reset)");
		break;
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_DEASSERT:
		DOCA_LOG_INFO("PERST# is deasserted (released from reset)");
		break;
	default:
		DOCA_LOG_WARN("Unknown PCI_EVENT opmode: %d", pci_event_opmode);
		break;
	}

	/* Complete the PCI_EVENT request */
	doca_devemu_pci_tlp_channel_req_complete_pci_event(tlp_req);
}

static void vblk_pci_ev_cb(struct doca_devemu_pci_tlp_channel *channel,
			   struct doca_devemu_pci_tlp_channel_req *tlp_req,
			   void *req_user_data)
{
	enum doca_devemu_pci_tlp_channel_req_opcode opcode = doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req);
	enum tlp_req_type type;

	(void)channel;
	(void)req_user_data;

	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_ACG) {
		if (g_tlp_ctx.acg_queue == NULL || g_tlp_ctx.acg_queue_count >= g_tlp_ctx.acg_queue_size) {
			doca_devemu_pci_tlp_channel_req_complete_acg(
				tlp_req,
				0,
				DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		} else {
			g_tlp_ctx.acg_queue[g_tlp_ctx.acg_queue_tail] = tlp_req;
			g_tlp_ctx.acg_queue_tail = (g_tlp_ctx.acg_queue_tail + 1) % g_tlp_ctx.acg_queue_size;
			g_tlp_ctx.acg_queue_count++;
			DOCA_LOG_DBG("ACG credit queued (%u/%u)", g_tlp_ctx.acg_queue_count, g_tlp_ctx.acg_queue_size);
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
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, NULL);
	}
}

static doca_error_t init_acg_queue(void)
{
	doca_error_t err;

	err = doca_devemu_pci_tlp_cap_get_max_acg(doca_dev_as_devinfo(g_tlp_ctx.dev), &g_tlp_ctx.acg_queue_size);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query max ACG: %s", doca_error_get_descr(err));
		return err;
	}
	if (g_tlp_ctx.acg_queue_size == 0) {
		DOCA_LOG_WARN("Device reports 0 ACG credits - MSI disabled");
		return DOCA_SUCCESS;
	}
	g_tlp_ctx.acg_queue = calloc(g_tlp_ctx.acg_queue_size, sizeof(struct doca_devemu_pci_tlp_channel_req *));
	if (g_tlp_ctx.acg_queue == NULL)
		return DOCA_ERROR_NO_MEMORY;
	DOCA_LOG_INFO("ACG queue initialized: capacity=%u", g_tlp_ctx.acg_queue_size);
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
	/* Flush pending ACG credits before stopping TLP channel */
	while (g_tlp_ctx.acg_queue_count > 0) {
		struct doca_devemu_pci_tlp_channel_req *acg = g_tlp_ctx.acg_queue[g_tlp_ctx.acg_queue_head];

		g_tlp_ctx.acg_queue_head = (g_tlp_ctx.acg_queue_head + 1) % g_tlp_ctx.acg_queue_size;
		g_tlp_ctx.acg_queue_count--;
		doca_devemu_pci_tlp_channel_req_complete_acg(acg,
							     0,
							     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
	}

	doca_error_t err;

	err = doca_ctx_stop(vblk_pci_ev_channel_ctx());
	/* consider: handle in progress either sync or async */
	if (err != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop vblk_pci event channel");
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
				result = doca_devemu_pci_tlp_dev_stop(ep->tlp_dev);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
				ep->device_present = false;
			}
			if (ep->tlp_dev) {
				result = doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to destroy TLP device: %s", doca_error_get_descr(result));
				ep->tlp_dev = NULL;
			}
			if (ep->rep) {
				result = doca_devemu_pci_type_destroy_rep(ep->rep);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to destroy PCI type rep: %s",
						     doca_error_get_descr(result));
				ep->rep = NULL;
			}
		}
	}
	vblk_pci_virtio_reset();
	if (g_tlp_ctx.tlp_channel) {
		doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
		g_tlp_ctx.tlp_channel = NULL;
	}
	free(g_tlp_ctx.acg_queue);
	free(g_tlp_ctx.virtio_devs);
	free(g_tlp_ctx.bdf_entries);
	free(g_tlp_ctx.devs_config);
	memset(&g_tlp_ctx, 0, sizeof(g_tlp_ctx));
}

doca_error_t vblk_pci_init(struct doca_dev *dev, uint32_t num_ep, bool hotplug_mode)
{
	doca_error_t err;
	uint16_t n;

	memset(&g_tlp_ctx, 0, sizeof(g_tlp_ctx));
	g_tlp_ctx.dev = dev;
	g_tlp_ctx.unplug_active = -1;

	err = doca_devemu_pci_tlp_cap_get_max_types(doca_dev_as_devinfo(dev), &n);
	if (err != DOCA_SUCCESS || n == 0) {
		DOCA_LOG_ERR("No TLP emulation support");
		return err != DOCA_SUCCESS ? err : DOCA_ERROR_NOT_SUPPORTED;
	}
	DOCA_LOG_INFO("device supports %d pci tlp types", n);

	err = vblk_pci_switch_init(&g_tlp_ctx, num_ep, hotplug_mode);
	if (err != DOCA_SUCCESS)
		return err;

	err = vblk_pci_ev_channel_init(dev);
	if (err != DOCA_SUCCESS)
		goto fail;

	if (hotplug_mode) {
		err = init_acg_queue();
		if (err != DOCA_SUCCESS) {
			doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
			goto fail;
		}
	}

	err = vblk_pci_virtio_block_init(dev);
	if (err != DOCA_SUCCESS) {
		free(g_tlp_ctx.acg_queue);
		doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx.tlp_channel);
		goto fail;
	}
	DOCA_LOG_INFO("vblk_pci initialized: num_ep=%u hotplug=%d", num_ep, hotplug_mode);
	return DOCA_SUCCESS;
fail:
	free(g_tlp_ctx.virtio_devs);
	free(g_tlp_ctx.bdf_entries);
	free(g_tlp_ctx.devs_config);
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
	err = doca_devemu_pci_type_create_rep(g_tlp_ctx.pci_type, &ep->rep);
	if (err != DOCA_SUCCESS)
		return err;
	err = doca_devemu_pci_tlp_dev_create(g_tlp_ctx.pci_type, ep->rep, &ep->tlp_dev);
	if (err != DOCA_SUCCESS) {
		doca_devemu_pci_type_destroy_rep(ep->rep);
		ep->rep = NULL;
		return err;
	}
	err = doca_devemu_pci_tlp_dev_start(ep->tlp_dev);
	if (err != DOCA_SUCCESS) {
		doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
		ep->tlp_dev = NULL;
		doca_devemu_pci_type_destroy_rep(ep->rep);
		ep->rep = NULL;
		return err;
	}
	ep->device_present = true;
	DOCA_LOG_INFO("Created device for endpoint %u", ep_index);
	return DOCA_SUCCESS;
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
			result = doca_devemu_pci_tlp_dev_stop(ep->tlp_dev);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
		}
		ep->device_present = false;
		result = doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy TLP device: %s", doca_error_get_descr(result));
		ep->tlp_dev = NULL;
	} else {
		ep->device_present = false;
	}
	if (ep->rep) {
		result = doca_devemu_pci_type_destroy_rep(ep->rep);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy PCI type rep: %s", doca_error_get_descr(result));
		ep->rep = NULL;
	}
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

	if (g_tlp_ctx.acg_queue == NULL || g_tlp_ctx.acg_queue_count == 0) {
		DOCA_LOG_WARN("No ACG credit available for MSI, host will poll");
		return DOCA_ERROR_AGAIN;
	}

	struct doca_devemu_pci_tlp_channel_req *acg = g_tlp_ctx.acg_queue[g_tlp_ctx.acg_queue_head];
	g_tlp_ctx.acg_queue_head = (g_tlp_ctx.acg_queue_head + 1) % g_tlp_ctx.acg_queue_size;
	g_tlp_ctx.acg_queue_count--;

	void *acg_buf = doca_devemu_pci_tlp_channel_req_get_acg_buf(acg);
	if (acg_buf == NULL) {
		DOCA_LOG_ERR("Failed to get ACG buffer");
		doca_devemu_pci_tlp_channel_req_complete_acg(acg,
							     0,
							     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		return DOCA_ERROR_DRIVER;
	}

	size_t tlp_size = set_memory_write_tlp_header(acg_buf, msi_addr, msi_data, dsp_bdf);

	doca_devemu_pci_tlp_channel_req_complete_acg(acg,
						     tlp_size,
						     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_MMIO_WRITE);
	DOCA_LOG_INFO("MSI sent: DSP %04x addr=0x%lx data=0x%x", dsp_bdf, msi_addr, msi_data);
	return DOCA_SUCCESS;
}

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
		DOCA_LOG_ERR("HP interrupt not enabled for DSP[%u]", dsp_index);
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	if (g_tlp_ctx.acg_queue_count == 0) {
		DOCA_LOG_DBG("No ACG credit available for MSI");
		return DOCA_ERROR_AGAIN;
	}

	/*
	 * ABP (Attention Button Pressed) mode:
	 * - Plug: ABP + PDC + PDS. Do NOT set DLActive (host sets it on Power ON)
	 * - Unplug: ABP only. Keep PDS=1, let host handle removal via Power OFF
	 * - DLActive transitions are handled in bridge_cap_write when host writes slot_control
	 */
	dsp->caps.express.slot_status |= SLOT_STS_ATTN_BTN_PRESSED;

	if (plug) {
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_CHANGED | SLOT_STS_PRESENCE_DETECT_STATE;
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
		err = doca_devemu_pci_type_create_rep(g_tlp_ctx.pci_type, &dev->dev_rep);
		if (err != DOCA_SUCCESS) {
			free(dev);
			return NULL;
		}
		err = doca_devemu_pci_tlp_dev_create(g_tlp_ctx.pci_type, dev->dev_rep, &dev->pci_tlp_dev);
		if (err != DOCA_SUCCESS) {
			doca_devemu_pci_type_destroy_rep(dev->dev_rep);
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

	if (!ep->device_present) {
		err = doca_devemu_pci_tlp_dev_start(dev->pci_tlp_dev);
		if (err != DOCA_SUCCESS)
			goto fail;
		ep->device_present = true;
	}
	g_tlp_ctx.virtio_devs[ei] = dev;
	DOCA_LOG_INFO("Created virtio_dev for EP[%u]", ei);
	return dev;
fail:
	if (created_resources) {
		doca_devemu_pci_tlp_dev_destroy(ep->tlp_dev);
		ep->tlp_dev = NULL;
		doca_devemu_pci_type_destroy_rep(ep->rep);
		ep->rep = NULL;
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
		result = doca_devemu_pci_tlp_dev_stop(dev->pci_tlp_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
		ep->device_present = false;
	}
	free(dev);
}

struct vblk_tlp_context *vblk_pci_get_tlp_ctx(void)
{
	return &g_tlp_ctx;
}
