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

#ifndef GPUNETIO_RDMA_VERBS_COMMON_H_
#define GPUNETIO_RDMA_VERBS_COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cuda.h>
#include <cuda_runtime_api.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_umem.h>
#include <doca_uar.h>

#include <doca_rdma_verbs.h>
#include <doca_rdma_verbs_bridge.h>
#include <doca_rdma_bridge.h>
#include <doca_gpunetio.h>
#include <doca_gpunetio_rdma_verbs_def.h>

#define ACCESS_VOLATILE(x) (*(volatile typeof(x) *)&(x))

#ifndef ACCESS_ONCE_64b
#define ACCESS_ONCE_64b(x) (*(volatile uint64_t *)&(x))
#endif

#ifndef WRITE_ONCE_64b
#define WRITE_ONCE_64b(x, v) (ACCESS_ONCE_64b(x) = (v))
#endif

#define NUM_QP 1
#define CUDA_THREADS_BW 512 // post list
#define CUDA_THREADS_LAT 1  // post list
#define NUM_ITERS 2048
#define NUM_MSG_SIZE 10

#define RDMA_VERBS_TEST_QUEUE_SIZE (2048)
// Should be sizeof(doca gpu verbs structs)
#define RDMA_VERBS_TEST_DBR_SIZE (64)
#define RDMA_VERBS_TEST_SIN_PORT (5000)
#define RDMA_VERBS_TEST_HOP_LIMIT (255)
#define RDMA_VERBS_TEST_MAX_SEND_SEGS (1)
#define RDMA_VERBS_TEST_MAX_RECEIVE_SEGS (1)
#define RDMA_VERBS_TEST_LOCAL_BUF_VALUE (0xA)
#define RDMA_VERBS_CUDA_BLOCK 2

#define DEFAULT_GID_INDEX (0)
#define MAX_PCI_ADDRESS_LEN 32U
#define MAX_IP_ADDRESS_LEN 128
#define SYSTEM_PAGE_SIZE 4096 /* Page size sysconf(_SC_PAGESIZE)*/

#define CPU_TO_BE16(val) __builtin_bswap16(val)
#define BE_TO_CPU16(val) __builtin_bswap16(val)
#define CPU_TO_BE32(val) __builtin_bswap32(val)
#define BE_TO_CPU32(val) __builtin_bswap32(val)
#define CPU_TO_BE64(val) __builtin_bswap64(val)
#define BE_TO_CPU64(val) __builtin_bswap64(val)

struct rdma_verbs_config {
	char nic_device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* SF DOCA device name */
	char gpu_pcie_addr[MAX_PCI_ADDRESS_LEN];	    /* PF DOCA device name */
	bool is_server;					    /* Sample is acting as client or server */
	uint32_t gid_index;				    /* GID index */
	char server_ip_addr[MAX_IP_ADDRESS_LEN];	    /* DOCA device name */
	uint32_t num_iters;				    /* total number of iterations per cuda kernel */
	uint32_t cuda_threads;				    /* cuda threads per cuda block */
	bool enable_cpu_proxy;				    /* enable CPU proxy */
	uint8_t exec_policy;
};

struct rdma_verbs_resources {
	struct rdma_verbs_config *cfg;		       /* RDMA Verbs test configuration parameters */
	struct doca_dev *dev;			       /* DOCA device to use */
	struct doca_gpu *gpu_dev;		       /* DOCA GPU device to use */
	uint8_t *data_buf[NUM_MSG_SIZE];	       /* The local data buffer */
	uint64_t *flag_buf[NUM_MSG_SIZE];	       /* The local data buffer */
	uint64_t remote_data_buf[NUM_MSG_SIZE];	       /* The remote buffer */
	uint64_t remote_flag_buf[NUM_MSG_SIZE];	       /* The remote buffer */
	struct doca_rdma_verbs_context *verbs_context; /* DOCA Verbs Context */
	struct doca_rdma_verbs_pd *verbs_pd;	       /* DOCA Verbs Protection Domain */
	struct doca_rdma_verbs_ah *verbs_ah;	       /* DOCA Verbs address handle */
	void *gpu_qp_umem_dev_ptr;
	struct doca_umem *gpu_qp_umem;
	struct doca_rdma_verbs_qp *verbs_qp; /* DOCA Verbs Protection Queue Pair */
	void *gpu_cq_sq_umem_dev_ptr;
	void *gpu_cq_rq_umem_dev_ptr;
	struct doca_umem *gpu_cq_sq_umem;
	struct doca_umem *gpu_cq_rq_umem;
	struct doca_rdma_verbs_cq *verbs_cq_sq;	      /* DOCA Verbs Protection Completion Queue */
	struct doca_rdma_verbs_cq *verbs_cq_rq;	      /* DOCA Verbs Protection Completion Queue */
	int conn_socket;			      /* Connection socket fd */
	uint32_t local_qp_number;		      /* Local QP number */
	uint32_t local_cq_sq_number;		      /* Local CQ SQ number */
	uint32_t local_cq_rq_number;		      /* Local CQ RQ number */
	uint32_t remote_qp_number;		      /* Remote QP number */
	uint32_t remote_data_mkey[NUM_MSG_SIZE];      /* remote MKEY */
	uint32_t remote_flag_mkey[NUM_MSG_SIZE];      /* remote MKEY */
	struct ibv_mr *data_mr[NUM_MSG_SIZE];	      /* local memory region */
	struct ibv_mr *flag_mr[NUM_MSG_SIZE];	      /* local memory region */
	struct ibv_pd *pd;			      /* local protection domain */
	struct doca_rdma_verbs_gid gid;		      /* local gid address */
	struct doca_rdma_verbs_gid remote_gid;	      /* remote gid address */
	int lid;				      /* IB: local ID */
	int dlid;				      /* IB: destination ID */
	struct doca_gpu_rdma_verbs_cq *cq_rq_cpu;     /* GPU handler with CQ RQ info */
	struct doca_gpu_rdma_verbs_cq *cq_sq_cpu;     /* GPU handler with CQ SQ info */
	struct doca_gpu_rdma_verbs_qp *qp_cpu;	      /* GPU handler with QP info */
	struct doca_gpu_dev_rdma_verbs_cq *cq_rq_gpu; /* GPU handler with CQ RQ info */
	struct doca_gpu_dev_rdma_verbs_cq *cq_sq_gpu; /* GPU handler with CQ SQ info */
	struct doca_gpu_dev_rdma_verbs_qp *qp_gpu;    /* GPU handler with QP info */
	uint32_t num_iters;			      /* total number of iterations per cuda kernel */
	uint32_t cuda_threads;			      /* threads */
	bool enable_cpu_proxy;			      /* enable CPU proxy */
	enum doca_gpu_dev_rdma_verbs_exec_scope scope;

	/* write_lat test */
	struct ibv_mr *local_poll_mr[NUM_MSG_SIZE]; /* local memory region */
	struct ibv_mr *local_post_mr[NUM_MSG_SIZE]; /* local memory region */

	uint8_t *local_poll_buf[NUM_MSG_SIZE]; /* The local data buffer */
	uint8_t *local_post_buf[NUM_MSG_SIZE]; /* The local data buffer */
};

struct cpu_proxy_args {
	struct doca_gpu_rdma_verbs_qp *qp_cpu;
	uint64_t *exit_flag;
};

/*
 * Target side of the RDMA Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_verbs_server(struct rdma_verbs_config *cfg);

/*
 * Initiator side of the RDMA Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_verbs_client(struct rdma_verbs_config *cfg);

/*
 * OOB connection to exchange RDMA info - server side
 *
 * @oob_sock_fd [out]: Socket FD
 * @oob_client_sock [out]: Client socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_rdma_verbs_connection_server_setup(int *oob_sock_fd, int *oob_client_sock);

/*
 * OOB connection to exchange RDMA info - server side closure
 *
 * @oob_sock_fd [in]: Socket FD
 * @oob_client_sock [in]: Client socket FD
 */
void oob_rdma_verbs_connection_server_close(int oob_sock_fd, int oob_client_sock);

/*
 * OOB connection to exchange RDMA info - client side
 *
 * @server_ip [in]: Server IP address to connect
 * @oob_sock_fd [out]: Socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_rdma_verbs_connection_client_setup(const char *server_ip, int *oob_sock_fd);

/*
 * OOB connection to exchange RDMA info - client side closure
 *
 * @oob_sock_fd [in]: Socket FD
 */
void oob_rdma_verbs_connection_client_close(int oob_sock_fd);

/*
 * Create and initialize DOCA RDMA Verbs resources
 *
 * @cfg [in]: Configuration parameters
 * @resources [in/out]: DOCA RDMA resources to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_rdma_verbs_resources(struct rdma_verbs_config *cfg, struct rdma_verbs_resources *resources);

/*
 * Destroy DOCA RDMA resources
 *
 * @resources [in]: DOCA RDMA resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_rdma_verbs_resources(struct rdma_verbs_resources *resources);

/*
 * Connect a DOCA RDMA Verbs QP to a remote one
 *
 * @resources [in]: DOCA RDMA Verbs resources with the QP
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t connect_rdma_verbs_qp(struct rdma_verbs_resources *resources);

doca_error_t export_datapath_attr_in_gpu(struct rdma_verbs_resources *resources);

/*
 * Server side of the RDMA write
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_write_server(struct rdma_verbs_config *cfg);

/*
 * Client side of the RDMA write
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_write_client(struct rdma_verbs_config *cfg);

/*
 * CPU proxy progresses the QP
 *
 * @args [in]: thread args
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
void *progress_cpu_proxy(void *args_);

#if __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel doing RDMA Write client
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @rdma_gpu [in]: RDMA GPU object
 * @client_local_buf_arr_B [in]: GPU buffer with local data B
 * @client_local_buf_arr_C [in]: GPU buffer with local data C
 * @client_local_buf_arr_F [in]: GPU buffer with local data F
 * @client_remote_buf_arr_A [in]: GPU buffer on remote server with data A
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_rdma_verbs_write_bw(cudaStream_t stream,
					  struct doca_gpu_dev_rdma_verbs_qp *qp,
					  uint32_t num_iters,
					  uint32_t cuda_blocks,
					  uint32_t cuda_threads,
					  uint32_t size,
					  uint8_t *src_buf,
					  uint32_t src_mkey,
					  uint8_t *dst_buf,
					  uint32_t dst_mkey,
					  bool is_cpu_proxy);

doca_error_t gpunetio_rdma_verbs_write_lat(cudaStream_t stream,
					   struct doca_gpu_dev_rdma_verbs_qp *qp,
					   uint32_t num_iters,
					   uint32_t cuda_blocks,
					   uint32_t cuda_threads,
					   uint32_t size,
					   uint8_t *local_poll_buf,
					   uint32_t local_poll_mkey,
					   uint8_t *local_post_buf,
					   uint32_t local_post_mkey,
					   uint8_t *dst_buf,
					   uint32_t dst_mkey,
					   bool is_cpu_proxy,
					   bool is_client);

doca_error_t gpunetio_rdma_verbs_client_server(cudaStream_t stream,
					       struct doca_gpu_dev_rdma_verbs_qp *qp,
					       uint32_t num_iters,
					       uint32_t cuda_threads,
					       uint32_t data_size,
					       uint8_t *src_buf,
					       uint32_t src_buf_mkey,
					       uint64_t *src_flag,
					       uint32_t src_flag_mkey,
					       uint8_t *dst_buf,
					       uint32_t dst_buf_mkey,
					       uint64_t *dst_flag,
					       uint32_t dst_flag_mkey,
					       bool is_client);

doca_error_t gpunetio_rdma_verbs_ops_bw(cudaStream_t stream,
					struct doca_gpu_dev_rdma_verbs_qp *qp,
					uint32_t cuda_threads_iters,
					uint32_t cuda_blocks,
					uint32_t cuda_threads,
					uint32_t size,
					uint8_t *src_buf,
					uint32_t src_mkey,
					uint8_t *dst_buf,
					uint32_t dst_mkey,
					uint64_t *src_flag,
					uint32_t src_flag_mkey,
					uint64_t *dst_flag,
					uint32_t dst_flag_mkey,
					bool is_cpu_proxy,
					enum doca_gpu_dev_rdma_verbs_exec_scope scope);

#if __cplusplus
}
#endif

#endif /* GPUNETIO_RDMA_VERBS_COMMON_H_ */
