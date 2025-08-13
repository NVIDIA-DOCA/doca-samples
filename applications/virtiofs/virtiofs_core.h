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

#ifndef VIRTIOFS_CORE_H
#define VIRTIOFS_CORE_H

#include <sys/queue.h>
#include <stdbool.h>
#include <stdint.h>
#include <doca_buf.h>
#include <doca_devemu_vfs_fuse_kernel.h>

#include "virtiofs_utils.h"
#include "virtiofs_thread.h"
#include "virtiofs_manager.h"
#include "virtiofs_device.h"

#define VIRTIOFS_MANAGER_NAME_LENGTH 128	/* Manager name length */
#define VIRTIOFS_CORE_MASK_SIZE 16		/* Core mask size */
#define VIRTIOFS_NFS_SERVER_SIZE 36		/* NFS server size */
#define VIRTIOFS_NFS_EXPORT_SIZE 36		/* NFS export size */
#define VIRTIOFS_CORE_MASK_DEFAULT "0x1"	/* Default core mask */
#define VIRTIOFS_NFS_SERVER_DEFAULT "localhost" /* Default NFS server */
#define VIRTIOFS_NFS_EXPORT_DEFAULT "/VIRTUAL"	/* Default NFS export */

/* VirtioFS callback */
typedef void (*virtiofs_cb_t)(void *cb_arg, doca_error_t status);

/* VirtioFS fsdev io callback */
typedef void (*vfs_doca_fsdev_io_cb)(void *app_ctxt, int status);

/* VirtioFS application configuration */
struct virtiofs_cfg {
	char core_mask[VIRTIOFS_CORE_MASK_SIZE];   /* Core mask */
	char nfs_server[VIRTIOFS_NFS_SERVER_SIZE]; /* NFS server */
	char nfs_export[VIRTIOFS_NFS_EXPORT_SIZE]; /* NFS export */
};

/* VirtioFS resources */
struct virtiofs_resources {
	SLIST_HEAD(, virtiofs_manager) managers; /* List of managers */
	SLIST_HEAD(, virtiofs_device) devices;	 /* List of devices */
	SLIST_HEAD(, virtiofs_fsdev) fsdevs;	 /* List of fsdevs */
	int num_managers;			 /* Number of managers */
	int num_threads;			 /* Number of threads */
	int num_devices;			 /* Number of devices */
	struct virtiofs_thread_ctx threads[];	 /* Threads */
};

/*
 * Create VirtioFS resources
 *
 * @param core_mask [in]: Core mask
 * @return: VirtioFS resources
 */
struct virtiofs_resources *virtiofs_create(uint32_t core_mask);

/*
 * Start VirtioFS resources
 *
 * @param ctx [in]: VirtioFS resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_UNKNOWN otherwise
 */
doca_error_t virtiofs_start(struct virtiofs_resources *ctx);

/*
 * Stop VirtioFS resources
 *
 * @param ctx [in]: VirtioFS resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_UNKNOWN otherwise
 */
doca_error_t virtiofs_stop(struct virtiofs_resources *ctx);

/*
 * Destroy VirtioFS resources
 *
 * @param ctx [in]: VirtioFS resources
 */
void virtiofs_destroy(struct virtiofs_resources *ctx);

#endif /* VIRTIOFS_CORE_H */
