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

#ifndef VBLK_CTRL_LU_H_
#define VBLK_CTRL_LU_H_

#include <stdint.h>

#include <doca_error.h>
#include <doca_pe.h>
#include <doca_dev.h>
#include <stdatomic.h>
#include <doca_devemu_vblk_offload_engine.h>

#include "vblk_pci_types_lu.h"

/* Queue lifecycle state (zero-initialized to DESTROYED) */
enum vblk_vq_state {
	/* VQ does not exist. Initial state or after doca_devemu_vblk_req_vq_destroy() completes. */
	VBLK_VQ_DESTROYED = 0,

	/* doca_devemu_virtio_vq_stop() called, waiting for completion. Maps to DOCA_DEVEMU_VIRTIO_VQ_STATE_DISABLING.
	 */
	VBLK_VQ_STOPPING,

	/* doca_devemu_virtio_vq_disable() + doca_devemu_virtio_io_unbind_vq() pending. Maps to
	 * DOCA_DEVEMU_VIRTIO_VQ_STATE_DISABLING. */
	VBLK_VQ_UNBINDING,

	/* doca_devemu_vblk_req_vq_create() + doca_devemu_virtio_vq_set_conf() + doca_devemu_virtio_vq_start() pending.
	 */
	VBLK_VQ_STARTING,

	/* VQ created + started with DB UNMAPPED. Used during LU handover:
	 * VQ waits here until SRC releases doorbells, then advances to BINDING. */
	VBLK_VQ_CONFIGURED,

	/* doca_devemu_virtio_io_bind_vq() + doca_devemu_virtio_vq_enable() pending. */
	VBLK_VQ_BINDING,

	/* VQ fully operational. doca_devemu_virtio_vq_enable() completed. Maps to DOCA_DEVEMU_VIRTIO_VQ_STATE_ENABLED.
	 */
	VBLK_VQ_RUNNING,
};
/* Per-device VQ configuration state from the host driver.
 * Shared between vblk_ctrl (authoritative) and LU messages. */
struct vblk_vq_cfg {
	uint16_t num_queues;
	struct vblk_pci_virtq_pci_cfg vqs[VBLK_PCI_VIRTIO_MAX_QUEUES];
};

/*
 * Snapshot of the guest-visible VirtIO PCI state.
 * Written by TLP, consumed by EMU.  Magic header/footer guard
 * data integrity across the IPC boundary.
 *
 * Also used as the LU device configuration carried in GET_STATE_ACK
 * and BEGIN_ACK during handover.
 */
#define VBLK_EMU_STATE_MAGIC 0xDB1CADA5U

struct vblk_lu_device_cfg {
	uint32_t magic_header;
	uint8_t device_status;
	uint8_t _pad[7];
	uint64_t driver_features;
	struct vblk_vq_cfg vq_cfg;
	uint32_t magic_footer;
};

#define vblk_emu_state vblk_lu_device_cfg

struct vblk_vq {
	_Atomic(struct doca_devemu_vblk_req_vq *) vq;
	_Atomic enum vblk_vq_state state;
	uint16_t io_ctx_id;
	struct vblk_pci_virtq_pci_cfg cfg;
};

/*
 * Each progress thread (io, offload engine) has a pending count of work it is expected to
 * complete (vblk_poll_state->pending). If this count is mistaken (due to a bug) work might
 * be ignored, for example io thread might never flush and unbind a queue. Enable this macro
 * to periodically poll pending events even if it was not requested (via vblk_poll_state->pending),
 * by reading the state directly. This is OFF by default to avoid added latency of iterating
 * owned vq's, but could be useful in the future for debugging state machine.
 *
 * VBLK_POLL_FALLBACK_ENABLE=0 (Use in release, for low latency, high performance) ->
 *     incorrect state tracking will result in queues stuck in their current state
 *     (state change will never be acknowledged)
 *
 * VBLK_POLL_FALLBACK_ENABLE=1 (Use in debugging) for queues not acknowledging states ->
 *     state tracking will be polled, even if work tracking is inaccurate, increases latency.
 */
#define VBLK_POLL_FALLBACK_ENABLE 0

#define VBLK_POLL_MODULO 1024

struct vblk_poll_state {
	_Atomic uint32_t pending;
	uint64_t counter;
};

#define VBLK_PCI_DEV_MAX_QUEUES 255
#define VBLK_APP_BF3_MAX_CORES 16
#define VBLK_APP_BF3_MAX_IO_CORES (VBLK_APP_BF3_MAX_CORES - 1)
#define VBLK_CTRLS_MAX_SEG_MAX 128

struct vblk_app_cfg {
	char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE];
	bool datapath_on_dpa;
	bool indirect_enabled;
};

#if VBLK_POLL_FALLBACK_ENABLE
#define vblk_poll_skip(ps) \
	(atomic_load_explicit(&(ps)->pending, memory_order_relaxed) == 0 && (++(ps)->counter % VBLK_POLL_MODULO) != 0)
#else
#define vblk_poll_skip(ps) (atomic_load_explicit(&(ps)->pending, memory_order_relaxed) == 0)
#endif
#define VBLK_CTRL_MAX_QUEUE_SIZE 256

#define VBLK_CTRL_CAPACITY_BYTES (1024UL * 1024UL * 1024UL)
#define VBLK_GET_ID_SIZE 20 /* SPEC: id size is 20 bytes, nul padded */
#define VBLK_CTRL_DEFAULT_ID "vblk_bdev0"

struct vblk_ctrl_io_ctx {
	struct doca_dma *dma_ctx;
	struct doca_devemu_vblk_io *vq_io_ctx;
	struct doca_pe *pe;
	struct vblk_ctrl *ctrl;
	struct vblk_mpool_set *mpool;
	/*
	 * Queue state transitions which are to be done by the io engine are
	 * acknowledged when the pending counter for that io context is incremented.
	 * This is done automatically when using vblk_vq_set_state. This prevents
	 * the io context from polling state changes for each queue.
	 */
	struct vblk_poll_state poll;
	uint32_t stats_ios;
	bool stats_wip;
};

struct vblk_ctrl {
	struct doca_devemu_vblk_offload_engine *vq_engine;
	struct vblk_ctrl_io_ctx io_ctxs[VBLK_APP_BF3_MAX_IO_CORES];
	struct vblk_vq *vqs;
	char vblk_dev_id[VBLK_GET_ID_SIZE];
	struct doca_devemu_virtio_queue_dbg_state **state_list;
	uint32_t ios_period;
	uint32_t list_len;
	/*
	 * shared flag: only one ctx can populate at a time
	 */
	_Atomic bool stats_populate_wip;
	/* Set when DRIVER_OK arrives during reset; offload engine checks this
	   after teardown completes to start VQs with cached configs. */
	_Atomic bool pending_queue_start;
	bool handover_dst; /* LU: park VQs at CONFIGURED instead of advancing to BINDING */

	/* Recovery: if set before vblk_ctrl_init, OE is created via create_from_export.
	 * Use vblk_ctrl_set_recovery_export / vblk_ctrl_release_recovery_export. */
	const void *recovery_export_desc;
	size_t recovery_export_desc_len;
	/*
	 * Queue state transitions which are to be done by the offload engine are
	 * acknowledged when the pending counter for that engine is incremented.
	 * This is done automatically when using vblk_vq_set_state.
	 */
	struct vblk_poll_state engine_poll;
	uint16_t num_queues;
	uint8_t num_io_ctx;
};

struct vblk_io_request {
	uint32_t type;
	uint64_t lba;
	struct doca_devemu_vblk_req *doca_req;
	struct doca_buf *dpu_buf;
};

struct vblk_ctrl_attrs {
	uint16_t seg_max;      /* Spec virtio_blk_config::seg_max */
	uint16_t num_queues;   /* Number of virtio queues */
	uint8_t num_io_ctx;    /* Number of IO contexts */
	bool indirect_enabled; /* True: enable Spec VIRTIO_F_INDIRECT_DESC feature */
};

doca_error_t vblk_init(const struct vblk_app_cfg *app_cfg, struct doca_dev **g_dev);
void vblk_reset(void);

doca_error_t vblk_ctrl_set_recovery_export(struct vblk_ctrl *ctrl, const void *desc, size_t len);
void vblk_ctrl_release_recovery_export(struct vblk_ctrl *ctrl);
doca_error_t vblk_ctrl_init(struct vblk_ctrl *ctrl, const struct vblk_ctrl_attrs *attr, struct doca_devemu_pci_ep *ep);
void vblk_ctrl_cleanup(struct vblk_ctrl *ctrl);

/* Must be invoked on the thread that will manage/progress the IO context */
doca_error_t vblk_ctrl_init_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id, struct doca_pe *pe);

/* Enable the virtio offload engine once all IO contexts are created and started */
doca_error_t vblk_ctrl_enable(struct vblk_ctrl *ctrl);

/* Stop queues and IO contexts for a given ctx_id (must be invoked on the managing thread) */
doca_error_t vblk_ctrl_shutdown_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id);

doca_error_t vblk_wait_ctx_idle(struct doca_pe *pe, struct doca_ctx *ctx, const char *name, int max_iters);
/* Must be invoked on the offload-engine thread (never blocks, progresses queue lifecycle) */
struct vblk_poll_state *vblk_ctrl_engine_poll(struct vblk_ctrl *ctrl);
void vblk_ctrl_offload_engine_progress(struct vblk_ctrl *ctrl);
struct vblk_poll_state *vblk_ctrl_io_ctx_poll(struct vblk_ctrl *ctrl, uint8_t ctx_id);
void vblk_ctrl_io_context_progress(struct vblk_ctrl *vblk_ctrl, uint8_t core_id);

/* Apply a state snapshot — level-based, no previous state needed */
void vblk_ctrl_apply_state(struct vblk_ctrl *ctrl, const struct vblk_lu_device_cfg *state);

/* IPC-driven queue operations */
doca_error_t vblk_ctrl_ipc_queue_start(struct vblk_ctrl *ctrl, uint16_t qid, const struct vblk_pci_virtq_pci_cfg *cfg);
doca_error_t vblk_ctrl_ipc_device_reset(struct vblk_ctrl *ctrl);

/* Advance all CONFIGURED VQs to STARTING (called by DST after SRC releases doorbells) */
void vblk_ctrl_resume_configured_vqs(struct vblk_ctrl *ctrl);

#endif
