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

#ifndef NFS_FSDEV_PUBLIC_H
#define NFS_FSDEV_PUBLIC_H

#include <stdint.h>

/**
 * @brief Opaque handle for the NFS fsdev context.
 */
struct nfs_fsdev;

/**
 * @brief Callback type for NFS fsdev I/O operations.
 * @param [in] context User context pointer.
 * @param [in] status  Status code of the operation.
 */
typedef void (*nfs_fsdev_io_cb)(void *context, int status);

/**
 * @brief Create a new NFS fsdev context.
 * @param [in] server The NFS server.
 * @param [in] mount_point The NFS mount point.
 * @return Pointer to the context, or NULL on failure.
 */
void *nfs_fsdev_create(char *server, char *mount_point);

/**
 * @brief Get the NFS fsdev handle from a context pointer.
 * @param [in] fsdev Context pointer.
 * @return Pointer to the NFS fsdev handle.
 */
struct nfs_fsdev *nfs_fsdev_get(void *fsdev);

/**
 * @brief Submit an I/O operation to the NFS fsdev.
 * @param [in] fsdev_priv   NFS fsdev handle.
 * @param [in] fuse_header  Pointer to the FUSE header.
 * @param [in] fuse_in      Pointer to the FUSE input buffer.
 * @param [out] fuse_out     Pointer to the FUSE output buffer.
 * @param [in] src_domain_id Source domain ID.
 * @param [in] req_id       Request ID.
 * @param [in] app_cb       Application callback.
 * @param [in] app_ctxt     Application context.
 */
void nfs_fsdev_submit(struct nfs_fsdev *fsdev_priv,
		      char *fuse_header,
		      char *cmnd_in_hdr,
		      char *datain,
		      char *fuse_out,
		      char *cmnd_out_hdr,
		      char *dataout,
		      uint16_t src_domain_id,
		      uint64_t req_id,
		      nfs_fsdev_io_cb app_cb,
		      void *app_ctxt);

/**
 * @brief Progress any pending I/O operations for the NFS fsdev.
 * @param [in] fsdev_priv NFS fsdev handle.
 */
void nfs_fsdev_progress(struct nfs_fsdev *fsdev_priv);

#endif /* NFS_FSDEV_PUBLIC_H */
