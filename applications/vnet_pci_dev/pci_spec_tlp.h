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

#ifndef PCI_SPEC_TLP_H
#define PCI_SPEC_TLP_H

#include <doca_devemu_pci_tlp.h>
#include <doca_bitfield.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <linux/if_ether.h>

struct ibv_pd;
struct ibv_context;

/*
 * TLP (Transaction Layer Packet) Protocol Definitions
 * These define the PCIe transaction layer packet formats and types
 * as per PCIe Base Specification
 */

/* PCIe TLP Header Format Types */
enum tlp_format {
	TLP_FMT_3DW_NODATA = 0x0, /* 3DW header, no data payload */
	TLP_FMT_4DW_NODATA = 0x1, /* 4DW header, no data payload */
	TLP_FMT_3DW_W_DATA = 0x2, /* 3DW header, with data payload */
	TLP_FMT_4DW_W_DATA = 0x3, /* 4DW header, with data payload */
	TLP_FMT_TLP_PREFIX = 0x4  /* TLP prefix (for extended features) */
};

#define TLP_REQ_TYPE_MEMORY_READ_WRITE 0x0
#define TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0 0x4
#define TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1 0x5

/* TLP Request Types */
/* TLP request type enumeration */
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

/* TLP Completion Status Codes */
enum tlp_completion_status {
	TLP_CPL_STATUS_SC = 0x0,  /* Successful Completion */
	TLP_CPL_STATUS_UR = 0x1,  /* Unsupported Request */
	TLP_CPL_STATUS_CRS = 0x2, /* Configuration Request Retry Status */
	TLP_CPL_STATUS_CA = 0x4	  /* Completer Abort */
};

/* TLP Completion Type Codes */
enum tlp_type {
	TLP_TYPE_COMPLETION = 0x0A /* Completion TLP type */
};

#define GET_TLP_REQ_FMT(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 29), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TYPE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(28, 24), ((uint32_t *)(tlp_req))[0])
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

#define MAX_NUM_USP 1
#define MAX_NUM_DSP 32 /* Maximum 32 Downstream bridges */
#define MAX_NUM_EP 32  /* Maximum 32 endpoints */
#define MAX_NUM_BRIDGE (MAX_NUM_USP + MAX_NUM_DSP)
#define DUMMY_DEV_NUM 1 /* 1 Dummy device for invalid requests */
#define MAX_NUM_PCI_DEVICE (MAX_NUM_BRIDGE + MAX_NUM_EP + DUMMY_DEV_NUM)

/* BDF macro */
#define BDF(bus, device, function) ((((bus)&0xff) << 8) | (((device)&0x1f) << 3) | ((function)&0x7))

/* Device index helper macros */
#define USP_IDX(tlp_ctx) 0
#define FIRST_DSP_IDX(tlp_ctx) MAX_NUM_USP
#define FIRST_PF_IDX(tlp_ctx) ((tlp_ctx)->num_bridges)
#define DUMMY_DEV_IDX(tlp_ctx) ((tlp_ctx)->num_bridges + (tlp_ctx)->num_ep)

/* BDF map configuration */
#define BDF_MAP_SIZE 256 /* Simple hash table size for BDF lookup */

/* Vendor/Device ID settings */
#define TLP_PCI_TYPE_VENDOR_ID 0x1af4		/* Red Hat, Inc. */
#define TLP_PCI_TYPE_ENDPOINT_DEVICE_ID 0x10f0	/* Endpoint device ID */
#define TLP_PCI_TYPE_BRIDGE_DEVICE_ID 0x10f1	/* Bridge device ID */
#define TLP_PCI_TYPE_SUBSYSTEM_VENDOR_ID 0x1af4 /* Red Hat, Inc. */
#define TLP_PCI_TYPE_SUBSYSTEM_ID 0x10f0	/* Subsystem ID */
#define TLP_PCI_TYPE_REVISION_ID 0

/* Class codes */
#define TLP_PCI_CLASS_CODE_ENDPOINT 0x020000 /* Network controller */
#define TLP_PCI_CLASS_CODE_BRIDGE 0x060400   /* PCI-to-PCI Bridge */

/* Header types */
#define HEADER_TYPE_ENDPOINT 0x00		/* Type 0 (endpoint) - single function */
#define HEADER_TYPE_ENDPOINT_MULTIFUNCTION 0x80 /* Type 0 (endpoint) + multi-function */
#define HEADER_TYPE_BRIDGE 0x01			/* Type 1 (bridge) - single function */
#define HEADER_TYPE_BRIDGE_MULTIFUNCTION 0x81	/* Type 1 (bridge) + multi-function */

/* Bar setting */
#define BAR_ENCODING_MEM_SPACE 0
#define BAR_MEM_TYPE_64_BIT 4
#define BAR_MEM_PREFETCHABLE 8
#define LOG_BAR_SIZE_16K 14 /* 2^14 = 16KB */
#define BAR_SIZE_16K (1 << LOG_BAR_SIZE_16K)
#define LOG_BAR_SIZE_32K 15 /* 2^15 = 32KB - matches VNET_VIRTIO_BAR_LOG_SIZE */
#define BAR_SIZE_32K (1 << LOG_BAR_SIZE_32K)
#define LOG_BAR_SIZE_64K 16
#define BAR_SIZE_64K (1 << LOG_BAR_SIZE_64K)

/* Constants for transaction region */
#define TRANSACTION_REGION_START 0x3000 /* After DB/MSI-X regions in 16KB BAR */
#define TRANSACTION_REGION_SIZE 0x1000	/* 4KB region */

/* TLP completion format definitions */
#define TLP_TYPE_COMPLETION 0x0A /* TLP completion type */
#define TLP_FMT_CPL_NODATA 0x00	 /* Completion with no data */
#define TLP_FMT_CPL_W_DATA 0x02	 /* Completion with data */

/* Constants for MMIO */
#define BYTES_IN_DWORD 4
#define TLP_DATA_ARRAY_MAX_SIZE 256
#define DUMMY_READ_DATA_BASE 0xDEADBEEF

/* Memory Write TLP format values */
#define MEM_WR_FMT_3DW_W_DATA 0x2 /* 3DW header with data (32-bit address) */
#define MEM_WR_FMT_4DW_W_DATA 0x3 /* 4DW header with data (64-bit address) */
#define MEM_WR_TYPE 0x0		  /* Memory Write type */

/* Helper macro to set completion context fields */
#define SET_CMPL_CONTEXT(dev, _req_id, _tag9, _tag8, _tag, _fmt, _status, _length) \
	do { \
		(dev)->requester_id = (_req_id); \
		(dev)->tag9 = (_tag9); \
		(dev)->tag8 = (_tag8); \
		(dev)->tag = (_tag); \
		(dev)->cmpl_fmt = (_fmt); \
		(dev)->cmpl_type = TLP_TYPE_COMPLETION; \
		(dev)->cmpl_status = (_status); \
		(dev)->cmpl_length = (_length); \
	} while (0)

/* BDF map entry for fast device lookup */
struct bdf_map_entry {
	uint32_t key; /* (tlp_type << 16) | bdf - includes TLP type to distinguish Type0/Type1 */
	struct pci_device_config *dev_cfg;
	struct bdf_map_entry *next;
};

/* PCI capability setting */
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

/* PCIe Express Capability - Slot Control bits */
#define SLOT_CTRL_ATTN_BTN_PRESSED_EN 0x0001   /* Attention Button Pressed Enable */
#define SLOT_CTRL_PWR_FAULT_DETECT_EN 0x0002   /* Power Fault Detected Enable */
#define SLOT_CTRL_MRL_SENSOR_CHANGED_EN 0x0004 /* MRL Sensor Changed Enable */
#define SLOT_CTRL_PRESENCE_DETECT_EN 0x0008    /* Presence Detect Changed Enable (PDCE) */
#define SLOT_CTRL_CMD_COMPLETED_INT_EN 0x0010  /* Command Completed Interrupt Enable (CCIE) */
#define SLOT_CTRL_HP_INT_EN 0x0020	       /* Hot-Plug Interrupt Enable (HPIE) */
#define SLOT_CTRL_POWER_CONTROLLER 0x0400      /* Power Controller Control (Bit 10: 0=ON, 1=OFF) */
#define SLOT_CTRL_DL_STATE_CHANGED_EN 0x1000   /* Data Link Layer State Changed Enable (DLLSCE) */

/* PCIe Express Capability - Slot Capabilities bits (per PCIe Base Spec) */
#define SLOT_CAP_ATTN_BTN_PRESENT 0x00000001   /* Bit 0: Attention Button Present */
#define SLOT_CAP_PWR_CTRL_PRESENT 0x00000002   /* Bit 1: Power Controller Present */
#define SLOT_CAP_MRL_SENSOR_PRESENT 0x00000004 /* Bit 2: MRL Sensor Present */
#define SLOT_CAP_ATTN_IND_PRESENT 0x00000008   /* Bit 3: Attention Indicator Present */
#define SLOT_CAP_PWR_IND_PRESENT 0x00000010    /* Bit 4: Power Indicator Present */
#define SLOT_CAP_HP_SURPRISE 0x00000020	       /* Bit 5: Hot-Plug Surprise */
#define SLOT_CAP_HP_CAPABLE 0x00000040	       /* Bit 6: Hot-Plug Capable */
#define SLOT_CAP_PWR_LIMIT_VALUE_SHIFT 7       /* Bits 14:7: Slot Power Limit Value */
#define SLOT_CAP_PWR_LIMIT_SCALE_SHIFT 15      /* Bits 16:15: Slot Power Limit Scale */
#define SLOT_CAP_EMI_PRESENT 0x00020000	       /* Bit 17: Electromechanical Interlock Present */
#define SLOT_CAP_NO_CMD_CMPL 0x00040000	       /* Bit 18: No Command Completed Support */
#define SLOT_CAP_PHYS_SLOT_NUM_SHIFT 19	       /* Bits 31:19: Physical Slot Number */

/* TLP Bridge Slot Capabilities configuration:
 * - Power Controller Present, Hot-Plug Surprise, Hot-Plug Capable
 * - Physical Slot Number = 56, Slot Power Limit Value = 32
 * - NoCompl = 0 (Host will enable CCIE)
 */
#define TLP_BRIDGE_PHYS_SLOT_NUM 56
#define TLP_BRIDGE_SLOT_PWR_LIMIT 32
#define TLP_BRIDGE_SLOT_CAPABILITIES \
	(SLOT_CAP_PWR_CTRL_PRESENT | SLOT_CAP_HP_SURPRISE | SLOT_CAP_HP_CAPABLE | \
	 (TLP_BRIDGE_SLOT_PWR_LIMIT << SLOT_CAP_PWR_LIMIT_VALUE_SHIFT) | \
	 (TLP_BRIDGE_PHYS_SLOT_NUM << SLOT_CAP_PHYS_SLOT_NUM_SHIFT))

/* PCIe Express Capability - Slot Status bits */
#define SLOT_STS_ATTN_BTN_PRESSED 0x0001	/* Attention Button Pressed */
#define SLOT_STS_PWR_FAULT_DETECTED 0x0002	/* Power Fault Detected */
#define SLOT_STS_MRL_SENSOR_CHANGED 0x0004	/* MRL Sensor Changed */
#define SLOT_STS_PRESENCE_DETECT_CHANGED 0x0008 /* Presence Detect Changed (PDC) */
#define SLOT_STS_CMD_COMPLETED 0x0010		/* Command Completed */
#define SLOT_STS_MRL_SENSOR_STATE 0x0020	/* MRL Sensor State */
#define SLOT_STS_PRESENCE_DETECT_STATE 0x0040	/* Presence Detect State */
#define SLOT_STS_DL_STATE_CHANGED 0x0100	/* Data Link Layer State Changed (DLLSC) */

/* PCIe Express Capability - Link Status bits */
#define LINK_STS_DL_ACTIVE 0x2000 /* Data Link Layer Link Active (Bit 13) */

/* Capability lengths in DWORDs */
#define TLP_CAP_PM_LEN_DW 2
#define TLP_CAP_MSI_LEN_DW 6
#define TLP_CAP_VPD_LEN_DW 2
#define TLP_CAP_EXPRESS_LEN_DW 15
#define TLP_CAP_MSIX_LEN_DW 3

/* Capability register numbers (offset / 4) */
#define TLP_CAP_PM_REG_NUM (TLP_PCI_CAP_OFFSET_PM / 4)
#define TLP_CAP_MSI_REG_NUM (TLP_PCI_CAP_OFFSET_MSI / 4)
#define TLP_CAP_VPD_REG_NUM (TLP_PCI_CAP_OFFSET_VPD / 4)
#define TLP_CAP_EXPRESS_REG_NUM (TLP_PCI_CAP_OFFSET_EXPRESS / 4)
#define TLP_CAP_MSIX_REG_NUM (TLP_PCI_CAP_OFFSET_MSIX / 4)

#define TLP_PCI_CAPS_NUM 4

/* Memory address extraction macros for TLP */
#define GET_MEM_ADDR_32BIT(tlp_req) (DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 2), ((uint32_t *)(tlp_req))[2]) << 2)
#define GET_MEM_ADDR_64BIT_HIGH(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 0), ((uint32_t *)(tlp_req))[2])
#define GET_MEM_ADDR_64BIT_LOW(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 2), ((uint32_t *)(tlp_req))[3])

struct pci_cfg_type0_header {
	/* DW 0x0 */
	uint16_t vendor_id;
	uint16_t device_id;
	/* DW 0x1 */
	uint16_t command;
	uint16_t status;
	/* DW 0x2 */
	uint8_t revision_id;
	uint32_t class_code : 24;
	/* DW 0x3 */
	uint8_t cache_line_size;
	uint8_t latency_timer;
	uint8_t header_type;
	uint8_t bist;
	/* DW 0x4 - 0x9 */
	uint32_t bar[6];
	/* DW 0xa */
	uint32_t cardbus_cis_pointer;
	/* DW 0xb */
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_id;
	/* DW 0xc */
	uint32_t exp_rom_base_addr;
	/* DW 0xd */
	uint8_t cap_ptr;
	uint32_t reserved_at_13 : 24;
	/* DW 0xe */
	uint32_t reserved_at_14;
	/* DW 0xf */
	uint8_t interrupt_line;
	uint8_t interrupt_pin;
	uint8_t min_grant;
	uint8_t max_latency;
};

/* PCI configuration space Type 1 header (Bridge) */
struct pci_cfg_type1_header {
	/* DW 0x0 */
	uint16_t vendor_id;
	uint16_t device_id;
	/* DW 0x1 */
	uint16_t command;
	uint16_t status;
	/* DW 0x2 */
	uint8_t revision_id;
	uint32_t class_code : 24;
	/* DW 0x3 */
	uint8_t cache_line_size;
	uint8_t latency_timer;
	uint8_t header_type;
	uint8_t bist;
	/* DW 0x4 - 0x5 */
	uint32_t bar[2];
	/* DW 0x6 */
	uint8_t primary_bus;
	uint8_t secondary_bus;
	uint8_t subordinate_bus;
	uint8_t secondary_latency;
	/* DW 0x7 */
	uint8_t io_base;
	uint8_t io_limit;
	uint16_t secondary_status;
	/* DW 0x8 */
	uint16_t memory_base;
	uint16_t memory_limit;
	/* DW 0x9 */
	uint16_t pre_memory_base;
	uint16_t pre_memory_limit;
	/* DW 0xa */
	uint32_t pre_memory_base_upper_32bit;
	/* DW 0xb */
	uint32_t pre_memory_limit_upper_32bit;
	/* DW 0xc */
	uint16_t io_base_upper_16bit;
	uint16_t io_limit_upper_16bit;
	/* DW 0xd */
	uint8_t cap_ptr;
	uint32_t reserved : 24;
	/* DW 0xe */
	uint32_t exp_rom_base_addr;
	/* DW 0xf */
	uint8_t interrupt_line;
	uint8_t interrupt_pin;
	uint16_t bridge_control;
};

/* PCI Express Capability structure (for endpoints only) */
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

/* PCI Capabilities container (for endpoints and bridges) */
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

/* PCI capability configuration array */
static const struct tlp_cap_conf tlp_pci_cap_confs[TLP_PCI_CAPS_NUM] = {
	{TLP_PCI_CAP_ID_EXPRESS, TLP_PCI_CAP_OFFSET_EXPRESS, TLP_CAP_EXPRESS_LEN_DW * 4},
	{TLP_PCI_CAP_ID_MSIX, TLP_PCI_CAP_OFFSET_MSIX, TLP_CAP_MSIX_LEN_DW * 4},
	{TLP_PCI_CAP_ID_VPD, TLP_PCI_CAP_OFFSET_VPD, TLP_CAP_VPD_LEN_DW * 4},
	{TLP_PCI_CAP_ID_PM, TLP_PCI_CAP_OFFSET_PM, TLP_CAP_PM_LEN_DW * 4},
};

struct pci_device_config {
	union {
		struct pci_cfg_type0_header type0;
		struct pci_cfg_type1_header type1;
	} cfg_space_hdr;

	/* Capabilities (only for endpoints) */
	struct pci_capabilities caps;

	/* Device topology */
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint16_t bdf;

	/* Device type flags */
	bool is_bridge;
	bool is_endpoint;
	bool is_dummy;
	bool is_bdf_set; /* Track if BDF has been set from first TLP */

	/* For endpoint devices */
	uint32_t vhca_id;
	atomic_bool device_present;	   /* Track if device is present in slot */
	atomic_bool pending_unplug;	   /* Pending unplug - wait for host reset before destroy */
	atomic_bool pending_destroy;	   /* Destroy work is queued (prevents duplicate submissions) */
	struct timespec unplug_start_time; /* Timestamp when unplug was initiated (for timeout) */
	/* MSI retry state (used on DSP bridges when ACG credit is temporarily exhausted):
	 * set by trigger_hotplug_event() / trigger_command_completed() when MSI send returns
	 * DOCA_ERROR_AGAIN, drained by run_progress_loop() once credits are replenished. */
	atomic_bool msi_retry_pending;		 /* Slot-event MSI was dropped, retry scheduled */
	atomic_uint_fast64_t msi_retry_since_ns; /* Retry window start time in monotonic ns, 0 if unset */
	pthread_rwlock_t endpoint_lock;		 /* RW-lock protecting tlp_dev/rep during hotplug removal */
	struct doca_dev_rep *rep;		 /* Representor (for both hotplug and static) */
	bool lu_rep_opened;			 /* true if rep was opened (LU) vs created (normal) */
	struct doca_devemu_pci_tlp_dev *tlp_dev; /* TLP device (for both hotplug and static) */

	/* TLP transaction fields */
	uint16_t requester_id;
	uint16_t completer_id;
	uint16_t tag9 : 1;
	uint16_t tag8 : 1;
	uint16_t tag : 8;
	uint8_t req_fmt;
	uint8_t req_type;
	uint8_t cmpl_fmt;
	uint8_t cmpl_type;
	uint16_t cmpl_length;
	uint8_t cmpl_status;
	uint32_t cmpl_data;
};

/* Context structure for managing TLP channel and devices */
struct tlp_context {
	struct doca_dev *dev;
	struct ibv_pd *imported_ibv_pd;	      /* LU: tracks ibv_alloc_pd() for cleanup */
	struct ibv_context *imported_ibv_ctx; /* LU: tracks ibv_import_device() for cleanup */
	struct doca_devemu_pci_type *pci_type;
	struct doca_devemu_pci_tlp_channel *tlp_channel;
	struct doca_pe *pe; /* PE1: Main progress engine for TLP handling */
	struct pci_device_config *devs_config;
	uint32_t num_devices;
	uint32_t num_ep;				    /* Number of endpoints (configurable) */
	uint32_t num_dsp;				    /* Number of DSP bridges (same as num_ep) */
	uint32_t num_bridges;				    /* Total number of bridges (USP + DSP) */
	void **transaction_region_memories;		    /* Array of memory regions (one per PF) */
	size_t transaction_region_size;			    /* Size of each transaction region */
	struct bdf_map_entry *bdf_map[BDF_MAP_SIZE];	    /* Hash table for fast BDF to device lookup */
	struct bdf_map_entry *bdf_entries;		    /* Pre-allocated entries for BDF map */
	struct doca_devemu_pci_tlp_channel_req **acg_queue; /* Circular queue of pending ACG credits */
	pthread_mutex_t acg_queue_lock;			    /* Protects ACG queue head/tail/count */
	uint16_t acg_queue_head;			    /* ACG queue head index */
	uint16_t acg_queue_tail;			    /* ACG queue tail index */
	uint16_t acg_queue_count;			    /* Number of ACG credits in queue */
	uint16_t acg_queue_size;			    /* ACG queue capacity (from device cap) */
	bool hotplug_mode;				    /* Hotplug mode: true for hotplug, false for static */
	struct vnet_pci_device *virtio_dev;
	struct vnet_pci_dev_controller *vnet_controller; /* VNet offload controller */
	uint8_t mac_bytes_base[ETH_ALEN];
	uint16_t mtu;
	uint16_t max_queue_pairs;
	uint16_t queue_size; /* VirtQueue size (entries per queue) */
};

/* ACG queue functions */
doca_error_t acg_queue_push(struct tlp_context *tlp_ctx, struct doca_devemu_pci_tlp_channel_req *acg_req);
void acg_queue_stats_get(struct tlp_context *tlp_ctx, uint16_t *count, uint16_t *size);
struct doca_devemu_pci_tlp_channel_req *acg_queue_pop(struct tlp_context *tlp_ctx);

/* Device index lookup */
doca_error_t get_pf_index_for_device(struct tlp_context *tlp_ctx,
				     struct pci_device_config *dev_cfg,
				     uint32_t *pf_index);

#endif
