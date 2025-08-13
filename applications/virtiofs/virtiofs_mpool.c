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

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <doca_mmap.h>
#include <doca_log.h>
#include <doca_buf_pool.h>

#include <virtiofs_mpool.h>
#include <virtiofs_core.h>
#include <virtiofs_utils.h>

DOCA_LOG_REGISTER(VIRTIOFS_MPOOL);

#define LOG_MAX_BUF_SIZE 31

struct virtiofs_mpool {
	struct doca_buf_pool *bpool;
	struct doca_mmap *mmap;
	void *memory;
	struct virtiofs_mpool_attr attr;
};

#define HUGE_PAGE_SIZE (2 * 1024 * 1024)

static size_t virtiofs_mpool_hp_align(size_t size)
{
	return (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
}

static void *virtiofs_mpool_hp_malloc(size_t size)
{
	size_t aligned_size = virtiofs_mpool_hp_align(size);
	void *ptr;

	ptr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (ptr == MAP_FAILED) {
		DOCA_LOG_ERR("Failed to allocate huge page memory, err: %s", strerror(errno));
		return NULL;
	}

	return ptr;
}

static void virtiofs_mpool_hp_free(void *ptr, size_t size)
{
	if (munmap(ptr, virtiofs_mpool_hp_align(size)) != 0)
		DOCA_LOG_ERR("Failed to free huge page memory, err: %s", strerror(errno));
}

struct virtiofs_mpool *virtiofs_mpool_create(struct virtiofs_mpool_attr *attr)
{
	struct virtiofs_mpool *mpool;
	enum doca_access_flag permissions;
	doca_error_t err;
	int idx, i;

	mpool = calloc(1, sizeof(*mpool));
	if (mpool == NULL) {
		DOCA_LOG_ERR("Failed to allocate doca mpool");
		goto out;
	}

	mpool->attr = *attr;

	mpool->memory = virtiofs_mpool_hp_malloc(attr->num_bufs * attr->buf_size);
	if (mpool->memory == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for mpool");
		goto mpool_free;
	}

	err = doca_mmap_create(&mpool->mmap);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap, err: %s\n", doca_error_get_name(err));
		goto memory_free;
	}

	err = doca_mmap_set_max_num_devices(mpool->mmap, attr->num_devs);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create set max num_devices for mmap, err: %s\n", doca_error_get_name(err));
		goto mmap_destroy;
	}

	err = doca_mmap_set_memrange(mpool->mmap, mpool->memory, attr->num_bufs * attr->buf_size);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap: set memrange failure, err: %s\n", doca_error_get_name(err));
		goto mmap_destroy;
	}

	permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_WRITE |
		      DOCA_ACCESS_FLAG_PCI_RELAXED_ORDERING;
	err = doca_mmap_set_permissions(mpool->mmap, permissions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap permissions, err: %s\n", doca_error_get_name(err));
		goto mmap_destroy;
	}

	for (idx = 0; idx < attr->num_devs; idx++) {
		err = doca_mmap_add_dev(mpool->mmap, attr->devs[idx]);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add dev to mmap, err: %s\n", doca_error_get_name(err));
			goto dev_rm;
		}
	}

	err = doca_mmap_start(mpool->mmap);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap, err: %s\n", doca_error_get_name(err));
		goto dev_rm;
	}

	err = doca_buf_pool_create(attr->num_bufs, attr->buf_size, mpool->mmap, &mpool->bpool);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create buf pool, err: %s\n", doca_error_get_name(err));
		goto mmap_stop;
	}

	err = doca_buf_pool_start(mpool->bpool);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start buf pool, err: %s\n", doca_error_get_name(err));
		goto bpool_destroy;
	}

	return mpool;

bpool_destroy:
	doca_buf_pool_destroy(mpool->bpool);
mmap_stop:
	doca_mmap_stop(mpool->mmap);
dev_rm:
	for (i = 0; i < idx; i++)
		doca_mmap_rm_dev(mpool->mmap, attr->devs[i]);
mmap_destroy:
	doca_mmap_destroy(mpool->mmap);
memory_free:
	free(mpool->memory);
mpool_free:
	free(mpool);
out:
	return NULL;
}

doca_error_t virtiofs_mpool_buf_get(struct virtiofs_mpool *mpool, struct doca_buf **buf)
{
	return doca_buf_pool_buf_alloc(mpool->bpool, buf);
}

void virtiofs_mpool_buf_put(struct doca_buf *buf)
{
	doca_buf_dec_refcount(buf, NULL);
}

void virtiofs_mpool_destroy(struct virtiofs_mpool *mpool)
{
	doca_buf_pool_stop(mpool->bpool);
	doca_buf_pool_destroy(mpool->bpool);
	doca_mmap_stop(mpool->mmap);
	doca_mmap_destroy(mpool->mmap);
	virtiofs_mpool_hp_free(mpool->memory, mpool->attr.num_bufs * mpool->attr.buf_size);
	free(mpool);
}

struct virtiofs_mpool_set {
	struct virtiofs_mpool *pool_map[LOG_MAX_BUF_SIZE + 1];
	int num_pools;
	struct virtiofs_mpool *mpools[];
};

static int virtiofs_mpool_attr_compare(const void *a, const void *b)
{
	struct virtiofs_mpool_attr *attr_a = (struct virtiofs_mpool_attr *)a;
	struct virtiofs_mpool_attr *attr_b = (struct virtiofs_mpool_attr *)b;

	return (attr_a->buf_size > attr_b->buf_size) - (attr_a->buf_size < attr_b->buf_size);
}

struct virtiofs_mpool_set *virtiofs_mpool_set_create(struct virtiofs_mpool_set_attr *attr)
{
	struct virtiofs_mpool_set *set;
	int i, j, log_buf_size;

	if (!attr || attr->num_pools <= 0 || !attr->mpools) {
		DOCA_LOG_ERR("Invalid arguments");
		return NULL;
	}

	set = calloc(1, sizeof(*set) + sizeof(*set->mpools) * attr->num_pools);
	if (!set)
		return NULL;

	set->num_pools = attr->num_pools;

	qsort(attr->mpools, attr->num_pools, sizeof(struct virtiofs_mpool_attr), virtiofs_mpool_attr_compare);

	for (i = 0; i < set->num_pools; i++) {
		if (attr->mpools[i].buf_size & (attr->mpools[i].buf_size - 1)) {
			DOCA_LOG_ERR("Invalid buffer size %lu: must be power of 2", attr->mpools[i].buf_size);
			goto mpools_destroy;
		}

		log_buf_size = LOG2_FLOOR(attr->mpools[i].buf_size);
		if (log_buf_size > LOG_MAX_BUF_SIZE) {
			DOCA_LOG_ERR("Invalid buffer size %lu: too large", attr->mpools[i].buf_size);
			goto mpools_destroy;
		}

		set->mpools[i] = virtiofs_mpool_create(&attr->mpools[i]);
		if (!set->mpools[i]) {
			DOCA_LOG_ERR("Failed to create mpool");
			goto mpools_destroy;
		}

		for (j = log_buf_size; j >= 0 && !set->pool_map[j]; j--)
			set->pool_map[j] = set->mpools[i];
	}

	return set;

mpools_destroy:
	for (j = 0; j < i; j++)
		virtiofs_mpool_destroy(set->mpools[j]);
	free(set);
	return NULL;
}

doca_error_t virtiofs_mpool_set_buf_get(struct virtiofs_mpool_set *set, size_t size, struct doca_buf **buf)
{
	size_t log_size = LOG2_CEIL(size);

	if (doca_unlikely(log_size > LOG_MAX_BUF_SIZE || !set->pool_map[log_size]))
		return DOCA_ERROR_NO_MEMORY;

	return virtiofs_mpool_buf_get(set->pool_map[log_size], buf);
}

void virtiofs_mpool_set_buf_put(struct doca_buf *buf)
{
	virtiofs_mpool_buf_put(buf);
}

void virtiofs_mpool_set_destroy(struct virtiofs_mpool_set *set)
{
	for (int i = 0; i < set->num_pools; i++)
		virtiofs_mpool_destroy(set->mpools[i]);

	free(set);
}
