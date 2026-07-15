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

#ifndef VERBS_COMMON_H_
#define VERBS_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_verbs_bridge.h>

#define MAX_IP_ADDRESS_LEN 128
#define DEFAULT_BUFFER_SIZE 64
#define DEFAULT_GID_INDEX 0
#define DEFAULT_NUM_OF_INTERATIONS 1
#define QP_TYPE_LEN 5
#define RDMA_OPER_TYPE_LEN 6
#define DEFAULT_PORT 1

enum verbs_sample_operation {
	VERBS_SAMPLE_OPERATION_SEND_RECEIVE, /**< RDMA send receive */
	VERBS_SAMPLE_OPERATION_WRITE,	     /**< RDMA write */
	VERBS_SAMPLE_OPERATION_READ,	     /**< RDMA read */
};

struct verbs_config {
	char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* DOCA device name */
	char server_ip_addr[MAX_IP_ADDRESS_LEN];	/* Server ip address */
	bool is_server;					/* Sample is acting as server or client */
	uint32_t gid_index;				/* GID index */
	uint32_t iter_num;				/* Number of iterations */
	size_t buf_size;				/* Buffer size */
	enum verbs_sample_operation rdma_oper_type;	/* RDMA operation type */
	uint32_t qp_type;				/* QP type */
	bool send_burst;				/* Send burst */
};

struct verbs_resources {
	struct verbs_config *cfg;		  /* RDMA Verbs Sample configuration parameters */
	struct doca_dev *dev;			  /* DOCA device */
	volatile void *local_buf;		  /* The local buffer */
	uint64_t remote_buf;			  /* The remote buffer */
	size_t remote_buf_size;			  /* The remote buffer size */
	struct doca_verbs_context *verbs_context; /* DOCA Verbs Context */
	struct doca_verbs_pd *verbs_pd;		  /* DOCA Verbs Protection Domain */
	struct doca_verbs_ah_attr *verbs_ah_attr; /* DOCA Verbs address handle */
	struct doca_verbs_qp *verbs_qp;		  /* DOCA Verbs Queue Pair */
	struct doca_verbs_cq *verbs_cq;		  /* DOCA Verbs Completion Queue */
	int conn_socket;			  /* Connection socket fd */
	uint32_t local_qp_number;		  /* Local QP number */
	uint32_t remote_qp_number;		  /* Remote QP number */
	uint32_t local_mkey;			  /* local MKEY */
	uint32_t remote_mkey;			  /* remote MKEY */
	struct ibv_mr *mr;			  /* local memory region */
	struct ibv_pd *pd;			  /* local protection domain */
	struct doca_verbs_gid gid;		  /* local gid address */
	struct doca_verbs_gid remote_gid;	  /* remote gid address */
	uint16_t local_lid;			  /* local lid address */
	uint16_t remote_lid;			  /* remote lid address */
	enum doca_verbs_addr_type addr_type;	  /* address type */
};

/*
 * Server side of the RDMA Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t verbs_server(struct verbs_config *cfg);

/*
 * Client side of the RDMA Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t verbs_client(struct verbs_config *cfg);

#endif /* VERBS_COMMON_H_ */