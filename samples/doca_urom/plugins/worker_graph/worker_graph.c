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

#include <stdlib.h>
#include <string.h>

#include <doca_log.h>
#include <doca_compat.h>

#include "urom_graph.h"
#include "worker_graph.h"

DOCA_LOG_REGISTER(UROM::WORKER::GRAPH);

static uint64_t plugin_version = 0x01; /* Graph plugin DPU version */

/*
 * Close graph worker plugin
 *
 * @worker_ctx [in]: DOCA UROM worker context
 */
static void urom_worker_graph_close(struct urom_worker_ctx *worker_ctx)
{
	struct urom_worker_graph *graph_worker = worker_ctx->plugin_ctx;

	if (graph_worker == NULL)
		return;

	ucp_worker_release_address(graph_worker->ucp_data.ucp_worker, graph_worker->ucp_data.worker_address);
	ucp_worker_destroy(graph_worker->ucp_data.ucp_worker);
	ucp_cleanup(graph_worker->ucp_data.ucp_context);
	free(graph_worker);
}

/*
 * Open graph worker plugin
 *
 * @ctx [in]: DOCA UROM worker context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_graph_open(struct urom_worker_ctx *ctx)
{
	struct urom_worker_graph *graph_worker;
	ucp_worker_params_t worker_params;
	ucp_params_t ucp_params;
	ucp_config_t *ucp_config;
	ucp_context_h ucp_context;
	ucp_worker_h ucp_worker;
	ucs_status_t status;

	graph_worker = malloc(sizeof(*graph_worker));
	if (graph_worker == NULL)
		return DOCA_ERROR_NO_MEMORY;

	status = ucp_config_read(NULL, NULL, &ucp_config);
	if (status != UCS_OK)
		goto err_cfg;

	/* Avoid SM when using XGVMI */
	status = ucp_config_modify(ucp_config, "TLS", "^sm");
	if (status != UCS_OK)
		goto err_cfg;

		/* Use TCP for CM, but do not allow TCP for RDMA when using xGVMI */
#if UCP_API_VERSION >= UCP_VERSION(1, 17)
	status = ucp_config_modify(ucp_config, "TCP_PUT_ENABLE", "n");
#else
	status = ucp_config_modify(ucp_config, "PUT_ENABLE", "n");
#endif
	if (status != UCS_OK)
		goto err_cfg;

	ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
	ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_RMA | UCP_FEATURE_AMO64 | UCP_FEATURE_EXPORTED_MEMH;
	ucp_params.features |= UCP_FEATURE_WAKEUP;

	status = ucp_init(&ucp_params, ucp_config, &ucp_context);
	ucp_config_release(ucp_config);
	if (status != UCS_OK)
		goto err_cfg;

	worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
	worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
	status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
	if (status != UCS_OK)
		goto err_worker_create;

	graph_worker->ucp_data.worker_address = NULL;
	status = ucp_worker_get_address(ucp_worker,
					&graph_worker->ucp_data.worker_address,
					&graph_worker->ucp_data.ucp_addrlen);
	if (status != UCS_OK)
		goto err_worker_address;

	DOCA_LOG_DBG("Worker addr length: %lu", graph_worker->ucp_data.ucp_addrlen);

	graph_worker->ucp_data.ucp_context = ucp_context;
	graph_worker->ucp_data.ucp_worker = ucp_worker;

	ucs_list_head_init(&graph_worker->completed_reqs);
	ctx->plugin_ctx = graph_worker;
	return DOCA_SUCCESS;

err_worker_address:
	ucp_worker_destroy(ucp_worker);
err_worker_create:
	ucp_cleanup(ucp_context);
err_cfg:
	free(graph_worker);
	return DOCA_ERROR_NOT_FOUND;
}

/*
 * Unpacking graph worker command
 *
 * @packed_cmd [in]: packed worker command
 * @packed_cmd_len [in]: packed worker command length
 * @cmd [out]: set unpacked UROM worker command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_graph_cmd_unpack(void *packed_cmd, size_t packed_cmd_len, struct urom_worker_cmd **cmd)
{
	if (packed_cmd_len < sizeof(struct urom_worker_graph_cmd)) {
		DOCA_LOG_INFO("Invalid packed command length");
		return DOCA_ERROR_INVALID_VALUE;
	}

	*cmd = packed_cmd;

	if ((*cmd)->len != sizeof(struct urom_worker_graph_cmd)) {
		DOCA_LOG_ERR("Received invalid graph command size");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Handle loopback graph command
 *
 * @ctx [in]: DOCA UROM worker context
 * @cmd_desc [in]: graph command descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t graph_loopback_cmd(struct urom_worker_ctx *ctx, struct urom_worker_cmd_desc *cmd_desc)
{
	struct urom_worker_graph *graph_worker = ctx->plugin_ctx;
	struct urom_worker_notify *notif;
	struct urom_worker_notif_desc *notif_desc;
	struct urom_worker_notify_graph *graph_notif;
	struct urom_worker_graph_cmd *graph_cmd;

	graph_cmd = (struct urom_worker_graph_cmd *)&cmd_desc->worker_cmd.plugin_cmd;
	notif_desc = malloc(sizeof(*notif_desc) + sizeof(*graph_notif));
	if (notif_desc == NULL)
		return DOCA_ERROR_NO_MEMORY;

	notif_desc->dest_id = cmd_desc->dest_id;

	notif = &notif_desc->worker_notif;
	notif->type = cmd_desc->worker_cmd.type;
	notif->status = DOCA_SUCCESS;
	notif->urom_context = cmd_desc->worker_cmd.urom_context;
	notif->len = sizeof(*graph_notif);

	graph_notif = (struct urom_worker_notify_graph *)notif->plugin_notif;
	graph_notif->type = UROM_WORKER_NOTIFY_GRAPH_LOOPBACK;
	graph_notif->loopback.data = graph_cmd->loopback.data;

	ucs_list_add_tail(&graph_worker->completed_reqs, &notif_desc->entry);
	return DOCA_SUCCESS;
}

/*
 * Handle UROM graph worker commands function
 *
 * @ctx [in]: DOCA UROM worker context
 * @cmd_list [in]: command descriptor list to handle
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_graph_worker_cmd(struct urom_worker_ctx *ctx, ucs_list_link_t *cmd_list)
{
	struct urom_worker_cmd *cmd;
	struct urom_worker_cmd_desc *cmd_desc;
	struct urom_worker_graph_cmd *graph_cmd;
	doca_error_t status = DOCA_SUCCESS;

	while (!ucs_list_is_empty(cmd_list)) {
		cmd_desc = ucs_list_extract_head(cmd_list, struct urom_worker_cmd_desc, entry);

		status = urom_worker_graph_cmd_unpack(&cmd_desc->worker_cmd, cmd_desc->worker_cmd.len, &cmd);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to unpack command");
			free(cmd_desc);
			return status;
		}
		graph_cmd = (struct urom_worker_graph_cmd *)cmd_desc->worker_cmd.plugin_cmd;
		switch (graph_cmd->type) {
		case UROM_WORKER_NOTIFY_GRAPH_LOOPBACK:
			status = graph_loopback_cmd(ctx, cmd_desc);
			break;
		default:
			DOCA_LOG_INFO("Invalid GRAPH command type: %lu", graph_cmd->type);
			free(cmd_desc);
			break;
		}
		free(cmd_desc);
		if (status != DOCA_SUCCESS)
			return status;
	}

	return status;
}

/*
 * Get graph worker address
 *
 * UROM worker calls the function twice, first one to get address length and second one to get address data
 *
 * @ctx [in]: DOCA UROM worker context
 * @addr [out]: set worker address
 * @addr_len [out]: set worker address length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_graph_addr(struct urom_worker_ctx *ctx, void *addr, uint64_t *addr_len)
{
	struct urom_worker_graph *graph_worker = (struct urom_worker_graph *)ctx->plugin_ctx;

	/* Always return address size */
	if (*addr_len < graph_worker->ucp_data.ucp_addrlen) {
		/* Return required buffer size on error */
		*addr_len = graph_worker->ucp_data.ucp_addrlen;
		return DOCA_ERROR_INVALID_VALUE;
	}

	*addr_len = graph_worker->ucp_data.ucp_addrlen;
	memcpy(addr, graph_worker->ucp_data.worker_address, *addr_len);

	return DOCA_SUCCESS;
}

/*
 * Check graph worker tasks progress to get notifications
 *
 * @ctx [in]: DOCA UROM worker context
 * @notif_list [out]: set notification descriptors for completed tasks
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_graph_progress(struct urom_worker_ctx *ctx, ucs_list_link_t *notif_list)
{
	struct urom_worker_graph *graph_worker = (struct urom_worker_graph *)ctx->plugin_ctx;
	struct urom_worker_notif_desc *nd;

	ucp_worker_progress(graph_worker->ucp_data.ucp_worker);

	if (ucs_list_is_empty(&graph_worker->completed_reqs))
		return DOCA_ERROR_EMPTY;

	while (!ucs_list_is_empty(&graph_worker->completed_reqs)) {
		nd = ucs_list_extract_head(&graph_worker->completed_reqs, struct urom_worker_notif_desc, entry);

		ucs_list_add_tail(notif_list, &nd->entry);
	}

	return DOCA_SUCCESS;
}

/*
 * Packing graph notification
 *
 * @notif [in]: graph notification to pack
 * @packed_notif_len [in/out]: set packed notification command buffer size
 * @packed_notif [out]: set packed notification command buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_graph_notif_pack(struct urom_worker_notify *notif,
						 size_t *packed_notif_len,
						 void *packed_notif)
{
	size_t pack_len;
	void *pack_head = packed_notif;

	/* pack base command */
	pack_len = ucs_offsetof(struct urom_worker_notify, plugin_notif) + sizeof(struct urom_worker_notify_graph);
	if (pack_len > *packed_notif_len) {
		DOCA_LOG_ERR("Notification pack length is greater than packed buffer length");
		return DOCA_ERROR_INITIALIZATION;
	}

	memcpy(pack_head, notif, pack_len);
	*packed_notif_len = pack_len;

	return DOCA_SUCCESS;
}

/* Define UROM graph plugin interface, set plugin functions */
static struct urom_worker_graph_iface urom_worker_graph = {
	.super.open = urom_worker_graph_open,
	.super.close = urom_worker_graph_close,
	.super.addr = urom_worker_graph_addr,
	.super.worker_cmd = urom_worker_graph_worker_cmd,
	.super.progress = urom_worker_graph_progress,
	.super.notif_pack = urom_worker_graph_notif_pack,
};

doca_error_t urom_plugin_get_iface(struct urom_plugin_iface *iface)
{
	if (iface == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	DOCA_STRUCT_CTOR(urom_worker_graph.super);
	*iface = urom_worker_graph.super;
	return DOCA_SUCCESS;
}

doca_error_t urom_plugin_get_version(uint64_t *version)
{
	if (version == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	*version = plugin_version;
	return DOCA_SUCCESS;
}
