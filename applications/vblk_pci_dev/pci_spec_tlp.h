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

#ifndef PCI_SPEC_TLP_H_
#define PCI_SPEC_TLP_H_

#include <stdint.h>
#include <linux/pci.h>

#include <doca_devemu_pci_tlp.h>
#include <doca_bitfield.h>
#include <stdatomic.h>

/* PCIe TLP Types */
enum tlp_type {
	TLP_TYPE_MEM_READ = 0x00,
	TLP_TYPE_MEM_READ_LOCKED = 0x01,
	TLP_TYPE_MEM_WRITE = 0x00,
	TLP_TYPE_IO_READ = 0x02,
	TLP_TYPE_IO_WRITE = 0x02,
	TLP_TYPE_CFG_READ_TYPE0 = 0x04,
	TLP_TYPE_CFG_WRITE_TYPE0 = 0x04,
	TLP_TYPE_CFG_READ_TYPE1 = 0x05,
	TLP_TYPE_CFG_WRITE_TYPE1 = 0x05,
	TLP_TYPE_MSG_TO_ROOT = 0x10,
	TLP_TYPE_MSG_BY_ADDR = 0x11,
	TLP_TYPE_MSG_BY_ID = 0x12,
	TLP_TYPE_MSG_FROM_ROOT = 0x13,
	TLP_TYPE_MSG_LOCAL = 0x14,
	TLP_TYPE_MSG_GATHER = 0x15,
	TLP_TYPE_COMPLETION = 0x0A,
	TLP_TYPE_COMPLETION_DATA = 0x0A,
	TLP_TYPE_COMPLETION_LOCKED = 0x0B,
	TLP_TYPE_COMPLETION_LOCKED_DATA = 0x0B
};

/* PCIe TLP Header Format Types */
enum tlp_format {
	TLP_FMT_3DW_NO_DATA = 0x0,   /* 3DW header, no data */
	TLP_FMT_4DW_NO_DATA = 0x1,   /* 4DW header, no data */
	TLP_FMT_3DW_WITH_DATA = 0x2, /* 3DW header, with data */
	TLP_FMT_4DW_WITH_DATA = 0x3, /* 4DW header, with data */
	TLP_FMT_TLP_PREFIX = 0x4     /* TLP prefix */
};

#define TLP_REQ_TYPE_MEMORY_READ_WRITE 0x0
#define TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0 0x4
#define TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1 0x5

/* Completion Status */
enum tlp_completion_status {
	TLP_CPL_STATUS_SC = 0x0,  /* Successful Completion */
	TLP_CPL_STATUS_UR = 0x1,  /* Unsupported Request */
	TLP_CPL_STATUS_CRS = 0x2, /* Configuration Request Retry Status */
	TLP_CPL_STATUS_CA = 0x4	  /* Completer Abort */
};

enum tlp_req_type {
	TLP_REQ_TYPE_INVALID = 0,
	TLP_REQ_TYPE_MEMORY_READ,
	TLP_REQ_TYPE_MEMORY_WRITE,
	TLP_REQ_TYPE_IO_READ,
	TLP_REQ_TYPE_IO_WRITE,
	TLP_REQ_TYPE_CONFIG_READ_TYPE_0,
	TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0,
	TLP_REQ_TYPE_CONFIG_READ_TYPE_1,
	TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1,
};

/* Note that incoming TLPs are in the big endian format */
#define GET_TLP_REQ_FMT(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 29), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TYPE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(28, 24), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_FMT_AND_TYPE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 24), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TAG9(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(23, 23), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TAG8(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(19, 19), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_LENGTH(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(9, 0), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_REQ_ID(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 16), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_TAG(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(15, 8), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_LAST_DW_BE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(7, 4), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_FIRST_DW_BE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(3, 0), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_BUS(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 24), ((uint32_t *)(tlp_req))[2])
#define GET_TLP_REQ_DEVICE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(23, 19), ((uint32_t *)(tlp_req))[2])
#define GET_TLP_REQ_FUNCTION(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(18, 16), ((uint32_t *)(tlp_req))[2])
#define GET_TLP_REQ_EXT_REG_NUM(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(11, 2), ((uint32_t *)(tlp_req))[2])

enum tlp_req_type tlp_req_get_type(struct doca_devemu_pci_tlp_channel_req *tlp_req);

/*
 * PCI Switch topology definitions for multi-endpoint support.
 *
 * Topology:
 *   USP (idx=0)
 *     +-- DSP[0] (idx=1)
 *     |     +-- PF[0]  (idx=num_bridges)
 *     |           +-- VF[0][0]
 *     |           +-- VF[0][1]
 *     |           +-- ...
 *     +-- DSP[1] (idx=2)
 *     |     +-- PF[1]  (idx=num_bridges+1)
 *     |           +-- VF[1][0]
 *     |           +-- VF[1][1]
 *     |           +-- ...
 *     +-- ...
 *     +-- DSP[N] (idx=N+1)
 *           +-- PF[N]  (idx=num_bridges+N)
 *                 +-- VF[N][0]
 *                 +-- VF[N][1]
 *                 +-- ...
 *   VF entries, when SR-IOV is enabled, follow PF entries.
 *   DUMMY (idx=num_bridges+num_ep+num_ep*[sriov_total_vfs when SR-IOV is enabled])
 */

#define MAX_NUM_USP 1
#define MAX_NUM_DSP 32
#define MAX_NUM_EP 32
#define MAX_NUM_BRIDGE (MAX_NUM_USP + MAX_NUM_DSP)
#define DUMMY_DEV_NUM 1

#define BDF(bus, device, function) ((((bus)&0xff) << 8) | (((device)&0x1f) << 3) | ((function)&0x7))

#define USP_IDX(tlp_ctx) 0
#define FIRST_DSP_IDX(tlp_ctx) MAX_NUM_USP
#define FIRST_PF_IDX(tlp_ctx) ((tlp_ctx)->num_bridges)
#define FIRST_VF_IDX(tlp_ctx) (FIRST_PF_IDX(tlp_ctx) + (tlp_ctx)->num_ep)
#define NUM_SRIOV_VFS(tlp_ctx) \
	(((tlp_ctx)->sriov_enabled && (tlp_ctx)->sriov_total_vfs != 0) ? \
		 (tlp_ctx)->num_ep * (tlp_ctx)->sriov_total_vfs : \
		 0)
#define DUMMY_DEV_IDX(tlp_ctx) (FIRST_VF_IDX(tlp_ctx) + NUM_SRIOV_VFS(tlp_ctx))

#define BDF_MAP_SIZE 256

/* TLP completion format definitions */
#define TLP_FMT_CPL_NODATA 0x00
#define TLP_FMT_CPL_W_DATA 0x02

/* Constants for MMIO */
#define BYTES_IN_DWORD 4
#define TLP_DATA_ARRAY_MAX_SIZE 256
#define DUMMY_READ_DATA_BASE 0xDEADBEEF

/* Memory Write TLP format values */
#define MEM_WR_FMT_3DW_W_DATA 0x2
#define MEM_WR_FMT_4DW_W_DATA 0x3
#define MEM_WR_TYPE 0x0

/* Memory address extraction macros for TLP */
#define GET_MEM_ADDR_32BIT(tlp_req) (DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 2), ((uint32_t *)(tlp_req))[2]) << 2)
#define GET_MEM_ADDR_64BIT_HIGH(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 0), ((uint32_t *)(tlp_req))[2])
#define GET_MEM_ADDR_64BIT_LOW(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 2), ((uint32_t *)(tlp_req))[3])

/* Vendor/Device ID settings */
#define TLP_PCI_TYPE_VENDOR_ID 0x1af4		/* Red Hat, Inc. */
#define TLP_PCI_TYPE_ENDPOINT_DEVICE_ID 0x10f0	/* Endpoint device ID */
#define TLP_PCI_TYPE_BRIDGE_DEVICE_ID 0x10f1	/* Bridge device ID */
#define TLP_PCI_TYPE_SUBSYSTEM_VENDOR_ID 0x1af4 /* Red Hat, Inc. */
#define TLP_PCI_TYPE_SUBSYSTEM_ID 0x10f0	/* Subsystem ID */
#define TLP_PCI_TYPE_REVISION_ID 0

/* Class codes */
#define TLP_PCI_CLASS_CODE_BRIDGE 0x060400

/* Header types */
#define HEADER_TYPE_ENDPOINT 0x00
#define HEADER_TYPE_ENDPOINT_MULTIFUNCTION 0x80
#define HEADER_TYPE_BRIDGE 0x01
#define HEADER_TYPE_BRIDGE_MULTIFUNCTION 0x81

/* PCI capability IDs and offsets */
#define TLP_PCI_CAP_ID_PM 0x01
#define TLP_PCI_CAP_ID_MSI 0x05
#define TLP_PCI_CAP_ID_VPD 0x03
#define TLP_PCI_CAP_ID_EXPRESS 0x10
#define TLP_PCI_CAP_ID_MSIX 0x11

#define TLP_PCI_CAP_OFFSET_PM 0x40
#define TLP_PCI_CAP_OFFSET_MSI 0x50
#define TLP_PCI_CAP_OFFSET_VPD 0x48
#define TLP_PCI_CAP_OFFSET_EXPRESS 0x60
#define TLP_PCI_CAP_OFFSET_MSIX 0x9c

/*
 * PCIe Express Capabilities Register (PCIe cap + 0x02):
 *   bits [3:0]  Capability Version
 *   bits [7:4]  Device/Port Type
 *   bit  [8]    Slot Implemented (meaningful only on Root/Downstream Ports)
 */
#define TLP_PCIE_CAP_VERSION 2u
#define TLP_PCIE_PORT_TYPE_UPSTREAM 5u
#define TLP_PCIE_PORT_TYPE_DOWNSTREAM 6u
#define TLP_PCIE_CAP_SLOT_IMPLEMENTED 0x0100u

#define TLP_PCIE_CAP_REG(port_type, slot_implemented) \
	((TLP_PCIE_CAP_VERSION & 0xFu) | (((port_type)&0xFu) << 4) | \
	 ((slot_implemented) ? TLP_PCIE_CAP_SLOT_IMPLEMENTED : 0u))

/* PCIe Express Capability - Slot Control bits */
#define SLOT_CTRL_ATTN_BTN_PRESSED_EN 0x0001
#define SLOT_CTRL_PWR_FAULT_DETECT_EN 0x0002
#define SLOT_CTRL_MRL_SENSOR_CHANGED_EN 0x0004
#define SLOT_CTRL_PRESENCE_DETECT_EN 0x0008
#define SLOT_CTRL_CMD_COMPLETED_INT_EN 0x0010
#define SLOT_CTRL_HP_INT_EN 0x0020
#define SLOT_CTRL_POWER_CONTROLLER 0x0400
#define SLOT_CTRL_DL_STATE_CHANGED_EN 0x1000

/* PCIe Express Capability - Slot Capabilities bits */
#define SLOT_CAP_ATTN_BTN_PRESENT 0x00000001
#define SLOT_CAP_PWR_CTRL_PRESENT 0x00000002
#define SLOT_CAP_MRL_SENSOR_PRESENT 0x00000004
#define SLOT_CAP_ATTN_IND_PRESENT 0x00000008
#define SLOT_CAP_PWR_IND_PRESENT 0x00000010
#define SLOT_CAP_HP_SURPRISE 0x00000020
#define SLOT_CAP_HP_CAPABLE 0x00000040
#define SLOT_CAP_PWR_LIMIT_VALUE_SHIFT 7
#define SLOT_CAP_PWR_LIMIT_SCALE_SHIFT 15
#define SLOT_CAP_EMI_PRESENT 0x00020000
#define SLOT_CAP_NO_CMD_CMPL 0x00040000
#define SLOT_CAP_PHYS_SLOT_NUM_SHIFT 19

#define TLP_BRIDGE_PHYS_SLOT_NUM 56
#define TLP_BRIDGE_SLOT_PWR_LIMIT 32
#define TLP_BRIDGE_SLOT_CAPABILITIES \
	(SLOT_CAP_PWR_CTRL_PRESENT | SLOT_CAP_HP_SURPRISE | SLOT_CAP_HP_CAPABLE | \
	 (TLP_BRIDGE_SLOT_PWR_LIMIT << SLOT_CAP_PWR_LIMIT_VALUE_SHIFT) | \
	 (TLP_BRIDGE_PHYS_SLOT_NUM << SLOT_CAP_PHYS_SLOT_NUM_SHIFT))

/* PCIe Express Capability - Slot Status bits */
#define SLOT_STS_ATTN_BTN_PRESSED 0x0001
#define SLOT_STS_PWR_FAULT_DETECTED 0x0002
#define SLOT_STS_MRL_SENSOR_CHANGED 0x0004
#define SLOT_STS_PRESENCE_DETECT_CHANGED 0x0008
#define SLOT_STS_CMD_COMPLETED 0x0010
#define SLOT_STS_MRL_SENSOR_STATE 0x0020
#define SLOT_STS_PRESENCE_DETECT_STATE 0x0040
#define SLOT_STS_DL_STATE_CHANGED 0x0100

/* PCIe Express Capability - Link Status bits/fields.
 * LBMS/LABS are W1C from host side; all other LinkSta bits are RO. */
#define LINK_STS_SPEED_16GT 0x0004	  /* Current Link Speed [3:0] = 16 GT/s (Gen4) */
#define LINK_STS_WIDTH_X16 0x0100	  /* Negotiated Link Width [9:4] = x16 */
#define LINK_STS_SLOT_CLOCK_CONFIG 0x1000 /* Slot Clock Configuration [12] */
#define LINK_STS_DL_ACTIVE 0x2000
#define LINK_STS_BW_MGMT 0x4000
#define LINK_STS_AUTO_BW 0x8000
#define LINK_STS_W1C (LINK_STS_BW_MGMT | LINK_STS_AUTO_BW)

/* Initial Link Status: Gen4 x16, slot clock config set. */
#define LINK_STS_INIT (LINK_STS_SPEED_16GT | LINK_STS_WIDTH_X16 | LINK_STS_SLOT_CLOCK_CONFIG)

/* Negotiated link parameters reported in Link Status when the link is up.
 * RO bits must be non-zero (Negotiated Link Width != 0) or the host's pciehp
 * link check fails with "Cannot train link". Combined with LINK_STS_DL_ACTIVE
 * on slot power-on to model the link training. */
#define LINK_STS_TRAINED LINK_STS_INIT

/* PCIe Express Capability - Link Capabilities bits/fields */
#define LINK_CAP_SPEED_16GT 0x00000004u		  /* Max Link Speed [3:0] = 16 GT/s (Gen4) */
#define LINK_CAP_WIDTH_X16 0x00000100u		  /* Max Link Width [9:4] = x16 */
#define LINK_CAP_DLL_ACTIVE_REPORTING 0x00100000u /* DLL Link Active Reporting Capable [20] */
#define LINK_CAP_ASPM_OPT_COMPLIANCE 0x00400000u  /* ASPM Optionality Compliance [22] */
#define LINK_CAP_PORT_NUM_SHIFT 24		  /* Port Number [31:24] */

/* Initial Link Capabilities: Gen4 x16, DLL active reporting, ASPM optional, at @port_num. */
#define LINK_CAP_INIT(port_num) \
	(LINK_CAP_SPEED_16GT | LINK_CAP_WIDTH_X16 | LINK_CAP_DLL_ACTIVE_REPORTING | LINK_CAP_ASPM_OPT_COMPLIANCE | \
	 ((uint32_t)(port_num) << LINK_CAP_PORT_NUM_SHIFT))

/* PCIe Express Capability - Device Status W1C bits [3:0] (CED/NFED/FED/URD).
 * Bit 4 (AUX Power Detected) is RO per PCIe Base Spec, so it must not be in
 * the W1C mask -- otherwise a host W1C write to bit 4 would clear a hardware
 * status the host is not allowed to change. */
#define DEV_STS_W1C_MASK 0x000F

/* Capability lengths in DWORDs */
#define TLP_CAP_PM_LEN_DW 2
#define TLP_CAP_MSI_LEN_DW 6
#define TLP_CAP_VPD_LEN_DW 2
#define TLP_CAP_EXPRESS_LEN_DW 15
#define TLP_CAP_MSIX_LEN_DW 3

#define TLP_CAP_PM_REG_NUM (TLP_PCI_CAP_OFFSET_PM / 4)
#define TLP_CAP_MSI_REG_NUM (TLP_PCI_CAP_OFFSET_MSI / 4)
#define TLP_CAP_VPD_REG_NUM (TLP_PCI_CAP_OFFSET_VPD / 4)
#define TLP_CAP_EXPRESS_REG_NUM (TLP_PCI_CAP_OFFSET_EXPRESS / 4)
#define TLP_CAP_MSIX_REG_NUM (TLP_PCI_CAP_OFFSET_MSIX / 4)

#define TLP_PCI_CAPS_NUM 4

/* BDF map entry for fast device lookup */
struct bdf_map_entry {
	uint32_t key;
	struct pci_device_config *dev_cfg;
	struct bdf_map_entry *next;
};

/* PCI configuration space Type 0 header (Endpoint) */
struct pci_cfg_type0_header {
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t command;
	uint16_t status;
	uint8_t revision_id;
	uint32_t class_code : 24;
	uint8_t cache_line_size;
	uint8_t latency_timer;
	uint8_t header_type;
	uint8_t bist;
	uint32_t bar[6];
	uint32_t cardbus_cis_pointer;
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_id;
	uint32_t exp_rom_base_addr;
	uint8_t cap_ptr;
	uint32_t reserved_at_13 : 24;
	uint32_t reserved_at_14;
	uint8_t interrupt_line;
	uint8_t interrupt_pin;
	uint8_t min_grant;
	uint8_t max_latency;
};

/* PCI configuration space Type 1 header (Bridge) */
struct pci_cfg_type1_header {
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t command;
	uint16_t status;
	uint8_t revision_id;
	uint32_t class_code : 24;
	uint8_t cache_line_size;
	uint8_t latency_timer;
	uint8_t header_type;
	uint8_t bist;
	uint32_t bar[2];
	uint8_t primary_bus;
	uint8_t secondary_bus;
	uint8_t subordinate_bus;
	uint8_t secondary_latency;
	uint8_t io_base;
	uint8_t io_limit;
	uint16_t secondary_status;
	uint16_t memory_base;
	uint16_t memory_limit;
	uint16_t pre_memory_base;
	uint16_t pre_memory_limit;
	uint32_t pre_memory_base_upper_32bit;
	uint32_t pre_memory_limit_upper_32bit;
	uint16_t io_base_upper_16bit;
	uint16_t io_limit_upper_16bit;
	uint8_t cap_ptr;
	uint32_t reserved : 24;
	uint32_t exp_rom_base_addr;
	uint8_t interrupt_line;
	uint8_t interrupt_pin;
	uint16_t bridge_control;
};

/* PCI Express Capability structure */
struct pci_express_capability {
	uint8_t cap_id;
	uint8_t next_cap_ptr;
	uint16_t pcie_cap_register;
	uint32_t dev_capabilities;
	uint16_t dev_control;
	uint16_t dev_status;
	uint32_t link_capabilities;
	uint16_t link_control;
	uint16_t link_status;
	uint32_t slot_capabilities;
	uint16_t slot_control;
	uint16_t slot_status;
	uint16_t root_control;
	uint16_t root_capabilities;
	uint32_t root_status;
	uint32_t dev_capabilities2;
	uint16_t dev_control2;
	uint16_t dev_status2;
	uint32_t link_capabilities2;
	uint16_t link_control2;
	uint16_t link_status2;
	uint32_t slot_capabilities2;
	uint16_t slot_control2;
	uint16_t slot_status2;
};

/* MSI Capability structure */
struct pci_msi_capability {
	uint8_t cap_id;
	uint8_t next_cap_ptr;
	uint16_t message_control;
	uint32_t message_address_low;
	uint32_t message_address_high;
	uint16_t message_data;
	uint16_t reserved;
	uint32_t mask_bits;
	uint32_t pending_bits;
};

/* MSI-X Capability structure */
struct pci_msix_capability {
	uint8_t cap_id;
	uint8_t next_cap_ptr;
	uint16_t message_control;
	uint32_t table_offset;
	uint32_t pba_offset;
};

/* VPD Capability structure */
struct pci_vpd_capability {
	uint8_t cap_id;
	uint8_t next_cap_ptr;
	uint16_t addr_register;
	uint32_t data_register;
};

/* PM Capability structure */
struct pci_pm_capability {
	uint8_t cap_id;
	uint8_t next_cap_ptr;
	uint16_t pmc;
	uint16_t pmcsr;
	uint8_t reserved;
	uint8_t data;
};

/* PCI Capabilities container */
struct pci_capabilities {
	struct pci_express_capability express;
	struct pci_msi_capability msi;
	struct pci_msix_capability msix;
	struct pci_vpd_capability vpd;
	struct pci_pm_capability pm;
};

/* TLP capability configuration structure */
struct tlp_cap_conf {
	uint8_t id;
	uint16_t offset;
	uint16_t length;
};

static const struct tlp_cap_conf tlp_pci_cap_confs[TLP_PCI_CAPS_NUM] = {
	{TLP_PCI_CAP_ID_EXPRESS, TLP_PCI_CAP_OFFSET_EXPRESS, TLP_CAP_EXPRESS_LEN_DW * 4},
	{TLP_PCI_CAP_ID_MSIX, TLP_PCI_CAP_OFFSET_MSIX, TLP_CAP_MSIX_LEN_DW * 4},
	{TLP_PCI_CAP_ID_VPD, TLP_PCI_CAP_OFFSET_VPD, TLP_CAP_VPD_LEN_DW * 4},
	{TLP_PCI_CAP_ID_PM, TLP_PCI_CAP_OFFSET_PM, TLP_CAP_PM_LEN_DW * 4},
};

struct vblk_sriov_pf_state;
struct vblk_sriov_vf_slot;

/* PCI device configuration (bridge or endpoint) in the PCI switch topology */
struct pci_device_config {
	union {
		struct pci_cfg_type0_header type0;
		struct pci_cfg_type1_header type1;
	} cfg_space_hdr;

	struct pci_capabilities caps;

	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint16_t bdf;

	bool is_bridge;
	bool is_endpoint;
	bool is_dummy;
	bool is_bdf_set;

	atomic_bool device_present;
	atomic_bool pending_unplug;
	atomic_bool host_power_off;
	atomic_bool host_power_on_done;
	/* Command Completed MSI deferred due to ACG credit exhaustion; retried on the TLP thread. */
	atomic_bool cmd_completed_msi_pending;
	struct doca_dev_rep *rep;
	struct doca_devemu_pci_tlp_dev *tlp_dev;
	uint32_t sriov_dynamic_first_vf_idx;
	bool sriov_dynamic_vf_range_owned;

	uint16_t requester_id;
	uint16_t completer_id;
	uint16_t tag9 : 1;
	uint16_t tag8 : 1;
	uint16_t tag : 8;
	uint8_t cmpl_fmt;
	uint8_t cmpl_type;
	uint16_t cmpl_length;
	uint8_t cmpl_status;

	/* NULL when SR-IOV is disabled for this endpoint. */
	struct vblk_sriov_pf_state *sriov;

	/* Valid only for SR-IOV VF entries. */
	struct vblk_sriov_vf_slot *sriov_vf;
};

struct vblk_pci_virtio_dev;
struct vblk_ctrl;

/* TLP context for managing the PCI switch topology and all devices */
struct vblk_tlp_context {
	struct doca_dev *dev;
	struct doca_devemu_pci_type *pci_type;
	struct doca_devemu_pci_tlp_channel *tlp_channel;
	struct doca_pe *pe;
	struct pci_device_config *devs_config;
	uint32_t num_devices;
	uint32_t num_ep;
	uint32_t num_dsp;
	uint32_t num_bridges;
	struct bdf_map_entry *bdf_map[BDF_MAP_SIZE];
	struct bdf_map_entry *bdf_entries;
	bool hotplug_mode;
	struct vblk_pci_virtio_dev **virtio_devs;
	struct vblk_ctrl *vblk_ctrls;
	uint16_t num_queues;
	bool sriov_enabled;
	uint16_t sriov_total_vfs;
	uint16_t sriov_num_vf_msix;
	uint16_t sriov_num_vf_db;
	struct {
		bool dynamic_sriov_supported;
		uint16_t max_total_vfs;
		uint16_t dynamic_vf_pool_size;
		uint16_t max_vf_stride;
		uint16_t max_first_vf_offset;
	} sriov_caps;
	struct {
		uint16_t pool_size;
		uint8_t *in_use;
	} sriov_pool;
	struct doca_devemu_pci_tlp_channel_req **acg_queue;
	uint16_t acg_queue_head;
	uint16_t acg_queue_tail;
	uint16_t acg_queue_count;
	uint16_t acg_queue_size; /* Buffer capacity (over-provisioned, see VBLK_ACG_QUEUE_BUFFER_SIZE) */
	uint16_t acg_device_max; /* Max outstanding ACGs reported by firmware (cap_get_max_acg) */
	uint32_t unplug_queue[MAX_NUM_EP];
	uint32_t unplug_head;
	uint32_t unplug_tail;
	uint32_t unplug_count;
	int32_t unplug_active;
};

#endif /* PCI_SPEC_TLP_H_ */
