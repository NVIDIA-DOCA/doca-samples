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
#include <stdatomic.h>

#include <doca_error.h>
#include <doca_dev.h>

#include "vblk_pcie_defs.h"
#include "vblk_pci_types.h"
#include "pci_spec_tlp.h"

struct vblk_pci_init_cfg {
	struct doca_dev *dev;
	uint32_t num_ep;
	bool hotplug_mode;
	bool sriov_enabled;
	uint16_t sriov_total_vfs;
	uint16_t sriov_num_vf_msix;
	uint16_t sriov_num_vf_db;
};

/* consider add device change cb */
doca_error_t vblk_pci_init(const struct vblk_pci_init_cfg *cfg);
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

	/* flr_in_progress: true while FLR is in progress (TLP thread sets true
	 * on FLR write; OE thread sets false after post-FLR cleanup chain */
	_Atomic bool flr_in_progress;

	/* flr_kickoff: one-shot signal raised by TLP/OE thread once all VQs
	 * are destroyed during an FLR window, telling the OE thread to start
	 * the post-FLR cleanup chain (fan out to per-io_ctx need_flr_teardown). */
	_Atomic bool flr_kickoff;

	/* emulated device */
	struct pcie_virtio_dev pcie_dev;

	_Atomic(pci_cfg_change_cb_t) pci_cfg_change_cb;
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

/* Opaque: the per-VF datapath controller is owned and managed by the OE thread
 * (upper layer, vblk_ctrl.c); the slot only holds a back-pointer to it. */
struct vblk_ctrl;

/*
 * VF datapath state machine. The OE thread owns state transitions; IO
 * threads only observe FLR_TEARDOWN/FLR_REBUILD to destroy or recreate
 * their per-thread VF io_ctx for a VF FLR.
 *
 * Normal VFE path:
 *
 *   DOWN -- VFE=1 / enable OE --> UP
 *    ^                            |
 *    +--- VFE=0 / disable OE -----+
 *
 * VF FLR datapath recycle:
 *
 *   DOWN or UP
 *      |
 *      | VF FLR
 *      v
 *   FLR_TEARDOWN -- all io_ctxs destroyed, OE recycled --> FLR_REBUILD
 *      ^                                                   |
 *      | new VF FLR while rebuilding                       |
 *      +---------------------------------------------------+
 *                                                          |
 *                  all io_ctxs rebuilt                     |
 *                  +-- dp_want_up=true  --> UP             |
 *                  +-- dp_want_up=false --> DOWN           |
 *
 * DOWN: VF ctrl exists, VF offload engine is disabled.
 * UP: VF ctrl exists, VF offload engine is enabled.
 * FLR_TEARDOWN: OE waits for each IO thread to destroy its VF io_ctx.
 * FLR_REBUILD: OE waits for each IO thread to recreate its VF io_ctx.
 */
enum vblk_sriov_vf_dp_state {
	VBLK_SRIOV_VF_DP_DOWN = 0,
	VBLK_SRIOV_VF_DP_UP,
	VBLK_SRIOV_VF_DP_FLR_TEARDOWN,
	VBLK_SRIOV_VF_DP_FLR_REBUILD,
};

/*
 * One SR-IOV VF: its representor, init-time VF TLP device, VF virtio template,
 * and optional per-VF datapath. The TLP thread owns cfg/pcie_dev/vf_dev/rep.
 * Final PF teardown stops the VF TLP device before the OE thread frees vf_ctrl.
 * The OE thread owns vf_ctrl and uses dp_want_up to enable or disable the VF
 * offload engine.
 */
struct vblk_sriov_vf_slot {
	struct pci_device_config *cfg;
	struct pcie_virtio_dev pcie_dev;
	struct vblk_pci_virtio_dev *vf_dev;
	struct doca_dev_rep *rep;
	uint32_t dynamic_vf_index;
	bool tlp_started;

	/* Per-VF datapath (offload engine + io_ctx + vqs), built and torn down
	 * on the OE thread. vf_ctrl is OE-owned; the TLP thread only reaches it
	 * through the stable VF config callback/cb_arg installed at build time.
	 * Zero-initialized when the slot array is calloc'd. */
	struct vblk_ctrl *vf_ctrl; /* OE-owned; NULL until built or after final teardown */
	_Atomic uint8_t dp_state;  /* OE -> IO: enum vblk_sriov_vf_dp_state */
	_Atomic bool dp_want_up;   /* TLP -> OE: VF offload engine should be enabled */

	/* VF-local datapath recycle progress. OE owns dp_state phase transitions; IO
	 * threads report completion for their ctx_id here. */
	_Atomic uint8_t flr_io_ctx_done;
};

/*
 * Per-PF SR-IOV runtime state (up to 7 VFs, no ARI).
 *
 * Allocated when SR-IOV is enabled for an endpoint. NULL when SR-IOV is
 * disabled for that endpoint.
 */
struct vblk_sriov_pf_state {
	/* Static config from CLI. */
	uint16_t total_vfs;	  /* TotalVFs / InitialVFs: 1..7 without ARI */
	uint16_t first_vf_offset; /* First VF immediately follows the PF without ARI */
	uint16_t vf_stride;	  /* Consecutive VF function numbers without ARI */
	uint16_t vf_device_id;
	uint16_t vf_num_msix;
	uint16_t vf_num_db;

	/* SR-IOV cap shadows updated by the PF extended-capability write path. */
	uint16_t control_shadow; /* VFE/MSE are writable; ARI remains clear without ARI */
	uint16_t num_vfs_shadow; /* Host-selected NumVFs */
	uint32_t system_pg_size; /* PCI_SRIOV_SYS_PGSIZE bitmap; only bit 0 is accepted */
	uint32_t vf_bar[PCI_SRIOV_NUM_BARS];

	/* TLP-side VF config entries; visibility follows VF Enable and NumVFs. */
	struct vblk_sriov_vf_slot *vf_slots;
};

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
void vblk_pci_virtio_cfg_change_set(struct vblk_pci_virtio_dev *dev, pci_cfg_change_cb_t cb, void *arg);
bool vblk_pci_virtio_cfg_change_notify(struct vblk_pci_virtio_dev *dev);
doca_error_t vblk_pci_sriov_vf_tlp_dev_stop(struct vblk_sriov_vf_slot *slot);
doca_error_t vblk_pci_sriov_vf_tlp_dev_start(struct vblk_sriov_vf_slot *slot);
doca_error_t vblk_pci_sriov_vf_tlp_dev_suspend(struct vblk_sriov_vf_slot *slot);
doca_error_t vblk_pci_sriov_vf_tlp_dev_resume(struct vblk_sriov_vf_slot *slot);
doca_error_t vblk_pci_pf_tlp_dev_suspend(struct vblk_pci_virtio_dev *dev);
doca_error_t vblk_pci_pf_tlp_dev_resume(struct vblk_pci_virtio_dev *dev);

/* Called on TLP thread main loop; invokes tlp_poll_cb for all devices */
void vblk_pci_tlp_poll(void);

/* Retry Command Completed MSIs deferred due to ACG credit exhaustion (TLP thread). */
void vblk_pci_retry_cmd_completed_msi(void);

/*
 * Consume hotplug plug/unplug MSI requests queued by the OE thread and send
 * the corresponding Memory-Write TLPs on the TLP thread. See vblk_pci.c for
 * the rationale on why every complete_acg() must originate from the TLP
 * thread (doca ctx is thread-unsafe and lib req_pool acquire()/release() is
 * not serialized across threads).
 */
struct vblk_ep_hotplug_state;
void vblk_pci_retry_hotplug_msi(struct vblk_ep_hotplug_state *hp, uint32_t num_ep);

/* Called on TLP thread main loop when block device capacity changes; invokes MSIX interrupt to driver */
void vblk_pci_notify_host(void);

/* Hotplug support */
doca_error_t vblk_pci_create_device(uint32_t ep_index);
void vblk_pci_destroy_device(uint32_t ep_index);
doca_error_t vblk_pci_trigger_hotplug(uint32_t dsp_index, bool plug);
void vblk_pci_ack_host_reset(uint32_t ep_index);

/* rw pointer to the current pci configuration */
struct vblk_pci_virtio_pci_common_cfg *vblk_pci_virtio_get_pci_cfg(struct vblk_pci_virtio_dev *dev);
/* ro queues */
const struct vblk_pci_virtq_pci_cfg *vblk_pci_virtio_get_virtq_pci_cfg(struct vblk_pci_virtio_dev *dev);
/* ro device config */
const struct vblk_pci_virtio_blk_config *vblk_pci_virtio_get_vblk_dev_cfg(struct vblk_pci_virtio_dev *dev);

/* Set block device capacity (called from TLP thread via stdin command) */
void vblk_pci_set_capacity(uint64_t capacity_bytes);

#endif
