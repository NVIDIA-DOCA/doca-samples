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

#ifndef VBLK_CTRL_H_
#define VBLK_CTRL_H_

#include <stdint.h>
#include <pthread.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_dev.h>
#include <stdatomic.h>
#include <doca_devemu_vblk_offload_engine.h>

#include "vblk_pci.h"

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

	/* doca_devemu_virtio_io_bind_vq() + doca_devemu_virtio_vq_enable() pending. */
	VBLK_VQ_BINDING,

	/* VQ fully operational. doca_devemu_virtio_vq_enable() completed. Maps to DOCA_DEVEMU_VIRTIO_VQ_STATE_ENABLED.
	 */
	VBLK_VQ_RUNNING,
};
struct vblk_vq {
	_Atomic(struct doca_devemu_vblk_req_vq *) vq;
	_Atomic enum vblk_vq_state state;
	uint16_t io_ctx_id;
	struct vblk_pci_virtq_pci_cfg cfg; /* cached config from TLP thread */
	/* Set when host reset arrives while this VQ is in BINDING (bind in flight,
	 * possible post-DRIVER_OK IO). Teardown is deferred until the IO thread
	 * completes bind, then it drives the standard UNBINDING path. */
	_Atomic bool reset_deferred;

	/* One-token flag: true while this VQ holds exactly one unit in
	 * ctrl->pending_vqs. Set (and pending_vqs++) when teardown starts,
	 * cleared (and pending_vqs--) at DESTROYED, both via atomic exchange, so
	 * each VQ contributes at most once even under concurrent threads. */
	_Atomic bool pending_counted;
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

/*
 * Max supported queues are 255
 * Note that the HW supports 256 but this is enforced to prevent dealing with queues sharing MSIX.
 */
#define VBLK_APP_MAX_QUEUES 255
#define VBLK_APP_BF3_MAX_CORES 16
#define VBLK_APP_BF3_MAX_IO_CORES (VBLK_APP_BF3_MAX_CORES - 1)

#define VBLK_CTRLS_MAX_SEG_MAX 128

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
	struct doca_pe *dma_pe;
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
	_Atomic bool need_flr_teardown; /* OE -> IO: shutdown + destroy this io_ctx on its owning thread */
};

struct vblk_ctrl {
	struct vblk_pci_virtio_dev *vblk_pci_dev;
	struct doca_devemu_vblk_offload_engine *vq_engine;
	struct vblk_ctrl_io_ctx io_ctxs[VBLK_APP_BF3_MAX_IO_CORES];
	struct vblk_vq *vqs;
	char vblk_dev_id[VBLK_GET_ID_SIZE];
	struct doca_devemu_virtio_queue_dbg_state **state_list;
	uint32_t ios_period;
	uint32_t list_len;
	uint32_t query_stats_cnt;
	bool inited;
	/*
	 * shared flag: only one ctx can populate at a time
	 */
	_Atomic bool stats_populate_wip;
	/*
	 * Flag to defer stats list destruction to IO thread (avoid blocking TLP thread)
	 */
	_Atomic bool stats_destroy_pending;
	/*
	 * Last observed virtio device_status (for edge-triggered reset handling on TLP thread)
	 */
	uint8_t last_device_status;
	/*
	 * Count of VQs pending cleanup during reset. Includes queues in any non-DESTROYED
	 * state (including STARTING/BINDING from unfinished startup).
	 * Atomic: TLP increments during reset begin, offload engine decrements on VQ destroy.
	 */
	_Atomic uint16_t pending_vqs;

	/* Host reset arrived while DRIVER_OK/bind handling may still be in flight.
	 * While set, IO threads complete bind then tear down instead of RUNNING.
	 * Cleared when all counted VQs reach DESTROYED. */
	_Atomic bool reset_pending;

	/*
	 * Queue state transitions which are to be done by the offload engine are
	 * acknowledged when the pending counter for that engine is incremented.
	 * This is done automatically when using vblk_vq_set_state.
	 */
	struct vblk_poll_state engine_poll;
	uint16_t num_queues;
	uint8_t num_io_ctx;
	/* Round-robin base for queue->io_ctx mapping: io_ctx_id = (io_ctx_base + qid) %
	 * num_io_ctx. 0 for a PF. For an SR-IOV VF it continues the rotation from its
	 * PF (and across the PF's VFs) so VF queues scatter across all cores instead of
	 * every function restarting on the low cores. Per-PF; does not cross PFs. */
	uint16_t io_ctx_base;
	/* True while the offload engine is enabled. Tracked so teardown only
	 * disables an engine that is actually enabled: a VFE-disabled VF (or a PF
	 * mid-FLR) has its engine already disabled, and calling disable again makes
	 * the SDK log a spurious "offload engine is not enabled" error. */
	bool engine_enabled;
	uint8_t indirect_enabled;
	uint32_t ep_index;
	uint16_t seg_max;
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
	uint32_t ep_index;     /* Endpoint index in the PCI switch topology */
	bool indirect_enabled; /* True: enable Spec VIRTIO_F_INDIRECT_DESC feature */
};

doca_error_t vblk_init(const char *device_name, bool indirect_enabled, struct doca_dev **g_dev);
void vblk_reset(void);

struct vblk_mpool_set;

doca_error_t vblk_ctrl_init(struct vblk_ctrl *ctrl, const struct vblk_ctrl_attrs *attr);
/* Best-effort cleanup. Returns:
 *   DOCA_SUCCESS       - engine stopped + destroyed, ctrl fully released.
 *   other doca_error_t - destroy itself failed (very rare); ctrl partially
 *     cleaned -- the EP slot is unusable until vblk restart. */
doca_error_t vblk_ctrl_cleanup(struct vblk_ctrl *ctrl, struct doca_pe *pe);

doca_error_t vblk_ctrl_connect_io_ctx(struct vblk_ctrl *ctrl, const int ctx_id, struct doca_pe *pe);

/* Must be invoked on the thread that will manage/progress the IO context.
 * If shared_mpool is non-NULL, the IO context will use it instead of creating its own. */
doca_error_t vblk_ctrl_init_io_ctx_on_thread(struct vblk_ctrl *ctrl,
					     uint16_t ctx_id,
					     struct doca_pe *pe,
					     struct vblk_mpool_set *shared_mpool);

/* Enable the virtio offload engine once all IO contexts are created and started */
doca_error_t vblk_ctrl_enable(struct vblk_ctrl *ctrl);
doca_error_t vblk_ctrl_disable(struct vblk_ctrl *ctrl);

/* Stop queues and IO contexts for a given ctx_id (must be invoked on the managing thread) */
doca_error_t vblk_ctrl_shutdown_io_ctx_on_thread(struct vblk_ctrl *ctrl, uint16_t ctx_id);

doca_error_t vblk_wait_ctx_idle(struct doca_pe *pe, struct doca_ctx *ctx, const char *name, int max_iters);
/* Must be invoked on the offload-engine thread (never blocks, progresses queue lifecycle) */
void vblk_ctrl_io_ctx_reset(struct vblk_ctrl_io_ctx *io_ctx, bool owns_mpool);
struct vblk_poll_state *vblk_ctrl_engine_poll(struct vblk_ctrl *ctrl);
void vblk_ctrl_offload_engine_progress(struct vblk_ctrl *ctrl);
doca_error_t vblk_ctrl_recycle_vq_backends(struct vblk_ctrl *ctrl);
void vblk_ctrl_replay_enabled_vqs(struct vblk_ctrl *ctrl);
struct vblk_poll_state *vblk_ctrl_io_ctx_poll(struct vblk_ctrl *ctrl, uint8_t ctx_id);
struct doca_pe *vblk_ctrl_io_ctx_dma_pe(struct vblk_ctrl *ctrl, uint8_t ctx_id);
void vblk_ctrl_io_context_progress(struct vblk_ctrl *vblk_ctrl, uint8_t core_id);
void vblk_ctrl_cleanup_vf(struct vblk_ctrl *ctrl, struct doca_pe *pe);
/* Build a VF's offload engine only (no io_ctx, left DISABLED). The per-thread
 * io_ctxs are created separately via vblk_ctrl_init_io_ctx_on_thread on each IO
 * thread, and the engine is enabled on the VFE 0->1 edge. num_io_ctx gives the
 * VF PF-parity fan-out; io_ctx_base offsets the queue->core rotation so VF
 * queues continue from the PF instead of restarting at core 0. */
doca_error_t vblk_ctrl_init_vf_engine(struct vblk_ctrl *ctrl,
				      struct vblk_pci_virtio_dev *vf_dev,
				      uint16_t num_queues,
				      uint16_t seg_max,
				      bool indirect_enabled,
				      uint8_t num_io_ctx,
				      uint16_t io_ctx_base);

#endif
