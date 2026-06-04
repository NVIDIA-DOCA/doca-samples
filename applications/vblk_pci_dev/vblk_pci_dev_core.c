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

#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_dev.h>

#include "vblk_pci.h"
#include "vblk_ctrl.h"
#include "vblk_io_ctx.h"
#include "vblk_tlp_ctx.h"
#include "vblk_pci_dev_core.h"
#include "vblk_mpool.h"
#include "utils.h"

DOCA_LOG_REGISTER(VBLK_PCI_DEV_CORE);

/*
 * State flow diagram (per endpoint, repeated for each EP in multi-EP mode):
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
 * |    +--> TLP context thread:                                                   |
 * |           vblk_pci_stop()                                                     |
 * |             - stop PCI event channel (doca_ctx_stop)                          |
 * |           signal_pci_stopped()                                                |
 * |                                                                               |
 * |  Offload engine thread (waits for above):                                     |
 * |    wait_all_io_exited()                                                       |
 * |    wait_pci_stopped()                                                         |
 * |    vblk_ctrl_cleanup()                                                        |
 * |      - reset IO contexts (vblk_ctrl_io_ctxs_reset)                            |
 * |      - disable offload engine (virtio_offload_engine_disable)                 |
 * |      - stop offload engine (virtio_offload_engine_stop)                       |
 * |      - destroy offload engine (vblk_offload_engine_destroy)                   |
 * |      - destroy PCI device (vblk_pci_virtio_dev_destroy)                       |
 * |                                                                               |
 * +-------------------------------------------------------------------------------+
 *
 * In static mode all endpoints are initialized/destroyed together.
 * In hotplug mode endpoints are created/destroyed dynamically.
 */
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
static _Atomic uint8_t vblk_app_pci_stopped;
static _Atomic doca_error_t vblk_app_err = DOCA_SUCCESS;

/* Application resources (accessed by callbacks via file-scope reference) */
static struct vblk_pci_dev_resources s_resources;

/* Static IO ops structure - must outlive threads that reference it */
static struct vblk_io_ctx_app_ops s_io_ops;

/**
 * @brief Close DOCA library resources
 *
 * @param[in] resources Application resources containing DOCA device handle
 */
static void doca_libs_close(struct vblk_pci_dev_resources *resources)
{
	if (resources && resources->doca_dev)
		doca_dev_close(resources->doca_dev);
}

/**
 * @brief Destroy all progress engine contexts and release resources
 *
 * Iterates over IO PE contexts and the TLP PE context, destroying
 * any that were previously initialized.
 *
 * @param[in] res Application resources containing PE contexts to destroy
 */
static void progress_contexts_destroy(struct vblk_pci_dev_resources *res)
{
	doca_error_t ret;
	int i;

	if (res->io_pe_ctxs == NULL)
		return;

	for (i = 0; i < res->num_io_ctx; i++) {
		struct vblk_pci_dev_pe_context *ctx = &(res->io_pe_ctxs[i]);

		if (!ctx->initialized)
			continue;

		ret = doca_pe_destroy(ctx->pe);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_WARN("failed to destroy progress engine");

		ctx->pe = NULL;
		ctx->initialized = false;
	}
	struct vblk_pci_dev_pe_context *tlp = &res->tlp_pe_ctx;
	if (tlp->initialized) {
		ret = doca_pe_destroy(tlp->pe);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_WARN("failed to destroy tlp progress engine");
		tlp->pe = NULL;
		tlp->initialized = false;
	}
}

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
 * @brief Signal that the PCI TLP thread has stopped
 */
static void app_signal_pci_stopped(void)
{
	vblk_app_pci_stopped = 1;
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
			*ctrl_out = s_resources.vblk_ctrls;
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
 * @brief Block until the PCI TLP thread has stopped
 */
static void app_wait_pci_stopped(void)
{
	while (vblk_app_pci_stopped == 0)
		sched_yield();
}

/**
 * @brief Initialize progress engines for all IO and TLP threads
 *
 * @param[in,out] res Application resources containing PE context arrays
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t progress_contexts_init(struct vblk_pci_dev_resources *res)
{
	doca_error_t err;
	int i;

	for (i = 0; i < res->num_io_ctx; i++) {
		struct vblk_pci_dev_pe_context *ctx = &(res->io_pe_ctxs[i]);

		err = doca_pe_create(&ctx->pe);
		if (err != DOCA_SUCCESS)
			goto reset_ctx;

		ctx->initialized = true;
	}
	struct vblk_pci_dev_pe_context *tlp = &res->tlp_pe_ctx;

	err = doca_pe_create(&tlp->pe);
	if (err != DOCA_SUCCESS)
		goto reset_ctx;
	tlp->initialized = true;

	return DOCA_SUCCESS;

reset_ctx:
	progress_contexts_destroy(res);
	return err;
}

static doca_error_t shared_mpools_create(struct vblk_pci_dev_resources *res)
{
	struct doca_dev *dev = res->doca_dev;

	res->shared_mpools = calloc(res->num_io_ctx, sizeof(struct vblk_mpool_set *));
	if (res->shared_mpools == NULL) {
		DOCA_LOG_ERR("Failed to allocate shared mpool array");
		return DOCA_ERROR_NO_MEMORY;
	}

	struct vblk_mpool_attr mpool_attr[] = {
		{.buf_size = 4096, .num_bufs = 1024, .devs = &dev, .num_devs = 1},
		{.buf_size = 8192, .num_bufs = 1024, .devs = &dev, .num_devs = 1},
		{.buf_size = 65536, .num_bufs = 512, .devs = &dev, .num_devs = 1},
		{.buf_size = 262144, .num_bufs = 512, .devs = &dev, .num_devs = 1},
		{.buf_size = 524288, .num_bufs = 256, .devs = &dev, .num_devs = 1},
	};
	struct vblk_mpool_set_attr mpool_set_attr = {.mpools = mpool_attr, .num_pools = 5};

	for (uint8_t i = 0; i < res->num_io_ctx; i++) {
		res->shared_mpools[i] = vblk_mpool_set_create(&mpool_set_attr);
		if (res->shared_mpools[i] == NULL) {
			DOCA_LOG_ERR("Failed to create shared mpool for IO thread %u", i);
			for (uint8_t j = 0; j < i; j++)
				vblk_mpool_set_destroy(res->shared_mpools[j]);
			free(res->shared_mpools);
			res->shared_mpools = NULL;
			return DOCA_ERROR_NO_MEMORY;
		}
		DOCA_LOG_INFO("Shared mpool %u created", i);
	}

	return DOCA_SUCCESS;
}

static void shared_mpools_destroy(struct vblk_pci_dev_resources *res)
{
	if (res->shared_mpools == NULL)
		return;

	for (uint8_t i = 0; i < res->num_io_ctx; i++) {
		if (res->shared_mpools[i] != NULL)
			vblk_mpool_set_destroy(res->shared_mpools[i]);
	}
	free(res->shared_mpools);
	res->shared_mpools = NULL;
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
	s_io_ops.wait_all_io_exited = app_wait_all_io_exited;
	s_io_ops.wait_pci_stopped = app_wait_pci_stopped;

	for (uint8_t ctx_id = 0; ctx_id < res->num_io_ctx; ctx_id++) {
		io_cfgs[ctx_id].pe = res->io_pe_ctxs[ctx_id].pe;
		io_cfgs[ctx_id].ctrls = res->vblk_ctrls;
		io_cfgs[ctx_id].hp_states = res->hp_states;
		io_cfgs[ctx_id].num_ep = config->num_ep;
		io_cfgs[ctx_id].ctx_id = ctx_id;
		io_cfgs[ctx_id].affinity_core = io_ctx_cores[ctx_id];
		io_cfgs[ctx_id].offload_engine_core_idx = config->offload_engine_core_idx;
		io_cfgs[ctx_id].num_io_ctx = res->num_io_ctx;
		io_cfgs[ctx_id].num_queues = config->num_queues;
		io_cfgs[ctx_id].seg_max = config->seg_max ? config->seg_max : 1;
		io_cfgs[ctx_id].indirect_enabled = config->indirect_enabled;
		io_cfgs[ctx_id].hotplug_mode = config->hotplug_mode;
		io_cfgs[ctx_id].stats_ios_period = config->stats_ios_period;
		io_cfgs[ctx_id].shared_mpool = res->shared_mpools[ctx_id];
		io_cfgs[ctx_id].ops = &s_io_ops;

		err = vblk_io_ctx_thread_create(&io_cfgs[ctx_id], &res->io_pe_ctxs[ctx_id].thread);
		if (err != DOCA_SUCCESS)
			return err;
		(*num_created)++;
	}
	return DOCA_SUCCESS;
}

/**
 * @brief Join all IO context and TLP threads
 *
 * @param[in] res Application resources containing thread handles
 * @param[in] num_io_threads Number of IO threads to join
 */
static void vblk_app_join_io_and_tlp_threads(struct vblk_pci_dev_resources *res, uint8_t num_io_threads)
{
	for (uint8_t i = 0; i < num_io_threads; i++)
		(void)pthread_join(res->io_pe_ctxs[i].thread, NULL);
	(void)pthread_join(res->tlp_pe_ctx.thread, NULL);
}

/**
 * @brief Start TLP and IO threads, wait for readiness, then block until shutdown
 *
 * Creates the TLP thread and IO context threads, waits for all to become
 * ready, enables the controller, then blocks until all threads complete.
 *
 * @param[in] res Application resources
 * @param[in] config Application configuration
 * @param[in] io_cfgs IO context thread configs array
 * @param[in] io_ctx_cores CPU core mapping for IO contexts
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t progress_contexts_start(struct vblk_pci_dev_resources *res,
					    struct vblk_pci_dev_config *config,
					    struct vblk_io_ctx_cfg *io_cfgs,
					    uint8_t *io_ctx_cores)
{
	struct vblk_tlp_ctx_cfg tlp_cfg = {0};
	uint8_t num_io_threads = 0;
	doca_error_t err;

	vblk_app_err = DOCA_SUCCESS;

	tlp_cfg.pe = res->tlp_pe_ctx.pe;
	tlp_cfg.hp_states = res->hp_states;
	tlp_cfg.hotplug_mode = config->hotplug_mode;
	tlp_cfg.num_ep = config->num_ep;
	tlp_cfg.signal_pci_stopped = app_signal_pci_stopped;
	err = vblk_pci_tlp_thread_create(&tlp_cfg, &res->tlp_pe_ctx.thread, config->tlp_core_idx);
	if (err != DOCA_SUCCESS)
		return err;

	err = vblk_io_ctx_oe_threads_create(res, config, io_cfgs, io_ctx_cores, &num_io_threads);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress context threads");
		goto force_quit_label;
	}

	/* Wait until all IO contexts are created on their managing threads */
	err = app_wait_io_ready();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize IO contexts (%s)", doca_error_get_name(err));
		goto force_quit_label;
	}

	err = app_wait_ctrl_enabled();
	if (err != DOCA_SUCCESS)
		goto force_quit_label;

	DOCA_LOG_INFO("All is ready (%u EPs, %u queues/EP), running progress loop. Press Ctrl-C to exit",
		      config->num_ep,
		      config->num_queues);
	DOCA_LOG_INFO("Runtime commands: cap <GB> (e.g. 'cap 1' to set capacity to 1 GB)");
	vblk_app_join_io_and_tlp_threads(res, num_io_threads);
	return DOCA_SUCCESS;

force_quit_label:
	force_quit = true;
	vblk_app_err = (int)DOCA_ERROR_INITIALIZATION;
	vblk_app_run_state = VBLK_APP_ERROR;
	vblk_app_join_io_and_tlp_threads(res, num_io_threads);
	return DOCA_ERROR_INITIALIZATION;
}

/**
 * @brief Validate application configuration
 *
 * Validates constraints between parameters and builds the io_ctx_cores
 * array from the CPU mask.
 *
 * @param[in] cfg Configuration to validate
 * @param[out] io_ctx_cores CPU core mapping array to populate
 * @param[out] num_io_ctx Number of IO contexts derived from mask
 * @return DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE on constraint violation
 */
static doca_error_t vblk_app_cfg_validate(struct vblk_pci_dev_config *cfg, uint8_t *io_ctx_cores, uint8_t *num_io_ctx)
{
	if (cfg->num_queues == 0 || cfg->num_queues > VBLK_PCI_DEV_MAX_QUEUES_PER_EP) {
		DOCA_LOG_ERR("num_queues must be in [1, %d]", VBLK_PCI_DEV_MAX_QUEUES_PER_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (cfg->num_ep == 0) {
		DOCA_LOG_ERR("num_ep must be at least 1");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (cfg->io_ctx_mask == 0) {
		DOCA_LOG_ERR("io_ctx_mask must have at least one bit set");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cfg->tlp_core_idx >= VBLK_PCI_DEV_MAX_CORES) {
		DOCA_LOG_ERR("tlp_core_idx (%u) out of range (max %u)", cfg->tlp_core_idx, VBLK_PCI_DEV_MAX_CORES - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if ((cfg->io_ctx_mask & (1UL << cfg->tlp_core_idx)) != 0) {
		DOCA_LOG_ERR("tlp_core_idx (%u) must not be in io_ctx_mask (0x%lx)",
			     cfg->tlp_core_idx,
			     cfg->io_ctx_mask);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if ((cfg->io_ctx_mask & (1UL << cfg->offload_engine_core_idx)) == 0) {
		DOCA_LOG_ERR("offload_engine_core_idx (%u) must be in io_ctx_mask (0x%lx)",
			     cfg->offload_engine_core_idx,
			     cfg->io_ctx_mask);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cfg->num_ep > MAX_NUM_EP) {
		DOCA_LOG_ERR("num_ep %u exceeds maximum %d", cfg->num_ep, MAX_NUM_EP);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Build io_ctx_cores array from mask */
	*num_io_ctx = 0;
	for (uint8_t c = 0; c < VBLK_PCI_DEV_MAX_CORES && *num_io_ctx < VBLK_PCI_DEV_MAX_IO_CORES; c++) {
		if (cfg->io_ctx_mask & (1UL << c))
			io_ctx_cores[(*num_io_ctx)++] = c;
	}

	if (cfg->num_queues < *num_io_ctx) {
		DOCA_LOG_ERR("num_queues (%u) must be >= num_io_ctx (%u)", cfg->num_queues, *num_io_ctx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Configuration validated: num_ep=%u queues=%u hotplug=%d seg_max=%u "
		      "io_ctx_mask=0x%lx (%u cores) tlp_core_idx=%u offload_engine_core_idx=%u",
		      cfg->num_ep,
		      cfg->num_queues,
		      cfg->hotplug_mode,
		      cfg->seg_max,
		      cfg->io_ctx_mask,
		      *num_io_ctx,
		      cfg->tlp_core_idx,
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
	vblk_app_pci_stopped = 0;
	vblk_app_err = DOCA_SUCCESS;
}

doca_error_t vblk_pci_dev_resources_init(uint8_t num_io_ctx, uint32_t num_ep, struct vblk_pci_dev_resources *resources)
{
	resources->io_pe_ctxs = calloc(num_io_ctx, sizeof(struct vblk_pci_dev_pe_context));
	if (resources->io_pe_ctxs == NULL) {
		DOCA_LOG_ERR("Failed to allocate PE contexts");
		return DOCA_ERROR_NO_MEMORY;
	}

	resources->vblk_ctrls = calloc(num_ep, sizeof(struct vblk_ctrl));
	if (resources->vblk_ctrls == NULL) {
		DOCA_LOG_ERR("Failed to allocate vblk controllers");
		goto free_pe_ctxs;
	}

	resources->hp_states = calloc(num_ep, sizeof(struct vblk_ep_hotplug_state));
	if (resources->hp_states == NULL) {
		DOCA_LOG_ERR("Failed to allocate hotplug states");
		goto free_ctrls;
	}

	resources->num_io_ctx = num_io_ctx;
	resources->num_ep = num_ep;
	return DOCA_SUCCESS;

free_ctrls:
	free(resources->vblk_ctrls);
	resources->vblk_ctrls = NULL;
free_pe_ctxs:
	free(resources->io_pe_ctxs);
	resources->io_pe_ctxs = NULL;
	return DOCA_ERROR_NO_MEMORY;
}

void vblk_pci_dev_resources_cleanup(struct vblk_pci_dev_resources *resources)
{
	if (resources == NULL)
		return;
	free(resources->io_pe_ctxs);
	resources->io_pe_ctxs = NULL;
	free(resources->vblk_ctrls);
	resources->vblk_ctrls = NULL;
	free(resources->hp_states);
	resources->hp_states = NULL;
	resources->num_io_ctx = 0;
	resources->num_ep = 0;
}

doca_error_t vblk_pci_dev_run(struct vblk_pci_dev_config *config)
{
	uint8_t io_ctx_cores[VBLK_PCI_DEV_MAX_IO_CORES] = {0};
	struct vblk_io_ctx_cfg *io_cfgs = NULL;
	uint8_t num_io_ctx = 0;
	doca_error_t err;

	DOCA_LOG_INFO("Initializing VBlk device (%u EPs, %s mode)...",
		      config->num_ep,
		      config->hotplug_mode ? "hotplug" : "static");

	/* Reset state for fresh run */
	memset(&s_resources, 0, sizeof(s_resources));
	vblk_app_reset_atomic_state();

	err = vblk_app_cfg_validate(config, io_ctx_cores, &num_io_ctx);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Invalid configuration");
		return err;
	}

	/* Initialize resources after num_io_ctx is determined by config validation */
	err = vblk_pci_dev_resources_init(num_io_ctx, config->num_ep, &s_resources);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize resources");
		goto cleanup;
	}

	/* Allocate IO context thread configs */
	io_cfgs = calloc(num_io_ctx, sizeof(struct vblk_io_ctx_cfg));
	if (io_cfgs == NULL) {
		DOCA_LOG_ERR("Failed to allocate IO context configs");
		err = DOCA_ERROR_NO_MEMORY;
		goto cleanup;
	}

	err = doca_devemu_vblk_set_datapath_on_dpa(config->datapath_on_dpa);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set datapath provider, err: %d", err);
		goto cleanup;
	}

	err = vblk_init(config->device_name, config->indirect_enabled, &s_resources.doca_dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize VBLK device: %s", doca_error_get_descr(err));
		goto cleanup;
	}

	/* Per-thread progress contexts */
	err = progress_contexts_init(&s_resources);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize progress contexts");
		goto cleanup_vblk;
	}

	err = shared_mpools_create(&s_resources);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create shared mpools");
		goto cleanup_pe;
	}

	err = vblk_pci_init(s_resources.doca_dev, config->num_ep, config->hotplug_mode);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize PCI: %s", doca_error_get_descr(err));
		goto cleanup_mpool;
	}

	s_resources.tlp_ctx = vblk_pci_get_tlp_ctx();
	s_resources.tlp_ctx->vblk_ctrls = s_resources.vblk_ctrls;
	s_resources.tlp_ctx->num_queues = config->num_queues;

	DOCA_LOG_INFO("VBlk device initialized successfully (%u EPs)", config->num_ep);

	/* Start contexts and run progress loop (blocks until force_quit) */
	err = progress_contexts_start(&s_resources, config, io_cfgs, io_ctx_cores);
	if (err != DOCA_SUCCESS)
		DOCA_LOG_ERR("Progress context error: %s", doca_error_get_descr(err));

	/* Cleanup resources in reverse order */
	vblk_pci_reset();

cleanup_vblk:
	vblk_reset();

cleanup_mpool:
	shared_mpools_destroy(&s_resources);

cleanup_pe:
	progress_contexts_destroy(&s_resources);
	doca_libs_close(&s_resources);

cleanup:
	vblk_pci_dev_resources_cleanup(&s_resources);
	free(io_cfgs);

	return err;
}
