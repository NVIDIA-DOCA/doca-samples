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
#include <dirent.h>
#include <poll.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <endian.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <infiniband/verbs.h>

#include <doca_bitfield.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_rdma_bridge.h>

#include <devemu_pci_common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_TLP_HANDLER_DPU::SAMPLE);

#define TLP_PCI_TYPE_NAME "Sample PCI TLP Type"
/* Default size for TLP channel context user data */
#define TLP_CHANNEL_CTX_USER_DATA_SIZE_DEFAULT (128)
#define TLP_COMPLETION_HEADER_DW_SIZE (3)

/* Config Header settings */
#define TLP_PCI_TYPE_DEVICE_ID 0x10f0
#define TLP_PCI_TYPE_VENDOR_ID 0x1af4
#define TLP_PCI_TYPE_SUBSYSTEM_ID 0x10f0
#define TLP_PCI_TYPE_SUBSYSTEM_VENDOR_ID 0x1af4
#define TLP_PCI_TYPE_REVISION_ID 0
#define TLP_PCI_TYPE_CLASS_CODE 0x020000

/* TLP request format definitions */
#define TLP_FMT_3DW_NODATA 0x0 /* 3DW header with no data */
#define TLP_FMT_4DW_NODATA 0x1 /* 4DW header with no data */
#define TLP_FMT_3DW_W_DATA 0x2 /* 3DW header with data */
#define TLP_FMT_4DW_W_DATA 0x3 /* 4DW header with data */

/* TLP request type definitions */
#define TLP_REQ_TYPE_MEMORY_READ_WRITE 0x0	  /* Memory Read/Write */
#define TLP_REQ_TYPE_IO_READ_WRITE 0x2		  /* I/O Read/Write */
#define TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0 0x4 /* Config Read/Write Type 0 */
#define TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1 0x5 /* Config Read/Write Type 1 */

/* TLP completion format definitions */
#define TLP_TYPE_COMPLETION 0x0A /* TLP completion type */
#define TLP_FMT_CPL_NODATA 0x00	 /* Completion with no data */
#define TLP_FMT_CPL_W_DATA 0x02	 /* Completion with data */

/* TLP completion length definitions */
#define TLP_CPL_LENGTH_SINGLE_DWORD 0x01 /* Single DWORD completion length */
#define TLP_CPL_LENGTH_ZERO 0x00	 /* Zero length completion */

/* TLP completion status codes */
#define TLP_CPL_STATUS_SC 0x00	/* Successful Completion */
#define TLP_CPL_STATUS_UR 0x01	/* Unsupported Request */
#define TLP_CPL_STATUS_CRS 0x02 /* Configuration Request Retry Status */
#define TLP_CPL_STATUS_CA 0x04	/* Completion Abort */

/* BAR encoding bit definitions */
#define BAR_ENCODING_MEM_SPACE 0 /* Memory space indicator (bit 0 = 0) */
#define BAR_MEM_TYPE_32_BIT 0	 /* 32-bit memory type (bits 2:1 = 00) */
#define BAR_MEM_TYPE_64_BIT 4	 /* 64-bit memory type (bits 2:1 = 10) */
#define BAR_MEM_PREFETCHABLE 8	 /* Prefetchable memory (bit 3 = 1) */
#define BAR_MEM_NON_PREFETCH 0	 /* Non-prefetchable memory (bit 3 = 0) */
#define BAR_ENCODING_IO_SPACE 1	 /* I/O space indicator (bit 0 = 1) */

/* Number of BARs in Type 0 configuration space header */
#define TYPE_0_CFG_SPACE_HEADER_BAR_NUM 6

/* TLP header field extraction macros */
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

/* Command register read-write bit definitions */
#define COMMAND_RW_MEM_SPACE_ENABLE 0x0002  /* Memory space enable (bit 1) */
#define COMMAND_RW_BUS_MASTER_ENABLE 0x0004 /* Bus master enable (bit 2) */
#define COMMAND_RW_PERR_ENABLE 0x0040	    /* Parity error response enable (bit 6) */
#define COMMAND_RW_SERR_ENABLE 0x0100	    /* SERR# enable (bit 8) */
#define COMMAND_RW_INT_DISABLE 0x0400	    /* Interrupt disable (bit 10) */

/* Status register write-1-to-clear bit definitions */
#define STATUS_WR1C_MASTER_DATA_PERR 0x0100 /* Master data parity error (bit 8) */
#define STATUS_WR1C_SIGNALED_TA 0x0800	    /* Signaled target abort (bit 11) */
#define STATUS_WR1C_RECEIVE_TA 0x1000	    /* Received target abort (bit 12) */
#define STATUS_WR1C_RECEIVE_MA 0x2000	    /* Received master abort (bit 13) */
#define STATUS_WR1C_SIGNALED_SERR 0x4000    /* Signaled system error (bit 14) */
#define STATUS_WR1C_DETECTED_PERR 0x8000    /* Detected parity error (bit 15) */

/* PCI capability setting */
#define TLP_PCI_CAP_ID_PM 0x01
#define TLP_PCI_CAP_ID_VPD 0x03
#define TLP_PCI_CAP_ID_EXPRESS 0x10
#define TLP_PCI_CAP_ID_MSIX 0x11

#define TLP_PCI_CAP_OFFSET_PM 0x40
#define TLP_PCI_CAP_OFFSET_VPD 0x48
#define TLP_PCI_CAP_OFFSET_EXPRESS 0x60
#define TLP_PCI_CAP_OFFSET_MSIX 0x9c

#define TLP_PCI_CAP_FIRST_CAP_OFFSET TLP_PCI_CAP_OFFSET_EXPRESS
#define TLP_PCI_CAP_NEXT_OF_EXPRESS TLP_PCI_CAP_OFFSET_VPD
#define TLP_PCI_CAP_NEXT_OF_VPD TLP_PCI_CAP_OFFSET_MSIX
#define TLP_PCI_CAP_NEXT_OF_MSIX TLP_PCI_CAP_OFFSET_PM
#define TLP_PCI_CAP_NEXT_OF_PM 0x00

/* PCIe capability setting */
#define TLP_PCIE_CAP_FIRST_CAP_OFFSET 0x100 /* First PCIe cap is at fixed offset 0x100 */
#define TLP_PCIE_CAP_ID_AER 0x01
#define TLP_PCIE_CAP_OFFSET_AER TLP_PCIE_CAP_FIRST_CAP_OFFSET
#define TLP_PCIE_CAP_NEXT_CAP_AER 0x00

/* SR-IOV Extended Capability (PF only). Placed right after the AER cap in the PCIe ext-cap chain.
 * The SR-IOV ext cap ID is 0010h and its structure is 16 DWORDs long. */
#define TLP_PCIE_CAP_ID_SRIOV 0x0010
#define TLP_PCIE_CAP_OFFSET_SRIOV 0x148 /* AER (0x100) + 18 DWORDs */
#define TLP_PCIE_CAP_NEXT_CAP_SRIOV 0x00
#define SRIOV_CAP_VERSION 0x01

/* SR-IOV Control register (offset 08h) bits */
#define SRIOV_CTRL_VF_ENABLE 0x0001 /* bit 0: VF Enable - VFs become accessible when Set */
#define SRIOV_CTRL_VF_MSE 0x0008    /* bit 3: VF Memory Space Enable - gates VF MMIO */

/* Fixed SR-IOV layout used by the sample (kept simple: contiguous functions, no ARI needed) */
#define SRIOV_FIRST_VF_OFFSET 1		      /* VF1 routing ID = PF routing ID + 1 */
#define SRIOV_VF_STRIDE 1		      /* consecutive VF routing IDs */
#define SRIOV_SUPPORTED_PAGE_SIZES 0x00000001 /* support 4KB pages (bit 0 => 2^12) */
#define VF_PCI_TYPE_DEVICE_ID 0x10f1	      /* device ID reported by VFs (SR-IOV VF Device ID) */

/* Limit total VFs to 7 so that all functions (PF=0, VFs=1..7) fit in the 3-bit function number
 * of a non-ARI device, avoiding the need to implement the ARI capability. */
#define TLP_MAX_TOTAL_VFS 7

/* PCI cap and ext_cap pointer and length */
#define BYTES_IN_DWORD (4)
#define TLP_CAP_PM_REG_NUM (TLP_PCI_CAP_OFFSET_PM / BYTES_IN_DWORD)	      /* reg_num: 16 */
#define TLP_CAP_VPD_REG_NUM (TLP_PCI_CAP_OFFSET_VPD / BYTES_IN_DWORD)	      /* reg_num: 18 */
#define TLP_CAP_EXPRESS_REG_NUM (TLP_PCI_CAP_OFFSET_EXPRESS / BYTES_IN_DWORD) /* reg_num: 24 */
#define TLP_CAP_MSIX_REG_NUM (TLP_PCI_CAP_OFFSET_MSIX / BYTES_IN_DWORD)	      /* reg_num: 39 */
#define TLP_CAP_AER_REG_NUM (TLP_PCIE_CAP_OFFSET_AER / BYTES_IN_DWORD)	      /* reg_num: 64 */
#define TLP_CAP_SRIOV_REG_NUM (TLP_PCIE_CAP_OFFSET_SRIOV / BYTES_IN_DWORD)    /* reg_num: 82 */

#define TLP_CAP_PM_LEN_DW 2	  /* 2 DWORD */
#define TLP_CAP_VPD_LEN_DW 2	  /* 2 DWORD */
#define TLP_CAP_MSIX_LEN_DW 3	  /* 3 DWORDS */
#define TLP_CAP_EXPRESS_LEN_DW 15 /* 15 DWORDS */
#define TLP_CAP_AER_LEN_DW 18	  /* 18 DWORDS */
#define TLP_CAP_SRIOV_LEN_DW 16	  /* 16 DWORDS (SR-IOV ext cap structure size) */

#define TLP_CAP_PM_STATE 0x0003
#define TLP_CAP_PM_PME_EN 0x0100
#define TLP_CAP_PM_DATA_SEL 0x1e00
#define TLP_CAP_PM_PME_STATUS 0x8000

#define TLP_PCI_CAPS_NUM 4  /* number of pci caps used in the sample */
#define TLP_PCIE_CAPS_NUM 2 /* number of pcie caps used in the sample */

#define TLP_TRANSACTION_CMPL_DATA_MAX_DW 5 /* Maximum dwords in transaction completion data array in the sample */

/* Expansion ROM bar definitions */
#define LOG_EXP_BAR_SIZE 16		     /* Log2 of expansion ROM bar size to set */
#define EXP_BAR_SIZE (1 << LOG_EXP_BAR_SIZE) /* Expansion ROM bar size */
#define EXP_BAR_ADDR_MASK 0xFFFFF800	     /* Expansion ROM bar address mask */
#define EXP_BAR_ENABLED_BIT_SET 0x00000001   /* Enabled bit set to 1 (enabled) */
#define EXP_BAR_INIT_PATTERN 0xBB	     /* Expansion ROM bar memory initialization pattern */

/* TLP data array size limits */
#define TLP_DATA_ARRAY_MAX_SIZE 16 /* Maximum size of TLP data array */

/* Transaction region definitions */
#define TRANSACTION_REGION_INIT_PATTERN 0xAA /* Transaction region memory initialization pattern */

/* Test data patterns */
#define DUMMY_READ_DATA_BASE 0xDEADBEE0 /* Base pattern for dummy read data */

/* Byte enable masks */
#define BYTE_ENABLE_ALL_MASK 0xF /* Full byte enable mask (all 4 bytes) */

/* TLP data offsets */
#define TLP_32BIT_ADDR_DATA_OFFSET_DW 3 /* Data DWORD offset in TLP for 32-bit addressing */
#define TLP_64BIT_ADDR_DATA_OFFSET_DW 4 /* Data DWORD offset in TLP for 64-bit addressing */

/* Common numeric constants */
#define DWORD_LOG_SIZE_BITS 3 /* Bit shift for DWORD size (1 << 3 = 8 bits per byte, 4 bytes per DWORD) */

/* PCIe capability version */
#define AER_CAP_VERSION 0x01 /* AER capability version */

/* Address mask */
#define DWORD_ALIGN_MASK 0xFFFFFFFC /* Mask to align address to DWORD boundary */

/* Socket configuration - Unix domain socket for SCM_RIGHTS (fd passing) */
#define TLP_CHANNEL_HANDOVER_PORT_DEFAULT 2000
#define TLP_CHANNEL_HANDOVER_SOCKET_PATH_MAX 64
#define TLP_CHANNEL_HANDOVER_SOCKET_PATH_FMT "/dev/shm/doca_tlp_oob_%u"

/* Forward declarations to allow usage before definition */
static void tlp_channel_event_fatal_state_handler_cb(struct doca_devemu_pci_tlp_channel *channel,
						     union doca_data user_data);

/* PCIe set cap conf */
struct tlp_cap_conf {
	uint8_t id;
	uint16_t offset;
	uint16_t length;
};

struct tlp_cap_conf tlp_pci_cap_confs[TLP_PCI_CAPS_NUM] = {
	{TLP_PCI_CAP_ID_EXPRESS, TLP_PCI_CAP_OFFSET_EXPRESS, TLP_CAP_EXPRESS_LEN_DW * 4},
	{TLP_PCI_CAP_ID_MSIX, TLP_PCI_CAP_OFFSET_MSIX, TLP_CAP_MSIX_LEN_DW * 4},
	{TLP_PCI_CAP_ID_VPD, TLP_PCI_CAP_OFFSET_VPD, TLP_CAP_VPD_LEN_DW * 4},
	{TLP_PCI_CAP_ID_PM, TLP_PCI_CAP_OFFSET_PM, TLP_CAP_PM_LEN_DW * 4},
};

struct tlp_cap_conf tlp_pcie_cap_confs[TLP_PCIE_CAPS_NUM] = {
	{TLP_PCIE_CAP_ID_AER, TLP_PCIE_CAP_OFFSET_AER, TLP_CAP_AER_LEN_DW * 4},
	{TLP_PCIE_CAP_ID_SRIOV, TLP_PCIE_CAP_OFFSET_SRIOV, TLP_CAP_SRIOV_LEN_DW * 4},
};

/**
 * @brief PCI configuration space header register 00h union
 *
 * Contains vendor ID [15:0] and device ID [31:16] fields
 */
union pci_config_space_header_reg0 {
	struct {
		uint16_t vendor_id;
		uint16_t device_id;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header register 01h union
 *
 * Contains command [15:0] and status [31:16] fields
 */
union pci_config_space_header_reg1 {
	struct {
		uint16_t command;
		uint16_t status;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header register 02h union
 *
 * Contains revision ID [7:0] and class code [31:8] fields
 */
union pci_config_space_header_reg2 {
	struct {
		uint8_t revision_id;
		uint32_t class_code : 24;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header register 03h union
 *
 * Contains cache line size [7:0], latency timer [15:8], header type [23:16], and BIST [31:24] fields
 */
union pci_config_space_header_reg3 {
	struct {
		uint8_t cache_line_size;
		uint8_t latency_timer;
		uint8_t header_type;
		uint8_t bist;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header register 0Bh union
 *
 * Contains subsystem vendor ID [15:0] and subsystem ID [31:16] fields
 */
union pci_config_space_header_regB {
	struct {
		uint16_t subsystem_vendor_id;
		uint16_t subsystem_id;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header register 0Dh union
 *
 * Contains capabilities pointer [7:0] and reserved [31:8] fields
 */
union pci_config_space_header_regD {
	struct {
		uint8_t capabilities_pointer;
		uint8_t reserved[3];
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header register 0Fh union
 *
 * Contains interrupt line [7:0], interrupt pin [15:8], min grant [23:16], and max latency [31:24] fields
 */
union pci_config_space_header_regF {
	struct {
		uint8_t interrupt_line;
		uint8_t interrupt_pin;
		uint8_t min_gnt;
		uint8_t max_lat;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space header structure
 *
 * Represents the standard PCI Type 0 configuration space header
 */
struct pci_config_space_header {
	union pci_config_space_header_reg0 reg0;
	union pci_config_space_header_reg1 reg1;
	union pci_config_space_header_reg2 reg2;
	union pci_config_space_header_reg3 reg3;

	uint32_t bar[TYPE_0_CFG_SPACE_HEADER_BAR_NUM];

	uint32_t cardbus_cis_pointer;
	union pci_config_space_header_regB regB;
	uint32_t expansion_rom_base_address;
	union pci_config_space_header_regD regD;
	uint32_t reserved;
	union pci_config_space_header_regF regF;
};

/**
 * @brief PCI configuration capability PCI express register 00h union
 */
union pci_config_space_cap_express_reg0 {
	struct {
		uint8_t cap_id;
		uint8_t next_cap_ptr;
		uint16_t pcie_register;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 02h union
 */
union pci_config_space_cap_express_reg2 {
	struct {
		uint16_t dev_control;
		uint16_t dev_status;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 04h union
 */
union pci_config_space_cap_express_reg4 {
	struct {
		uint16_t link_control;
		uint16_t link_status;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 06h union
 */
union pci_config_space_cap_express_reg6 {
	struct {
		uint16_t slot_control;
		uint16_t slot_status;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 07h union
 */
union pci_config_space_cap_express_reg7 {
	struct {
		uint16_t root_control;
		uint16_t root_cap;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 0Ah union
 */
union pci_config_space_cap_express_regA {
	struct {
		uint16_t dev_control2;
		uint16_t dev_status2;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 0Ch union
 */
union pci_config_space_cap_express_regC {
	struct {
		uint16_t link_control2;
		uint16_t link_status2;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PCI express register 0Eh union
 */
union pci_config_space_cap_express_regE {
	struct {
		uint16_t slot_control2;
		uint16_t slot_status2;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space PCI express capability structure
 */
struct pci_config_space_cap_express {
	union pci_config_space_cap_express_reg0 reg0;
	uint32_t cap_register;
	union pci_config_space_cap_express_reg2 reg2;
	uint32_t link_cap;
	union pci_config_space_cap_express_reg4 reg4;
	uint32_t slot_cap;

	union pci_config_space_cap_express_reg6 reg6;
	union pci_config_space_cap_express_reg7 reg7;
	uint32_t root_status;
	uint32_t dev_cap2;
	union pci_config_space_cap_express_regA regA;
	uint32_t link_cap2;

	union pci_config_space_cap_express_regC regC;
	uint32_t slot_cap2;
	union pci_config_space_cap_express_regE regE;
};

/**
 * @brief PCI configuration capability MSIX register 0Eh union
 */
union pci_config_space_cap_msix_reg0 {
	struct {
		uint8_t cap_id;
		uint8_t next_cap_ptr;
		uint16_t msg_control;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability MSIX register 01h union
 */
union pci_config_space_cap_msix_reg1 {
	struct {
		uint32_t tbl_bir : 3;
		uint32_t tbl_offset : 29;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability MSIX register 02h union
 */
union pci_config_space_cap_msix_reg2 {
	struct {
		uint32_t pba_bir : 3;
		uint32_t pba_offset : 29;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space MSIX capability structure
 */
struct pci_config_space_cap_msix {
	union pci_config_space_cap_msix_reg0 reg0;
	union pci_config_space_cap_msix_reg1 reg1;
	union pci_config_space_cap_msix_reg2 reg2;
};

/**
 * @brief PCI configuration capability VPD register 00h union
 */
union pci_config_space_cap_vpd_reg0 {
	struct {
		uint8_t cap_id;
		uint8_t next_cap_ptr;
		uint16_t addr_register;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space VPD capability structure
 */
struct pci_config_space_cap_vpd {
	union pci_config_space_cap_vpd_reg0 reg0;
	uint32_t msg_addr;
};

/**
 * @brief PCI configuration capability PM register 00h union
 */
union pci_config_space_cap_pm_reg0 {
	struct {
		uint8_t cap_id;
		uint8_t next_cap_ptr;
		uint16_t pmc;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration capability PM register 01h union
 */
union pci_config_space_cap_pm_reg1 {
	struct {
		uint16_t pmcsr;
		uint8_t reserved;
		uint8_t data;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space PM capability structure
 */
struct pci_config_space_cap_pm {
	union pci_config_space_cap_pm_reg0 reg0;
	union pci_config_space_cap_pm_reg1 reg1;
};

/**
 * @brief PCI configuration extend capability AER register 01h union
 */
union pci_config_space_ext_cap_aer_reg0 {
	struct {
		uint16_t cap_id;
		uint16_t cap_version : 4;
		uint16_t next_cap_offset : 12;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space extend capability AER structure
 */
struct pci_config_space_ext_cap_aer {
	union pci_config_space_ext_cap_aer_reg0 reg0;
	uint32_t regs[TLP_CAP_AER_LEN_DW - 1];
};

/**
 * @brief SR-IOV ext cap register 00h: Extended Capability Header
 */
union pci_config_space_ext_cap_sriov_reg0 {
	struct {
		uint16_t cap_id;
		uint16_t cap_version : 4;
		uint16_t next_cap_offset : 12;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief SR-IOV ext cap register 08h: SR-IOV Control [15:0] and SR-IOV Status [31:16]
 */
union pci_config_space_ext_cap_sriov_reg2 {
	struct {
		uint16_t control;
		uint16_t status;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief SR-IOV ext cap register 0Ch: InitialVFs [15:0] and TotalVFs [31:16]
 */
union pci_config_space_ext_cap_sriov_reg3 {
	struct {
		uint16_t initial_vfs;
		uint16_t total_vfs;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief SR-IOV ext cap register 10h: NumVFs [15:0] and Function Dependency Link [23:16]
 */
union pci_config_space_ext_cap_sriov_reg4 {
	struct {
		uint16_t num_vfs;
		uint8_t fcn_dep_link;
		uint8_t reserved;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief SR-IOV ext cap register 14h: First VF Offset [15:0] and VF Stride [31:16]
 */
union pci_config_space_ext_cap_sriov_reg5 {
	struct {
		uint16_t first_vf_offset;
		uint16_t vf_stride;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief SR-IOV ext cap register 18h: VF Device ID [31:16]
 */
union pci_config_space_ext_cap_sriov_reg6 {
	struct {
		uint16_t reserved;
		uint16_t vf_device_id;
	} fields;
	uint32_t as_dword;
};

/**
 * @brief PCI configuration space extended capability SR-IOV structure (PF only)
 *
 * Mirrors the SR-IOV Extended Capability register layout.
 */
struct pci_config_space_ext_cap_sriov {
	union pci_config_space_ext_cap_sriov_reg0 reg0;	  /* 00h: ext cap header */
	uint32_t cap_register;				  /* 04h: SR-IOV Capabilities (RO) */
	union pci_config_space_ext_cap_sriov_reg2 reg2;	  /* 08h: Control / Status */
	union pci_config_space_ext_cap_sriov_reg3 reg3;	  /* 0Ch: InitialVFs / TotalVFs */
	union pci_config_space_ext_cap_sriov_reg4 reg4;	  /* 10h: NumVFs / Fcn Dep Link */
	union pci_config_space_ext_cap_sriov_reg5 reg5;	  /* 14h: First VF Offset / VF Stride */
	union pci_config_space_ext_cap_sriov_reg6 reg6;	  /* 18h: VF Device ID */
	uint32_t supported_page_sizes;			  /* 1Ch: Supported Page Sizes (RO) */
	uint32_t system_page_size;			  /* 20h: System Page Size (RW) */
	uint32_t vf_bar[TYPE_0_CFG_SPACE_HEADER_BAR_NUM]; /* 24h-38h: VF BAR0..VF BAR5 (RW) */
};

/**
 * @brief PCI configuration space capabilities structure
 */
struct pci_config_space_caps {
	struct pci_config_space_cap_pm pm_cap;
	struct pci_config_space_cap_vpd vpd_cap;
	struct pci_config_space_cap_msix msix_cap;
	struct pci_config_space_cap_express express_cap;
};

/**
 * @brief PCI configuration space extended capabilities structure
 */
struct pci_config_space_ext_caps {
	struct pci_config_space_ext_cap_aer aer_ext_cap;
	struct pci_config_space_ext_cap_sriov sriov_ext_cap; /* Populated/used for the PF only */
};

/**
 * @brief PCI configuration space structure
 *
 * Complete PCI configuration space representation
 */
struct pci_config_space {
	struct pci_config_space_header header;
	struct pci_config_space_caps caps;
	struct pci_config_space_ext_caps ext_caps;
};

/**
 * @brief TLP handler context structure
 *
 * Contains all context information needed for TLP request handling
 */
struct tlp_handler_cxt {
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint16_t bdf;

	uint16_t requester_id;
	uint16_t completer_id;

	uint8_t req_fmt;
	uint8_t req_type;

	uint8_t cmpl_fmt;
	uint8_t cmpl_type;
	uint8_t cmpl_length;
	uint8_t cmpl_status;

	uint16_t tag9 : 1;
	uint16_t tag8 : 1;
	uint16_t tag : 8;

	uint16_t cfg_space_section : 4;
	uint16_t capability_offset : 12;
	uint16_t capability_id;
	uint8_t first_dw_be;

	uint64_t bar_region_size;
	uint32_t exp_bar_size;

	void *transaction_region_memory;
	size_t transaction_region_size;

	void *exp_bar_memory;
};

/**
 * @brief Per-function emulation state (one PF, plus one per VF when SR-IOV is enabled)
 *
 * All emulated functions (the PF and its VFs) share a single PCIe endpoint and a single TLP
 * channel, but each function has its own representor, TLP device and configuration space. The
 * SR-IOV capability is only present in the PF's configuration space (func_type == PF).
 */
struct devemu_pci_func {
	struct doca_dev_rep *rep;		  /* The function's representor */
	struct doca_devemu_pci_tlp_dev *tlp_dev;  /* The function's TLP device */
	enum doca_pci_func_type func_type;	  /* PF or VF */
	uint16_t vf_index;			  /* 0-based VF index (valid only for VFs) */
	struct pci_config_space pci_config_space; /* The function's PCI configuration space */
	struct tlp_handler_cxt tlp_handler_cxt;	  /* The function's TLP handling context */
};

/**
 * @brief Device emulation TLP resources structure
 *
 * Contains all resources and context needed for TLP device emulation
 */
struct devemu_tlp_resources {
	struct doca_pe *pe;
	struct doca_dev *dev;
	struct doca_dev *channel_dev;
	struct doca_devemu_pci_type *pci_type;
	struct doca_devemu_pci_tlp_channel *tlp_channel;
	struct doca_ctx *channel_ctx;
	const char *shm_dir_path;
	int32_t src_socket;
	int32_t src_listen_socket;
	int32_t dst_socket;
	bool is_channel_owner;
	uint8_t is_dsp_bdf_supported;

	/* Verbs resources used by the destination channel */
	struct ibv_pd *ibv_pd;
	struct ibv_context *ibv_ctx;

	/* Emulated functions sharing the single PCIe endpoint */
	struct devemu_pci_func pf;		      /* The physical function (always present) */
	struct devemu_pci_func vf[TLP_MAX_TOTAL_VFS]; /* The virtual functions (when SR-IOV enabled) */
	uint16_t total_vfs;			      /* Number of VFs to expose (0 => SR-IOV disabled) */

	/* The function currently being serviced by the TLP request handler. Points at 'pf' at all
	 * times except while a per-function TLP is being routed and handled. Letting the existing
	 * per-function handlers operate on 'cur_func' keeps them function-agnostic. */
	struct devemu_pci_func *cur_func;
};

/* All IPC messages start with a value of this enum */
enum tlp_channel_handover_msg {
	TLP_CHANNEL_HANDOVER_MSG_INVALID = 0,
	TLP_CHANNEL_HANDOVER_MSG_SETUP,
	TLP_CHANNEL_HANDOVER_MSG_SETUP_ACK,  /*
					     {
						     int cmd_fd;
						     uint16_t total_vfs;
					     }
					     */
	TLP_CHANNEL_HANDOVER_MSG_SETUP_NACK, /*
					     {
						     doca_error_t error;
					     }
					     */
	TLP_CHANNEL_HANDOVER_MSG_EXPORT,
	TLP_CHANNEL_HANDOVER_MSG_EXPORT_ACK,  /*
					      {
						      size_t export_desc_len;
						      uint8_t export_desc[export_desc_len];
					      }
					      */
	TLP_CHANNEL_HANDOVER_MSG_EXPORT_NACK, /*
					      {
						      doca_error_t error;
					      }
					      */
	TLP_CHANNEL_HANDOVER_MSG_BEGIN,
	TLP_CHANNEL_HANDOVER_MSG_BEGIN_ACK,  /*
					     {
						     // PF state:
						     struct pci_config_space pci_config_space;
						     struct tlp_handler_cxt tlp_handler_cxt;
						     uint8_t transaction_region[PCI_TYPE_MAX_TRANSACTION_REGION_SIZE];
						     uint8_t exp_bar_memory[EXP_BAR_SIZE];
						     // then, repeated total_vfs times, each VF state (no exp ROM BAR):
						     struct pci_config_space pci_config_space;
						     struct tlp_handler_cxt tlp_handler_cxt;
						     uint8_t transaction_region[PCI_TYPE_MAX_TRANSACTION_REGION_SIZE];
					     }
					     */
	TLP_CHANNEL_HANDOVER_MSG_BEGIN_NACK, /*
					     {
						     doca_error_t error;
					     }
					     */
	TLP_CHANNEL_HANDOVER_MSG_END,
	TLP_CHANNEL_HANDOVER_MSG_END_ACK, /*
					  {
						  uint8_t is_handover_successful;
					  }
					  */
};

/**
 * @brief TLP request type enumeration
 *
 * Defines all supported TLP request types
 */
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

static volatile bool force_quit; /* Shared variable to allow for a proper shutdown */

/**
 * Signal handler
 *
 * @signum [in]: Signal number to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/**
 * Send data to a Unix domain socket
 *
 * @socket_fd [in]: Unix domain socket
 * @buffer [in]: Buffer to send the data
 * @length [in]: Length of the data to send
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t lu_oob_send(int socket_fd, const void *buffer, size_t length)
{
	const uint8_t *data = (const uint8_t *)buffer;
	size_t sent_total = 0;

	while (sent_total < length) {
		ssize_t sent = send(socket_fd, data + sent_total, length - sent_total, 0);
		if (sent < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			DOCA_LOG_ERR("OOB socket: failed to send: %s", strerror(errno));
			return DOCA_ERROR_IO_FAILED;
		}
		sent_total += (size_t)sent;
	}

	return DOCA_SUCCESS;
}

/**
 * Receive data from a Unix domain socket
 *
 * @socket_fd [in]: Unix domain socket
 * @buffer [out]: Buffer to store the received data
 * @length [in]: Length of the data to receive
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t lu_oob_recv(int socket_fd, void *buffer, size_t length)
{
	uint8_t *data = (uint8_t *)buffer;
	size_t recv_total = 0;

	while (recv_total < length) {
		ssize_t recv_len = recv(socket_fd, data + recv_total, length - recv_total, 0);
		if (recv_len < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return DOCA_ERROR_TIME_OUT;
			DOCA_LOG_ERR("OOB socket: failed to recv: %s", strerror(errno));
			return DOCA_ERROR_IO_FAILED;
		}
		if (recv_len == 0) {
			DOCA_LOG_ERR("OOB socket: connection closed");
			return DOCA_ERROR_NOT_CONNECTED;
		}
		recv_total += (size_t)recv_len;
	}

	return DOCA_SUCCESS;
}

/**
 * Initialize the TLP Live upgrade OOB server for the source process
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t tlp_lu_oob_server_init(struct devemu_tlp_resources *resources)
{
	struct sockaddr_un server_addr = {0};
	doca_error_t result;
	int socket_desc;
	int enable = 1;
	int name_len;

	socket_desc = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_desc < 0) {
		DOCA_LOG_ERR("OOB socket: error while creating socket: %s", strerror(errno));
		return DOCA_ERROR_DRIVER;
	}

	if (setsockopt(socket_desc, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
		DOCA_LOG_ERR("OOB socket: error setting socket options: %s", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto close_socket;
	}

	server_addr.sun_family = AF_UNIX;
	name_len = snprintf(server_addr.sun_path,
			    sizeof(server_addr.sun_path),
			    TLP_CHANNEL_HANDOVER_SOCKET_PATH_FMT,
			    (unsigned int)TLP_CHANNEL_HANDOVER_PORT_DEFAULT);
	if (name_len < 0 || name_len >= (int)sizeof(server_addr.sun_path)) {
		DOCA_LOG_ERR("OOB socket: path too long");
		result = DOCA_ERROR_INVALID_VALUE;
		goto close_socket;
	}

	(void)unlink(server_addr.sun_path);

	if (bind(socket_desc, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		DOCA_LOG_ERR("OOB socket: couldn't bind to %s: %s", server_addr.sun_path, strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto close_socket;
	}

	if (listen(socket_desc, 1) < 0) {
		DOCA_LOG_ERR("OOB socket: error while listening: %s", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto unlink_socket;
	}

	if (fcntl(socket_desc, F_SETFL, O_NONBLOCK) != 0) {
		DOCA_LOG_ERR("OOB socket: failed to set listen socket non-blocking: %s", strerror(errno));
		result = DOCA_ERROR_IO_FAILED;
		goto unlink_socket;
	}

	resources->src_listen_socket = socket_desc;

	return DOCA_SUCCESS;

unlink_socket:
	(void)unlink(server_addr.sun_path);
close_socket:
	(void)close(socket_desc);
	return result;
}

/**
 * Cleanup the TLP Live upgrade OOB server for the source process
 *
 * @resources [in]: Pointer to TLP resources structure
 * @skip_unlink [in]: Whether to skip unlinking the socket path
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static void tlp_lu_oob_server_cleanup(struct devemu_tlp_resources *resources, bool skip_unlink)
{
	(void)close(resources->src_listen_socket);
	resources->src_listen_socket = -1;

	if (!skip_unlink) {
		char socket_path[TLP_CHANNEL_HANDOVER_SOCKET_PATH_MAX];
		(void)snprintf(socket_path,
			       sizeof(socket_path),
			       TLP_CHANNEL_HANDOVER_SOCKET_PATH_FMT,
			       (unsigned int)TLP_CHANNEL_HANDOVER_PORT_DEFAULT);
		(void)unlink(socket_path);
	}
}

/**
 * Connect to the TLP OOB server for the destination process
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t tlp_oob_client_connect(struct devemu_tlp_resources *resources)
{
	struct sockaddr_un server_addr = {0};
	int socket_desc;
	int name_len;

	socket_desc = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_desc < 0) {
		DOCA_LOG_ERR("OOB socket: unable to create socket: %s", strerror(errno));
		return DOCA_ERROR_DRIVER;
	}

	/* Set socket timeout to 2 seconds */
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	if (setsockopt(socket_desc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
		DOCA_LOG_ERR("OOB socket: error setting socket options: %s", strerror(errno));
		(void)close(socket_desc);
		return DOCA_ERROR_IO_FAILED;
	}

	server_addr.sun_family = AF_UNIX;
	name_len = snprintf(server_addr.sun_path,
			    sizeof(server_addr.sun_path),
			    TLP_CHANNEL_HANDOVER_SOCKET_PATH_FMT,
			    (unsigned int)TLP_CHANNEL_HANDOVER_PORT_DEFAULT);
	if (name_len < 0 || name_len >= (int)sizeof(server_addr.sun_path)) {
		DOCA_LOG_ERR("OOB socket: path too long");
		(void)close(socket_desc);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (connect(socket_desc, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		DOCA_LOG_ERR("OOB socket: unable to connect to %s: %s", server_addr.sun_path, strerror(errno));
		(void)close(socket_desc);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	resources->dst_socket = socket_desc;

	return DOCA_SUCCESS;
}

/**
 * Send an FD via SCM_RIGHTS over a Unix domain socket.
 *
 * @socket_fd [in]: Unix domain socket
 * @fd_to_send [in]: FD to pass to the receiver
 * @return DOCA_SUCCESS on success
 */
static doca_error_t lu_oob_send_fd(int socket_fd, int fd_to_send)
{
	char dummy = 0;
	struct iovec iov = {
		.iov_base = &dummy,
		.iov_len = sizeof(dummy),
	};
	char control_buf[CMSG_SPACE(sizeof(int))];
	struct msghdr msgh = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control_buf,
		.msg_controllen = sizeof(control_buf),
		.msg_flags = 0,
	};

	memset(control_buf, 0, sizeof(control_buf));
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(fd_to_send));

	if (sendmsg(socket_fd, &msgh, 0) < 0) {
		DOCA_LOG_ERR("OOB socket: failed to send fd: %s", strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}

	return DOCA_SUCCESS;
}

/**
 * Receive an FD via SCM_RIGHTS over a Unix domain socket.
 *
 * @socket_fd [in]: Unix domain socket
 * @fd_out [out]: Received FD on success; otherwise, set to -1
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t lu_oob_recv_fd(int socket_fd, int *fd_out)
{
	char buf = 0;
	struct iovec iov = {.iov_base = &buf, .iov_len = sizeof(buf)};
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} cmsg_buf;
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	ssize_t rc;

	*fd_out = -1;
	memset(&cmsg_buf, 0, sizeof(cmsg_buf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf.buf;
	msg.msg_controllen = sizeof(cmsg_buf.buf);

	rc = recvmsg(socket_fd, &msg, 0);
	if (rc == 0) {
		DOCA_LOG_ERR("OOB socket: recvmsg(SCM_RIGHTS): peer closed connection");
		return DOCA_ERROR_IO_FAILED;
	}
	if (rc < 0) {
		DOCA_LOG_ERR("OOB socket: recvmsg(SCM_RIGHTS) failed: %s", strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}
	if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		DOCA_LOG_ERR("OOB socket: recvmsg(SCM_RIGHTS): message truncated (flags=0x%x)", msg.msg_flags);
		return DOCA_ERROR_IO_FAILED;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
		DOCA_LOG_ERR("OOB socket: SCM_RIGHTS ancillary data invalid or missing");
		return DOCA_ERROR_IO_FAILED;
	}

	memcpy(fd_out, CMSG_DATA(cmsg), sizeof(int));

	return DOCA_SUCCESS;
}

/**
 * Find a supported TLP device
 *
 * @pci_address [in]: PCI address string to search for
 * @dev [out]: Pointer to store the found device
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t find_supported_tlp_device(const char *pci_address, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list;
	uint16_t max_tlp_types;
	uint32_t nb_devs;
	doca_error_t res;
	uint8_t is_equal;
	size_t i;

	/* Set default return value */
	*dev = NULL;
	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		if (doca_devinfo_is_equal_pci_addr(dev_list[i], pci_address, &is_equal) != DOCA_SUCCESS ||
		    is_equal == 0)
			continue;

		res = doca_devemu_pci_tlp_cap_get_max_types(dev_list[i], &max_tlp_types);
		if (res != DOCA_SUCCESS)
			continue;

		if (max_tlp_types == 0) {
			DOCA_LOG_WARN("Found device with matching address, but does not support PCI TLP emulation");
			continue;
		}

		res = doca_dev_open(dev_list[i], dev);
		if (res == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			return res;
		}
	}

	DOCA_LOG_WARN("Matching device not found");

	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

/**
 * Configure the SR-IOV initialization attributes on the PF representor init attributes
 *
 * @resources [in]: Pointer to TLP resources structure
 * @init_attr [in]: Representor initialization attributes to populate
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t config_pf_rep_sriov_attrs(struct devemu_tlp_resources *resources,
					      struct doca_devemu_pci_rep_init_attr *init_attr)
{
	const struct doca_devinfo *devinfo = doca_dev_as_devinfo(resources->dev);
	uint16_t max_total_vfs = 0;
	uint8_t sriov_supported = 0;
	doca_error_t result;

	result = doca_devemu_pci_tlp_cap_type_is_dynamic_sriov_supported(devinfo, &sriov_supported);
	if (result != DOCA_SUCCESS || !sriov_supported) {
		DOCA_LOG_ERR("Dynamic SR-IOV is not supported by the device");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	result = doca_devemu_pci_cap_get_max_total_vfs(devinfo, &max_total_vfs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to query maximum total VFs: %s", doca_error_get_descr(result));
		return result;
	}
	if (resources->total_vfs > max_total_vfs) {
		DOCA_LOG_ERR("Requested %u VFs exceeds device maximum of %u", resources->total_vfs, max_total_vfs);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_devemu_pci_rep_init_attr_set_total_vfs(init_attr, resources->total_vfs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SR-IOV total VFs: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_devemu_pci_rep_init_attr_set_vf_stride(init_attr, SRIOV_VF_STRIDE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SR-IOV VF stride: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_devemu_pci_rep_init_attr_set_first_vf_offset(init_attr, SRIOV_FIRST_VF_OFFSET);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SR-IOV first VF offset: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_devemu_pci_rep_init_attr_set_first_dynamic_vf_idx(init_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SR-IOV first dynamic VF index: %s", doca_error_get_descr(result));
		return result;
	}
	/* Keep the sample simple: VFs are exposed with no MSI-X vectors and no doorbells */
	result = doca_devemu_pci_rep_init_attr_set_vf_num_msix(init_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SR-IOV VF MSI-X count: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_devemu_pci_rep_init_attr_set_vf_num_db(init_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SR-IOV VF doorbell count: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Create and initialize PCI device representor with expansion ROM BAR
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_rep_dev(struct devemu_tlp_resources *resources)
{
	struct doca_devemu_pci_rep_init_attr *init_attr;
	doca_error_t result;

	result = doca_devemu_pci_rep_init_attr_create(&init_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create representor device initialization attributes: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_rep_init_attr_set_log_exp_bar_size(init_attr, LOG_EXP_BAR_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set log expansion ROM bar size: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_rep_init_attr_destroy(init_attr);
		return result;
	}

	/* When SR-IOV is requested, configure the PF representor so that creating its TLP device also
	 * provisions the dynamic VFs. */
	if (resources->total_vfs > 0) {
		result = config_pf_rep_sriov_attrs(resources, init_attr);
		if (result != DOCA_SUCCESS) {
			(void)doca_devemu_pci_rep_init_attr_destroy(init_attr);
			return result;
		}
	}

	/* init_rep_dev only ever creates the PF representor (VF representors are opened in init_vfs) */
	result = doca_devemu_pci_type_create_rep_ex(resources->pci_type, init_attr, &resources->pf.rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create representor device: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_rep_init_attr_destroy(init_attr);
		return result;
	}

	result = doca_devemu_pci_rep_init_attr_destroy(init_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to destroy representor device initialization attributes: %s",
			     doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Initialize TLP device
 *
 * @resources [in]: Pointer to TLP resources structure
 * @func [in]: The function (PF or a VF) to create/start the TLP device for
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_tlp_dev(struct devemu_tlp_resources *resources, struct devemu_pci_func *func)
{
	doca_error_t result;

	if (resources->is_channel_owner) {
		result = doca_devemu_pci_tlp_dev_create(resources->pci_type, func->rep, &func->tlp_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create TLP device: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_devemu_pci_tlp_dev_start(func->tlp_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start TLP device: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		/* If not a channel owner, assume the TLP device was already configured in another process */
		result = doca_devemu_pci_tlp_dev_create_started(resources->pci_type, func->rep, &func->tlp_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create started TLP device: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/**
 * Parse 64-bit address from TLP request header
 *
 * @tlp_dwords [in]: TLP request header as uint32_t array
 * @return: Parsed 64-bit address aligned to DWORD boundary
 */
static inline uint64_t parse_64bit_address(const uint32_t *tlp_dwords)
{
	uint32_t addr_high = be32toh(tlp_dwords[2]);
	uint32_t addr_low = be32toh(tlp_dwords[3]);
	return ((uint64_t)addr_high << 32) | (addr_low & DWORD_ALIGN_MASK);
}

/**
 * Parse 32-bit address from TLP request header
 *
 * @tlp_dwords [in]: TLP request header as uint32_t array
 * @return: Parsed 32-bit address aligned to DWORD boundary
 */
static inline uint64_t parse_32bit_address(const uint32_t *tlp_dwords)
{
	uint32_t addr_low = be32toh(tlp_dwords[2]);
	return addr_low & DWORD_ALIGN_MASK;
}

/**
 * Get TLP request type from TLP request header buffer
 *
 * @tlp_req [in]: Pointer to TLP request
 *
 * @return: TLP request type enumeration value based on PCI specification
 */
static inline enum tlp_req_type get_tlp_req_type(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint8_t type = GET_TLP_REQ_TYPE(tlp_req_header);

	switch (type) {
	case TLP_REQ_TYPE_MEMORY_READ_WRITE: /* Memory read or write */
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_MEMORY_WRITE;
		} else {
			return TLP_REQ_TYPE_MEMORY_READ;
		}
	case TLP_REQ_TYPE_IO_READ_WRITE: /* IO read or write */
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_IO_WRITE;
		} else {
			return TLP_REQ_TYPE_IO_READ;
		}
	case TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0: /* Config read type 0 or write type 0 */
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0;
		} else {
			return TLP_REQ_TYPE_CONFIG_READ_TYPE_0;
		}
	case TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1: /* Config read type 1 or write type 1 */
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1;
		} else {
			return TLP_REQ_TYPE_CONFIG_READ_TYPE_1;
		}
	default:
		return TLP_REQ_TYPE_INVALID;
	}
}

/**
 * Check if TLP request is non-posted (requires completion)
 *
 * @tlp_req [in]: Pointer to TLP request
 *
 * @return: true if non-posted request, false if posted
 */
static inline bool is_non_posted_tlp_req(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	enum tlp_req_type req_type = get_tlp_req_type(tlp_req);

	return req_type != TLP_REQ_TYPE_INVALID && req_type != TLP_REQ_TYPE_MEMORY_WRITE;
}

/******************************************************************************
 ******************************************************************************
 *                 Handle TLP request by type - Start                         *
 ******************************************************************************/

/******************************************************************************
 *            Config read TLP - PCI configuration space header                *
 ******************************************************************************/

/**
 * Read PCI configuration space ext cap AER
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t config_space_ext_cap_aer_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned cap_offset = reg_num - TLP_CAP_AER_REG_NUM;

	switch (cap_offset) {
	case 0:
		return resources->cur_func->pci_config_space.ext_caps.aer_ext_cap.reg0.as_dword;
	default:
		return 0;
	}

	return 0;
}

/**
 * Read PCI configuration space ext cap SR-IOV (PF only)
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t config_space_ext_cap_sriov_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	const struct pci_config_space_ext_cap_sriov *sriov =
		&resources->cur_func->pci_config_space.ext_caps.sriov_ext_cap;
	const unsigned vf_bar_offset = 9;
	unsigned cap_offset = reg_num - TLP_CAP_SRIOV_REG_NUM;

	/* 24h-38h: VF BAR0..VF BAR5 */
	if (cap_offset >= vf_bar_offset && cap_offset < vf_bar_offset + TYPE_0_CFG_SPACE_HEADER_BAR_NUM)
		return sriov->vf_bar[cap_offset - vf_bar_offset];

	switch (cap_offset) {
	case 0: /* 00h: ext cap header */
		return sriov->reg0.as_dword;
	case 1: /* 04h: SR-IOV Capabilities */
		return sriov->cap_register;
	case 2: /* 08h: Control / Status */
		return sriov->reg2.as_dword;
	case 3: /* 0Ch: InitialVFs / TotalVFs */
		return sriov->reg3.as_dword;
	case 4: /* 10h: NumVFs / Fcn Dep Link */
		return sriov->reg4.as_dword;
	case 5: /* 14h: First VF Offset / VF Stride */
		return sriov->reg5.as_dword;
	case 6: /* 18h: VF Device ID */
		return sriov->reg6.as_dword;
	case 7: /* 1Ch: Supported Page Sizes */
		return sriov->supported_page_sizes;
	case 8: /* 20h: System Page Size */
		return sriov->system_page_size;
	case 15:
	default:
		/* VF migration state array and any reserved holes are not implemented. */
		return 0;
	}
}

/**
 * Read PCI configuration space cap PM
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t config_space_cap_pm_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned cap_offset = reg_num - TLP_CAP_PM_REG_NUM;

	switch (cap_offset) {
	case 0:
		return resources->cur_func->pci_config_space.caps.pm_cap.reg0.as_dword;
	case 1:
		return resources->cur_func->pci_config_space.caps.pm_cap.reg1.as_dword;
	default:
		return 0;
	}

	return 0;
}

/**
 * Read PCI configuration space cap MSIX
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t config_space_cap_msix_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned cap_offset = reg_num - TLP_CAP_MSIX_REG_NUM;

	switch (cap_offset) {
	case 0:
		return resources->cur_func->pci_config_space.caps.msix_cap.reg0.as_dword;
	case 1:
		return resources->cur_func->pci_config_space.caps.msix_cap.reg1.as_dword;
	case 2:
		return resources->cur_func->pci_config_space.caps.msix_cap.reg2.as_dword;
	default:
		return 0;
	}

	return 0;
}

/**
 * Read PCI configuration space cap VPD
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t config_space_cap_vpd_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned cap_offset = reg_num - TLP_CAP_VPD_REG_NUM;

	switch (cap_offset) {
	case 0:
		return resources->cur_func->pci_config_space.caps.vpd_cap.reg0.as_dword;
	case 1:
		return resources->cur_func->pci_config_space.caps.vpd_cap.msg_addr;
	default:
		return 0;
	}

	return 0;
}

/**
 * Read PCI configuration space cap PCI express
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t config_space_cap_pcie_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned cap_offset = reg_num - TLP_CAP_EXPRESS_REG_NUM;

	switch (cap_offset) {
	case 0:
		return resources->cur_func->pci_config_space.caps.express_cap.reg0.as_dword;
	case 1:
		return resources->cur_func->pci_config_space.caps.express_cap.cap_register;
	case 2:
		return resources->cur_func->pci_config_space.caps.express_cap.reg2.as_dword;
	case 3:
		return resources->cur_func->pci_config_space.caps.express_cap.link_cap;
	case 4:
		return resources->cur_func->pci_config_space.caps.express_cap.reg4.as_dword;
	case 5:
		return resources->cur_func->pci_config_space.caps.express_cap.slot_cap;
	case 6:
		return resources->cur_func->pci_config_space.caps.express_cap.reg6.as_dword;
	case 7:
		return resources->cur_func->pci_config_space.caps.express_cap.reg7.as_dword;
	case 8:
		return resources->cur_func->pci_config_space.caps.express_cap.root_status;
	case 9:
		return resources->cur_func->pci_config_space.caps.express_cap.dev_cap2;
	case 10:
		return resources->cur_func->pci_config_space.caps.express_cap.regA.as_dword;
	case 11:
		return resources->cur_func->pci_config_space.caps.express_cap.link_cap2;
	case 12:
		return resources->cur_func->pci_config_space.caps.express_cap.regC.as_dword;
	case 13:
		return resources->cur_func->pci_config_space.caps.express_cap.slot_cap2;
	case 14:
		return resources->cur_func->pci_config_space.caps.express_cap.regE.as_dword;
	default:
		return 0;
	}

	return 0;
}

/**
 * Read PCI configuration space header register 00h
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg0(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.reg0.as_dword;
}

/**
 * Read PCI configuration space header register 01h
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg1(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.reg1.as_dword;
}

/**
 * Read PCI configuration space header register 02h
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg2(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.reg2.as_dword;
}

/**
 * Read PCI configuration space header register 03h
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg3(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.reg3.as_dword;
}

/**
 * Read PCI configuration space BAR register (04h to 09h)
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: BAR register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_bar(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned bar_id = reg_num - 0x4;

	return resources->cur_func->pci_config_space.header.bar[bar_id];
}

/**
 * Read PCI configuration space header register 11 (0Bh) (subsystem IDs)
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg11(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.regB.as_dword;
}

/**
 * Read PCI configuration space header register 12 (0Ch) (expansion ROM)
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg12(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.expansion_rom_base_address;
}

/**
 * Read PCI configuration space header register 13 (0Dh) (capabilities pointer)
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg13(struct devemu_tlp_resources *resources)
{
	return resources->cur_func->pci_config_space.header.regD.as_dword;
}

/**
 * Read PCI configuration space header register 15 (0Fh) (interrupt info)
 *
 * @return: Register value as 32-bit unsigned integer
 */
static inline uint32_t config_space_header_read_reg15(void)
{
	DOCA_LOG_DBG("%s:: SW Disabled", __func__);

	return 0;
}

/**
 * Handle PCI configuration space header read request
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_header_read(unsigned reg_num, struct devemu_tlp_resources *resources)
{
	unsigned data;

	switch (reg_num) {
	case 0x0:
		data = config_space_header_read_reg0(resources);
		break;
	case 0x1:
		data = config_space_header_read_reg1(resources);
		break;
	case 0x2:
		data = config_space_header_read_reg2(resources);
		break;
	case 0x3:
		data = config_space_header_read_reg3(resources);
		break;
	case 0x4 ... 0x9:
		data = config_space_header_read_bar(reg_num, resources);
		break;
	case 0xa:
		data = 0;
		break; /* Hard coded 0 */
	case 0xb:
		data = config_space_header_read_reg11(resources);
		break;
	case 0xc:
		data = config_space_header_read_reg12(resources);
		break;
	case 0xd:
		data = config_space_header_read_reg13(resources);
		break;
	case 0xe:
		data = 0;
		break; /* Reserved */
	case 0xf:
		data = config_space_header_read_reg15();
		break;
	default:
		data = 0;
		break;
	}

	/* set the completion header fields for Config Read */
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_W_DATA;
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_SINGLE_DWORD;
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;

	return data;
}

/******************************************************************************
 *            Config read TLP - PCI configuration space capabilities          *
 ******************************************************************************/

/**
 * Handle PCI configuration space capabilities read request
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 * @cap_id [out]: Pointer to capability ID to be filled
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_caps_read(unsigned reg_num,
						     struct devemu_tlp_resources *resources,
						     uint16_t *cap_id)
{
	unsigned data;

	switch (reg_num) {
	case TLP_CAP_EXPRESS_REG_NUM ...(TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW - 1):
		data = config_space_cap_pcie_read(reg_num, resources);
		*cap_id = TLP_PCI_CAP_ID_EXPRESS;
		break;
	case TLP_CAP_VPD_REG_NUM ...(TLP_CAP_VPD_REG_NUM + TLP_CAP_VPD_LEN_DW - 1):
		data = config_space_cap_vpd_read(reg_num, resources);
		*cap_id = TLP_PCI_CAP_ID_VPD;
		break;
	case TLP_CAP_MSIX_REG_NUM ...(TLP_CAP_MSIX_REG_NUM + TLP_CAP_MSIX_LEN_DW - 1):
		data = config_space_cap_msix_read(reg_num, resources);
		*cap_id = TLP_PCI_CAP_ID_MSIX;
		break;
	case TLP_CAP_PM_REG_NUM ...(TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW - 1):
		data = config_space_cap_pm_read(reg_num, resources);
		*cap_id = TLP_PCI_CAP_ID_PM;
		break;
	default:
		data = 0;
		break;
	}

	/* set completion header fields for Config Space Read */
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_W_DATA;
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_SINGLE_DWORD;
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;

	return data;
}

/******************************************************************************
 *      Config read TLP - PCI configuration space extended capabilities       *
 ******************************************************************************/

/**
 * Handle PCI configuration space extended capabilities read request
 *
 * @reg_num [in]: Register number to read
 * @resources [in]: Pointer to TLP resources structure
 * @cap_id [out]: Pointer to capability ID to be filled
 *
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_ext_caps_read(unsigned reg_num,
							 struct devemu_tlp_resources *resources,
							 uint16_t *cap_id)
{
	unsigned data;

	switch (reg_num) {
	case TLP_CAP_AER_REG_NUM ...(TLP_CAP_AER_REG_NUM + TLP_CAP_AER_LEN_DW - 1):
		data = config_space_ext_cap_aer_read(reg_num, resources);
		*cap_id = TLP_PCIE_CAP_ID_AER;
		break;
	case TLP_CAP_SRIOV_REG_NUM ...(TLP_CAP_SRIOV_REG_NUM + TLP_CAP_SRIOV_LEN_DW - 1):
		/* The SR-IOV capability exists only in the PF configuration space */
		if (resources->cur_func->func_type == DOCA_PCI_FUNC_TYPE_PF) {
			data = config_space_ext_cap_sriov_read(reg_num, resources);
			*cap_id = TLP_PCIE_CAP_ID_SRIOV;
		} else {
			data = 0;
		}
		break;
	default:
		data = 0;
		break;
	}

	/* set completion header fields for Extended Config Space Read */
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_W_DATA;
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_SINGLE_DWORD;
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;

	return data;
}

/******************************************************************************
 *            Config write TLP - PCI configuration space header              *
 ******************************************************************************/

/**
 * Write PCI configuration space cap PM
 *
 * @reg_num [in]: Register number to write
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void config_space_cap_pm_write(unsigned reg_num,
					     unsigned data_in,
					     unsigned be_mask,
					     struct devemu_tlp_resources *resources)
{
	struct pci_config_space_cap_pm *pm_cap = &resources->cur_func->pci_config_space.caps.pm_cap;
	union pci_config_space_cap_pm_reg1 data, mask;
	unsigned cap_offset = reg_num - TLP_CAP_PM_REG_NUM;

	/* cap_pm write only for dw1 */
	if (cap_offset != 1)
		return;

	data.as_dword = data_in;
	mask.as_dword = be_mask;

	if (mask.fields.pmcsr & TLP_CAP_PM_STATE) {
		if (data.fields.pmcsr & TLP_CAP_PM_STATE) {
			pm_cap->reg1.fields.pmcsr &= ~TLP_CAP_PM_STATE;
			pm_cap->reg1.fields.pmcsr |= (data.fields.pmcsr & TLP_CAP_PM_STATE);
		} else
			pm_cap->reg1.fields.pmcsr &= ~TLP_CAP_PM_STATE;
	}

	if (mask.fields.pmcsr & TLP_CAP_PM_PME_EN) {
		if (data.fields.pmcsr & TLP_CAP_PM_PME_EN)
			pm_cap->reg1.fields.pmcsr |= TLP_CAP_PM_PME_EN;
		else
			pm_cap->reg1.fields.pmcsr &= ~TLP_CAP_PM_PME_EN;
	}

	if (mask.fields.pmcsr & TLP_CAP_PM_DATA_SEL) {
		if (data.fields.pmcsr & TLP_CAP_PM_DATA_SEL) {
			pm_cap->reg1.fields.pmcsr &= ~TLP_CAP_PM_DATA_SEL;
			pm_cap->reg1.fields.pmcsr |= (data.fields.pmcsr & TLP_CAP_PM_DATA_SEL);
		} else
			pm_cap->reg1.fields.pmcsr &= ~TLP_CAP_PM_DATA_SEL;
	}

	if (mask.fields.pmcsr & TLP_CAP_PM_PME_STATUS) {
		if (data.fields.pmcsr & TLP_CAP_PM_PME_STATUS)
			pm_cap->reg1.fields.pmcsr |= TLP_CAP_PM_PME_STATUS;
		else
			pm_cap->reg1.fields.pmcsr &= ~TLP_CAP_PM_PME_STATUS;
	}

	resources->cur_func->tlp_handler_cxt.capability_offset = cap_offset;
	resources->cur_func->tlp_handler_cxt.capability_id = TLP_PCI_CAP_ID_PM;
}

/**
 * Write PCI configuration space header register 01h (command/status)
 *
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void config_space_header_write_reg1(unsigned data_in,
						  unsigned be_mask,
						  struct devemu_tlp_resources *resources)
{
	union pci_config_space_header_reg1 data;
	union pci_config_space_header_reg1 mask;
	data.as_dword = data_in;
	mask.as_dword = be_mask;

	/* Handle Command register bits (Read/Write) */
	/* For each command bit: if enabled in mask, set/clear based on data value */
	if (mask.fields.command & COMMAND_RW_MEM_SPACE_ENABLE) {
		if (data.fields.command & COMMAND_RW_MEM_SPACE_ENABLE) {
			resources->cur_func->pci_config_space.header.reg1.fields.command |= COMMAND_RW_MEM_SPACE_ENABLE;
		} else {
			resources->cur_func->pci_config_space.header.reg1.fields.command &=
				~COMMAND_RW_MEM_SPACE_ENABLE;
		}
	}
	if (mask.fields.command & COMMAND_RW_BUS_MASTER_ENABLE) {
		if (data.fields.command & COMMAND_RW_BUS_MASTER_ENABLE) {
			resources->cur_func->pci_config_space.header.reg1.fields.command |=
				COMMAND_RW_BUS_MASTER_ENABLE;
		} else {
			resources->cur_func->pci_config_space.header.reg1.fields.command &=
				~COMMAND_RW_BUS_MASTER_ENABLE;
		}
	}
	if (mask.fields.command & COMMAND_RW_PERR_ENABLE) {
		if (data.fields.command & COMMAND_RW_PERR_ENABLE) {
			resources->cur_func->pci_config_space.header.reg1.fields.command |= COMMAND_RW_PERR_ENABLE;
		} else {
			resources->cur_func->pci_config_space.header.reg1.fields.command &= ~COMMAND_RW_PERR_ENABLE;
		}
	}
	if (mask.fields.command & COMMAND_RW_SERR_ENABLE) {
		if (data.fields.command & COMMAND_RW_SERR_ENABLE) {
			resources->cur_func->pci_config_space.header.reg1.fields.command |= COMMAND_RW_SERR_ENABLE;
		} else {
			resources->cur_func->pci_config_space.header.reg1.fields.command &= ~COMMAND_RW_SERR_ENABLE;
		}
	}
	if (mask.fields.command & COMMAND_RW_INT_DISABLE) {
		if (data.fields.command & COMMAND_RW_INT_DISABLE) {
			resources->cur_func->pci_config_space.header.reg1.fields.command |= COMMAND_RW_INT_DISABLE;
		} else {
			resources->cur_func->pci_config_space.header.reg1.fields.command &= ~COMMAND_RW_INT_DISABLE;
		}
	}

	/* Handle Status register bits (Write-1-to-Clear for WR1C bits) */
	/* For WR1C bits: if the bit is set in mask AND data, clear the corresponding bit in status */
	if (mask.fields.status & STATUS_WR1C_MASTER_DATA_PERR) {
		if (data.fields.status & STATUS_WR1C_MASTER_DATA_PERR) {
			resources->cur_func->pci_config_space.header.reg1.fields.status &=
				~STATUS_WR1C_MASTER_DATA_PERR;
		}
	}
	if (mask.fields.status & STATUS_WR1C_SIGNALED_TA) {
		if (data.fields.status & STATUS_WR1C_SIGNALED_TA) {
			resources->cur_func->pci_config_space.header.reg1.fields.status &= ~STATUS_WR1C_SIGNALED_TA;
		}
	}
	if (mask.fields.status & STATUS_WR1C_RECEIVE_TA) {
		if (data.fields.status & STATUS_WR1C_RECEIVE_TA) {
			resources->cur_func->pci_config_space.header.reg1.fields.status &= ~STATUS_WR1C_RECEIVE_TA;
		}
	}
	if (mask.fields.status & STATUS_WR1C_RECEIVE_MA) {
		if (data.fields.status & STATUS_WR1C_RECEIVE_MA) {
			resources->cur_func->pci_config_space.header.reg1.fields.status &= ~STATUS_WR1C_RECEIVE_MA;
		}
	}
	if (mask.fields.status & STATUS_WR1C_SIGNALED_SERR) {
		if (data.fields.status & STATUS_WR1C_SIGNALED_SERR) {
			resources->cur_func->pci_config_space.header.reg1.fields.status &= ~STATUS_WR1C_SIGNALED_SERR;
		}
	}
	if (mask.fields.status & STATUS_WR1C_DETECTED_PERR) {
		if (data.fields.status & STATUS_WR1C_DETECTED_PERR) {
			resources->cur_func->pci_config_space.header.reg1.fields.status &= ~STATUS_WR1C_DETECTED_PERR;
		}
	}
}

/**
 * Write PCI configuration space header register 03h (cache/latency/header/BIST)
 *
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void config_space_header_write_reg3(unsigned data_in,
						  unsigned be_mask,
						  struct devemu_tlp_resources *resources)
{
	union pci_config_space_header_reg3 data;
	union pci_config_space_header_reg3 mask;
	data.as_dword = data_in;
	mask.as_dword = be_mask;

	/* only need to write cache_line_size */
	if (mask.fields.cache_line_size)
		resources->cur_func->pci_config_space.header.reg3.fields.cache_line_size = data.fields.cache_line_size &
											   mask.fields.cache_line_size;
}

/**
 * Write PCI configuration space BAR register (04h to 09h)
 *
 * @reg_num [in]: Register number
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void config_space_header_write_bar(unsigned reg_num,
						 unsigned data_in,
						 unsigned be_mask,
						 struct devemu_tlp_resources *resources)
{
	uint64_t region_size = resources->cur_func->tlp_handler_cxt.bar_region_size;
	unsigned bar_id = reg_num - 0x4;

	/* VF BARs are read-only in the VF config space; they are sized/located via the PF SR-IOV VF BARs */
	if (resources->cur_func->func_type == DOCA_PCI_FUNC_TYPE_VF)
		return;

	if (bar_id > 1)
		return; /* only support bar0 and bar1 */

	if ((data_in & be_mask) == 0xFFFFFFFF) {
		if (reg_num & 0x1)
			resources->cur_func->pci_config_space.header.bar[bar_id] = (~(region_size - 1) >> 32) &
										   0xFFFFFFFF;
		else
			resources->cur_func->pci_config_space.header.bar[bar_id] = ~(region_size - 1) & 0xFFFFFFFF;
	} else {
		resources->cur_func->pci_config_space.header.bar[bar_id] = data_in & be_mask;
	}
	if (bar_id == 0) {
		resources->cur_func->pci_config_space.header.bar[bar_id] &= 0xFFFFFFF0;
		resources->cur_func->pci_config_space.header.bar[bar_id] |=
			(BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT | BAR_MEM_PREFETCHABLE);
	}
}

/**
 * Write PCI configuration space header register 12 (0Ch) (expansion ROM)
 *
 */
static inline void config_space_header_write_reg12(unsigned data_in,
						   unsigned be_mask,
						   struct devemu_tlp_resources *resources)
{
	uint32_t exp_bar_size = resources->cur_func->tlp_handler_cxt.exp_bar_size;

	if ((data_in & be_mask & EXP_BAR_ADDR_MASK) == EXP_BAR_ADDR_MASK) {
		resources->cur_func->pci_config_space.header.expansion_rom_base_address = ~(exp_bar_size - 1) &
											  0xFFFFFFFF;
	} else {
		resources->cur_func->pci_config_space.header.expansion_rom_base_address = data_in & be_mask;
	}

	resources->cur_func->pci_config_space.header.expansion_rom_base_address &= EXP_BAR_ADDR_MASK;
	resources->cur_func->pci_config_space.header.expansion_rom_base_address |= EXP_BAR_ENABLED_BIT_SET;
}

/**
 * Write PCI configuration space header register 15 (0Fh) (interrupt info)
 *
 */
static inline void config_space_header_write_reg15(void)
{
	DOCA_LOG_DBG("%s:: SW Disabled", __func__);
}

/**
 * Handle PCI configuration space header write request
 *
 * @reg_num [in]: Register number to write
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void handle_config_space_header_write(unsigned reg_num,
						    unsigned data_in,
						    unsigned be_mask,
						    struct devemu_tlp_resources *resources)
{
	switch (reg_num) {
	case 0x0: /* RO */
		break;
	case 0x1:
		config_space_header_write_reg1(data_in, be_mask, resources);
		break;
	case 0x2: /* RO */
		break;
	case 0x3:
		config_space_header_write_reg3(data_in, be_mask, resources);
		break;
	case 0x4 ... 0x9:
		config_space_header_write_bar(reg_num, data_in, be_mask, resources);
		break;
	case 0xA: /* RO */
		break;
	case 0xB: /* RO */
		break;
	case 0xC:
		config_space_header_write_reg12(data_in, be_mask, resources);
		break;
	case 0xD: /* RO */
		break;
	case 0xE: /* Reserved */
		break;
	case 0xF:
		config_space_header_write_reg15();
		break;
	default:
		break;
	}

	/* set the completion header fields for Config Write */
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_NODATA; /* No need to return data for Config Write
									     */
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO; /* 0 DW (0 bytes) */
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;
}

/******************************************************************************
 *           Config write TLP - PCI configuration space capabilities          *
 ******************************************************************************/

/**
 * Handle PCI configuration space capabilities write request
 *
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void handle_config_space_caps_write(unsigned reg_num,
						  unsigned data_in,
						  unsigned be_mask,
						  struct devemu_tlp_resources *resources,
						  uint16_t *cap_id)
{
	switch (reg_num) {
	case TLP_CAP_PM_REG_NUM ...(TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW - 1):
		config_space_cap_pm_write(reg_num, data_in, be_mask, resources);
		*cap_id = TLP_PCI_CAP_ID_PM;
		break;
	default:
		break;
	}

	/* set completion header fields for Config Space Write */
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_NODATA; /* No need to return data for Config Write
									     */
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO; /* 0 DW */
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;
}

/******************************************************************************
 *      Config write TLP - PCI configuration space extended capabilities      *
 ******************************************************************************/

/**
 * Write PCI configuration space ext cap SR-IOV (PF only)
 *
 * Only a subset of the SR-IOV registers are writable: the SR-IOV Control register (VF Enable and
 * VF MSE bits), NumVFs (writable only while VF Enable is clear), System Page Size, and the VF BARs.
 *
 * @reg_num [in]: Register number to write
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void config_space_ext_cap_sriov_write(unsigned reg_num,
						    unsigned data_in,
						    unsigned be_mask,
						    struct devemu_tlp_resources *resources)
{
	struct pci_config_space_ext_cap_sriov *sriov = &resources->cur_func->pci_config_space.ext_caps.sriov_ext_cap;
	unsigned cap_offset = reg_num - TLP_CAP_SRIOV_REG_NUM;

	switch (cap_offset) {
	case 2: { /* 08h: SR-IOV Control - only VF Enable and VF MSE are writable */
		uint16_t ctrl_mask = (uint16_t)(be_mask & (SRIOV_CTRL_VF_ENABLE | SRIOV_CTRL_VF_MSE));

		sriov->reg2.fields.control = (sriov->reg2.fields.control & ~ctrl_mask) |
					     ((uint16_t)data_in & ctrl_mask);
		DOCA_LOG_DBG("SR-IOV Control updated: VF Enable=%d, VF MSE=%d",
			     !!(sriov->reg2.fields.control & SRIOV_CTRL_VF_ENABLE),
			     !!(sriov->reg2.fields.control & SRIOV_CTRL_VF_MSE));
		break;
	}
	case 4: { /* 10h: NumVFs - writable only while VF Enable is clear */
		if (!(sriov->reg2.fields.control & SRIOV_CTRL_VF_ENABLE))
			sriov->reg4.fields.num_vfs = (sriov->reg4.fields.num_vfs & ~be_mask) |
						     ((uint16_t)data_in & be_mask);
		break;
	}
	case 8: /* 20h: System Page Size (RW) */
		sriov->system_page_size = (sriov->system_page_size & ~be_mask) | (data_in & be_mask);
		break;
	case 9 ... 14: { /* 24h-38h: VF BAR0..VF BAR5 (RW), with BAR sizing semantics */
		unsigned bar_id = cap_offset - 9;
		uint64_t vf_bar_size = (1ULL << layout_configs[0].log_size);

		/* Like the PF, only BAR0/BAR1 form the single 64-bit VF BAR; VF BAR2..5 are read-only 0 */
		if (bar_id > 1)
			break;

		if ((data_in & be_mask) == 0xFFFFFFFF) {
			/* Host is probing the VF BAR size by writing all 1s */
			if (bar_id == 1) /* upper 32 bits of the 64-bit VF BAR0 */
				sriov->vf_bar[bar_id] = (~(vf_bar_size - 1) >> 32) & 0xFFFFFFFF;
			else
				sriov->vf_bar[bar_id] = ~(vf_bar_size - 1) & 0xFFFFFFFF;
		} else {
			sriov->vf_bar[bar_id] = data_in & be_mask;
		}
		if (bar_id == 0) {
			sriov->vf_bar[bar_id] &= 0xFFFFFFF0;
			sriov->vf_bar[bar_id] |= (BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT | BAR_MEM_PREFETCHABLE);
		}
		break;
	}
	default: /* All other SR-IOV registers are read-only in this sample */
		break;
	}
}

/**
 * Handle PCI configuration space extended capabilities write request
 *
 * @reg_num [in]: Register number to write
 * @data_in [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @resources [in]: Pointer to TLP resources structure
 * @cap_id [out]: Pointer to capability ID to be filled
 */
static inline void handle_config_space_ext_caps_write(unsigned reg_num,
						      unsigned data_in,
						      unsigned be_mask,
						      struct devemu_tlp_resources *resources,
						      uint16_t *cap_id)
{
	/* The SR-IOV capability is only present (and writable) in the PF configuration space */
	if (resources->cur_func->func_type == DOCA_PCI_FUNC_TYPE_PF && reg_num >= TLP_CAP_SRIOV_REG_NUM &&
	    reg_num < TLP_CAP_SRIOV_REG_NUM + TLP_CAP_SRIOV_LEN_DW) {
		config_space_ext_cap_sriov_write(reg_num, data_in, be_mask, resources);
		*cap_id = TLP_PCIE_CAP_ID_SRIOV;
	}

	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_NODATA; /* No need to return data for Config Write
									     */
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO; /* 0 DW */
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;
}

/******************************************************************************
 *                   Handle TLP request by type - End                         *
 ******************************************************************************
 ******************************************************************************/

/**
 * Set TLP request completion header context
 *
 * @tlp_response_header [out]: Pointer to completion header buffer
 * @resources [in]: Pointer to TLP resources structure
 * @byte_count [in]: Byte count for the completion header
 */
static inline void set_tlp_req_completion_header(void *tlp_response_header,
						 struct devemu_tlp_resources *resources,
						 uint16_t byte_count)
{
	uint32_t *header_dw = (uint32_t *)tlp_response_header;

	memset(tlp_response_header, 0, TLP_COMPLETION_HEADER_DW_SIZE * sizeof(uint32_t));

	/* DW0: fmt[31:29], type[28:24], tag9[bit 23], tag8[bit 19], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), resources->cur_func->tlp_handler_cxt.cmpl_fmt, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), resources->cur_func->tlp_handler_cxt.cmpl_type, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), resources->cur_func->tlp_handler_cxt.tag9, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), resources->cur_func->tlp_handler_cxt.tag8, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), resources->cur_func->tlp_handler_cxt.cmpl_length, &header_dw[0]);

	/* DW1: completer_id[31:16], cmpl_status[15:13], byte_cnt[11:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), resources->cur_func->tlp_handler_cxt.completer_id, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), resources->cur_func->tlp_handler_cxt.cmpl_status, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &header_dw[1]);

	/* DW2: requester_id[31:16], tag[15:8] (set to 0), lower_addr[6:0] (set to 0) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), resources->cur_func->tlp_handler_cxt.requester_id, &header_dw[2]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), resources->cur_func->tlp_handler_cxt.tag, &header_dw[2]);
	/* lower_addr are already zeroed by memset */
}

/**
 * Map first DW byte enable to mask based on PCI specification
 *
 * @first_dw_be [in]: First DW byte enable value
 *
 * @return: Corresponding byte mask based on PCI specification
 */
static inline unsigned map_first_dw_be_to_mask(unsigned first_dw_be)
{
	static const unsigned first_dw_be_to_mask[] = {
		0x00000000, /* 0000 */
		0x000000FF, /* 0001 */
		0x0000FF00, /* 0010 */
		0x0000FFFF, /* 0011 */
		0x00FF0000, /* 0100 */
		0x00FF00FF, /* 0101 */
		0x00FFFF00, /* 0110 */
		0x00FFFFFF, /* 0111 */
		0xFF000000, /* 1000 */
		0xFF0000FF, /* 1001 */
		0xFF00FF00, /* 1010 */
		0xFF00FFFF, /* 1011 */
		0xFFFF0000, /* 1100 */
		0xFFFF00FF, /* 1101 */
		0xFFFFFF00, /* 1110 */
		0xFFFFFFFF, /* 1111 */
	};

	return first_dw_be_to_mask[first_dw_be];
}

/**
 * Handle TLP request read type_0
 *
 * @tlp_req [in]: Pointer to TLP request
 * @resources [in]: Pointer to TLP resources structure
 * @ext_reg_num [in]: ext reg num
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_read_type_0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct devemu_tlp_resources *resources,
					      uint32_t ext_reg_num,
					      uint16_t *cap_id,
					      bool *is_cap_id_valid,
					      bool *is_pcie_cap)
{
	uint32_t cfg_read_data = 0;

	DOCA_LOG_DBG("Handle TLP Request for Config Read,  ext_reg_num: %d", ext_reg_num);
	switch (ext_reg_num) {
	case 0 ... 15:
		cfg_read_data = handle_config_space_header_read(ext_reg_num, resources);
		break;
	case 16 ... 63:
		cfg_read_data = handle_config_space_caps_read(ext_reg_num, resources, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = false;
		break;
	case 64 ... 1023:
		cfg_read_data = handle_config_space_ext_caps_read(ext_reg_num, resources, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = true;
		break;
	default:
		DOCA_LOG_DBG("Invalid read ext_reg_num %d", ext_reg_num);
		break;
	}

	DOCA_LOG_DBG(" --> cfg_read_data:  %08x", cfg_read_data);
	/* Set the completion data */
	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(tlp_cpl_data, &cfg_read_data, sizeof(cfg_read_data));
	/* Set the completion header */
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      resources,
				      BYTES_IN_DWORD);
}
/**
 * Handle TLP request write type_0
 *
 * @tlp_req [in]: Pointer to TLP request
 * @resources [in]: Pointer to TLP resources structure
 * @ext_reg_num [in]: ext reg num
 * @tlp_req_header [in]: Pointer to TLP request header
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_write_type_0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct devemu_tlp_resources *resources,
					       uint32_t ext_reg_num,
					       const void *tlp_req_header,
					       uint16_t *cap_id,
					       bool *is_cap_id_valid,
					       bool *is_pcie_cap)
{
	unsigned be_mask = map_first_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	const uint32_t *cfg_write_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);

	DOCA_LOG_DBG("Handle TLP Request for Config Write, ext_reg_num: %d", ext_reg_num);
	DOCA_LOG_DBG(" --> cfg_write_data: %08x, be_mask: %08x", cfg_write_data[0], be_mask);
	switch (ext_reg_num) {
	case 0 ... 15:
		handle_config_space_header_write(ext_reg_num, cfg_write_data[0], be_mask, resources);
		break;
	case 16 ... 63:
		handle_config_space_caps_write(ext_reg_num, cfg_write_data[0], be_mask, resources, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = false;
		break;
	case 64 ... 1023:
		handle_config_space_ext_caps_write(ext_reg_num, cfg_write_data[0], be_mask, resources, cap_id);
		if (cap_id != 0) {
			*is_cap_id_valid = true;
			*is_pcie_cap = true;
		}
		break;
	default:
		DOCA_LOG_DBG("Invalid write ext_reg_num %d", ext_reg_num);
		break;
	}
	/* Set the completion header */
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      resources,
				      BYTES_IN_DWORD);
}

/**
 * Initialize transaction region memory
 *
 * @func [in]: The function (PF or a VF) to allocate the transaction region for
 * @size [in]: Size of transaction region to allocate
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE for invalid parameters,
 *          DOCA_ERROR_NO_MEMORY for memory allocation failure
 */
static doca_error_t tlp_init_transaction_region(struct devemu_pci_func *func, size_t size)
{
	if (!func) {
		DOCA_LOG_ERR("Invalid function pointer");
		return DOCA_ERROR_INVALID_VALUE;
	}

	func->tlp_handler_cxt.transaction_region_memory = malloc(size);
	if (!func->tlp_handler_cxt.transaction_region_memory) {
		DOCA_LOG_ERR("Failed to allocate transaction region memory of size %zu", size);
		return DOCA_ERROR_NO_MEMORY;
	}

	func->tlp_handler_cxt.transaction_region_size = size;

	/* Initialize with a pattern for testing */
	memset(func->tlp_handler_cxt.transaction_region_memory, TRANSACTION_REGION_INIT_PATTERN, size);

	DOCA_LOG_INFO("Transaction region initialized: size=%zu bytes, base_addr=%p",
		      size,
		      func->tlp_handler_cxt.transaction_region_memory);

	return DOCA_SUCCESS;
}

/**
 * Initialize Expansion ROM bar memory
 *
 * @resources [in]: Pointer to TLP resources structure
 * @size [in]: Size of Expansion ROM bar to allocate
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE for invalid parameters,
 *          DOCA_ERROR_NO_MEMORY for memory allocation failure
 */
static doca_error_t tlp_init_exp_bar(struct devemu_tlp_resources *resources, size_t size)
{
	if (!resources) {
		DOCA_LOG_ERR("Invalid resources pointer");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Only the PF owns an Expansion ROM BAR; VFs do not have one */
	resources->pf.tlp_handler_cxt.exp_bar_memory = malloc(size);
	if (!resources->pf.tlp_handler_cxt.exp_bar_memory) {
		DOCA_LOG_ERR("Failed to allocate Expansion ROM bar memory of size %zu", size);
		return DOCA_ERROR_NO_MEMORY;
	}

	resources->pf.tlp_handler_cxt.exp_bar_size = size;

	/* Initialize with a pattern for testing */
	memset(resources->pf.tlp_handler_cxt.exp_bar_memory, EXP_BAR_INIT_PATTERN, size);

	DOCA_LOG_INFO("Expansion ROM bar region initialized: size=%zu bytes, base_addr=%p",
		      size,
		      resources->pf.tlp_handler_cxt.exp_bar_memory);

	return DOCA_SUCCESS;
}

/**
 * Cleanup transaction region memory
 *
 * @func [in]: The function (PF or a VF) whose transaction region should be freed
 */
static void tlp_cleanup_transaction_region(struct devemu_pci_func *func)
{
	if (func && func->tlp_handler_cxt.transaction_region_memory) {
		free(func->tlp_handler_cxt.transaction_region_memory);
		func->tlp_handler_cxt.transaction_region_memory = NULL;
		func->tlp_handler_cxt.transaction_region_size = 0;
		DOCA_LOG_INFO("Transaction region cleaned up");
	}
}

/**
 * Cleanup Expansion ROM bar memory
 *
 * @resources [in]: Pointer to TLP resources structure
 */
static void tlp_cleanup_exp_bar(struct devemu_tlp_resources *resources)
{
	/* Only the PF owns an Expansion ROM BAR */
	if (resources && resources->pf.tlp_handler_cxt.exp_bar_memory) {
		free(resources->pf.tlp_handler_cxt.exp_bar_memory);
		resources->pf.tlp_handler_cxt.exp_bar_memory = NULL;
		resources->pf.tlp_handler_cxt.exp_bar_size = 0;
		DOCA_LOG_INFO("Expansion ROM bar region cleaned up");
	}
}

/**
 * Helper function to determine byte enable mask for a specific DWORD
 *
 * @dw_idx [in]: DWORD index
 * @num_dwords [in]: Total number of DWORDs
 * @first_dw_be [in]: First DWORD byte enable
 * @last_dw_be [in]: Last DWORD byte enable
 *
 * @return: Byte enable mask for the specified DWORD
 */
static inline unsigned get_dword_byte_enable_mask(unsigned dw_idx,
						  unsigned num_dwords,
						  unsigned first_dw_be,
						  unsigned last_dw_be)
{
	if (num_dwords == 1 || dw_idx == 0) {
		return first_dw_be;
	} else if (dw_idx == num_dwords - 1) {
		return last_dw_be;
	} else {
		return BYTE_ENABLE_ALL_MASK;
	}
}

/**
 * Get the BAR0 base address of the currently selected function.
 *
 * For the PF the base comes from its own configuration-space BAR0. For a VF the BAR is owned by the
 * PF SR-IOV VF BAR0; each VF occupies a contiguous range of one BAR size within that window, so
 * VF i base = (SR-IOV VF BAR0 base) + i * (BAR size).
 *
 * @resources [in]: Pointer to TLP resources structure
 * @return: BAR0 base address of resources->cur_func
 */
static inline uint64_t get_func_bar0_base(struct devemu_tlp_resources *resources)
{
	struct devemu_pci_func *func = resources->cur_func;

	if (func->func_type == DOCA_PCI_FUNC_TYPE_VF) {
		const struct pci_config_space_ext_cap_sriov *sriov =
			&resources->pf.pci_config_space.ext_caps.sriov_ext_cap;
		uint64_t vf_bar_base = (((uint64_t)sriov->vf_bar[1]) << 32) | (sriov->vf_bar[0] & 0xFFFFFFF0);
		uint64_t bar_size = (1ULL << layout_configs[0].log_size);

		return vf_bar_base + (uint64_t)func->vf_index * bar_size;
	}

	return (((uint64_t)func->pci_config_space.header.bar[1]) << 32) |
	       (func->pci_config_space.header.bar[0] & 0xFFFFFFF0);
}

/**
 * Helper function to evaluate transaction operation parameters
 *
 * @address [in]: Transaction region address
 * @length [in]: Length in bytes
 * @resources [in]: Pointer to TLP resources structure
 * @offset [out]: Calculated offset within transaction region
 * @num_dwords [out]: Number of DWORDs
 * @transaction_memory_address [out]: Pointer to the transaction region memory
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_NOT_FOUND if no transaction region allocated or address is not
 * within the transaction region range
 */
static inline doca_error_t evaluate_transaction_operation_params(uint64_t address,
								 unsigned length,
								 struct devemu_tlp_resources *resources,
								 uint64_t *offset,
								 unsigned *num_dwords,
								 uint64_t *transaction_memory_address)
{
	if (!resources->cur_func->tlp_handler_cxt.transaction_region_memory ||
	    resources->cur_func->tlp_handler_cxt.transaction_region_size == 0) {
		DOCA_LOG_ERR("No transaction region allocated");
		return DOCA_ERROR_NOT_FOUND;
	}

	uint64_t bar_base_address = get_func_bar0_base(resources);
	bar_base_address += transaction_configs[0].start_address;
	uint64_t bar_address_end = bar_base_address + resources->cur_func->tlp_handler_cxt.transaction_region_size;

	DOCA_LOG_DBG("transaction address: 0x%lx, bar_base_address: 0x%lx, bar_address_end: 0x%lx\n",
		     address,
		     bar_base_address,
		     bar_address_end);
	if (address < bar_base_address || address >= bar_address_end) {
		DOCA_LOG_DBG("Address 0x%lx is not within the transaction region range", address);
		return DOCA_ERROR_NOT_FOUND;
	}

	*offset = address - bar_base_address;
	size_t max_size = resources->cur_func->tlp_handler_cxt.transaction_region_size - *offset;
	size_t actual_size = (length < max_size) ? length : max_size;
	*num_dwords = (actual_size + (BYTES_IN_DWORD - 1)) / BYTES_IN_DWORD;
	DOCA_LOG_DBG("Transaction operation: offset=0x%lx, size=%zu, dwords=%u", *offset, actual_size, *num_dwords);
	*transaction_memory_address = (uint64_t)(resources->cur_func->tlp_handler_cxt.transaction_region_memory);
	return DOCA_SUCCESS;
}

/**
 * Helper function to evaluate Expansion ROM bar operation parameters
 *
 * @address [in]: Expansion ROM bar address
 * @length [in]: Length in bytes
 * @resources [in]: Pointer to TLP resources structure
 * @offset [out]: Calculated offset within Expansion ROM bar
 * @num_dwords [out]: Number of DWORDs
 * @exp_bar_memory_address [out]: Pointer to the Expansion ROM bar memory
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_NOT_FOUND if no Expansion ROM bar allocated or address is not
 * within Expansion ROM bar range
 */
static inline doca_error_t evaluate_exp_bar_operation_params(uint64_t address,
							     unsigned length,
							     struct devemu_tlp_resources *resources,
							     uint64_t *offset,
							     unsigned *num_dwords,
							     uint64_t *exp_bar_memory_address)
{
	if (!resources->cur_func->tlp_handler_cxt.exp_bar_memory ||
	    resources->cur_func->tlp_handler_cxt.exp_bar_size == 0) {
		DOCA_LOG_ERR("No Expansion ROM bar region allocated");
		return DOCA_ERROR_NOT_FOUND;
	}

	uint64_t bar_base_address = resources->cur_func->pci_config_space.header.expansion_rom_base_address &
				    EXP_BAR_ADDR_MASK;
	uint64_t bar_address_end = bar_base_address + resources->cur_func->tlp_handler_cxt.exp_bar_size;

	DOCA_LOG_DBG("Expansion ROM bar address: 0x%lx, bar_base_address: 0x%lx, bar_address_end: 0x%lx\n",
		     address,
		     bar_base_address,
		     bar_address_end);
	if (address < bar_base_address || address >= bar_address_end) {
		DOCA_LOG_DBG("Address 0x%lx is not within the Expansion ROM bar range", address);
		return DOCA_ERROR_NOT_FOUND;
	}

	*offset = address - bar_base_address;
	size_t max_size = resources->cur_func->tlp_handler_cxt.exp_bar_size - *offset;
	size_t actual_size = (length < max_size) ? length : max_size;
	*num_dwords = (actual_size + (BYTES_IN_DWORD - 1)) / BYTES_IN_DWORD;
	DOCA_LOG_DBG("Expansion ROM bar operation: offset=0x%lx, size=%zu, dwords=%u",
		     *offset,
		     actual_size,
		     *num_dwords);
	*exp_bar_memory_address = (uint64_t)(resources->cur_func->tlp_handler_cxt.exp_bar_memory);
	return DOCA_SUCCESS;
}
/**
 * Get memory read data
 *
 * @address [in]: Transaction address
 * @length [in]: Length in bytes
 * @first_dw_be [in]: First DWORD byte enable
 * @last_dw_be [in]: Last DWORD byte enable
 * @resources [in]: Pointer to TLP resources structure
 * @cmpl_data_array [out]: Array to store completion data
 */
static inline void get_memory_read_data(uint64_t address,
					unsigned length,
					unsigned first_dw_be,
					unsigned last_dw_be,
					struct devemu_tlp_resources *resources,
					uint32_t *cmpl_data_array)
{
	unsigned num_dwords = length / BYTES_IN_DWORD;
	uint64_t offset;
	unsigned actual_dwords;

	if (num_dwords > TLP_TRANSACTION_CMPL_DATA_MAX_DW)
		num_dwords = TLP_TRANSACTION_CMPL_DATA_MAX_DW;
	memset(cmpl_data_array, 0, num_dwords * sizeof(uint32_t));
	DOCA_LOG_DBG("Transaction Read: address=0x%lx, length=%d, first_dw_be=0x%x, last_dw_be=0x%x",
		     address,
		     length,
		     first_dw_be,
		     last_dw_be);
	/* Try to evaluate transaction parameters; on failure, output params are unused and we return dummy data
	 */
	uint64_t memory_address;
	if (evaluate_transaction_operation_params(address, length, resources, &offset, &actual_dwords, &memory_address) !=
		    DOCA_SUCCESS &&
	    evaluate_exp_bar_operation_params(address, length, resources, &offset, &actual_dwords, &memory_address) !=
		    DOCA_SUCCESS) {
		DOCA_LOG_DBG(
			"Unable to evaluate operation parameters - returning dummy data for read (address=0x%lx, length=%d)",
			address,
			length);
		for (unsigned i = 0; i < num_dwords; i++) {
			cmpl_data_array[i] = DUMMY_READ_DATA_BASE + i;
		}
	} else {
		uint8_t *transaction_ptr = (uint8_t *)memory_address + offset;
		if (actual_dwords > num_dwords)
			actual_dwords = num_dwords;
		for (unsigned dw_idx = 0; dw_idx < actual_dwords; dw_idx++) {
			uint8_t *dword_ptr = transaction_ptr + (dw_idx * BYTES_IN_DWORD);
			uint32_t dword_data = 0;
			uint8_t *data_ptr = (uint8_t *)&dword_data;
			unsigned be_mask = get_dword_byte_enable_mask(dw_idx, num_dwords, first_dw_be, last_dw_be);

			for (unsigned byte_idx = 0; byte_idx < BYTES_IN_DWORD; byte_idx++) {
				if (be_mask & (1 << byte_idx)) {
					data_ptr[byte_idx] = dword_ptr[byte_idx];
				}
			}
			cmpl_data_array[dw_idx] = dword_data;
			DOCA_LOG_DBG("  DW[%d]: data=0x%08x, be_mask=0x%x", dw_idx, dword_data, be_mask);
		}
	}
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_W_DATA;
	resources->cur_func->tlp_handler_cxt.cmpl_length = num_dwords;
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_SC;
}

/**
 * Write data to transaction region
 *
 * @address [in]: Transaction address
 * @length [in]: Length in bytes
 * @data_array [in]: Array containing write data
 * @first_dw_be [in]: First DWORD byte enable
 * @last_dw_be [in]: Last DWORD byte enable
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void set_memory_write_data(uint64_t address,
					 unsigned length,
					 uint32_t *data_array,
					 unsigned first_dw_be,
					 unsigned last_dw_be,
					 struct devemu_tlp_resources *resources)
{
	uint64_t offset;
	unsigned num_dwords;

	DOCA_LOG_DBG("Transaction Write: address=0x%lx, length=%d, first_dw_be=0x%x, last_dw_be=0x%x",
		     address,
		     length,
		     first_dw_be,
		     last_dw_be);
	uint64_t memory_address;
	if (evaluate_transaction_operation_params(address, length, resources, &offset, &num_dwords, &memory_address) !=
	    DOCA_SUCCESS) {
		DOCA_LOG_DBG("Unable to evaluate operation parameters, write ignored");
		return;
	}
	uint8_t *transaction_ptr = (uint8_t *)memory_address + offset;
	for (unsigned dw_idx = 0; dw_idx < num_dwords; dw_idx++) {
		uint32_t dword_data = data_array[dw_idx];
		uint8_t *dword_ptr = transaction_ptr + (dw_idx * BYTES_IN_DWORD);
		uint8_t *data_ptr = (uint8_t *)&dword_data;
		unsigned be_mask = get_dword_byte_enable_mask(dw_idx, num_dwords, first_dw_be, last_dw_be);

		for (unsigned byte_idx = 0; byte_idx < BYTES_IN_DWORD; byte_idx++) {
			if (be_mask & (1 << byte_idx)) {
				dword_ptr[byte_idx] = data_ptr[byte_idx];
			}
		}
		DOCA_LOG_DBG("  DW[%d]: data=0x%08x, be_mask=0x%x", dw_idx, dword_data, be_mask);
	}
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = 0;
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO;
	resources->cur_func->tlp_handler_cxt.cmpl_status = 0;
}

/**
 * Handle TLP memory read request
 *
 * @tlp_req [in]: Pointer to TLP request
 * @resources [in]: Pointer to TLP resources structure
 * @tlp_req_header [in]: TLP request header
 */
static inline void handle_tlp_req_memory_read(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct devemu_tlp_resources *resources,
					      const void *tlp_req_header)
{
	unsigned length = GET_TLP_REQ_LENGTH(tlp_req_header);
	unsigned first_dw_be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	unsigned last_dw_be = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
	const uint32_t *tlp_dwords = (const uint32_t *)tlp_req_header;
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint64_t address;

	DOCA_LOG_DBG("Handle TLP Request for Memory Read");
	if (fmt == TLP_FMT_4DW_NODATA) {
		address = parse_64bit_address(tlp_dwords);
	} else {
		address = parse_32bit_address(tlp_dwords);
	}

	uint32_t cmpl_data_array[TLP_TRANSACTION_CMPL_DATA_MAX_DW] = {0};
	get_memory_read_data(address, length * BYTES_IN_DWORD, first_dw_be, last_dw_be, resources, cmpl_data_array);

	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(tlp_cpl_data, cmpl_data_array, resources->cur_func->tlp_handler_cxt.cmpl_length * sizeof(uint32_t));
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      resources,
				      BYTES_IN_DWORD);
	DOCA_LOG_DBG("Memory Read: addr=0x%lx, len=%d DWs, cmpl_len=%d",
		     address,
		     length,
		     resources->cur_func->tlp_handler_cxt.cmpl_length);
}

/**
 * Handle TLP memory write request
 *
 * @resources [in]: Pointer to TLP resources structure
 * @tlp_req_header [in]: TLP request header
 */
static inline void handle_tlp_req_memory_write(struct devemu_tlp_resources *resources, const void *tlp_req_header)
{
	unsigned length = GET_TLP_REQ_LENGTH(tlp_req_header);
	unsigned first_dw_be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	unsigned last_dw_be = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
	const uint32_t *tlp_dwords = (const uint32_t *)tlp_req_header;
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint64_t address;
	uint32_t data_array[TLP_DATA_ARRAY_MAX_SIZE] = {0};

	DOCA_LOG_DBG("Handle TLP Request for Memory Write");
	if (fmt == TLP_FMT_4DW_W_DATA) {
		address = parse_64bit_address(tlp_dwords);
		for (unsigned i = 0; i < length && i < TLP_DATA_ARRAY_MAX_SIZE; i++) {
			data_array[i] = tlp_dwords[TLP_64BIT_ADDR_DATA_OFFSET_DW + i];
		}
	} else {
		address = parse_32bit_address(tlp_dwords);
		for (unsigned i = 0; i < length && i < TLP_DATA_ARRAY_MAX_SIZE; i++) {
			data_array[i] = tlp_dwords[TLP_32BIT_ADDR_DATA_OFFSET_DW + i];
		}
	}
	set_memory_write_data(address, length * BYTES_IN_DWORD, data_array, first_dw_be, last_dw_be, resources);
	DOCA_LOG_DBG("Memory Write: addr=0x%lx, len=%d DWs", address, length);
}

/**
 * Handle TLP request default
 *
 * @tlp_req [in]: Pointer to TLP request
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void handle_tlp_req_default(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					  struct devemu_tlp_resources *resources)
{
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_NODATA;
	resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO;
	resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_UR;
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      resources,
				      BYTES_IN_DWORD);
}

/**
 * Handle TLP request processing
 *
 * @tlp_req [in]: Pointer to TLP request
 * @resources [in]: Pointer to TLP resources structure
 */
static inline void handle_tlp_req(struct doca_devemu_pci_tlp_channel_req *tlp_req,
				  struct devemu_tlp_resources *resources)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	resources->cur_func->tlp_handler_cxt.requester_id = GET_TLP_REQ_REQ_ID(tlp_req_header);
	resources->cur_func->tlp_handler_cxt.tag9 = GET_TLP_REQ_TAG9(tlp_req_header);
	resources->cur_func->tlp_handler_cxt.tag8 = GET_TLP_REQ_TAG8(tlp_req_header);
	resources->cur_func->tlp_handler_cxt.tag = GET_TLP_REQ_TAG(tlp_req_header);
	enum tlp_req_type tlp_type = get_tlp_req_type(tlp_req);
	uint16_t cap_id = 0;
	bool is_cap_id_valid = false;
	bool is_pcie_cap = false;

	switch (tlp_type) {
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_0: {
		handle_tlp_req_read_type_0(tlp_req, resources, ext_reg_num, &cap_id, &is_cap_id_valid, &is_pcie_cap);
		doca_devemu_pci_tlp_channel_req_complete_config_read(tlp_req,
								     resources->cur_func->tlp_dev,
								     (uint8_t)is_cap_id_valid,
								     cap_id,
								     (uint8_t)is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0: {
		handle_tlp_req_write_type_0(tlp_req,
					    resources,
					    ext_reg_num,
					    tlp_req_header,
					    &cap_id,
					    &is_cap_id_valid,
					    &is_pcie_cap);
		doca_devemu_pci_tlp_channel_req_complete_config_write(tlp_req,
								      resources->cur_func->tlp_dev,
								      (uint8_t)is_cap_id_valid,
								      cap_id,
								      (uint8_t)is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_MEMORY_READ: {
		handle_tlp_req_memory_read(tlp_req, resources, tlp_req_header);
		break;
	}
	case TLP_REQ_TYPE_MEMORY_WRITE: {
		handle_tlp_req_memory_write(resources, tlp_req_header);
		break;
	}
	default:
		DOCA_LOG_ERR("Unsupported TLP request type: %d", tlp_type);
		handle_tlp_req_default(tlp_req, resources);
		break;
	}

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req,
						     (uint8_t)is_non_posted_tlp_req(tlp_req),
						     resources->cur_func->tlp_dev);
}

/**
 * Check whether the VF at the given index is currently enabled by the PF SR-IOV capability.
 *
 * A VF responds to TLPs only once the PF has set the SR-IOV VF Enable bit and programmed NumVFs to
 * cover that VF index.
 *
 * @resources [in]: Pointer to TLP resources structure
 * @vf_index [in]: Zero-based VF index
 * @return: true if the VF is enabled, false otherwise
 */
static inline bool is_vf_enabled(struct devemu_tlp_resources *resources, unsigned vf_index)
{
	const struct pci_config_space_ext_cap_sriov *sriov = &resources->pf.pci_config_space.ext_caps.sriov_ext_cap;

	return (sriov->reg2.fields.control & SRIOV_CTRL_VF_ENABLE) && (vf_index < sriov->reg4.fields.num_vfs);
}

/**
 * Route a config (type 0) TLP to the target function based on its BDF.
 *
 * All emulated functions share a single PCIe endpoint at device 0: function 0 is the PF and
 * functions [first_vf_offset, first_vf_offset + total_vfs - 1] are the VFs. The bus number is
 * captured dynamically to tolerate host reboots that reassign bus numbers. On success
 * resources->cur_func is repointed to the targeted function.
 *
 * @tlp_req [in]: Pointer to TLP request
 * @resources [in]: Pointer to TLP resources structure
 * @return: true if the targeted function exists and may respond, false otherwise (=> UR)
 */
static bool route_config_tlp(struct doca_devemu_pci_tlp_channel_req *tlp_req, struct devemu_tlp_resources *resources)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint8_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint8_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint8_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);
	struct devemu_pci_func *func;

	/* All functions of this endpoint live on device 0 */
	if (device != 0)
		return false;

	if (function == 0) {
		func = &resources->pf;
	} else {
		/* VFs occupy contiguous function numbers starting at SRIOV_FIRST_VF_OFFSET (stride 1) */
		unsigned vf_index = function - SRIOV_FIRST_VF_OFFSET;

		if (resources->total_vfs == 0 || function < SRIOV_FIRST_VF_OFFSET || vf_index >= resources->total_vfs ||
		    !is_vf_enabled(resources, vf_index))
			return false;

		func = &resources->vf[vf_index];
	}

	resources->cur_func = func;
	func->tlp_handler_cxt.bus = bus;
	func->tlp_handler_cxt.device = device;
	func->tlp_handler_cxt.function = function;
	func->tlp_handler_cxt.bdf = (bus << 8) | (device << DWORD_LOG_SIZE_BITS) | function;
	func->tlp_handler_cxt.completer_id = func->tlp_handler_cxt.bdf;

	/* Keep the PF bus up to date so VF MMIO completer IDs can be derived from it */
	resources->pf.tlp_handler_cxt.bus = bus;

	return true;
}

/**
 * Parse the target address of a memory (MMIO) TLP from its header.
 *
 * @tlp_req_header [in]: Pointer to TLP request header
 * @return: 64-bit target address
 */
static inline uint64_t get_mmio_address(const void *tlp_req_header)
{
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);

	if (fmt == TLP_FMT_4DW_NODATA || fmt == TLP_FMT_4DW_W_DATA)
		return parse_64bit_address((const uint32_t *)tlp_req_header);

	return parse_32bit_address((const uint32_t *)tlp_req_header);
}

/**
 * Route a memory (MMIO) TLP to the target function based on its address.
 *
 * The PF uses its own BAR in the configuration space, while all VF BARs are carved contiguously out of the PF
 * SR-IOV VF BAR0 window (VF i occupies a BAR size chunk at offset i). VF memory only responds when
 * the PF SR-IOV VF Enable and VF Memory Space Enable (MSE) bits are both set and the VF is in range.
 * Any address that does not fall in a VF BAR memory range defaults to the PF.
 *
 * @resources [in]: Pointer to TLP resources structure
 * @address [in]: MMIO target address
 * @return: pointer to the target function, or NULL if it targets disabled VF memory (=> UR)
 */
static struct devemu_pci_func *route_mmio_tlp(struct devemu_tlp_resources *resources, uint64_t address)
{
	const struct pci_config_space_ext_cap_sriov *sriov;
	uint64_t bar_size;
	uint64_t vf_bar_base;

	if (resources->total_vfs == 0)
		return &resources->pf;

	sriov = &resources->pf.pci_config_space.ext_caps.sriov_ext_cap;
	bar_size = (1ULL << layout_configs[0].log_size);
	vf_bar_base = (((uint64_t)sriov->vf_bar[1]) << 32) | (sriov->vf_bar[0] & 0xFFFFFFF0);

	if (vf_bar_base != 0 && address >= vf_bar_base &&
	    address < vf_bar_base + (uint64_t)resources->total_vfs * bar_size) {
		unsigned vf_index = (unsigned)((address - vf_bar_base) / bar_size);
		struct devemu_pci_func *vf = &resources->vf[vf_index];
		/* It is possible to receive MMIO TLPs to VF memory before its BDF is captured (otherwise we would
		 * simply take vf->tlp_handler_cxt->function). */
		uint8_t function = SRIOV_FIRST_VF_OFFSET + vf_index * SRIOV_VF_STRIDE;

		/* VF memory is accessible only when VF Enable + VF MSE are set and the VF is enabled */
		if (!(sriov->reg2.fields.control & SRIOV_CTRL_VF_MSE) || !is_vf_enabled(resources, vf_index))
			return NULL;

		/* Derive the VF completer ID from the captured PF bus and the VF function number (device is 0) */
		vf->tlp_handler_cxt.completer_id = (resources->pf.tlp_handler_cxt.bus << 8) | function;
		return vf;
	}

	return &resources->pf;
}

/**
 * Handle PCI_EVENT request
 *
 * @tlp_req [in]: Pointer to TLP request
 */
static void handle_tlp_req_pci_event(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	enum doca_devemu_pci_tlp_channel_req_pci_event_opmode pci_event_opmode =
		doca_devemu_pci_tlp_channel_req_get_pci_event_opmode(tlp_req);
	switch (pci_event_opmode) {
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_ASSERT:
		DOCA_LOG_DBG("PERST# is asserted (enters reset)");
		break;
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_DEASSERT:
		DOCA_LOG_DBG("PERST# is deasserted (released from reset)");
		break;
	default:
		DOCA_LOG_ERR("Unknown PCI_EVENT opmode: %d", pci_event_opmode);
		break;
	}

	/* Complete the PCI_EVENT request */
	doca_devemu_pci_tlp_channel_req_complete_pci_event(tlp_req);
}

/**
 * TLP request handler callback function
 *
 * @channel [in]: Pointer to TLP channel
 * @tlp_req [in]: Pointer to TLP request
 * @req_user_data [in]: User data associated with the request
 */
static void tlp_req_handler_cb(struct doca_devemu_pci_tlp_channel *channel,
			       struct doca_devemu_pci_tlp_channel_req *tlp_req,
			       void *req_user_data)
{
	(void)req_user_data;

	struct devemu_tlp_resources *resources;
	union doca_data channel_user_data;
	doca_error_t result;

	result = doca_ctx_get_user_data(doca_devemu_pci_tlp_channel_as_ctx(channel), &channel_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from tlp channel context: %s", doca_error_get_descr(result));
		return;
	}
	resources = (struct devemu_tlp_resources *)channel_user_data.ptr;

	DOCA_LOG_DBG("TLP Channel Request received");

	enum doca_devemu_pci_tlp_channel_req_opcode opcode = doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req);
	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_PCI_EVENT) {
		handle_tlp_req_pci_event(tlp_req);
		return;
	}

	enum tlp_req_type req_type = get_tlp_req_type(tlp_req);

	/* Default to servicing the PF; routing repoints cur_func to the addressed function below */
	resources->cur_func = &resources->pf;

	if (req_type == TLP_REQ_TYPE_CONFIG_READ_TYPE_0 || req_type == TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0) {
		/* For dsp_id > 0, return UR using the DSP's own BDF as completer_id to avoid
		 * having to define a whole upstream port for unused DSPs.
		 */
		if (resources->is_dsp_bdf_supported) {
			uint8_t dsp_id = doca_devemu_pci_tlp_channel_req_get_dsp_id(tlp_req);
			if (dsp_id != 0 && dsp_id < DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_DSP_ID_IMPLICIT) {
				resources->cur_func->tlp_handler_cxt.completer_id =
					doca_devemu_pci_tlp_channel_req_get_dsp_bdf(tlp_req);
				handle_tlp_req_default(tlp_req, resources);
				doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, resources->cur_func->tlp_dev);
				return;
			}
		}

		/* Route config TLPs by BDF (also captures the bus number to tolerate host reboots) */
		if (!route_config_tlp(tlp_req, resources)) {
			/* Non-existent or disabled function => Unsupported Request */
			resources->cur_func = &resources->pf;
			resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_NODATA;
			resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO;
			resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_UR;
			set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
						      resources,
						      BYTES_IN_DWORD);
			doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, resources->cur_func->tlp_dev);
			return;
		}
	} else if (req_type == TLP_REQ_TYPE_MEMORY_READ || req_type == TLP_REQ_TYPE_MEMORY_WRITE) {
		/* Route MMIO TLPs by address to the owning function */
		const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
		struct devemu_pci_func *target = route_mmio_tlp(resources, get_mmio_address(tlp_req_header));

		if (target == NULL) {
			/* MMIO to disabled VF memory => UR for reads (non-posted), silently drop writes */
			resources->cur_func = &resources->pf;
			resources->cur_func->tlp_handler_cxt.cmpl_fmt = TLP_FMT_CPL_NODATA;
			resources->cur_func->tlp_handler_cxt.cmpl_length = TLP_CPL_LENGTH_ZERO;
			resources->cur_func->tlp_handler_cxt.cmpl_status = TLP_CPL_STATUS_UR;
			set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
						      resources,
						      BYTES_IN_DWORD);
			doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req,
								     (uint8_t)is_non_posted_tlp_req(tlp_req),
								     resources->cur_func->tlp_dev);
			return;
		}
		resources->cur_func = target;
	}

	/* Reset the completion fields, before handle request */
	resources->cur_func->tlp_handler_cxt.cmpl_fmt = 0;
	resources->cur_func->tlp_handler_cxt.cmpl_length = 0;
	resources->cur_func->tlp_handler_cxt.cmpl_status = 0;
	handle_tlp_req(tlp_req, resources);
}

/**
 * Configure and start PCI TLP type
 *
 * @pci_type [in]: Pointer to PCI type object
 * @dev [in]: Pointer to DOCA device
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t configure_and_start_pci_tlp_type(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev)
{
	doca_error_t result;
	const struct bar_memory_layout_config *layout_config;
	const struct bar_db_region_config *db_config;
	const struct bar_region_config *region_config;
	int idx;

	result = doca_devemu_pci_type_set_dev(pci_type, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set device for PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	for (idx = 0; idx < TLP_PCI_CAPS_NUM; idx++) {
		result = doca_devemu_pci_tlp_type_set_pci_cap_conf(pci_type,
								   tlp_pci_cap_confs[idx].id,
								   tlp_pci_cap_confs[idx].offset,
								   tlp_pci_cap_confs[idx].length);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set PCI capabilityconfiguration at idx %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	for (idx = 0; idx < TLP_PCIE_CAPS_NUM; idx++) {
		result = doca_devemu_pci_tlp_type_set_pcie_cap_conf(pci_type,
								    tlp_pcie_cap_confs[idx].id,
								    tlp_pcie_cap_confs[idx].offset,
								    tlp_pcie_cap_confs[idx].length);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set PCIE capabilityconfiguration at idx %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MEMORY_LAYOUT; ++idx) {
		layout_config = &layout_configs[idx];
		result = doca_devemu_pci_type_set_memory_bar_conf(pci_type,
								  layout_config->bar_id,
								  layout_config->log_size,
								  layout_config->memory_type,
								  layout_config->prefetchable);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set layout at index %d: %s", idx, doca_error_get_descr(result));
			return result;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_DB_REGIONS; ++idx) {
		db_config = &db_configs[idx];
		if (db_config->with_data)
			result = doca_devemu_pci_type_set_bar_db_region_by_data_conf(pci_type,
										     db_config->region.bar_id,
										     db_config->region.start_address,
										     db_config->region.size,
										     db_config->log_db_size,
										     db_config->db_id_msbyte,
										     db_config->db_id_lsbyte);
		else
			result = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(pci_type,
										       db_config->region.bar_id,
										       db_config->region.start_address,
										       db_config->region.size,
										       db_config->log_db_size,
										       db_config->log_db_stride_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set DB region at index %d: %s", idx, doca_error_get_descr(result));
			return result;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_TRANSACTION_REGIONS; ++idx) {
		region_config = &transaction_configs[idx];
		result = doca_devemu_pci_tlp_type_set_bar_transaction_region_conf(pci_type,
										  region_config->bar_id,
										  region_config->start_address,
										  region_config->size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set transaction region at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_TABLE_REGIONS; ++idx) {
		region_config = &msix_table_configs[idx];
		result = doca_devemu_pci_type_set_bar_msix_table_region_conf(pci_type,
									     region_config->bar_id,
									     region_config->start_address,
									     region_config->size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X table region at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_PBA_REGIONS; ++idx) {
		region_config = &msix_pba_configs[idx];
		result = doca_devemu_pci_type_set_bar_msix_pba_region_conf(pci_type,
									   region_config->bar_id,
									   region_config->start_address,
									   region_config->size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X pending bit array region at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	result = doca_devemu_pci_type_start(pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Initialize TLP channel and register TLP request handler callback
 *
 * @resources [in]: Pointer to TLP resources structure
 * @export_desc [in]: Pointer to export descriptor (optional)
 * @export_desc_len [in]: Length of export descriptor (optional depending on export_desc)
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_tlp_channel(struct devemu_tlp_resources *resources, void *export_desc, size_t export_desc_len)
{
	doca_error_t result;

	result = doca_pe_create(&resources->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	if (export_desc != NULL && export_desc_len > 0) {
		result = doca_devemu_pci_tlp_channel_create_from_export(export_desc,
									export_desc_len,
									resources->shm_dir_path,
									resources->channel_dev,
									&resources->tlp_channel);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create TLP channel from export: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set TLP channel as primary: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_devemu_pci_tlp_channel_create(resources->channel_dev, &resources->tlp_channel);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create TLP channel: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set TLP channel as primary: %s", doca_error_get_descr(result));
			return result;
		}

		/* Path can be empty for standalone mode, library accepts it */
		result = doca_devemu_pci_tlp_channel_set_shm_dir_path(resources->tlp_channel, resources->shm_dir_path);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set TLP channel shm dir path: %s", doca_error_get_descr(result));
			return result;
		}

		uint16_t max_np_rsp_timeout = 0;
		result = doca_devemu_pci_tlp_cap_get_max_np_rsp_timeout(doca_dev_as_devinfo(resources->channel_dev),
									&max_np_rsp_timeout);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get maximum non-posted response timeout: %s",
				     doca_error_get_descr(result));
			return result;
		}

		if (max_np_rsp_timeout != 0) {
			result = doca_devemu_pci_tlp_channel_set_np_rsp_timeout(resources->tlp_channel,
										max_np_rsp_timeout);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to set non-posted response timeout: %s",
					     doca_error_get_descr(result));
				return result;
			}
		}
	}

	result = doca_devemu_pci_tlp_channel_set_req_user_data_size(resources->tlp_channel,
								    TLP_CHANNEL_CTX_USER_DATA_SIZE_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set TLP request user data size: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_tlp_channel_event_req_register(resources->tlp_channel, tlp_req_handler_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register TLP request handler: %s", doca_error_get_descr(result));
		return result;
	}

	union doca_data user_data = {0};
	user_data.ptr = resources;
	result = doca_devemu_pci_tlp_channel_event_fatal_state_register(resources->tlp_channel,
									tlp_channel_event_fatal_state_handler_cb,
									user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register TLP channel fatal state handler: %s", doca_error_get_descr(result));
		return result;
	}

	resources->channel_ctx = doca_devemu_pci_tlp_channel_as_ctx(resources->tlp_channel);

	result = doca_pe_connect_ctx(resources->pe, resources->channel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PE for TLP channel context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_set_user_data(resources->channel_ctx, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set user data for TLP channel context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(resources->channel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start TLP channel context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_tlp_cap_is_channel_req_dsp_bdf_supported(doca_dev_as_devinfo(resources->channel_dev),
									  &resources->is_dsp_bdf_supported);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query DSP BDF capability: %s", doca_error_get_descr(result));
		return result;
	}

	/* The sample implements a single PCIe endpoint and is designed to support exactly one channel downstream port.
	 * If querying the DSP BDF is supported, then we can return unsupported request completions for DSPs with
	 * dsp_id > 0, otherwise we need to verify that only one DSP is present. */
	if (!resources->is_dsp_bdf_supported) {
		uint8_t num_dsp = 0;
		result = doca_devemu_pci_tlp_channel_get_num_dsp(resources->tlp_channel, &num_dsp);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get number of downstream ports: %s", doca_error_get_descr(result));
			return result;
		}

		if (num_dsp != 1) {
			DOCA_LOG_ERR(
				"Sample does not support TLP channel with more or less than one downstream port, use mlxconfig to set the number of TLP ports to 1");
			return DOCA_ERROR_NOT_SUPPORTED;
		}
	}

	return DOCA_SUCCESS;
}

/* Forward declarations of the devices (PF, and VFs if SR-IOV is enabled) setup/cleanup helpers, shared between the main
 * setup flow, the cleanup flow and the channel fatal-state recovery handler. */
static doca_error_t tlp_devices_setup(struct devemu_tlp_resources *resources);
static void tlp_devices_cleanup(struct devemu_tlp_resources *resources);

/**
 * TLP channel fatal state handler callback function
 *
 * Teardown the devices, Stops the channel, drains all outstanding TLP requests, destroys the channel, and then
 * re-creates and starts the channel, as well as the devices.
 *
 * @channel [in]: Pointer to TLP channel
 * @user_data [in]: User data associated with the channel fatal state event registration
 */
static void tlp_channel_event_fatal_state_handler_cb(struct doca_devemu_pci_tlp_channel *channel,
						     union doca_data user_data)
{
	(void)channel; /* This sample uses only one channel, so we know it's the same one as in the resources */
	doca_error_t result;

	DOCA_LOG_DBG("TLP channel entered fatal state");

	struct devemu_tlp_resources *resources = (struct devemu_tlp_resources *)user_data.ptr;

	tlp_devices_cleanup(resources);

	result = doca_ctx_stop(resources->channel_ctx);
	if (result == DOCA_ERROR_IN_PROGRESS) {
		/* Drain the TLP channel */
		enum doca_ctx_states ctx_state;
		do {
			(void)doca_pe_progress(resources->pe);
			result = doca_ctx_get_state(resources->channel_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get state of channel context: %s",
					     doca_error_get_descr(result));
				force_quit = true;
				return;
			}
		} while (ctx_state != DOCA_CTX_STATE_IDLE);
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop TLP channel context: %s", doca_error_get_descr(result));
		force_quit = true;
		return;
	}

	resources->channel_ctx = NULL;

	result = doca_devemu_pci_tlp_channel_destroy(resources->tlp_channel);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy TLP channel: %s", doca_error_get_descr(result));
		force_quit = true;
		return;
	}

	resources->tlp_channel = NULL;

	result = doca_pe_destroy(resources->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(result));
		force_quit = true;
		return;
	}

	resources->pe = NULL;

	result = init_tlp_channel(resources, NULL, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to initialize TLP channel: %s", doca_error_get_descr(result));
		force_quit = true;
		return;
	}

	result = init_rep_dev(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to recreate representor device: %s", doca_error_get_descr(result));
		force_quit = true;
		return;
	}

	result = tlp_devices_setup(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to recreate TLP devices: %s", doca_error_get_descr(result));
		force_quit = true;
		return;
	}
}

/**
 * Request handover setup from the source
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t request_handover_setup(struct devemu_tlp_resources *resources)
{
	doca_error_t result;
	enum tlp_channel_handover_msg msg;

	msg = TLP_CHANNEL_HANDOVER_MSG_SETUP;
	DOCA_LOG_INFO("Destination -> Source: SETUP");
	result = lu_oob_send(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover setup message: %s", doca_error_get_descr(result));
		return result;
	}

	result = lu_oob_recv(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive handover setup message: %s", doca_error_get_descr(result));
		return result;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_SETUP_NACK) {
		doca_error_t error;
		result = lu_oob_recv(resources->dst_socket, &error, sizeof(error));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive error: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_ERR("Received handover message SETUP_NACK from the source: %s", doca_error_get_descr(error));
		return error;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_SETUP_ACK) {
		DOCA_LOG_INFO("Destination <- Source: SETUP_ACK");

		int cmd_fd = -1;
		result = lu_oob_recv_fd(resources->dst_socket, &cmd_fd);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive IBV context FD: %s", doca_error_get_descr(result));
			return result;
		}

		if (cmd_fd < 0) {
			DOCA_LOG_ERR("SETUP_ACK missing IBV context FD");
			return DOCA_ERROR_INVALID_VALUE;
		}

		/* The source and destination must emulate the exact same number of VFs, otherwise the
		 * per-function state transferred later would not line up. Fail fast on mismatch. */
		uint16_t src_total_vfs;
		result = lu_oob_recv(resources->dst_socket, &src_total_vfs, sizeof(src_total_vfs));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive total VFs: %s", doca_error_get_descr(result));
			(void)close(cmd_fd);
			return result;
		}
		if (src_total_vfs != resources->total_vfs) {
			DOCA_LOG_ERR("Handover VF count mismatch: source has %u VFs, destination has %u VFs",
				     src_total_vfs,
				     resources->total_vfs);
			(void)close(cmd_fd);
			return DOCA_ERROR_BAD_CONFIG;
		}

		resources->ibv_ctx = ibv_import_device(cmd_fd);
		if (resources->ibv_ctx == NULL) {
			DOCA_LOG_ERR("Failed to import device: %s", strerror(errno));
			(void)close(cmd_fd);
			return DOCA_ERROR_DRIVER;
		}

		resources->ibv_pd = ibv_alloc_pd(resources->ibv_ctx);
		if (resources->ibv_pd == NULL) {
			DOCA_LOG_ERR("Failed to allocate PD: %s", strerror(errno));
			return DOCA_ERROR_NO_MEMORY;
		}

		result = doca_rdma_bridge_open_dev_from_pd(resources->ibv_pd, &resources->channel_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open device from PD: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		DOCA_LOG_ERR("Received unexpected handover message: %d (expected %d or %d)",
			     msg,
			     TLP_CHANNEL_HANDOVER_MSG_SETUP_ACK,
			     TLP_CHANNEL_HANDOVER_MSG_SETUP_NACK);
		return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/**
 * Request handover export from the source
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t request_handover_export(struct devemu_tlp_resources *resources)
{
	doca_error_t result;
	enum tlp_channel_handover_msg msg;

	msg = TLP_CHANNEL_HANDOVER_MSG_EXPORT;
	DOCA_LOG_INFO("Destination -> Source: EXPORT");
	result = lu_oob_send(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover export message: %s", doca_error_get_descr(result));
		return result;
	}

	result = lu_oob_recv(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive handover export message: %s", doca_error_get_descr(result));
		return result;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_EXPORT_NACK) {
		doca_error_t error;
		result = lu_oob_recv(resources->dst_socket, &error, sizeof(error));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive error: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_ERR("Received handover message EXPORT_NACK from the source: %s", doca_error_get_descr(error));
		return error;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_EXPORT_ACK) {
		DOCA_LOG_INFO("Destination <- Source: EXPORT_ACK");

		size_t export_desc_len;
		result = lu_oob_recv(resources->dst_socket, &export_desc_len, sizeof(export_desc_len));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive export descriptor length: %s", doca_error_get_descr(result));
			return result;
		}

		if (export_desc_len == 0) {
			DOCA_LOG_ERR("Received export des length is 0");
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (export_desc_len > (size_t)UINT16_MAX) {
			DOCA_LOG_ERR("Received export descriptor length %zu exceeds maximum", export_desc_len);
			return DOCA_ERROR_INVALID_VALUE;
		}

		uint8_t *export_desc = malloc(export_desc_len);
		if (export_desc == NULL) {
			DOCA_LOG_ERR("Failed to allocate export descriptor buffer");
			return DOCA_ERROR_NO_MEMORY;
		}

		result = lu_oob_recv(resources->dst_socket, export_desc, export_desc_len);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive export descriptor: %s", doca_error_get_descr(result));
			free(export_desc);
			return result;
		}

		result = init_tlp_channel(resources, (void *)export_desc, export_desc_len);
		free(export_desc);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize TLP channel: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		DOCA_LOG_ERR("Received unexpected handover message: %d (expected %d or %d)",
			     msg,
			     TLP_CHANNEL_HANDOVER_MSG_EXPORT_ACK,
			     TLP_CHANNEL_HANDOVER_MSG_EXPORT_NACK);
		return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/**
 * Receive and apply a single function's (PF or VF) state during handover begin
 *
 * @resources [in]: Pointer to TLP resources structure
 * @func [in/out]: The function whose state should be restored from the source
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t recv_handover_func_state(struct devemu_tlp_resources *resources, struct devemu_pci_func *func)
{
	struct pci_config_space pci_config_space;
	struct tlp_handler_cxt tlp_handler_cxt;
	void *tmp_transaction_region;
	void *tmp_exp_bar_memory;
	doca_error_t result;

	result = lu_oob_recv(resources->dst_socket, &pci_config_space, sizeof(struct pci_config_space));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive PCI config space: %s", doca_error_get_descr(result));
		return result;
	}

	result = lu_oob_recv(resources->dst_socket, &tlp_handler_cxt, sizeof(struct tlp_handler_cxt));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive TLP handler context: %s", doca_error_get_descr(result));
		return result;
	}

	tmp_transaction_region = func->tlp_handler_cxt.transaction_region_memory;
	result = lu_oob_recv(resources->dst_socket, tmp_transaction_region, PCI_TYPE_MAX_TRANSACTION_REGION_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive transaction region: %s", doca_error_get_descr(result));
		return result;
	}

	tmp_exp_bar_memory = func->tlp_handler_cxt.exp_bar_memory;
	if (func->func_type == DOCA_PCI_FUNC_TYPE_PF) {
		result = lu_oob_recv(resources->dst_socket, tmp_exp_bar_memory, EXP_BAR_SIZE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive exp bar memory: %s", doca_error_get_descr(result));
			return result;
		}
	}

	memcpy(&func->pci_config_space, &pci_config_space, sizeof(struct pci_config_space));
	memcpy(&func->tlp_handler_cxt, &tlp_handler_cxt, sizeof(struct tlp_handler_cxt));
	func->tlp_handler_cxt.transaction_region_memory = tmp_transaction_region;
	func->tlp_handler_cxt.exp_bar_memory = tmp_exp_bar_memory;

	return DOCA_SUCCESS;
}

/**
 * Request handover begin from the source
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t request_handover_begin(struct devemu_tlp_resources *resources)
{
	doca_error_t result;
	enum tlp_channel_handover_msg msg;

	msg = TLP_CHANNEL_HANDOVER_MSG_BEGIN;
	DOCA_LOG_INFO("Destination -> Source: BEGIN");
	result = lu_oob_send(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover begin message: %s", doca_error_get_descr(result));
		return result;
	}

	result = lu_oob_recv(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive handover begin message: %s", doca_error_get_descr(result));
		return result;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_BEGIN_NACK) {
		doca_error_t error;
		result = lu_oob_recv(resources->dst_socket, &error, sizeof(error));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to receive error: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_ERR("Received handover message BEGIN_NACK from the source");
		return error;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_BEGIN_ACK) {
		DOCA_LOG_INFO("Destination <- Source: BEGIN_ACK");

		/* Restore the PF state, then each VF state */
		result = recv_handover_func_state(resources, &resources->pf);
		if (result != DOCA_SUCCESS)
			return result;

		for (uint16_t i = 0; i < resources->total_vfs; i++) {
			result = recv_handover_func_state(resources, &resources->vf[i]);
			if (result != DOCA_SUCCESS)
				return result;
		}

		result = doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set TLP channel as primary: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		DOCA_LOG_ERR("Received unexpected handover message: %d (expected %d or %d)",
			     msg,
			     TLP_CHANNEL_HANDOVER_MSG_BEGIN_ACK,
			     TLP_CHANNEL_HANDOVER_MSG_BEGIN_NACK);
		return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/**
 * Request handover end from the source
 *
 * @resources [in]: Pointer to TLP resources structure
 * @is_handover_successful [in]: Flag indicating if the handover was successful
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t request_handover_end(struct devemu_tlp_resources *resources, uint8_t is_handover_successful)
{
	doca_error_t result;
	enum tlp_channel_handover_msg msg;

	msg = TLP_CHANNEL_HANDOVER_MSG_END;
	DOCA_LOG_INFO("Destination -> Source: END");
	result = lu_oob_send(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover end message: %s", doca_error_get_descr(result));
		/* Source process won't get the END message and will rollback */
		(void)doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 0);
		return result;
	}

	result = lu_oob_send(resources->dst_socket, &is_handover_successful, sizeof(is_handover_successful));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover end status: %s", doca_error_get_descr(result));
		/* Source process won't get the END message status and will rollback */
		(void)doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 0);
		return result;
	}

	result = lu_oob_recv(resources->dst_socket, &msg, sizeof(msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive handover end acknowledgement message: %s",
			     doca_error_get_descr(result));
		/* Assuming source process had a failure during the handover end, at this point the destination process
		 * can still take over */
		return DOCA_SUCCESS;
	}

	if (msg == TLP_CHANNEL_HANDOVER_MSG_END_ACK) {
		DOCA_LOG_INFO("Destination <- Source: END_ACK (handover completed)");
	} else {
		DOCA_LOG_ERR("Received unexpected handover message: %d (expected %d)",
			     msg,
			     TLP_CHANNEL_HANDOVER_MSG_END_ACK);
		return DOCA_ERROR_UNEXPECTED;
	}

	/* Only take channel ownership when the handover actually succeeded */
	if (is_handover_successful)
		resources->is_channel_owner = true;

	return DOCA_SUCCESS;
}

/**
 * Initiate TLP channel handover as the destination process that will take over the TLP channel from the source process
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t initiate_tlp_channel_handover(struct devemu_tlp_resources *resources)
{
	doca_error_t result;

	result = tlp_oob_client_connect(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect to TLP channel handover: %s", doca_error_get_descr(result));
		return result;
	}

	result = request_handover_setup(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to request handover setup: %s", doca_error_get_descr(result));
		return result;
	}

	result = request_handover_export(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to request handover export: %s", doca_error_get_descr(result));
		return result;
	}

	result = request_handover_begin(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to request handover begin: %s", doca_error_get_descr(result));
		(void)request_handover_end(resources, false);
		return result;
	}

	result = request_handover_end(resources, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to request handover end: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Handle handover setup message from the destination
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_handover_setup(struct devemu_tlp_resources *resources)
{
	enum tlp_channel_handover_msg response;
	struct ibv_pd *pd;
	doca_error_t result;

	DOCA_LOG_INFO("Source <- Destination: SETUP");

	result = doca_rdma_bridge_get_dev_pd(resources->channel_dev, &pd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PD for device: %s", doca_error_get_descr(result));
		response = TLP_CHANNEL_HANDOVER_MSG_SETUP_NACK;
		(void)lu_oob_send(resources->src_socket, &response, sizeof(response));
		(void)lu_oob_send(resources->src_socket, &result, sizeof(result));
		DOCA_LOG_INFO("Source -> Destination: SETUP_NACK");
		return result;
	}

	response = TLP_CHANNEL_HANDOVER_MSG_SETUP_ACK;
	result = lu_oob_send(resources->src_socket, &response, sizeof(response));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover setup ack message: %s", doca_error_get_descr(result));
		return result;
	}
	result = lu_oob_send_fd(resources->src_socket, pd->context->cmd_fd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send file descriptor: %s", doca_error_get_descr(result));
		return result;
	}
	/* Tell the destination how many VFs it must emulate so it can reject a mismatched configuration */
	result = lu_oob_send(resources->src_socket, &resources->total_vfs, sizeof(resources->total_vfs));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send total VFs: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Source -> Destination: SETUP_ACK");
	return DOCA_SUCCESS;
}

/**
 * Handle handover export message from the destination
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_handover_export(struct devemu_tlp_resources *resources)
{
	enum tlp_channel_handover_msg response;
	const void *export_desc;
	size_t export_desc_len;
	doca_error_t result;

	DOCA_LOG_INFO("Source <- Destination: EXPORT");

	result = doca_devemu_pci_tlp_channel_export(resources->tlp_channel, &export_desc, &export_desc_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to export TLP channel: %s", doca_error_get_descr(result));
		response = TLP_CHANNEL_HANDOVER_MSG_EXPORT_NACK;
		(void)lu_oob_send(resources->src_socket, &response, sizeof(response));
		(void)lu_oob_send(resources->src_socket, &result, sizeof(result));
		DOCA_LOG_INFO("Source -> Destination: EXPORT_NACK");
		return result;
	}

	response = TLP_CHANNEL_HANDOVER_MSG_EXPORT_ACK;
	result = lu_oob_send(resources->src_socket, &response, sizeof(response));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover export ack message: %s", doca_error_get_descr(result));
		return result;
	}
	result = lu_oob_send(resources->src_socket, &export_desc_len, sizeof(export_desc_len));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send export descriptor length: %s", doca_error_get_descr(result));
		return result;
	}
	result = lu_oob_send(resources->src_socket, export_desc, export_desc_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send export descriptor: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Source -> Destination: EXPORT_ACK");

	return DOCA_SUCCESS;
}

/**
 * Send a single function's (PF or VF) state during handover begin
 *
 * @resources [in]: Pointer to TLP resources structure
 * @func [in]: The function whose state should be sent to the destination
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_handover_func_state(struct devemu_tlp_resources *resources, struct devemu_pci_func *func)
{
	doca_error_t result;

	result = lu_oob_send(resources->src_socket, &func->pci_config_space, sizeof(struct pci_config_space));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send PCI config space: %s", doca_error_get_descr(result));
		return result;
	}

	result = lu_oob_send(resources->src_socket, &func->tlp_handler_cxt, sizeof(struct tlp_handler_cxt));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send TLP handler context: %s", doca_error_get_descr(result));
		return result;
	}

	result = lu_oob_send(resources->src_socket,
			     func->tlp_handler_cxt.transaction_region_memory,
			     PCI_TYPE_MAX_TRANSACTION_REGION_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send transaction region: %s", doca_error_get_descr(result));
		return result;
	}

	if (func->func_type == DOCA_PCI_FUNC_TYPE_PF) {
		result = lu_oob_send(resources->src_socket, func->tlp_handler_cxt.exp_bar_memory, EXP_BAR_SIZE);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send exp bar memory: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/**
 * Handle handover begin message from the destination
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_handover_begin(struct devemu_tlp_resources *resources)
{
	enum tlp_channel_handover_msg response = TLP_CHANNEL_HANDOVER_MSG_INVALID;
	doca_error_t result;

	DOCA_LOG_INFO("Source <- Destination: BEGIN");

	result = doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set TLP channel as primary: %s", doca_error_get_descr(result));
		goto send_nack;
	}

	result = doca_ctx_stop(resources->channel_ctx);
	if (result == DOCA_ERROR_IN_PROGRESS) {
		/* Drain the TLP channel */
		enum doca_ctx_states ctx_state;
		do {
			(void)doca_pe_progress(resources->pe);
			result = doca_ctx_get_state(resources->channel_ctx, &ctx_state);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get state of channel context: %s",
					     doca_error_get_descr(result));
				goto send_nack;
			}
		} while (ctx_state != DOCA_CTX_STATE_IDLE);
		resources->channel_ctx = NULL;
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop TLP channel context: %s", doca_error_get_descr(result));
		goto send_nack;
	} else {
		resources->channel_ctx = NULL;
	}

	response = TLP_CHANNEL_HANDOVER_MSG_BEGIN_ACK;
	result = lu_oob_send(resources->src_socket, &response, sizeof(response));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send handover begin ack message: %s", doca_error_get_descr(result));
		return result;
	}
	/* Send the PF state, then each VF state */
	result = send_handover_func_state(resources, &resources->pf);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint16_t i = 0; i < resources->total_vfs; i++) {
		result = send_handover_func_state(resources, &resources->vf[i]);
		if (result != DOCA_SUCCESS)
			return result;
	}

	DOCA_LOG_INFO("Source -> Destination: BEGIN_ACK");
	return DOCA_SUCCESS;

send_nack:
	response = TLP_CHANNEL_HANDOVER_MSG_BEGIN_NACK;
	(void)lu_oob_send(resources->src_socket, &response, sizeof(response));
	(void)lu_oob_send(resources->src_socket, &result, sizeof(result));
	DOCA_LOG_INFO("Source -> Destination: BEGIN_NACK");
	return result;
}

/**
 * Handle handover end message from the destination
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_handover_end(struct devemu_tlp_resources *resources)
{
	enum tlp_channel_handover_msg response;
	uint8_t is_handover_successful;
	doca_error_t result;

	DOCA_LOG_INFO("Source <- Destination: END");

	result = lu_oob_recv(resources->src_socket, &is_handover_successful, sizeof(is_handover_successful));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to receive handover end status: %s", doca_error_get_descr(result));
		return result;
	}

	if (is_handover_successful == 0) {
		DOCA_LOG_ERR("Destination process failed to complete the handover, attempting rollback");
		result = doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set primary channel: %s", doca_error_get_descr(result));
			force_quit = true;
			return result;
		}
		resources->channel_ctx = doca_devemu_pci_tlp_channel_as_ctx(resources->tlp_channel);
		result = doca_ctx_start(resources->channel_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start channel context: %s", doca_error_get_descr(result));
			force_quit = true;
			return result;
		}
	} else {
		DOCA_LOG_INFO("Destination process completed the handover successfully");
		(void)doca_devemu_pci_tlp_channel_destroy(resources->tlp_channel);
		resources->tlp_channel = NULL;
		force_quit = true;
		resources->is_channel_owner = false;
	}

	response = TLP_CHANNEL_HANDOVER_MSG_END_ACK;
	(void)lu_oob_send(resources->src_socket, &response, sizeof(response));
	DOCA_LOG_INFO("Source -> Destination: END_ACK");
	return DOCA_SUCCESS;
}

/**
 * Handle TLP Live upgrade OOB message from the destination
 *
 * @resources [in]: Pointer to TLP resources structure
 * @msg [in]: TLP Live upgrade OOB message
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_tlp_lu_oob_message_from_dst(struct devemu_tlp_resources *resources,
						       enum tlp_channel_handover_msg msg)
{
	doca_error_t result;

	switch (msg) {
	case TLP_CHANNEL_HANDOVER_MSG_SETUP:
		return handle_handover_setup(resources);
	case TLP_CHANNEL_HANDOVER_MSG_EXPORT:
		return handle_handover_export(resources);
	case TLP_CHANNEL_HANDOVER_MSG_BEGIN:
		result = handle_handover_begin(resources);
		if (result != DOCA_SUCCESS) {
			/* Re-set the TLP channel as primary to ensure complete cleanup of the TLP channel */
			(void)doca_devemu_pci_tlp_channel_set_primary(resources->tlp_channel, 1);
		}
		return result;
	case TLP_CHANNEL_HANDOVER_MSG_END:
		return handle_handover_end(resources);
	default:
		DOCA_LOG_ERR("Handover message %d from a destination is not supported", msg);
		return DOCA_ERROR_INVALID_VALUE;
	}
}

/**
 * Tear down all VF functions (transaction regions, TLP devices and representors).
 *
 * VF TLP devices derive from the PF, so they must be destroyed before the PF TLP device. VF
 * representors were opened (not created) here, so they are closed rather than destroyed.
 *
 * @resources [in]: Pointer to TLP resources structure
 */
static void cleanup_vf_funcs(struct devemu_tlp_resources *resources)
{
	doca_error_t result;

	for (uint16_t i = 0; i < resources->total_vfs; i++) {
		struct devemu_pci_func *vf = &resources->vf[i];

		tlp_cleanup_transaction_region(vf);

		if (vf->tlp_dev != NULL) {
			result = doca_devemu_pci_tlp_dev_stop(vf->tlp_dev);
			if (result == DOCA_SUCCESS)
				result = doca_devemu_pci_tlp_dev_destroy(vf->tlp_dev);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to stop/destroy VF %u TLP device: %s",
					     i,
					     doca_error_get_descr(result));
			vf->tlp_dev = NULL;
		}

		if (vf->rep != NULL) {
			result = doca_dev_rep_close(vf->rep);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to close VF %u representor: %s", i, doca_error_get_descr(result));
			vf->rep = NULL;
		}
	}
}

/**
 * Tear down all PCI function device resources: VF and PF TLP devices, their representors, the PF
 * transaction region and the PF Expansion ROM BAR.
 *
 * @resources [in]: Pointer to TLP resources structure
 */
static void tlp_devices_cleanup(struct devemu_tlp_resources *resources)
{
	doca_error_t result;

	/* Destroy the VFs first: their TLP devices derive from the PF TLP device */
	if (resources->total_vfs > 0)
		cleanup_vf_funcs(resources);

	tlp_cleanup_transaction_region(&resources->pf);
	tlp_cleanup_exp_bar(resources);

	if (resources->pf.tlp_dev != NULL) {
		result = doca_devemu_pci_tlp_dev_stop(resources->pf.tlp_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated Device TLP device: %s",
				     doca_error_get_descr(result));
		else {
			result = doca_devemu_pci_tlp_dev_destroy(resources->pf.tlp_dev);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to destroy DOCA Emulated Device TLP device: %s",
					     doca_error_get_descr(result));
		}

		resources->pf.tlp_dev = NULL;
	}

	if (resources->pf.rep != NULL) {
		if (!resources->is_channel_owner) {
			result = doca_dev_rep_close(resources->pf.rep);
		} else {
			result = doca_devemu_pci_type_destroy_rep(resources->pf.rep);
		}
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA Emulated Device representor: %s",
				     doca_error_get_descr(result));

		resources->pf.rep = NULL;
	}
}

/**
 * Clean up and destroy TLP resources
 *
 * @resources [in]: Pointer to TLP resources structure
 */
static void devemu_tlp_resources_cleanup(struct devemu_tlp_resources *resources)
{
	doca_error_t result;

	/* Tear down the per-function TLP devices and their representors */
	tlp_devices_cleanup(resources);

	bool channel_ctx_in_progress = false;
	if (resources->channel_ctx != NULL) {
		result = doca_ctx_stop(resources->channel_ctx);
		if (result == DOCA_ERROR_IN_PROGRESS)
			channel_ctx_in_progress = true;
		else if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated Device context: %s", doca_error_get_descr(result));
		else /* DOCA_SUCCESS */
			resources->channel_ctx = NULL;
	}

	/* Drain the TLP channel */
	while (channel_ctx_in_progress) {
		(void)doca_pe_progress(resources->pe);

		enum doca_ctx_states ctx_state;
		result = doca_ctx_get_state(resources->channel_ctx, &ctx_state);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get state of channel context: %s", doca_error_get_descr(result));
		else if (ctx_state == DOCA_CTX_STATE_IDLE) {
			resources->channel_ctx = NULL;
			channel_ctx_in_progress = false;
		}
	}

	if (resources->tlp_channel != NULL) {
		result = doca_devemu_pci_tlp_channel_destroy(resources->tlp_channel);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA Emulated Device TLP channel: %s",
				     doca_error_get_descr(result));

		resources->tlp_channel = NULL;
	}

	if (resources->src_socket >= 0) {
		(void)close(resources->src_socket);
		resources->src_socket = -1;
	}
	if (resources->src_listen_socket >= 0) {
		tlp_lu_oob_server_cleanup(resources, !resources->is_channel_owner);
	}
	if (resources->dst_socket >= 0) {
		(void)close(resources->dst_socket);
		resources->dst_socket = -1;
	}

	if (resources->pe != NULL) {
		result = doca_pe_destroy(resources->pe);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(result));

		resources->pe = NULL;
	}

	if (resources->pci_type != NULL) {
		result = doca_devemu_pci_type_stop(resources->pci_type);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated PCI Type: %s", doca_error_get_descr(result));
		else {
			result = doca_devemu_pci_type_destroy(resources->pci_type);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to destroy DOCA Emulated PCI Type: %s",
					     doca_error_get_descr(result));
			else
				resources->pci_type = NULL;
		}
	}

	if (resources->channel_dev != NULL && resources->channel_dev != resources->dev) {
		result = doca_dev_close(resources->channel_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA channel device: %s", doca_error_get_descr(result));

		resources->channel_dev = NULL;
	}

	if (resources->dev != NULL) {
		result = doca_dev_close(resources->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(result));

		resources->dev = NULL;
	}

	if (resources->ibv_pd != NULL) {
		(void)ibv_dealloc_pd(resources->ibv_pd);
		resources->ibv_pd = NULL;
	}
	if (resources->ibv_ctx != NULL) {
		(void)ibv_close_device(resources->ibv_ctx);
		resources->ibv_ctx = NULL;
	}
}

/**
 * Query all representors for a given PCI type and get its VHCA id
 *
 * @pci_type [in]: Pointer to PCI type object
 * @dev_rep [in]: Pointer to DoCA device representor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t query_existing_tlp_reps(struct doca_devemu_pci_type *pci_type, struct doca_dev_rep **dev_rep)
{
	doca_error_t result;
	struct doca_devinfo_rep **dev_list_rep;
	uint32_t nb_reps, rep_idx;
	uint16_t vhca_id = 0;

	result = doca_devemu_pci_type_create_rep_list(pci_type, &dev_list_rep, &nb_reps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Couldn't create the device representors list: %s", doca_error_get_name(result));
		return result;
	}

	for (rep_idx = 0; rep_idx < nb_reps; rep_idx++) {
		enum doca_pci_func_type func_type;

		/* Only the PF representor is opened here; VF representors are gathered later by init_vfs */
		result = doca_devinfo_rep_get_pci_func_type(dev_list_rep[rep_idx], &func_type);
		if (result != DOCA_SUCCESS || func_type != DOCA_PCI_FUNC_TYPE_PF)
			continue;

		result = doca_devinfo_rep_get_vhca_id(dev_list_rep[rep_idx], &vhca_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN(
				"Failed to get DOCA Emulated Device representor's VHCA ID: %s, try to find next available one",
				doca_error_get_descr(result));
			/* Try another representor with same type naming */
			continue;
		} else {
			DOCA_LOG_INFO("Found existing representor with VHCD ID %#x\n", vhca_id);
			break;
		}
	}
	if (rep_idx < nb_reps) {
		result = doca_dev_rep_open(dev_list_rep[rep_idx], dev_rep);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to open DOCA Emulated Device representor");
	} else {
		DOCA_LOG_INFO("No matching representor was found, the application will proceed to create a new one");
		result = DOCA_ERROR_NOT_FOUND;
	}
	doca_devinfo_rep_destroy_list(dev_list_rep);
	return result;
}

/**
 * Initialize PCI configuration space capabilities with default values
 *
 * @caps_hdr [out]: Pointer to PCI config space capabilities structure to initialize
 */
static void init_pci_config_space_caps(struct pci_config_space_caps *caps_hdr)
{
	/* PCI express cap */
	caps_hdr->express_cap.reg0.fields.cap_id = TLP_PCI_CAP_ID_EXPRESS;	      /* 8 bits */
	caps_hdr->express_cap.reg0.fields.next_cap_ptr = TLP_PCI_CAP_NEXT_OF_EXPRESS; /* 8 bits */
	caps_hdr->express_cap.reg0.fields.pcie_register = 0x0002;		      /* high 16 bits */

	caps_hdr->express_cap.cap_register = 0x112c8fe2;

	caps_hdr->express_cap.reg2.fields.dev_control = 0x2950;
	caps_hdr->express_cap.reg2.fields.dev_status = 0x0000;

	caps_hdr->express_cap.link_cap = 0;
	caps_hdr->express_cap.reg4.fields.link_control = 0x0000;
	caps_hdr->express_cap.reg4.fields.link_status = 0x1104;

	caps_hdr->express_cap.dev_cap2 = 0x0;

	caps_hdr->express_cap.slot_cap = 0x0;
	caps_hdr->express_cap.reg6.fields.slot_status = 0x0;
	caps_hdr->express_cap.reg6.fields.slot_control = 0x0;

	caps_hdr->express_cap.reg7.fields.root_cap = 0x0;
	caps_hdr->express_cap.reg7.fields.root_control = 0x0;
	caps_hdr->express_cap.root_status = 0x0;
	caps_hdr->express_cap.dev_cap2 = 0x00030397;
	caps_hdr->express_cap.regA.fields.dev_status2 = 0x0;
	caps_hdr->express_cap.regA.fields.dev_control2 = 0x0;
	caps_hdr->express_cap.link_cap2 = 0x0180003e;

	caps_hdr->express_cap.regC.fields.link_status2 = 0;
	caps_hdr->express_cap.regC.fields.link_control2 = 0;
	caps_hdr->express_cap.slot_cap2 = 0;
	caps_hdr->express_cap.regE.fields.slot_status2 = 0;
	caps_hdr->express_cap.regE.fields.slot_control2 = 0;

	/* VPD cap */
	caps_hdr->vpd_cap.reg0.fields.cap_id = TLP_PCI_CAP_ID_VPD;	      /* 8 bits */
	caps_hdr->vpd_cap.reg0.fields.next_cap_ptr = TLP_PCI_CAP_NEXT_OF_VPD; /* 8 bits */
	caps_hdr->vpd_cap.reg0.fields.addr_register = 0x8000;		      /* high 16 bits */
	caps_hdr->vpd_cap.msg_addr = 0x00000078;

	/* MSIX cap */
	caps_hdr->msix_cap.reg0.fields.cap_id = TLP_PCI_CAP_ID_MSIX;		/* 8 bits */
	caps_hdr->msix_cap.reg0.fields.next_cap_ptr = TLP_PCI_CAP_NEXT_OF_MSIX; /* 8 bits */
	caps_hdr->msix_cap.reg0.fields.msg_control = 0x0000;			/* high 16 bits */

	caps_hdr->msix_cap.reg1.fields.tbl_bir = msix_table_configs[0].bar_id;
	caps_hdr->msix_cap.reg1.fields.tbl_offset = msix_table_configs[0].start_address >> DWORD_LOG_SIZE_BITS;
	caps_hdr->msix_cap.reg2.fields.pba_bir = msix_pba_configs[0].bar_id;
	caps_hdr->msix_cap.reg2.fields.pba_offset = msix_pba_configs[0].start_address >> DWORD_LOG_SIZE_BITS;

	/* PM cap */
	caps_hdr->pm_cap.reg0.fields.cap_id = TLP_PCI_CAP_ID_PM;	    /* 8 bits */
	caps_hdr->pm_cap.reg0.fields.next_cap_ptr = TLP_PCI_CAP_NEXT_OF_PM; /* 8 bits */
	caps_hdr->pm_cap.reg0.fields.pmc = 0x81c3;			    /* high 16 bits */

	caps_hdr->pm_cap.reg1.fields.data = 0;
	caps_hdr->pm_cap.reg1.fields.reserved = 0;
	caps_hdr->pm_cap.reg1.fields.pmcsr = 0x0008;
}

/**
 * Initialize PCI configuration space extended capabilities with default values
 *
 * @ext_caps_hdr [out]: Pointer to PCI config space extended capabilities structure to initialize
 * @total_vfs [in]: Number of VFs to advertise via InitialVFs/TotalVFs (0 disables SR-IOV)
 */
static void init_pci_config_space_ext_caps(struct pci_config_space_ext_caps *ext_caps_hdr, uint16_t total_vfs)
{
	struct pci_config_space_ext_cap_sriov *sriov;

	/* AER cap */
	ext_caps_hdr->aer_ext_cap.reg0.fields.cap_id = TLP_PCIE_CAP_ID_AER;		   /* 16 bits */
	ext_caps_hdr->aer_ext_cap.reg0.fields.cap_version = AER_CAP_VERSION;		   /* low 4 bits */
	ext_caps_hdr->aer_ext_cap.reg0.fields.next_cap_offset = TLP_PCIE_CAP_NEXT_CAP_AER; /* high 12 bits */
	for (int i = 0; i < 17; i++)
		ext_caps_hdr->aer_ext_cap.regs[i] = 0;

	/* Without SR-IOV the ext cap chain ends at AER */
	if (total_vfs == 0)
		return;

	/* SR-IOV cap */
	sriov = &ext_caps_hdr->sriov_ext_cap;
	ext_caps_hdr->aer_ext_cap.reg0.fields.next_cap_offset = TLP_PCIE_CAP_OFFSET_SRIOV;

	memset(sriov, 0, sizeof(*sriov));

	sriov->reg0.fields.cap_id = TLP_PCIE_CAP_ID_SRIOV;
	sriov->reg0.fields.cap_version = SRIOV_CAP_VERSION;
	sriov->reg0.fields.next_cap_offset = TLP_PCIE_CAP_NEXT_CAP_SRIOV;

	/* SR-IOV Capabilities (RO): no VF migration, no ARI preserved, no larger tags */
	sriov->cap_register = 0;

	/* Control / Status start cleared: VF Enable = 0, VF MSE = 0 */
	sriov->reg2.fields.control = 0;
	sriov->reg2.fields.status = 0;

	/* InitialVFs and TotalVFs must match (MR-IOV which allowed them to differ is deprecated) */
	sriov->reg3.fields.initial_vfs = total_vfs;
	sriov->reg3.fields.total_vfs = total_vfs;

	/* NumVFs is programmed by the host (while VF Enable is clear) before enabling the VFs */
	sriov->reg4.fields.num_vfs = 0;
	sriov->reg4.fields.fcn_dep_link = 0;

	/* Contiguous routing IDs: VF k routing ID = PF routing ID + first_vf_offset + (k-1) * stride */
	sriov->reg5.fields.first_vf_offset = SRIOV_FIRST_VF_OFFSET;
	sriov->reg5.fields.vf_stride = SRIOV_VF_STRIDE;

	sriov->reg6.fields.vf_device_id = VF_PCI_TYPE_DEVICE_ID;

	sriov->supported_page_sizes = SRIOV_SUPPORTED_PAGE_SIZES;
	sriov->system_page_size = 0;

	/* VF BAR0 is a 64-bit prefetchable memory BAR shared by all VFs; the host sizes it by writing
	 * all 1s and then programs the base. VF BAR1 holds the upper 32 bits of the 64-bit BAR. */
	sriov->vf_bar[0] = BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT | BAR_MEM_PREFETCHABLE;
	sriov->vf_bar[1] = 0;
}

/**
 * Initialize PCI configuration space header with default values
 *
 * @pci_config_space_header [out]: Pointer to PCI config space header structure
 */
static void init_pci_config_space_header(struct pci_config_space_header *pci_config_space_header)
{
	pci_config_space_header->reg0.fields.vendor_id = TLP_PCI_TYPE_VENDOR_ID;
	pci_config_space_header->reg0.fields.device_id = TLP_PCI_TYPE_DEVICE_ID;
	pci_config_space_header->reg1.fields.command = 0x0000;
	pci_config_space_header->reg1.fields.status = 0x0010; /* set bit4 for capabilities list */
	pci_config_space_header->reg2.fields.revision_id = TLP_PCI_TYPE_REVISION_ID;
	pci_config_space_header->reg2.fields.class_code = TLP_PCI_TYPE_CLASS_CODE;
	pci_config_space_header->reg3.fields.cache_line_size = 0x10;
	pci_config_space_header->reg3.fields.latency_timer = 0x00;
	pci_config_space_header->reg3.fields.header_type = 0x00;
	pci_config_space_header->reg3.fields.bist = 0x00;
	pci_config_space_header->bar[0] = 0x00000000 | BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT |
					  BAR_MEM_PREFETCHABLE;
	pci_config_space_header->bar[1] = 0x00000000;
	pci_config_space_header->bar[2] = 0x00000000;
	pci_config_space_header->bar[3] = 0x00000000;
	pci_config_space_header->bar[4] = 0x00000000;
	pci_config_space_header->bar[5] = 0x00000000;
	pci_config_space_header->cardbus_cis_pointer = 0x00000000;
	pci_config_space_header->regB.fields.subsystem_vendor_id = TLP_PCI_TYPE_SUBSYSTEM_VENDOR_ID;
	pci_config_space_header->regB.fields.subsystem_id = TLP_PCI_TYPE_SUBSYSTEM_ID;
	pci_config_space_header->expansion_rom_base_address = EXP_BAR_ENABLED_BIT_SET;
	pci_config_space_header->regD.fields.capabilities_pointer = TLP_PCI_CAP_OFFSET_EXPRESS;
	pci_config_space_header->regF.fields.interrupt_line = 0x00;
	pci_config_space_header->regF.fields.interrupt_pin = 0x00;
	pci_config_space_header->regF.fields.min_gnt = 0x00;
	pci_config_space_header->regF.fields.max_lat = 0x00;
}

/**
 * Initialize PCI configuration space with default values
 *
 * @pci_config_space [out]: Pointer to PCI config space structure
 * @total_vfs [in]: Number of VFs to advertise via the SR-IOV ext cap (0 disables SR-IOV)
 */
static void init_pci_config_space(struct pci_config_space *pci_config_space, uint16_t total_vfs)
{
	init_pci_config_space_header(&pci_config_space->header);
	init_pci_config_space_caps(&pci_config_space->caps);
	init_pci_config_space_ext_caps(&pci_config_space->ext_caps, total_vfs);
}

/**
 * Initialize TLP handler context with default values
 *
 * @tlp_handler_cxt [out]: Pointer to TLP handler context structure
 */
static void init_tlp_handler_cxt(struct tlp_handler_cxt *tlp_handler_cxt)
{
	tlp_handler_cxt->bus = 0x00;
	tlp_handler_cxt->device = 0x00;
	tlp_handler_cxt->function = 0x00;
	tlp_handler_cxt->bdf = 0x0000;

	tlp_handler_cxt->requester_id = 0x0000;
	tlp_handler_cxt->completer_id = 0x0000;
	tlp_handler_cxt->tag = 0x0000;
	tlp_handler_cxt->tag8 = 0x0000;
	tlp_handler_cxt->tag9 = 0x0000;
	tlp_handler_cxt->req_fmt = 0x00;
	tlp_handler_cxt->req_type = 0x00;
	tlp_handler_cxt->cmpl_fmt = 0x00;
	tlp_handler_cxt->cmpl_type = TLP_TYPE_COMPLETION;
	tlp_handler_cxt->cmpl_length = 0x00;
	tlp_handler_cxt->cmpl_status = 0x00;

	tlp_handler_cxt->cfg_space_section = 0x0000;
	tlp_handler_cxt->capability_offset = 0x0000;
	tlp_handler_cxt->capability_id = 0x0000;
	tlp_handler_cxt->first_dw_be = 0x00;

	tlp_handler_cxt->bar_region_size = (1 << layout_configs[0].log_size);
	tlp_handler_cxt->exp_bar_size = 0;

	tlp_handler_cxt->transaction_region_memory = NULL;
	tlp_handler_cxt->transaction_region_size = 0;
}

/**
 * Gather the VF representors provisioned by the PF and bring up a TLP device per VF.
 *
 * Once the PF TLP device is created and started with SR-IOV attributes, the device provisions the
 * VFs and exposes a representor per VF. We list the representors, pick the VF-typed ones, and for
 * each create + start a TLP device (function type VF) plus its own config space, handler context and
 * transaction region.
 *
 * @resources [in]: Pointer to TLP resources structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_vfs(struct devemu_tlp_resources *resources)
{
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_reps;
	uint16_t vf_count = 0;
	doca_error_t result;

	result = doca_devemu_pci_type_create_rep_list(resources->pci_type, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to list representors to gather VFs: %s", doca_error_get_descr(result));
		return result;
	}

	for (uint32_t i = 0; i < nb_reps && vf_count < resources->total_vfs; i++) {
		enum doca_pci_func_type func_type;
		struct devemu_pci_func *vf;

		result = doca_devinfo_rep_get_pci_func_type(rep_list[i], &func_type);
		if (result != DOCA_SUCCESS || func_type != DOCA_PCI_FUNC_TYPE_VF)
			continue;

		vf = &resources->vf[vf_count];
		vf->func_type = DOCA_PCI_FUNC_TYPE_VF;
		vf->vf_index = vf_count;

		result = doca_dev_rep_open(rep_list[i], &vf->rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open VF %u representor: %s", vf_count, doca_error_get_descr(result));
			goto destroy_list;
		}

		result = init_tlp_dev(resources, vf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize VF %u TLP device: %s",
				     vf_count,
				     doca_error_get_descr(result));
			goto destroy_list;
		}

		init_pci_config_space(&vf->pci_config_space, 0);
		init_tlp_handler_cxt(&vf->tlp_handler_cxt);

		/* A VF reports the SR-IOV VF Device ID and owns no BARs/expansion ROM of its own: those
		 * are provided by the PF SR-IOV capability, so keep the VF config space BARs read-only 0 */
		vf->pci_config_space.header.reg0.fields.device_id = VF_PCI_TYPE_DEVICE_ID;
		memset(vf->pci_config_space.header.bar, 0, sizeof(vf->pci_config_space.header.bar));
		vf->pci_config_space.header.expansion_rom_base_address = 0;

		result = tlp_init_transaction_region(vf, transaction_configs[0].size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize VF %u transaction region: %s",
				     vf_count,
				     doca_error_get_descr(result));
			goto destroy_list;
		}

		vf_count++;
	}

	if (vf_count != resources->total_vfs) {
		DOCA_LOG_ERR("Expected %u VF representors but only found %u", resources->total_vfs, vf_count);
		result = DOCA_ERROR_NOT_FOUND;
		goto destroy_list;
	}

	result = DOCA_SUCCESS;

destroy_list:
	(void)doca_devinfo_rep_destroy_list(rep_list);
	return result;
}

/**
 * Setup all the TLP devices (PF, and VFs if SR-IOV is enabled).
 *
 * @resources [in]: Pointer to TLP resources structure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t tlp_devices_setup(struct devemu_tlp_resources *resources)
{
	doca_error_t result;

	result = init_tlp_dev(resources, &resources->pf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to initialize TLP device: %s", doca_error_get_descr(result));
		return result;
	}

	init_pci_config_space(&resources->pf.pci_config_space, resources->total_vfs);
	init_tlp_handler_cxt(&resources->pf.tlp_handler_cxt);

	result = tlp_init_transaction_region(&resources->pf, transaction_configs[0].size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize transaction region: %s", doca_error_get_descr(result));
		return result;
	}

	result = tlp_init_exp_bar(resources, EXP_BAR_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize Expansion ROM bar: %s", doca_error_get_descr(result));
		return result;
	}

	if (resources->total_vfs > 0) {
		result = init_vfs(resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize VFs: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/**
 * Sample's logic for handling PCI TLP requests
 *
 * @pci_address [in]: PCI address of the device
 * @is_handover_destination [in]: Whether the sample is running as a handover destination
 * @shm_dir_path [in]: Shared memory directory path
 * @total_vfs [in]: Number of SR-IOV VFs to expose (0 => SR-IOV disabled)
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_tlp_handler_dpu(const char *pci_address,
					       bool is_handover_destination,
					       const char *shm_dir_path,
					       uint16_t total_vfs)
{
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = TLP_PCI_TYPE_NAME;
	struct devemu_tlp_resources resources = {0};
	doca_error_t result;

	resources.shm_dir_path = shm_dir_path;

	resources.pf.func_type = DOCA_PCI_FUNC_TYPE_PF;
	resources.total_vfs = total_vfs;

	/* Initialize sockets to -1 to avoid cleanup issues */
	resources.dst_socket = -1;
	resources.src_socket = -1;
	resources.src_listen_socket = -1;

	DOCA_LOG_INFO("Sample [%s] called with pci_address: %s", __func__, pci_address);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Find a device that supports PCI TLP emulation */
	result = find_supported_tlp_device(pci_address, &resources.dev);
	if (result != DOCA_SUCCESS) {
		goto exit;
	}

	result = doca_devemu_pci_tlp_type_create(pci_type_name, &resources.pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI TLP type: %s", doca_error_get_descr(result));
		goto exit;
	}

	/* Set PCIe configuration space values */
	result = configure_and_start_pci_tlp_type(resources.pci_type, resources.dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to configure and start PCI type: %s", doca_error_get_descr(result));
		goto exit;
	}

	result = query_existing_tlp_reps(resources.pci_type, &resources.pf.rep);
	/* Create a representor if couldn't query existing device so host side could enumerate the device */
	if (result != DOCA_SUCCESS) {
		/* Device is not created from export so we use the existing device */
		resources.channel_dev = resources.dev;
		resources.is_channel_owner = true;

		/* Initialize a TLP channel */
		result = init_tlp_channel(&resources, NULL, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to initialize TLP channel: %s", doca_error_get_descr(result));
			goto exit;
		}

		result = init_rep_dev(&resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to initialize representor device: %s", doca_error_get_descr(result));
			goto exit;
		}
	} else {
		/*
		 * Another instance already created the representor and owns the TLP channel. Starting this process
		 * with a non-empty shm_dir_path and is_handover_destination=false configures a handover source, which
		 * must own the channel and be able to export it later. This conflicts with the existing owner.
		 */
		if (!is_handover_destination && *resources.shm_dir_path != '\0') {
			DOCA_LOG_ERR("A handover source must own the TLP channel");
			result = DOCA_ERROR_BAD_CONFIG;
			goto exit;
		}
		resources.is_channel_owner = false;
	}

	/* Setup all the TLP devices (PF, and VFs if SR-IOV is enabled) */
	result = tlp_devices_setup(&resources);
	if (result != DOCA_SUCCESS)
		goto exit;

	/* If running as handover destination, connect to the source app and receive the TLP channel */
	if (is_handover_destination) {
		result = initiate_tlp_channel_handover(&resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initiate TLP channel handover: %s", doca_error_get_descr(result));
			goto exit;
		}
	}

	/* Poll on the TLP channel to get TLP requests */
	DOCA_LOG_INFO("Polling on the TLP channel to get TLP requests. Press Ctrl+C to exit.");

	if (*resources.shm_dir_path == '\0') {
		/* Standalone mode: no shared memory or handover support, just process TLP requests */
		while (!force_quit) {
			if (resources.is_channel_owner) {
				doca_pe_progress(resources.pe);
			}
		}
		goto exit;
	}

	while (!force_quit) {
		/* At this point the channel should be primary, so it's safe to initialize the server */
		if (resources.src_listen_socket < 0) {
			result = tlp_lu_oob_server_init(&resources);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to initialize TLP Live upgrade OOB server: %s",
					     doca_error_get_descr(result));
				goto exit;
			}
		}
		if (resources.is_channel_owner) {
			doca_pe_progress(resources.pe);
			if (resources.src_socket < 0) {
				int accepted_socket = accept(resources.src_listen_socket, NULL, NULL);
				if (accepted_socket >= 0) {
					DOCA_LOG_INFO("Accepted socket: %d", accepted_socket);
					resources.src_socket = accepted_socket;
				} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
					DOCA_LOG_ERR("Failed to accept socket: %s", strerror(errno));
					result = DOCA_ERROR_IO_FAILED;
					goto exit;
				}
				continue;
			}

			/* Poll the Live upgrade OOB socket for incoming handover messages from the destination.
			 * A short timeout (1 ms) lets the loop quickly call doca_pe_progress()
			 * again on the next iteration, keeping TLP request handling responsive while
			 * waiting for the next handover step. */
			struct pollfd pfd = {
				.fd = resources.src_socket,
				.events = POLLIN,
			};
			int poll_ret = poll(&pfd, 1, 1);
			if (poll_ret > 0 && (pfd.revents & POLLIN)) {
				enum tlp_channel_handover_msg msg;
				result = lu_oob_recv(resources.src_socket, &msg, sizeof(msg));
				if (result == DOCA_SUCCESS) {
					DOCA_LOG_DBG("Received TLP Live upgrade OOB message: msg=%d", msg);
					result = handle_tlp_lu_oob_message_from_dst(&resources, msg);
					if (result != DOCA_SUCCESS) {
						DOCA_LOG_ERR("Failed to handle TLP Live upgrade OOB message: %s",
							     doca_error_get_descr(result));
						goto exit;
					}
				} else if (result == DOCA_ERROR_NOT_CONNECTED) {
					if (resources.channel_ctx == NULL) {
						DOCA_LOG_WARN(
							"Connection lost after channel was stopped, attempting rollback");
						result = doca_devemu_pci_tlp_channel_set_primary(resources.tlp_channel,
												 1);
						if (result != DOCA_SUCCESS) {
							DOCA_LOG_ERR("Failed to set primary channel: %s",
								     doca_error_get_descr(result));
							goto exit;
						}
						resources.channel_ctx =
							doca_devemu_pci_tlp_channel_as_ctx(resources.tlp_channel);
						result = doca_ctx_start(resources.channel_ctx);
						if (result != DOCA_SUCCESS) {
							DOCA_LOG_ERR("Failed to start channel context: %s",
								     doca_error_get_descr(result));
							goto exit;
						}
					}
					/* Clean up source socket to prepare for a future reconnection attempt */
					(void)close(resources.src_socket);
					resources.src_socket = -1;
					/* Reset result to avoid returning this error at teardown */
					result = DOCA_SUCCESS;
				} else {
					DOCA_LOG_ERR("Failed to receive message: %s", doca_error_get_descr(result));
					goto exit;
				}
			} else if (poll_ret < 0 && errno != EINTR) {
				DOCA_LOG_ERR("Poll failed: %s", strerror(errno));
				goto exit;
			}
		}
	}

exit:
	DOCA_LOG_INFO("Exiting...");

	/* Clean and destroy all relevant objects */
	devemu_tlp_resources_cleanup(&resources);

	return result;
}
