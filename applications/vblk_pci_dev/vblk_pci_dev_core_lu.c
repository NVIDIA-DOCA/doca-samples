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

#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "vblk_ctrl_lu.h"
#include "vblk_io_ctx_lu.h"
#include "vblk_ipc.h"
#include "vblk_ipc_msgs.h"
#include "vblk_handover.h"
#include "vblk_pci_dev_core_lu.h"

DOCA_LOG_REGISTER(VBLK_PCI_DEV_CORE);

/*
 * State flow diagram:
 *
 * +----------------------------------- STARTUP -----------------------------------+
 * |                                                                               |
 * |  INIT                                                                         |
 * |    |                                                                          |
 * |    |  vblk_ctrl_init()                                                        |
 * |    |    - create PCI endpoint (vblk_pci_virtio_dev_create)                    |
 * |    |    - create offload engine (doca_devemu_vblk_offload_engine_create)      |
 * |    |    - start offload engine (doca_devemu_virtio_offload_engine_start)      |
 * |    v                                                                          |
 * |  CTRL_CREATED -----------------------------------------------------> ERROR    |
 * |    |                                                                   ^      |
 * |    |  vblk_ctrl_init_io_ctx_on_thread() [per IO context]               |      |
 * |    |    - init IO context (vblk_ctrl_io_ctx_init)                      |      |
 * |    |    - connect to PE (vblk_ctrl_connect_io_ctx)                     |      |
 * |    |    - start IO context (doca_ctx_start)                            |      |
 * |    v                                                                   |      |
 * |  IO_READY -------------------------------------------------------------+      |
 * |    |                                                                   |      |
 * |    |  vblk_ctrl_enable()                                               |      |
 * |    |    - enable offload engine (virtio_offload_engine_enable)         |      |
 * |    v                                                                   |      |
 * |  CTRL_ENABLED ---------------------------------------------------------+      |
 * |    |                                                                          |
 * |    |  [normal operation - processing I/O requests]                            |
 * |                                                                               |
 * +-------------------------------------------------------------------------------+
 *
 * +----------------------------------- TEARDOWN ----------------------------------+
 * |                                                                               |
 * |  force_quit = true (signal handler or error)                                  |
 * |    |                                                                          |
 * |    +--> IO context threads:                                                   |
 * |    |      vblk_ctrl_shutdown_io_ctx_on_thread()                               |
 * |    |        - stop queues mapped to this ctx (vblk_ctrl_queue_stop)           |
 * |    |        - stop DMA & IO contexts (doca_ctx_stop)                          |
 * |    |      signal_io_exited()                                                  |
 * |    |                                                                          |
 * |  Offload engine thread (waits for above):                                     |
 * |    wait_all_io_exited()                                                       |
 * |    vblk_ctrl_cleanup()                                                        |
 * |      - reset IO contexts (vblk_ctrl_io_ctxs_reset)                            |
 * |      - disable offload engine (virtio_offload_engine_disable)                 |
 * |      - stop offload engine (virtio_offload_engine_stop)                       |
 * |      - destroy offload engine (vblk_offload_engine_destroy)                   |
 * |      - destroy PCI device (vblk_pci_virtio_dev_destroy)                       |
 * |                                                                               |
 * +-------------------------------------------------------------------------------+
 */

#define EMU_INITIAL_STATE_TIMEOUT_MS 5000

enum vblk_app_run_state_e {
	VBLK_APP_INIT = 0,
	VBLK_APP_CTRL_CREATED,
	VBLK_APP_IO_READY,
	VBLK_APP_CTRL_ENABLED,
	VBLK_APP_ERROR,
};

/* Atomic state variables for thread synchronization */
static _Atomic int vblk_app_run_state;
static _Atomic uint8_t vblk_app_io_ready;
static _Atomic uint8_t vblk_app_io_exited;
static _Atomic doca_error_t vblk_app_err = DOCA_SUCCESS;
static _Atomic bool vblk_app_pre_enable_done;

/* Application resources (accessed by callbacks via file-scope reference) */
static struct vblk_pci_dev_resources s_resources;

/* Static IO ops structure - must outlive threads that reference it */
static struct vblk_io_ctx_app_ops s_io_ops;

/**
 * @brief Signal that the VBlk controller has been created (or failed)
 *
 * Called by the offload engine thread after vblk_ctrl_init() completes.
 * On failure, sets the error state and requests graceful shutdown.
 *
 * @param[in] err Result of controller creation
 * @param[in] ctrl Created controller handle (NULL on failure)
 */
static void app_signal_ctrl_created(doca_error_t err, struct vblk_ctrl *ctrl)
{
	(void)ctrl;
	if (err != DOCA_SUCCESS) {
		vblk_app_err = (int)err;
		vblk_app_run_state = VBLK_APP_ERROR;
		force_quit = true;
	} else {
		vblk_app_run_state = VBLK_APP_CTRL_CREATED;
	}
}

/**
 * @brief Signal that one IO context is ready
 *
 * Increments the ready counter. When all IO contexts are ready, transitions
 * the application state to VBLK_APP_IO_READY.
 */
static void app_signal_io_ready(void)
{
	if (++vblk_app_io_ready == s_resources.num_io_ctx)
		vblk_app_run_state = VBLK_APP_IO_READY;
}

/**
 * @brief Signal that the controller has been enabled (or failed)
 *
 * @param[in] err Result of controller enable operation
 */
static void app_signal_ctrl_enabled(doca_error_t err)
{
	if (err != DOCA_SUCCESS) {
		vblk_app_err = (int)err;
		vblk_app_run_state = VBLK_APP_ERROR;
		force_quit = true;
	} else {
		vblk_app_run_state = VBLK_APP_CTRL_ENABLED;
	}
}

/**
 * @brief Signal that one IO context thread has exited
 */
static void app_signal_io_exited(void)
{
	++vblk_app_io_exited;
}

/**
 * @brief Block until the controller is created or an error occurs
 *
 * @param[out] ctrl_out Pointer to receive the controller handle
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t app_wait_ctrl_created(struct vblk_ctrl **ctrl_out)
{
	while (!force_quit) {
		const int err = vblk_app_err;
		if (err != DOCA_SUCCESS)
			return (doca_error_t)err;
		if (vblk_app_run_state >= VBLK_APP_CTRL_CREATED) {
			*ctrl_out = &s_resources.vblk_ctrl;
			return DOCA_SUCCESS;
		}
		sched_yield();
	}
	return (doca_error_t)vblk_app_err;
}

/**
 * @brief Block until all IO contexts are ready or an error occurs
 *
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t app_wait_io_ready(void)
{
	while (!force_quit) {
		const int err = vblk_app_err;
		if (err != DOCA_SUCCESS)
			return (doca_error_t)err;
		if (vblk_app_run_state >= VBLK_APP_IO_READY)
			return DOCA_SUCCESS;
		sched_yield();
	}
	return (doca_error_t)vblk_app_err;
}

/**
 * @brief Block (on OE thread) until the main thread finishes pre-enable work (export)
 */
static doca_error_t app_wait_pre_enable(void)
{
	while (!force_quit && !vblk_app_pre_enable_done) {
		const int err = vblk_app_err;
		if (err != DOCA_SUCCESS)
			return (doca_error_t)err;
		sched_yield();
	}
	return (doca_error_t)vblk_app_err;
}

/**
 * @brief Block until the controller is enabled or an error occurs
 *
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t app_wait_ctrl_enabled(void)
{
	while (!force_quit) {
		const int err = vblk_app_err;
		if (err != DOCA_SUCCESS)
			return (doca_error_t)err;
		if (vblk_app_run_state >= VBLK_APP_CTRL_ENABLED)
			return DOCA_SUCCESS;
		sched_yield();
	}
	return (doca_error_t)vblk_app_err;
}

/**
 * @brief Block until the expected number of IO contexts have exited
 *
 * @param[in] expected Number of IO context exits to wait for
 */
static void app_wait_all_io_exited(uint8_t expected)
{
	while (vblk_app_io_exited < expected)
		sched_yield();
}

/**
 * @brief Create IO context and offload engine threads
 *
 * Initializes IO ops and spawns one thread per IO context. The thread
 * whose affinity matches offload_engine_core_idx also runs the offload engine.
 *
 * @param[in] res Application resources
 * @param[in] config Application configuration
 * @param[in] io_cfgs IO context thread configs array
 * @param[in] io_ctx_cores CPU core mapping for IO contexts
 * @param[out] num_created Number of threads successfully created
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t vblk_io_ctx_oe_threads_create(struct vblk_pci_dev_resources *res,
						  struct vblk_pci_dev_config *config,
						  struct vblk_io_ctx_cfg *io_cfgs,
						  uint8_t *io_ctx_cores,
						  uint8_t *num_created)
{
	doca_error_t err;

	*num_created = 0;

	/* Initialize static IO ops structure - must be static because threads use it after function returns */
	s_io_ops.signal_ctrl_created = app_signal_ctrl_created;
	s_io_ops.signal_io_ready = app_signal_io_ready;
	s_io_ops.signal_ctrl_enabled = app_signal_ctrl_enabled;
	s_io_ops.signal_io_exited = app_signal_io_exited;
	s_io_ops.wait_ctrl_created = app_wait_ctrl_created;
	s_io_ops.wait_io_ready = app_wait_io_ready;
	s_io_ops.wait_pre_enable = app_wait_pre_enable;
	s_io_ops.wait_all_io_exited = app_wait_all_io_exited;

	for (uint8_t ctx_id = 0; ctx_id < res->num_io_ctx; ctx_id++) {
		io_cfgs[ctx_id].pe = res->io_pe_ctxs[ctx_id].pe;
		io_cfgs[ctx_id].ctrl = &res->vblk_ctrl;
		io_cfgs[ctx_id].ctx_id = ctx_id;
		io_cfgs[ctx_id].affinity_core = io_ctx_cores[ctx_id];
		io_cfgs[ctx_id].offload_engine_core_idx = config->offload_engine_core_idx;
		io_cfgs[ctx_id].num_io_ctx = res->num_io_ctx;
		io_cfgs[ctx_id].num_queues = config->num_queues;
		io_cfgs[ctx_id].seg_max = config->seg_max ? config->seg_max : 1;
		io_cfgs[ctx_id].indirect_enabled = config->indirect_enabled;
		io_cfgs[ctx_id].stats_ios_period = config->stats_ios_period;
		io_cfgs[ctx_id].ops = &s_io_ops;

		err = vblk_io_ctx_thread_create(&io_cfgs[ctx_id], &res->io_pe_ctxs[ctx_id].thread);
		if (err != DOCA_SUCCESS)
			return err;
		(*num_created)++;
	}
	return DOCA_SUCCESS;
}

/**
 * @brief Validate and apply defaults to application configuration
 *
 * Applies default values for unset fields, validates constraints between
 * parameters, and builds the io_ctx_cores array from the CPU mask.
 *
 * @param[in,out] cfg Configuration to validate (defaults applied in-place)
 * @param[out] io_ctx_cores CPU core mapping array to populate
 * @param[out] num_io_ctx Number of IO contexts derived from mask
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE on constraint violation
 */
static doca_error_t vblk_app_cfg_validate(struct vblk_pci_dev_config *cfg, uint8_t *io_ctx_cores, uint8_t *num_io_ctx)
{
	/* Apply defaults */
	if (cfg->num_queues == 0)
		cfg->num_queues = VBLK_PCI_DEV_DEFAULT_NUM_QUEUES;

	if (cfg->io_ctx_mask == 0) {
		cfg->io_ctx_mask = VBLK_PCI_DEV_DEFAULT_IO_CTX_MASK;
		cfg->offload_engine_core_idx = VBLK_PCI_DEV_DEFAULT_OFFLOAD_ENGINE_CORE_IDX;
	}

	/* Validate constraints */
	if (cfg->io_ctx_mask == 0) {
		DOCA_LOG_ERR("io_ctx_mask must have at least one bit set");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if ((cfg->io_ctx_mask & (1UL << cfg->offload_engine_core_idx)) == 0) {
		DOCA_LOG_ERR("offload_engine_core_idx (%u) must be in io_ctx_mask (0x%lx)",
			     cfg->offload_engine_core_idx,
			     cfg->io_ctx_mask);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Build io_ctx_cores array from mask */
	*num_io_ctx = 0;
	for (uint8_t c = 0; c < VBLK_APP_BF3_MAX_CORES; c++) {
		if (cfg->io_ctx_mask & (1UL << c))
			io_ctx_cores[(*num_io_ctx)++] = c;
	}

	DOCA_LOG_INFO(
		"Configuration validated: queues=%u seg_max=%u io_ctx_mask=0x%lx (%u cores) offload_engine_core_idx=%u",
		cfg->num_queues,
		cfg->seg_max,
		cfg->io_ctx_mask,
		*num_io_ctx,
		cfg->offload_engine_core_idx);

	return DOCA_SUCCESS;
}

/**
 * @brief Reset atomic state variables for a fresh run
 */
static void vblk_app_reset_atomic_state(void)
{
	vblk_app_run_state = VBLK_APP_INIT;
	vblk_app_io_ready = 0;
	vblk_app_io_exited = 0;
	vblk_app_err = DOCA_SUCCESS;
	vblk_app_pre_enable_done = false;
}

static doca_error_t vblk_pci_dev_resources_init(uint8_t num_io_ctx, struct vblk_pci_dev_resources *resources)
{
	resources->io_pe_ctxs = calloc(num_io_ctx, sizeof(struct vblk_pci_dev_pe_context));
	if (resources->io_pe_ctxs == NULL)
		return DOCA_ERROR_NO_MEMORY;

	resources->num_io_ctx = num_io_ctx;
	return DOCA_SUCCESS;
}

static void vblk_pci_dev_resources_cleanup(struct vblk_pci_dev_resources *resources)
{
	if (resources == NULL)
		return;
	free(resources->io_pe_ctxs);
	resources->io_pe_ctxs = NULL;
	resources->num_io_ctx = 0;
}

static doca_error_t emu_request_initial_state(struct vblk_ipc_ep *ipc, struct vblk_ctrl *ctrl)
{
	static uint8_t buf[VBLK_IPC_MAX_PAYLOAD];
	uint32_t type, len;
	struct pollfd pfd = {.fd = ipc->fd, .events = POLLIN};

	vblk_ipc_send(ipc, VBLK_MSG_STATE_REQUEST, NULL, 0);

	while (poll(&pfd, 1, EMU_INITIAL_STATE_TIMEOUT_MS) > 0) {
		if (vblk_ipc_recv(ipc, &type, buf, sizeof(buf), &len) != DOCA_SUCCESS)
			continue;
		if (type == VBLK_MSG_RECOVERY_STATE && len > 0) {
			doca_error_t alloc_err = vblk_ctrl_set_recovery_export(ctrl, buf, len);
			if (alloc_err != DOCA_SUCCESS)
				return alloc_err;
			DOCA_LOG_INFO("EMU SRC: recovery descriptor (%u bytes)", len);
		}
		/* STATE_UPDATE carries VQ configs that require an initialized ctrl
		 * (num_queues, vqs[]).  At this point ctrl is not yet created, so
		 * we defer: a second STATE_REQUEST is sent after ctrl_enable. */
		if (type == VBLK_MSG_STATE_UPDATE)
			return DOCA_SUCCESS;
	}

	DOCA_LOG_ERR("EMU SRC: timed out waiting for initial state from TLP");
	return DOCA_ERROR_TIME_OUT;
}

static doca_error_t emu_dst_collect_initial_state(struct vblk_ho_ctx *ho, struct vblk_ctrl *ctrl)
{
	doca_error_t err = vblk_ho_dst_get_initial_state(ho);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU DST: initial state collection failed");
		return err;
	}
	err = vblk_ctrl_set_recovery_export(ctrl, ho->export_desc, ho->export_desc_len);
	if (err != DOCA_SUCCESS)
		return err;
	ctrl->handover_dst = true;
	return DOCA_SUCCESS;
}

static void emu_dst_rebind_as_src(struct vblk_ho_ctx *ho)
{
	doca_error_t err;

	DOCA_LOG_INFO("EMU DST: handover done — rebinding as SRC");
	vblk_ipc_ep_close(ho->ipc);
	err = vblk_ipc_ep_open(ho->ipc, VBLK_IPC_EMU_SRC_PATH, VBLK_IPC_TLP_PATH);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU DST: failed to rebind IPC as SRC: %s", doca_error_get_name(err));
		return;
	}
	ho->phase = VBLK_HO_IDLE;
	ho->role = VBLK_HO_ROLE_SRC;
	ho->peer_path = VBLK_IPC_EMU_DST_PATH;

	vblk_ipc_send(ho->ipc, VBLK_MSG_EXPORT_DESC, ho->export_desc, ho->export_desc_len);
	DOCA_LOG_INFO("EMU SRC: sent export descriptor (%zu bytes)", ho->export_desc_len);
}

static void emu_ipc_poll(void *arg)
{
	struct vblk_ho_ctx *ho = arg;
	static union {
		struct vblk_lu_device_cfg state;
		uint8_t raw[VBLK_IPC_MAX_PAYLOAD];
	} buf;
	uint32_t type, len;

	while (vblk_ipc_recv(ho->ipc, &type, &buf, sizeof(buf), &len) == DOCA_SUCCESS) {
		if (vblk_ho_process_msg(ho, type, &buf, len))
			continue;

		if (type == VBLK_MSG_STATE_UPDATE && len >= sizeof(struct vblk_lu_device_cfg))
			vblk_ctrl_apply_state(&s_resources.vblk_ctrl, &buf.state);
	}

	vblk_ho_poll(ho);

	if (ho->role == VBLK_HO_ROLE_SRC && vblk_ho_is_done(ho)) {
		DOCA_LOG_INFO("EMU SRC: handover done — requesting exit");
		ho->ipc->path[0] = '\0';
		force_quit = true;
	}

	if (ho->role == VBLK_HO_ROLE_DST && vblk_ho_is_done(ho))
		emu_dst_rebind_as_src(ho);
}

doca_error_t vblk_pci_dev_emu_run(struct vblk_pci_dev_config *config,
				  struct doca_devemu_pci_ep *ep,
				  struct vblk_ipc_ep *ipc,
				  int role)
{
	uint8_t io_ctx_cores[VBLK_APP_BF3_MAX_IO_CORES];
	struct vblk_io_ctx_cfg *io_cfgs = NULL;
	uint8_t num_io_ctx = 0, num_io_threads = 0;
	doca_error_t err;

	static struct vblk_ho_ctx ho_ctx;
	vblk_ho_init(&ho_ctx, (enum vblk_ho_role)role, &s_resources.vblk_ctrl, ipc);

	DOCA_LOG_INFO("EMU: initializing...");

	memset(&s_resources, 0, sizeof(s_resources));
	vblk_app_reset_atomic_state();
	err = vblk_app_cfg_validate(config, io_ctx_cores, &num_io_ctx);
	if (err != DOCA_SUCCESS)
		return err;

	err = vblk_pci_dev_resources_init(num_io_ctx, &s_resources);
	if (err != DOCA_SUCCESS)
		return err;

	if (role == VBLK_HO_ROLE_SRC)
		err = emu_request_initial_state(ipc, &s_resources.vblk_ctrl);
	else
		err = emu_dst_collect_initial_state(&ho_ctx, &s_resources.vblk_ctrl);
	if (err != DOCA_SUCCESS)
		return err;

	bool is_recovery = (s_resources.vblk_ctrl.recovery_export_desc != NULL);

	/* SRC recovery skips re-export, so seed ho_ctx.export_desc
	 * from the recovery descriptor before vblk_ctrl_init() frees it.
	 * DST always re-exports after ctrl init so this is not needed. */
	if (is_recovery && role == VBLK_HO_ROLE_SRC) {
		err = vblk_export_desc_store(&ho_ctx.export_desc,
					     &ho_ctx.export_desc_len,
					     s_resources.vblk_ctrl.recovery_export_desc,
					     s_resources.vblk_ctrl.recovery_export_desc_len);
		if (err != DOCA_SUCCESS)
			return err;
	}

	io_cfgs = calloc(num_io_ctx, sizeof(struct vblk_io_ctx_cfg));
	if (io_cfgs == NULL) {
		err = DOCA_ERROR_NO_MEMORY;
		goto cleanup;
	}

	/* Create IO PEs only (no TLP PE needed) */
	for (int i = 0; i < num_io_ctx; i++) {
		err = doca_pe_create(&s_resources.io_pe_ctxs[i].pe);
		if (err != DOCA_SUCCESS)
			goto cleanup_pe;
		s_resources.io_pe_ctxs[i].initialized = true;
	}

	/*
	 * Multi-device: passing ep to each io_ctx_cfg should be reconsidered, as it will be per-device.
	 */
	for (int i = 0; i < num_io_ctx; i++) {
		io_cfgs[i].ep = ep;
		io_cfgs[i].ipc_poll = emu_ipc_poll;
		io_cfgs[i].ipc_poll_arg = &ho_ctx;
	}

	err = vblk_io_ctx_oe_threads_create(&s_resources, config, io_cfgs, io_ctx_cores, &num_io_threads);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU: failed to create threads");
		goto force_quit;
	}

	err = app_wait_io_ready();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU: io_ready failed: %s", doca_error_get_name(err));
		goto force_quit;
	}

	if (role == VBLK_HO_ROLE_SRC && !is_recovery) {
		const void *exp_desc;
		size_t exp_len;

		/* Skipped on recovery since OE reconfiguration is not allowed. */
		err = doca_devemu_vblk_offload_engine_export(s_resources.vblk_ctrl.vq_engine, &exp_desc, &exp_len);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EMU SRC: export failed: %s", doca_error_get_name(err));
			goto force_quit;
		}
		vblk_ipc_send(ipc, VBLK_MSG_EXPORT_DESC, exp_desc, exp_len);
		err = vblk_export_desc_store(&ho_ctx.export_desc, &ho_ctx.export_desc_len, exp_desc, exp_len);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EMU SRC: failed to store export descriptor: %s", doca_error_get_name(err));
			goto force_quit;
		}
		DOCA_LOG_INFO("EMU SRC: exported OE state (%zu bytes)", exp_len);
	}

	/* Signal OE thread that pre-enable work is done; it can now call enable */
	vblk_app_pre_enable_done = true;

	err = app_wait_ctrl_enabled();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU: ctrl_enabled failed: %s", doca_error_get_name(err));
		goto force_quit;
	}
	DOCA_LOG_INFO("EMU: ctrl enabled, entering role-specific init");

	if (role == VBLK_HO_ROLE_SRC) {
		err = vblk_ipc_send(ipc, VBLK_MSG_STATE_REQUEST, NULL, 0);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EMU SRC: failed to send state request: %s", doca_error_get_name(err));
			goto force_quit;
		}
	} else if (role == VBLK_HO_ROLE_DST) {
		/* OE is started but not yet enabled (handover_dst skipped
		 * enable).  Export now — before begin_switchover eventually
		 * triggers enable in dst_handle_begin_ack(). */
		const void *dst_exp_desc;
		size_t dst_exp_len;

		err = doca_devemu_vblk_offload_engine_export(s_resources.vblk_ctrl.vq_engine,
							     &dst_exp_desc,
							     &dst_exp_len);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EMU DST: export failed: %s", doca_error_get_name(err));
			goto force_quit;
		}
		err = vblk_export_desc_store(&ho_ctx.export_desc, &ho_ctx.export_desc_len, dst_exp_desc, dst_exp_len);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EMU DST: failed to store export descriptor: %s", doca_error_get_name(err));
			goto force_quit;
		}
		DOCA_LOG_INFO("EMU DST: exported OE state (%zu bytes)", dst_exp_len);

		DOCA_LOG_INFO("EMU DST: starting switchover (vq_engine=%p)", (void *)s_resources.vblk_ctrl.vq_engine);
		err = vblk_ho_dst_begin_switchover(&ho_ctx);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("EMU DST: begin_switchover failed: %s", doca_error_get_name(err));
			goto force_quit;
		}
	}

	DOCA_LOG_INFO("EMU %s: all ready, running. Press Ctrl-C to exit", role == VBLK_HO_ROLE_DST ? "DST" : "SRC");
	goto wait_threads;

force_quit:
	DOCA_LOG_ERR("EMU: entering force_quit path (err=%s)", doca_error_get_name(err));
	force_quit = true;
	vblk_app_err = (int)DOCA_ERROR_INITIALIZATION;
	vblk_app_run_state = VBLK_APP_ERROR;
wait_threads:
	for (uint8_t i = 0; i < num_io_threads; i++)
		(void)pthread_join(s_resources.io_pe_ctxs[i].thread, NULL);

cleanup_pe:
	for (int i = 0; i < num_io_ctx; i++) {
		if (s_resources.io_pe_ctxs[i].initialized)
			(void)doca_pe_destroy(s_resources.io_pe_ctxs[i].pe);
	}

cleanup:
	vblk_export_desc_release(&ho_ctx.export_desc, &ho_ctx.export_desc_len);
	vblk_pci_dev_resources_cleanup(&s_resources);
	free(io_cfgs);
	return err;
}
