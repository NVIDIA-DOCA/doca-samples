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

#ifndef VBLK_PCI_TYPES_H_
#define VBLK_PCI_TYPES_H_

#include <linux/types.h>

/*
 * virtio/virtio block pci layout as described in the
 * https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html
 *
 * Note:
 *  assumes little endian platform such as dpu(arm)/x86
 *  use vblk_pci prefix in order not to collide with linux/virtio_pci.h
 */

#define VBLK_PCI_VIRTIO_VENDOR_ID 0x1af4
#define VBLK_PCI_VIRTIO_NON_TRANSITION_DEV_ID_BASE 0x1040
#define VBLK_PCI_VIRTIO_F_VERSION_1 32
#define VBLK_PCI_VIRTIO_F_ACCESS_PLATFORM 33

#define VBLK_F_MQ 12
#define VBLK_PCI_VIRTIO_F_SIZE_MAX 1
#define VBLK_PCI_VIRTIO_F_SEG_MAX 2
#define VBLK_PCI_VIRTIO_F_INDIRECT_DESC 28

#define VBLK_PCI_VIRTIO_DEVICE_STATUS_ACK 1
#define VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER 2
#define VBLK_PCI_VIRTIO_DEVICE_STATUS_FAILED 128
#define VBLK_PCI_VIRTIO_DEVICE_STATUS_FEATURES_OK 8
#define VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER_OK 4
#define VBLK_PCI_VIRTIO_DEVICE_STATUS_NEEDS_RESET 64

#define VBLK_T_IN 0
#define VBLK_T_OUT 1
#define VBLK_T_FLUSH 4
#define VBLK_T_GET_ID 8
#define VBLK_T_GET_LIFETIME 10
#define VBLK_T_DISCARD 11
#define VBLK_T_WRITE_ZEROES 13
#define VBLK_T_SECURE_ERASE 14

#define VBLK_S_OK 0
#define VBLK_S_IOERR 1
#define VBLK_S_UNSUPP 2

struct vblk_pci_virtq_pci_cfg {
	uint16_t queue_size;		  /* read-write */
	uint16_t queue_msix_vector;	  /* read-write */
	uint16_t queue_enable;		  /* read-write */
	uint16_t queue_notify_off;	  /* read-only for driver */
	uint64_t queue_desc;		  /* read-write */
	uint64_t queue_driver;		  /* read-write */
	uint64_t queue_device;		  /* read-write */
	uint16_t queue_notif_config_data; /* read-only for driver */
	uint16_t queue_reset;		  /* read-write */
} __attribute__((packed));

__attribute__((packed)) struct vblk_pci_virtio_pci_common_cfg {
	/* About the whole device. */
	uint32_t device_feature_select; /* read-write */
	uint32_t device_feature;	/* read-only for driver */
	uint32_t driver_feature_select; /* read-write */
	uint32_t driver_feature;	/* read-write */

	uint16_t config_msix_vector; /* read-write */
	uint16_t num_queues;	     /* read-only for driver */
	uint8_t device_status;	     /* read-write */
	uint8_t config_generation;   /* read-only for driver */

	/* About a specific virtqueue. */
	uint16_t queue_select; /* read-write */
	struct vblk_pci_virtq_pci_cfg vq;

	/* About the administration virtqueue. */
	uint16_t admin_queue_index; /* read-only for driver */
	uint16_t admin_queue_num;   /* read-only for driver */
};

struct vblk_pci_virtio_blk_config {
	uint64_t capacity;
	uint32_t size_max;
	uint32_t seg_max;
	struct vblk_pci_virtio_blk_geometry {
		uint16_t cylinders;
		uint8_t heads;
		uint8_t sectors;
	} geometry;
	uint32_t blk_size;
	struct vblk_pci_virtio_blk_topology {
		// # of logical blocks per physical block (log2)
		uint8_t physical_block_exp;
		// offset of first aligned logical block
		uint8_t alignment_offset;
		// suggested minimum I/O size in blocks
		uint16_t min_io_size;
		// optimal (suggested maximum) I/O size in blocks
		uint32_t opt_io_size;
	} topology;
	uint8_t writeback;
	uint8_t unused0;
	uint16_t num_queues;
	uint32_t max_discard_sectors;
	uint32_t max_discard_seg;
	uint32_t discard_sector_alignment;
	uint32_t max_write_zeroes_sectors;
	uint32_t max_write_zeroes_seg;
	uint8_t write_zeroes_may_unmap;
	uint8_t unused1[3];
	uint32_t max_secure_erase_sectors;
	uint32_t max_secure_erase_seg;
	uint32_t secure_erase_sector_alignment;
	struct vblk_pci_virtio_blk_zoned_characteristics {
		uint32_t zone_sectors;
		uint32_t max_open_zones;
		uint32_t max_active_zones;
		uint32_t max_append_sectors;
		uint32_t write_granularity;
		uint8_t model;
		uint8_t unused2[3];
	} zoned;
} __attribute__((packed));

#endif
