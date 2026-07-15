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

#include <pthread.h>
#include <doca_types.h>
#include <doca_dma.h>
#include <doca_log.h>
#include <doca_devemu_vfs.h>
#include <doca_devemu_vfs_dev.h>
#include <doca_devemu_vfs_io.h>

#include <virtiofs_core.h>
#include <virtiofs_request.h>
#include <virtiofs_device.h>
#include <nfs_fsdev.h>
#include "priv_nfs_fsdev.h"

DOCA_LOG_REGISTER(VIRTIOFS_DEVICE)

#define MAX_REQS_PER_DIO 1024
#define MAX_DOCA_BUF_PER_DATA_DMA 1

/*
 * Max number of DMA requests per Virtio FS request.
 * Add +1 for fuse_out_header and +1 for fuse_cmd_specific_header.
 */
#define MAX_PARALLEL_DMAS_PER_REQ (MAX_DOCA_BUF_PER_DATA_DMA + 2)

static struct virtiofs_device *virtiofs_device_get_by_name(struct virtiofs_resources *ctx, char *name)
{
	struct virtiofs_device *device;

	SLIST_FOREACH(device, &ctx->devices, entry)
	if (strcmp(device->config.name, name) == 0)
		return device;

	return NULL;
}

static doca_error_t virtiofs_devemu_vfs_dev_destroy(struct virtiofs_device *dev)
{
	doca_error_t err;

	err = doca_devemu_vfs_dev_destroy(dev->vfs_dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy vfs_dev, err: %s", doca_error_get_name(err));
		return err;
	}

	dev->vfs_dev = NULL;

	err = doca_dev_rep_close(dev->dev_rep);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close dev_rep, err: %s", doca_error_get_name(err));
		return err;
	}

	dev->dev_rep = NULL;

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_device_destroy(struct virtiofs_resources *ctx, char *name)
{
	struct virtiofs_device *dev;
	doca_error_t err;

	dev = virtiofs_device_get_by_name(ctx, name);
	if (dev == NULL)
		return DOCA_ERROR_NOT_FOUND;

	if (dev->state != VIRTIOFS_DEVICE_STATE_STOPPED)
		return DOCA_ERROR_BAD_STATE;

	err = virtiofs_devemu_vfs_dev_destroy(dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy vfs_dev, err: %s", doca_error_get_name(err));
		return err;
	}

	SLIST_REMOVE(&ctx->devices, dev, virtiofs_device, entry);
	ctx->num_devices--;

	free(dev);

	return DOCA_SUCCESS;
}

static struct doca_devinfo_rep *virtiofs_get_devinfo_rep_from_vuid(struct doca_devinfo_rep **list,
								   uint32_t num_devices,
								   struct doca_dev *dev,
								   char *vuid)
{
	(void)dev;
	uint32_t idx;
	doca_error_t err;
	char buf[DOCA_DEVINFO_REP_VUID_SIZE] = {0};

	for (idx = 0; idx < num_devices; idx++) {
		err = doca_devinfo_rep_get_vuid(list[idx], buf, DOCA_DEVINFO_REP_VUID_SIZE);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get vuid for devinfo_rep, ret: %s\n", doca_error_get_name(err));
			continue;
		}

		if (!strncmp(vuid, buf, DOCA_DEVINFO_REP_VUID_SIZE)) {
			return list[idx];
		}
	}

	return NULL;
}

static struct doca_dev_rep *virtiofs_get_dev_rep_from_vuid(struct virtiofs_device *dev)
{
	struct doca_dev_rep *dev_rep;
	struct doca_devinfo_rep *devinfo_rep;
	struct doca_devinfo_rep **devinfo_rep_list;
	uint32_t num_devices = 0;
	doca_error_t err;

	err = doca_devinfo_rep_create_list(dev->manager->dev,
					   DOCA_DEVINFO_REP_FILTER_EMULATED,
					   &devinfo_rep_list,
					   &num_devices);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get representors of device, ret: %s\n", doca_error_get_name(err));
		return NULL;
	}

	if (num_devices == 0) {
		DOCA_LOG_ERR("No representors that support emulation were found\n");
		goto destroy_devinfo_rep_list;
	}

	devinfo_rep = virtiofs_get_devinfo_rep_from_vuid(devinfo_rep_list, num_devices, dev->manager->dev, dev->vuid);
	if (!devinfo_rep) {
		DOCA_LOG_ERR("Failed to get devinfo_rep for vuid: %s, err: %s\n", dev->vuid, doca_error_get_name(err));
		goto destroy_devinfo_rep_list;
	}

	/* Open a representor */
	err = doca_dev_rep_open(devinfo_rep, &dev_rep);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device representor, err: %s\n", doca_error_get_name(err));
		goto destroy_devinfo_rep_list;
	}

	doca_devinfo_rep_destroy_list(devinfo_rep_list);

	return dev_rep;

destroy_devinfo_rep_list:
	doca_devinfo_rep_destroy_list(devinfo_rep_list);
	return NULL;
}

static doca_error_t virtiofs_devemu_vfs_dev_create(struct virtiofs_device *dev, struct doca_pe *pe)
{
	doca_error_t err;

	dev->dev_rep = virtiofs_get_dev_rep_from_vuid(dev);
	if (!dev->dev_rep) {
		err = DOCA_ERROR_NOT_FOUND;
		goto out;
	}

	err = doca_devemu_vfs_dev_create(dev->manager->vfs_type, dev->dev_rep, pe, &dev->vfs_dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vfs_dev, err: %s", doca_error_get_name(err));
		dev->vfs_dev = NULL;
		goto out_rep_free;
	}

	return DOCA_SUCCESS;

out_rep_free:
	doca_dev_rep_close(dev->dev_rep);
	dev->dev_rep = NULL;
out:
	return err;
}

static doca_error_t virtiofs_device_attributes_modify(struct virtiofs_device *dev)
{
	doca_error_t err = DOCA_SUCCESS;

	err = doca_devemu_vfs_dev_set_tag(dev->vfs_dev, dev->config.tag);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify tag for: [%s] err: [%s]\n", dev->config.name, doca_error_get_name(err));
		return err;
	}

	if (dev->config.num_request_queues) {
		err = doca_devemu_vfs_dev_set_num_request_queues(dev->vfs_dev, dev->config.num_request_queues);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify num_request_queues for : [%s], err: [%s]\n",
				     dev->config.name,
				     doca_error_get_name(err));
			return err;
		}

		/*
		 * num_requests represents the request_queues and one hipro queue.
		 * Hence, add 1 to the "num_request_queues" value here.
		 */
		err = doca_devemu_virtio_dev_set_num_queues(doca_devemu_vfs_dev_as_virtio_dev(dev->vfs_dev),
							    dev->config.num_request_queues + 1);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify num_queues for: [%s], err: [%s]\n",
				     dev->config.name,
				     doca_error_get_name(err));
			return err;
		}
	}

	if (dev->config.queue_size) {
		err = doca_devemu_virtio_dev_set_queue_size(doca_devemu_vfs_dev_as_virtio_dev(dev->vfs_dev),
							    dev->config.queue_size);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to modify queue_size for: [%s], err: [%s]\n",
				     dev->config.name,
				     doca_error_get_name(err));
			return err;
		}
	}

	return DOCA_SUCCESS;
}

static void virtiofs_device_reset_start_done(void *arg, doca_error_t status)
{
	struct virtiofs_device *dev = arg;

	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("%s: Device start failed", dev->config.name);
		return;
	}

	DOCA_LOG_INFO("%s: Device was reset successfully", dev->config.name);
}

static void virtiofs_device_reset_stop_done(void *arg, doca_error_t status)
{
	struct virtiofs_device *dev = arg;

	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("%s: Device stop failed", dev->config.name);
		return;
	}

	virtiofs_device_start(dev->ctx, dev->config.name, virtiofs_device_reset_start_done, dev);
}

static void virtiofs_reset_event_cb(struct doca_devemu_virtio_dev *virtio_dev, union doca_data event_user_data)
{
	(void)virtio_dev;
	struct virtiofs_device *dev = event_user_data.ptr;

	DOCA_LOG_INFO("%s", dev->config.name);

	virtiofs_device_stop(dev->ctx, dev->config.name, virtiofs_device_reset_stop_done, dev);
}

static void virtiofs_flr_event_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data event_user_data)
{
	(void)pci_dev;
	struct virtiofs_device *dev = event_user_data.ptr;

	DOCA_LOG_INFO("%s", dev->config.name);

	virtiofs_device_stop(dev->ctx, dev->config.name, virtiofs_device_reset_stop_done, dev);
}

static doca_error_t virtiofs_register_cb_for_doca_ctx_state_change(struct doca_ctx *ctx,
								   doca_ctx_state_changed_callback_t cb,
								   void *cb_arg)
{
	doca_error_t err;
	union doca_data ctx_user_data;

	ctx_user_data.ptr = cb_arg;
	err = doca_ctx_set_user_data(ctx, ctx_user_data);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ctx's user data, err: %s\n", doca_error_get_name(err));
		return err;
	}

	err = doca_ctx_set_state_changed_cb(ctx, cb);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register state change cb, err: %s\n", doca_error_get_name(err));
		return err;
	}

	return DOCA_SUCCESS;
}

static void virtiofs_dev_state_change_cb(const union doca_data user_data,
					 struct doca_ctx *ctx,
					 enum doca_ctx_states prev_state,
					 enum doca_ctx_states next_state)
{
	(void)ctx;
	struct virtiofs_device *dev = user_data.ptr;

	DOCA_LOG_INFO("%s, state change %d->%d", dev->config.name, prev_state, next_state);

	if (prev_state == DOCA_CTX_STATE_STARTING) {
		if (next_state == DOCA_CTX_STATE_RUNNING) {
			dev->state = VIRTIOFS_DEVICE_STATE_STARTED;
			if (dev->start_cb)
				dev->start_cb(dev->start_cb_arg, DOCA_SUCCESS);
		} else {
			dev->state = VIRTIOFS_DEVICE_STATE_STOPPED;
			if (dev->start_cb)
				dev->start_cb(dev->start_cb_arg, DOCA_ERROR_UNKNOWN);
		}
		dev->start_cb = NULL;
		dev->start_cb_arg = NULL;
	} else if (prev_state == DOCA_CTX_STATE_STOPPING && next_state == DOCA_CTX_STATE_IDLE) {
		dev->state = VIRTIOFS_DEVICE_STATE_STOPPED;
		if (dev->stop_cb) {
			dev->stop_cb(dev->stop_cb_arg, DOCA_SUCCESS);
			dev->stop_cb = NULL;
			dev->stop_cb_arg = NULL;
		}
	}
}

static doca_error_t virtiofs_device_vfs_dev_create(struct virtiofs_thread *thread, struct virtiofs_device *dev)
{
	union doca_data cb_data;
	doca_error_t err;

	err = virtiofs_devemu_vfs_dev_create(dev, thread->admin_pe);
	if (err != DOCA_SUCCESS)
		goto out;

	err = doca_devemu_virtio_dev_set_num_required_running_virtio_io_ctxs(
		doca_devemu_vfs_dev_as_virtio_dev(dev->vfs_dev),
		dev->num_threads);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set num virtio_io ctxs in virtio_dev, err: %s\n", doca_error_get_name(err));
		goto vfs_destroy;
	}

	err = virtiofs_device_attributes_modify(dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify device attributes, err: %s\n", doca_error_get_name(err));
		goto vfs_destroy;
	}

	cb_data.ptr = dev;

	err = doca_devemu_virtio_dev_event_reset_register(doca_devemu_vfs_dev_as_virtio_dev(dev->vfs_dev),
							  virtiofs_reset_event_cb,
							  cb_data);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register reset event handler, err: %s\n", doca_error_get_name(err));
		goto vfs_destroy;
	}

	err = doca_devemu_pci_dev_event_flr_register(doca_devemu_vfs_dev_as_pci_dev(dev->vfs_dev),
						     virtiofs_flr_event_cb,
						     cb_data);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register reset event handler, err: %s\n", doca_error_get_name(err));
		goto vfs_destroy;
	}

	err = virtiofs_register_cb_for_doca_ctx_state_change(doca_devemu_vfs_dev_as_ctx(dev->vfs_dev),
							     virtiofs_dev_state_change_cb,
							     dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register state change cb, err: %s\n", doca_error_get_name(err));
		goto vfs_destroy;
	}

	err = doca_devemu_vfs_dev_set_fuse_client_arch(dev->vfs_dev, DOCA_DEVEMU_VFS_FUSE_CLIENT_ARCH_X86_64);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set fuse_client_arch, err: %s\n", doca_error_get_name(err));
		goto vfs_destroy;
	}

	return DOCA_SUCCESS;

vfs_destroy:
	virtiofs_devemu_vfs_dev_destroy(dev);
out:
	return err;
}

doca_error_t virtiofs_device_create(struct virtiofs_resources *ctx,
				    struct virtiofs_device_config *config,
				    char *manager,
				    char *vuid,
				    char *fsdev)
{
	struct virtiofs_device *dev;
	doca_error_t err;

	if (virtiofs_device_get_by_name(ctx, config->name)) {
		DOCA_LOG_ERR("Device %s already exists!", config->name);
		return DOCA_ERROR_ALREADY_EXIST;
	}

	dev = calloc(1, sizeof(struct virtiofs_device) + ctx->num_threads * sizeof(struct virtiofs_device_io_ctx *));
	if (dev == NULL) {
		DOCA_LOG_ERR("Failed to allocate dev");
		return DOCA_ERROR_NO_MEMORY;
	}

	strncpy(dev->vuid, vuid, DOCA_DEVINFO_REP_VUID_SIZE);
	dev->vuid[DOCA_DEVINFO_REP_VUID_SIZE - 1] = '\0';
	dev->ctx = ctx;
	dev->config = *config;
	dev->num_threads = ctx->num_threads;
	dev->state = VIRTIOFS_DEVICE_STATE_STOPPED;

	dev->manager = virtiofs_manager_get_by_name(ctx, manager);
	if (dev->manager == NULL) {
		DOCA_LOG_ERR("Failed to find manager %s", manager);
		free(dev);
		return DOCA_ERROR_NOT_FOUND;
	}

	dev->fsdev = virtiofs_fsdev_get_by_name(ctx, fsdev);
	if (dev->fsdev == NULL) {
		DOCA_LOG_ERR("Failed to find fsdev %s", fsdev);
		free(dev);
		return DOCA_ERROR_NOT_FOUND;
	}

	err = virtiofs_thread_exec(ctx->threads[0].thread,
				   (virtiofs_thread_exec_fn_t)virtiofs_device_vfs_dev_create,
				   dev,
				   true);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("%s: Failed to create vfs dev", config->name);
		free(dev);
		return err;
	}

	SLIST_INSERT_HEAD(&ctx->devices, dev, entry);
	ctx->num_devices++;

	DOCA_LOG_INFO("Device %s created successfully, num_request_queues: %d, tag: %s",
		      config->name,
		      config->num_request_queues,
		      config->tag);

	return DOCA_SUCCESS;
}

static void virtiofs_dma_resources_destroy(struct virtiofs_device_io_ctx *dio)
{
	doca_error_t err;

	dio->reqs_avail = 0;

	/* We expect no inflights here */
	err = doca_ctx_stop(doca_dma_as_ctx(dio->dma_ctx));
	if (err != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop doca_ctx for vfs_io's dma_ctx, err: %s\n", doca_error_get_name(err));

	err = doca_dma_destroy(dio->dma_ctx);
	if (err != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA DMA context, err: [%s]\n", doca_error_get_name(err));

	dio->dma_ctx = NULL;
}

static doca_error_t virtiofs_dma_resources_create(struct virtiofs_device_io_ctx *dio, struct doca_pe *pe)
{
	doca_error_t err;
	uint16_t queue_size, num_queues;
	uint32_t max_requests;
	union doca_data ctx_user_data = {0};

	err = doca_dma_create(dio->dev->manager->dev, &dio->dma_ctx);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA DMA ctx, err: %s\n", doca_error_get_name(err));
		return err;
	}

	err = doca_pe_connect_ctx(pe, doca_dma_as_ctx(dio->dma_ctx));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DOCA progress engine to DOCA DMA: %s\n", doca_error_get_name(err));
		goto destroy_dma;
	}

	ctx_user_data.ptr = dio;
	err = doca_ctx_set_user_data(doca_dma_as_ctx(dio->dma_ctx), ctx_user_data);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_ctx user data, err: [%s]\n", doca_error_get_name(err));
		goto destroy_dma;
	}

	err = doca_dma_set_ordered_completions(dio->dma_ctx, 1);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered completions, err: [%s]", doca_error_get_name(err));
		goto destroy_dma;
	}

	err = doca_devemu_virtio_dev_get_queue_size(doca_devemu_vfs_dev_as_virtio_dev(dio->dev->vfs_dev), &queue_size);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get queue size: %s\n", doca_error_get_name(err));
		goto destroy_dma;
	}

	err = doca_devemu_virtio_dev_get_num_queues(doca_devemu_vfs_dev_as_virtio_dev(dio->dev->vfs_dev), &num_queues);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get num queues: %s\n", doca_error_get_name(err));
		goto destroy_dma;
	}

	max_requests = queue_size * num_queues;
	err = doca_dma_task_memcpy_set_conf(dio->dma_ctx,
					    virtiofs_request_dma_done,
					    virtiofs_request_dma_err,
					    max_requests * MAX_PARALLEL_DMAS_PER_REQ);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set configurations for DMA memcpy task: %s\n", doca_error_get_name(err));
		goto destroy_dma;
	}

	/* start vfs_io's DMA doca_ctx */
	err = doca_ctx_start(doca_dma_as_ctx(dio->dma_ctx));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca_ctx for vfs_io's dma_ctx, err: %s\n", doca_error_get_name(err));
		goto destroy_dma;
	}

	dio->reqs_avail = max_requests;

	return DOCA_SUCCESS;

destroy_dma:
	doca_dma_destroy(dio->dma_ctx);
	return err;
}

static void virtiofs_io_state_change_cb(const union doca_data user_data,
					struct doca_ctx *doca_ctx,
					enum doca_ctx_states prev_state,
					enum doca_ctx_states next_state)
{
	(void)doca_ctx;
	struct virtiofs_device_io_ctx *dio = user_data.ptr;

	DOCA_LOG_INFO("io ctx %p, state change: %d->%d", (void *)dio, prev_state, next_state);

	if (prev_state == DOCA_CTX_STATE_STOPPING && next_state == DOCA_CTX_STATE_IDLE) {
		struct virtiofs_device *dev = dio->dev;
		if (dio->fsdev_tctx)
			dio->fsdev_tctx->fsdev->ops->thread_ctx_put(dio->fsdev_tctx);
		virtiofs_dma_resources_destroy(dio);
		doca_devemu_vfs_io_destroy(dio->vfs_io);
		dio->vfs_io = NULL;
		free(dio);
		if (dev && dev->io_ctxs_alive > 0)
			dev->io_ctxs_alive--;
	}
}

static doca_error_t virtiofs_register_fuse_cbs(struct doca_devemu_vfs_io *vfs_io)
{
	doca_error_t err;

	err = doca_devemu_vfs_io_event_vfs_fuse_init_req_handler_register(vfs_io, virtiofs_request_init_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse init handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_destroy_req_handler_register(vfs_io, virtiofs_request_destroy_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse destroy handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_lookup_req_handler_register(vfs_io, virtiofs_request_lookup_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse lookup handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_create_req_handler_register(vfs_io, virtiofs_request_create_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse create handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_open_req_handler_register(vfs_io, virtiofs_request_open_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse open handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_read_req_handler_register(vfs_io, virtiofs_request_read_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse read handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_write_req_handler_register(vfs_io, virtiofs_request_write_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse write handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_mknod_req_handler_register(vfs_io, virtiofs_request_mknod_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse mknod handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_readdir_req_handler_register(vfs_io, virtiofs_request_readdir_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse readdir handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_readdirplus_req_handler_register(vfs_io,
										 virtiofs_request_readdir_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse readdirplus handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_getattr_req_handler_register(vfs_io, virtiofs_request_getattr_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse getattr handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_releasedir_req_handler_register(vfs_io,
										virtiofs_request_releasedir_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse releasedir handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_opendir_req_handler_register(vfs_io, virtiofs_request_opendir_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse opendir handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_setattr_req_handler_register(vfs_io, virtiofs_request_setattr_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse setattr handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_release_req_handler_register(vfs_io, virtiofs_request_release_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse release handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_flush_req_handler_register(vfs_io, virtiofs_request_flush_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse flush handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_forget_req_handler_register(vfs_io, virtiofs_request_forget_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse forget handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_statfs_req_handler_register(vfs_io, virtiofs_request_statfs_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse statfs handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_unlink_req_handler_register(vfs_io, virtiofs_request_unlink_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse unlink handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_getxattr_req_handler_register(vfs_io,
									      virtiofs_request_getxattr_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse getxattr handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_mkdir_req_handler_register(vfs_io, virtiofs_request_mkdir_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse mkdir handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_rmdir_req_handler_register(vfs_io, virtiofs_request_rmdir_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse rmdir handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_rename_req_handler_register(vfs_io, virtiofs_request_rename_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse rename handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_rename2_req_handler_register(vfs_io, virtiofs_request_rename2_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse rename2 handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_readlink_req_handler_register(vfs_io,
									      virtiofs_request_readlink_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse readlink handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_symlink_req_handler_register(vfs_io, virtiofs_request_symlink_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse symlink handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_link_req_handler_register(vfs_io, virtiofs_request_link_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse link handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_fsync_req_handler_register(vfs_io, virtiofs_request_fsync_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse fsync handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_fallocate_req_handler_register(vfs_io,
									       virtiofs_request_fallocate_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse fallocate handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_setxattr_req_handler_register(vfs_io,
									      virtiofs_request_setxattr_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse setxattr handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_listxattr_req_handler_register(vfs_io,
									       virtiofs_request_listxattr_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse listxattr handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_removexattr_req_handler_register(vfs_io,
										 virtiofs_request_removexattr_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse remove handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_syncfs_req_handler_register(vfs_io, virtiofs_request_syncfs_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse syncfs handler, err: %s", doca_error_get_name(err));
		return err;
	}

	err = doca_devemu_vfs_io_event_vfs_fuse_ioctl_req_handler_register(vfs_io, virtiofs_request_ioctl_handler);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse ioctl handler, err: %s", doca_error_get_name(err));
		return err;
	}

	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_device_io_ctx_start(struct virtiofs_thread *thread, struct virtiofs_device_io_ctx *dio)
{
	doca_error_t err;

	err = doca_devemu_vfs_io_create(dio->dev->vfs_dev, thread->io_pe, &dio->vfs_io);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vfs_io, err: %s\n", doca_error_get_name(err));
		goto out_err;
	}

	err = virtiofs_register_fuse_cbs(dio->vfs_io);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register request handler, err: %s\n", doca_error_get_name(err));
		goto out_destroy;
	}

	err = virtiofs_dma_resources_create(dio, thread->dma_pe);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register fuse command handler, err: %s", doca_error_get_name(err));
		goto out_destroy;
	}

	/* Register cb to signal vfs_io transition to "RUNNING" state */
	err = virtiofs_register_cb_for_doca_ctx_state_change(doca_devemu_vfs_io_as_ctx(dio->vfs_io),
							     virtiofs_io_state_change_cb,
							     dio);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register state change cb, err: %s\n", doca_error_get_name(err));
		goto out_dma_destroy;
	}

	/* start vfs_io's doca_ctx */
	err = doca_ctx_start(doca_devemu_vfs_io_as_ctx(dio->vfs_io));
	if (err != DOCA_ERROR_IN_PROGRESS && err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca_ctx for vfs_io, err: %s\n", doca_error_get_name(err));
		goto out_dma_destroy;
	}

	return 0;

out_dma_destroy:
	virtiofs_dma_resources_destroy(dio);
out_destroy:
	doca_devemu_vfs_io_destroy(dio->vfs_io);
out_err:
	return err;
}

static doca_error_t virtiofs_device_io_ctxs_start(struct virtiofs_resources *ctx, struct virtiofs_device *dev)
{
	doca_error_t err;
	int i;

	for (i = 0; i < ctx->num_threads; i++) {
		dev->io_ctxs[i] = calloc(1, sizeof(*dev->io_ctxs[i]));
		if (dev->io_ctxs[i] == NULL) {
			DOCA_LOG_ERR("%s: Failed to create io ctx for thread %d", dev->config.name, i);
			return DOCA_ERROR_NO_MEMORY;
		}

		dev->io_ctxs[i]->dev = dev;
		dev->io_ctxs[i]->mpool_set = ctx->threads[i].mpool_set;
		dev->io_ctxs[i]->thread = ctx->threads[i].thread;
		TAILQ_INIT(&dev->io_ctxs[i]->order);
		TAILQ_INIT(&dev->io_ctxs[i]->pending);

		err = virtiofs_thread_exec(ctx->threads[i].thread,
					   (virtiofs_thread_exec_fn_t)virtiofs_device_io_ctx_start,
					   dev->io_ctxs[i],
					   false);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start io ctx for thread %d", i);
			return err;
		}
		dev->io_ctxs_alive++;
	}

	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_device_vfs_dev_start(struct virtiofs_thread *thread, struct virtiofs_device *dev)
{
	(void)thread;
	return doca_ctx_start(doca_devemu_vfs_dev_as_ctx(dev->vfs_dev));
}

doca_error_t virtiofs_device_start(struct virtiofs_resources *ctx, char *dev_name, virtiofs_cb_t cb, void *cb_arg)
{
	struct virtiofs_device *dev;
	doca_error_t err;

	dev = virtiofs_device_get_by_name(ctx, dev_name);
	if (dev == NULL) {
		DOCA_LOG_ERR("Couldn't find virtio fs doca device %s", dev_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	if (dev->state != VIRTIOFS_DEVICE_STATE_STOPPED) {
		DOCA_LOG_ERR("Device %s is not in stopped state", dev_name);
		return DOCA_ERROR_BAD_STATE;
	}

	dev->state = VIRTIOFS_DEVICE_STATE_STARTING;
	dev->start_cb = cb;
	dev->start_cb_arg = cb_arg;

	err = virtiofs_thread_exec(ctx->threads[0].thread,
				   (virtiofs_thread_exec_fn_t)virtiofs_device_vfs_dev_start,
				   dev,
				   true);
	if (err != DOCA_ERROR_IN_PROGRESS && err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start device vfs dev, err: %s", doca_error_get_name(err));
		return err;
	}

	err = virtiofs_device_io_ctxs_start(ctx, dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start device io ctxs, err: %s", doca_error_get_name(err));
		return err;
	}

	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_device_io_ctx_stop(struct virtiofs_thread *thread, struct virtiofs_device_io_ctx *dio)
{
	(void)thread;
	return doca_ctx_stop(doca_devemu_vfs_io_as_ctx(dio->vfs_io));
}

static doca_error_t virtiofs_device_vfs_dev_stop(struct virtiofs_thread *thread, struct virtiofs_device *dev)
{
	(void)thread;
	return doca_ctx_stop(doca_devemu_vfs_dev_as_ctx(dev->vfs_dev));
}

doca_error_t virtiofs_device_stop(struct virtiofs_resources *ctx, char *dev_name, virtiofs_cb_t cb, void *cb_arg)
{
	struct virtiofs_device *dev;
	doca_error_t err;
	int i;

	dev = virtiofs_device_get_by_name(ctx, dev_name);
	if (dev == NULL) {
		DOCA_LOG_ERR("Couldn't find virtio fs doca device %s", dev_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	if (dev->state != VIRTIOFS_DEVICE_STATE_STARTED) {
		DOCA_LOG_ERR("Device %s is not in started state", dev_name);
		return DOCA_ERROR_BAD_STATE;
	}

	dev->state = VIRTIOFS_DEVICE_STATE_STOPPING;
	dev->stop_cb = cb;
	dev->stop_cb_arg = cb_arg;

	err = virtiofs_thread_exec(ctx->threads[0].thread,
				   (virtiofs_thread_exec_fn_t)(virtiofs_device_vfs_dev_stop),
				   dev,
				   true);
	if (err != DOCA_ERROR_IN_PROGRESS && err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("%s: Couldn't stop vfs dev, err: %s", dev_name, doca_error_get_name(err));
		return err;
	}

	for (i = 0; i < dev->num_threads; i++) {
		err = virtiofs_thread_exec(ctx->threads[i].thread,
					   (virtiofs_thread_exec_fn_t)(virtiofs_device_io_ctx_stop),
					   dev->io_ctxs[i],
					   false);
		if (err != DOCA_ERROR_IN_PROGRESS && err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("%s: Couldn't stop vfs dev io ctx %d, err: %s",
				     dev_name,
				     i,
				     doca_error_get_name(err));
			return err;
		}
	}

	return DOCA_SUCCESS;
}

struct virtiofs_fsdev *virtiofs_fsdev_get_by_name(struct virtiofs_resources *ctx, char *name)
{
	struct virtiofs_fsdev *fsdev;

	SLIST_FOREACH(fsdev, &ctx->fsdevs, entry)
	{
		if (strcmp(fsdev->name, name) == 0)
			return fsdev;
	}

	DOCA_LOG_ERR("fsdev with name '%s' not found", name);
	return NULL;
}

doca_error_t virtiofs_fsdev_init(struct virtiofs_resources *ctx,
				 struct virtiofs_fsdev *fsdev,
				 char *name,
				 struct virtiofs_fsdev_ops *ops)
{
	int name_len = strnlen(name, VIRTIOFS_FSDEV_NAME_LENGTH + 1);

	if (name_len > VIRTIOFS_FSDEV_NAME_LENGTH) {
		DOCA_LOG_ERR("Failed to create fsdev- name is too long (max is %d)", VIRTIOFS_FSDEV_NAME_LENGTH);
		return DOCA_ERROR_INVALID_VALUE;
	}

	fsdev->ctx = ctx;
	fsdev->ops = ops;
	pthread_mutex_init(&fsdev->lock, NULL);
	SLIST_INIT(&fsdev->thread_ctxs);
	strncpy(fsdev->name, name, name_len);

	SLIST_INSERT_HEAD(&ctx->fsdevs, fsdev, entry);

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_fsdev_destroy(struct virtiofs_fsdev *fsdev)
{
	if (!SLIST_EMPTY(&fsdev->thread_ctxs)) {
		DOCA_LOG_ERR("Failed to destroy fsdev: thread ctxs list is not empty!");
		return DOCA_ERROR_IN_USE;
	}

	SLIST_REMOVE(&fsdev->ctx->fsdevs, fsdev, virtiofs_fsdev, entry);

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_fsdev_thread_ctx_init(struct virtiofs_fsdev *fsdev, struct virtiofs_fsdev_thread_ctx *tctx)
{
	tctx->fsdev = fsdev;
	pthread_mutex_lock(&fsdev->lock);
	SLIST_INSERT_HEAD(&fsdev->thread_ctxs, tctx, entry);
	pthread_mutex_unlock(&fsdev->lock);

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_fsdev_thread_ctx_destroy(struct virtiofs_fsdev_thread_ctx *tctx)
{
	pthread_mutex_lock(&tctx->fsdev->lock);
	SLIST_REMOVE(&tctx->fsdev->thread_ctxs, tctx, virtiofs_fsdev_thread_ctx, entry);
	pthread_mutex_unlock(&tctx->fsdev->lock);

	return DOCA_SUCCESS;
}

struct virtiofs_nfs_fsdev_thread_ctx {
	struct nfs_fsdev *nfs_fsdev_priv;
	struct virtiofs_fsdev_thread_ctx base;
};

struct virtiofs_nfs_fsdev {
	struct nfs_fsdev_context *nfs_fsdev;
	struct virtiofs_fsdev base;
};

static void virtiofs_nfs_fsdev_submit(struct virtiofs_fsdev_thread_ctx *tctx,
				      char *fuse_header,
				      char *cmnd_in_hdr,
				      char *datain,
				      char *fuse_out,
				      char *cmnd_out_hdr,
				      char *dataout,
				      uint16_t src_domain_id,
				      uint64_t req_id,
				      vfs_doca_fsdev_io_cb app_cb,
				      void *app_ctxt)
{
	struct virtiofs_nfs_fsdev_thread_ctx *nfs_tctx = container_of(tctx, struct virtiofs_nfs_fsdev_thread_ctx, base);

	nfs_fsdev_submit(nfs_tctx->nfs_fsdev_priv,
			 fuse_header,
			 cmnd_in_hdr,
			 datain,
			 fuse_out,
			 cmnd_out_hdr,
			 dataout,
			 src_domain_id,
			 req_id,
			 app_cb,
			 app_ctxt);
}

static void virtiofs_nfs_fsdev_progress(void *arg)
{
	struct virtiofs_nfs_fsdev_thread_ctx *nfs_tctx = arg;

	nfs_fsdev_progress(nfs_tctx->nfs_fsdev_priv);
}

static struct virtiofs_fsdev_thread_ctx *virtiofs_nfs_fsdev_thread_ctx_get(struct virtiofs_fsdev *fsdev)
{
	struct virtiofs_nfs_fsdev *nfs_fsdev = container_of(fsdev, struct virtiofs_nfs_fsdev, base);
	struct virtiofs_nfs_fsdev_thread_ctx *tctx;
	doca_error_t err;

	tctx = calloc(1, sizeof(*tctx));
	if (tctx == NULL) {
		DOCA_LOG_ERR("Failed to allocate nfs thread ctx");
		return NULL;
	}

	err = virtiofs_fsdev_thread_ctx_init(fsdev, &tctx->base);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init nfs thread ctx");
		free(tctx);
		return NULL;
	}

	tctx->nfs_fsdev_priv = nfs_fsdev_get(nfs_fsdev->nfs_fsdev);
	if (tctx->nfs_fsdev_priv == NULL) {
		DOCA_LOG_ERR("Failed to get nfs private");
		free(tctx);
		return NULL;
	}

	err = virtiofs_thread_poller_add(virtiofs_thread_get(fsdev->ctx), virtiofs_nfs_fsdev_progress, tctx);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add thread poller, err: %s", doca_error_get_name(err));
		free(tctx);
		return NULL;
	}

	return &tctx->base;
}

static doca_error_t virtiofs_nfs_fsdev_thread_ctx_put(struct virtiofs_fsdev_thread_ctx *tctx)
{
	struct virtiofs_nfs_fsdev_thread_ctx *nfs_tctx = container_of(tctx, struct virtiofs_nfs_fsdev_thread_ctx, base);
	doca_error_t err;

	err = virtiofs_thread_poller_remove(virtiofs_thread_get(tctx->fsdev->ctx),
					    virtiofs_nfs_fsdev_progress,
					    nfs_tctx);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to unregister nfs progress poller, err: %s", doca_error_get_name(err));
		return err;
	}

	priv_nfs_fsdev_deinit(nfs_tctx->nfs_fsdev_priv);

	err = virtiofs_fsdev_thread_ctx_destroy(&nfs_tctx->base);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca fsdev thread ctx, err: %s", doca_error_get_name(err));
		return err;
	}

	free(nfs_tctx);
	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_nfs_fsdev_destroy(struct virtiofs_fsdev *fsdev)
{
	struct virtiofs_nfs_fsdev *nfs_fsdev = container_of(fsdev, struct virtiofs_nfs_fsdev, base);
	doca_error_t err;

	err = virtiofs_fsdev_destroy(&nfs_fsdev->base);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca fsdev, err: %s", doca_error_get_name(err));
		return err;
	}

	nfs_fsdev_destroy(nfs_fsdev->nfs_fsdev);
	free(nfs_fsdev);
	return DOCA_SUCCESS;
}

struct virtiofs_fsdev_ops ops = {
	.destroy = virtiofs_nfs_fsdev_destroy,
	.thread_ctx_get = virtiofs_nfs_fsdev_thread_ctx_get,
	.thread_ctx_put = virtiofs_nfs_fsdev_thread_ctx_put,
	.submit = virtiofs_nfs_fsdev_submit,
};

doca_error_t virtiofs_nfs_fsdev_create(struct virtiofs_resources *ctx, char *name, char *server, char *mount_point)
{
	struct virtiofs_nfs_fsdev *fsdev;
	doca_error_t err;

	fsdev = calloc(1, sizeof(*fsdev));
	if (fsdev == NULL) {
		DOCA_LOG_ERR("Failed to allocate doca fsdev");
		return DOCA_ERROR_NO_MEMORY;
	}

	err = virtiofs_fsdev_init(ctx, &fsdev->base, name, &ops);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca fsdev, err: %s", doca_error_get_name(err));
		free(fsdev);
		return err;
	}

	fsdev->nfs_fsdev = nfs_fsdev_create(server, mount_point);
	if (fsdev->nfs_fsdev == NULL) {
		DOCA_LOG_ERR("Failed to allocate doca fsdev");
		free(fsdev);
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}
