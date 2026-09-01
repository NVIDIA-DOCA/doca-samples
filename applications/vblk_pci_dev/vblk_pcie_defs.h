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

#ifndef VBLK_PCIE_DEFS_H_
#define VBLK_PCIE_DEFS_H_

#include <stddef.h>
#include <stdint.h>

#include <linux/pci_regs.h>
#include <linux/virtio_pci.h>

// PCI Command Register Bits
#define PCI_COMMAND_RES_BIT_MASK ((1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15))

#define PCIE_CAP_VERSION_2 0x2 // PCI Express Capability Version 2

/* Device Capabilities Max_Payload_Size_Supported field ([2:0]); value 1 encodes
 * 256 bytes (MPS = 128 << MPSS). Matches the host PCIe fabric MPS so the host's
 * hotplug MPS tuning does not warn "can't set Max Payload Size to 256". */
#define PCIE_DEVCAP_MPSS_256B 0x1

// PCI Device Classes
#define PCI_CLASS_NOT_DEFINED 0x00
#define PCI_CLASS_STORAGE 0x01
#define PCI_CLASS_NETWORK 0x02
#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY 0x05
#define PCI_CLASS_BRIDGE 0x06
#define PCI_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_SYSTEM 0x08
#define PCI_CLASS_INPUT 0x09
#define PCI_CLASS_DOCKING 0x0A
#define PCI_CLASS_PROCESSOR 0x0B
#define PCI_CLASS_SERIAL 0x0C
#define PCI_CLASS_WIRELESS 0x0D
#define PCI_CLASS_INTELLIGENT 0x0E
#define PCI_CLASS_SATELLITE 0x0F
#define PCI_CLASS_CRYPT 0x10
#define PCI_CLASS_SIGNAL_PROCESSING 0x11
#define PCI_CLASS_OTHERS 0xFF

// Storage device subclasses
#define PCI_SUBCLASS_STORAGE_SCSI 0x00
#define PCI_SUBCLASS_STORAGE_IDE 0x01
#define PCI_SUBCLASS_STORAGE_FLOPPY 0x02
#define PCI_SUBCLASS_STORAGE_IPI 0x03
#define PCI_SUBCLASS_STORAGE_RAID 0x04
#define PCI_SUBCLASS_STORAGE_ATA 0x05
#define PCI_SUBCLASS_STORAGE_SATA 0x06
#define PCI_SUBCLASS_STORAGE_SAS 0x07
#define PCI_SUBCLASS_STORAGE_NVM 0x08
#define PCI_SUBCLASS_STORAGE_OTHER 0x80

// PCI Bridge Device Classes
#define PCI_CLASS_BRIDGE_HOST 0x00
#define PCI_CLASS_BRIDGE_ISA 0x01
#define PCI_CLASS_BRIDGE_EISA 0x02
#define PCI_CLASS_BRIDGE_MC 0x03
#define PCI_CLASS_BRIDGE_PCI 0x04
#define PCI_CLASS_BRIDGE_PCMCIA 0x05
#define PCI_CLASS_BRIDGE_NUBUS 0x06
#define PCI_CLASS_BRIDGE_CARDBUS 0x07
#define PCI_CLASS_BRIDGE_RACEWAY 0x08
#define PCI_CLASS_BRIDGE_OTHER 0x80

// Standard PCI Configuration Space Header (Type 0)
struct pci_config_header_type0 {
	uint16_t vendor_id; // 0x00 - reg0
	uint16_t device_id; // 0x02

	uint16_t command; // 0x04 - reg1
	uint16_t status;  // 0x06

	uint8_t revision_id; // 0x08 - reg2 (ro)
	uint8_t prog_if;     // 0x09
	uint8_t subclass;    // 0x0A
	uint8_t class_code;  // 0x0B

	uint8_t cache_line_size; // 0x0C - reg3
	uint8_t latency_timer;	 // 0x0D
	uint8_t header_type;	 // 0x0E
	uint8_t bist;		 // 0x0F

	union {
		uint32_t base_address[6]; // 0x10-0x27 reg: 4-9
		uint64_t base_address64[3];
	};

	uint32_t cardbus_cis; // 0x28 - reg10

	uint16_t subsystem_vendor_id; // 0x2C - reg11
	uint16_t subsystem_id;	      // 0x2E

	uint32_t rom_address; // 0x30 - reg12

	uint8_t capabilities_ptr; // 0x34 - reg13
	uint8_t reserved1[3];	  // 0x35-0x37

	uint32_t reserved2; // 0x38 - reg14

	uint8_t interrupt_line; // 0x3C - reg15
	uint8_t interrupt_pin;	// 0x3D
	uint8_t min_gnt;	// 0x3E
	uint8_t max_lat;	// 0x3F
} __attribute__((packed));

// PCI-to-PCI Bridge Configuration Space Header (Type 1)
struct pci_config_header_type1 {
	uint16_t vendor_id;	       // 0x00
	uint16_t device_id;	       // 0x02
	uint16_t command;	       // 0x04
	uint16_t status;	       // 0x06
	uint8_t revision_id;	       // 0x08
	uint8_t prog_if;	       // 0x09
	uint8_t subclass;	       // 0x0A
	uint8_t class_code;	       // 0x0B
	uint8_t cache_line_size;       // 0x0C
	uint8_t latency_timer;	       // 0x0D
	uint8_t header_type;	       // 0x0E
	uint8_t bist;		       // 0x0F
	uint32_t base_address[2];      // 0x10-0x17
	uint8_t primary_bus;	       // 0x18
	uint8_t secondary_bus;	       // 0x19
	uint8_t subordinate_bus;       // 0x1A
	uint8_t secondary_latency;     // 0x1B
	uint8_t io_base;	       // 0x1C
	uint8_t io_limit;	       // 0x1D
	uint16_t secondary_status;     // 0x1E
	uint16_t memory_base;	       // 0x20
	uint16_t memory_limit;	       // 0x22
	uint16_t prefetch_base;	       // 0x24
	uint16_t prefetch_limit;       // 0x26
	uint32_t prefetch_base_upper;  // 0x28
	uint32_t prefetch_limit_upper; // 0x2C
	uint16_t io_base_upper;	       // 0x30
	uint16_t io_limit_upper;       // 0x32
	uint8_t capabilities_ptr;      // 0x34
	uint8_t reserved1[3];	       // 0x35-0x37
	uint32_t rom_address;	       // 0x38
	uint8_t interrupt_line;	       // 0x3C
	uint8_t interrupt_pin;	       // 0x3D
	uint16_t bridge_control;       // 0x3E
} __attribute__((packed));

// PCI Capability Header
struct pci_capability_header {
	uint8_t cap_id;	  // Capability ID
	uint8_t next_ptr; // Next capability pointer
} __attribute__((packed));

// PCIe Extended Capability Header
struct pcie_ext_capability_header {
	uint16_t cap_id;	  // Extended Capability ID
	uint16_t cap_version : 4; // Capability Version
	uint16_t next_ptr : 12;	  // Next capability pointer
} __attribute__((packed));

/*
 * SR-IOV PCIe extended capability on-wire layout.
 *
 * This structure is documentation/reference only. The SR-IOV config-space
 * reader/writer should synthesize each DW from struct vblk_sriov_pf_state
 * instead of embedding this structure into struct pcie_virtio_dev.
 *
 * Offsets, control bits, VF BAR count, and capability size come from
 * linux/pci_regs.h (PCI_SRIOV_*, PCI_EXT_CAP_ID_SRIOV,
 * PCI_EXT_CAP_SRIOV_SIZEOF).
 */
struct vblk_sriov_ext_cap_layout {
	uint32_t cap_header;		     /* DW0: cap_id, version, next_cap_offset */
	uint32_t sriov_capabilities;	     /* DW1: PCI_SRIOV_CAP */
	uint32_t control_status;	     /* DW2: PCI_SRIOV_CTRL + PCI_SRIOV_STATUS */
	uint32_t initial_vfs_total_vfs;	     /* DW3: PCI_SRIOV_INITIAL_VF + PCI_SRIOV_TOTAL_VF */
	uint32_t num_vfs_func_link;	     /* DW4: PCI_SRIOV_NUM_VF + PCI_SRIOV_FUNC_LINK */
	uint32_t first_vf_offset_stride;     /* DW5: PCI_SRIOV_VF_OFFSET + PCI_SRIOV_VF_STRIDE */
	uint32_t vf_device_id;		     /* DW6: PCI_SRIOV_VF_DID */
	uint32_t supported_pg_sizes;	     /* DW7: PCI_SRIOV_SUP_PGSIZE */
	uint32_t system_pg_size;	     /* DW8: PCI_SRIOV_SYS_PGSIZE */
	uint32_t vf_bar[PCI_SRIOV_NUM_BARS]; /* DW9..14: PCI_SRIOV_BAR */
	uint32_t vf_migration_state_offset;  /* DW15: PCI_SRIOV_VFM */
} __attribute__((packed));

_Static_assert(sizeof(struct vblk_sriov_ext_cap_layout) == PCI_EXT_CAP_SRIOV_SIZEOF,
	       "SR-IOV capability layout size mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, sriov_capabilities) == PCI_SRIOV_CAP,
	       "SR-IOV Capabilities offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, control_status) == PCI_SRIOV_CTRL,
	       "SR-IOV Control offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, initial_vfs_total_vfs) == PCI_SRIOV_INITIAL_VF,
	       "SR-IOV InitialVFs offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, num_vfs_func_link) == PCI_SRIOV_NUM_VF,
	       "SR-IOV NumVFs offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, first_vf_offset_stride) == PCI_SRIOV_VF_OFFSET,
	       "SR-IOV FirstVFOffset offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, vf_device_id) + sizeof(uint16_t) == PCI_SRIOV_VF_DID,
	       "SR-IOV VFDeviceID offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, supported_pg_sizes) == PCI_SRIOV_SUP_PGSIZE,
	       "SR-IOV SupportedPageSizes offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, system_pg_size) == PCI_SRIOV_SYS_PGSIZE,
	       "SR-IOV SystemPageSize offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, vf_bar) == PCI_SRIOV_BAR, "SR-IOV VF BAR offset mismatch");
_Static_assert(offsetof(struct vblk_sriov_ext_cap_layout, vf_migration_state_offset) == PCI_SRIOV_VFM,
	       "SR-IOV VF Migration State offset mismatch");

struct pcie_capability {
	struct pci_capability_header cap;

	uint16_t pcie_cap; // Bit 0-3: Capability Version, Bit 4-7: Device/Port Type

	uint32_t dev_cap; // Device Capabilities, bit28: FLR cap bit

	uint16_t dev_ctrl;   // Device Control, bit15: FLR bit
	uint16_t dev_status; // Device Status

	uint32_t link_cap;     // Link Capabilities
	uint16_t link_ctrl;    // Link Control
	uint16_t link_status;  // Link Status
	uint32_t slot_caps;    // Slot Capabilities (if applicable)
	uint16_t slot_ctrl;    // Slot Control
	uint16_t slot_status;  // Slot Status
	uint16_t root_ctrl;    // Root Control (Root Ports only)
	uint16_t root_caps;    // Root Capabilities
	uint32_t root_status;  // Root Status
	uint32_t dev_caps2;    // Device Capabilities 2
	uint16_t dev_ctrl2;    // Device Control 2
	uint16_t dev_status2;  // Device Status 2
	uint32_t link_caps2;   // Link Capabilities 2
	uint16_t link_ctrl2;   // Link Control 2
	uint16_t link_status2; // Link Status 2
	uint32_t slot_caps2;   // Slot Capabilities 2
	uint16_t slot_ctrl2;   // Slot Control 2
	uint16_t slot_status2; // Slot Status 2
} __attribute__((packed));

struct msix_capability {
	struct pci_capability_header cap;
	uint16_t msgctl;       // Message Control
	uint32_t table_offset; // BAR indicator + offset
	uint32_t pba_offset;   // BAR indicator + offset
} __attribute__((packed));

struct virtio_pci_capability {
	struct pci_capability_header cap; // = 0x09 (Vendor-Specific PCI capability)
	uint8_t cap_len;		  // Length of this capability structure in bytes
	uint8_t cfg_type;		  // Type of Virtio structure (1=common, 2=notify, etc.)

	uint8_t bar; // Which BAR this structure maps to
	uint8_t id;  // Multiple capabilities of the same type
	uint16_t rsrvd1;

	uint32_t offset; // Offset within the BAR
	uint32_t length; // Length of the region
} __attribute__((packed));

struct virtio_pci_cfg_capability {
	struct virtio_pci_capability base;

	uint8_t pci_cfg_data[4];
} __attribute__((packed));

struct virtio_pci_notify_capability {
	struct virtio_pci_capability base;

	uint32_t notify_off_multiplier;
} __attribute__((packed));

struct pcie_bar64_map {
	uint8_t log_size;
};

// Example of pcie devices
struct pcie_raw_cfg {
	uint32_t dw_regs[PCI_CFG_SPACE_EXP_SIZE / sizeof(uint32_t)];
	struct pcie_bar64_map bar64_map[3];
} __attribute__((packed));

struct pcie_virtio_dev {
	union {
		struct {
			struct pci_config_header_type0 regs;
			struct pcie_capability pcie_cap;
			struct msix_capability msix_cap;
			struct virtio_pci_capability common_cfg;
			struct virtio_pci_notify_capability notify_cfg;
			struct virtio_pci_capability isr_cfg;
			struct virtio_pci_capability device_cfg;
			struct virtio_pci_cfg_capability pci_cfg;

		} cfg;
		uint32_t dw_regs[PCI_CFG_SPACE_EXP_SIZE / sizeof(uint32_t)];
	};
	struct pcie_bar64_map bar64_map[3];
} __attribute__((packed));

#define TO_PCIE_RAW_CFG(dev) ((struct pcie_raw_cfg *)(dev))

static inline uint32_t pcie_config_read(const struct pcie_raw_cfg *dev, const uint16_t ext_reg_num)
{
	return dev->dw_regs[ext_reg_num];
}

static inline void pcie_config_write(struct pcie_raw_cfg *dev, const uint16_t ext_reg_num, const uint32_t val)
{
	dev->dw_regs[ext_reg_num] = val;
}

#endif
