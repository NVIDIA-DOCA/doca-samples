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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include <doca_rdma_bridge.h>

#include "verbs_server_client_common.h"

#define VERBS_SAMPLE_CTRL_SEG_OPCODE_RDMA_SEND 0xA
#define VERBS_SAMPLE_CTRL_SEG_OPCODE_RDMA_WRITE 0x8
#define VERBS_SAMPLE_CTRL_SEG_OPCODE_RDMA_READ 0x10
#define VERBS_SAMPLE_DBR_SEND 1
#define VERBS_SAMPLE_DBR_RECEIVE 0
#define VERBS_SAMPLE_CQE_OPCODE_SHIFT 4
#define VERBS_SAMPLE_CQE_INVALID 0xf
#define VERBS_SAMPLE_CQE_OWNER_MASK 1
#define VERBS_SAMPLE_CQE_REQ_ERR 0xd
#define VERBS_SAMPLE_CQE_RESP_ERR 0xe
#define VERBS_SAMPLE_CQ_SIZE 128
#define VERBS_SAMPLE_MIN_RQ_SIZE 4
#define VERBS_SAMPLE_SIN_PORT 5000
#define VERBS_SAMPLE_HOP_LIMIT 255
#define VERBS_SAMPLE_MAX_SEND_SEGS 1
#define VERBS_SAMPLE_MAX_RECEIVE_SEGS 1
#define VERBS_SAMPLE_FM_CE_SE_CQE_ALWAYS (2 << 2)

DOCA_LOG_REGISTER(VERBS_SERVER_CLIENT::SAMPLE);

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

	/* Create socket */
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

	/* Send connection request to server */
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
	socket_addr.sin_addr.s_addr = INADDR_ANY; /* listen on any interface */

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

	char client_ip[INET_ADDRSTRLEN];
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
	if (oob_sock_fd > 0)
		close(oob_sock_fd);
}

/*
 * Close server's oob connection
 *
 * @oob_sock_fd [in]: server's oob socket file descriptor
 * @oob_client_sock [in]: client's oob socket file descriptor
 */
static void oob_connection_server_close(int oob_sock_fd, int oob_client_sock)
{
	if (oob_client_sock > 0)
		close(oob_client_sock);

	if (oob_sock_fd > 0)
		close(oob_sock_fd);
}

/*
 * Open verbs context from device name
 *
 * @device_name [in]: device name
 * @verbs_ctx [out]: verbs context
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t open_verbs_context(char *device_name, struct doca_verbs_context **verbs_ctx)
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
			status = doca_verbs_context_create(devinfo_list[i],
							   DOCA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
							   verbs_ctx);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create verbs context: %s", doca_error_get_descr(status));
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}

			break;
		}
	}

	status = doca_devinfo_destroy_list(devinfo_list);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_descr(status));
		if (*verbs_ctx)
			(void)doca_verbs_context_destroy(*verbs_ctx);
		return status;
	}

	if (*verbs_ctx == NULL) {
		DOCA_LOG_ERR("The requested device was not found");
		return DOCA_ERROR_NOT_FOUND;
	}

	return DOCA_SUCCESS;
}

/*
 * Create verbs CQ
 *
 * @verbs_context [in]: verbs context
 * @cq_size [in]: CQ size
 * @verbs_cq [out]: verbs CQ
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_verbs_cq(struct doca_verbs_context *verbs_context,
				    uint32_t cq_size,
				    struct doca_verbs_cq **verbs_cq)
{
	doca_error_t status, tmp_status;
	struct doca_verbs_cq_attr *verbs_cq_attr = NULL;
	struct doca_verbs_cq *new_cq = NULL;

	status = doca_verbs_cq_attr_create(&verbs_cq_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs CQ attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_cq_attr_set_cq_size(verbs_cq_attr, cq_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Verbs CQ size: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	status = doca_verbs_cq_attr_set_external_datapath_en(verbs_cq_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external datapath: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	status = doca_verbs_cq_attr_set_entry_size(verbs_cq_attr, DOCA_VERBS_CQ_ENTRY_SIZE_64);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Verbs CQ entry size: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	status = doca_verbs_cq_create(verbs_context, verbs_cq_attr, &new_cq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs CQ: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	status = doca_verbs_cq_attr_destroy(verbs_cq_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs CQ attributes: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	*verbs_cq = new_cq;

	return DOCA_SUCCESS;

destroy_verbs_cq:
	if (verbs_cq_attr != NULL) {
		tmp_status = doca_verbs_cq_attr_destroy(verbs_cq_attr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy Verbs CQ attributes: %s", doca_error_get_descr(tmp_status));
	}

	if (new_cq != NULL) {
		tmp_status = doca_verbs_cq_destroy(new_cq);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy Verbs CQ: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

/*
 * Create verbs AH
 *
 * @verbs_context [in]: verbs context
 * @gid_index [in]: gid index
 * @addr_type [in]: address type
 * @verbs_ah_attr [out]: verbs AH attributes
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_verbs_ah_attr(struct doca_verbs_context *verbs_context,
					 uint32_t gid_index,
					 enum doca_verbs_addr_type addr_type,
					 struct doca_verbs_ah_attr **verbs_ah_attr)
{
	doca_error_t status, tmp_status;
	struct doca_verbs_ah_attr *new_ah_attr = NULL;

	status = doca_verbs_ah_attr_create(verbs_context, &new_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs AH attributes: %s", doca_error_get_descr(status));
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

	if (addr_type == DOCA_VERBS_ADDR_TYPE_IPv4) {
		status = doca_verbs_ah_attr_set_hop_limit(new_ah_attr, VERBS_SAMPLE_HOP_LIMIT);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set hop limit: %s", doca_error_get_descr(status));
			goto destroy_verbs_ah;
		}
	}

	*verbs_ah_attr = new_ah_attr;

	return DOCA_SUCCESS;

destroy_verbs_ah:
	tmp_status = doca_verbs_ah_attr_destroy(new_ah_attr);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs AH attributes: %s", doca_error_get_descr(tmp_status));

	return status;
}

/*
 * Create verbs QP
 *
 * @verbs_context [in]: verbs context
 * @verbs_pd [in]: verbs pd
 * @verbs_cq [in]: verbs CQ
 * @qp_type [in]: QP type
 * @qp_rq_wr [in]: QP rq_wr
 * @qp_sq_wr [in]: QP sq_wr
 * @verbs_qp [out]: verbs QP
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_verbs_qp(struct doca_verbs_context *verbs_context,
				    struct doca_verbs_pd *verbs_pd,
				    struct doca_verbs_cq *verbs_cq,
				    uint32_t qp_type,
				    uint32_t qp_rq_wr,
				    uint32_t qp_sq_wr,
				    struct doca_verbs_qp **verbs_qp)
{
	doca_error_t status, tmp_status;
	struct doca_verbs_qp_init_attr *verbs_qp_init_attr = NULL;
	struct doca_verbs_qp *new_qp = NULL;

	status = doca_verbs_qp_init_attr_create(&verbs_qp_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_qp_init_attr_set_pd(verbs_qp_init_attr, verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Verbs PD: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_sq_wr(verbs_qp_init_attr, qp_sq_wr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ size: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_rq_wr(verbs_qp_init_attr, qp_rq_wr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ size: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_qp_type(verbs_qp_init_attr, qp_type);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set QP type: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_send_cq(verbs_qp_init_attr, verbs_cq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Verbs send CQ: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_receive_cq(verbs_qp_init_attr, verbs_cq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Verbs receive CQ: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	doca_verbs_qp_init_attr_set_external_datapath_en(verbs_qp_init_attr, 0);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external datapath: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_send_max_sges(verbs_qp_init_attr, VERBS_SAMPLE_MAX_SEND_SEGS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set send_max_sges: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_set_receive_max_sges(verbs_qp_init_attr, VERBS_SAMPLE_MAX_RECEIVE_SEGS);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set receive_max_sges: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_create(verbs_context, verbs_qp_init_attr, &new_qp);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	status = doca_verbs_qp_init_attr_destroy(verbs_qp_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs QP attributes: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	*verbs_qp = new_qp;

	return DOCA_SUCCESS;

destroy_verbs_qp:
	if (verbs_qp_init_attr != NULL) {
		tmp_status = doca_verbs_qp_init_attr_destroy(verbs_qp_init_attr);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy Verbs QP attributes: %s", doca_error_get_descr(tmp_status));
	}

	if (new_qp != NULL) {
		tmp_status = doca_verbs_qp_destroy(new_qp);
		if (tmp_status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy Verbs QP: %s", doca_error_get_descr(tmp_status));
	}

	return status;
}

/*
 * Create local memory objects
 *
 * @buf_size [in]: buffer size
 * @resources [in/out]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t create_local_memory_objects(size_t buf_size, struct verbs_resources *resources)
{
	/* Allocate local buffer */
	resources->local_buf = (void *)calloc(1, resources->cfg->buf_size * resources->cfg->iter_num);
	if (resources->local_buf == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory buffer of size = %zu", resources->cfg->buf_size);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Get ibv_pd */
	resources->pd = doca_verbs_bridge_verbs_pd_get_ibv_pd(resources->verbs_pd);

	/* Register local buffer */
	resources->mr = ibv_reg_mr(resources->pd,
				   (void *)resources->local_buf,
				   (buf_size * resources->cfg->iter_num),
				   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (resources->mr == NULL) {
		DOCA_LOG_ERR("Failed to register local buffer");
		free((void *)resources->local_buf);
		return DOCA_ERROR_NO_MEMORY;
	}

	resources->local_mkey = resources->mr->rkey;

	return DOCA_SUCCESS;
}

/*
 * Destroy local memory objects
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t destroy_local_memory_objects(struct verbs_resources *resources)
{
	int ret = 0;

	if (resources->mr) {
		ret = ibv_dereg_mr(resources->mr);
		if (ret != 0) {
			DOCA_LOG_ERR("ibv_dereg_mr failed with error=%d", ret);
			return DOCA_ERROR_DRIVER;
		}
	}

	if (resources->local_buf)
		free((void *)resources->local_buf);

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
	doca_error_t status, tmp_status;
	union ibv_gid rgid;
	struct ibv_pd *pd = NULL;
	int ret = 0;
	struct ibv_port_attr port_attr;

	resources->cfg = cfg;

	/* Open Verbs Context */
	status = open_verbs_context(cfg->device_name, &resources->verbs_context);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create Verbs PD */
	status = doca_verbs_pd_create(resources->verbs_context, &resources->verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs PD: %s", doca_error_get_descr(status));
		goto destroy_verbs_context;
	}

	/* Create DOCA dev from the verbs PD */
	status = doca_verbs_pd_as_doca_dev(resources->verbs_pd, &resources->dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca dev: %s", doca_error_get_descr(status));
		goto destroy_verbs_pd;
	}

	/* Create Verbs CQ */
	status = create_verbs_cq(resources->verbs_context, cfg->iter_num, &resources->verbs_cq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs CQ: %s", doca_error_get_descr(status));
		goto close_doca_dev;
	}

	/* Get ibv_pd associated with the doca_dev */
	status = doca_rdma_bridge_get_dev_pd(resources->dev, &pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get ibv_pd: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	/* Query local GID address */
	ret = ibv_query_gid(pd->context, DEFAULT_PORT, cfg->gid_index, &rgid);
	if (ret) {
		DOCA_LOG_ERR("Failed to query ibv gid attributes");
		status = DOCA_ERROR_DRIVER;
		goto destroy_verbs_cq;
	}

	memcpy(resources->gid.raw, rgid.raw, DOCA_GID_BYTE_LENGTH);

	/* Adjust address type to port's link layer configuration */
	ret = ibv_query_port(pd->context, DEFAULT_PORT, &port_attr);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to query ibv port \n");
		status = DOCA_ERROR_DRIVER;
		goto destroy_verbs_cq;
	}
	if (port_attr.state != IBV_PORT_ACTIVE) {
		DOCA_LOG_ERR("Port state in inactive \n");
		status = DOCA_ERROR_BAD_STATE;
		goto destroy_verbs_cq;
	}
	if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
		DOCA_LOG_INFO("Running on ROCE Link layer \n");
		resources->addr_type = DOCA_VERBS_ADDR_TYPE_IPv4;
	} else if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
		DOCA_LOG_INFO("Running on IB Link layer \n");
		resources->addr_type = DOCA_VERBS_ADDR_TYPE_IB_NO_GRH;
		resources->local_lid = port_attr.lid;
	} else {
		DOCA_LOG_ERR("Port's Link Layer is not ROCE or IB \n");
		status = DOCA_ERROR_UNEXPECTED;
		goto destroy_verbs_cq;
	}

	/* Create Verbs AH attributes */
	status = create_verbs_ah_attr(resources->verbs_context,
				      cfg->gid_index,
				      resources->addr_type,
				      &resources->verbs_ah_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs AH attributes: %s", doca_error_get_descr(status));
		goto destroy_verbs_cq;
	}

	/* Create Verbs QP */
	status = create_verbs_qp(resources->verbs_context,
				 resources->verbs_pd,
				 resources->verbs_cq,
				 cfg->qp_type,
				 (cfg->iter_num < VERBS_SAMPLE_MIN_RQ_SIZE) ? VERBS_SAMPLE_MIN_RQ_SIZE : cfg->iter_num,
				 cfg->iter_num,
				 &resources->verbs_qp);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs QP: %s", doca_error_get_descr(status));
		goto destroy_verbs_ah_attr;
	}

	/* Get QP number */
	resources->local_qp_number = doca_verbs_qp_get_qpn(resources->verbs_qp);

	/* Create local memory objects */
	status = create_local_memory_objects(cfg->buf_size, resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local memory objects: %s", doca_error_get_descr(status));
		goto destroy_verbs_qp;
	}

	return DOCA_SUCCESS;

destroy_verbs_qp:
	tmp_status = doca_verbs_qp_destroy(resources->verbs_qp);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs QP: %s", doca_error_get_descr(tmp_status));

destroy_verbs_ah_attr:
	tmp_status = doca_verbs_ah_attr_destroy(resources->verbs_ah_attr);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs AH attributes: %s", doca_error_get_descr(tmp_status));

destroy_verbs_cq:
	tmp_status = doca_verbs_cq_destroy(resources->verbs_cq);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs CQ: %s", doca_error_get_descr(tmp_status));

close_doca_dev:
	tmp_status = doca_dev_close(resources->dev);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_status));

destroy_verbs_pd:
	tmp_status = doca_verbs_pd_destroy(resources->verbs_pd);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs PD: %s", doca_error_get_descr(tmp_status));

destroy_verbs_context:
	tmp_status = doca_verbs_context_destroy(resources->verbs_context);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs Context: %s", doca_error_get_descr(tmp_status));

	return status;
}

/*
 * Destroy local resources
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t destroy_local_resources(struct verbs_resources *resources)
{
	doca_error_t status;

	status = destroy_local_memory_objects(resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory resources: %s", doca_error_get_descr(status));
		return status;
	}

	if (resources->verbs_qp) {
		status = doca_verbs_qp_destroy(resources->verbs_qp);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs QP: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_ah_attr) {
		status = doca_verbs_ah_attr_destroy(resources->verbs_ah_attr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs AH attributes: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_cq) {
		status = doca_verbs_cq_destroy(resources->verbs_cq);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs CQ: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->dev) {
		status = doca_dev_close(resources->dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_pd) {
		status = doca_verbs_pd_destroy(resources->verbs_pd);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs PD: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (resources->verbs_context) {
		status = doca_verbs_context_destroy(resources->verbs_context);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs Context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Exchange local RDMA parameters with remote peer
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t exchange_params_with_remote_peer(struct verbs_resources *resources)
{
	uint64_t local_addr = (uint64_t)resources->local_buf;

	/* Send local buffer address to remote peer */
	if (send(resources->conn_socket, &local_addr, sizeof(uint64_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local buffer address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Receive remote buffer address from remote peer */
	if (recv(resources->conn_socket, &resources->remote_buf, sizeof(uint64_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote buffer address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Send local MKEY to remote peer */
	if (send(resources->conn_socket, &resources->local_mkey, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local MKEY, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Receive remote MKEY from remote peer */
	if (recv(resources->conn_socket, &resources->remote_mkey, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote MKEY, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Send local QP number to remote peer */
	if (send(resources->conn_socket, &resources->local_qp_number, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local QP number, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Receive remote QP number from remote peer */
	if (recv(resources->conn_socket, &resources->remote_qp_number, sizeof(uint32_t), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote QP number, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Send local GID address to remote peer */
	if (send(resources->conn_socket, &resources->gid.raw, sizeof(resources->gid.raw), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local GID address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Receive remote GID address from remote peer */
	if (recv(resources->conn_socket, &resources->remote_gid.raw, sizeof(resources->gid.raw), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote GID address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Send local LID address to remote peer */
	if (send(resources->conn_socket, &resources->local_lid, sizeof(resources->local_lid), 0) < 0) {
		DOCA_LOG_ERR("Failed to send local LID address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Receive remote LID address from remote peer */
	if (recv(resources->conn_socket, &resources->remote_lid, sizeof(resources->remote_lid), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive remote LID address, err = %s", strerror(errno));
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Synchronize local and remote peer
 *
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t sync_remote_peer(struct verbs_resources *resources)
{
	int send_ack = 0, receive_ack = 0;

	/* Send ack to remote peer */
	if (send(resources->conn_socket, &send_ack, sizeof(int), 0) < 0) {
		DOCA_LOG_ERR("Failed to send ack");
		return DOCA_ERROR_CONNECTION_ABORTED;
	}

	/* Receive ack from remote peer */
	if (recv(resources->conn_socket, &receive_ack, sizeof(int), 0) < 0) {
		DOCA_LOG_ERR("Failed to receive ack ");
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
	doca_error_t status, tmp_status;
	struct doca_verbs_qp_attr *verbs_qp_attr;
	int qp_attr_mask = 0;

	if (resources->addr_type == DOCA_VERBS_ADDR_TYPE_IPv4) {
		status = doca_verbs_ah_attr_set_gid(resources->verbs_ah_attr, resources->remote_gid);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set remote gid: %s", doca_error_get_descr(status));
			return status;
		}
	} else { /* addr_type = NO_GRH */
		status = doca_verbs_ah_attr_set_dlid(resources->verbs_ah_attr, resources->remote_lid);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set remote gid: %s", doca_error_get_descr(status));
			return status;
		}
	}

	/* Create QP attributes */
	status = doca_verbs_qp_attr_create(&verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA Verbs QP attributes: %s", doca_error_get_descr(status));
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
	qp_attr_mask |= DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN | DOCA_VERBS_QP_ATTR_DEST_QP_NUM |
			DOCA_VERBS_QP_ATTR_MIN_RNR_TIMER | DOCA_VERBS_QP_ATTR_PATH_MTU | DOCA_VERBS_QP_ATTR_AH_ATTR;

	/* Modify QP - INIT2RTR */
	status = doca_verbs_qp_modify(resources->verbs_qp, verbs_qp_attr, qp_attr_mask);
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

	status = doca_verbs_qp_attr_set_rnr_retry(verbs_qp_attr, 7);
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

	/* Destroy QP attributes */
	status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy RDMA Verbs QP attributes: %s", doca_error_get_descr(status));
		return status;
	}

	DOCA_LOG_INFO("QP has been successfully connected and ready to use");

	return DOCA_SUCCESS;

destroy_verbs_qp_attr:
	tmp_status = doca_verbs_qp_attr_destroy(verbs_qp_attr);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy RDMA Verbs QP attributes: %s", doca_error_get_descr(tmp_status));

	return status;
}

/*
 * Execute client's data path
 *
 * @rdma_oper_type [in]: RDMA operation type
 * @buf_size [in]: buffer size
 * @iter_num [in]: number of iterations
 * @send_burst [in]: send burst flag
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t execute_data_path_client(enum verbs_sample_operation rdma_oper_type,
					     uint32_t buf_size,
					     uint32_t iter_num,
					     bool send_burst,
					     struct verbs_resources *resources)
{
	struct ibv_sge buffer = {};
	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_wc wc = {};
	uint8_t *local_buf_addr = NULL;
	uint32_t pi = 0;
	uint32_t ci = 0;
	uint32_t burst_size = 0;
	uint32_t i = 0;
	int ret = 0;

	buffer.length = buf_size;
	buffer.lkey = resources->local_mkey;

	while (ci < iter_num) {
		if (send_burst) {
			/* Send all messages at once in a burst */
			burst_size = iter_num;
		} else {
			/* Get burst size from the user and send bursts interactively */
			DOCA_LOG_INFO("Enter the required burst size and press enter");
			ret = scanf("%u", &burst_size);
			if (ret != 1)
				return DOCA_ERROR_UNEXPECTED;
			if (burst_size > iter_num - ci) {
				burst_size = iter_num - ci;
			}
		}
		for (i = 0; i < burst_size; i++) {
			/* Calculate the local buffer address for this WR */
			local_buf_addr = (uint8_t *)(resources->local_buf) + (pi * buf_size);
			/* Set pi + 1 as the message to be sent to the server */
			*(uint32_t *)local_buf_addr = pi + 1;
			buffer.addr = (uint64_t)local_buf_addr;

			memset(&wr, 0, sizeof(wr));
			wr.wr_id = i;
			wr.next = NULL;
			wr.sg_list = &buffer;
			wr.num_sge = 1;
			wr.send_flags = IBV_SEND_SIGNALED;

			/* Set WR parameters according to the requested RDMA operation */
			switch (rdma_oper_type) {
			case VERBS_SAMPLE_OPERATION_SEND_RECEIVE:
				wr.opcode = IBV_WR_SEND;
				break;
			case VERBS_SAMPLE_OPERATION_WRITE:
				wr.wr.rdma.remote_addr = resources->remote_buf + (pi * buf_size);
				wr.wr.rdma.rkey = resources->remote_mkey;
				wr.opcode = IBV_WR_RDMA_WRITE;
				break;
			case VERBS_SAMPLE_OPERATION_READ:
				wr.wr.rdma.remote_addr = resources->remote_buf + (pi * buf_size);
				wr.wr.rdma.rkey = resources->remote_mkey;
				wr.opcode = IBV_WR_RDMA_READ;
				break;
			default:
				return DOCA_ERROR_INVALID_VALUE;
			}

			/* Post send WR */
			ret = doca_verbs_bridge_post_send(resources->verbs_qp, &wr, &bad_wr);
			if (ret != 0 || bad_wr != NULL) {
				DOCA_LOG_ERR("Failed to post Send WR");
				return DOCA_ERROR_IO_FAILED;
			}
			pi++;

			/* Poll Completion */
			memset(&wc, 0, sizeof(wc));

			while (doca_verbs_bridge_poll_cq(resources->verbs_cq, 1, &wc) == 0)
				/* do nothing */
				;

			/* Verify the WC status */
			if (wc.status != IBV_WC_SUCCESS) {
				DOCA_LOG_ERR("Received completion with error: status=%s, vendor_error=0x%x\n",
					     ibv_wc_status_str(wc.status),
					     wc.vendor_err);
				return DOCA_ERROR_IO_FAILED;
			}

			/* Verify the WC opcode */
			if ((rdma_oper_type == VERBS_SAMPLE_OPERATION_SEND_RECEIVE && wc.opcode != IBV_WC_SEND) ||
			    (rdma_oper_type == VERBS_SAMPLE_OPERATION_WRITE && wc.opcode != IBV_WC_RDMA_WRITE) ||
			    (rdma_oper_type == VERBS_SAMPLE_OPERATION_READ && wc.opcode != IBV_WC_RDMA_READ)) {
				DOCA_LOG_ERR("Received unexpected WC opcode");
				return DOCA_ERROR_UNEXPECTED;
			}

			ci++;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Execute server's data path
 *
 * @rdma_oper_type [in]: RDMA operation type
 * @buf_size [in]: buffer size
 * @iter_num [in]: number of iterations
 * @resources [in]: verbs resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t execute_data_path_server(enum verbs_sample_operation rdma_oper_type,
					     uint32_t buf_size,
					     uint32_t iter_num,
					     struct verbs_resources *resources)
{
	uint32_t pi = 0;
	uint32_t ci = 0;
	int ret = 0;
	struct ibv_sge buffer = {};
	struct ibv_recv_wr wr = {};
	struct ibv_recv_wr *bad_wr = NULL;
	struct ibv_wc wc = {};
	uint8_t *local_buf_addr = NULL;
	uint32_t i = 0;

	/*
	   RDMA Send/Receive operation - both the client and the server post WRs.
	   RDMA Write operation - only the client posts WRs.
	   RDMA Read operation - only the client posts WRs.
	*/
	if (rdma_oper_type == VERBS_SAMPLE_OPERATION_READ) {
		return DOCA_SUCCESS;
	}

	buffer.length = buf_size;
	buffer.lkey = resources->local_mkey;

	if (rdma_oper_type == VERBS_SAMPLE_OPERATION_SEND_RECEIVE) {
		/* In case of RDMA receive operation, post receive WRs and poll their completions */
		for (i = 0; i < iter_num; i++) {
			/* Calculate the local buffer address for this WR */
			local_buf_addr = (uint8_t *)(resources->local_buf) + (pi * buf_size);
			buffer.addr = (uint64_t)local_buf_addr;

			memset(&wr, 0, sizeof(wr));
			wr.wr_id = pi;
			wr.next = NULL;
			wr.sg_list = &buffer;
			wr.num_sge = 1;

			/* Post receive WR */
			ret = doca_verbs_bridge_post_recv(resources->verbs_qp, &wr, &bad_wr);
			if (ret != 0 || bad_wr != NULL) {
				DOCA_LOG_ERR("Failed to post Receive WR");
				return DOCA_ERROR_IO_FAILED;
			}
			pi++;
		}

		/* Poll Completions */
		while (ci < iter_num) {
			memset(&wc, 0, sizeof(wc));

			while (doca_verbs_bridge_poll_cq(resources->verbs_cq, 1, &wc) == 0)
				/* do nothing */
				;

			/* Verify the WC status */
			if (wc.status != IBV_WC_SUCCESS) {
				DOCA_LOG_ERR("Received completion with error: status=%s, vendor_error=0x%x\n",
					     ibv_wc_status_str(wc.status),
					     wc.vendor_err);
				return DOCA_ERROR_IO_FAILED;
			}
			/* Verify the WC opcode */
			if (wc.opcode != IBV_WC_RECV) {
				DOCA_LOG_ERR("Received unexpected WC opcode");
				return DOCA_ERROR_UNEXPECTED;
			}

			/* Calculate the local buffer address and read the message from the client */
			local_buf_addr = (uint8_t *)(resources->local_buf) + (buf_size * (ci));
			DOCA_LOG_INFO("Received a valid message #%d from client", *(uint32_t *)(local_buf_addr));
			ci++;
		}
	} else if (rdma_oper_type == VERBS_SAMPLE_OPERATION_WRITE) {
		/* In case of RDMA Write operation, poll the memory buffer and wait for the expected message */
		for (i = 0; i < iter_num; i++) {
			local_buf_addr = (uint8_t *)(resources->local_buf) + (buf_size * i);
			/* Verify the received message from client (expected message = i + 1) */
			while (*(uint32_t *)(local_buf_addr) != (i + 1))
				/* do nothing */
				;

			DOCA_LOG_INFO("Received a valid message #%d from client", (i + 1));
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Server main sample flow
 *
 * @cfg [in]: sample's verbs configuration
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
doca_error_t verbs_server(struct verbs_config *cfg)
{
	doca_error_t status, tmp_status;
	struct verbs_resources resources = {0};
	int server_sock_fd = -1;
	resources.conn_socket = -1;

	/* Create local resources for RDMA Verbs sample application */
	status = create_local_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local resources: %s", doca_error_get_descr(status));
		return status;
	}

	/* Setup connection with client */
	status = connection_server_setup(&server_sock_fd, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	/* Exchange RDMA parameters with client */
	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	/* Connect local and remote QPs */
	status = connect_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect RDMA Verbs QP: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	DOCA_LOG_INFO("local QP number: %d", resources.local_qp_number);

	/* Execute data path */
	status = execute_data_path_server(cfg->rdma_oper_type, cfg->buf_size, cfg->iter_num, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to execute data-path: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

	/* Synchronize client */
	status = sync_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to synchronize client: %s", doca_error_get_descr(status));
		goto server_cleanup;
	}

server_cleanup:
	/* Destroy local resources */
	tmp_status = destroy_local_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	/* Close connection */
	oob_connection_server_close(server_sock_fd, resources.conn_socket);

	return status;
}

/*
 * Client main sample flow
 *
 * @cfg [in]: sample's verbs configuration
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
doca_error_t verbs_client(struct verbs_config *cfg)
{
	doca_error_t status, tmp_status;
	struct verbs_resources resources = {0};
	resources.conn_socket = -1;

	/* Create local resources for RDMA Verbs sample application */
	status = create_local_resources(cfg, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA Verbs resources: %s", doca_error_get_descr(status));
		return status;
	}

	/* Setup connection with server */
	status = connection_client_setup(cfg->server_ip_addr, &resources.conn_socket);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup OOB connection with remote peer: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	/* Exchange RDMA parameters with server */
	status = exchange_params_with_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange params with remote peer: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	/* Connect local and remote QPs */
	status = connect_verbs_qp(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect RDMA Verbs QP: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	DOCA_LOG_INFO("local QP number: %d", resources.local_qp_number);

	/* Execute data path */
	status =
		execute_data_path_client(cfg->rdma_oper_type, cfg->buf_size, cfg->iter_num, cfg->send_burst, &resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to execute data-path: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

	/* Synchronize server */
	status = sync_remote_peer(&resources);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to synchronize server: %s", doca_error_get_descr(status));
		goto client_cleanup;
	}

client_cleanup:
	/* Destroy local resources */
	tmp_status = destroy_local_resources(&resources);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local resources: %s", doca_error_get_descr(tmp_status));
		DOCA_ERROR_PROPAGATE(status, tmp_status);
	}

	/* Close connection */
	oob_connection_client_close(resources.conn_socket);

	return status;
}
