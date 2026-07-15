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

#include <errno.h>
#include <doca_dma.h>
#include <doca_log.h>
#include <doca_devemu_vfs_io.h>
#include <doca_devemu_vfs_fuse_kernel.h>

#include <virtiofs_request.h>
#include <virtiofs_mpool.h>
#include <virtiofs_core.h>

DOCA_LOG_REGISTER(VIRTIOFS_REQUEST)

extern bool skip_rw;

static void virtiofs_req_complete(struct virtiofs_request *dreq, doca_error_t err);

static doca_error_t virtiofs_dma_task_submit(struct virtiofs_request *dreq)
{
	struct virtiofs_device_io_ctx *dio = dreq->dio;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct doca_buf *src_buf, *dest_buf;
	struct doca_dma_task_memcpy *dma_task;
	doca_error_t err;

	task_user_data.ptr = dreq;

	if (dreq->dma_op == VIRTIOFS_REQUEST_DMA_OP_FROM_HOST) {
		src_buf = dreq->datain.host_doca_buf;
		dest_buf = dreq->datain.arm_doca_buf;
	} else {
		src_buf = dreq->dataout.arm_doca_buf;
		dest_buf = dreq->dataout.host_doca_buf;
	}

	/* Allocate and construct DMA task */
	err = doca_dma_task_memcpy_alloc_init(dio->dma_ctx, src_buf, dest_buf, task_user_data, &dma_task);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to allocate DMA memcpy task, err: [%s]\n", doca_error_get_name(err));
		goto out;
	}

	task = doca_dma_task_memcpy_as_task(dma_task);

	/* Submit DMA task */
	err = doca_task_submit(task);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to submit DMA task, err: [%s]", doca_error_get_name(err));
		goto free_task;
	}

	/* Save "task" to be freed later once the DMA is complete */
	dreq->task = task;

	return DOCA_SUCCESS;

free_task:
	doca_task_free(task);
out:
	return err;
}

static bool virtiofs_is_resource_available(struct virtiofs_request *dreq)
{
	/* if no dma resources, queue the request for later */
	if (dreq->dio->reqs_avail == 0) {
		DOCA_LOG_WARN("dev %s: adding req 0x%p to dio 0x%p pending list",
			      dreq->dio->dev->config.name,
			      dreq,
			      dreq->dio);
		TAILQ_INSERT_TAIL(&dreq->dio->pending, dreq, entry);
		return false;
	}

	dreq->dio->reqs_avail--;
	dreq->dio->thread->curr_inflights++;

	return true;
}

static doca_error_t virtiofs_process_datain(struct virtiofs_request *dreq)
{
	doca_error_t err = DOCA_SUCCESS;
	struct virtiofs_device_io_ctx *dio = dreq->dio;

	dreq->datain.host_doca_buf = dreq->in_datain;
	if (dreq->datain.host_doca_buf == NULL) {
		DOCA_LOG_ERR("dev %s: Failed to get datain doca_buf from req", dio->dev->config.name);
		err = DOCA_ERROR_INVALID_VALUE;
		goto out_err;
	}

	dreq->datain.length = doca_devemu_vfs_fuse_req_get_datain_data_len(dreq->devemu_fuse_req);

	/* This is the destination buffer for the DMA of datain from the host */
	err = virtiofs_mpool_set_buf_get(dio->mpool_set, dreq->datain.length, &dreq->datain.arm_doca_buf);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca buf from mpool, err: %s", doca_error_get_name(err));
		goto out_err;
	}
	/* Reset data_len as the "entry" is reused */
	err = doca_buf_reset_data_len(dreq->datain.arm_doca_buf);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to reset data len for datain doca_buf, err: [%s]", doca_error_get_name(err));
		goto out_err;
	}

	dreq->dma_op = VIRTIOFS_REQUEST_DMA_OP_FROM_HOST;

	err = virtiofs_dma_task_submit(dreq);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to submit DMA task, err: [%s]\n", doca_error_get_name(err));
		goto out_err;
	}

	return DOCA_SUCCESS;

out_err:
	dreq->dataout.length = 0;
	virtiofs_req_complete(dreq, err);
	return err;
}

static doca_error_t virtiofs_request_dma_from_host_done(struct virtiofs_request *dreq)
{
	struct virtiofs_io_req_data *dataout = &dreq->dataout;
	void *datain_data = NULL, *dataout_data = NULL;
	doca_error_t err;

	if (dreq->in_datain) {
		err = doca_buf_get_data(dreq->datain.arm_doca_buf, &datain_data);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get doca buf data, err: %s", doca_error_get_name(err));
			return err;
		}
	}

	if (dreq->out_dataout) {
		dataout->host_doca_buf = dreq->out_dataout;
		dataout->length = doca_devemu_vfs_fuse_req_get_dataout_data_len(dreq->devemu_fuse_req);
		size_t len;
		doca_buf_get_data_len(dataout->host_doca_buf, &len);

		err = virtiofs_mpool_set_buf_get(dreq->dio->mpool_set, dataout->length, &dreq->dataout.arm_doca_buf);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get doca buffer from mpool, err: %s", doca_error_get_name(err));
			return err;
		}

		err = doca_buf_get_data(dreq->dataout.arm_doca_buf, &dataout_data);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get doca buffer data, err: %s", doca_error_get_name(err));
			return err;
		}
	}

	if (doca_unlikely(skip_rw)) {
		struct fuse_in_header *hdr_in = (struct fuse_in_header *)dreq->hdr_in;
		if (hdr_in->opcode == FUSE_WRITE) {
			struct fuse_out_header *hdr_out = (struct fuse_out_header *)(dreq->hdr_out);
			struct fuse_write_in *write_in = (struct fuse_write_in *)(dreq->cmnd_hdr_in);
			struct fuse_write_out *write_out = (struct fuse_write_out *)(dreq->cmnd_hdr_out);
			hdr_out->error = 0;
			hdr_out->len = sizeof(struct fuse_out_header) + sizeof(struct fuse_write_out);
			hdr_out->unique = hdr_in->unique;
			write_out->size = write_in->size;
			dreq->cb(dreq, 0);
			return DOCA_SUCCESS;
		} else if (hdr_in->opcode == FUSE_READ) {
			struct fuse_out_header *hdr_out = (struct fuse_out_header *)(dreq->hdr_out);
			struct fuse_read_in *read_in = (struct fuse_read_in *)(dreq->cmnd_hdr_in);
			hdr_out->error = 0;
			hdr_out->len = sizeof(struct fuse_out_header) + read_in->size;
			hdr_out->unique = hdr_in->unique;
			dreq->cb(dreq, 0);
			return DOCA_SUCCESS;
		}
	}

	if (doca_unlikely(dreq->dio->fsdev_tctx == NULL)) {
		dreq->dio->fsdev_tctx = dreq->dio->dev->fsdev->ops->thread_ctx_get(dreq->dio->dev->fsdev);
		if (doca_unlikely(dreq->dio->fsdev_tctx == NULL)) {
			DOCA_LOG_ERR("Failed to get fsdev thread ctx");
			return DOCA_ERROR_UNKNOWN;
		}
	}

	dreq->dio->fsdev_tctx->fsdev->ops->submit(dreq->dio->fsdev_tctx,
						  (char *)dreq->hdr_in,
						  dreq->cmnd_hdr_in,
						  datain_data,
						  dreq->hdr_out,
						  dreq->cmnd_hdr_out,
						  dataout_data,
						  doca_devemu_vfs_fuse_req_get_src_domain_id(dreq->devemu_fuse_req),
						  doca_devemu_vfs_fuse_req_get_id(dreq->devemu_fuse_req),
						  dreq->cb,
						  dreq);
	return DOCA_SUCCESS;
}

static void virtiofs_process_dataout(struct virtiofs_request *dreq)
{
	virtiofs_request_dma_from_host_done(dreq);
}

static void virtiofs_request_handle(struct virtiofs_request *dreq)
{
	if (!virtiofs_is_resource_available(dreq))
		return;

	if (dreq->in_datain) {
		/* If a request has "datain", we can submit it to doca_dma and add it to order list,
		 * since the doca_dma gurantees ordered completions. */
		if (virtiofs_process_datain(dreq) == DOCA_SUCCESS)
			TAILQ_INSERT_TAIL(&dreq->dio->order, dreq, order);
	} else {
		if (TAILQ_EMPTY(&dreq->dio->order)) {
			/* Request without "datain", safe to submit to backend as there are no outstanding IOs */
			virtiofs_process_dataout(dreq);
		} else {
			/* Order list is not empty, can't skip the queue, wait for the turn */
			TAILQ_INSERT_TAIL(&dreq->dio->order, dreq, order);
		}
	}
}

static void virtiofs_req_complete(struct virtiofs_request *dreq, doca_error_t error)
{
	struct virtiofs_device_io_ctx *dio = dreq->dio;
	struct virtiofs_request *pending_dreq;
	void *head;

	dio->reqs_avail++;
	dio->thread->curr_inflights--;

	doca_devemu_vfs_fuse_req_complete(dreq->devemu_fuse_req, error);

	if (dreq->datain.arm_doca_buf) {
		virtiofs_mpool_buf_put(dreq->datain.arm_doca_buf);
		dreq->datain.arm_doca_buf = NULL;
		dreq->datain.length = 0;
	}

	if (dreq->dataout.arm_doca_buf) {
		/* Reset the "data" ptr of dataout doca_buf */
		doca_buf_get_head(dreq->dataout.arm_doca_buf, &head);
		doca_buf_set_data(dreq->dataout.arm_doca_buf, head, 0);

		virtiofs_mpool_buf_put(dreq->dataout.arm_doca_buf);
		dreq->dataout.arm_doca_buf = NULL;
		dreq->dataout.length = 0;
	}

	dreq->task = NULL;
	dreq->devemu_fuse_req = NULL;
	dreq->dma_to_host_offset = 0;

	DOCA_LOG_DBG("%s, core_id %d", dio->dev->config.name, dio->thread->attr.core_id);

	dreq->hdr_in = NULL;
	dreq->hdr_out = NULL;
	dreq->cmnd_hdr_in = NULL;
	dreq->cmnd_hdr_out = NULL;
	dreq->in_datain = NULL;
	dreq->out_dataout = NULL;

	/* handle single pending request, if exist */
	if (!TAILQ_EMPTY(&dio->pending)) {
		pending_dreq = TAILQ_FIRST(&dio->pending);
		TAILQ_REMOVE(&dio->pending, pending_dreq, entry);
		virtiofs_request_handle(pending_dreq);
	}
}

static doca_error_t virtiofs_request_dma_to_host_progress(struct virtiofs_request *dreq)
{
	struct virtiofs_io_req_data *dataout = &dreq->dataout;
	struct doca_buf *next_host_buf;
	uint8_t *arm_buf_head, *host_buf_data;
	size_t curr_host_buf_len;
	doca_error_t err;

	if (dataout->host_doca_buf == NULL)
		return DOCA_SUCCESS;

	/*
	 * doca_dma does not support chaining of doca_buf for destination.
	 * Therefore we DMA the dataout doca_buf(s) one by one.
	 */

	/* Get current writable descriptor len */
	err = doca_buf_get_data_len(dataout->host_doca_buf, &curr_host_buf_len);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca_buf data len, err: [%s]", doca_error_get_name(err));
		return err;
	}

	/* Set arm_doca_buf's data addr/len to the next dataout segment to be DMAed to host */
	err = doca_buf_get_head(dataout->arm_doca_buf, (void **)&arm_buf_head);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to get data from doca_buf, err: [%s]", doca_error_get_name(err));
		return err;
	}

	err = doca_buf_set_data(dataout->arm_doca_buf, arm_buf_head + dreq->dma_to_host_offset, curr_host_buf_len);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to set data from doca_buf, err: [%s]", doca_error_get_name(err));
		return err;
	}

	/* Set current host_doca_buf's data_len to 0 as this is the dest now */
	err = doca_buf_get_data(dataout->host_doca_buf, (void **)&host_buf_data);
	err = doca_buf_set_data(dataout->host_doca_buf, host_buf_data, 0);

	dreq->dma_op = VIRTIOFS_REQUEST_DMA_OP_TO_HOST;

	err = virtiofs_dma_task_submit(dreq);
	if (doca_unlikely(err != DOCA_SUCCESS)) {
		DOCA_LOG_ERR("Failed to submit DMA task, err: [%s]", doca_error_get_name(err));
		return err;
	}

	/* Continue to the next writable descriptor */
	dreq->dma_to_host_offset += curr_host_buf_len;
	doca_buf_get_next_in_list(dataout->host_doca_buf, &next_host_buf);
	dataout->host_doca_buf = next_host_buf;

	return DOCA_ERROR_IN_PROGRESS;
}

static void virtiofs_process_dio_order_list(struct virtiofs_device_io_ctx *dio)
{
	struct virtiofs_request *dreq;

	while (!TAILQ_EMPTY(&dio->order)) {
		dreq = TAILQ_FIRST(&dio->order);
		if (dreq->in_datain == NULL) {
			/* Request without any "datain" waiting for its turn */
			virtiofs_process_dataout(dreq);
			TAILQ_REMOVE(&dio->order, dreq, order);
		} else {
			/* Request with "datain", will be processed when dma cb is received */
			break;
		}
	}
}

void virtiofs_request_dma_done(struct doca_dma_task_memcpy *dma_task,
			       union doca_data task_user_data,
			       union doca_data ctx_user_data)
{
	(void)dma_task;
	(void)ctx_user_data;
	struct virtiofs_request *dreq = task_user_data.ptr, *order_dreq;
	doca_error_t err;

	DOCA_LOG_DBG("%s, core_id %d", dreq->dio->dev->config.name, dreq->dio->thread->attr.core_id);

	doca_task_free(dreq->task);

	if (dreq->dma_op == VIRTIOFS_REQUEST_DMA_OP_FROM_HOST) {
		order_dreq = TAILQ_FIRST(&dreq->dio->order);
		if (order_dreq != dreq) {
			/* This can never happen as completions from doca_dma are guaranteed */
			DOCA_LOG_ERR("Invalid condition order_dreq:%p dreq:%p", order_dreq, dreq);
			assert(0);
		}

		virtiofs_request_dma_from_host_done(dreq);

		TAILQ_REMOVE(&dreq->dio->order, dreq, order);

		virtiofs_process_dio_order_list(dreq->dio);
	} else {
		err = virtiofs_request_dma_to_host_progress(dreq);
		if (err != DOCA_ERROR_IN_PROGRESS) {
			/* DMA to host done/failed, complete the req */
			virtiofs_req_complete(dreq, err);
		}
	}
}

void virtiofs_request_dma_err(struct doca_dma_task_memcpy *dma_task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	(void)dma_task;
	struct virtiofs_device_io_ctx *dio = ctx_user_data.ptr;
	struct virtiofs_request *dreq = task_user_data.ptr;

	DOCA_LOG_ERR("%s", dio->dev->config.name);

	doca_task_free(dreq->task);

	virtiofs_req_complete(dreq, -EINVAL);
}

static const char *fuse_opcode_to_string(int fuse_opcode)
{
	switch (fuse_opcode) {
	case FUSE_LOOKUP:
		return "FUSE_LOOKUP";
	case FUSE_FORGET:
		return "FUSE_FORGET";
	case FUSE_GETATTR:
		return "FUSE_GETATTR";
	case FUSE_SETATTR:
		return "FUSE_SETATTR";
	case FUSE_READLINK:
		return "FUSE_READLINK";
	case FUSE_SYMLINK:
		return "FUSE_SYMLINK";
	case FUSE_MKNOD:
		return "FUSE_MKNOD";
	case FUSE_MKDIR:
		return "FUSE_MKDIR";
	case FUSE_UNLINK:
		return "FUSE_UNLINK";
	case FUSE_RMDIR:
		return "FUSE_RMDIR";
	case FUSE_RENAME:
		return "FUSE_RENAME";
	case FUSE_LINK:
		return "FUSE_LINK";
	case FUSE_OPEN:
		return "FUSE_OPEN";
	case FUSE_READ:
		return "FUSE_READ";
	case FUSE_WRITE:
		return "FUSE_WRITE";
	case FUSE_STATFS:
		return "FUSE_STATFS";
	case FUSE_RELEASE:
		return "FUSE_RELEASE";
	case FUSE_FSYNC:
		return "FUSE_FSYNC";
	case FUSE_SETXATTR:
		return "FUSE_SETXATTR";
	case FUSE_GETXATTR:
		return "FUSE_GETXATTR";
	case FUSE_LISTXATTR:
		return "FUSE_LISTXATTR";
	case FUSE_REMOVEXATTR:
		return "FUSE_REMOVEXATTR";
	case FUSE_FLUSH:
		return "FUSE_FLUSH";
	case FUSE_INIT:
		return "FUSE_INIT";
	case FUSE_OPENDIR:
		return "FUSE_OPENDIR";
	case FUSE_READDIR:
		return "FUSE_READDIR";
	case FUSE_RELEASEDIR:
		return "FUSE_RELEASEDIR";
	case FUSE_FSYNCDIR:
		return "FUSE_FSYNCDIR";
	case FUSE_GETLK:
		return "FUSE_GETLK";
	case FUSE_SETLK:
		return "FUSE_SETLK";
	case FUSE_SETLKW:
		return "FUSE_SETLKW";
	case FUSE_ACCESS:
		return "FUSE_ACCESS";
	case FUSE_CREATE:
		return "FUSE_CREATE";
	case FUSE_INTERRUPT:
		return "FUSE_INTERRUPT";
	case FUSE_BMAP:
		return "FUSE_BMAP";
	case FUSE_DESTROY:
		return "FUSE_DESTROY";
	case FUSE_IOCTL:
		return "FUSE_IOCTL";
	case FUSE_POLL:
		return "FUSE_POLL";
	case FUSE_NOTIFY_REPLY:
		return "FUSE_NOTIFY_REPLY";
	case FUSE_BATCH_FORGET:
		return "FUSE_BATCH_FORGET";
	case FUSE_FALLOCATE:
		return "FUSE_FALLOCATE";
	case FUSE_READDIRPLUS:
		return "FUSE_READDIRPLUS";
	case FUSE_RENAME2:
		return "FUSE_RENAME2";
	case FUSE_LSEEK:
		return "FUSE_LSEEK";
	case FUSE_COPY_FILE_RANGE:
		return "FUSE_COPY_FILE_RANGE";
	case FUSE_SETUPMAPPING:
		return "FUSE_SETUPMAPPING";
	case FUSE_REMOVEMAPPING:
		return "FUSE_REMOVEMAPPING";
	default:
		return "UNKNOWN_OPCODE";
	}
}

static void vfs_doca_complete_fuse_req(struct virtiofs_request *dreq, int error)
{
	struct fuse_in_header *in;

	if (error) {
		in = (struct fuse_in_header *)dreq->hdr_in;
		if (error == -ENOSYS)
			DOCA_LOG_ERR("%s not supported by backend", fuse_opcode_to_string(in->opcode));
		else if (error != -ENOENT)
			DOCA_LOG_ERR("%s request failed by backend, err: %d", fuse_opcode_to_string(in->opcode), error);
	}

	virtiofs_req_complete(dreq, error ? DOCA_ERROR_UNKNOWN : 0);
}

static void virtiofs_fuse_cmnd_done_cb(void *cb_arg, int error)
{
	vfs_doca_complete_fuse_req((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_lookup_done_cb(void *cb_arg, int error)
{
	vfs_doca_complete_fuse_req((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_create_done_cb(void *cb_arg, int error)
{
	vfs_doca_complete_fuse_req((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_write_done_cb(void *cb_arg, int error)
{
	vfs_doca_complete_fuse_req((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_mknod_done_cb(void *cb_arg, int error)
{
	vfs_doca_complete_fuse_req((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_unlink_done_cb(void *cb_arg, int error)
{
	vfs_doca_complete_fuse_req((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_dma_to_host(struct virtiofs_request *dreq, int error)
{
	doca_error_t err;

	if (error)
		goto out;

	err = virtiofs_request_dma_to_host_progress(dreq);
	if (err != DOCA_ERROR_IN_PROGRESS)
		goto out;

	return;

out:
	/* DMA to host done/failed, complete the req */
	vfs_doca_complete_fuse_req(dreq, error);
}

static void virtiofs_read_done_cb(void *cb_arg, int error)
{
	virtiofs_dma_to_host((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_readdir_done_cb(void *cb_arg, int error)
{
	virtiofs_dma_to_host((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_getxattr_done_cb(void *cb_arg, int error)
{
	virtiofs_dma_to_host((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_listxattr_done_cb(void *cb_arg, int error)
{
	virtiofs_dma_to_host((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_readlink_done_cb(void *cb_arg, int error)
{
	virtiofs_dma_to_host((struct virtiofs_request *)cb_arg, error);
}

static void virtiofs_ioctl_done_cb(void *cb_arg, int error)
{
	virtiofs_dma_to_host((struct virtiofs_request *)cb_arg, error);
}

static doca_error_t vfs_doca_fill_dio_in_dreq(struct virtiofs_request *dreq, struct doca_devemu_vfs_fuse_req *req)
{
	doca_error_t err;
	union doca_data user_data;
	struct virtiofs_device_io_ctx *dio;
	struct doca_devemu_vfs_io *vfs_io = doca_devemu_vfs_fuse_req_get_vfs_io(req);

	err = doca_ctx_get_user_data(doca_devemu_vfs_io_as_ctx(vfs_io), &user_data);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca_ctx user_data, err: %s", doca_error_get_name(err));
		return err;
	}

	dio = user_data.ptr;
	dreq->dio = dio;
	dreq->devemu_fuse_req = req;

	return DOCA_SUCCESS;
}

static void virtiofs_process_request(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     char *hdr_in,
				     char *cmnd_hdr_in,
				     struct doca_buf *datain,
				     char *hdr_out,
				     char *cmnd_hdr_out,
				     struct doca_buf *dataout,
				     vfs_doca_fsdev_io_cb cb)
{
	doca_error_t err;
	struct virtiofs_request *dreq = req_user_data;

	dreq->hdr_in = hdr_in;
	dreq->cmnd_hdr_in = cmnd_hdr_in;
	dreq->in_datain = datain;
	dreq->hdr_out = hdr_out;
	dreq->cmnd_hdr_out = cmnd_hdr_out;
	dreq->out_dataout = dataout;
	dreq->cb = cb;

	err = vfs_doca_fill_dio_in_dreq(dreq, req);
	if (err != DOCA_SUCCESS) {
		vfs_doca_complete_fuse_req(dreq, err);
		return;
	}

	virtiofs_request_handle(dreq);
}

void virtiofs_request_init_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_init_in *init_in,
				   struct fuse_out_header *out,
				   struct fuse_init_out *init_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)init_in,
				 NULL,
				 (char *)out,
				 (char *)init_out,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_destroy_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_lookup_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out,
				     struct fuse_entry_out *entry)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 datain,
				 (char *)out,
				 (char *)entry,
				 NULL,
				 virtiofs_lookup_done_cb);
}

void virtiofs_request_open_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_open_in *open_in,
				   struct fuse_out_header *out,
				   struct fuse_open_out *open_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)open_in,
				 NULL,
				 (char *)out,
				 (char *)open_out,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_create_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_create_in *create_in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out,
				     struct fuse_entry_out *entry_out,
				     struct fuse_open_out *open_out)
{
	(void)open_out;
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)create_in,
				 datain,
				 (char *)out,
				 (char *)entry_out,
				 NULL,
				 virtiofs_create_done_cb);
}

void virtiofs_request_read_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_read_in *read_in,
				   struct fuse_out_header *out,
				   struct doca_buf *dataout)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)read_in,
				 NULL,
				 (char *)out,
				 NULL,
				 dataout,
				 virtiofs_read_done_cb);
}

void virtiofs_request_write_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_write_in *write_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_write_out *write_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)write_in,
				 datain,
				 (char *)out,
				 (char *)write_out,
				 NULL,
				 virtiofs_write_done_cb);
}

void virtiofs_request_mknod_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_mknod_in *mknod_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_entry_out *entry_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)mknod_in,
				 datain,
				 (char *)out,
				 (char *)entry_out,
				 NULL,
				 virtiofs_mknod_done_cb);
}

void virtiofs_request_readdir_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_read_in *read_in,
				      struct fuse_out_header *out,
				      struct doca_buf *dataout)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)read_in,
				 NULL,
				 (char *)out,
				 NULL,
				 dataout,
				 virtiofs_readdir_done_cb);
}

void virtiofs_request_getattr_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_getattr_in *getattr_in,
				      struct fuse_out_header *out,
				      struct fuse_attr_out *attr_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)getattr_in,
				 NULL,
				 (char *)out,
				 (char *)attr_out,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_releasedir_handler(struct doca_devemu_vfs_fuse_req *req,
					 void *req_user_data,
					 struct fuse_in_header *in,
					 struct fuse_release_in *release_in,
					 struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)release_in,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_opendir_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_open_in *open_in,
				      struct fuse_out_header *out,
				      struct fuse_open_out *open_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)open_in,
				 NULL,
				 (char *)out,
				 (char *)open_out,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_setattr_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_setattr_in *setattr_in,
				      struct fuse_out_header *out,
				      struct fuse_attr_out *attr_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)setattr_in,
				 NULL,
				 (char *)out,
				 (char *)attr_out,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_release_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_release_in *release_in,
				      struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)release_in,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_flush_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_flush_in *flush_in,
				    struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)flush_in,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_forget_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_forget_in *forget_in)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)forget_in,
				 NULL,
				 NULL,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_statfs_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_out_header *out,
				     struct fuse_statfs_out *statfs_out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 NULL,
				 (char *)out,
				 (char *)statfs_out,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_unlink_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 datain,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_unlink_done_cb);
}

void virtiofs_request_getxattr_handler(struct doca_devemu_vfs_fuse_req *req,
				       void *req_user_data,
				       struct fuse_in_header *in,
				       struct fuse_getxattr_in *getxattr_in,
				       struct doca_buf *datain,
				       struct fuse_out_header *out,
				       struct fuse_getxattr_out *getxattr_out,
				       struct doca_buf *dataout)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)getxattr_in,
				 datain,
				 (char *)out,
				 (char *)getxattr_out,
				 dataout,
				 virtiofs_getxattr_done_cb);
}

void virtiofs_request_mkdir_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_mkdir_in *mkdir_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_entry_out *entry)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)mkdir_in,
				 datain,
				 (char *)out,
				 (char *)entry,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_rmdir_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 datain,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_rename_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_rename_in *rename_in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)rename_in,
				 datain,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_rename2_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_rename2_in *rename_in,
				      struct doca_buf *datain,
				      struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)rename_in,
				 datain,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_readlink_handler(struct doca_devemu_vfs_fuse_req *req,
				       void *req_user_data,
				       struct fuse_in_header *in,
				       struct fuse_out_header *out,
				       struct doca_buf *dataout)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 NULL,
				 (char *)out,
				 NULL,
				 dataout,
				 virtiofs_readlink_done_cb);
}

void virtiofs_request_symlink_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct doca_buf *datain,
				      struct fuse_out_header *out,
				      struct fuse_entry_out *entry)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 datain,
				 (char *)out,
				 (char *)entry,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_link_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_link_in *link_in,
				   struct doca_buf *datain,
				   struct fuse_out_header *out,
				   struct fuse_entry_out *entry)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)link_in,
				 datain,
				 (char *)out,
				 (char *)entry,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_fsync_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_fsync_in *fsync_in,
				    struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)fsync_in,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_fallocate_handler(struct doca_devemu_vfs_fuse_req *req,
					void *req_user_data,
					struct fuse_in_header *in,
					struct fuse_fallocate_in *fallocate_in,
					struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)fallocate_in,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_setxattr_handler(struct doca_devemu_vfs_fuse_req *req,
				       void *req_user_data,
				       struct fuse_in_header *in,
				       struct fuse_setxattr_in *setxattr_in,
				       struct doca_buf *datain,
				       struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)setxattr_in,
				 datain,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_listxattr_handler(struct doca_devemu_vfs_fuse_req *req,
					void *req_user_data,
					struct fuse_in_header *in,
					struct fuse_getxattr_in *listxattr_in,
					struct fuse_out_header *out,
					struct fuse_getxattr_out *listxattr_out,
					struct doca_buf *dataout)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)listxattr_in,
				 NULL,
				 (char *)out,
				 (char *)listxattr_out,
				 dataout,
				 virtiofs_listxattr_done_cb);
}

void virtiofs_request_removexattr_handler(struct doca_devemu_vfs_fuse_req *req,
					  void *req_user_data,
					  struct fuse_in_header *in,
					  struct doca_buf *datain,
					  struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 NULL,
				 datain,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_syncfs_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_syncfs_in *syncfs_in,
				     struct fuse_out_header *out)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)syncfs_in,
				 NULL,
				 (char *)out,
				 NULL,
				 NULL,
				 virtiofs_fuse_cmnd_done_cb);
}

void virtiofs_request_ioctl_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_ioctl_in *ioctl_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_ioctl_out *ioctl_out,
				    struct doca_buf *dataout)
{
	virtiofs_process_request(req,
				 req_user_data,
				 (char *)in,
				 (char *)ioctl_in,
				 datain,
				 (char *)out,
				 (char *)ioctl_out,
				 dataout,
				 virtiofs_ioctl_done_cb);
}
