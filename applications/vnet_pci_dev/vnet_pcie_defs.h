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
