/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef WORKER_SANDBOX_H_
#define WORKER_SANDBOX_H_

#include <ucp/api/ucp.h>
#include <ucs/datastruct/khash.h>
#include <ucs/datastruct/list.h>

#include <doca_error.h>
#include <doca_urom_plugin.h>

/* UROM worker sandbox context */
struct urom_worker_sandbox;

/* UROM sandbox worker interface */
struct urom_worker_sandbox_iface {
	struct urom_plugin_iface super; /* DOCA UROM worker plugin interface */
};

/* Sandbox command request structure */
struct urom_worker_sandbox_request {
	ucs_list_link_t entry;			    /* UCX list entry */
	struct urom_worker_sandbox *sandbox_worker; /* Worker sandbox context */
	struct urom_worker_notif_desc *notif_desc;  /* Worker sandbox notification descriptor */
	int inline_data;			    /* If notification contains inline data */
};

/* Init endpoints UCX map */
KHASH_MAP_INIT_INT64(ep, ucp_ep_h);

/* Sandbox UCP data structure */
struct urom_worker_sandbox_ucp_data {
	ucp_context_h ucp_context;     /* UCP context */
	ucp_worker_h ucp_worker;       /* UCP worker instance */
	ucp_address_t *worker_address; /* UCP worker address */
	size_t ucp_addrlen;	       /* UCP worker address length */
	khash_t(ep) * eps;	       /* Worker endpoints map */
};

/* UROM worker sandbox context */
struct urom_worker_sandbox {
	struct urom_worker_sandbox_ucp_data ucp_data; /* Sandbox UCP data */
	ucs_list_link_t completed_reqs;		      /* Sandbox worker commands completion list */
};

/*
 * Get DOCA worker plugin interface for sandbox plugin, DOCA UROM worker will load the urom_plugin_get_iface symbol
 * to get the sandbox interface
 *
 * @iface [out]: Set DOCA UROM plugin interface for sandbox
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_plugin_get_iface(struct urom_plugin_iface *iface);

/*
 * Get sandbox plugin version, will be used to verify that the host and DPU plugin versions are compatible
 *
 * @version [out]: Set the sandbox worker plugin version
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t urom_plugin_get_version(uint64_t *version);

#endif /* WORKER_SANDBOX_H_ */
