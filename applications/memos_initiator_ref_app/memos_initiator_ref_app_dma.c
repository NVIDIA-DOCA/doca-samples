/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_dma.h"
#include "memos_initiator_ref_app_nvme_spec.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_devemu_nvme_io.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_DMA);

/* Host PRP page size. This app advertises CAP.MPSMIN = 0 (4 KiB) and does not honor a larger
 * CC.MPS, so PRP entries always carve the host buffer into 4 KiB pages. */
#define MEMOS_INITIATOR_REF_APP_DMA_PAGE_SIZE 4096
/* Buffer inventory budget: per in-flight op we hold at most one scratch buf plus two host bufs
 * (PRP1 + PRP2 split), with slack. */
#define MEMOS_INITIATOR_REF_APP_DMA_BUFS_PER_OP 4
/* Bounded wait for the DMA ctx to reach IDLE on teardown (no DMA should be in flight by then). */
#define MEMOS_INITIATOR_REF_APP_DMA_STOP_TIMEOUT_US (1 * 1000 * 1000) /* 1 s */
#define MEMOS_INITIATOR_REF_APP_DMA_STOP_SLEEP_NS (10 * 1000)	      /* 10 us */

/* One in-flight DMA. Each op owns a fixed scratch slot for its lifetime; bufs are acquired per
 * transfer and released on completion. host_head is the head of the host-side buffer list (single
 * buf, or PRP1 chained to PRP2); decrementing its refcount releases the whole list. It is the
 * transfer's destination when moving to the host and its source when moving from the host, which is
 * why it is named for the side it addresses rather than for a direction. */
struct memos_initiator_ref_app_dma_op {
	struct memos_initiator_ref_app_resources *app;
	struct doca_devemu_nvme_req *req;
	struct doca_buf *scratch_buf;
	struct doca_buf *host_head;
	uint8_t *scratch;
	size_t len;
	uint32_t success_dw0;
	uint16_t success_status;
	bool in_use;
};

struct memos_initiator_ref_app_dma {
	struct doca_dma *dma;
	struct doca_pe *pe;
	struct doca_mmap *scratch_mmap;
	struct doca_buf_inventory *buf_inv;
	uint8_t *scratch_base;
	uint32_t pool_size;
	struct memos_initiator_ref_app_dma_op *ops;
};

static struct memos_initiator_ref_app_dma_op *dma_op_alloc(struct memos_initiator_ref_app_dma *eng)
{
	uint32_t i;

	for (i = 0; i < eng->pool_size; ++i) {
		struct memos_initiator_ref_app_dma_op *op = &eng->ops[i];

		if (!op->in_use) {
			op->in_use = true;
			op->req = NULL;
			op->scratch_buf = NULL;
			op->host_head = NULL;
			op->len = 0;
			op->success_dw0 = 0;
			op->success_status = NVME_SC_SUCCESS;
			return op;
		}
	}
	return NULL;
}

static void dma_op_release_bufs(struct memos_initiator_ref_app_dma_op *op)
{
	if (op->scratch_buf != NULL) {
		(void)doca_buf_dec_refcount(op->scratch_buf, NULL);
		op->scratch_buf = NULL;
	}
	/* Releasing the list head releases every chained host buf (NVMe PRP1 + PRP2 split). */
	if (op->host_head != NULL) {
		(void)doca_buf_dec_refcount(op->host_head, NULL);
		op->host_head = NULL;
	}
}

static void dma_op_free(struct memos_initiator_ref_app_dma_op *op)
{
	dma_op_release_bufs(op);
	op->req = NULL;
	op->in_use = false;
}

/*
 * Resolve the host side of a transfer of @len bytes from the command's PRP1/PRP2 and build the host
 * doca_buf list in op->host_head. PRP1/PRP2 only: a transfer that spans more than two pages would
 * need a PRP list and is rejected with DOCA_ERROR_NOT_SUPPORTED.
 */
/*
 * Declare how much valid data a host buffer holds: its own range when it is the transfer's source,
 * zero when it is the destination, where data_len is the DMA's output rather than its input.
 */
static doca_error_t dma_set_host_data(struct doca_buf *buf, uint64_t addr, size_t len, bool host_is_src)
{
	doca_error_t result = doca_buf_set_data(buf, (void *)addr, host_is_src ? len : 0);

	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to set host buf data: %s", doca_error_get_name(result));
	return result;
}

static doca_error_t dma_host_buf_build(struct memos_initiator_ref_app_resources *app,
				       struct memos_initiator_ref_app_dma *eng,
				       const void *sqe,
				       size_t len,
				       bool host_is_src,
				       struct doca_buf **head_out)
{
	const uint64_t page = MEMOS_INITIATOR_REF_APP_DMA_PAGE_SIZE;
	uint64_t prp1 = nvme_sqe_prp1(sqe);
	uint64_t prp2 = nvme_sqe_prp2(sqe);
	uint64_t prp1_len = page - (prp1 & (page - 1));
	struct doca_buf *tail;
	doca_error_t result;

	(*head_out) = NULL;

	/* Fits in the first PRP page: a single buffer. */
	if (len <= prp1_len) {
		result = doca_buf_inventory_buf_get_by_addr(eng->buf_inv,
							    app->host_mmap,
							    (void *)prp1,
							    len,
							    &(*head_out));
		if (result == DOCA_SUCCESS)
			result = dma_set_host_data((*head_out), prp1, len, host_is_src);
		if (result != DOCA_SUCCESS && (*head_out) != NULL) {
			(void)doca_buf_dec_refcount((*head_out), NULL);
			(*head_out) = NULL;
		}
		return result;
	}

	/* Spans into the second page: PRP2 supplies it. More than two pages needs a PRP list. */
	if (len > prp1_len + page) {
		DOCA_LOG_WARN("Transfer of %zu B needs a PRP list (prp1=0x%" PRIx64 " prp2=0x%" PRIx64
			      ") - unsupported",
			      len,
			      prp1,
			      prp2);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* PRP2 is only a data pointer when the remainder fits in one page, and then it addresses the
	 * start of that page. A value with a page offset is either a PRP list pointer or malformed,
	 * and treating it as data would move the transfer to the wrong host memory. */
	if ((prp2 & (page - 1)) != 0) {
		DOCA_LOG_ERR("PRP2 0x%016" PRIx64 " is not page-aligned; PRP lists are not supported", prp2);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* If PRP2 continues directly after PRP1's page, the region is contiguous in host memory. */
	if (prp1 + prp1_len == prp2) {
		result = doca_buf_inventory_buf_get_by_addr(eng->buf_inv,
							    app->host_mmap,
							    (void *)prp1,
							    len,
							    &(*head_out));
		if (result == DOCA_SUCCESS)
			result = dma_set_host_data((*head_out), prp1, len, host_is_src);
		if (result != DOCA_SUCCESS && (*head_out) != NULL) {
			(void)doca_buf_dec_refcount((*head_out), NULL);
			(*head_out) = NULL;
		}
		return result;
	}

	result = doca_buf_inventory_buf_get_by_addr(eng->buf_inv, app->host_mmap, (void *)prp1, prp1_len, &(*head_out));
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_buf_inventory_buf_get_by_addr(eng->buf_inv, app->host_mmap, (void *)prp2, len - prp1_len, &tail);
	if (result != DOCA_SUCCESS) {
		(void)doca_buf_dec_refcount((*head_out), NULL);
		(*head_out) = NULL;
		return result;
	}

	/* A buffer's data segment must stay inside its own memory range - exceeding it is undefined,
	 * and the check is compiled out of release builds. Each half carries its own. */
	result = dma_set_host_data((*head_out), prp1, prp1_len, host_is_src);
	if (result == DOCA_SUCCESS)
		result = dma_set_host_data(tail, prp2, len - prp1_len, host_is_src);
	if (result != DOCA_SUCCESS) {
		(void)doca_buf_dec_refcount(tail, NULL);
		(void)doca_buf_dec_refcount((*head_out), NULL);
		(*head_out) = NULL;
		return result;
	}

	result = doca_buf_chain_list((*head_out), tail);
	if (result != DOCA_SUCCESS) {
		(void)doca_buf_dec_refcount(tail, NULL);
		(void)doca_buf_dec_refcount((*head_out), NULL);
		(*head_out) = NULL;
	}
	return result;
}

doca_error_t memos_initiator_ref_app_dma_host_buf_get(struct memos_initiator_ref_app_resources *app,
						      struct memos_initiator_ref_app_dma *eng,
						      const void *sqe,
						      size_t len,
						      bool host_is_src,
						      struct doca_buf **head,
						      uint32_t *list_len)
{
	doca_error_t result;

	*head = NULL;
	*list_len = 0;

	result = dma_host_buf_build(app, eng, sqe, len, host_is_src, head);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_buf_get_list_len(*head, list_len);
	if (result != DOCA_SUCCESS) {
		memos_initiator_ref_app_dma_host_buf_put(*head);
		*head = NULL;
	}
	return result;
}

void memos_initiator_ref_app_dma_host_buf_put(struct doca_buf *head)
{
	if (head != NULL)
		(void)doca_buf_dec_refcount(head, NULL);
}

static doca_error_t dma_resolve_prp_host(struct memos_initiator_ref_app_resources *app,
					 struct memos_initiator_ref_app_dma *eng,
					 const void *sqe,
					 size_t len,
					 bool host_is_src,
					 struct memos_initiator_ref_app_dma_op *op)
{
	return dma_host_buf_build(app, eng, sqe, len, host_is_src, &op->host_head);
}

static void dma_memcpy_completed_cb(struct doca_dma_task_memcpy *task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
	struct memos_initiator_ref_app_dma_op *op = (struct memos_initiator_ref_app_dma_op *)task_user_data.ptr;

	(void)ctx_user_data;

	(void)doca_devemu_nvme_req_complete(op->req, op->success_dw0, 0, op->success_status);
	doca_task_free(doca_dma_task_memcpy_as_task(task));
	dma_op_free(op);
}

static void dma_memcpy_error_cb(struct doca_dma_task_memcpy *task,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	struct memos_initiator_ref_app_dma_op *op = (struct memos_initiator_ref_app_dma_op *)task_user_data.ptr;
	struct doca_task *base = doca_dma_task_memcpy_as_task(task);

	(void)ctx_user_data;
	DOCA_LOG_ERR("DMA to host failed: %s", doca_error_get_descr(doca_task_get_status(base)));
	(void)doca_devemu_nvme_req_complete(op->req, 0, 0, NVME_SC_DATA_XFER_ERROR);
	doca_task_free(base);
	dma_op_free(op);
}

/*
 * Acquire the scratch-slot doca_buf for @op. @with_data marks the slot's bytes as populated, which
 * a DMA source requires and a DMA destination must not have.
 */
static doca_error_t dma_acquire_scratch_buf(struct memos_initiator_ref_app_dma *eng,
					    struct memos_initiator_ref_app_dma_op *op,
					    size_t len,
					    bool with_data)
{
	doca_error_t result;

	result =
		doca_buf_inventory_buf_get_by_addr(eng->buf_inv, eng->scratch_mmap, op->scratch, len, &op->scratch_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to acquire scratch buf: %s", doca_error_get_descr(result));
		return result;
	}
	if (!with_data)
		return DOCA_SUCCESS;

	result = doca_buf_set_data(op->scratch_buf, op->scratch, len);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to set scratch buf data: %s", doca_error_get_descr(result));
	return result;
}

/*
 * Validate an engine and a requested length, then take an op slot. On any failure @req is completed
 * here and NULL is returned.
 *
 * Pool exhaustion completes with a bare INTERNAL_ERROR. Leaving the Do Not Retry bit clear is the
 * point: it tells the host the command may succeed if resubmitted, which turns "more commands in
 * flight than this engine has slots" into backpressure rather than an I/O error.
 */
static struct memos_initiator_ref_app_dma_op *dma_begin(struct memos_initiator_ref_app_resources *app,
							struct memos_initiator_ref_app_dma *eng,
							struct doca_devemu_nvme_req *req,
							size_t len)
{
	struct memos_initiator_ref_app_dma_op *op;

	if (eng == NULL) {
		DOCA_LOG_ERR("DMA engine not initialized");
		(void)doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_DATA_XFER_ERROR);
		return NULL;
	}
	if (len == 0 || len > MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE) {
		DOCA_LOG_ERR("DMA payload length %zu out of range (1..%u)", len, MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE);
		(void)doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INVALID_FIELD);
		return NULL;
	}

	op = dma_op_alloc(eng);
	if (op == NULL) {
		DOCA_LOG_WARN("DMA op pool exhausted (%u slots) -> retryable INTERNAL_ERROR", eng->pool_size);
		(void)doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_INTERNAL_ERROR);
		return NULL;
	}

	op->app = app;
	op->req = req;
	op->len = len;
	return op;
}

/* Build the memcpy task for @op and submit it. On failure @req is completed DATA_XFER_ERROR. */
static doca_error_t dma_submit(struct memos_initiator_ref_app_dma *eng,
			       struct memos_initiator_ref_app_dma_op *op,
			       struct doca_buf *src,
			       struct doca_buf *dst)
{
	union doca_data task_user_data = {0};
	struct doca_devemu_nvme_req *req = op->req;
	struct doca_dma_task_memcpy *task;
	doca_error_t result;

	task_user_data.ptr = op;
	result = doca_dma_task_memcpy_alloc_init(eng->dma, src, dst, task_user_data, &task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA task: %s", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_task_submit(doca_dma_task_memcpy_as_task(task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit DMA task: %s", doca_error_get_descr(result));
		doca_task_free(doca_dma_task_memcpy_as_task(task));
		goto fail;
	}

	return DOCA_SUCCESS;

fail:
	dma_op_free(op);
	(void)doca_devemu_nvme_req_complete(req, 0, 0, NVME_SC_DATA_XFER_ERROR);
	return result;
}

/* Abandon @op after a setup failure, completing its request with @status. */
static doca_error_t dma_abort(struct memos_initiator_ref_app_dma_op *op, uint16_t status, doca_error_t result)
{
	struct doca_devemu_nvme_req *req = op->req;

	dma_op_free(op);
	(void)doca_devemu_nvme_req_complete(req, 0, 0, status);
	return result;
}

static doca_error_t dma_to_host_ex(struct memos_initiator_ref_app_resources *app,
				   struct memos_initiator_ref_app_dma *eng,
				   struct doca_devemu_nvme_req *req,
				   const void *sqe,
				   const void *src,
				   size_t len,
				   uint32_t success_dw0,
				   uint16_t success_status)
{
	struct memos_initiator_ref_app_dma_op *op;
	doca_error_t result;

	op = dma_begin(app, eng, req, len);
	if (op == NULL)
		return DOCA_ERROR_BAD_STATE;

	op->success_status = success_status;
	op->success_dw0 = success_dw0;
	memcpy(op->scratch, src, len);

	result = dma_resolve_prp_host(app, eng, sqe, len, /* host_is_src */ false, op);
	if (result != DOCA_SUCCESS)
		return dma_abort(op, NVME_SC_INVALID_FIELD, result);

	result = dma_acquire_scratch_buf(eng, op, len, true);
	if (result != DOCA_SUCCESS)
		return dma_abort(op, NVME_SC_DATA_XFER_ERROR, result);

	return dma_submit(eng, op, op->scratch_buf, op->host_head);
}

doca_error_t memos_initiator_ref_app_dma_to_host(struct memos_initiator_ref_app_resources *app,
						 struct doca_devemu_nvme_req *req,
						 const void *sqe,
						 const void *src,
						 size_t len,
						 uint16_t success_status)
{
	return dma_to_host_ex(app, app->dma, req, sqe, src, len, 0, success_status);
}

static doca_error_t dma_setup_ctx(struct memos_initiator_ref_app_resources *app,
				  struct memos_initiator_ref_app_dma *eng)
{
	union doca_data ctx_user_data = {0};
	struct doca_ctx *ctx;
	doca_error_t result;

	result = doca_dma_create(app->dev, &eng->dma);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DMA ctx: %s", doca_error_get_descr(result));
		return result;
	}
	ctx = doca_dma_as_ctx(eng->dma);

	result = doca_dma_task_memcpy_set_conf(eng->dma, dma_memcpy_completed_cb, dma_memcpy_error_cb, eng->pool_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure DMA memcpy task: %s", doca_error_get_descr(result));
		return result;
	}

	ctx_user_data.ptr = eng;
	result = doca_ctx_set_user_data(ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DMA ctx user data: %s", doca_error_get_descr(result));
		return result;
	}

	/* A DMA ctx must be drained by the thread that submits to it, so it rides the PE of whichever
	 * handler will use it: admin_pe (main thread) for admin payloads, a worker's io_pe for KV data
	 * transfer. The caller picks; this function just wires it. */
	result = doca_pe_connect_ctx(eng->pe, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect DMA ctx to PE: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DMA ctx: %s", doca_error_get_descr(result));
		return result;
	}
	return DOCA_SUCCESS;
}

static doca_error_t dma_setup_scratch(struct memos_initiator_ref_app_resources *app,
				      struct memos_initiator_ref_app_dma *eng)
{
	size_t scratch_size = (size_t)eng->pool_size * MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE;
	uint32_t i;
	doca_error_t result;

	eng->ops = calloc(eng->pool_size, sizeof(*eng->ops));
	if (eng->ops == NULL) {
		DOCA_LOG_ERR("Failed to allocate %u DMA op slots", eng->pool_size);
		return DOCA_ERROR_NO_MEMORY;
	}

	eng->scratch_base = aligned_alloc(MEMOS_INITIATOR_REF_APP_DMA_PAGE_SIZE, scratch_size);
	if (eng->scratch_base == NULL) {
		DOCA_LOG_ERR("Failed to allocate %zu B DMA scratch", scratch_size);
		return DOCA_ERROR_NO_MEMORY;
	}
	memset(eng->scratch_base, 0, scratch_size);
	for (i = 0; i < eng->pool_size; ++i)
		eng->ops[i].scratch = eng->scratch_base + (size_t)i * MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE;

	result = doca_mmap_create(&eng->scratch_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create scratch mmap: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_mmap_add_dev(eng->scratch_mmap, app->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add dev to scratch mmap: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_mmap_set_memrange(eng->scratch_mmap, eng->scratch_base, scratch_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set scratch mmap range: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_mmap_set_permissions(eng->scratch_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set scratch mmap permissions: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_mmap_start(eng->scratch_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start scratch mmap: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_inventory_create((size_t)eng->pool_size * MEMOS_INITIATOR_REF_APP_DMA_BUFS_PER_OP,
					   &eng->buf_inv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create buf inventory: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_buf_inventory_start(eng->buf_inv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start buf inventory: %s", doca_error_get_descr(result));
		return result;
	}
	return DOCA_SUCCESS;
}

/* Progress the PE this engine's ctx is connected to. The admin engine shares admin_pe with the rest
 * of the control plane, so it goes through progress_all(); a worker engine owns its io_pe alone. */
static int dma_engine_pump(struct memos_initiator_ref_app_resources *app, struct memos_initiator_ref_app_dma *eng)
{
	if (eng->pe == app->admin_pe)
		return memos_initiator_ref_app_progress_all(app);
	return doca_pe_progress(eng->pe);
}

doca_error_t memos_initiator_ref_app_dma_engine_create(struct memos_initiator_ref_app_resources *app,
						       struct doca_pe *pe,
						       uint32_t pool_size,
						       struct memos_initiator_ref_app_dma **out_eng)
{
	struct memos_initiator_ref_app_dma *eng;
	doca_error_t result;

	eng = calloc(1, sizeof(*eng));
	if (eng == NULL) {
		DOCA_LOG_ERR("Failed to allocate the DMA engine state");
		return DOCA_ERROR_NO_MEMORY;
	}
	eng->pe = pe;
	eng->pool_size = pool_size;

	result = dma_setup_ctx(app, eng);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = dma_setup_scratch(app, eng);
	if (result != DOCA_SUCCESS)
		goto fail;

	DOCA_LOG_INFO("DMA engine up: %u scratch slots x %u B, %u bufs",
		      pool_size,
		      MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE,
		      pool_size * MEMOS_INITIATOR_REF_APP_DMA_BUFS_PER_OP);
	*out_eng = eng;
	return DOCA_SUCCESS;

fail:
	(void)memos_initiator_ref_app_dma_engine_destroy(app, eng);
	return result;
}

doca_error_t memos_initiator_ref_app_dma_init(struct memos_initiator_ref_app_resources *app)
{
	return memos_initiator_ref_app_dma_engine_create(app,
							 app->admin_pe,
							 MEMOS_INITIATOR_REF_APP_DMA_ADMIN_POOL_SIZE,
							 &app->dma);
}

doca_error_t memos_initiator_ref_app_dma_teardown(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result = memos_initiator_ref_app_dma_engine_destroy(app, app->dma);

	if (result == DOCA_SUCCESS)
		app->dma = NULL;
	return result;
}

doca_error_t memos_initiator_ref_app_dma_engine_destroy(struct memos_initiator_ref_app_resources *app,
							struct memos_initiator_ref_app_dma *eng)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_DMA_STOP_SLEEP_NS};
	doca_error_t result;

	if (eng == NULL)
		return DOCA_SUCCESS;

	if (eng->dma != NULL) {
		struct doca_ctx *ctx = doca_dma_as_ctx(eng->dma);
		enum doca_ctx_states state;
		uint64_t spins = MEMOS_INITIATOR_REF_APP_DMA_STOP_TIMEOUT_US /
				 (MEMOS_INITIATOR_REF_APP_DMA_STOP_SLEEP_NS / 1000);

		result = doca_ctx_stop(ctx);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to stop DMA ctx: %s", doca_error_get_descr(result));

		while (spins-- > 0) {
			if (doca_ctx_get_state(ctx, &state) != DOCA_SUCCESS || state == DOCA_CTX_STATE_IDLE)
				break;
			/* Pump whichever PE this ctx rides so it reaches IDLE. A worker engine is torn
			 * down after its thread has been joined, so the caller is the sole progressor
			 * of that io_pe here - the same guarantee the main thread has for admin_pe. */
			if (dma_engine_pump(app, eng) == 0) {
				struct timespec ts = sleep_ts;

				(void)nanosleep(&ts, NULL);
			}
		}

		result = doca_dma_destroy(eng->dma);
		if (result != DOCA_SUCCESS) {
			/* A task still in flight holds a pointer into this engine and will write its
			 * operation record when it completes. Releasing the engine now would hand the
			 * library freed memory to complete into, so keep every allocation it can still
			 * reach and leak it deliberately - this runs on the way out of the process. */
			DOCA_LOG_ERR(
				"Failed to destroy DMA ctx (%s); leaving the DMA engine allocated because tasks may still complete into it",
				doca_error_get_descr(result));
			return result;
		}
		eng->dma = NULL;
	}

	if (eng->buf_inv != NULL) {
		result = doca_buf_inventory_destroy(eng->buf_inv);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy buf inventory: %s", doca_error_get_descr(result));
		eng->buf_inv = NULL;
	}

	if (eng->scratch_mmap != NULL) {
		result = doca_mmap_stop(eng->scratch_mmap);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to stop scratch mmap: %s", doca_error_get_descr(result));
		result = doca_mmap_destroy(eng->scratch_mmap);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy scratch mmap: %s", doca_error_get_descr(result));
		eng->scratch_mmap = NULL;
	}

	free(eng->scratch_base);
	eng->scratch_base = NULL;
	free(eng->ops);
	eng->ops = NULL;
	free(eng);

	return DOCA_SUCCESS;
}
