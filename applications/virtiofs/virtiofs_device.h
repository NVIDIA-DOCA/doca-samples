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

#ifndef VIRTIOFS_DEVICE_H
#define VIRTIOFS_DEVICE_H

#include <sys/queue.h>
#include <stdbool.h>
#include <doca_types.h>
#include <stdint.h>
#include <doca_buf.h>
#include <doca_devemu_vfs_fuse_kernel.h>

#include "virtiofs_utils.h"
#include "virtiofs_thread.h"
#include "virtiofs_core.h"

#define VIRTIOFS_FSDEV_NAME_LENGTH 128	    /* Fsdev name length */
#define VIRTIOFS_DEVICE_NAME_MAX_LENGTH 255 /* Device name max length */
#define VIRTIOFS_DEVICE_TAG_MAX_LENGTH 36   /* Device tag max length */

/* VirtioFS callback */
typedef void (*virtiofs_cb_t)(void *cb_arg, doca_error_t status);

/* VirtioFS fsdev io callback */
typedef void (*vfs_doca_fsdev_io_cb)(void *app_ctxt, int status);

/* VirtioFS nfs fsdev io callback */
typedef void (*nfs_fsdev_io_cb)(void *app_ctxt, int status);

/* VirtioFS device state */
enum virtiofs_device_state {
	VIRTIOFS_DEVICE_STATE_STOPPED,	/* Stopped */
	VIRTIOFS_DEVICE_STATE_STARTING, /* Starting */
	VIRTIOFS_DEVICE_STATE_STARTED,	/* Started */
	VIRTIOFS_DEVICE_STATE_STOPPING, /* Stopping */
};

/* VirtioFS device config */
struct virtiofs_device_config {
	char name[VIRTIOFS_DEVICE_NAME_MAX_LENGTH + 1]; /* Device name */
	char tag[VIRTIOFS_DEVICE_TAG_MAX_LENGTH + 1];	/* Device tag */
	uint16_t queue_size;				/* Queue size */
	uint16_t num_request_queues;			/* Number of request queues */
};

/* VirtioFS device io context */
struct virtiofs_device_io_ctx {
	struct virtiofs_device *dev;		      /* VirtioFS device */
	struct virtiofs_thread *thread;		      /* VirtioFS thread */
	struct doca_dma *dma_ctx;		      /* DMA context */
	struct doca_devemu_vfs_io *vfs_io;	      /* VirtioFS vfs io */
	struct virtiofs_mpool_set *mpool_set;	      /* VirtioFS mpool set */
	struct virtiofs_fsdev_thread_ctx *fsdev_tctx; /* VirtioFS fsdev thread context */
	uint32_t reqs_avail;			      /* Number of available requests */
	TAILQ_HEAD(, virtiofs_request) pending;
} CL_ALIGNED;

/* VirtioFS device */
struct virtiofs_device {
	struct virtiofs_resources *ctx;	       /* VirtioFS resources */
	struct virtiofs_manager *manager;      /* VirtioFS manager */
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE]; /* VirtioFS device unique id */
	struct virtiofs_device_config config;  /* VirtioFS device config */
	SLIST_ENTRY(virtiofs_device) entry;    /* Entry */
	struct doca_dev_rep *dev_rep;	       /* Device representation */
	struct doca_devemu_vfs_dev *vfs_dev;   /* VirtioFS device */
	struct virtiofs_fsdev *fsdev;	       /* VirtioFS fsdev */
	virtiofs_cb_t start_cb;		       /* Start callback */
	void *start_cb_arg;		       /* Start callback argument */
	virtiofs_cb_t stop_cb;		       /* Stop callback */
	void *stop_cb_arg;		       /* Stop callback argument */
	int num_threads;		       /* Number of threads */
	enum virtiofs_device_state state;
	bool skip_rw;
	struct virtiofs_device_io_ctx *io_ctxs[];
};

/* VirtioFS fsdev ops */
struct virtiofs_fsdev_ops {
	doca_error_t (*destroy)(struct virtiofs_fsdev *fsdev);
	struct virtiofs_fsdev_thread_ctx *(*thread_ctx_get)(struct virtiofs_fsdev *fsdev);
	doca_error_t (*thread_ctx_put)(struct virtiofs_fsdev_thread_ctx *tctx);
	void (*submit)(struct virtiofs_fsdev_thread_ctx *tctx, /* VirtioFS fsdev thread context */
		       char *fuse_header,		       /* Fuse header */
		       char *cmnd_in_hdr,		       /* Command in header */
		       char *datain,			       /* Data in */
		       char *fuse_out,			       /* Fuse out */
		       char *cmnd_out_hdr,		       /* Command out header */
		       char *dataout,			       /* Data out */
		       uint16_t src_domain_id,		       /* Source domain id */
		       uint64_t req_id,			       /* Request id */
		       vfs_doca_fsdev_io_cb app_cb,	       /* Application callback */
		       void *app_ctxt);			       /* Application context */
};

/* VirtioFS fsdev thread context */
struct virtiofs_fsdev_thread_ctx {
	struct virtiofs_fsdev *fsdev;		      /* Fsdev */
	SLIST_ENTRY(virtiofs_fsdev_thread_ctx) entry; /* Entry */
};

/* VirtioFS fsdev */
struct virtiofs_fsdev {
	struct virtiofs_resources *ctx;			     /* VirtioFS resources */
	struct virtiofs_fsdev_ops *ops;			     /* VirtioFS fsdev ops */
	SLIST_ENTRY(virtiofs_fsdev) entry;		     /* Entry */
	char name[VIRTIOFS_FSDEV_NAME_LENGTH];		     /* Fsdev name */
	pthread_mutex_t lock;				     /* Lock */
	SLIST_HEAD(, virtiofs_fsdev_thread_ctx) thread_ctxs; /* VirtioFS fsdev thread contexts */
};

/*
 * Create static devices
 *
 * @param ctx [in]: VirtioFS resources
 * @param nfs_server [in]: NFS server
 * @param nfs_export [in]: NFS export
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_device_create_static(struct virtiofs_resources *ctx, char *nfs_server, char *nfs_export);

/*
 * Create devices
 *
 * @param ctx [in]: VirtioFS resources
 * @param config [in]: VirtioFS device config
 * @param manager [in]: Manager name
 * @param vuid [in]: VirtioFS device unique id
 * @param fsdev [in]: VirtioFS fsdev
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_device_create(struct virtiofs_resources *ctx,
				    struct virtiofs_device_config *config,
				    char *manager,
				    char *vuid,
				    char *fsdev);

/*
 * Start device
 *
 * @param ctx [in]: VirtioFS resources
 * @param dev_name [in]: Device name
 * @param cb [in]: Callback
 * @param cb_arg [in]: Callback argument
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_device_start(struct virtiofs_resources *ctx, char *dev_name, virtiofs_cb_t cb, void *cb_arg);

/*
 * Stop device
 *
 * @param ctx [in]: VirtioFS resources
 * @param dev_name [in]: Device name
 * @param cb [in]: Callback
 * @param cb_arg [in]: Callback argument
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_device_stop(struct virtiofs_resources *ctx, char *dev_name, virtiofs_cb_t cb, void *cb_arg);

/*
 * Destroy device
 *
 * @param ctx [in]: VirtioFS resources
 * @param name [in]: Device name
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_device_destroy(struct virtiofs_resources *ctx, char *name);

/*
 * Create NFS fsdev
 *
 * @param ctx [in]: VirtioFS resources
 * @param name [in]: Fsdev name
 * @param server [in]: NFS server
 * @param mount_point [in]: NFS mount point
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_nfs_fsdev_create(struct virtiofs_resources *ctx, char *name, char *server, char *mount_point);

/*
 * Initialize fsdev
 *
 * @param ctx [in]: VirtioFS resources
 * @param fsdev [in]: VirtioFS fsdev
 * @param name [in]: Fsdev name
 * @param ops [in]: Fsdev ops
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_fsdev_init(struct virtiofs_resources *ctx,
				 struct virtiofs_fsdev *fsdev,
				 char *name,
				 struct virtiofs_fsdev_ops *ops);

/*
 * Destroy fsdev
 *
 * @param fsdev [in]: VirtioFS fsdev
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_fsdev_destroy(struct virtiofs_fsdev *fsdev);

/*
 * Initialize fsdev thread context
 *
 * @param fsdev [in]: VirtioFS fsdev
 * @param tctx [in]: VirtioFS fsdev thread context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_fsdev_thread_ctx_init(struct virtiofs_fsdev *fsdev, struct virtiofs_fsdev_thread_ctx *tctx);

/*
 * Destroy fsdev thread context
 *
 * @param tctx [in]: VirtioFS fsdev thread context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_fsdev_thread_ctx_destroy(struct virtiofs_fsdev_thread_ctx *tctx);

/*
 * Get fsdev by name
 *
 * @param ctx [in]: VirtioFS resources
 * @param name [in]: Fsdev name
 */
struct virtiofs_fsdev *virtiofs_fsdev_get_by_name(struct virtiofs_resources *ctx, char *name);

#endif /* VIRTIOFS_DEVICE_H */
