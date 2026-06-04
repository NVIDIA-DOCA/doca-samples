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

#ifndef VNET_VIRTIO_TYPES_H
#define VNET_VIRTIO_TYPES_H

#include <linux/types.h>
#include <linux/if_ether.h>
#include <linux/virtio_pci.h>
#include <stdint.h>
#include <stddef.h>

#include "vnet_pcie_defs.h"
#include "virtio_pci_spec.h"

/*
 * virtio network pci layout as described in the
 * https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html
 *
 * Note assumes little endian platform such as dpu(arm)/x86
 */

#define VNET_VIRTIO_F_VERSION_1 32
#define VNET_VIRTIO_F_ACCESS_PLATFORM 33

#define VNET_VIRTIO_DEVICE_STATUS_ACK 1
#define VNET_VIRTIO_DEVICE_STATUS_DRIVER 2
#define VNET_VIRTIO_DEVICE_STATUS_FAILED 128
#define VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK 8
#define VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK 4
#define VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET 64

struct vnet_virtio_queue_config {
	uint16_t queue_size;		  /* read-write */
	uint16_t queue_msix_vector;	  /* read-write */
	uint16_t queue_enable;		  /* read-write */
	uint16_t queue_notify_off;	  /* read-only for driver */
	uint64_t queue_desc;		  /* read-write */
	uint64_t queue_driver;		  /* read-write */
	uint64_t queue_device;		  /* read-write */
	uint16_t queue_notif_config_data; /* read-only for driver */
	uint16_t queue_reset;		  /* read-write */
};

struct vnet_virtio_common_config {
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
	struct vnet_virtio_queue_config vq;

	/* About the administration virtqueue. */
	uint16_t admin_queue_index; /* read-only for driver */
	uint16_t admin_queue_num;   /* read-only for driver */
} __attribute__((packed));

/* VirtIO Network Device Configuration */
struct vnet_virtio_net_config {
	uint8_t mac[ETH_ALEN];			   /* MAC address */
	uint16_t status;			   /* Link status */
	uint16_t max_virtqueue_pairs;		   /* Maximum queue pairs */
	uint16_t mtu;				   /* Maximum transmission unit */
	uint32_t speed;				   /* Link speed in Mbps */
	uint8_t duplex;				   /* Duplex mode (0=half, 1=full) */
	uint8_t rss_max_key_size;		   /* RSS key size */
	uint16_t rss_max_indirection_table_length; /* RSS indirection table length */
	uint32_t supported_hash_types;		   /* Supported hash types */
} __attribute__((packed));

/* VirtIO Network Control Command Classes */
#define VIRTIO_NET_CTRL_RX 0		 /* RX mode control */
#define VIRTIO_NET_CTRL_MAC 1		 /* MAC address filtering */
#define VIRTIO_NET_CTRL_VLAN 2		 /* VLAN filtering */
#define VIRTIO_NET_CTRL_ANNOUNCE 3	 /* Gratuitous ARP announcement */
#define VIRTIO_NET_CTRL_MQ 4		 /* Multi-queue control */
#define VIRTIO_NET_CTRL_GUEST_OFFLOADS 5 /* Guest offload configuration */

/* VirtIO Network Control MQ Control Commands */
#define VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET 0 /* Set number of active queue pairs */
#define VIRTIO_NET_CTRL_MQ_RSS_CONFIG 1	  /* RSS configuration */
#define VIRTIO_NET_CTRL_MQ_HASH_CONFIG 2  /* Hash configuration */

/* VirtIO Network Control Command Status */
#define VIRTIO_NET_OK 0	 /* Command succeeded */
#define VIRTIO_NET_ERR 1 /* Command failed */

/* VirtIO Network Feature Bits */
#define VIRTIO_NET_F_CSUM 0		   /* Host handles pkts w/ partial csum */
#define VIRTIO_NET_F_GUEST_CSUM 1	   /* Guest handles pkts w/ partial csum */
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS 2 /* Dynamic offload configuration */
#define VIRTIO_NET_F_MTU 3		   /* Initial MTU advice */
#define VIRTIO_NET_F_MAC 5		   /* Host has given MAC address */
#define VIRTIO_NET_F_GUEST_TSO4 7	   /* Guest can handle TSOv4 in */
#define VIRTIO_NET_F_GUEST_TSO6 8	   /* Guest can handle TSOv6 in */
#define VIRTIO_NET_F_GUEST_ECN 9	   /* Guest can handle TSO[6] w/ ECN in */
#define VIRTIO_NET_F_GUEST_UFO 10	   /* Guest can handle UFO in */
#define VIRTIO_NET_F_HOST_TSO4 11	   /* Host can handle TSOv4 in */
#define VIRTIO_NET_F_HOST_TSO6 12	   /* Host can handle TSOv6 in */
#define VIRTIO_NET_F_HOST_ECN 13	   /* Host can handle TSO[6] w/ ECN in */
#define VIRTIO_NET_F_HOST_UFO 14	   /* Host can handle UFO in */
#define VIRTIO_NET_F_MRG_RXBUF 15	   /* Host can merge receive buffers */
#define VIRTIO_NET_F_STATUS 16		   /* virtio_net_config.status available */
#define VIRTIO_NET_F_CTRL_VQ 17		   /* Control channel available */
#define VIRTIO_NET_F_CTRL_RX 18		   /* Control channel RX mode support */
#define VIRTIO_NET_F_CTRL_VLAN 19	   /* Control channel VLAN filtering */
#define VIRTIO_NET_F_MQ 22		   /* Device supports Receive Flow Steering */
#define VIRTIO_NET_F_SPEED_DUPLEX 63	   /* Device set linkspeed and duplex */

/* Link status bits */
#define VIRTIO_NET_S_LINK_UP 1	/* Link is up */
#define VIRTIO_NET_S_ANNOUNCE 2 /* Announcement needed */

/* MSI-X Register Numbers */
#define VIRTIO_NET_MSI_X_REGISTER_NUM (offsetof(struct pcie_virtio_dev, cfg.msix_cap) / sizeof(uint32_t))

/* Complete VirtIO Device Structure (Framework-specific combination of PCIe + VirtIO) */
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

#endif /* VNET_VIRTIO_TYPES_H */
