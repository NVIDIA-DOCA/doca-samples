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

#ifndef VIRTIOFS_MPOOL_H
#define VIRTIOFS_MPOOL_H

#include <errno.h>
#include <stdint.h>
#include <doca_buf.h>

#define VIRTIOFS_MPOOL_BUF_SIZE 8192 /* Mpool buffer size */

/* VirtioFS mpool attributes */
struct virtiofs_mpool_attr {
	size_t buf_size;	/* Buffer size */
	int num_bufs;		/* Number of buffers */
	struct doca_dev **devs; /* Devices */
	int num_devs;		/* Number of devices */
};

/* VirtioFS mpool set attributes */
struct virtiofs_mpool_set_attr {
	struct virtiofs_mpool_attr *mpools; /* Mpools */
	int num_pools;			    /* Number of pools */
};

/*
 * Create mpool
 *
 * @param attr [in]: Mpool attributes
 * @return: Mpool on success and NULL otherwise
 */
struct virtiofs_mpool *virtiofs_mpool_create(struct virtiofs_mpool_attr *attr);

/*
 * Get buffer from mpool
 *
 * @param mpool [in]: Mpool
 * @param buf [out]: Buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_mpool_buf_get(struct virtiofs_mpool *mpool, struct doca_buf **buf);

/*
 * Put buffer to mpool
 *
 * @param buf [in]: Buffer
 */
void virtiofs_mpool_buf_put(struct doca_buf *buf);

/*
 * Destroy mpool
 *
 * @param mpool [in]: Mpool
 */
void virtiofs_mpool_destroy(struct virtiofs_mpool *mpool);

/*
 * Create mpool set
 *
 * @param attr [in]: Mpool set attributes
 * @return: Mpool set on success and NULL otherwise
 */
struct virtiofs_mpool_set *virtiofs_mpool_set_create(struct virtiofs_mpool_set_attr *attr);

/*
 * Get buffer from mpool set
 *
 * @param set [in]: Mpool set
 * @param size [in]: Buffer size
 * @param buf [out]: Buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_mpool_set_buf_get(struct virtiofs_mpool_set *set, size_t size, struct doca_buf **buf);

/*
 * Put buffer to mpool set
 *
 * @param buf [in]: Buffer
 */
void virtiofs_mpool_set_buf_put(struct doca_buf *buf);

/*
 * Destroy mpool set
 *
 * @param set [in]: Mpool set
 */
void virtiofs_mpool_set_destroy(struct virtiofs_mpool_set *set);

#endif /* VIRTIOFS_MPOOL_H */
