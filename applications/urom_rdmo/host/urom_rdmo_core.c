/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sched.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <malloc.h>

#include <ucp/api/ucp.h>

#include <doca_argp.h>
#include <doca_ctx.h>
#include <doca_urom.h>

#include <samples/common.h>

#include "urom_rdmo_core.h"
#include "utils.h"
#include "worker_rdmo.h"

DOCA_LOG_REGISTER(UROM::RDMO::CORE);

#define FLUSH_ID 0xbeef		    /* Flush callback id */
#define MAX_WORKER_ADDRESS_LEN 1024 /* Maximum address length */

/* Remote buffer descriptor */
struct rbuf_desc {
	uint64_t rkey;	 /* Remote key */
	uint64_t *raddr; /* Remote address */
};

/* RDMO client init result */
struct client_init_result {
	char *addr;	   /* Device UCP worker address */
	uint64_t addr_len; /* Address length */
};

/* RDMO RQ create result */
struct rq_create_result {
	uint64_t rq_id; /* RQ id */
};

/* RDMO RQ destroy result */
struct rq_destroy_result {
	uint64_t rq_id; /* RQ id */
};

/* RDMO MR register result */
struct mr_register_result {
	uint64_t rkey; /* Memory remote key */
};

/* RDMO MR deregister result */
struct mr_deregister_result {
	uint64_t rkey; /* Memory remote key */
};

/* RDMO task result structure */
struct rdmo_result {
	doca_error_t result; /* Task result */
	union {
		struct client_init_result client_init; /* Client init result */
		struct rq_create_result rq_create;     /* RQ create result */
		struct rq_destroy_result rq_destroy;   /* RQ destroy result */
		struct mr_register_result mr_reg;      /* MR register result */
		struct mr_deregister_result mr_dereg;  /* MR deregister result */
	};
};

/*******************************************************************************
 * Client functions
 ******************************************************************************/
/*
 * Client data exchange function
 *
 * @server_name [in]: server host name
 * @port [in]: socket port
 * @loc_data [in]: local data to send to the server
 * @loc_datalen [in]: local data length
 * @rem_data [out]: remote data to get from the server
 * @rem_datalen [out]: remote data length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t client_exchange(const char *server_name,
				    int port,
				    void *loc_data,
				    size_t loc_datalen,
				    void **rem_data,
				    size_t *rem_datalen)
{
	int n;
	ssize_t ret;
	void *data;
	char *service;
	struct addrinfo *res, *t;
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	static int client_sockfd = -1;

	if (client_sockfd >= 0)
		goto connected;

	if (asprintf(&service, "%d", port) < 0)
		return DOCA_ERROR_IO_FAILED;

	n = getaddrinfo(server_name, service, &hints, &res);
	if (n < 0) {
		DOCA_LOG_ERR("getaddrinfo() returned error [%s] for %s:%d", gai_strerror(n), server_name, port);
		free(service);
		return DOCA_ERROR_IO_FAILED;
	}

	for (t = res; t; t = t->ai_next) {
		client_sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (client_sockfd >= 0) {
			if (!connect(client_sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(client_sockfd);
			client_sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (client_sockfd < 0) {
		DOCA_LOG_ERR("Couldn't connect to %s:%d", server_name, port);
		return DOCA_ERROR_BAD_STATE;
	}

connected:

	ret = send(client_sockfd, &loc_datalen, sizeof(loc_datalen), 0);
	if (ret < 0 || (size_t)ret != sizeof(loc_datalen)) {
		DOCA_LOG_ERR("Couldn't send local datalen");
		return DOCA_ERROR_IO_FAILED;
	}

	ret = send(client_sockfd, loc_data, loc_datalen, 0);
	if (ret < 0 || (size_t)ret != loc_datalen) {
		DOCA_LOG_ERR("Couldn't send local data");
		return DOCA_ERROR_IO_FAILED;
	}

	*rem_datalen = 0;
	ret = recv(client_sockfd, rem_datalen, sizeof(*rem_datalen), 0);
	if (ret < 0 || (size_t)ret != sizeof(*rem_datalen)) {
		DOCA_LOG_ERR("Couldn't read/write remote datalen");
		return DOCA_ERROR_IO_FAILED;
	}

	if (!*rem_datalen)
		return DOCA_SUCCESS;

	if (*rem_datalen > MAX_WORKER_ADDRESS_LEN) {
		DOCA_LOG_ERR("Received data length greater than the limit %d", MAX_WORKER_ADDRESS_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	data = calloc(1, *rem_datalen);
	if (data == NULL) {
		DOCA_LOG_ERR("Failed to allocate client data buffer");
		return DOCA_ERROR_NO_MEMORY;
	}

	ret = recv(client_sockfd, data, *rem_datalen, 0);
	if (ret < 0 || (size_t)ret != *rem_datalen) {
		free(data);
		DOCA_LOG_ERR("Couldn't read/write remote data");
		return DOCA_ERROR_IO_FAILED;
	}

	*rem_data = data;

	DOCA_LOG_INFO("Client received remote data, length: %lu", *rem_datalen);

	return DOCA_SUCCESS;
}

/*
 * Handle RDMO scatter operation
 *
 * @client_ucp_worker [in]: client UCP worker structure
 * @client_ucp_ep [in]: client UCP endpoint structure
 * @target [in]: remote target
 * @data [in]: data to scatter
 * @len [in]: data length
 * @chunk_size [in]: chunk size to split data to chunks
 * @rkey [in]: data remote memory key
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_scatter(ucp_worker_h client_ucp_worker,
				 ucp_ep_h client_ucp_ep,
				 uint64_t target,
				 void *data,
				 size_t len,
				 int chunk_size,
				 uint64_t rkey)
{
	int i;
	void *hdr;
	int chunks = len / chunk_size;
	struct urom_rdmo_hdr *rdmo_hdr;
	ucs_status_ptr_t ucs_status_ptr;
	struct urom_rdmo_scatter_iov *iov;
	struct urom_rdmo_scatter_hdr *scatter_hdr;
	ucp_request_param_t req_param = {
		.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS,
		.flags = UCP_AM_SEND_FLAG_REPLY,
	};
	int req_data_len = chunks * (sizeof(struct urom_rdmo_scatter_iov) + chunk_size);
	size_t hdr_len = sizeof(struct urom_rdmo_hdr) + sizeof(struct urom_rdmo_scatter_hdr);
	uint8_t req_data[req_data_len];

	memset(req_data, 0, req_data_len);

	hdr = alloca(hdr_len);
	if (hdr == NULL)
		return DOCA_ERROR_NO_MEMORY;

	rdmo_hdr = (struct urom_rdmo_hdr *)hdr;
	scatter_hdr = (struct urom_rdmo_scatter_hdr *)(rdmo_hdr + 1);

	rdmo_hdr->id = 0;
	rdmo_hdr->op_id = UROM_RDMO_OP_SCATTER;
	rdmo_hdr->flags = 0;
	scatter_hdr->count = chunks;

	iov = (struct urom_rdmo_scatter_iov *)req_data;
	for (i = 0; i < chunks; i++) {
		iov->addr = target;
		iov->len = chunk_size;
		iov->rkey = rkey;
		memcpy(iov + 1, data, chunk_size);

		data += chunk_size;
		target += chunk_size;
		iov = (struct urom_rdmo_scatter_iov *)((uintptr_t)iov + sizeof(*iov) + chunk_size);
	}

	ucs_status_ptr = ucp_am_send_nbx(client_ucp_ep, 0, hdr, hdr_len, req_data, req_data_len, &req_param);
	if (UCS_PTR_IS_ERR(ucs_status_ptr))
		return DOCA_ERROR_DRIVER;

	if (UCS_PTR_STATUS(ucs_status_ptr) == UCS_INPROGRESS) {
		while (ucp_request_check_status(ucs_status_ptr) == UCS_INPROGRESS)
			ucp_worker_progress(client_ucp_worker);

		if (ucp_request_check_status(ucs_status_ptr) != UCS_OK)
			return DOCA_ERROR_DRIVER;
		ucp_request_free(ucs_status_ptr);
	} else {
		if (UCS_PTR_STATUS(ucs_status_ptr) != UCS_OK)
			return DOCA_ERROR_DRIVER;
	}

	DOCA_LOG_INFO("RDMO Scatter complete");
	return DOCA_SUCCESS;
}

/*
 * Handle RDMO flush operation
 *
 * @client_ucp_worker [in]: client UCP worker structure
 * @client_ucp_ep [in]: client UCP endpoint structure
 * @flushed [out]: will be set once the flush operation finished
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_flush(ucp_worker_h client_ucp_worker, ucp_ep_h client_ucp_ep, int *flushed)
{
	struct urom_rdmo_hdr *hdr;
	ucs_status_ptr_t ucs_status_ptr;
	struct urom_rdmo_flush_hdr *flush_hdr;
	size_t hdr_len = sizeof(*hdr) + sizeof(*flush_hdr);
	ucp_request_param_t req_param = {
		.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS,
		.flags = UCP_AM_SEND_FLAG_REPLY,
	};

	hdr = alloca(hdr_len);
	flush_hdr = (struct urom_rdmo_flush_hdr *)(hdr + 1);

	hdr->id = 0;
	hdr->op_id = UROM_RDMO_OP_FLUSH;
	hdr->flags = UROM_RDMO_REQ_FLAG_FENCE;
	flush_hdr->flush_id = FLUSH_ID;
	*flushed = 0;

	ucs_status_ptr = ucp_am_send_nbx(client_ucp_ep, 0, hdr, hdr_len, NULL, 0, &req_param);
	if (UCS_PTR_IS_ERR(ucs_status_ptr)) {
		DOCA_LOG_ERR("ucp_am_send_nbx() returned error [%s]",
			     ucs_status_string(ucp_request_check_status(ucs_status_ptr)));
		return DOCA_ERROR_DRIVER;
	}

	if (UCS_PTR_STATUS(ucs_status_ptr) == UCS_INPROGRESS) {
		while (ucp_request_check_status(ucs_status_ptr) == UCS_INPROGRESS)
			ucp_worker_progress(client_ucp_worker);

		if (ucp_request_check_status(ucs_status_ptr) != UCS_OK)
			return DOCA_ERROR_DRIVER;

		ucp_request_free(ucs_status_ptr);
	} else {
		if (UCS_PTR_STATUS(ucs_status_ptr) != UCS_OK)
			return DOCA_ERROR_DRIVER;
	}

	DOCA_LOG_INFO("Sent flush request");

	while (!*flushed)
		ucp_worker_progress(client_ucp_worker);

	DOCA_LOG_INFO("Flush complete");
	return DOCA_SUCCESS;
}

/*
 * Handle RDMO append operation
 *
 * @client_ucp_worker [in]: client UCP worker structure
 * @client_ucp_ep [in]: client UCP endpoint structure
 * @ptr_addr [in]: remote buffer address
 * @data [in]: data to set
 * @len [in]: data length
 * @rkey [in]: memory remote key
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_append(ucp_worker_h client_ucp_worker,
				ucp_ep_h client_ucp_ep,
				uint64_t *ptr_addr,
				void *data,
				size_t len,
				uint64_t rkey)
{
	void *hdr;
	struct urom_rdmo_hdr *rdmo_hdr;
	ucs_status_ptr_t ucs_status_ptr;
	struct urom_rdmo_append_hdr *append_hdr;
	size_t hdr_len = sizeof(struct urom_rdmo_hdr) + sizeof(struct urom_rdmo_append_hdr);
	ucp_request_param_t req_param = {
		.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS,
		.flags = UCP_AM_SEND_FLAG_REPLY,
	};

	hdr = alloca(hdr_len);
	rdmo_hdr = (struct urom_rdmo_hdr *)hdr;
	append_hdr = (struct urom_rdmo_append_hdr *)(rdmo_hdr + 1);

	rdmo_hdr->id = 0;
	rdmo_hdr->op_id = UROM_RDMO_OP_APPEND;
	rdmo_hdr->flags = 0;
	append_hdr->ptr_addr = (uint64_t)ptr_addr;
	append_hdr->ptr_rkey = rkey;
	append_hdr->data_rkey = rkey;

	ucs_status_ptr = ucp_am_send_nbx(client_ucp_ep, 0, hdr, hdr_len, data, len, &req_param);
	if (UCS_PTR_IS_ERR(ucs_status_ptr)) {
		DOCA_LOG_ERR("ucp_am_send_nbx() returned error [%s]",
			     ucs_status_string(ucp_request_check_status(ucs_status_ptr)));
		return DOCA_ERROR_DRIVER;
	}

	if (UCS_PTR_STATUS(ucs_status_ptr) == UCS_INPROGRESS) {
		while (ucp_request_check_status(ucs_status_ptr) == UCS_INPROGRESS)
			ucp_worker_progress(client_ucp_worker);

		if (ucp_request_check_status(ucs_status_ptr) != UCS_OK)
			return DOCA_ERROR_DRIVER;

		ucp_request_free(ucs_status_ptr);
	} else {
		if (UCS_PTR_STATUS(ucs_status_ptr) != UCS_OK)
			return DOCA_ERROR_DRIVER;
	}
	DOCA_LOG_INFO("RDMO Append complete");
	return DOCA_SUCCESS;
}

/*
 * RDMO recv callback
 *
 * @arg [in]: program argument
 * @header [in]: active message header
 * @header_length [in]: header length
 * @data [in]: received data
 * @length [in]: data length
 * @param [in]: data receive parameters
 * @return: UCS_OK on success and UCS_ERR otherwise
 */
static ucs_status_t rdmo_am_cb(void *arg,
			       const void *header,
			       size_t header_length,
			       void *data,
			       size_t length,
			       const ucp_am_recv_param_t *param)
{
	(void)header_length;
	(void)data;
	(void)length;
	(void)param;

	int *flushed = (int *)arg;
	struct urom_rdmo_rsp_hdr *rsp_hdr;
	struct urom_rdmo_flush_rsp_hdr *flush_hdr;

	rsp_hdr = (struct urom_rdmo_rsp_hdr *)header;
	flush_hdr = (struct urom_rdmo_flush_rsp_hdr *)(rsp_hdr + 1);
	*flushed = 1;

	DOCA_LOG_INFO("Received AM Reply, ID: %#lx", flush_hdr->flush_id);
	return UCS_OK;
}

/*
 * Client wireup function
 *
 * @server_name [in]: server host name
 * @port [in]: socket port
 * @client_ucp_ep [out]: set client UCP endpoint
 * @ucp_worker [out]: set client UCP worker
 * @flushed [out]: client argument will be set once flush op is done
 * @ucp_context_p [out]: set UCP context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_wireup_client(char *server_name,
				       int port,
				       ucp_ep_h *client_ucp_ep,
				       ucp_worker_h *ucp_worker,
				       int *flushed,
				       ucp_context_h *ucp_context_p)
{
	doca_error_t result;
	ucp_params_t ucp_params;
	ucs_status_t ucs_status;
	ucp_config_t *ucp_config;
	ucp_context_h ucp_context;
	ucs_status_ptr_t close_req;
	ucp_worker_h client_ucp_worker;
	ucp_am_handler_param_t am_param;
	ucp_ep_params_t ep_params = {0};
	ucp_worker_params_t worker_params;
	size_t client_worker_addr_len, client_peer_dev_addr_len;
	ucp_address_t *client_worker_addr = NULL, *client_peer_dev_addr = NULL;
	ucp_request_param_t close_params = {/* Indicate that flags parameter is specified */
					    .op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS,
					    /* UCP EP closure flags */
					    .flags = UCP_OP_ATTR_FIELD_FLAGS};

	ucs_status = ucp_config_read(NULL, NULL, &ucp_config);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to get UCP config structure");
		return DOCA_ERROR_DRIVER;
	}

	ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
	ucp_params.features = UCP_FEATURE_AM | UCP_FEATURE_RMA | UCP_FEATURE_AMO64 | UCP_FEATURE_EXPORTED_MEMH;

	ucs_status = ucp_init(&ucp_params, ucp_config, &ucp_context);
	ucp_config_release(ucp_config);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to init UCP");
		return DOCA_ERROR_DRIVER;
	}

	worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
	worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

	/* Client worker (initiates RDMOs) */
	ucs_status = ucp_worker_create(ucp_context, &worker_params, &client_ucp_worker);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to create UCP worker address");
		result = DOCA_ERROR_DRIVER;
		goto ucp_context_free;
	}

	ucs_status = ucp_worker_get_address(client_ucp_worker, &client_worker_addr, &client_worker_addr_len);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to get UCP worker address");
		result = DOCA_ERROR_DRIVER;
		goto ucp_worker_destroy;
	}

	DOCA_LOG_INFO("Created client UCP Worker");

	/* Exchange worker addresses, give: local worker addr and get: server urom rdmo worker addr */
	result = client_exchange(server_name,
				 port,
				 client_worker_addr,
				 client_worker_addr_len,
				 (void **)&client_peer_dev_addr,
				 &client_peer_dev_addr_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange data with server");
		goto worker_addr_destroy;
	}

	DOCA_LOG_INFO("Received dev addr (len: %lu)", client_peer_dev_addr_len);

	/* Create initiator EP */
	ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
	ep_params.address = client_peer_dev_addr;

	ucs_status = ucp_ep_create(client_ucp_worker, &ep_params, client_ucp_ep);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to create UCP endpoint");
		result = DOCA_ERROR_DRIVER;
		goto dev_addr_free;
	}

	am_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID | UCP_AM_HANDLER_PARAM_FIELD_CB |
			      UCP_AM_HANDLER_PARAM_FIELD_ARG;
	am_param.id = 0;
	am_param.cb = rdmo_am_cb;
	am_param.arg = flushed;

	ucs_status = ucp_worker_set_am_recv_handler(client_ucp_worker, &am_param);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to set AM recv handler");
		result = DOCA_ERROR_DRIVER;
		goto ep_destroy;
	}

	DOCA_LOG_INFO("Created initiator EP: %p", *client_ucp_ep);

	ucp_worker_release_address(client_ucp_worker, client_worker_addr);
	free(client_peer_dev_addr);

	*ucp_worker = client_ucp_worker;
	*ucp_context_p = ucp_context;

	return DOCA_SUCCESS;

ep_destroy:
	close_req = ucp_ep_close_nbx(*client_ucp_ep, &close_params);
	if (UCS_PTR_IS_PTR(close_req)) {
		/* Wait completion of UCP EP close operation */
		do {
			/* Progress UCP worker */
			ucp_worker_progress(client_ucp_worker);
		} while (ucp_request_check_status(close_req) == UCS_INPROGRESS);

		/* Free UCP request */
		ucp_request_free(close_req);
	}

dev_addr_free:
	free(client_peer_dev_addr);

worker_addr_destroy:
	ucp_worker_release_address(client_ucp_worker, client_worker_addr);

ucp_worker_destroy:
	ucp_worker_destroy(client_ucp_worker);

ucp_context_free:
	ucp_cleanup(ucp_context);

	return result;
}

/*
 * Client UCP objects destroy function
 *
 * @client_ucp_ep [in]: client UCP endpoint
 * @ucp_worker [in]: client UCP worker
 * @ucp_context [in]: UCP context
 */
static void rdmo_ucp_client_destroy(ucp_ep_h client_ucp_ep, ucp_worker_h ucp_worker, ucp_context_h ucp_context)
{
	ucs_status_ptr_t close_req;
	ucp_request_param_t close_params = {/* Indicate that flags parameter is specified */
					    .op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS,
					    /* UCP EP closure flags */
					    .flags = UCP_OP_ATTR_FIELD_FLAGS};

	close_req = ucp_ep_close_nbx(client_ucp_ep, &close_params);
	if (UCS_PTR_IS_PTR(close_req)) {
		/* Wait completion of UCP EP close operation */
		do {
			/* Progress UCP worker */
			ucp_worker_progress(ucp_worker);
		} while (ucp_request_check_status(close_req) == UCS_INPROGRESS);

		/* Free UCP request */
		ucp_request_free(close_req);
	}

	ucp_worker_destroy(ucp_worker);
	ucp_cleanup(ucp_context);
}

/*******************************************************************************
 * Server functions
 ******************************************************************************/

/*
 * RDMO MR register callback
 *
 * @result [in]: task result
 * @cookie [in]: program cookie
 * @rkey [out]: memory region rkey
 */
static void mr_register_finished(doca_error_t result, union doca_data cookie, uint64_t rkey)
{
	struct rdmo_result *res = (struct rdmo_result *)cookie.ptr;

	if (res == NULL)
		return;

	res->result = result;
	if (result != DOCA_SUCCESS)
		return;

	res->mr_reg.rkey = rkey;
}

/*
 * Server data exchange function
 *
 * @port [in]: socket port
 * @loc_data [in]: local data to send to the client
 * @loc_datalen [in]: local data length
 * @rem_data [out]: remote data to get from the client
 * @rem_datalen [out]: remote data length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t server_exchange(int port, void *loc_data, size_t loc_datalen, void **rem_data, size_t *rem_datalen)
{
	int n;
	ssize_t ret;
	void *data;
	char *service;
	struct addrinfo *res, *t;
	struct addrinfo hints = {.ai_flags = AI_PASSIVE, .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	static int server_sockfd = -1, server_connfd = -1;

	if (server_connfd >= 0)
		goto connected;

	if (asprintf(&service, "%d", port) < 0)
		return DOCA_ERROR_IO_FAILED;

	n = getaddrinfo(NULL, service, &hints, &res);
	if (n < 0) {
		DOCA_LOG_ERR("%s for port %d", gai_strerror(n), port);
		free(service);
		return DOCA_ERROR_IO_FAILED;
	}

	for (t = res; t; t = t->ai_next) {
		server_sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (server_sockfd >= 0) {
			n = 1;
			if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n) != 0) {
				close(server_sockfd);
				server_sockfd = -1;
				break;
			}

			if (!bind(server_sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(server_sockfd);
			server_sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (server_sockfd < 0) {
		DOCA_LOG_ERR("Couldn't listen to port %d", port);
		return DOCA_ERROR_IO_FAILED;
	}

	listen(server_sockfd, 1);
	server_connfd = accept(server_sockfd, NULL, NULL);
	if (server_connfd < 0) {
		DOCA_LOG_ERR("accept() failed");
		return DOCA_ERROR_IO_FAILED;
	}

connected:
	ret = send(server_connfd, &loc_datalen, sizeof(loc_datalen), 0);
	if (ret < 0 || (size_t)ret != sizeof(loc_datalen)) {
		DOCA_LOG_ERR("Couldn't write local datalen");
		return DOCA_ERROR_IO_FAILED;
	}

	ret = send(server_connfd, loc_data, loc_datalen, 0);
	if (ret < 0 || (size_t)ret != loc_datalen) {
		DOCA_LOG_ERR("Couldn't write local data");
		return DOCA_ERROR_IO_FAILED;
	}

	*rem_datalen = 0;
	ret = recv(server_connfd, rem_datalen, sizeof(*rem_datalen), 0);
	if (ret < 0 || (size_t)ret != sizeof(*rem_datalen)) {
		DOCA_LOG_ERR("%ld/%lu: Couldn't read remote data", ret, sizeof(*rem_datalen));
		return DOCA_ERROR_IO_FAILED;
	}

	if (!*rem_datalen)
		return DOCA_SUCCESS;

	if (*rem_datalen > MAX_WORKER_ADDRESS_LEN) {
		DOCA_LOG_ERR("Received data length greater than the limit %d", MAX_WORKER_ADDRESS_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}

	data = malloc(*rem_datalen);
	if (data == NULL) {
		DOCA_LOG_ERR("Failed to create server exchanged data buffer");
		return DOCA_ERROR_NO_MEMORY;
	}

	ret = recv(server_connfd, data, *rem_datalen, 0);
	if (ret < 0 || (size_t)ret != *rem_datalen) {
		free(data);
		DOCA_LOG_ERR("%ld/%lu: Couldn't read remote data", ret, *rem_datalen);
		return DOCA_ERROR_IO_FAILED;
	}

	*rem_data = data;

	DOCA_LOG_INFO("Server received remote data, length: %lu", *rem_datalen);

	return DOCA_SUCCESS;
}

/*
 * Offloading MR register task
 *
 * @server_ucp_worker [in]: server UCP worker
 * @worker [in]: DOCA UROM worker context
 * @pe [in]: DOCA progress engine
 * @ucp_context [in]: server UCP context
 * @buf [in]: server memory region to register
 * @len [in]: memory length
 * @memh [out]: buffer memory handle
 * @rkey [out]: buffer remote key
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_mr_reg(ucp_worker_h server_ucp_worker,
				struct doca_urom_worker *worker,
				struct doca_pe *pe,
				ucp_context_h ucp_context,
				void *buf,
				size_t len,
				ucp_mem_h *memh,
				uint64_t *rkey)
{
	int ret;
	ucp_mem_h mh;
	void *packed_memh;
	void *packed_rkey;
	doca_error_t result;
	size_t packed_memh_len;
	size_t packed_rkey_len;
	struct rdmo_result res = {0};
	ucs_status_t ucs_status;
	union doca_data cookie = {0};
	ucp_mem_map_params_t mmap_params;
	ucp_memh_pack_params_t pack_params;

	cookie.ptr = &res;

	/* Memory map */
	mmap_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
	mmap_params.address = buf;
	mmap_params.length = len;

	ucs_status = ucp_mem_map(ucp_context, &mmap_params, &mh);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("ucp_mem_map() returned an error [%s]", ucs_status_string(ucs_status));
		return DOCA_ERROR_DRIVER;
	}

	/* Memory rkey pack */
	ucs_status = ucp_rkey_pack(ucp_context, mh, &packed_rkey, &packed_rkey_len);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("ucp_rkey_pack() returned an error [%s]", ucs_status_string(ucs_status));
		result = DOCA_ERROR_DRIVER;
		goto memh_destroy;
	}

	/* Memory memh pack */
	pack_params.field_mask = UCP_MEMH_PACK_PARAM_FIELD_FLAGS;
	pack_params.flags = UCP_MEMH_PACK_FLAG_EXPORT;

	ucs_status = ucp_memh_pack(mh, &pack_params, &packed_memh, &packed_memh_len);
	if (ucs_status == UCS_OK) {
		DOCA_LOG_INFO("ucp_memh_pack() packed length: %lu", packed_memh_len);
	} else if (ucs_status == UCS_ERR_UNSUPPORTED) {
		DOCA_LOG_WARN("ucp_memh_pack() export is not supported");
		packed_memh = NULL;
		packed_memh_len = 0;
	} else {
		DOCA_LOG_ERR("ucp_memh_pack() returned error [%s]", ucs_status_string(ucs_status));
		result = DOCA_ERROR_DRIVER;
		goto packed_rkey_free;
	}

	res.result = DOCA_SUCCESS;
	result = urom_rdmo_task_mr_register(worker,
					    cookie,
					    (uint64_t)buf,
					    len,
					    packed_rkey,
					    packed_rkey_len,
					    packed_memh,
					    packed_memh_len,
					    mr_register_finished);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register server MR");
		goto packed_mh_free;
	}

	do {
		ret = doca_pe_progress(pe);
		ucp_worker_progress(server_ucp_worker);
	} while (ret == 0 && res.result == DOCA_SUCCESS);

	if (res.result != DOCA_SUCCESS) {
		result = res.result;
		goto packed_mh_free;
	}

	*memh = mh;
	*rkey = res.mr_reg.rkey;

	ucp_rkey_buffer_release(packed_rkey);
	ucp_memh_buffer_release(packed_memh, NULL);

	DOCA_LOG_INFO("Allocated rkey: %lu", *rkey);
	return DOCA_SUCCESS;

packed_mh_free:
	ucp_memh_buffer_release(packed_memh, NULL);

packed_rkey_free:
	ucp_rkey_buffer_release(packed_rkey);

memh_destroy:
	if (ucp_mem_unmap(ucp_context, mh) != UCS_OK)
		DOCA_LOG_ERR("Failed to unmap memory handle");
	return result;
}

/*
 * RDMO client init callback
 *
 * @result [in]: task result
 * @cookie [in]: program cookie
 * @addr [in]: device UCP worker address
 * @addr_len [in]: address length
 */
static void client_init_finished(doca_error_t result, union doca_data cookie, void *addr, uint64_t addr_len)
{
	struct rdmo_result *res = (struct rdmo_result *)cookie.ptr;

	if (res == NULL)
		return;

	res->result = result;
	if (result != DOCA_SUCCESS)
		return;

	res->client_init.addr = malloc(addr_len);
	if (res->client_init.addr == NULL) {
		res->result = DOCA_ERROR_NO_MEMORY;
		return;
	}
	memcpy(res->client_init.addr, addr, addr_len);
	res->client_init.addr_len = addr_len;
}

/*
 * RDMO RQ create callback
 *
 * @result [in]: task result
 * @cookie [in]: program cookie
 * @rq_id [in]: RQ id
 */
static void rq_create_finished(doca_error_t result, union doca_data cookie, uint64_t rq_id)
{
	struct rdmo_result *res = (struct rdmo_result *)cookie.ptr;

	if (res == NULL)
		return;

	res->result = result;
	if (result != DOCA_SUCCESS)
		return;

	res->rq_create.rq_id = rq_id;
}

/*
 * RDMO RQ destroy callback
 *
 * @result [in]: task result
 * @cookie [in]: program cookie
 * @rq_id [in]: RQ id
 */
static void rq_destroy_finished(doca_error_t result, union doca_data cookie, uint64_t rq_id)
{
	struct rdmo_result *res = (struct rdmo_result *)cookie.ptr;

	if (res == NULL)
		return;

	res->result = result;
	if (result != DOCA_SUCCESS)
		return;

	res->rq_destroy.rq_id = rq_id;
}

/*
 * RDMO MR deregister callback
 *
 * @result [in]: task result
 * @cookie [in]: program cookie
 * @rkey [in]: MR rkey
 */
static void mr_deregister_finished(doca_error_t result, union doca_data cookie, uint64_t rkey)
{
	struct rdmo_result *res = (struct rdmo_result *)cookie.ptr;

	if (res == NULL)
		return;

	res->result = result;
	if (result != DOCA_SUCCESS)
		return;

	res->mr_dereg.rkey = rkey;
}

/*
 * Offloading MR deregister task
 *
 * @worker [in]: UROM worker context
 * @pe [in]: DOCA progress engine
 * @rkey [in]: MR rkey to deregister
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_deregister_mr(struct doca_urom_worker *worker, struct doca_pe *pe, uint64_t rkey)
{
	int ret;
	doca_error_t result;
	union doca_data cookie;
	struct rdmo_result res = {0};

	cookie.ptr = &res;

	result = urom_rdmo_task_mr_deregister(worker, cookie, rkey, mr_deregister_finished);
	if (result != DOCA_SUCCESS)
		return result;

	do {
		ret = doca_pe_progress(pe);
	} while (ret == 0 && res.result == DOCA_SUCCESS);

	if (res.result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange client address");
		return res.result;
	}

	if (rkey != res.mr_dereg.rkey) {
		DOCA_LOG_ERR("MR deregister notification received wrong rkey");
		return DOCA_ERROR_INVALID_VALUE;
	}
	DOCA_LOG_INFO("Deallocated rkey: %lu", rkey);
	return DOCA_SUCCESS;
}

/*
 * Offloading RQ destroy task
 *
 * @worker [in]: UROM worker context
 * @pe [in]: DOCA progress engine
 * @rq_id [in]: RQ id to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_destroy_rq(struct doca_urom_worker *worker, struct doca_pe *pe, uint64_t rq_id)
{
	int ret;
	doca_error_t result;
	union doca_data cookie;
	struct rdmo_result res = {0};

	cookie.ptr = &res;

	result = urom_rdmo_task_rq_destroy(worker, cookie, rq_id, rq_destroy_finished);
	if (result != DOCA_SUCCESS)
		return result;

	do {
		ret = doca_pe_progress(pe);
	} while (ret == 0 && res.result == DOCA_SUCCESS);

	if (res.result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange client address");
		return res.result;
	}

	if (rq_id != res.rq_destroy.rq_id) {
		DOCA_LOG_ERR("RQ destroy notification received wrong id");
		return DOCA_ERROR_INVALID_VALUE;
	}
	DOCA_LOG_INFO("Destroyed RQ for ID: %lu", rq_id);
	return DOCA_SUCCESS;
}

/*
 * Server wireup function
 *
 * @worker [in]: UROM worker context
 * @pe [in]: DOCA progress engine
 * @port [in]: socket port
 * @ucp_context [out]: set server UCP context
 * @server_ucp_worker [out]: set server UCP worker
 * @rq_id [out]: set client receive queue
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_wireup_server(struct doca_urom_worker *worker,
				       struct doca_pe *pe,
				       int port,
				       ucp_context_h *ucp_context,
				       ucp_worker_h *server_ucp_worker,
				       uint64_t *rq_id)
{
	int ret;
	doca_error_t result;
	uint64_t server_rq_id;
	struct rdmo_result res = {0};
	ucs_status_t ucs_status;
	ucp_config_t *ucp_config;
	ucp_params_t ucp_params;
	union doca_data cookie = {0};
	size_t server_worker_addr_len;
	ucp_address_t *server_worker_addr;
	size_t server_peer_host_addr_len;
	ucp_worker_params_t worker_params;
	ucp_address_t *server_peer_host_addr;

	cookie.ptr = &res;

	ucs_status = ucp_config_read(NULL, NULL, &ucp_config);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to read UCP configuration");
		return DOCA_ERROR_DRIVER;
	}

	/* Create UCP worker for the server */
	ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
	ucp_params.features = UCP_FEATURE_AM | UCP_FEATURE_RMA | UCP_FEATURE_AMO64 | UCP_FEATURE_EXPORTED_MEMH;

	ucs_status = ucp_init(&ucp_params, ucp_config, ucp_context);
	ucp_config_release(ucp_config);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to init UCP layer");
		return DOCA_ERROR_DRIVER;
	}

	worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
	worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

	/* Server worker (receives RDMOs) */
	ucs_status = ucp_worker_create(*ucp_context, &worker_params, server_ucp_worker);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to create server worker");
		result = DOCA_ERROR_DRIVER;
		goto ucp_context_free;
	}

	ucs_status = ucp_worker_get_address(*server_ucp_worker, &server_worker_addr, &server_worker_addr_len);
	if (ucs_status != UCS_OK) {
		DOCA_LOG_ERR("Failed to create server worker");
		result = DOCA_ERROR_DRIVER;
		goto worker_destroy;
	}

	DOCA_LOG_INFO("Created server UCP Worker");

	res.result = DOCA_SUCCESS;
	/* RDMO Client init */
	result = urom_rdmo_task_client_init(worker,
					    cookie,
					    0,
					    server_worker_addr,
					    server_worker_addr_len,
					    client_init_finished);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create client init");
		goto worker_addr_destroy;
	}

	do {
		ret = doca_pe_progress(pe);
		ucp_worker_progress(*server_ucp_worker);
	} while (ret == 0 && res.result == DOCA_SUCCESS);

	if (res.result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Client init task finished with error");
		result = res.result;
		goto worker_addr_destroy;
	}

	DOCA_LOG_INFO("Initialized RDMO client (host addr len: %lu, dev addr len: %lu)",
		      server_worker_addr_len,
		      res.client_init.addr_len);

	/* Exchange worker addresses, give: server urom rdmo worker addr get:  client worker addr */
	result = server_exchange(port,
				 res.client_init.addr,
				 res.client_init.addr_len,
				 (void **)&server_peer_host_addr,
				 &server_peer_host_addr_len);
	free(res.client_init.addr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange client address");
		goto worker_addr_destroy;
	}

	DOCA_LOG_INFO("Received host addr (len: %lu)", server_peer_host_addr_len);

	res.result = DOCA_SUCCESS;
	/* Create RDMO receive queue */
	urom_rdmo_task_rq_create(worker, cookie, server_peer_host_addr, server_peer_host_addr_len, rq_create_finished);

	do {
		ret = doca_pe_progress(pe);
	} while (ret == 0 && res.result == DOCA_SUCCESS);

	if (res.result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange client address");
		free(server_peer_host_addr);
		goto worker_addr_destroy;
	}

	server_rq_id = res.rq_create.rq_id;
	DOCA_LOG_INFO("Created RQ: %#lx", server_rq_id);
	ucp_worker_release_address(*server_ucp_worker, server_worker_addr);
	free(server_peer_host_addr);
	*rq_id = server_rq_id;
	return DOCA_SUCCESS;

worker_addr_destroy:
	ucp_worker_release_address(*server_ucp_worker, server_worker_addr);

worker_destroy:
	ucp_worker_destroy(*server_ucp_worker);

ucp_context_free:
	ucp_cleanup(*ucp_context);

	return result;
}

/*
 * Init server UROM objects
 *
 * @device_name [in]: UROM device name
 * @pe [out]: set DOCA progress engine
 * @service [out]: set DOCA UROM service engine
 * @worker [out]: set DOCA UROM worker engine
 * @dev [out]: set DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_server_urom_init(char *device_name,
					  struct doca_pe **pe,
					  struct doca_urom_service **service,
					  struct doca_urom_worker **worker,
					  struct doca_dev **dev)
{
	size_t i, plugins_count = 0;
	char *plugin_name = "worker_rdmo";
	const struct doca_urom_service_plugin_info *plugins, *rdmo_info = NULL;
	doca_error_t result, tmp_result;
	enum doca_ctx_states state;

	/* UROM service create and connect */
	result = open_doca_device_with_ibdev_name((uint8_t *)device_name, strlen(device_name), NULL, dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_pe_create(pe);
	if (result != DOCA_SUCCESS)
		goto close_dev;

	result = start_urom_service(*pe, *dev, 2, service);
	if (result != DOCA_SUCCESS)
		goto pe_cleanup;

	result = doca_urom_service_get_plugins_list(*service, &plugins, &plugins_count);
	if (result != DOCA_SUCCESS || plugins_count == 0)
		goto service_stop;

	for (i = 0; i < plugins_count; i++) {
		if (strcmp(plugin_name, plugins[i].plugin_name) == 0) {
			rdmo_info = &plugins[i];
			break;
		}
	}

	if (rdmo_info == NULL) {
		DOCA_LOG_ERR("Failed to match RDMO plugin");
		result = DOCA_ERROR_INVALID_VALUE;
		goto service_stop;
	}

	result = urom_rdmo_init(rdmo_info->id, rdmo_info->version);
	if (result != DOCA_SUCCESS)
		goto service_stop;

	/* Create and start worker context */
	result =
		start_urom_worker(*pe, *service, DOCA_UROM_WORKER_ID_ANY, NULL, 16, NULL, NULL, 0, rdmo_info->id, worker);
	if (result != DOCA_SUCCESS)
		goto service_stop;

	/* Loop till worker state changes to running */
	do {
		doca_pe_progress(*pe);
		result = doca_ctx_get_state(doca_urom_worker_as_ctx(*worker), &state);
	} while (state == DOCA_CTX_STATE_STARTING && result == DOCA_SUCCESS);

	if (state != DOCA_CTX_STATE_RUNNING || result != DOCA_SUCCESS)
		goto worker_cleanup;

	return DOCA_SUCCESS;

worker_cleanup:
	tmp_result = doca_urom_worker_destroy(*worker);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM worker");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

service_stop:
	tmp_result = doca_ctx_stop(doca_urom_service_as_ctx(*service));
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop UROM service");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_urom_service_destroy(*service);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM service");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

pe_cleanup:
	tmp_result = doca_pe_destroy(*pe);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy PE");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

close_dev:
	tmp_result = doca_dev_close(*dev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Destroy server UROM objects
 *
 * @pe [in]: DOCA progress engine
 * @service [in]: DOCA UROM service engine
 * @worker [in]: DOCA UROM worker engine
 * @dev [in]: DOCA UROM device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdmo_server_urom_destroy(struct doca_pe *pe,
					     struct doca_urom_service *service,
					     struct doca_urom_worker *worker,
					     struct doca_dev *dev)
{
	int ret = 0;
	enum doca_ctx_states state;
	doca_error_t tmp_result, result = DOCA_SUCCESS;

	tmp_result = doca_ctx_stop(doca_urom_worker_as_ctx(worker));
	if (tmp_result != DOCA_SUCCESS && tmp_result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to request stop UROM worker");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	do {
		ret += doca_pe_progress(pe);
		tmp_result = doca_ctx_get_state(doca_urom_worker_as_ctx(worker), &state);
	} while (state != DOCA_CTX_STATE_IDLE);

	if (ret == 0 || tmp_result != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(result, tmp_result);

	tmp_result = doca_urom_worker_destroy(worker);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM worker");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	tmp_result = doca_ctx_stop(doca_urom_service_as_ctx(service));
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop UROM service");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_urom_service_destroy(service);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM service");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	tmp_result = doca_pe_destroy(pe);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy PE");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	tmp_result = doca_dev_close(dev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close device");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*******************************************************************************
 * Common server and client functions
 ******************************************************************************/

/*
 * Client-Server barrier
 *
 * @server_name [in]: server hostname
 * @port [in]: socket port
 * @mode [in]: client or server mode
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t cs_barrier(const char *server_name, int port, enum rdmo_mode mode)
{
	size_t len;

	if (mode == RDMO_MODE_SERVER)
		return server_exchange(port, NULL, 0, NULL, &len);
	else
		return client_exchange(server_name, port, NULL, 0, NULL, &len);
}

/*
 * ARGP Callback - Handle mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mode_callback(void *param, void *config)
{
	struct rdmo_cfg *rdmo_cfg = (struct rdmo_cfg *)config;
	char *mode = (char *)param;

	if (rdmo_cfg->mode != RDMO_MODE_UNKNOWN)
		return DOCA_ERROR_BAD_STATE;

	if (strcmp("client", mode) == 0)
		rdmo_cfg->mode = RDMO_MODE_CLIENT;
	else if (strcmp("server", mode) == 0)
		rdmo_cfg->mode = RDMO_MODE_SERVER;
	else {
		DOCA_LOG_ERR("Invalid mode type [%s]", mode);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle server name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t server_address_callback(void *param, void *config)
{
	struct rdmo_cfg *rdmo_cfg = (struct rdmo_cfg *)config;
	char *server_name = (char *)param;
	int len;

	len = strnlen(server_name, HOST_NAME_MAX);
	if (len == HOST_NAME_MAX) {
		DOCA_LOG_ERR("Entered server name exceeding the maximum size of %d", HOST_NAME_MAX - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(rdmo_cfg->server_name, server_name, HOST_NAME_MAX);

	return DOCA_SUCCESS;
}

/*******************************************************************************
 * External server and client functions
 ******************************************************************************/
doca_error_t rdmo_server(char *device_name)
{
	ucp_mem_h memh;
	size_t bytes_sent = 8;
	int port = 18515;
	uint64_t rq_id = 0, expected_ptr;
	bool succeeded = true;
	void *queue_buf = NULL;
	ucp_context_h ucp_context;
	struct rbuf_desc rbuf_desc;
	char *byte, data_val = 0x33;
	size_t queue_len = 128 * 1024;
	ucp_worker_h server_ucp_worker;
	doca_error_t result, tmp_result;
	size_t i, *queue_ptr, send_len = 8;
	uint64_t rbuf_desc_len, rkey = 0;
	/* DOCA UROM objects */
	struct doca_pe *pe;
	struct doca_dev *dev;
	struct doca_urom_service *service;
	struct doca_urom_worker *worker;

	if (device_name == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Create UROM objects */
	result = rdmo_server_urom_init(device_name, &pe, &service, &worker, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize UROM objects");
		return result;
	}

	/* Server wireup */
	result = rdmo_wireup_server(worker, pe, port, &ucp_context, &server_ucp_worker, &rq_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to execute server wireup");
		goto destroy_urom;
	}

	/* buffer setup */
	queue_buf = calloc(1, queue_len);
	if (queue_buf == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto ucp_destroy;
	}

	queue_ptr = queue_buf;
	*queue_ptr = (uint64_t)(queue_ptr + 1);

	result = rdmo_mr_reg(server_ucp_worker, worker, pe, ucp_context, queue_buf, queue_len, &memh, &rkey);
	if (result != DOCA_SUCCESS)
		goto queue_free;

	/* Exchange rkey */
	rbuf_desc.rkey = rkey;
	rbuf_desc.raddr = queue_ptr;

	DOCA_LOG_INFO("Sent rkey %lu and queue pointer %p", rkey, queue_ptr);

	result = server_exchange(port, &rbuf_desc, sizeof(rbuf_desc), NULL, &rbuf_desc_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange server data");
		goto memh_unmap;
	}

	/* Worker progress append and flush */
	expected_ptr = (uintptr_t)queue_buf + sizeof(queue_ptr) + bytes_sent;
	while (*queue_ptr != expected_ptr) {
		ucp_worker_progress(server_ucp_worker);
		sched_yield();
	}

	succeeded = true;
	byte = (char *)&queue_ptr[1];
	for (i = 0; i < bytes_sent; i++) {
		if (byte[i] != data_val) {
			succeeded = false;
			DOCA_LOG_ERR("Append bad data[%ld]: %#x, expected: %#x", i, byte[i], data_val);
		}
	}

	if (!succeeded) {
		DOCA_LOG_ERR("Append operation failed");
		result = DOCA_ERROR_BAD_STATE;
		goto memh_unmap;
	} else
		DOCA_LOG_INFO("Append operation was finished successfully");

	/* One additional progress for flush operation */
	ucp_worker_progress(server_ucp_worker);

	memset(queue_ptr, 0, queue_len);

	/* Client-Server barrier */
	cs_barrier(NULL, port, RDMO_MODE_SERVER);

	/* Worker progress scatter and flush */
	byte = (char *)queue_ptr;
	while (byte[send_len - 1] != data_val) {
		ucp_worker_progress(server_ucp_worker);
		sched_yield();
	}

	succeeded = true;
	for (i = 0; i < send_len; i++) {
		if (byte[i] != data_val) {
			succeeded = false;
			DOCA_LOG_ERR("Scatter Bad data[%ld]: %#x, expected: %#x", i, byte[i], data_val);
		}
	}

	if (!succeeded) {
		DOCA_LOG_ERR("Scatter operation failed");
		result = DOCA_ERROR_BAD_STATE;
		goto memh_unmap;
	} else
		DOCA_LOG_INFO("Scatter operation was finished successfully");

	result = DOCA_SUCCESS;

memh_unmap:
	ucp_mem_unmap(ucp_context, memh);
	rdmo_deregister_mr(worker, pe, rkey);
queue_free:
	free(queue_buf);
ucp_destroy:
	tmp_result = rdmo_destroy_rq(worker, pe, rq_id);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy RQ");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	ucp_worker_destroy(server_ucp_worker);
	ucp_cleanup(ucp_context);
destroy_urom:
	tmp_result = rdmo_server_urom_destroy(pe, service, worker, dev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy UROM objects");
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t rdmo_client(char *server_name)
{
	size_t send_len;
	doca_error_t result;
	char data_val = 0x33;
	void *send_buf = NULL;
	ucp_ep_h client_ucp_ep;
	ucp_worker_h ucp_worker;
	ucp_context_h ucp_context;
	size_t queue_len = 128 * 1024;
	struct rbuf_desc *rbuf_desc = NULL;
	int flushed = 0, port = 18515;
	uint64_t rkey, rbuf_desc_len, *queue_ptr;
	int scatter_chunk_size = 8, scatter_chunks = 16;

	/* Client wireup */
	result = rdmo_wireup_client(server_name, port, &client_ucp_ep, &ucp_worker, &flushed, &ucp_context);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("rdmo_wireup_client() returned error");
		return result;
	}

	/* Create send buffer */
	send_buf = calloc(1, queue_len);
	if (send_buf == NULL) {
		DOCA_LOG_ERR("Failed to allocate send buffer memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto ucp_destroy;
	}

	memset(send_buf, data_val, queue_len);
	DOCA_LOG_INFO("Send buffer contains [%c] char", data_val);

	/* Exchange rkey */
	result = client_exchange(server_name, port, NULL, 0, (void **)&rbuf_desc, &rbuf_desc_len);
	if (result != DOCA_SUCCESS || rbuf_desc == NULL) {
		DOCA_LOG_ERR("Failed to exchange client data");
		goto free_buf;
	}

	rkey = rbuf_desc->rkey;
	queue_ptr = rbuf_desc->raddr;
	free(rbuf_desc);
	DOCA_LOG_INFO("Received rkey %lu and queue pointer %p", rkey, queue_ptr);

	/* Set send buffer length */
	send_len = 8;

	/* RDMO append operation */
	result = rdmo_append(ucp_worker, client_ucp_ep, queue_ptr, send_buf, send_len, rkey);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start append RDMO op");
		goto free_buf;
	}

	/* RDMO flush operation */
	result = rdmo_flush(ucp_worker, client_ucp_ep, &flushed);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start flush RDMO op");
		goto free_buf;
	}

	/* Client-Server barrier */
	result = cs_barrier(server_name, port, RDMO_MODE_CLIENT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to execute barrier between client and server");
		goto free_buf;
	}

	/* RDMO scatter operation */
	send_len = scatter_chunk_size * scatter_chunks;

	result = rdmo_scatter(ucp_worker,
			      client_ucp_ep,
			      (uint64_t)queue_ptr,
			      send_buf,
			      send_len,
			      scatter_chunk_size,
			      rkey);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start scatter RDMO op");
		goto free_buf;
	}

	/* RDMO flush operation */
	result = rdmo_flush(ucp_worker, client_ucp_ep, &flushed);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start flush RDMO op");
		goto free_buf;
	}

	return DOCA_SUCCESS;

free_buf:
	free(send_buf);

ucp_destroy:
	rdmo_ucp_client_destroy(client_ucp_ep, ucp_worker, ucp_context);

	return result;
}

doca_error_t register_urom_rdmo_params(void)
{
	doca_error_t result;
	struct doca_argp_param *server_name, *mode;

	result = register_urom_common_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register UROM common param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register server name param */
	result = doca_argp_param_create(&server_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(server_name, "s");
	doca_argp_param_set_long_name(server_name, "server-name");
	doca_argp_param_set_arguments(server_name, "<server name>");
	doca_argp_param_set_description(server_name, "server name.");
	doca_argp_param_set_callback(server_name, server_address_callback);
	doca_argp_param_set_type(server_name, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(server_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register server name param */
	result = doca_argp_param_create(&mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(mode, "m");
	doca_argp_param_set_long_name(mode, "mode");
	doca_argp_param_set_arguments(mode, "{server, client}");
	doca_argp_param_set_description(mode, "Set mode type {server, client}");
	doca_argp_param_set_callback(mode, mode_callback);
	doca_argp_param_set_type(mode, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(mode);
	result = doca_argp_register_param(mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}
