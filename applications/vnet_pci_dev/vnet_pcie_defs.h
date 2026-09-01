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

#ifndef VNET_PCIE_DEFS_H
#define VNET_PCIE_DEFS_H

#include <stdint.h>
#include <stddef.h> /* For offsetof */

/* PCIe Configuration Space Size (per PCIe Base Specification) */
#define PCIE_CONFIG_SPACE_SIZE 4096

/* PCI Capability IDs */
#define PCI_CAP_ID_EXP 0x10  /* PCIe Express Capability */
#define PCI_CAP_ID_MSIX 0x11 /* MSI-X Capability */
#define PCI_CAP_ID_VNDR 0x09 /* Vendor Specific Capability */

/* PCI Status Register Bits */
#define PCI_STATUS_CAP_LIST 0x0010 /* Capabilities List Present */

/* PCI Class Codes */
#define PCI_CLASS_NETWORK 0x02 /* Network Controller */

/* PCI Base Address Register Bits */
#define PCI_BASE_ADDRESS_MEM_TYPE_64 0x04  /* 64-bit memory BAR */
#define PCI_BASE_ADDRESS_MEM_PREFETCH 0x08 /* Prefetchable memory */

/* PCIe Express Capability — Device/Port Type values (Capability Register bits 7:4) */
#define PCIE_TYPE_ENDPOINT 0x0	      /* PCIe Endpoint */
#define PCIE_TYPE_DOWNSTREAM_PORT 0x6 /* Switch Downstream Port */

/* PCIe Express Capability — FLR-related bits */
#define PCIE_CAP_VERSION_2 0x2		  /* PCIe Capability Version 2 */
#define PCI_EXP_DEVCAP_FLR 0x10000000	  /* Device Capabilities: FLR Capable (bit 28) */
#define PCI_EXP_DEVCTL_BCR_FLR 0x00008000 /* Device Control: Initiate FLR (bit 15) */

/* PCIe Extended Capability IDs (PCIe Base Spec §7.6) */
#define PCIE_EXT_CAP_ID_SRIOV 0x0010 /* Single Root I/O Virtualization */
#define PCIE_EXT_CAP_ID_ARI 0x000E   /* Alternative Routing-ID Interpretation */

/* PCIe Device Capabilities 2 / Device Control 2 — ARI Forwarding (bit 5).
 * A Downstream Port advertises ARI Forwarding Supported in Device Capabilities
 * 2; system software then sets ARI Forwarding Enable in Device Control 2 so the
 * port forwards the full 8-bit Function Number (Device Number forced to 0) to
 * the bus below. Required for a PF to expose more than 7 SR-IOV VFs on one bus. */
#define PCIE_DEVCAP2_ARI_FORWARDING 0x00000020u /* Device Capabilities 2 bit 5 (RO) */
#define PCIE_DEVCTL2_ARI_FORWARDING 0x0020u	/* Device Control 2 bit 5 (RW) */

/* SR-IOV PCIe Extended Capability layout (PCIe Base Spec §9.3.3).
 * The SR-IOV cap is a 64-byte (16 DW) block. vnet places it at byte offset 0x100
 * (the first valid PCIe extended-cap offset), so its DW-indexed register number
 * is 0x100 / 4 = 64. */
#define PCIE_EXT_CAP_OFFSET_SRIOV 0x100 /* byte offset within cfg space */
#define PCIE_EXT_CAP_VERSION_SRIOV 0x1
#define PCIE_EXT_CAP_LEN_SRIOV_DW 16 /* 64 bytes = 16 DW */
#define PCIE_EXT_CAP_REG_NUM_SRIOV (PCIE_EXT_CAP_OFFSET_SRIOV / sizeof(uint32_t))
#define PCIE_EXT_CAP_END_REG_NUM_SRIOV (PCIE_EXT_CAP_REG_NUM_SRIOV + PCIE_EXT_CAP_LEN_SRIOV_DW)

/* ARI PCIe Extended Capability layout (PCIe Base Spec §7.8.8).
 * Two DW: cap header + ARI Capability/Control register. Placed immediately
 * after the SR-IOV cap (0x100 + 16 DW = 0x140) and chained from it, so a PF
 * that advertises SR-IOV ARI hierarchy also exposes the ARI cap the host needs
 * to enable ARI forwarding (Linux pci_configure_ari requires PCI_EXT_CAP_ID_ARI
 * on the function plus ARI Forwarding Supported on the parent Downstream Port). */
#define PCIE_EXT_CAP_OFFSET_ARI 0x140 /* byte offset within cfg space */
#define PCIE_EXT_CAP_VERSION_ARI 0x1
#define PCIE_EXT_CAP_LEN_ARI_DW 2 /* 8 bytes = 2 DW */
#define PCIE_EXT_CAP_REG_NUM_ARI (PCIE_EXT_CAP_OFFSET_ARI / sizeof(uint32_t))
#define PCIE_EXT_CAP_END_REG_NUM_ARI (PCIE_EXT_CAP_REG_NUM_ARI + PCIE_EXT_CAP_LEN_ARI_DW)

/* ACS (Access Control Services) PCIe Extended Capability (PCIe Base Spec §7.7.7).
 * Exposed on the PF (chained after ARI) and the Downstream Port above the VFs so
 * the host places each VF in its own IOMMU group, which VFIO requires to assign
 * a VF to a VM. The Capability/Control register is left 0: a SW bridge has no
 * peer-to-peer path, and Linux treats the absent ACS features as hard-wired
 * isolated. */
#define PCIE_EXT_CAP_ID_ACS 0x000D
#define PCIE_EXT_CAP_VERSION_ACS 0x1
#define PCIE_EXT_CAP_LEN_ACS_DW 2 /* cap header + Capability/Control register */

/* EP/VF: ACS chained after ARI (0x140 + 2 DW = 0x148). */
#define PCIE_EXT_CAP_OFFSET_ACS_EP 0x148
#define PCIE_EXT_CAP_REG_NUM_ACS_EP (PCIE_EXT_CAP_OFFSET_ACS_EP / sizeof(uint32_t))
#define PCIE_EXT_CAP_END_REG_NUM_ACS_EP (PCIE_EXT_CAP_REG_NUM_ACS_EP + PCIE_EXT_CAP_LEN_ACS_DW)

/* Downstream Port: ACS is the first (only) ext cap, at 0x100. */
#define PCIE_EXT_CAP_OFFSET_ACS_DSP 0x100
#define PCIE_EXT_CAP_REG_NUM_ACS_DSP (PCIE_EXT_CAP_OFFSET_ACS_DSP / sizeof(uint32_t))
#define PCIE_EXT_CAP_END_REG_NUM_ACS_DSP (PCIE_EXT_CAP_REG_NUM_ACS_DSP + PCIE_EXT_CAP_LEN_ACS_DW)

/* SR-IOV Capabilities register bits (DW1, PCIe Base Spec §9.3.3.2) */
#define SRIOV_CAP_VF_MIGRATION 0x00000001u	      /* bit 0: VF Migration Capable */
#define SRIOV_CAP_ARI_HIERARCHY_PRESERVED 0x00000002u /* bit 1: ARI Capable Hierarchy Preserved */

/* SR-IOV Control register bits (PCIe Base Spec §9.3.3.3.1) */
#define SRIOV_CTRL_VFE 0x0001		    /* VF Enable */
#define SRIOV_CTRL_VFM 0x0002		    /* VF Migration Enable */
#define SRIOV_CTRL_VF_MIG_INT_ENABLE 0x0004 /* VF Migration Interrupt Enable */
#define SRIOV_CTRL_VF_MSE 0x0008	    /* VF Memory Space Enable */
#define SRIOV_CTRL_ARI_HIERARCHY 0x0010	    /* ARI Capable Hierarchy */

/* Aggregate RW-bit mask for the SR-IOV Control register (lo 16 bits of DW2).
 * All other bits are reserved per spec and must be preserved on host writes. */
#define PCIE_SRIOV_CTRL_RW_MASK \
	(SRIOV_CTRL_VFE | SRIOV_CTRL_VFM | SRIOV_CTRL_VF_MIG_INT_ENABLE | SRIOV_CTRL_VF_MSE | SRIOV_CTRL_ARI_HIERARCHY)

/* W1C-bit mask for the SR-IOV Status register (hi 16 bits of DW2).
 * Only VF Migration Status (bit 0) is implemented (and it is W1C); all
 * other status bits are RO. */
#define PCIE_SRIOV_STATUS_W1C_MASK 0x0001 /* VF Migration Status */

/* Supported Page Sizes (DW7) — encoded per PCIe Base Spec §9.3.3.10:
 *   bit N set => host may use page size 4 KB * 2^N
 * 0x553 = bits {0,1,4,6,8,10} = {4 KB, 8 KB, 64 KB, 256 KB, 1 MB, 4 MB}.
 * Single source of truth used by both sriov_cap_read() (DW7 = RO) and the
 * SystemPageSize (DW8) cap-write validator. */
#define PCIE_SRIOV_SUPPORTED_PAGE_SIZES 0x00000553u

/*
 * SR-IOV cap on-wire register layout — for documentation/reference only.
 *
 * This struct is NOT embedded in pcie_virtio_dev (it would force padding to
 * push the field to byte offset 0x100, which is fragile). Instead the
 * SR-IOV cfg-space reader/writer (added in subsequent commits) synthesizes
 * each DW from struct vnet_sriov_pf_state. This struct exists so reviewers
 * have one place to see the on-wire DW layout, matching PCIe Base Spec
 * §9.3.3.
 */
struct sriov_ext_cap_layout {
	uint32_t cap_header;		    /* DW0:  cap_id[15:0] | version[19:16] | next_cap_offset[31:20] */
	uint32_t sriov_capabilities;	    /* DW1:  VF migration, ARI hierarchy preserved, 10/14-bit tag */
	uint32_t control_status;	    /* DW2:  Control[15:0] | Status[31:16] */
	uint32_t initialvfs_totalvfs;	    /* DW3:  InitialVFs[15:0] | TotalVFs[31:16] */
	uint32_t numvfs_funclink;	    /* DW4:  NumVFs[15:0] | FunctionDepLink[23:16] */
	uint32_t firstvf_offset_stride;	    /* DW5:  FirstVFOffset[15:0] | VFStride[31:16] */
	uint32_t vf_device_id;		    /* DW6:  reserved | VFDeviceID[31:16] */
	uint32_t supported_pg_sizes;	    /* DW7:  Supported Page Sizes */
	uint32_t system_pg_size;	    /* DW8:  System Page Size (RW) */
	uint32_t vf_bar[6];		    /* DW9..14: VF BARs */
	uint32_t vf_migration_state_offset; /* DW15: VF Migration State Array Offset */
} __attribute__((packed));

/* PCI Configuration Space Header (Type 0) */
struct pci_config_header_type0 {
	uint16_t vendor_id;	      /* 0x00 */
	uint16_t device_id;	      /* 0x02 */
	uint16_t command;	      /* 0x04 */
	uint16_t status;	      /* 0x06 */
	uint8_t revision_id;	      /* 0x08 */
	uint8_t prog_if;	      /* 0x09 */
	uint8_t subclass;	      /* 0x0A */
	uint8_t class_code;	      /* 0x0B */
	uint8_t cache_line_size;      /* 0x0C */
	uint8_t latency_timer;	      /* 0x0D */
	uint8_t header_type;	      /* 0x0E */
	uint8_t bist;		      /* 0x0F */
	uint32_t base_address[6];     /* 0x10-0x27 */
	uint32_t cardbus_cis;	      /* 0x28 */
	uint16_t subsystem_vendor_id; /* 0x2C */
	uint16_t subsystem_id;	      /* 0x2E */
	uint32_t rom_address;	      /* 0x30 */
	uint8_t capabilities_ptr;     /* 0x34 */
	uint8_t reserved1[3];	      /* 0x35-0x37 */
	uint32_t reserved2;	      /* 0x38 */
	uint8_t interrupt_line;	      /* 0x3C */
	uint8_t interrupt_pin;	      /* 0x3D */
	uint8_t min_gnt;	      /* 0x3E */
	uint8_t max_lat;	      /* 0x3F */
} __attribute__((packed));

/* PCI Capability Header */
struct pci_capability_header {
	uint8_t cap_id;	  /* Capability ID */
	uint8_t next_ptr; /* Next capability pointer */
} __attribute__((packed));

/* PCIe Express Capability */
struct pcie_capability {
	struct pci_capability_header cap;
	uint16_t pcie_cap;    /* PCIe Capability Register */
	uint32_t dev_cap;     /* Device Capabilities */
	uint16_t dev_ctrl;    /* Device Control */
	uint16_t dev_status;  /* Device Status */
	uint32_t link_cap;    /* Link Capabilities */
	uint16_t link_ctrl;   /* Link Control */
	uint16_t link_status; /* Link Status */
	uint32_t slot_caps;   /* Slot Capabilities */
	uint16_t slot_ctrl;   /* Slot Control */
	uint16_t slot_status; /* Slot Status */
	uint16_t root_ctrl;   /* Root Control */
	uint16_t root_caps;   /* Root Capabilities */
	uint32_t root_status; /* Root Status */
} __attribute__((packed));

/* MSI-X Capability */
struct msix_capability {
	struct pci_capability_header cap;
	uint16_t msgctl;       /* Message Control */
	uint32_t table_offset; /* BAR indicator + offset */
	uint32_t pba_offset;   /* BAR indicator + offset */
} __attribute__((packed));

/* BAR mapping information */
struct pcie_bar64_map {
	uint8_t log_size;
};

/* Raw PCIe Configuration Space */
struct pcie_raw_cfg {
	uint32_t dw_regs[PCIE_CONFIG_SPACE_SIZE / sizeof(uint32_t)];
	struct pcie_bar64_map bar64_map[3];
} __attribute__((packed));

/* Enhanced type-safe macros for VirtIO device configuration access */
#define VIRTIO_DEV_CONFIG_READ(virtio_dev, pos) ((virtio_dev)->pcie_dev.dw_regs[(pos)])

#define VIRTIO_DEV_CONFIG_WRITE(virtio_dev, pos, val) ((virtio_dev)->pcie_dev.dw_regs[(pos)] = (val))

#endif /* VNET_PCIE_DEFS_H */
