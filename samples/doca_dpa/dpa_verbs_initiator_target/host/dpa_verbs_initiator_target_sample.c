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

#include <sys/socket.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include <doca_rdma_bridge.h>
#include <doca_verbs.h>
#include <doca_verbs_bridge.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_umem.h>
#include <doca_uar.h>
#include <doca_sync_event.h>

#include "common.h"
#include "dpa_common.h"
#include "../common/dpa_verbs_initiator_target_common_defs.h"

DOCA_LOG_REGISTER(DPA_VERBS::SAMPLE);

/**
 * DPA sample application
 */
extern struct doca_dpa_app *dpa_sample_app;

/**
 * Sync event mask
 */
#define SYNC_EVENT_MASK_FFS (0xFFFFFFFFFFFFFFFF)
/**
 * Max IP address length
 */
#define MAX_IP_ADDRESS_LEN (128)
/**
 * Sample's sync event wait threshold
 */
#define VERBS_SAMPLE_EVENT_WAIT_THRESHOLD (9)
/**
 * QP queue size
 */
#define VERBS_SAMPLE_QUEUE_SIZE (4)
/**
 * Socket Port used for communication
 */
#define VERBS_SAMPLE_SIN_PORT (5000)
/**
 * Sample's Hop limit
 */
#define VERBS_SAMPLE_HOP_LIMIT (255)
/**
 * Sample's DBR size
 */
#define VERBS_SAMPLE_DBR_SIZE (64)
/**
 * Max send scatter\gather elements
 */
#define VERBS_SAMPLE_MAX_SEND_SEGS (1)
/**
 * Max receive scatter\gather element
 */
#define VERBS_SAMPLE_MAX_RECEIVE_SEGS (1)
/**
 * Log WQEBB size
 */
#define VERBS_SAMPLE_LOG_WQEBB_SIZE (6)
/**
 * WQEBB size
 */
#define VERBS_SAMPLE_WQEBB_SIZE (1U << VERBS_SAMPLE_LOG_WQEBB_SIZE)
/**
 * Cacheline size
 */
#define VERBS_SAMPLE_CACHELINE_SIZE (64)

/**
 * kernel/RPC declaration
 */
doca_dpa_func_t initiator_thread_kernel;
doca_dpa_func_t initiator_trigger_first_iteration_rpc;
doca_dpa_func_t target_thread_kernel;
doca_dpa_func_t target_trigger_first_iteration_rpc;

/**
 * Verbs Sample's Configuration Struct
 */
struct verbs_config {
	char pf_device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* PF DOCA device name */
	char sf_device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* SF DOCA device name */
	char target_ip_addr[MAX_IP_ADDRESS_LEN];	   /* Target ip address */
	bool is_target;					   /* Sample is acting as initiator or target */
	uint32_t gid_index;				   /* GID index */
};

/**
 * Verbs Sample's Resources Struct
 */
struct verbs_resources {
	struct verbs_config *cfg;	    /* Verbs sample configuration parameters */
	struct doca_dev *pf_dev;	    /* PF DOCA device */
	struct doca_dev *dev;		    /* DOCA device to use, when running from host it will be PF DOCA device
						    and when running from DPU, it will be SF DOCA device */
	struct doca_dpa *pf_dpa_ctx;	    /* PF DOCA DPA context */
	struct doca_dpa *dpa_ctx;	    /* DOCA DPA context to use, when running from host it will be pf_dpa_ctx
						    and when running from DPU, it will be an extended dpa_ctx */
	doca_dpa_dev_t dpa_ctx_handle;	    /* DOCA DPA context handle */
	struct doca_sync_event *comp_event; /* DOCA completion sync event */
	doca_dpa_dev_sync_event_t comp_event_handle;	/* DOCA completion sync event handle */
	doca_dpa_dev_uintptr_t dpa_thread_arg_dev_ptr;	/* DOCA DPA thread arguments handle */
	struct doca_dpa_thread *dpa_thread;		/* DOCA DPA thread */
	struct doca_verbs_context *verbs_context;	/* DOCA Verbs Context */
	struct doca_verbs_qp *verbs_qp;			/* DOCA Verbs Queue Pair */
	struct doca_verbs_pd *verbs_pd;			/* DOCA Verbs Protection Domain */
	struct doca_verbs_ah_attr *verbs_ah_attr;	/* DOCA Verbs Address Handle attribute */
	struct doca_umem *dpa_qp_umem;			/* DOCA DPA QP umem */
	doca_dpa_dev_uintptr_t dpa_qp_umem_dev_ptr;	/* DOCA DPA QP umem handle */
	struct doca_umem *dpa_qp_dbr_umem;		/* DOCA DPA QP dbr umem */
	doca_dpa_dev_uintptr_t dpa_qp_dbr_umem_dev_ptr; /* DOCA DPA QP dbr umem handle */
	struct doca_uar *dpa_uar;			/* DOCA DPA uar */
	int conn_socket;				/* Connection socket fd */
	uint32_t local_qp_number;			/* Local QP number */
	uint32_t remote_qp_number;			/* Remote QP number */
	struct doca_verbs_gid gid;			/* local gid address */
	struct doca_verbs_gid remote_gid;		/* remote gid address */
	doca_dpa_dev_verbs_qp_t verbs_qp_handle;	/* DOCA Verbs QP handle */
	struct dpa_completion_obj dpa_completion_obj;	/* DOCA DPA completion object */
	doca_dpa_dev_uintptr_t local_dpa_buff_addr;	/* DOCA DPA local buffer address */
	doca_dpa_dev_uintptr_t remote_dpa_buff_addr;	/* DOCA DPA remote buffer address */
	struct doca_mmap_obj local_buff_mmap_obj;	/* DOCA DPA local buffer mmap object */
	doca_dpa_dev_mmap_t remote_dpa_mmap_handle;	/* DOCA DPA remote buffer mmap handle */
};

/*
 * Setup client connection
 *
 * @server_ip [in]: server IP address
 * @client_sock_fd [out]: client socket file descriptor
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t connection_client_setup(const char *server_ip, int *client_sock_fd)
{
	struct sockaddr_in socket_addr = {0};
	int client_fd;

	client_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (client_fd < 0) {
		DOCA_LOG_ERR("Failed to create socket");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	socket_addr.sin_family = AF_INET;
	socket_addr.sin_port = htons(VERBS_SAMPLE_SIN_PORT);

	if (inet_pton(AF_INET, server_ip, &(socket_addr.sin_addr)) <= 0) {
		close(client_fd);
		DOCA_LOG_ERR("inet_pton error occurred");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (connect(client_fd, (struct sockaddr *)&socket_addr, sizeof(socket_addr)) < 0) {
		close(client_fd);
		DOCA_LOG_ERR("Unable to connect to server at %s", server_ip);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Client has successfully connected to the server");

	*client_sock_fd = client_fd;
	return DOCA_SUCCESS;
}

/*
 * Setup server connection
 *
 * @server_sock_fd [out]: server socket file descriptor
 * @conn_socket [out]: connection socket
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t connection_server_setup(int *server_sock_fd, int *conn_socket)
{
	struct sockaddr_in socket_addr = {0}, client_addr = {0};
	int addrlen = sizeof(client_addr);
	int opt = 1;
	int server_fd = 0;
	int new_socket = 0;
	char client_ip[INET_ADDRSTRLEN];

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) {
		DOCA_LOG_ERR("Failed to create socket %d", server_fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
		DOCA_LOG_ERR("Failed to set socket options");
		close(server_fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		DOCA_LOG_ERR("Failed to set socket options");
		close(server_fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	socket_addr.sin_family = AF_INET;
	socket_addr.sin_port = htons(VERBS_SAMPLE_SIN_PORT);
	socket_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(server_fd, (struct sockaddr *)&socket_addr, sizeof(socket_addr)) < 0) {
		DOCA_LOG_ERR("Failed to bind port");
		close(server_fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (listen(server_fd, 1) < 0) {
		DOCA_LOG_ERR("Failed to listen");
		close(server_fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	DOCA_LOG_INFO("Server is listening for incoming connections");

	new_socket = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&addrlen);
	if (new_socket < 0) {
		DOCA_LOG_ERR("Failed to accept connection %d", new_socket);
		close(server_fd);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
	DOCA_LOG_INFO("Server is connected to client at IP: %s and port: %i", client_ip, ntohs(socket_addr.sin_port));

	*(server_sock_fd) = server_fd;
	*(conn_socket) = new_socket;

	return DOCA_SUCCESS;
}

/*
 * Close client's oob connection
 *
 * @oob_sock_fd [in]: client's oob socket file descriptor
 */
static void oob_connection_client_close(int oob_sock_fd)
{
	if (oob_sock_fd > 0) {
		close(oob_sock_fd);
	}
}

/*
 * Close server's oob connection
 *
 * @oob_sock_fd [in]: server's oob socket file descriptor
 * @oob_client_sock [in]: client's oob socket file descriptor
 */
static void oob_connection_server_close(int oob_sock_fd, int oob_client_sock)
{
	if (oob_client_sock > 0) {
		close(oob_client_sock);
	}

	if (oob_sock_fd > 0) {
		close(oob_sock_fd);
	}
}

/*
 * Create verbs AH
 *
 * @verbs_context [in]: verbs context
 * @gid_index [in]: gid index
 * @addr_type [in]: address type
 * @verbs_ah_attr [out]: verbs AH attribute
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_verbs_ah_attr(struct doca_verbs_context *verbs_context,
					 uint32_t gid_index,
					 enum doca_verbs_addr_type addr_type,
					 struct doca_verbs_ah_attr **verbs_ah_attr)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_verbs_ah_attr *new_ah_attr = NULL;

	status = doca_verbs_ah_attr_create(verbs_context, &new_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs ah: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_ah_attr_set_addr_type(new_ah_attr, addr_type);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address type: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	status = doca_verbs_ah_attr_set_sgid_index(new_ah_attr, gid_index);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set sgid index: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	status = doca_verbs_ah_attr_set_hop_limit(new_ah_attr, VERBS_SAMPLE_HOP_LIMIT);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set hop limit: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah;
	}

	*verbs_ah_attr = new_ah_attr;

	return DOCA_SUCCESS;

destroy_verbs_ah:
	tmp_status = doca_verbs_ah_attr_destroy(new_ah_attr);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy doca verbs AH: %s", doca_error_get_descr(tmp_status));

	return status;
}

/*
 * Calculate QP external umem size
 *
 * @rq_size [in]: receive queue size
 * @sq_size [in]: send queue size
 * @return: umem size
 */
static uint32_t calc_qp_external_umem_size(uint32_t rq_size, uint32_t sq_size)
{
	uint32_t rq_ring_size = 0;
	uint32_t sq_ring_size = 0;

	if (rq_size != 0)
		rq_ring_size = (uint32_t)(next_power_of_two(rq_size) * sizeof(struct mlx5_wqe_data_seg));
	if (sq_size != 0)
		sq_ring_size = (uint32_t)(next_power_of_two(sq_size) * VERBS_SAMPLE_WQEBB_SIZE);

	return align_up_uint32(rq_ring_size + sq_ring_size, VERBS_SAMPLE_CACHELINE_SIZE);
}

/*
 * Create verbs QP
 *
 * @verbs_context [in]: verbs context
 * @dpa_ctx [in]: DPA context
 * @verbs_pd [in]: verbs pd
 * @dpa_completion [in]: DPA completion
 * @dpa_uar [in]: DPA UAR
 * @qp_rq_wr [in]: QP receive queue work request
 * @qp_sq_wr [in]: QP send queue work request
 * @dpa_umem_dev_ptr [in]: DPA umem pointer
 * @dpa_umem [in]: DPA umem
 * @dpa_dbr_umem_dev_ptr [in]: DPA dbr umem pointer
 * @dpa_dbr_umem [in]:  DPA dbr umem
 * @verbs_qp [out]: verbs QP
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_verbs_qp(struct doca_verbs_context *verbs_context,
				    struct doca_dpa *dpa_ctx,
				    struct doca_verbs_pd *verbs_pd,
				    struct doca_dpa_completion *dpa_completion,
				    struct doca_uar *dpa_uar,
				    uint32_t qp_rq_wr,
				    uint32_t qp_sq_wr,
				    doca_dpa_dev_uintptr_t *dpa_umem_dev_ptr,
				    struct doca_umem **dpa_umem,
				    doca_dpa_dev_uintptr_t *dpa_dbr_umem_dev_ptr,
				    struct doca_umem **dpa_dbr_umem,
				    struct doca_verbs_qp **verbs_qp)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_verbs_qp_init_attr *verbs_qp_init_attr = NULL;
	struct doca_verbs_qp *new_qp = NULL;
	uint32_t external_umem_size = 0;

	status = doca_verbs_qp_init_attr_create(&verbs_qp_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs qp attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_qp_init_attr_set_external_datapath_en(verbs_qp_init_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs external datapath en: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	external_umem_size = calc_qp_external_umem_size(qp_rq_wr, qp_sq_wr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to calc external umem size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_mem_alloc(dpa_ctx, external_umem_size, dpa_umem_dev_ptr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc dpa memory for external umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_umem_dpa_create(dpa_ctx,
				      *dpa_umem_dev_ptr,
				      external_umem_size,
				      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					      DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC,
				      dpa_umem);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create dpa umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_external_umem(verbs_qp_init_attr, *dpa_umem, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs qp external umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_mem_alloc(dpa_ctx, VERBS_SAMPLE_DBR_SIZE, dpa_dbr_umem_dev_ptr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc dpa memory for external dbr umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_umem_dpa_create(dpa_ctx,
				      *dpa_dbr_umem_dev_ptr,
				      VERBS_SAMPLE_DBR_SIZE,
				      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
					      DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_ATOMIC,
				      dpa_dbr_umem);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create dpa dbr umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_external_dbr_umem(verbs_qp_init_attr, *dpa_dbr_umem, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs qp external dbr umem: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_external_uar(verbs_qp_init_attr, dpa_uar);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs qp external uar: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_pd(verbs_qp_init_attr, verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs PD: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_sq_wr(verbs_qp_init_attr, qp_sq_wr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_rq_wr(verbs_qp_init_attr, qp_rq_wr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ size: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_qp_type(verbs_qp_init_attr, DOCA_VERBS_QP_TYPE_RC);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set QP type: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_send_dpa_completion(verbs_qp_init_attr, dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs CQ number: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_receive_dpa_completion(verbs_qp_init_attr, dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca verbs CQ number: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_send_max_sges(verbs_qp_init_attr, VERBS_SAMPLE_MAX_SEND_SEGS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set send_max_sges: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_set_receive_max_sges(verbs_qp_init_attr, VERBS_SAMPLE_MAX_RECEIVE_SEGS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set receive_max_sges: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_create(verbs_context, verbs_qp_init_attr, &new_qp);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs QP: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_verbs_qp_init_attr_destroy(verbs_qp_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca verbs QP attributes: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	*verbs_qp = new_qp;

	return DOCA_SUCCESS;

destroy_resources:
	if (verbs_qp_init_attr != NULL) {
		tmp_status = doca_verbs_qp_init_attr_destroy(verbs_qp_init_attr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy doca verbs QP attributes: %s",
				     doca_error_get_descr(tmp_status));
	}

	if (*dpa_dbr_umem != NULL) {
		tmp_status = doca_umem_destroy(*dpa_dbr_umem);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy dpa dbr umem: %s", doca_error_get_descr(tmp_status));
	}

	if (*dpa_dbr_umem_dev_ptr != 0) {
		tmp_status = doca_dpa_mem_free(dpa_ctx, *dpa_dbr_umem_dev_ptr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy dpa memory of dbr umem: %s", doca_error_get_descr(tmp_status));
	}

	if (*dpa_umem != NULL) {
		tmp_status = doca_umem_destroy(*dpa_umem);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy dpa umem: %s", doca_error_get_descr(tmp_status));
	}

	if (*dpa_umem_dev_ptr != 0) {
		tmp_status = doca_dpa_mem_free(dpa_ctx, *dpa_umem_dev_ptr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy dpa memory of umem: %s", doca_error_get_descr(tmp_status));
	}

	if (new_qp != NULL) {
		tmp_status = doca_verbs_qp_destroy(new_qp);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy doca verbs QP: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

/*
 * Create completion sync event
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_completion_sync_event(struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;

	status = doca_sync_event_create(&(resources->comp_event));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca sync event: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_sync_event_add_publisher_location_dpa(resources->comp_event, resources->pf_dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set dpa as publisher for doca sync event: %s", doca_error_get_descr(status));
		goto destroy_comp_event;
	}

	status = doca_sync_event_add_subscriber_location_cpu(resources->comp_event, resources->pf_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cpu as subscriber for doca sync event: %s", doca_error_get_descr(status));
		goto destroy_comp_event;
	}

	status = doca_sync_event_start(resources->comp_event);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca sync event: %s", doca_error_get_descr(status));
		goto destroy_comp_event;
	}

	status = doca_sync_event_get_dpa_handle(resources->comp_event,
						resources->pf_dpa_ctx,
						&(resources->comp_event_handle));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca sync event dpa handle: %s", doca_error_get_descr(status));
		goto destroy_comp_event;
	}

	return status;

destroy_comp_event:
	tmp_status = doca_sync_event_destroy(resources->comp_event);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca sync event: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

/*
 * Open verbs context, verbs pd and doca device from device name
 *
 * @device_name [in]: device name
 * @verbs_ctx [out]: verbs context
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t open_verbs_resources(char *device_name,
					 struct doca_verbs_context **verbs_ctx,
					 struct doca_verbs_pd **verbs_pd,
					 struct doca_dev **dev)
{
	struct doca_devinfo **devinfo_list = NULL;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
	uint32_t nb_devs = 0;
	doca_error_t status = DOCA_SUCCESS;

	status = doca_devinfo_create_list(&devinfo_list, &nb_devs);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create devinfo list: %s", doca_error_get_descr(status));
		return status;
	}

	/* Search for the requested device */
	for (uint32_t i = 0; i < nb_devs; i++) {
		status = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
		if (status == DOCA_SUCCESS && (strcmp(ibdev_name, device_name) == 0)) {
			status = doca_verbs_context_create(devinfo_list[i],
							   DOCA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
							   verbs_ctx);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create verbs context: %s", doca_error_get_descr(status));
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}

			status = doca_verbs_pd_create(*verbs_ctx, verbs_pd);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create verbs pd: %s", doca_error_get_descr(status));
				(void)doca_verbs_context_destroy(*verbs_ctx);
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}

			status = doca_verbs_pd_as_doca_dev(*verbs_pd, dev);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create doca dev: %s", doca_error_get_descr(status));
				(void)doca_verbs_pd_destroy(*verbs_pd);
				(void)doca_verbs_context_destroy(*verbs_ctx);
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}

			break;
		}
	}

	status = doca_devinfo_destroy_list(devinfo_list);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_descr(status));
		if (*verbs_ctx != NULL) {
			(void)doca_dev_close(*dev);
			(void)doca_verbs_pd_destroy(*verbs_pd);
			(void)doca_verbs_context_destroy(*verbs_ctx);
		}
		return status;
	}

	if (*verbs_ctx == NULL) {
		DOCA_LOG_ERR("The requested device was not found");
		return DOCA_ERROR_NOT_FOUND;
	}

	return DOCA_SUCCESS;
}

#ifdef DOCA_ARCH_DPU
/*
 * Open doca device from device name (without verbs resources)
 *
 * @device_name [in]: device name
 * @dev [out]: doca device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_doca_device(const char *device_name, struct doca_dev **dev)
{
	struct doca_devinfo **devinfo_list = NULL;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
	uint32_t nb_devs = 0;
	doca_error_t status;

	status = doca_devinfo_create_list(&devinfo_list, &nb_devs);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create devinfo list: %s", doca_error_get_descr(status));
		return status;
	}

	/* Search for the requested device */
	for (uint32_t i = 0; i < nb_devs; i++) {
		status = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
		if (status == DOCA_SUCCESS && (strcmp(ibdev_name, device_name) == 0)) {
			status = doca_dev_open(devinfo_list[i], dev);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to open doca device: %s", doca_error_get_descr(status));
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}
			break;
		}
	}

	status = doca_devinfo_destroy_list(devinfo_list);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_descr(status));
		if (*dev != NULL)
			(void)doca_dev_close(*dev);
		return status;
	}

	if (*dev == NULL) {
		DOCA_LOG_ERR("The requested device was not found: %s", device_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	return DOCA_SUCCESS;
}
#endif /* DOCA_ARCH_DPU */

/*
 * Destroy local resources
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t destroy_local_resources(struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS;

	if (resources->comp_event) {
		status = doca_sync_event_destroy(resources->comp_event);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca sync event: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_qp) {
		if (resources->dpa_qp_dbr_umem != NULL) {
			status = doca_umem_destroy(resources->dpa_qp_dbr_umem);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy dpa qp dbr umem: %s", doca_error_get_descr(status));
				return status;
			}
		}

		if (resources->dpa_qp_dbr_umem_dev_ptr != 0) {
			status = doca_dpa_mem_free(resources->dpa_ctx, resources->dpa_qp_dbr_umem_dev_ptr);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy dpa memory of qp dbr: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}

		if (resources->dpa_qp_umem != NULL) {
			status = doca_umem_destroy(resources->dpa_qp_umem);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy dpa qp umem: %s", doca_error_get_descr(status));
				return status;
			}
		}

		if (resources->dpa_qp_umem_dev_ptr != 0) {
			status = doca_dpa_mem_free(resources->dpa_ctx, resources->dpa_qp_umem_dev_ptr);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy dpa memory of qp ring buffer: %s",
					     doca_error_get_descr(status));
				return status;
			}
		}

		status = doca_verbs_qp_destroy(resources->verbs_qp);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs QP: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_ah_attr) {
		status = doca_verbs_ah_attr_destroy(resources->verbs_ah_attr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs AH: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dpa_completion_obj.dpa_comp) {
		status = dpa_completion_obj_destroy(&resources->dpa_completion_obj);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca dpa completion obj %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dpa_uar) {
		status = doca_uar_destroy(resources->dpa_uar);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca uar: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dpa_thread) {
		status = doca_dpa_thread_destroy(resources->dpa_thread);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca dpa thread: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dpa_thread_arg_dev_ptr) {
		status = doca_dpa_mem_free(resources->dpa_ctx, resources->dpa_thread_arg_dev_ptr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca dpa thread argument memory: %s",
				     doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->local_buff_mmap_obj.dpa_mmap_handle) {
		status = doca_mmap_obj_destroy(&resources->local_buff_mmap_obj);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca mmap object: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->local_dpa_buff_addr) {
		status = doca_dpa_mem_free(resources->dpa_ctx, resources->local_dpa_buff_addr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca dpa local buff memory: %s", doca_error_get_descr(status));
			return status;
		}
	}

#ifdef DOCA_ARCH_DPU
	if (resources->dpa_ctx) {
		status = doca_dpa_destroy(resources->dpa_ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy extended doca dpa context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dev) {
		status = doca_dev_close(resources->dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close sf device: %s", doca_error_get_descr(status));
			return status;
		}
	}
#endif

	if (resources->pf_dpa_ctx) {
		status = doca_dpa_destroy(resources->pf_dpa_ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy pf doca dpa context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->pf_dev) {
		status = doca_dev_close(resources->pf_dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close pf device: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_pd) {
		status = doca_verbs_pd_destroy(resources->verbs_pd);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs PD: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_context) {
		status = doca_verbs_context_destroy(resources->verbs_context);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca verbs Context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create local resources
 *
 * @cfg [in]: sample's verbs configuration
 * @resources [out]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_local_resources(struct verbs_config *cfg, struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	union ibv_gid rgid;
	struct ibv_pd *pd;
	int ret = 0;
	resources->cfg = cfg;

#ifdef DOCA_ARCH_DPU
	/* On DPU: Open PF device for DPA context (no verbs needed) */
	status = open_doca_device(cfg->pf_device_name, &resources->pf_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open PF device: %s", doca_error_get_descr(status));
		return status;
	}

	/* On DPU: Open SF device with verbs for RDMA operations */
	status = open_verbs_resources(cfg->sf_device_name,
				      &resources->verbs_context,
				      &resources->verbs_pd,
				      &resources->dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open SF device with verbs: %s", doca_error_get_descr(status));
		goto close_pf_dev;
	}
#else
	/* On Host: Open PF device with verbs (used for both DPA and RDMA) */
	status = open_verbs_resources(cfg->pf_device_name,
				      &resources->verbs_context,
				      &resources->verbs_pd,
				      &resources->pf_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open PF device with verbs: %s", doca_error_get_descr(status));
		return status;
	}

	resources->dev = resources->pf_dev;
#endif

	status = doca_dpa_create(resources->pf_dev, &resources->pf_dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pf doca dpa context: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_set_app(resources->pf_dpa_ctx, dpa_sample_app);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pf doca dpa app: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_start(resources->pf_dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start pf doca dpa context: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

#ifdef DOCA_ARCH_DPU
	status = doca_dpa_device_extend(resources->pf_dpa_ctx, resources->dev, &resources->dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to extend doca dpa context: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_get_dpa_handle(resources->dpa_ctx, &resources->dpa_ctx_handle);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca dpa context handle: %s", doca_error_get_descr(status));
		goto destroy_resources;
	}
#else
	resources->dpa_ctx = resources->pf_dpa_ctx;
#endif

	status = doca_dpa_mem_alloc(resources->dpa_ctx, sizeof(uint64_t), &resources->local_dpa_buff_addr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_mem_alloc failed (%s)", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_memset(resources->dpa_ctx, resources->local_dpa_buff_addr, 0, sizeof(uint64_t));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_memset failed (%s)", doca_error_get_descr(status));
		goto destroy_resources;
	}

	resources->local_buff_mmap_obj.mmap_type = MMAP_TYPE_DPA;
	resources->local_buff_mmap_obj.doca_dpa = resources->dpa_ctx;
	resources->local_buff_mmap_obj.doca_device = resources->dev;
	resources->local_buff_mmap_obj.permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
						     DOCA_ACCESS_FLAG_RDMA_READ;
	resources->local_buff_mmap_obj.memrange_addr = (void *)resources->local_dpa_buff_addr;
	resources->local_buff_mmap_obj.memrange_len = sizeof(uint64_t);
	status = doca_mmap_obj_init(&resources->local_buff_mmap_obj);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_obj_init failed (%s)", doca_error_get_descr(status));
		goto destroy_resources;
	}

	status = doca_dpa_mem_alloc(resources->dpa_ctx,
				    sizeof(struct dpa_thread_arg),
				    &resources->dpa_thread_arg_dev_ptr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc doca dpa thread argument memory: %s", doca_error_get_descr(status));
		goto destroy_mmap_obj;
	}

	status = doca_dpa_thread_create(resources->dpa_ctx, &resources->dpa_thread);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca dpa thread: %s", doca_error_get_descr(status));
		goto destroy_mmap_obj;
	}

	if (cfg->is_target) {
		status = doca_dpa_thread_set_func_arg(resources->dpa_thread,
						      &target_thread_kernel,
						      resources->dpa_thread_arg_dev_ptr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca dpa thread func: %s", doca_error_get_descr(status));
			goto destroy_mmap_obj;
		}
	} else {
		status = doca_dpa_thread_set_func_arg(resources->dpa_thread,
						      &initiator_thread_kernel,
						      resources->dpa_thread_arg_dev_ptr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca dpa thread func: %s", doca_error_get_descr(status));
			goto destroy_mmap_obj;
		}
	}

	status = doca_dpa_thread_start(resources->dpa_thread);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca dpa thread: %s", doca_error_get_descr(status));
		goto destroy_mmap_obj;
	}

	status = doca_uar_dpa_create(resources->dpa_ctx, &resources->dpa_uar);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca uar: %s", doca_error_get_descr(status));
		goto destroy_mmap_obj;
	}

	resources->dpa_completion_obj.doca_dpa = resources->dpa_ctx;
	resources->dpa_completion_obj.queue_size = 4;
	resources->dpa_completion_obj.thread = resources->dpa_thread;
	status = dpa_completion_obj_init(&resources->dpa_completion_obj);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dpa_completion_obj_init failed (%s)", doca_error_get_descr(status));
		goto destroy_mmap_obj;
	}

	status = doca_dpa_thread_run(resources->dpa_thread);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run doca dpa thread: %s", doca_error_get_descr(status));
		goto destroy_completion_obj;
	}

	status = doca_rdma_bridge_get_dev_pd(resources->dev, &pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get ibv_pd: %s", doca_error_get_descr(status));
		goto destroy_completion_obj;
	}

	ret = ibv_query_gid(pd->context, 1, cfg->gid_index, &rgid);
	if (ret) {
		DOCA_LOG_ERR("Failed to query ibv gid attributes");
		status = DOCA_ERROR_DRIVER;
		goto destroy_completion_obj;
	}
	memcpy(resources->gid.raw, rgid.raw, DOCA_GID_BYTE_LENGTH);

	status = create_verbs_ah_attr(resources->verbs_context,
				      cfg->gid_index,
				      DOCA_VERBS_ADDR_TYPE_IPv4,
				      &resources->verbs_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs ah: %s", doca_error_get_descr(status));
		goto destroy_completion_obj;
	}

	status = create_verbs_qp(resources->verbs_context,
				 resources->dpa_ctx,
				 resources->verbs_pd,
				 resources->dpa_completion_obj.dpa_comp,
				 resources->dpa_uar,
				 VERBS_SAMPLE_QUEUE_SIZE,
				 VERBS_SAMPLE_QUEUE_SIZE,
				 &resources->dpa_qp_umem_dev_ptr,
				 &resources->dpa_qp_umem,
				 &resources->dpa_qp_dbr_umem_dev_ptr,
				 &resources->dpa_qp_dbr_umem,
				 &resources->verbs_qp);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs qp: %s", doca_error_get_descr(status));
		goto destroy_completion_obj;
	}
	resources->local_qp_number = doca_verbs_qp_get_qpn(resources->verbs_qp);

	status = create_completion_sync_event(resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create completion sync event: %s", doca_error_get_descr(status));
		goto destroy_completion_obj;
	}

	return DOCA_SUCCESS;

destroy_completion_obj:
	tmp_status = dpa_completion_obj_destroy(&resources->dpa_completion_obj);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca dpa completion obj %s", doca_error_get_descr(status));
	}

destroy_mmap_obj:
	tmp_status = doca_mmap_obj_destroy(&resources->local_buff_mmap_obj);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca mmap object: %s", doca_error_get_descr(status));
	}

destroy_resources:
	tmp_status = destroy_local_resources(resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy resources: %s", doca_error_get_descr(tmp_status));
	}

	return status;

#ifdef DOCA_ARCH_DPU
close_pf_dev:
	tmp_status = doca_dev_close(resources->pf_dev);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close PF device: %s", doca_error_get_descr(tmp_status));
	}
	return status;
#endif
}

/*
 * Exchange local RDMA parameters with remote peer
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t exchange_params_with_remote_peer(struct verbs_resources *resources)
{
	if (send(resources->conn_socket, &resources->local_dpa_buff_addr, sizeof(uint64_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local buffer address");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->remote_dpa_buff_addr, sizeof(uint64_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote buffer address ");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (send(resources->conn_socket, &resources->local_buff_mmap_obj.dpa_mmap_handle, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local MKEY");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->remote_dpa_mmap_handle, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote MKEY, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (send(resources->conn_socket, &resources->local_qp_number, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local QP number");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->remote_qp_number, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote QP number, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (send(resources->conn_socket, &resources->gid.raw, sizeof(resources->gid.raw), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local GID address");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	if (recv(resources->conn_socket, &resources->remote_gid.raw, sizeof(resources->gid.raw), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote GID address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Connect local and remote QPs
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t connect_verbs_qp(struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct doca_verbs_qp_attr *verbs_qp_attr = NULL;

	status = doca_verbs_ah_attr_set_gid(resources->verbs_ah_attr, resources->remote_gid);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set remote gid: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create QP attributes */
	status = doca_verbs_qp_attr_create(&verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	/* Set QP attributes for RST2INIT */
	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_INIT);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_allow_remote_write(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote write: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_allow_remote_read(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote read: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_port_num(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port number: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	/* Modify QP - RST2INIT */
	status = doca_verbs_qp_modify(resources->verbs_qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
					      DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_READ | DOCA_VERBS_QP_ATTR_PKEY_INDEX |
					      DOCA_VERBS_QP_ATTR_PORT_NUM);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	/* Set QP attributes for INIT2RTR */
	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTR);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_rq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ PSN: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_dest_qp_num(verbs_qp_attr, resources->remote_qp_number);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set destination QP number: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_min_rnr_timer(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set minimum RNR timer: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_path_mtu(verbs_qp_attr, DOCA_MTU_SIZE_1K_BYTES);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set path MTU: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_ah_attr(verbs_qp_attr, resources->verbs_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address handle: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	/* Modify QP - INIT2RTR */
	status = doca_verbs_qp_modify(resources->verbs_qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN |
					      DOCA_VERBS_QP_ATTR_DEST_QP_NUM | DOCA_VERBS_QP_ATTR_MIN_RNR_TIMER |
					      DOCA_VERBS_QP_ATTR_PATH_MTU | DOCA_VERBS_QP_ATTR_AH_ATTR);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	/* Set QP attributes for RTR2RTS */
	status = doca_verbs_qp_attr_set_next_state(verbs_qp_attr, DOCA_VERBS_QP_STATE_RTS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_sq_psn(verbs_qp_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ PSN: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	status = doca_verbs_qp_attr_set_ack_timeout(verbs_qp_attr, 14);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ACK timeout: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_retry_cnt(verbs_qp_attr, 7);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set retry counter: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_set_rnr_retry(verbs_qp_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RNR retry: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}
	/* Modify QP - RTR2RTS */
	status = doca_verbs_qp_modify(resources->verbs_qp,
				      verbs_qp_attr,
				      DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_SQ_PSN |
					      DOCA_VERBS_QP_ATTR_ACK_TIMEOUT | DOCA_VERBS_QP_ATTR_RETRY_CNT |
					      DOCA_VERBS_QP_ATTR_RNR_RETRY);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp_attr;
	}

	status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	DOCA_LOG_INFO("QP has been successfully connected and ready to use");

	return DOCA_SUCCESS;

destroy_verbs_qp_attr:
	tmp_status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca verbs QP attributes: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

/*
 * Init data path attributes in DPA
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t init_datapath_attr_in_dpa(struct verbs_resources *resources)
{
	doca_error_t status = DOCA_SUCCESS;
	struct dpa_thread_arg arg = {0};

	/* Init qp on the dpa */
	status = doca_verbs_qp_get_dpa_handle(resources->verbs_qp, resources->dpa_ctx, &resources->verbs_qp_handle);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get dpa qp handle: %s", doca_error_get_descr(status));
		return status;
	}

	/* Copy start value to the initiator dpa buffer */
	if (!resources->cfg->is_target) {
		uint64_t local_buf_val = VERBS_SAMPLE_LOCAL_BUF_START_VALUE;
		status = doca_dpa_h2d_memcpy(resources->dpa_ctx,
					     resources->local_dpa_buff_addr,
					     &local_buf_val,
					     sizeof(uint64_t));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize dpa memory of qp external datapath attributes: %s",
				     doca_error_get_descr(status));
			return status;
		}
	}

	/* init dpa thread arg on the dpa */
	arg.dpa_ctx_handle = resources->dpa_ctx_handle;
	arg.dpa_comp_handle = resources->dpa_completion_obj.handle;
	arg.dpa_verbs_qp_handle = resources->verbs_qp_handle;
	arg.comp_sync_event_handle = resources->comp_event_handle;
	arg.comp_sync_event_val = VERBS_SAMPLE_EVENT_WAIT_THRESHOLD + 1;
	arg.local_dpa_buff_addr = resources->local_dpa_buff_addr;
	arg.local_dpa_buff_addr_mmap_handle = resources->local_buff_mmap_obj.dpa_mmap_handle;
	arg.local_dpa_buff_addr_length = resources->local_buff_mmap_obj.memrange_len;
	arg.remote_dpa_buff_addr = resources->remote_dpa_buff_addr;
	arg.remote_dpa_buff_addr_mmap_handle = resources->remote_dpa_mmap_handle;
	status = doca_dpa_h2d_memcpy(resources->dpa_ctx,
				     resources->dpa_thread_arg_dev_ptr,
				     &arg,
				     sizeof(struct dpa_thread_arg));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update dpa thread argument: %s", doca_error_get_descr(status));
		return status;
	}

	return status;
}

/*
 * Target's Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_verbs_target(struct verbs_config *cfg)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct verbs_resources resources = {0};
	int server_sock_fd = -1;
	uint64_t retval = 0;

	resources.conn_socket = -1;

	status = create_local_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local resources: %s", doca_error_get_descr(status));
		return status;
	}

	status = connection_server_setup(&server_sock_fd, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}

	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}

	status = connect_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect doca verbs QP: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}

	status = init_datapath_attr_in_dpa(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init datapath attr in dpa: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}

	/* Start data path */
	status = doca_dpa_rpc(resources.dpa_ctx,
			      &target_trigger_first_iteration_rpc,
			      &retval,
			      resources.dpa_ctx_handle,
			      resources.verbs_qp_handle,
			      resources.local_dpa_buff_addr,
			      resources.local_buff_mmap_obj.dpa_mmap_handle,
			      resources.local_buff_mmap_obj.memrange_len);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RPC failed to post receive: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}

	DOCA_LOG_INFO("Waiting on completion sync event...");
	status = doca_sync_event_wait_gt(resources.comp_event, VERBS_SAMPLE_EVENT_WAIT_THRESHOLD, SYNC_EVENT_MASK_FFS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to wait on completion sync event: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}

	/* check return status*/
	struct dpa_thread_arg args;
	status = doca_dpa_d2h_memcpy(resources.dpa_ctx,
				     &args,
				     resources.dpa_thread_arg_dev_ptr,
				     sizeof(struct dpa_thread_arg));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to copy dpa thread argument: %s", doca_error_get_descr(status));
		goto target_cleanup;
	}
	status = (args.return_status == 0) ? DOCA_SUCCESS : DOCA_ERROR_BAD_STATE;

target_cleanup:
	tmp_status = destroy_local_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	oob_connection_server_close(server_sock_fd, resources.conn_socket);

	return status;
}

/*
 * Initiator's Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_verbs_initiator(struct verbs_config *cfg)
{
	doca_error_t status = DOCA_SUCCESS, tmp_status = DOCA_SUCCESS;
	struct verbs_resources resources = {0};
	uint64_t retval = 0;

	resources.conn_socket = -1;

	status = create_local_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca verbs resources: %s", doca_error_get_descr(status));
		return status;
	}

	status = connection_client_setup(cfg->target_ip_addr, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto initiator_cleanup;
	}

	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto initiator_cleanup;
	}

	status = connect_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect doca verbs QP: %s", doca_error_get_descr(status));
		goto initiator_cleanup;
	}

	status = init_datapath_attr_in_dpa(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init datapath attr in dpa: %s", doca_error_get_descr(status));
		goto initiator_cleanup;
	}

	/* sleep to make sure target did post receive */
	sleep(2);

	/* start data path */
	status = doca_dpa_rpc(resources.dpa_ctx,
			      &initiator_trigger_first_iteration_rpc,
			      &retval,
			      resources.dpa_ctx_handle,
			      resources.verbs_qp_handle,
			      resources.local_dpa_buff_addr,
			      resources.local_buff_mmap_obj.dpa_mmap_handle,
			      resources.local_buff_mmap_obj.memrange_len,
			      resources.remote_dpa_buff_addr,
			      resources.remote_dpa_mmap_handle);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RPC failed to trigger first iteration: %s", doca_error_get_descr(status));
		goto initiator_cleanup;
	}

	DOCA_LOG_INFO("Waiting on completion sync event...");
	status = doca_sync_event_wait_gt(resources.comp_event, VERBS_SAMPLE_EVENT_WAIT_THRESHOLD, SYNC_EVENT_MASK_FFS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to wait on completion sync event: %s", doca_error_get_descr(status));
		goto initiator_cleanup;
	}

initiator_cleanup:
	tmp_status = destroy_local_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	oob_connection_client_close(resources.conn_socket);

	return status;
}