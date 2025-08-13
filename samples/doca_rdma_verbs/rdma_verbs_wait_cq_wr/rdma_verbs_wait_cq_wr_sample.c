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

#include <infiniband/verbs.h>
#include <stdlib.h>
#include <time.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_rdma_bridge.h>
#include <doca_rdma_verbs.h>
#include <doca_rdma_verbs_bridge.h>
#include <doca_uar.h>

#include "rdma_verbs_wait_cq_wr_sample.h"
#include "common.h"

DOCA_LOG_REGISTER(wait_cq_wr::SAMPLE);

#define CQ_SIZE 64
#define CQ_ENTRY_SIZE DOCA_RDMA_VERBS_CQ_ENTRY_SIZE_64
#define QP_WR 64
#define MESSAGE_SIZE 64

/* Default QP connection parameters */
#define DEFAULT_PORT_NUM 1
#define DEFAULT_ACK_TIMEOUT 14
#define DEFAULT_RETRY_CNT 7
#define DEFAULT_RNR_RETRY 7
#define DEFAULT_MIN_RNR_TIMER 1
#define DEFAULT_HOP_LIMIT 255
#define DEFAULT_TRAFFIC_CLASS 0

struct connection_resources {
	struct doca_rdma_verbs_cq *cq;
	struct doca_rdma_verbs_qp *qp1;
	struct doca_rdma_verbs_qp *qp2;
	struct doca_rdma_verbs_cq_attr *rdma_verbs_cq_attr;
	struct doca_rdma_verbs_qp_init_attr *rdma_verbs_qp_init_attr1;
	struct doca_rdma_verbs_qp_init_attr *rdma_verbs_qp_init_attr2;
};

struct wait_cq_wr_resources {
	struct doca_rdma_verbs_context *rdma_verbs_ctx;
	struct doca_rdma_verbs_gid gid;
	uint32_t gid_index;
	struct ibv_context *ibv_ctx;
	struct doca_dev *dev;
	struct doca_devinfo *devinfo;
	struct doca_rdma_verbs_pd *pd;
	struct ibv_pd *ibv_pd;
	struct doca_uar *uar;
	struct doca_rdma_verbs_ah *ah;
	struct connection_resources conn1;
	struct connection_resources conn2;
};

/**
 * Destroys all resources associated with a connection
 *
 * @resources [in]: Pointer to connection resources to destroy
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t destroy_connection_resources(struct connection_resources *resources)
{
	doca_error_t result, total_result = DOCA_SUCCESS;

	if (resources->qp2 != NULL) {
		result = doca_rdma_verbs_qp_destroy(resources->qp2);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy QP2: %s", doca_error_get_descr(result));
		} else {
			resources->qp2 = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->rdma_verbs_qp_init_attr2 != NULL) {
		result = doca_rdma_verbs_qp_init_attr_destroy(resources->rdma_verbs_qp_init_attr2);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy QP2 attributes: %s", doca_error_get_descr(result));
		} else {
			resources->rdma_verbs_qp_init_attr2 = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->qp1 != NULL) {
		result = doca_rdma_verbs_qp_destroy(resources->qp1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy QP1: %s", doca_error_get_descr(result));
		} else {
			resources->qp1 = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->rdma_verbs_qp_init_attr1 != NULL) {
		result = doca_rdma_verbs_qp_init_attr_destroy(resources->rdma_verbs_qp_init_attr1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy QP1 attributes: %s", doca_error_get_descr(result));
		} else {
			resources->rdma_verbs_qp_init_attr1 = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->cq != NULL) {
		result = doca_rdma_verbs_cq_destroy(resources->cq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy CQ: %s", doca_error_get_descr(result));
		} else {
			resources->cq = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->rdma_verbs_cq_attr != NULL) {
		result = doca_rdma_verbs_cq_attr_destroy(resources->rdma_verbs_cq_attr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy CQ attributes: %s", doca_error_get_descr(result));
		} else {
			resources->rdma_verbs_cq_attr = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	return total_result;
}

/**
 * Initializes QP attributes for send or receive operations
 *
 * @resources [in]: Pointer to message chaining send resources
 * @conn_resources [in]: Pointer to connection resources
 * @snd [in]: Flag indicating if this is for send (1) or receive (0) operations
 * @rdma_verbs_qp_init_attr [out]: Pointer to store the created QP attributes
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t init_qp_attr(struct wait_cq_wr_resources *resources,
				 struct connection_resources *conn_resources,
				 int snd,
				 struct doca_rdma_verbs_qp_init_attr **rdma_verbs_qp_init_attr)
{
	doca_error_t result;

	result = doca_rdma_verbs_qp_init_attr_create(rdma_verbs_qp_init_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create QP attributes: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_rdma_verbs_qp_init_attr_set_external_uar(*rdma_verbs_qp_init_attr, resources->uar);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external UAR: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_qp_init_attr_set_pd(*rdma_verbs_qp_init_attr, resources->pd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PD: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_qp_init_attr_set_external_datapath_en(*rdma_verbs_qp_init_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external datapath en: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_qp_init_attr_set_max_inline_data(*rdma_verbs_qp_init_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max inline data: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_qp_init_attr_set_qp_type(*rdma_verbs_qp_init_attr, DOCA_RDMA_VERBS_QP_TYPE_RC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set QP type: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	if (snd) {
		/* Set attributes for QP1, send qp*/
		result = doca_rdma_verbs_qp_init_attr_set_send_cq(*rdma_verbs_qp_init_attr, conn_resources->cq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set send CQ: %s", doca_error_get_descr(result));
			goto exit_failure;
		}

		result = doca_rdma_verbs_qp_init_attr_set_sq_wr(*rdma_verbs_qp_init_attr, QP_WR);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set send queue work requests: %s", doca_error_get_descr(result));
			goto exit_failure;
		}

		result = doca_rdma_verbs_qp_init_attr_set_send_max_sges(*rdma_verbs_qp_init_attr, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set send max sges: %s", doca_error_get_descr(result));
			goto exit_failure;
		}

		result = doca_rdma_verbs_qp_init_attr_set_sq_sig_all(*rdma_verbs_qp_init_attr, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set send sig all: %s", doca_error_get_descr(result));
			goto exit_failure;
		}

	} else {
		/* Set attributes for QP2, receive qp*/
		result = doca_rdma_verbs_qp_init_attr_set_receive_cq(*rdma_verbs_qp_init_attr, conn_resources->cq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set receive CQ: %s", doca_error_get_descr(result));
			goto exit_failure;
		}

		result = doca_rdma_verbs_qp_init_attr_set_rq_wr(*rdma_verbs_qp_init_attr, QP_WR);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set receive queue work requests: %s", doca_error_get_descr(result));
			goto exit_failure;
		}

		result = doca_rdma_verbs_qp_init_attr_set_receive_max_sges(*rdma_verbs_qp_init_attr, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set receive max sges: %s", doca_error_get_descr(result));
			goto exit_failure;
		}
	}
	return DOCA_SUCCESS;

exit_failure:
	DOCA_ERROR_PROPAGATE(result, doca_rdma_verbs_qp_init_attr_destroy(*rdma_verbs_qp_init_attr));
	*rdma_verbs_qp_init_attr = NULL;
	return result;
}

/**
 * Initializes all resources needed for a connection
 *
 * @resources [in]: Pointer to message chaining send resources
 * @conn [in]: Pointer to connection resources to initialize
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t init_connection_resources(struct wait_cq_wr_resources *resources,
					      struct connection_resources *conn)
{
	doca_error_t result, destroy_result;

	/* Create CQ attributes and CQ*/
	result = doca_rdma_verbs_cq_attr_create(&conn->rdma_verbs_cq_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA verbs CQ attributes: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_cq_attr_set_external_uar(conn->rdma_verbs_cq_attr, resources->uar);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external UAR: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_cq_attr_set_cq_size(conn->rdma_verbs_cq_attr, CQ_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CQ size: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_cq_attr_set_external_datapath_en(conn->rdma_verbs_cq_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external datapath enable: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_cq_attr_set_entry_size(conn->rdma_verbs_cq_attr, CQ_ENTRY_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CQ entry size: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_cq_create(resources->rdma_verbs_ctx, conn->rdma_verbs_cq_attr, &conn->cq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create CQ: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	/* Initialize QP attributes and create QPs */
	result = init_qp_attr(resources, conn, 1, &conn->rdma_verbs_qp_init_attr1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize QP1 attributes: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_qp_create(resources->rdma_verbs_ctx, conn->rdma_verbs_qp_init_attr1, &conn->qp1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create QP1: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = init_qp_attr(resources, conn, 0, &conn->rdma_verbs_qp_init_attr2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize QP2 attributes: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_qp_create(resources->rdma_verbs_ctx, conn->rdma_verbs_qp_init_attr2, &conn->qp2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create QP2: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	return DOCA_SUCCESS;

exit_failure:
	destroy_result = destroy_connection_resources(conn);
	if (destroy_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy connection resources: %s", doca_error_get_descr(destroy_result));
	}
	return result;
}

/**
 * Destroys all resources used by the message chaining send sample
 *
 * @resources [in]: Pointer to resources to destroy
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t destroy_resources(struct wait_cq_wr_resources *resources)
{
	doca_error_t result, total_result = DOCA_SUCCESS;

	result = destroy_connection_resources(&resources->conn1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy connection resources: %s", doca_error_get_descr(result));
	}
	DOCA_ERROR_PROPAGATE(total_result, result);

	result = destroy_connection_resources(&resources->conn2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy connection resources: %s", doca_error_get_descr(result));
	}
	DOCA_ERROR_PROPAGATE(total_result, result);

	if (resources->ah != NULL) {
		result = doca_rdma_verbs_ah_destroy(resources->ah);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy AH: %s", doca_error_get_descr(result));
		} else {
			resources->ah = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->uar != NULL) {
		result = doca_uar_destroy(resources->uar);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy UAR: %s", doca_error_get_descr(result));
		} else {
			resources->uar = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->pd != NULL) {
		result = doca_rdma_verbs_pd_destroy(resources->pd);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy RDMA verbs PD: %s", doca_error_get_descr(result));
		} else {
			resources->pd = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->rdma_verbs_ctx != NULL) {
		result = doca_rdma_verbs_context_destroy(resources->rdma_verbs_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy RDMA verbs context: %s", doca_error_get_descr(result));
		} else {
			resources->rdma_verbs_ctx = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	if (resources->dev != NULL) {
		result = doca_dev_close(resources->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA device: %s", doca_error_get_descr(result));
		} else {
			resources->dev = NULL;
		}
		DOCA_ERROR_PROPAGATE(total_result, result);
	}

	return total_result;
}

/**
 * Initializes all resources needed for the message chaining send sample
 *
 * @resources [in]: Pointer to resources to initialize
 * @cfg [in]: Configuration parameters for the sample
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t init_resources(struct wait_cq_wr_resources *resources,
				   struct wait_cq_wr_cfg *cfg)
{
	doca_error_t result;
	struct ibv_pd *pd = NULL;
	union ibv_gid rgid;
	int ret;

	result = open_doca_device_with_ibdev_name((const uint8_t *)cfg->ibv_device_name,
						  strlen(cfg->ibv_device_name),
						  NULL,
						  &resources->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_bridge_get_dev_pd(resources->dev, &pd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DOCA device PD: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	resources->ibv_ctx = pd->context;
	resources->ibv_pd = pd;

	result = doca_rdma_verbs_bridge_rdma_verbs_pd_create(pd, &resources->pd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA verbs PD: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_bridge_rdma_verbs_context_import(resources->ibv_ctx,
								  DOCA_RDMA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
								  &resources->rdma_verbs_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA verbs context: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	/* Query local GID address */
	ret = ibv_query_gid(pd->context, 1, cfg->gid_index, &rgid);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to get GID address");
		result = DOCA_ERROR_DRIVER;
		goto exit_failure;
	}

	memcpy(resources->gid.raw, rgid.raw, DOCA_GID_BYTE_LENGTH);
	resources->gid_index = cfg->gid_index;

	result = doca_uar_create(resources->dev, DOCA_UAR_ALLOCATION_TYPE_NONCACHE, &resources->uar);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create UAR: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	/* Create AH */
	result = doca_rdma_verbs_ah_create(resources->rdma_verbs_ctx, &resources->ah);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create AH: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_ah_set_gid(resources->ah, resources->gid);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set GID: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_ah_set_addr_type(resources->ah,
						  cfg->is_ipv6 ? DOCA_RDMA_VERBS_ADDR_TYPE_IPv6 :
								 DOCA_RDMA_VERBS_ADDR_TYPE_IPv4);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address type: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_ah_set_dlid(resources->ah, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DLID: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_ah_set_sgid_index(resources->ah, resources->gid_index);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SGIID: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_ah_set_hop_limit(resources->ah, DEFAULT_HOP_LIMIT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set hop limit: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = doca_rdma_verbs_ah_set_traffic_class(resources->ah, DEFAULT_TRAFFIC_CLASS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set traffic class: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = init_connection_resources(resources, &resources->conn1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize connection resources: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	result = init_connection_resources(resources, &resources->conn2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize connection resources: %s", doca_error_get_descr(result));
		goto exit_failure;
	}

	return DOCA_SUCCESS;

exit_failure:
	destroy_resources(resources);
	return result;
}

/**
 * Connects a QP to a destination QP
 *
 * @resources [in]: Pointer to message chaining send resources
 * @conn [in]: Pointer to connection resources
 * @qp [in]: QP to connect
 * @dest_qp_num [in]: Destination QP number
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t connect_qp(struct wait_cq_wr_resources *resources,
			       struct doca_rdma_verbs_qp *qp,
			       const uint32_t dest_qp_num)
{
	doca_error_t result, destroy_result;
	struct doca_rdma_verbs_qp_attr *rdma_verbs_qp_attr;
	int qp_attr_mask = 0;

	result = doca_rdma_verbs_qp_attr_create(&rdma_verbs_qp_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RDMA verbs QP attributes: %s", doca_error_get_descr(result));
		return result;
	}

	/* Set QP attributes for RST2INIT */
	result = doca_rdma_verbs_qp_attr_set_next_state(rdma_verbs_qp_attr, DOCA_RDMA_VERBS_QP_STATE_INIT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_allow_remote_write(rdma_verbs_qp_attr, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote write: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_allow_remote_read(rdma_verbs_qp_attr, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set allow remote read: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_port_num(rdma_verbs_qp_attr, DEFAULT_PORT_NUM);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set port number: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	/* Modify QP - RST2INIT */
	qp_attr_mask = DOCA_RDMA_VERBS_QP_ATTR_NEXT_STATE | DOCA_RDMA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
		       DOCA_RDMA_VERBS_QP_ATTR_ALLOW_REMOTE_READ | DOCA_RDMA_VERBS_QP_ATTR_PKEY_INDEX |
		       DOCA_RDMA_VERBS_QP_ATTR_PORT_NUM;

	result = doca_rdma_verbs_qp_modify(qp, rdma_verbs_qp_attr, qp_attr_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	/* Set QP attributes for INIT2RTR */
	result = doca_rdma_verbs_qp_attr_set_next_state(rdma_verbs_qp_attr, DOCA_RDMA_VERBS_QP_STATE_RTR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_rq_psn(rdma_verbs_qp_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RQ PSN: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_dest_qp_num(rdma_verbs_qp_attr, dest_qp_num);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set destination QP number: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_min_rnr_timer(rdma_verbs_qp_attr, DEFAULT_MIN_RNR_TIMER);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set minimum RNR timer: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_path_mtu(rdma_verbs_qp_attr, DOCA_MTU_SIZE_1K_BYTES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set path MTU: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_ah_attr(rdma_verbs_qp_attr, resources->ah);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set address handle: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	/* Modify QP - INIT2RTR */
	qp_attr_mask = DOCA_RDMA_VERBS_QP_ATTR_NEXT_STATE | DOCA_RDMA_VERBS_QP_ATTR_RQ_PSN |
		       DOCA_RDMA_VERBS_QP_ATTR_DEST_QP_NUM | DOCA_RDMA_VERBS_QP_ATTR_MIN_RNR_TIMER |
		       DOCA_RDMA_VERBS_QP_ATTR_PATH_MTU | DOCA_RDMA_VERBS_QP_ATTR_AH_ATTR;

	result = doca_rdma_verbs_qp_modify(qp, rdma_verbs_qp_attr, qp_attr_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	/* Set QP attributes for RTR2RTS */
	result = doca_rdma_verbs_qp_attr_set_next_state(rdma_verbs_qp_attr, DOCA_RDMA_VERBS_QP_STATE_RTS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set next state: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_sq_psn(rdma_verbs_qp_attr, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set SQ PSN: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_ack_timeout(rdma_verbs_qp_attr, DEFAULT_ACK_TIMEOUT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ACK timeout: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_retry_cnt(rdma_verbs_qp_attr, DEFAULT_RETRY_CNT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set retry counter: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	result = doca_rdma_verbs_qp_attr_set_rnr_retry(rdma_verbs_qp_attr, DEFAULT_RNR_RETRY);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set RNR retry: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	/* Modify QP - RTR2RTS */
	qp_attr_mask = DOCA_RDMA_VERBS_QP_ATTR_NEXT_STATE | DOCA_RDMA_VERBS_QP_ATTR_SQ_PSN |
		       DOCA_RDMA_VERBS_QP_ATTR_ACK_TIMEOUT | DOCA_RDMA_VERBS_QP_ATTR_RETRY_CNT |
		       DOCA_RDMA_VERBS_QP_ATTR_RNR_RETRY;

	result = doca_rdma_verbs_qp_modify(qp, rdma_verbs_qp_attr, qp_attr_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify QP: %s", doca_error_get_descr(result));
		goto destroy_verbs_qp_attr;
	}

	/* Destroy QP attributes */
	result = doca_rdma_verbs_qp_attr_destroy(rdma_verbs_qp_attr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy RDMA Verbs QP attributes: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("QP has been successfully connected and ready to use");

	return DOCA_SUCCESS;

destroy_verbs_qp_attr:
	destroy_result = doca_rdma_verbs_qp_attr_destroy(rdma_verbs_qp_attr);
	if (destroy_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy RDMA Verbs QP attributes: %s", doca_error_get_descr(destroy_result));

	return result;
}

/**
 * Initializes a connection between two QPs
 *
 * @resources [in]: Pointer to message chaining send resources
 * @conn [in]: Pointer to connection resources
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t init_connection(struct wait_cq_wr_resources *resources,
				    struct connection_resources *conn)
{
	doca_error_t result;
	uint32_t qp1_num, qp2_num;

	qp1_num = doca_rdma_verbs_qp_get_qpn(conn->qp1);
	qp2_num = doca_rdma_verbs_qp_get_qpn(conn->qp2);

	result = connect_qp(resources, conn->qp1, qp2_num);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect QP: %s", doca_error_get_descr(result));
		return result;
	}

	result = connect_qp(resources, conn->qp2, qp1_num);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect QP: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Creates memory buffers for RDMA operations
 *
 * @resources [in]: Pointer to message chaining send resources
 * @buf_size [in]: Size of each buffer
 * @num_buffers [in]: Number of buffers to create
 * @buffers_mr [out]: Pointer to store the memory region
 * @buffers [out]: Array to store the scatter/gather elements
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t create_buffers(struct wait_cq_wr_resources *resources,
				   uint32_t buf_size,
				   uint32_t num_buffers,
				   void *buffers_mem,
				   uint32_t mem_size,
				   struct ibv_mr **buffers_mr,
				   struct ibv_sge *buffers)
{
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

	struct ibv_mr *mr = ibv_reg_mr(resources->ibv_pd, buffers_mem, mem_size, access);
	if (mr == NULL) {
		return DOCA_ERROR_NO_MEMORY;
	}

	*buffers_mr = mr;

	for (uint32_t i = 0; i < num_buffers; i++) {
		buffers[i].addr = (uint64_t)buffers_mem + i * buf_size;
		buffers[i].length = buf_size;
		buffers[i].lkey = mr->lkey;
	}

	return DOCA_SUCCESS;
}

/**
 * Destroys memory buffers used for RDMA operations
 *
 * @mr [in]: Memory region to destroy
 */
static void destroy_buffers(struct ibv_mr *mr)
{
	if (mr != NULL) {
		ibv_dereg_mr(mr);
	}
}

/**
 * Posts a receive work request to a QP
 *
 * @qp [in]: QP to post the receive to
 * @wrid [in]: Work request ID
 * @buffer [in]: Buffer to receive data into
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t post_recv(struct doca_rdma_verbs_qp *qp, uint64_t wrid, struct ibv_sge *buffer)
{
	struct ibv_recv_wr wr = {0};
	struct ibv_recv_wr *bad_wr = NULL;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = wrid;
	wr.sg_list = buffer;
	wr.num_sge = 1;
	wr.next = NULL;

	int result = doca_rdma_verbs_bridge_post_recv(qp, &wr, &bad_wr);
	if (result != 0) {
		DOCA_LOG_ERR("Failed to post recv: %s", doca_error_get_descr(result));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}

/**
 * Posts a send work request to a QP
 *
 * @qp [in]: QP to post the send to
 * @wrid [in]: Work request ID
 * @buffer [in]: Buffer containing data to send
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t post_send(struct doca_rdma_verbs_qp *qp, uint64_t wrid, struct ibv_sge *buffer)
{
	struct ibv_send_wr wr = {0};
	struct ibv_send_wr *bad_wr = NULL;

	wr.wr_id = wrid;
	wr.next = NULL;
	wr.sg_list = buffer;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = 0;

	int result = doca_rdma_verbs_bridge_post_send(qp, &wr, &bad_wr);
	if (result != 0) {
		DOCA_LOG_ERR("Failed to post send: %s", doca_error_get_descr(result));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}

/**
 * Posts a send work request with wait operation to a QP
 *
 * @qp [in]: QP to post the send to
 * @wrid [in]: Work request ID
 * @cq [in]: Completion queue to wait on
 * @wait_pi [in]: Producer index to wait for
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
static doca_error_t post_send_wait(struct doca_rdma_verbs_qp *qp,
				   uint64_t wrid,
				   struct doca_rdma_verbs_cq *cq,
				   uint32_t wait_pi)
{
	struct ibv_send_wr wr = {0};
	struct ibv_send_wr *bad_wr = NULL;

	int result = doca_rdma_verbs_bridge_set_wait_cq_pi_wr(cq, wait_pi, &wr);
	if (result != 0) {
		DOCA_LOG_ERR("Failed to set wait CQ PI WR: %s", doca_error_get_descr(result));
		return DOCA_ERROR_DRIVER;
	}

	wr.wr_id = wrid;
	wr.next = NULL;
	wr.send_flags = 0;

	result = doca_rdma_verbs_bridge_post_send(qp, &wr, &bad_wr);
	if (result != 0) {
		DOCA_LOG_ERR("Failed to post send: %s", doca_error_get_descr(result));
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}

/**
 * Main function that demonstrates message chaining between QPs
 *
 * @cfg [in]: Configuration parameters for the sample
 * @return: DOCA_SUCCESS on success, error code otherwise
 */
doca_error_t send_chained_messages(struct wait_cq_wr_cfg *cfg)
{
	doca_error_t result;
	struct wait_cq_wr_resources resources = {0};
	struct ibv_mr *buffers_mr;
	struct ibv_sge buffers[3];
	uint64_t next_id = 0;
	struct ibv_wc wc = {0};
	uint32_t wait_pi;
	uint32_t buffers_mem_size;
	void *buffers_mem;

	result = init_resources(&resources, cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize resources: %s", doca_error_get_descr(result));
		goto exit;
	}

	result = init_connection(&resources, &resources.conn1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize connection: %s", doca_error_get_descr(result));
		goto exit;
	}

	result = init_connection(&resources, &resources.conn2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize connection: %s", doca_error_get_descr(result));
		goto exit;
	}

	buffers_mem_size = MESSAGE_SIZE * 3;
	buffers_mem = calloc(1, MESSAGE_SIZE * 3);
	if (buffers_mem == NULL) {
		DOCA_LOG_ERR("Failed to allocate buffers memory");
		result = DOCA_ERROR_NO_MEMORY;
		goto exit;
	}

	result = create_buffers(&resources, MESSAGE_SIZE, 3, buffers_mem, buffers_mem_size, &buffers_mr, buffers);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create buffers: %s", doca_error_get_descr(result));
		goto exit;
	}

	/* Fill first buffer with data */
	memset((void *)buffers[0].addr, 0x11, MESSAGE_SIZE);

	/* Sample logic:
	 * 2 Pairs of QPs, each pair use it's own CQ.
	 * We will call the first pair of QPs "QP1" and "QP2" and the second pair of QPs "QP3" and "QP4".
	 * QP1 sends a message (stored in buffer1) to QP2, which will be stored in buffer2.
	 * In the meantime, QP3 will be waiting for the message to arrive at buffer2 using message chaining,
	 * and once the message arrives it sends it's content from buffer2 to QP4, to be stored on buffer3.
	 * Chaining the messages promises the send from QP3 will not be preformed until the data is ready in buffer2.
	 */

	/* Post recv WRs on QP2 and QP4 in preperation for the messages */
	result = post_recv(resources.conn1.qp2, next_id++, &buffers[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to post recv: %s", doca_error_get_descr(result));
		goto free_buffers;
	}
	DOCA_LOG_INFO("Posted recv WR on QP2, WR ID %ld", next_id - 1);

	result = post_recv(resources.conn2.qp2, next_id++, &buffers[2]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to post recv: %s", doca_error_get_descr(result));
		goto free_buffers;
	}
	DOCA_LOG_INFO("Posted recv WR on QP4, WR ID %ld", next_id - 1);

	/* Pose Wait WR on QP3 and after that a send WR to send the data to QP4 */

	/* Calculate the wait PI value for the wait WR.
	 * We know there are no inflight WRs on CQ1, so we know the current CI is equal to the PI.
	 * If there were inflight WRs, we would need to add the number of inflight WRs to the current CI.
	 * The wait WR will wait until the CQ's PI passes the given value
	 * We want to wait for both the send and recv WRs from QP1 and QP2 to complete,
	 * so we will add 1 to the current CI so QP 3 will wait until CQ1's PI advances twice.
	 */
	wait_pi = doca_rdma_verbs_bridge_get_current_cq_ci(resources.conn1.cq) + 1;

	/* Post the wait WR on QP3, waiting for CQ1 to reach the given PI value */
	result = post_send_wait(resources.conn2.qp1, next_id++, resources.conn1.cq, wait_pi);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to post wait WR: %s", doca_error_get_descr(result));
		goto free_buffers;
	}
	DOCA_LOG_INFO("Posted wait WR on QP3 with WR ID %ld, waiting for CQ1 to reach PI %d", next_id - 1, wait_pi);

	/* Post the send WR on QP3, will only be executed after the wait WR completes */
	result = post_send(resources.conn2.qp1, next_id++, &buffers[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to post send: %s", doca_error_get_descr(result));
		goto free_buffers;
	}
	DOCA_LOG_INFO("Posted send WR on QP3 with WR ID %ld", next_id - 1);

	/* Post the send WR on QP1, sending the data to QP2. once both of them complete, the wait WR on QP3 will
	 * complete and the send WR will be processed. */
	result = post_send(resources.conn1.qp1, next_id++, &buffers[0]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to post send: %s", doca_error_get_descr(result));
		goto free_buffers;
	}
	DOCA_LOG_INFO("Posted send WR on QP1 with WR ID %ld", next_id - 1);

	/* Poll the CQ1 and wait for the send and recv WRs to complete, we expect 2 completions */
	for (uint32_t i = 0; i < 2; i++) {
		memset(&wc, 0, sizeof(wc));
		/* Poll the CQ until a completion is available */
		while (!doca_rdma_verbs_bridge_poll_cq(resources.conn1.cq, 1, &wc))
			;

		if (wc.status != IBV_WC_SUCCESS) {
			DOCA_LOG_ERR("Completion status for WR ID %ld is not success", wc.wr_id);
			result = DOCA_ERROR_DRIVER;
			goto free_buffers;
		}

		if (wc.opcode == IBV_WC_RECV) {
			DOCA_LOG_INFO("Recv WR completed:");
		} else if (wc.opcode == IBV_WC_SEND) {
			DOCA_LOG_INFO("Send WR completed:");
		} else {
			DOCA_LOG_ERR("Unexpected completion opcode: %d", wc.opcode);
			result = DOCA_ERROR_DRIVER;
			goto free_buffers;
		}

		DOCA_LOG_INFO("Completion for WR ID %ld", wc.wr_id);
	}

	/* Poll the CQ2 and wait for the send WR to complete, we expect 3 completions (including the wait WR) */
	for (uint32_t i = 0; i < 3; i++) {
		memset(&wc, 0, sizeof(wc));
		while (!doca_rdma_verbs_bridge_poll_cq(resources.conn2.cq, 1, &wc))
			;

		if (wc.status != IBV_WC_SUCCESS) {
			DOCA_LOG_ERR("Completion status for WR ID %ld is not success", wc.wr_id);
			result = DOCA_ERROR_DRIVER;
			goto free_buffers;
		}

		if (wc.opcode == IBV_WC_SEND) {
			DOCA_LOG_INFO("Send WR completed:");
		} else if (wc.opcode == IBV_WC_RECV) {
			DOCA_LOG_INFO("Recv WR completed:");
		} else if (wc.opcode == IBV_WC_DRIVER1) {
			/* The WC for the wait WR returns with the opcode IBV_WC_DRIVER1 */
			DOCA_LOG_INFO("Wait WR completed:");
		} else {
			DOCA_LOG_ERR("Unexpected completion opcode: %d", wc.opcode);
			result = DOCA_ERROR_DRIVER;
			goto free_buffers;
		}
		DOCA_LOG_INFO("Completion for WR ID %ld", wc.wr_id);
	}

	/* Verify the data was sent and received correctly, including the data in buffer3, which should be idenctical to
	 * the data in buffer1 if the chaining worked */
    if (memcmp((void *)buffers[0].addr, (void *)buffers[2].addr, MESSAGE_SIZE) != 0) {
        DOCA_LOG_ERR("Data in buffer3 is not identical to the data in buffer1");
        result = DOCA_ERROR_UNEXPECTED;
        goto free_buffers;
    } else {
		DOCA_LOG_INFO("Data in buffer3 is identical to the data in buffer1");
	}

	result = DOCA_SUCCESS;

free_buffers:
	if (buffers_mr != NULL)
		destroy_buffers(buffers_mr);
	free(buffers_mem);
exit:
	(void)destroy_resources(&resources);
	return result;
}
