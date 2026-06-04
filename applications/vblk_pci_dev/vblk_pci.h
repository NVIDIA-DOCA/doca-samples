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

#ifndef VBLK_PCI_H_
#define VBLK_PCI_H_

#include <sys/queue.h>
#include <stdint.h>
#include <stdbool.h>

#include <doca_error.h>
#include <doca_dev.h>

#include "vblk_pcie_defs.h"
#include "vblk_pci_types.h"
#include "pci_spec_tlp.h"

/* consider add device change cb */
doca_error_t vblk_pci_init(struct doca_dev *dev, uint32_t num_ep, bool hotplug_mode);
void vblk_pci_reset(void);

doca_error_t vblk_pci_tlp_start(void);
void vblk_pci_stop(void);

struct doca_ctx *vblk_pci_ev_channel_ctx(void);
struct vblk_tlp_context *vblk_pci_get_tlp_ctx(void);

/* Our virtio layout has one bar. In general virtio may have several
 * different bars
 */
#define VBLK_PCI_VIRTIO_BAR_ID 0
#define VBLK_PCI_VIRTIO_BAR_LOG_SIZE 15
/* max number of queues supported by the controller */
#define VBLK_PCI_VIRTIO_MAX_QUEUES 256
/* Hard caps for doorbell and MSIX resources */
#define VBLK_PCI_VIRTIO_MAX_NUM_DB 256
#define VBLK_PCI_VIRTIO_MAX_NUM_MSIX 256

/* offsets/sizes are taken from the current emulated virtio block device */

/* pci_config offset/size */
#define VBLK_PCI_VIRTIO_PCI_CFG_OFFSET 0
#define VBLK_PCI_VIRTIO_PCI_CFG_LEN 0x100

/* isr */
#define VBLK_PCI_VIRTIO_ISR_CFG_OFFSET 0x100
#define VBLK_PCI_VIRTIO_ISR_CFG_LEN 0x1

/* device config */
#define VBLK_PCI_VIRTIO_DEV_CFG_OFFSET 0x200
#define VBLK_PCI_VIRTIO_DEV_CFG_LEN 0x100

/* doorbells */
#define VBLK_PCI_VIRTIO_DB_NOTIFY_BY_OFFSET 0 // Hardware Notify by data instead of offset
#define VBLK_PCI_VIRTIO_DB_STRIDE 3	      // Hardware Stride
#define VBLK_PCI_VIRTIO_DB_OFFSET 0x4000
#define VBLK_PCI_VIRTIO_DB_LEN 0x1000

/* MSIX table/pba */
#define VBLK_PCI_MAX_NUM_MSIX VBLK_PCI_VIRTIO_MAX_NUM_MSIX
#define VBLK_PCI_VIRTIO_MSIX_TABLE_OFFSET 0x2000
#define VBLK_PCI_VIRTIO_MSIX_TABLE_LEN 0x1000

#define VBLK_PCI_VIRTIO_MSIX_PBA_OFFSET 0x3000
#define VBLK_PCI_VIRTIO_MSIX_PBA_LEN 0x1000

struct vblk_pci_virtio_dev;

typedef void (*pci_cfg_change_cb_t)(struct vblk_pci_virtio_dev *dev, void *arg);
typedef void (*pci_tlp_poll_cb_t)(void *arg);

struct vblk_pci_virtio_dev {
	LIST_ENTRY(vblk_pci_virtio_dev) entry;
	struct doca_dev_rep *dev_rep;
	struct doca_devemu_pci_tlp_dev *pci_tlp_dev;
	struct doca_devemu_pci_msix *msix;
	uint16_t config_msix_vector;
	bool is_enumerated;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint32_t ep_index;

	/* emulated device */
	struct pcie_virtio_dev pcie_dev;

	pci_cfg_change_cb_t pci_cfg_change_cb;
	pci_tlp_poll_cb_t tlp_poll_cb;
	void *cb_arg;

	/* selectors */
	uint64_t device_features;
	uint64_t driver_features;

	/* vq shadow array - consider: dynamic allocation */
	struct vblk_pci_virtq_pci_cfg vqs[VBLK_PCI_VIRTIO_MAX_QUEUES];

	/* virtio pci config - consider: static assert */
	struct vblk_pci_virtio_pci_common_cfg pci_cfg;

	/* device configs - consider add static assert */
	union {
		struct vblk_pci_virtio_blk_config vblk_cfg;
		/* placeholder, all device config sizes must fit */
		char dev_cfg[sizeof(struct vblk_pci_virtio_blk_config)];
	};
};

/* consider: inheritance from the base ep */
LIST_HEAD(vblk_pci_dev_list, vblk_pci_virtio_dev);

/* Virtio device ids as specified in the spec: "5 Device Types" */
enum vblk_pci_virtio_subsystem_device_type {
	VBLK_PCI_DEVICE_TYPE_VBLK = 2,
};

struct vblk_pci_virtio_attrs {
	enum vblk_pci_virtio_subsystem_device_type device_type;
	uint64_t device_features;
	uint16_t num_queues;
	uint32_t ep_index;
	const void *dev_cfg;
	pci_cfg_change_cb_t pci_cfg_change_cb;
	pci_tlp_poll_cb_t tlp_poll_cb;
	void *cb_arg;
};

/* create/destroy virtio block pci endpoint
 * pci layout is preconfigured inside the vblk_pci
 * the endpoint is plugged into pci switch provided by the fw
 * the endpoint will be visible upon next pci enumeration. For example warm
 * boot or forced pci rescan in linux:
 * echo 1 > /sys/bus/pci/rescan
 */
struct vblk_pci_virtio_dev *vblk_pci_virtio_dev_create(const struct vblk_pci_virtio_attrs *attr);
void vblk_pci_virtio_dev_destroy(struct vblk_pci_virtio_dev *dev);
void vblk_pci_virtio_dev_reset(struct vblk_pci_virtio_dev *dev);

/* Called on TLP thread main loop; invokes tlp_poll_cb for all devices */
void vblk_pci_tlp_poll(void);

/* Called on TLP thread main loop when block device capacity changes; invokes MSIX interrupt to driver */
void vblk_pci_notify_host(void);

/* Hotplug support */
doca_error_t vblk_pci_create_device(uint32_t ep_index);
void vblk_pci_destroy_device(uint32_t ep_index);
doca_error_t vblk_pci_trigger_hotplug(uint32_t dsp_index, bool plug);

/* rw pointer to the current pci configuration */
struct vblk_pci_virtio_pci_common_cfg *vblk_pci_virtio_get_pci_cfg(struct vblk_pci_virtio_dev *dev);
/* ro queues */
const struct vblk_pci_virtq_pci_cfg *vblk_pci_virtio_get_virtq_pci_cfg(struct vblk_pci_virtio_dev *dev);
/* ro device config */
const struct vblk_pci_virtio_blk_config *vblk_pci_virtio_get_vblk_dev_cfg(struct vblk_pci_virtio_dev *dev);

/* Set block device capacity (called from TLP thread via stdin command) */
void vblk_pci_set_capacity(uint64_t capacity_bytes);

#endif
