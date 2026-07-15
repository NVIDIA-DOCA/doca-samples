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

#include "nfs_fsdev_gpt.hpp"
#include "nfs_fsdev_io_device.h"
#include "nfs_fsdev.h"

extern "C" {

struct nfs_fsdev_context *create_nfs_fsdev_context(char *server, char *mount_point)
{
	return (struct nfs_fsdev_context *)(new IoDevice<nfs_fsdev_global, nfs_fsdev>(server, mount_point));
}
void destroy_nfs_fsdev_context(struct nfs_fsdev_context *nfs_fsdev_context)
{
	delete (IoDevice<nfs_fsdev_global, nfs_fsdev> *)nfs_fsdev_context;
}

struct nfs_fsdev_global *get_global_context_nfs_fsdev(struct nfs_fsdev_context *nfs_fsdev_context)
{
	IoDevice<nfs_fsdev_global, nfs_fsdev> *io_device = (IoDevice<nfs_fsdev_global, nfs_fsdev> *)nfs_fsdev_context;
	return &(io_device->global_data);
}

struct nfs_fsdev *get_private_context_nfs_fsdev(struct nfs_fsdev_context *nfs_fsdev_context)
{
	IoDevice<nfs_fsdev_global, nfs_fsdev> *io_device = (IoDevice<nfs_fsdev_global, nfs_fsdev> *)nfs_fsdev_context;
	return &(io_device->getPerThreadData());
}
}
