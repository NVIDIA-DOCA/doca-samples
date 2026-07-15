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

#ifndef NFS_FSDEV_IO_DEVICE_H
#define NFS_FSDEV_IO_DEVICE_H

#include "priv_nfs_fsdev.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nfs_fsdev_context;
/**
 * @brief Allocate and initialize the NFS fsdev context.
 * @param [in] server The NFS server.
 * @param [in] mount_point The NFS mount point.
 * @return Pointer to the context, or NULL on failure.
 */
struct nfs_fsdev_context *create_nfs_fsdev_context(char *server, char *mount_point);

/**
 * @brief Destroy the NFS fsdev context.
 * @param [in] nfs_fsdev_context The NFS fsdev context.
 */
void destroy_nfs_fsdev_context(struct nfs_fsdev_context *nfs_fsdev_context);

/**
 * @brief Get the global context from an NFS fsdev context.
 * @param [in] nfs_fsdev The NFS fsdev context.
 * @return Pointer to the global context.
 */
struct nfs_fsdev_global *get_global_context_nfs_fsdev(struct nfs_fsdev_context *nfs_fsdev_context);

/**
 * @brief Get the private context from an NFS fsdev context.
 * @param [in] nfs_fsdev The NFS fsdev context.
 * @return Pointer to the private context.
 */
struct nfs_fsdev *get_private_context_nfs_fsdev(struct nfs_fsdev_context *nfs_fsdev_context);

#ifdef __cplusplus
}
#endif

#endif /* NFS_FSDEV_IO_DEVICE_H */