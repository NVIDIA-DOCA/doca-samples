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

#ifndef VIRTIOFS_THREAD_H
#define VIRTIOFS_THREAD_H

#include <sys/queue.h>
#include <stdbool.h>
#include <doca_types.h>
#include <doca_dev.h>
#include <doca_devemu_vfs_fuse_kernel.h>
#include <virtiofs_utils.h>

#define VIRTIOFS_MPOOL_BUF_SIZE 8192	 /* Mpool buffer size */
#define VIRTIOFS_FSDEV_NAME_LENGTH 128	 /* Fsdev name length */
#define VIRTIOFS_MANAGER_NAME_LENGTH 128 /* Manager name length */

/* Thread poller function */
typedef void (*virtiofs_thread_poller_fn)(void *arg);

/* Thread poller */
struct virtiofs_thread_poller {
	virtiofs_thread_poller_fn fn;		   /* Poller function */
	void *arg;				   /* Poller argument */
	SLIST_ENTRY(virtiofs_thread_poller) entry; /* List entry of pollers */
};

/* Thread attributes */
struct virtiofs_thread_attr {
	struct virtiofs_resources *ctx; /* VirtioFS resources */
	uint32_t max_inflights;		/* Maximum IO inflights */
	int core_id;			/* Core ID of the thread */
	bool admin_thread;		/* Admin thread flag */
};

/* VirtioFS thread */
struct virtiofs_thread {
	struct virtiofs_thread_attr attr;	      /* Thread attributes */
	pthread_t pthread;			      /* Thread pthread pointer */
	struct doca_pe *io_pe;			      /* IO DOCA processing engine */
	struct doca_pe *dma_pe;			      /* DMA DOCA processing engine */
	struct doca_pe *admin_pe;		      /* Admin DOCA processing engine */
	uint64_t next_admin_poll;		      /* Next admin poll */
	SLIST_HEAD(, virtiofs_thread_poller) pollers; /* List of pollers */
	pthread_mutex_t lock;			      /* Lock */
	uint32_t curr_inflights;		      /* Current IO inflights */
	volatile bool suspend;			      /* Suspend flag */
	volatile bool stop;			      /* Stop flag */
} CL_ALIGNED;

/* VirtioFS thread context */
struct virtiofs_thread_ctx {
	struct virtiofs_thread *thread;	      /* VirtioFS thread */
	struct virtiofs_mpool_set *mpool_set; /* VirtioFS mpool set */
};

/* VirtioFS thread execution function */
typedef doca_error_t (*virtiofs_thread_exec_fn_t)(struct virtiofs_thread *thread, void *cb_arg);

/*
 * VirtioFS thread create
 *
 * @param attr [in]: VirtioFS thread attributes
 * @param thread [out]: VirtioFS thread
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_thread_create(struct virtiofs_thread_attr *attr, struct virtiofs_thread **thread);

/*
 * VirtioFS thread start
 *
 * @param thread [in]: VirtioFS thread
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_thread_start(struct virtiofs_thread *thread);

/*
 * VirtioFS thread execute
 *
 * @param thread [in]: VirtioFS thread
 * @param fn [in]: VirtioFS thread execution function
 * @param arg [in]: VirtioFS thread execution argument
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_thread_exec(struct virtiofs_thread *thread, virtiofs_thread_exec_fn_t fn, void *arg);

/*
 * VirtioFS thread poller add
 *
 * @param thread [in]: VirtioFS thread
 * @param fn [in]: VirtioFS thread poller function
 * @param arg [in]: VirtioFS thread poller argument
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_thread_poller_add(struct virtiofs_thread *thread, virtiofs_thread_poller_fn fn, void *arg);

/*
 * VirtioFS thread poller remove
 *
 * @param thread [in]: VirtioFS thread
 * @param fn [in]: VirtioFS thread poller function
 * @param arg [in]: VirtioFS thread poller argument
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_thread_poller_remove(struct virtiofs_thread *thread, virtiofs_thread_poller_fn fn, void *arg);

/*
 * VirtioFS thread get
 *
 * @param ctx [in]: VirtioFS resources
 * @return: VirtioFS thread
 */
struct virtiofs_thread *virtiofs_thread_get(struct virtiofs_resources *ctx);

/*
 * VirtioFS thread stop
 *
 * @param thread [in]: VirtioFS thread
 */
void virtiofs_thread_stop(struct virtiofs_thread *thread);

/*
 * VirtioFS thread destroy
 *
 * @param thread [in]: VirtioFS thread
 */
void virtiofs_thread_destroy(struct virtiofs_thread *thread);

#endif /* VIRTIOFS_THREAD_H */
