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

#ifndef VIRTIOFS_MANAGER_H
#define VIRTIOFS_MANAGER_H

#include <sys/queue.h>
#include <stdbool.h>
#include <doca_dev.h>
#include <doca_types.h>
#include <stdint.h>
#include <doca_buf.h>
#include <doca_devemu_vfs_fuse_kernel.h>

#include "virtiofs_utils.h"
#include "virtiofs_thread.h"
#include "virtiofs_core.h"
#include "virtiofs_device.h"

#define VIRTIOFS_MANAGER_NAME_LENGTH 128 /* Manager name length */

/* VirtioFS PCI emulation function */
struct virtiofs_function {
	struct doca_dev_rep *rep;		      /* Device representation */
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE];	      /* Device unique id */
	char pci_buf[DOCA_DEVINFO_REP_PCI_ADDR_SIZE]; /* PCI address */
	enum doca_pci_func_type pci_function_type;    /* PCI function type */
	uint16_t pf_index;			      /* PF index */
	uint8_t is_hotplugged;			      /* Hotplugged flag */
	STAILQ_ENTRY(virtiofs_function) entry;	      /* List entry */
};

/* VirtioFS manager */
struct virtiofs_manager {
	char name[VIRTIOFS_MANAGER_NAME_LENGTH]; /* Manager name */
	struct doca_dev *dev;			 /* Device */
	struct doca_devemu_vfs_type *vfs_type;	 /* VFS type */
	STAILQ_HEAD(, virtiofs_function) funcs;	 /* List of functions */
	uint8_t hotplug_supported;		 /* Hotplug supported flag */
	SLIST_ENTRY(virtiofs_manager) entry;	 /* List entry */
};

/*
 * Get available function
 *
 * @param ctx [in]: VirtioFS resources
 * @param func_out [out]: Function
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_function_get_available(struct virtiofs_resources *ctx, struct virtiofs_function **func_out);

/*
 * Create managers
 *
 * @param ctx [in]: VirtioFS resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t virtiofs_managers_create(struct virtiofs_resources *ctx);

/*
 * Destroy managers
 *
 * @param ctx [in]: VirtioFS resources
 */
void virtiofs_managers_destroy(struct virtiofs_resources *ctx);

/*
 * Get manager by name
 *
 * @param ctx [in]: VirtioFS resources
 * @param name [in]: Manager name
 * @return: Manager on success and NULL otherwise
 */
struct virtiofs_manager *virtiofs_manager_get_by_name(struct virtiofs_resources *ctx, char *name);

#endif /* VIRTIOFS_MANAGER_H */
