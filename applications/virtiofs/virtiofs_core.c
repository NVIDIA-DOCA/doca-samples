/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>
#include <pthread.h>

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_types.h>
#include <doca_devemu_vfs.h>
#include <doca_devemu_vfs_dev.h>
#include <doca_devemu_vfs_type.h>

#include <virtiofs_core.h>
#include <virtiofs_request.h>
#include <virtiofs_mpool.h>
#include <virtiofs_device.h>

DOCA_LOG_REGISTER(VIRTIOFS_CORE)

#define VIRTIOFS_THREAD_MAX_INFLIGHTS 128

uint32_t num_devs;
struct doca_dev *devs[64];

static int virtiofs_num_cores_get(uint32_t core_mask)
{
	int i, num_threads = 0;

	for (i = 0; i < sysconf(_SC_NPROCESSORS_ONLN); i++)
		if (core_mask & (1 << i))
			num_threads++;

	return num_threads;
}

static doca_error_t virtiofs_thread_ctx_init(struct virtiofs_resources *ctx,
					     int core_id,
					     bool admin_thread,
					     struct virtiofs_thread_ctx *tctx)
{
	struct doca_dev *devs[ctx->num_managers];
	struct virtiofs_manager *manager;
	doca_error_t err;
	int i = 0;

	struct virtiofs_thread_attr thread_attr = {
		.ctx = ctx,
		.core_id = core_id,
		.admin_thread = admin_thread,
		.max_inflights = VIRTIOFS_THREAD_MAX_INFLIGHTS,
	};

	err = virtiofs_thread_create(&thread_attr, &tctx->thread);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create thread for core id %d", i);
		return err;
	}

	SLIST_FOREACH(manager, &ctx->managers, entry)
	devs[i++] = manager->dev;

	struct virtiofs_mpool_attr mpool_attr[3] = {
		{.buf_size = 8192,
		 .num_bufs = VIRTIOFS_THREAD_MAX_INFLIGHTS * 4,
		 .devs = devs,
		 .num_devs = ctx->num_managers},
		{.buf_size = 65536,
		 .num_bufs = VIRTIOFS_THREAD_MAX_INFLIGHTS * 4,
		 .devs = devs,
		 .num_devs = ctx->num_managers},
		{.buf_size = 262144,
		 .num_bufs = VIRTIOFS_THREAD_MAX_INFLIGHTS * 4,
		 .devs = devs,
		 .num_devs = ctx->num_managers},
	};

	struct virtiofs_mpool_set_attr mpool_set_attr = {.mpools = mpool_attr, .num_pools = 3};

	tctx->mpool_set = virtiofs_mpool_set_create(&mpool_set_attr);
	if (tctx->mpool_set == NULL) {
		DOCA_LOG_ERR("Failed to create mpool set for core id %d", core_id);
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_get_possible_doca_devs(void)
{
	doca_error_t err;
	uint32_t nb_devs, i;
	uint8_t is_supported = false;
	struct doca_devinfo **dev_list;

	err = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA dev list, err: %s", doca_error_get_name(err));
		return err;
	}

	for (i = 0; i < nb_devs; i++) {
		err = doca_devemu_vfs_is_default_vfs_type_supported(dev_list[i], &is_supported);
		if (err != DOCA_SUCCESS || is_supported == false)
			continue;

		err = doca_dev_open(dev_list[i], &devs[num_devs]);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open doca dev, err: %s\n", doca_error_get_name(err));
			doca_devinfo_destroy_list(dev_list);
			return err;
		}

		num_devs += 1;
	}

	doca_devinfo_destroy_list(dev_list);
	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_devemu_vfs_init(void)
{
	struct doca_devemu_vfs_cfg *cfg;
	doca_error_t err;

	err = doca_devemu_vfs_cfg_create(&cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca devemu vfs cfg, err: %d", err);
		return err;
	}

	err = virtiofs_get_possible_doca_devs();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get possible virtiofs supported doca_dev list, err: %s",
			     doca_error_get_name(err));
		return err;
	}

	for (uint32_t i = 0; i < num_devs; i++) {
		err = doca_devemu_vfs_cfg_add_dev(cfg, devs[i]);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add dev to cfg at idx:%u, err: %s", i, doca_error_get_name(err));
		}
	}

	err = doca_devemu_vfs_cfg_set_vfs_fuse_req_user_data_size(cfg, sizeof(struct virtiofs_request));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cfg req user data size, err: %d", err);
		return err;
	}

	err = doca_devemu_vfs_init(cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca devemu vfs, err: %d", err);
		return err;
	}

	err = doca_devemu_vfs_cfg_destroy(cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca devemu vfs cfg, err: %d", err);
		return err;
	}

	return DOCA_SUCCESS;
}

struct virtiofs_resources *virtiofs_create(uint32_t core_mask)
{
	struct virtiofs_resources *ctx;
	int num_cores;
	doca_error_t err;

	err = virtiofs_devemu_vfs_init();
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca devemu vfs");
		return NULL;
	}

	num_cores = virtiofs_num_cores_get(core_mask);
	if (num_cores == 0) {
		DOCA_LOG_ERR("At least one core needs to be configured");
		return NULL;
	}

	ctx = calloc(1, sizeof(struct virtiofs_resources) + num_cores * sizeof(struct virtiofs_thread_ctx));
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to allocate doca context");
		return NULL;
	}

	err = virtiofs_managers_create(ctx);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca managers, err: %d", err);
		free(ctx);
		return NULL;
	}

	for (int i = 0; i < sysconf(_SC_NPROCESSORS_ONLN); i++) {
		if (core_mask & (1 << i)) {
			virtiofs_thread_ctx_init(ctx, i, ctx->num_threads == 0, &ctx->threads[ctx->num_threads]);
			ctx->num_threads++;
		}
	}

	return ctx;
}

doca_error_t virtiofs_start(struct virtiofs_resources *ctx)
{
	doca_error_t err;

	for (int idx = 0; idx < ctx->num_threads; idx++) {
		err = virtiofs_thread_start(ctx->threads[idx].thread);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start doca thread for thread %d", idx);
			return err;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_device_create_static(struct virtiofs_resources *ctx, char *nfs_server, char *nfs_export)
{
	doca_error_t err;
	struct virtiofs_function *func;

	err = virtiofs_nfs_fsdev_create(ctx, "nfs_fsdev", nfs_server, nfs_export);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create fsdev nfs");
		return err;
	}

	static struct virtiofs_device_config config = {
		.name = "vfs_controller0",
		.num_request_queues = 32,
		.queue_size = 256,
		.tag = "docavirtiofs",
	};

	err = virtiofs_function_get_available(ctx, &func);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get available function");
		return err;
	}

	err = virtiofs_device_create(ctx, &config, "mlx5_0", func->vuid, "nfs_fsdev");
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vfs device");
		return err;
	}

	err = virtiofs_device_start(ctx, "vfs_controller0", NULL, NULL);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start vfs device");
		return err;
	}

	return DOCA_SUCCESS;
}

static void virtiofs_stop_cb(void *cb_arg, doca_error_t status)
{
	struct virtiofs_device *dev = cb_arg;

	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop device %s, err: %s", dev->config.name, doca_error_get_name(status));
	}
}

static doca_error_t virtiofs_devices_teardown_progress(struct virtiofs_resources *ctx)
{
	struct virtiofs_device *dev, *next;
	doca_error_t err;

	if (SLIST_EMPTY(&ctx->devices)) {
		DOCA_LOG_INFO("Device teardown completed");
		return DOCA_SUCCESS;
	}

	SLIST_FOREACH_SAFE(dev, &ctx->devices, entry, next)
	{
		switch (dev->state) {
		case VIRTIOFS_DEVICE_STATE_STARTED:
			err = virtiofs_device_stop(ctx, dev->config.name, virtiofs_stop_cb, dev);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop device %s, err: %s",
					     dev->config.name,
					     doca_error_get_name(err));
				return err;
			}
			break;

		case VIRTIOFS_DEVICE_STATE_STOPPED:
			if (dev->io_ctxs_alive != 0)
				break;
			err = virtiofs_device_destroy(ctx, dev->config.name);
			if (err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy device %s, err: %s",
					     dev->config.name,
					     doca_error_get_name(err));
				return err;
			}
			break;

		case VIRTIOFS_DEVICE_STATE_STOPPING:
		case VIRTIOFS_DEVICE_STATE_STARTING:
			break;
		}
	}

	return DOCA_ERROR_IN_PROGRESS;
}

doca_error_t virtiofs_stop(struct virtiofs_resources *ctx)
{
	doca_error_t err;
	int i;

	while ((err = virtiofs_devices_teardown_progress(ctx)) == DOCA_ERROR_IN_PROGRESS)
		usleep(1000);

	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop devices, err: %s", doca_error_get_name(err));
		return err;
	}

	for (i = 0; i < ctx->num_threads; i++)
		virtiofs_thread_stop(ctx->threads[i].thread);

	return DOCA_SUCCESS;
}

void virtiofs_destroy(struct virtiofs_resources *ctx)
{
	doca_devemu_vfs_teardown();

	for (int i = 0; i < ctx->num_threads; i++) {
		virtiofs_mpool_set_destroy(ctx->threads[i].mpool_set);
		virtiofs_thread_destroy(ctx->threads[i].thread);
	}

	struct virtiofs_fsdev *f, *tmp;
	SLIST_FOREACH_SAFE(f, &ctx->fsdevs, entry, tmp)
	f->ops->destroy(f);

	virtiofs_managers_destroy(ctx);
	free(ctx);
}
