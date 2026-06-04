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

#ifndef VIRTIO_PCI_SPEC_H
#define VIRTIO_PCI_SPEC_H

#include <stdint.h>
#include "vnet_pcie_defs.h"

/*
 * VirtIO PCI Transport Specification
 * Based on: https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html Section 4.1
 *
 * These definitions are standard across ALL VirtIO device types (network, block, etc.)
 * and can be shared between vnet, vblk, vfs, and other VirtIO implementations.
 */

/* VirtIO PCI Vendor ID and Device ID Base */
#define VIRTIO_PCI_VENDOR_ID 0x1af4
#define VIRTIO_PCI_DEVICE_ID_BASE 0x1040

/* Command register read-write bit definitions */
#define COMMAND_RW_IO_SPACE_ENABLE 0x0001   /* I/O space enable (bit 0) */
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

/* VirtIO PCI Capability Register Bits */
#define VIRTIO_PCI_CAP_CMD_REG_RW_BITS \
	(COMMAND_RW_IO_SPACE_ENABLE | COMMAND_RW_MEM_SPACE_ENABLE | COMMAND_RW_BUS_MASTER_ENABLE | \
	 COMMAND_RW_PERR_ENABLE | COMMAND_RW_SERR_ENABLE | COMMAND_RW_INT_DISABLE)
#define VIRTIO_PCI_CAP_STATUS_REG_W1C_BITS \
	(STATUS_WR1C_MASTER_DATA_PERR | STATUS_WR1C_SIGNALED_TA | STATUS_WR1C_RECEIVE_TA | STATUS_WR1C_RECEIVE_MA | \
	 STATUS_WR1C_SIGNALED_SERR | STATUS_WR1C_DETECTED_PERR)

/* VirtIO PCI Capability Types (per VirtIO specification) */
#define VIRTIO_PCI_CAP_COMMON_CFG 1 /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2 /* Notification configuration */
#define VIRTIO_PCI_CAP_ISR_CFG 3    /* ISR status configuration */
#define VIRTIO_PCI_CAP_DEVICE_CFG 4 /* Device-specific configuration */
#define VIRTIO_PCI_CAP_PCI_CFG 5    /* PCI configuration access */

/* VirtIO PCI Capability Structures */
struct virtio_pci_capability {
	struct pci_capability_header cap;
	uint8_t cap_len;  /* Length of this capability structure in bytes */
	uint8_t cfg_type; /* Type of VirtIO structure (1=common, 2=notify, etc.) */
	uint8_t bar;	  /* Which BAR this structure maps to */
	uint8_t id;	  /* Multiple capabilities of the same type */
	uint16_t rsrvd1;
	uint32_t offset; /* Offset within the BAR */
	uint32_t length; /* Length of the region */
} __attribute__((packed));

/* VirtIO PCI Notify Capability */
struct virtio_pci_notify_capability {
	struct virtio_pci_capability base;
	uint32_t notify_off_multiplier;
} __attribute__((packed));

/* VirtIO PCI Configuration Access Capability */
struct virtio_pci_cfg_capability {
	struct virtio_pci_capability base;
	uint8_t pci_cfg_data[4];
} __attribute__((packed));

#endif /* VIRTIO_PCI_SPEC_H */
