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

#include <worker_sandbox.h>
#include "urom_sandbox.h"

DOCA_LOG_REGISTER(UROM::WORKER::SANDBOX);

static uint64_t plugin_version = 0x01; /* Sandbox plugin DPU version */

/*
 * Close sandbox worker plugin
 *
 * @worker_ctx [in]: DOCA UROM worker context
 */
static void urom_worker_sandbox_close(struct urom_worker_ctx *worker_ctx)
{
	struct urom_worker_sandbox *sandbox_worker = worker_ctx->plugin_ctx;

	if (sandbox_worker == NULL)
		return;

	kh_destroy(ep, sandbox_worker->ucp_data.eps);
	ucp_worker_release_address(sandbox_worker->ucp_data.ucp_worker, sandbox_worker->ucp_data.worker_address);
	ucp_worker_destroy(sandbox_worker->ucp_data.ucp_worker);
	ucp_cleanup(sandbox_worker->ucp_data.ucp_context);
	free(sandbox_worker);
}

/*
 * Open sandbox worker plugin
 *
 * @ctx [in]: DOCA UROM worker context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_sandbox_open(struct urom_worker_ctx *ctx)
{
	struct urom_worker_sandbox *sandbox_worker;
	ucp_worker_params_t worker_params;
	ucp_params_t ucp_params;
	ucp_config_t *ucp_config;
	ucp_context_h ucp_context;
	ucp_worker_h ucp_worker;
	ucs_status_t status;

	sandbox_worker = malloc(sizeof(*sandbox_worker));
	if (sandbox_worker == NULL)
		return DOCA_ERROR_NO_MEMORY;
	ctx->plugin_ctx = sandbox_worker;

	status = ucp_config_read(NULL, NULL, &ucp_config);
	if (status != UCS_OK)
		goto err_cfg;

	/* Avoid SM when using xGVMI */
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
	ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_RMA | UCP_FEATURE_AMO64;
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

	sandbox_worker->ucp_data.worker_address = NULL;
	status = ucp_worker_get_address(ucp_worker,
					&sandbox_worker->ucp_data.worker_address,
					&sandbox_worker->ucp_data.ucp_addrlen);
	if (status != UCS_OK)
		goto err_worker_address;

	DOCA_LOG_DBG("Worker addr length: %lu", sandbox_worker->ucp_data.ucp_addrlen);

	sandbox_worker->ucp_data.ucp_context = ucp_context;
	sandbox_worker->ucp_data.ucp_worker = ucp_worker;

	sandbox_worker->ucp_data.eps = kh_init(ep);
	if (!sandbox_worker->ucp_data.eps)
		goto err_eps;

	ucs_list_head_init(&sandbox_worker->completed_reqs);

	return DOCA_SUCCESS;

err_eps:
	ucp_worker_release_address(ucp_worker, sandbox_worker->ucp_data.worker_address);
err_worker_address:
	ucp_worker_destroy(ucp_worker);
err_worker_create:
	ucp_cleanup(ucp_context);
err_cfg:
	free(sandbox_worker);
	return DOCA_ERROR_NOT_FOUND;
}

/*
 * Worker endpoint lookup, check if endpoint in worker map
 *
 * @ctx [in]: DOCA UROM worker context
 * @dest [in]: worker destination
 * @ep [in]: worker endpoint to check
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sandbox_ep_lookup(struct urom_worker_ctx *ctx, uint64_t dest, ucp_ep_h *ep)
{
	struct urom_worker_sandbox *sandbox_worker = (struct urom_worker_sandbox *)ctx->plugin_ctx;
	khint_t k;
	int ret;
	void *addr;
	ucp_ep_params_t ep_params;
	ucs_status_t ucs_status;
	ucp_ep_h new_ep;
	doca_error_t status;

	k = kh_get(ep, sandbox_worker->ucp_data.eps, dest);
	if (k != kh_end(sandbox_worker->ucp_data.eps)) {
		*ep = kh_value(sandbox_worker->ucp_data.eps, k);
		return DOCA_SUCCESS;
	}

	/* Create new EP */
	status = doca_urom_worker_domain_addr_lookup(ctx, dest, &addr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_DBG("ID not found in domain:: %#lx", dest);
		return DOCA_ERROR_NOT_FOUND;
	}

	ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
	ep_params.address = addr;

	ucs_status = ucp_ep_create(sandbox_worker->ucp_data.ucp_worker, &ep_params, &new_ep);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("ucp_ep_create() returned: %s", ucs_status_string(ucs_status));
		return DOCA_ERROR_INITIALIZATION;
	}

	k = kh_put(ep, sandbox_worker->ucp_data.eps, dest, &ret);
	if (ret <= 0)
		return DOCA_ERROR_DRIVER;
	kh_value(sandbox_worker->ucp_data.eps, k) = new_ep;

	*ep = new_ep;

	DOCA_LOG_DBG("Created EP for dest: %#lx", dest);

	return DOCA_SUCCESS;
}

/*
 * Handle memory map sandbox command
 *
 * @sandbox_worker [in]: Sandbox worker context
 * @cmd_desc [in]: sandbox command descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sandbox_mem_map_cmd(struct urom_worker_sandbox *sandbox_worker,
					struct urom_worker_cmd_desc *cmd_desc)
{
	struct urom_worker_cmd *worker_cmd = (struct urom_worker_cmd *)&cmd_desc->worker_cmd;
	struct urom_worker_sandbox_cmd *sandbox_cmd = (struct urom_worker_sandbox_cmd *)worker_cmd->plugin_cmd;
	struct urom_worker_notif_desc *notif_desc;
	struct urom_worker_notify_sandbox *sandbox_notif;
	ucs_status_t ucs_status;
	ucp_mem_h memh;

	notif_desc = calloc(1, sizeof(*notif_desc) + sizeof(*sandbox_notif));
	if (notif_desc == NULL) {
		DOCA_LOG_ERR("Failed to allocate notification");
		return DOCA_ERROR_NO_MEMORY;
	}

	ucs_status = ucp_mem_map(sandbox_worker->ucp_data.ucp_context, &sandbox_cmd->mem_map.map_params, &memh);
	if (ucs_status != UCS_OK)
		DOCA_LOG_ERR("Memory mapping failed");

	DOCA_LOG_DBG("Mapped buffer, context: %#lx memh_id: %#lx", sandbox_cmd->mem_map.context, (uint64_t)memh);

	notif_desc->dest_id = cmd_desc->dest_id;
	notif_desc->worker_notif.urom_context = worker_cmd->urom_context;
	notif_desc->worker_notif.type = cmd_desc->worker_cmd.type;

	notif_desc->worker_notif.len = sizeof(*sandbox_notif);

	sandbox_notif = (struct urom_worker_notify_sandbox *)notif_desc->worker_notif.plugin_notif;
	sandbox_notif->mem_map.context = sandbox_cmd->mem_map.context;
	sandbox_notif->mem_map.memh_id = (uint64_t)memh;
	sandbox_notif->type = UROM_WORKER_NOTIFY_SANDBOX_TAG_SEND;

	if (ucs_status != UCS_OK)
		notif_desc->worker_notif.status = DOCA_ERROR_DRIVER;
	else
		notif_desc->worker_notif.status = DOCA_SUCCESS;

	ucs_list_add_tail(&sandbox_worker->completed_reqs, &notif_desc->entry);

	return notif_desc->worker_notif.status;
}

/*
 * Tag send task callback function, is registered on UCX task and will be called once task is completed
 *
 * @ucp_req [in]: UCP request handler
 * @ucs_status [in]: task status
 * @user_data [in]: task user data
 */
static void sandbox_tag_send_cb(void *ucp_req, ucs_status_t ucs_status, void *user_data)
{
	struct urom_worker_sandbox_request *req = (struct urom_worker_sandbox_request *)user_data;
	struct urom_worker_notify *notif = (struct urom_worker_notify *)&req->notif_desc->worker_notif;
	struct urom_worker_notify_sandbox *notif_sandbox = (struct urom_worker_notify_sandbox *)notif->plugin_notif;

	notif->len = sizeof(*notif_sandbox);
	notif_sandbox->type = UROM_WORKER_NOTIFY_SANDBOX_TAG_SEND;
	notif_sandbox->tag_send.status = ucs_status;

	if (ucs_status != UCS_OK)
		notif->status = DOCA_ERROR_DRIVER;
	else
		notif->status = DOCA_SUCCESS;
	ucs_list_add_tail(&req->sandbox_worker->completed_reqs, &req->notif_desc->entry);

	ucp_request_free(ucp_req);
	free(req);

	DOCA_LOG_DBG("Completed tagged send, context: %#lx", notif_sandbox->tag_send.context);
}

/*
 * Handle tag send sandbox command
 *
 * @ctx [in]: DOCA UROM worker context
 * @cmd_desc [in]: sandbox command descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sandbox_tag_send_cmd(struct urom_worker_ctx *ctx, struct urom_worker_cmd_desc *cmd_desc)
{
	struct urom_worker_sandbox *sandbox_worker = (struct urom_worker_sandbox *)ctx->plugin_ctx;
	struct urom_worker_cmd *worker_cmd = (struct urom_worker_cmd *)&cmd_desc->worker_cmd;
	struct urom_worker_sandbox_cmd *sandbox_cmd = (struct urom_worker_sandbox_cmd *)&worker_cmd->plugin_cmd;
	ucp_request_param_t req_param = {};
	doca_error_t status;
	ucs_status_ptr_t ucp_status;
	ucp_ep_h ep = NULL;
	struct urom_worker_sandbox_request *req = NULL;
	int inl = !sandbox_cmd->tag_send.memh_id; /* inline if null memh */
	struct urom_worker_notify *notif;
	struct urom_worker_notify_sandbox *sandbox_notif;
	struct urom_worker_notif_desc *notif_desc;

	notif_desc = malloc(sizeof(*notif_desc) + sizeof(*sandbox_notif));
	if (notif_desc == NULL) {
		DOCA_LOG_ERR("Failed to allocate notif_desc");
		return DOCA_ERROR_NO_MEMORY;
	}
	notif_desc->dest_id = cmd_desc->dest_id;

	notif = (struct urom_worker_notify *)&notif_desc->worker_notif;
	notif->type = worker_cmd->type;
	notif->len = sizeof(*sandbox_notif);
	notif->urom_context = worker_cmd->urom_context;

	sandbox_notif = (struct urom_worker_notify_sandbox *)notif->plugin_notif;
	sandbox_notif->type = UROM_WORKER_NOTIFY_SANDBOX_TAG_SEND;
	sandbox_notif->tag_send.context = sandbox_cmd->tag_send.context;

	if (!inl) {
		req = malloc(sizeof(*req));
		if (req == NULL) {
			DOCA_LOG_ERR("Failed to allocate request");
			status = DOCA_ERROR_NO_MEMORY;
			goto push_error;
		}

		req->notif_desc = notif_desc;
		req->sandbox_worker = sandbox_worker;
		req->inline_data = inl;

		req_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
		req_param.cb.send = sandbox_tag_send_cb;
		req_param.user_data = req;

		req_param.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
		req_param.memh = (ucp_mem_h)sandbox_cmd->tag_send.memh_id;
		DOCA_LOG_DBG("Using memh_id: %#lx", sandbox_cmd->tag_send.memh_id);
	}

	status = sandbox_ep_lookup(ctx, sandbox_cmd->tag_send.dest, &ep);
	if (status != DOCA_SUCCESS) {
		if (!inl)
			free(req);
		goto push_error;
	}

	ucp_status = ucp_tag_send_nbx(ep,
				      (void *)sandbox_cmd->tag_send.buffer,
				      sandbox_cmd->tag_send.count,
				      sandbox_cmd->tag_send.tag,
				      &req_param);

	if (inl) {
		if (UCS_PTR_STATUS(ucp_status) != UCS_OK) {
			if (UCS_PTR_STATUS(ucp_status) != UCS_INPROGRESS) {
				DOCA_LOG_DBG("ucp_tag_send_nbx() (inline) returned error: %s",
					     ucs_status_string(UCS_PTR_STATUS(ucp_status)));
				status = DOCA_ERROR_INVALID_VALUE;
				sandbox_notif->tag_send.status = UCS_PTR_STATUS(ucp_status);
				goto push_error;
			}

			/* Block until local completion */
			while (ucp_request_check_status(ucp_status) == UCS_INPROGRESS)
				ucp_worker_progress(sandbox_worker->ucp_data.ucp_worker);

			sandbox_notif->tag_send.status = ucp_request_check_status(ucp_status);

			if (sandbox_notif->tag_send.status != UCS_OK)
				notif->status = DOCA_ERROR_DRIVER;
			else
				notif->status = DOCA_SUCCESS;

			ucs_list_add_tail(&sandbox_worker->completed_reqs, &notif_desc->entry);
			ucp_request_free(ucp_status);
		} else {
			sandbox_notif->tag_send.status = UCS_OK;
			notif->status = DOCA_SUCCESS;
			ucs_list_add_tail(&sandbox_worker->completed_reqs, &notif_desc->entry);
		}

	} else {
		if (UCS_PTR_STATUS(ucp_status) != UCS_INPROGRESS) {
			DOCA_LOG_DBG("ucp_tag_send_nbx() returned error: %s",
				     ucs_status_string(UCS_PTR_STATUS(ucp_status)));
			free(req);
			status = DOCA_ERROR_INVALID_VALUE;
			sandbox_notif->tag_send.status = UCS_PTR_STATUS(ucp_status);
			goto push_error;
		}
	}

	DOCA_LOG_DBG("Issued tagged send, dest: %lu, len: %lu%s",
		     sandbox_cmd->tag_send.dest,
		     sandbox_cmd->tag_send.count,
		     inl ? " (inl)" : "");

	return DOCA_SUCCESS;
push_error:
	notif->status = status;
	ucs_list_add_tail(&sandbox_worker->completed_reqs, &notif_desc->entry);
	return status;
}

/*
 * Tag recv task callback function, is registered on UCX task and will be called once task is completed
 *
 * @ucp_req [in]: UCP request handler
 * @ucs_status [in]: task status
 * @tag_info [in]: UCP tag recv info
 * @user_data [in]: task user data
 */
static void sandbox_tag_recv_cb(void *ucp_req,
				ucs_status_t ucs_status,
				const ucp_tag_recv_info_t *tag_info,
				void *user_data)
{
	struct urom_worker_sandbox_request *req = (struct urom_worker_sandbox_request *)user_data;
	struct urom_worker_notify *notif = (struct urom_worker_notify *)&req->notif_desc->worker_notif;
	struct urom_worker_notify_sandbox *sandbox_notify = (struct urom_worker_notify_sandbox *)notif->plugin_notif;

	notif->len = sizeof(*sandbox_notify);
	if (sandbox_notify->tag_recv.buffer != NULL)
		notif->len += tag_info->length;

	sandbox_notify->type = UROM_WORKER_NOTIFY_SANDBOX_TAG_RECV;
	sandbox_notify->tag_recv.count = tag_info->length;
	sandbox_notify->tag_recv.sender_tag = tag_info->sender_tag;
	sandbox_notify->tag_recv.status = ucs_status;

	if (ucs_status != UCS_OK)
		notif->status = DOCA_ERROR_DRIVER;
	else
		notif->status = DOCA_SUCCESS;

	ucs_list_add_tail(&req->sandbox_worker->completed_reqs, &req->notif_desc->entry);

	ucp_request_free(ucp_req);
	free(req);

	DOCA_LOG_DBG("Completed tagged receive, context: %#lx", sandbox_notify->tag_recv.context);
}

/*
 * Handle tag recv sandbox command
 *
 * @sandbox_worker [in]: Sandbox worker context
 * @cmd_desc [in]: sandbox command descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t sandbox_tag_recv_cmd(struct urom_worker_sandbox *sandbox_worker,
					 struct urom_worker_cmd_desc *cmd_desc)
{
	doca_error_t status;
	struct urom_worker_cmd *worker_cmd = (struct urom_worker_cmd *)&cmd_desc->worker_cmd;
	struct urom_worker_sandbox_cmd *sandbox_cmd = (struct urom_worker_sandbox_cmd *)worker_cmd->plugin_cmd;
	struct urom_worker_notify *notif;
	struct urom_worker_notify_sandbox *sandbox_notif;
	ucp_request_param_t req_param = {0};
	ucs_status_ptr_t ucp_status;
	struct urom_worker_sandbox_request *req;
	int inl = !sandbox_cmd->tag_recv.memh_id; /* inline if null memh */
	int nd_size;
	void *buffer;

	req = malloc(sizeof(*req));
	if (req == NULL) {
		DOCA_LOG_ERR("Failed to allocate request");
		return DOCA_ERROR_NO_MEMORY;
	}

	req->sandbox_worker = sandbox_worker;
	req->inline_data = inl;

	/* |- notif_desc -|- notif -|- recv_buf -| */
	nd_size = sizeof(*req->notif_desc) + sizeof(*sandbox_notif);
	if (inl)
		nd_size += sandbox_cmd->tag_recv.count;

	req->notif_desc = calloc(1, nd_size);
	if (req->notif_desc == NULL) {
		free(req);
		return DOCA_ERROR_NO_MEMORY;
	}

	req->notif_desc->dest_id = cmd_desc->dest_id;

	notif = (struct urom_worker_notify *)&req->notif_desc->worker_notif;
	notif->urom_context = worker_cmd->urom_context;
	notif->type = cmd_desc->worker_cmd.type;
	notif->len = sizeof(*sandbox_notif);

	sandbox_notif = (struct urom_worker_notify_sandbox *)notif->plugin_notif;
	sandbox_notif->tag_recv.context = sandbox_cmd->tag_recv.context;

	req_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
	req_param.cb.recv = sandbox_tag_recv_cb;
	req_param.user_data = req;

	if (!inl) {
		req_param.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
		req_param.memh = (ucp_mem_h)sandbox_cmd->tag_recv.memh_id;

		buffer = (void *)sandbox_cmd->tag_recv.buffer;
		sandbox_notif->tag_recv.buffer = NULL;
	} else {
		buffer = (void *)notif + ucs_offsetof(struct urom_worker_notify, plugin_notif) +
			 sizeof(struct urom_worker_notify_sandbox);
		sandbox_notif->tag_recv.buffer = buffer;
	}

	ucp_status = ucp_tag_recv_nbx(sandbox_worker->ucp_data.ucp_worker,
				      buffer,
				      sandbox_cmd->tag_recv.count,
				      sandbox_cmd->tag_recv.tag,
				      sandbox_cmd->tag_recv.tag_mask,
				      &req_param);
	if (UCS_PTR_IS_ERR(ucp_status)) {
		status = DOCA_ERROR_DRIVER;
		sandbox_notif->tag_recv.status = UCS_PTR_STATUS(ucp_status);
		goto push_error;
	}

	DOCA_LOG_DBG("Posted tagged receive, context: %#lx", sandbox_cmd->tag_recv.context);

	return DOCA_SUCCESS;
push_error:
	notif->status = status;
	ucs_list_add_tail(&sandbox_worker->completed_reqs, &req->notif_desc->entry);
	free(req);
	return status;
}

/*
 * Unpacking sandbox worker command
 *
 * @packed_cmd [in]: packed worker command
 * @packed_cmd_len [in]: packed worker command length
 * @cmd [out]: set unpacked UROM worker command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_sandbox_cmd_unpack(void *packed_cmd,
						   size_t packed_cmd_len,
						   struct urom_worker_cmd **cmd)
{
	void *ptr;
	uint64_t extended_mem = 0;
	struct urom_worker_sandbox_cmd *sandbox_cmd;

	if (packed_cmd_len < sizeof(struct urom_worker_sandbox_cmd)) {
		DOCA_LOG_INFO("Invalid packed command length");
		return DOCA_ERROR_INVALID_VALUE;
	}

	*cmd = packed_cmd;
	ptr = packed_cmd + ucs_offsetof(struct urom_worker_cmd, plugin_cmd) + sizeof(struct urom_worker_sandbox_cmd);
	sandbox_cmd = (struct urom_worker_sandbox_cmd *)(*cmd)->plugin_cmd;

	switch (sandbox_cmd->type) {
	case UROM_WORKER_CMD_SANDBOX_MEM_MAP:
		sandbox_cmd->mem_map.map_params.exported_memh_buffer = ptr;
		extended_mem += sandbox_cmd->mem_map.exported_memh_buffer_len;
		break;
	case UROM_WORKER_CMD_SANDBOX_TAG_SEND:
		if (!sandbox_cmd->tag_send.memh_id) {
			sandbox_cmd->tag_send.buffer = (uint64_t)ptr;
			extended_mem += sandbox_cmd->tag_send.count;
		}
		break;
	}

	if ((*cmd)->len != extended_mem + sizeof(struct urom_worker_sandbox_cmd)) {
		DOCA_LOG_ERR("Invalid sandbox command length");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Handle UROM sandbox worker commands function
 *
 * @ctx [in]: DOCA UROM worker context
 * @cmd_list [in]: command descriptor list to handle
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_sandbox_worker_cmd(struct urom_worker_ctx *ctx, ucs_list_link_t *cmd_list)
{
	struct urom_worker_sandbox *sandbox_worker = (struct urom_worker_sandbox *)ctx->plugin_ctx;
	struct urom_worker_cmd_desc *cmd_desc;
	struct urom_worker_cmd *cmd;
	struct urom_worker_sandbox_cmd *sandbox_cmd;
	doca_error_t status = DOCA_SUCCESS;

	while (!ucs_list_is_empty(cmd_list)) {
		cmd_desc = ucs_list_extract_head(cmd_list, struct urom_worker_cmd_desc, entry);

		status = urom_worker_sandbox_cmd_unpack(&cmd_desc->worker_cmd, cmd_desc->worker_cmd.len, &cmd);
		if (status != DOCA_SUCCESS) {
			free(cmd_desc);
			return status;
		}
		sandbox_cmd = (struct urom_worker_sandbox_cmd *)cmd->plugin_cmd;
		switch (sandbox_cmd->type) {
		case UROM_WORKER_CMD_SANDBOX_MEM_MAP:
			status = sandbox_mem_map_cmd(sandbox_worker, cmd_desc);
			break;
		case UROM_WORKER_CMD_SANDBOX_TAG_SEND:
			status = sandbox_tag_send_cmd(ctx, cmd_desc);
			break;
		case UROM_WORKER_CMD_SANDBOX_TAG_RECV:
			status = sandbox_tag_recv_cmd(sandbox_worker, cmd_desc);
			break;
		default:
			DOCA_LOG_INFO("Invalid SANDBOX command type: %lu", sandbox_cmd->type);
			status = DOCA_ERROR_INVALID_VALUE;
			break;
		}
		free(cmd_desc);
		if (status != DOCA_SUCCESS)
			return status;
	}

	return status;
}

/*
 * Get sandbox worker address
 *
 * UROM worker calls the function twice, first one to get address length and second one to get address data
 *
 * @worker_ctx [in]: DOCA UROM worker context
 * @addr [out]: set worker address
 * @addr_len [out]: set worker address length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_sandbox_addr(struct urom_worker_ctx *worker_ctx, void *addr, uint64_t *addr_len)
{
	struct urom_worker_sandbox *sandbox_worker = (struct urom_worker_sandbox *)worker_ctx->plugin_ctx;

	/* Always return address size */
	if (*addr_len < sandbox_worker->ucp_data.ucp_addrlen) {
		/* Return required buffer size on error */
		*addr_len = sandbox_worker->ucp_data.ucp_addrlen;
		return DOCA_ERROR_INVALID_VALUE;
	}

	*addr_len = sandbox_worker->ucp_data.ucp_addrlen;
	memcpy(addr, sandbox_worker->ucp_data.worker_address, *addr_len);

	return DOCA_SUCCESS;
}

/*
 * Check sandbox worker tasks progress to get notifications
 *
 * @ctx [in]: DOCA UROM worker context
 * @notif_list [out]: set notification descriptors for completed tasks
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_sandbox_progress(struct urom_worker_ctx *ctx, ucs_list_link_t *notif_list)
{
	struct urom_worker_sandbox *sandbox_worker = (struct urom_worker_sandbox *)ctx->plugin_ctx;
	struct urom_worker_notif_desc *nd;

	ucp_worker_progress(sandbox_worker->ucp_data.ucp_worker);

	if (ucs_list_is_empty(&sandbox_worker->completed_reqs))
		return DOCA_ERROR_EMPTY;

	while (!ucs_list_is_empty(&sandbox_worker->completed_reqs)) {
		nd = ucs_list_extract_head(&sandbox_worker->completed_reqs, struct urom_worker_notif_desc, entry);

		ucs_list_add_tail(notif_list, &nd->entry);

		/* Caller must free entries in notif_list */
	}

	return DOCA_SUCCESS;
}

/*
 * Packing sandbox notification
 *
 * @notif [in]: sandbox notification to pack
 * @packed_notif_len [in/out]: set packed notification command buffer size
 * @packed_notif [out]: set packed notification command buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t urom_worker_sandbox_notif_pack(struct urom_worker_notify *notif,
						   size_t *packed_notif_len,
						   void *packed_notif)
{
	int pack_len;
	void *pack_head;
	void *pack_tail = packed_notif;
	struct urom_worker_notify_sandbox *sandbox_notif = (struct urom_worker_notify_sandbox *)notif->plugin_notif;

	/* pack base command */
	pack_len = ucs_offsetof(struct urom_worker_notify, plugin_notif) + sizeof(struct urom_worker_notify_sandbox);
	pack_head = urom_sandbox_serialize_next_raw(&pack_tail, void, pack_len);
	memcpy(pack_head, notif, pack_len);
	*packed_notif_len = pack_len;

	/* pack inline data */
	switch (sandbox_notif->type) {
	case UROM_WORKER_NOTIFY_SANDBOX_TAG_RECV:
		if (sandbox_notif->tag_recv.buffer) {
			pack_len = sandbox_notif->tag_recv.count;
			pack_head = urom_sandbox_serialize_next_raw(&pack_tail, void, pack_len);
			memcpy(pack_head, (void *)sandbox_notif->tag_recv.buffer, pack_len);
			*packed_notif_len += pack_len;
		}
		break;
	}
	return DOCA_SUCCESS;
}

/* Define UROM sandbox plugin interface, set plugin functions */
static struct urom_worker_sandbox_iface urom_worker_sandbox = {
	.super.open = urom_worker_sandbox_open,
	.super.close = urom_worker_sandbox_close,
	.super.addr = urom_worker_sandbox_addr,
	.super.worker_cmd = urom_worker_sandbox_worker_cmd,
	.super.progress = urom_worker_sandbox_progress,
	.super.notif_pack = urom_worker_sandbox_notif_pack,
};

doca_error_t urom_plugin_get_iface(struct urom_plugin_iface *iface)
{
	if (iface == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	DOCA_STRUCT_CTOR(urom_worker_sandbox.super);
	*iface = urom_worker_sandbox.super;
	return DOCA_SUCCESS;
}

doca_error_t urom_plugin_get_version(uint64_t *version)
{
	if (version == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	*version = plugin_version;
	return DOCA_SUCCESS;
}
