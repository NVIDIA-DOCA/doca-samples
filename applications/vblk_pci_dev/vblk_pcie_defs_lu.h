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

#ifndef VBLK_PCIE_DEFS_LU_H_
#define VBLK_PCIE_DEFS_LU_H_

#include <stdint.h>

// PCI Configuration Space Layout
#define PCI_CONFIG_SPACE_SIZE 256
#define PCIE_CONFIG_SPACE_SIZE 4096

// Standard PCI Configuration Space Offsets
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_REVISION_ID 0x08
#define PCI_CLASS_PROG 0x09
#define PCI_CLASS_DEVICE 0x0A
#define PCI_CLASS_CODE 0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER 0x0D
#define PCI_HEADER_TYPE 0x0E
#define PCI_BIST 0x0F
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_BASE_ADDRESS_1 0x14
#define PCI_BASE_ADDRESS_2 0x18
#define PCI_BASE_ADDRESS_3 0x1C
#define PCI_BASE_ADDRESS_4 0x20
#define PCI_BASE_ADDRESS_5 0x24
#define PCI_CARDBUS_CIS 0x28
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID 0x2E
#define PCI_ROM_ADDRESS 0x30
#define PCI_CAPABILITIES_PTR 0x34
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN 0x3D
#define PCI_MIN_GNT 0x3E
#define PCI_MAX_LAT 0x3F

// PCI-to-PCI Bridge Configuration Space Offsets
#define PCI_PRIMARY_BUS 0x18
#define PCI_SECONDARY_BUS 0x19
#define PCI_SUBORDINATE_BUS 0x1A
#define PCI_SEC_LATENCY_TIMER 0x1B
#define PCI_IO_BASE 0x1C
#define PCI_IO_LIMIT 0x1D
#define PCI_SEC_STATUS 0x1E
#define PCI_MEMORY_BASE 0x20
#define PCI_MEMORY_LIMIT 0x22
#define PCI_PREF_MEMORY_BASE 0x24
#define PCI_PREF_MEMORY_LIMIT 0x26
#define PCI_PREF_BASE_UPPER32 0x28
#define PCI_PREF_LIMIT_UPPER32 0x2C
#define PCI_IO_BASE_UPPER16 0x30
#define PCI_IO_LIMIT_UPPER16 0x32
#define PCI_BRIDGE_ROM_ADDRESS 0x38
#define PCI_BRIDGE_CONTROL 0x3E

// PCI Command Register Bits
#define PCI_COMMAND_IO 0x0001	       // Enable I/O Space
#define PCI_COMMAND_MEMORY 0x0002      // Enable Memory Space
#define PCI_COMMAND_MASTER 0x0004      // Enable Bus Mastering
#define PCI_COMMAND_SPECIAL 0x0008     // Enable Special Cycles
#define PCI_COMMAND_INVALIDATE 0x0010  // Enable Memory Write and Invalidate
#define PCI_COMMAND_VGA_PALETTE 0x0020 // Enable VGA Palette Snooping
#define PCI_COMMAND_PARITY 0x0040      // Enable Parity Checking
#define PCI_COMMAND_WAIT 0x0080	       // Enable Address/Data Stepping
#define PCI_COMMAND_SERR 0x0100	       // Enable SERR
#define PCI_COMMAND_FAST_BACK 0x0200   // Enable Fast Back-to-Back
#define PCI_COMMAND_INT_DISABLE 0x0400 // Disable INT style interrupts
#define PCI_COMMAND_RES_BIT_MASK ((1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15))

// PCI Status Register Bits
#define PCI_STATUS_READY 0x0001	      // Immediate Readiness
#define PCI_STATUS_INTERRUPT 0x0008   // Interrupt Status
#define PCI_STATUS_CAP_LIST 0x0010    // Capabilities List
#define PCI_STATUS_66MHZ 0x0020	      // 66 MHz Capable
#define PCI_STATUS_UDF 0x0040	      // User Definable Features
#define PCI_STATUS_FAST_BACK 0x0080   // Fast Back-to-Back Capable
#define PCI_STATUS_PARITY 0x0100      // Data Parity Detected
#define PCI_STATUS_DEVSEL_MASK 0x0600 // DEVSEL Timing
#define PCI_STATUS_DEVSEL_FAST 0x0000
#define PCI_STATUS_DEVSEL_MEDIUM 0x0200
#define PCI_STATUS_DEVSEL_SLOW 0x0400
#define PCI_STATUS_SIG_TARGET_ABORT 0x0800 // Signaled Target Abort
#define PCI_STATUS_REC_TARGET_ABORT 0x1000 // Received Target Abort
#define PCI_STATUS_REC_MASTER_ABORT 0x2000 // Received Master Abort
#define PCI_STATUS_SIG_SYSTEM_ERROR 0x4000 // Signaled System Error
#define PCI_STATUS_DETECTED_PARITY 0x8000  // Detected Parity Error

// PCI Header Types
#define PCI_HEADER_TYPE_NORMAL 0x00
#define PCI_HEADER_TYPE_BRIDGE 0x01
#define PCI_HEADER_TYPE_CARDBUS 0x02
#define PCI_HEADER_TYPE_MASK 0x7F
#define PCI_HEADER_TYPE_MULTI_FUNC 0x80

// PCI Base Address Register (BAR) Bits
#define PCI_BASE_ADDRESS_SPACE 0x01 // 0 = Memory, 1 = I/O
#define PCI_BASE_ADDRESS_SPACE_IO 0x01
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0x00
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06
#define PCI_BASE_ADDRESS_MEM_TYPE_32 0x00  // 32-bit
#define PCI_BASE_ADDRESS_MEM_TYPE_1M 0x02  // Below 1M (obsolete)
#define PCI_BASE_ADDRESS_MEM_TYPE_64 0x04  // 64-bit
#define PCI_BASE_ADDRESS_MEM_PREFETCH 0x08 // Prefetchable
#define PCI_BASE_ADDRESS_MEM_MASK 0xFFFFFFF0
#define PCI_BASE_ADDRESS_IO_MASK 0xFFFFFFFC

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

// PCI Capability IDs
#define PCI_CAP_ID_PM 0x01     // Power Management
#define PCI_CAP_ID_AGP 0x02    // AGP
#define PCI_CAP_ID_VPD 0x03    // Vital Product Data
#define PCI_CAP_ID_SLOTID 0x04 // Slot Identification
#define PCI_CAP_ID_MSI 0x05    // Message Signaled Interrupts
#define PCI_CAP_ID_CHSWP 0x06  // CompactPCI HotSwap
#define PCI_CAP_ID_PCIX 0x07   // PCI-X
#define PCI_CAP_ID_HT 0x08     // HyperTransport
#define PCI_CAP_ID_VNDR 0x09   // Vendor Specific
#define PCI_CAP_ID_DBG 0x0A    // Debug port
#define PCI_CAP_ID_CCRC 0x0B   // CompactPCI Central Resource Control
#define PCI_CAP_ID_SHPC 0x0C   // PCI Standard Hot-Plug Controller
#define PCI_CAP_ID_SSVID 0x0D  // Bridge subsystem vendor/device ID
#define PCI_CAP_ID_AGP3 0x0E   // AGP Target PCI-PCI bridge
#define PCI_CAP_ID_SECDEV 0x0F // Secure Device
#define PCI_CAP_ID_EXP 0x10    // PCI Express
#define PCI_CAP_ID_MSIX 0x11   // MSI-X
#define PCI_CAP_ID_SATA 0x12   // SATA Data/Index Conf.
#define PCI_CAP_ID_AF 0x13     // PCI Advanced Features
#define PCI_CAP_ID_EA 0x14     // PCI Enhanced Allocation

// PCIe Extended Capability IDs
#define PCIE_EXT_CAP_ID_ERR 0x0001     // Advanced Error Reporting
#define PCIE_EXT_CAP_ID_VC 0x0002      // Virtual Channel
#define PCIE_EXT_CAP_ID_DSN 0x0003     // Device Serial Number
#define PCIE_EXT_CAP_ID_PWR 0x0004     // Power Budgeting
#define PCIE_EXT_CAP_ID_RCLD 0x0005    // Root Complex Link Declaration
#define PCIE_EXT_CAP_ID_RCILC 0x0006   // Root Complex Internal Link Control
#define PCIE_EXT_CAP_ID_RCEC 0x0007    // Root Complex Event Collector
#define PCIE_EXT_CAP_ID_MFVC 0x0008    // Multi-Function VC
#define PCIE_EXT_CAP_ID_VC9 0x0009     // Virtual Channel (9)
#define PCIE_EXT_CAP_ID_RCRB 0x000A    // Root Complex RB
#define PCIE_EXT_CAP_ID_VNDR 0x000B    // Vendor Specific
#define PCIE_EXT_CAP_ID_CAC 0x000C     // Configuration Access Correlation
#define PCIE_EXT_CAP_ID_ACS 0x000D     // Access Control Services
#define PCIE_EXT_CAP_ID_ARI 0x000E     // Alternative Routing-ID
#define PCIE_EXT_CAP_ID_ATS 0x000F     // Address Translation Services
#define PCIE_EXT_CAP_ID_SRIOV 0x0010   // Single Root I/O Virtualization
#define PCIE_EXT_CAP_ID_MRIOV 0x0011   // Multi Root I/O Virtualization
#define PCIE_EXT_CAP_ID_MCAST 0x0012   // Multicast
#define PCIE_EXT_CAP_ID_PRI 0x0013     // Page Request Interface
#define PCIE_EXT_CAP_ID_AMD_XXX 0x0014 // AMD reserved
#define PCIE_EXT_CAP_ID_REBAR 0x0015   // Resizable BAR
#define PCIE_EXT_CAP_ID_DPA 0x0016     // Dynamic Power Allocation
#define PCIE_EXT_CAP_ID_TPH 0x0017     // TPH Requester
#define PCIE_EXT_CAP_ID_LTR 0x0018     // Latency Tolerance Reporting
#define PCIE_EXT_CAP_ID_SECPCI 0x0019  // Secondary PCIe
#define PCIE_EXT_CAP_ID_PMUX 0x001A    // Protocol Multiplexing
#define PCIE_EXT_CAP_ID_PASID 0x001B   // Process Address Space ID
#define PCIE_EXT_CAP_ID_LNR 0x001C     // LN Requester
#define PCIE_EXT_CAP_ID_DPC 0x001D     // Downstream Port Containment
#define PCIE_EXT_CAP_ID_L1SS 0x001E    // L1 PM Substates
#define PCIE_EXT_CAP_ID_PTM 0x001F     // Precision Time Measurement

// PCIe Device/Port Types
#define PCIE_TYPE_ENDPOINT 0x0	  // PCIe Endpoint
#define PCIE_TYPE_LEG_END 0x1	  // Legacy PCIe Endpoint
#define PCIE_TYPE_ROOT_PORT 0x4	  // Root Port of RC
#define PCIE_TYPE_UPSTREAM 0x5	  // Upstream Port of Switch
#define PCIE_TYPE_DOWNSTREAM 0x6  // Downstream Port of Switch
#define PCIE_TYPE_PCI_BRIDGE 0x7  // PCIe to PCI/PCI-X Bridge
#define PCIE_TYPE_PCI_HOST 0x8	  // PCI/PCI-X to PCIe Bridge
#define PCIE_TYPE_ROOT_INT_EP 0x9 // Root Complex Integrated Endpoint
#define PCIE_TYPE_ROOT_EC 0xA	  // Root Complex Event Collector

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

struct pcie_capability {
	struct pci_capability_header cap;

	uint16_t pcie_cap; // Bit 0-3: Capability Version, Bit 4-7: Device/Port Type

	uint32_t dev_cap; // Device Capabilities

	uint16_t dev_ctrl;   // Device Control
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

// Virtio specific caps
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG 5

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
	uint32_t dw_regs[PCIE_CONFIG_SPACE_SIZE / sizeof(uint32_t)];
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
		uint32_t dw_regs[PCIE_CONFIG_SPACE_SIZE / sizeof(uint32_t)];
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
