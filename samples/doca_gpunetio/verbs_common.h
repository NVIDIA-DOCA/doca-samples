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

#ifndef GPUNETIO_VERBS_COMMON_H_
#define GPUNETIO_VERBS_COMMON_H_

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

#include <doca_verbs.h>
#include <doca_verbs_bridge.h>
#include <doca_rdma_bridge.h>
#include <doca_gpunetio.h>
#include <doca_gpunetio_verbs_def.h>

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

#define VERBS_TEST_QUEUE_SIZE (2048)
// Should be sizeof(doca gpu verbs structs)
#define VERBS_TEST_DBR_SIZE (8)
#define VERBS_TEST_SIN_PORT (5000)
#define VERBS_TEST_HOP_LIMIT (255)
#define VERBS_TEST_MAX_SEND_SEGS (1)
#define VERBS_TEST_MAX_RECEIVE_SEGS (1)
#define VERBS_TEST_LOCAL_BUF_VALUE (0xA)
#define VERBS_CUDA_BLOCK 2

#define DEFAULT_GID_INDEX (0)
#define MAX_PCI_ADDRESS_LEN 32U
#define MAX_IP_ADDRESS_LEN 128
#define SYSTEM_PAGE_SIZE 4096 /* Page size sysconf(_SC_PAGESIZE)*/

#define ROUND_UP(unaligned_mapping_size, align_val) ((unaligned_mapping_size) + (align_val)-1) & (~((align_val)-1))

/* High-level API */
struct doca_gpu_verbs_qp_init_attr_hl {
	struct doca_gpu *gpu_dev;
	struct doca_dev *dev;
	struct doca_verbs_context *verbs_context;
	struct doca_verbs_pd *verbs_pd;
	uint16_t sq_nwqe;
	uint16_t rq_nwqe;
	enum doca_gpu_dev_verbs_nic_handler nic_handler;
};

struct doca_gpu_verbs_qp_hl {
	struct doca_gpu *gpu_dev; /* DOCA GPU device to use */

	// CQ
	struct doca_verbs_cq *cq_rq;
	struct doca_verbs_cq *cq_sq;
	void *cq_sq_umem_gpu_ptr;
	void *cq_rq_umem_gpu_ptr;
	struct doca_umem *cq_sq_umem;
	struct doca_umem *cq_rq_umem;
	void *cq_sq_umem_dbr_gpu_ptr;
	void *cq_rq_umem_dbr_gpu_ptr;
	struct doca_umem *cq_sq_umem_dbr;
	struct doca_umem *cq_rq_umem_dbr;

	// QP
	struct doca_verbs_qp *qp;
	void *qp_umem_gpu_ptr;
	struct doca_umem *qp_umem;
	void *qp_umem_dbr_gpu_ptr;
	struct doca_umem *qp_umem_dbr;
	struct doca_uar *external_uar;

	enum doca_gpu_dev_verbs_nic_handler nic_handler;

	// QP GPUNetIO Object
	struct doca_gpu_verbs_qp *qp_gverbs;
};

struct doca_gpu_verbs_qp_group_hl {
	struct doca_gpu_verbs_qp_hl qp_main;
	struct doca_gpu_verbs_qp_hl qp_companion;
};

struct verbs_config {
	char nic_device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* SF DOCA device name */
	char gpu_pcie_addr[MAX_PCI_ADDRESS_LEN];	    /* PF DOCA device name */
	bool is_server;					    /* Sample is acting as client or server */
	uint32_t gid_index;				    /* GID index */
	char server_ip_addr[MAX_IP_ADDRESS_LEN];	    /* DOCA device name */
	uint32_t num_iters;				    /* total number orations per cuda kernel */
	uint32_t cuda_threads;				    /* cuda threads per cuda block */
	enum doca_gpu_dev_verbs_nic_handler nic_handler;
	uint8_t exec_scope;
};

struct verbs_resources {
	struct verbs_config *cfg;		   /* Verbs test configuration parameters */
	struct doca_dev *dev;			   /* DOCA device to use */
	struct doca_gpu *gpu_dev;		   /* DOCA GPU device to use */
	uint8_t *data_buf[NUM_MSG_SIZE];	   /* The local data buffer */
	uint64_t *flag_buf[NUM_MSG_SIZE];	   /* The local data buffer */
	uint64_t *prev_flag_buf[NUM_MSG_SIZE];	   /* The local data buffer */
	uint64_t remote_data_buf[NUM_MSG_SIZE];	   /* The remote buffer */
	uint64_t remote_flag_buf[NUM_MSG_SIZE];	   /* The remote buffer */
	uint64_t *dump_flag_buf;		   /* The remote buffer */
	struct doca_verbs_context *verbs_context;  /* DOCA Verbs Context */
	struct doca_verbs_pd *verbs_pd;		   /* DOCA Verbs Protection Domain */
	struct doca_verbs_ah_attr *verbs_ah_attr;  /* DOCA Verbs address handle */
	int conn_socket;			   /* Connection socket fd */
	uint32_t local_qp_number;		   /* Local QP number */
	uint32_t local_qp_main_number;		   /* Local QP main number, main QP*/
	uint32_t local_qp_companion_number;	   /* Local QP companion number */
	uint32_t local_cq_sq_number;		   /* Local CQ SQ number */
	uint32_t local_cq_rq_number;		   /* Local CQ RQ number */
	uint32_t remote_qp_number;		   /* Remote QP number */
	uint32_t remote_qp_main_number;		   /* Remote QP main number, main QP*/
	uint32_t remote_qp_companion_number;	   /* Remote QP companion number */
	uint32_t remote_data_mkey[NUM_MSG_SIZE];   /* remote MKEY */
	uint32_t remote_flag_mkey[NUM_MSG_SIZE];   /* remote MKEY */
	struct ibv_mr *data_mr[NUM_MSG_SIZE];	   /* local memory region */
	struct ibv_mr *flag_mr[NUM_MSG_SIZE];	   /* local memory region */
	struct ibv_mr *prev_flag_mr[NUM_MSG_SIZE]; /* local memory region */
	struct ibv_mr *dump_flag_mr;		   /* local memory region */
	struct ibv_pd *pd;			   /* local protection domain */
	struct doca_verbs_gid gid;		   /* local gid address */
	struct doca_verbs_gid remote_gid;	   /* remote gid address */
	int lid;				   /* IB: local ID */
	int dlid;				   /* IB: destination ID */
	uint32_t num_iters;			   /* total number orations per cuda kernel */
	uint32_t cuda_threads;			   /* threads */
	struct doca_gpu_verbs_qp_hl *qp;	   /* DOCA GPUNetIO high-level Verbs QP */
	bool qp_group;
	struct doca_gpu_verbs_qp_group_hl *qpg;
	enum doca_gpu_dev_verbs_nic_handler nic_handler;
	enum doca_gpu_dev_verbs_exec_scope scope;

	/* _lat test */
	struct ibv_mr *local_poll_mr[NUM_MSG_SIZE]; /* local memory region */
	struct ibv_mr *local_post_mr[NUM_MSG_SIZE]; /* local memory region */

	uint8_t *local_poll_buf[NUM_MSG_SIZE]; /* The local data buffer */
	uint8_t *local_post_buf[NUM_MSG_SIZE]; /* The local data buffer */
};

struct cpu_proxy_args {
	struct doca_gpu_verbs_qp *qp_cpu_main;
	struct doca_gpu_verbs_qp *qp_cpu_companion;
	uint64_t *exit_flag;
};

doca_error_t nic_handler_callback(void *param, void *config);
doca_error_t nic_device_name_callback(void *param, void *config);
doca_error_t gpu_pcie_addr_callback(void *param, void *config);
doca_error_t gid_index_callback(void *param, void *config);
doca_error_t client_param_callback(void *param, void *config);
doca_error_t iters_callback(void *param, void *config);
doca_error_t threads_callback(void *param, void *config);
doca_error_t exec_callback(void *param, void *config);

/*
 * Target side of the Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t verbs_server(struct verbs_config *cfg);

/*
 * Initiator side of the Verbs sample
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t verbs_client(struct verbs_config *cfg);

/*
 * OOB connection to exchange RDMA info - server side
 *
 * @oob_sock_fd [out]: Socket FD
 * @oob_client_sock [out]: Client socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_verbs_connection_server_setup(int *oob_sock_fd, int *oob_client_sock);

/*
 * OOB connection to exchange RDMA info - server side closure
 *
 * @oob_sock_fd [in]: Socket FD
 * @oob_client_sock [in]: Client socket FD
 */
void oob_verbs_connection_server_close(int oob_sock_fd, int oob_client_sock);

/*
 * OOB connection to exchange RDMA info - client side
 *
 * @server_ip [in]: Server IP address to connect
 * @oob_sock_fd [out]: Socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_verbs_connection_client_setup(const char *server_ip, int *oob_sock_fd);

/*
 * OOB connection to exchange RDMA info - client side closure
 *
 * @oob_sock_fd [in]: Socket FD
 */
void oob_verbs_connection_client_close(int oob_sock_fd);

/*
 * Create and initialize DOCA Verbs resources
 *
 * @cfg [in]: Configuration parameters
 * @resources [in/out]: DOCA RDMA resources to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_verbs_resources(struct verbs_config *cfg, struct verbs_resources *resources);

/*
 * Destroy DOCA RDMA resources
 *
 * @resources [in]: DOCA RDMA resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_verbs_resources(struct verbs_resources *resources);

/*
 * Connect a DOCA Verbs QP to a remote one
 *
 * @resources [in]: DOCA Verbs resources with the QP
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t connect_verbs_qp(struct verbs_resources *resources);

/*
 * CPU proxy progresses the QP
 *
 * @args [in]: thread args
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
void *progress_cpu_proxy(void *args_);

/**
 * Create an high-level GPUNetIO QP.
 * This function encapsulate all required steps using doca verbs and doca gpunetio to
 * create a GDAKI QP.
 *
 * @param [in] qp_init_attr
 * High-level QP init attributes.
 * @param [out] qp
 * GPUNetIO device handler.
 *
 * @return
 * DOCA_SUCCESS - in case of success.
 * doca_error code - in case of failure:
 * - DOCA_ERROR_INVALID_VALUE - if an invalid input had been received.
 */
doca_error_t doca_gpu_verbs_create_qp_hl(struct doca_gpu_verbs_qp_init_attr_hl *qp_init_attr,
					 struct doca_gpu_verbs_qp_hl **qp);

/**
 * Destroy an high-level GPUNetIO QP.
 *
 * @param [in] qp
 * GPUNetIO high-level QP to destroy
 *
 * @return
 * DOCA_SUCCESS - in case of success.
 * doca_error code - in case of failure:
 * - DOCA_ERROR_INVALID_VALUE - if an invalid input had been received.
 */
doca_error_t doca_gpu_verbs_destroy_qp_hl(struct doca_gpu_verbs_qp_hl *qp);

/**
 * Create an high-level GPUNetIO QP group (main and companion).
 * This function encapsulate all required steps using doca verbs and doca gpunetio to
 * create two GDAKI QPs, main one and the one used for core direct operations.
 * The two QPs share the same UAR.
 *
 * @param [in] qp_init_attr
 * High-level QP init attributes.
 * @param [out] qpg
 * GPUNetIO QP Group device handler.
 *
 * @return
 * DOCA_SUCCESS - in case of success.
 * doca_error code - in case of failure:
 * - DOCA_ERROR_INVALID_VALUE - if an invalid input had been received.
 */
doca_error_t doca_gpu_verbs_create_qp_group_hl(struct doca_gpu_verbs_qp_init_attr_hl *qp_init_attr,
					       struct doca_gpu_verbs_qp_group_hl **qpg);

/**
 * Destroy an high-level GPUNetIO QP group.
 *
 * @param [in] qp
 * GPUNetIO high-level QP group to destroy
 *
 * @return
 * DOCA_SUCCESS - in case of success.
 * doca_error code - in case of failure:
 * - DOCA_ERROR_INVALID_VALUE - if an invalid input had been received.
 */
doca_error_t doca_gpu_verbs_destroy_qp_group_hl(struct doca_gpu_verbs_qp_group_hl *qpg);

#if __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel with to measure RDMA Write bandwidth
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @num_iters [in]: Total number of iterations
 * @cuda_blocks [in]: Number of CUDA blocks to launch the kernel
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_write_bw(cudaStream_t stream,
				     struct doca_gpu_dev_verbs_qp *qp,
				     uint32_t num_iters,
				     uint32_t cuda_blocks,
				     uint32_t cuda_threads,
				     uint32_t data_size,
				     uint8_t *src_buf,
				     uint32_t src_buf_mkey,
				     uint8_t *dst_buf,
				     uint32_t dst_buf_mkey);

/*
 * Launch a CUDA kernel with to measure RDMA Write latency
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @num_iters [in]: Total number of iterations
 * @cuda_blocks [in]: Number of CUDA blocks to launch the kernel
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel
 * @data_size [in]: Data buffer size (number of bytes)
 * @local_poll_buf [in]: Flag address to poll waiting for remote peer notification
 * @local_poll_mkey [in]: Flag mkey to poll waiting for remote peer notification
 * @local_post_buf [in]: Flag address to post notification to remote peer
 * @local_post_mkey [in]: Flag mkey to post notification to remote peer
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @is_client [in]: This kernel should act like the client (true) or server (false)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_write_lat(cudaStream_t stream,
				      struct doca_gpu_dev_verbs_qp *qp,
				      uint32_t num_iters,
				      uint32_t cuda_blocks,
				      uint32_t cuda_threads,
				      uint32_t data_size,
				      uint8_t *local_poll_buf,
				      uint32_t local_poll_mkey,
				      uint8_t *local_post_buf,
				      uint32_t local_post_mkey,
				      uint8_t *dst_buf,
				      uint32_t dst_buf_mkey,
				      bool is_client);

/*
 * Launch a CUDA kernel to test two sided (send/recv) operations
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @start_iters [in]: Start iteration idx (after warmup)
 * @num_iters [in]: Total number of iterations
 * @cuda_blocks [in]: Number of CUDA blocks to launch the kernel
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @src_flag [in]: Source GPU flag buffer address. Used by client to poll waiting for server notification.
 * @src_flag_mkey [in]: Source GPU data buffer memory key
 * @dst_flag [in]: Destination GPU data buffer address. Used by server to notify client.
 * @dst_flag_mkey [in]: Destination GPU data buffer memory key
 * @dump_flag [in]: Flag in GPU memory to use with Dump WQE, if needed
 * @dump_flag_mkey [in]: Dump flag memory key
 * @scope [in]: Each put is called per CUDA thread or per CUDA warp
 * @is_client [in]: This kernel should act like the client (true) or server (false)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_two_sided_bw(cudaStream_t stream,
					 struct doca_gpu_dev_verbs_qp *qp,
					 uint32_t start_iters,
					 uint32_t num_iters,
					 uint32_t cuda_blocks,
					 uint32_t cuda_threads,
					 uint32_t data_size,
					 uint8_t *src_buf,
					 uint32_t src_buf_mkey,
					 uint64_t *src_flag,
					 uint32_t src_flag_mkey,
					 uint64_t *dst_flag,
					 uint32_t dst_flag_mkey,
					 uint64_t *dump_flag,
					 uint32_t dump_flag_mkey,
					 enum doca_gpu_dev_verbs_exec_scope scope,
					 bool is_client);
/*
 * Launch a CUDA kernel with to measure One-Sided Put Shared QP bandwidth
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @num_iters [in]: Total number of iterations
 * @cuda_blocks [in]: Number of CUDA blocks to launch the kernel
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @scope [in]: Each put is called per CUDA thread or per CUDA warp
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_put_bw(cudaStream_t stream,
				   struct doca_gpu_dev_verbs_qp *qp,
				   uint32_t num_iters,
				   uint32_t cuda_blocks,
				   uint32_t cuda_threads,
				   uint32_t data_size,
				   uint8_t *src_buf,
				   uint32_t src_buf_mkey,
				   uint8_t *dst_buf,
				   uint32_t dst_buf_mkey,
				   enum doca_gpu_dev_verbs_exec_scope scope);

/*
 * Launch a CUDA kernel with to measure One-Sided Put Signal Shared QP bandwidth
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @num_iters [in]: Total number of iterations
 * @cuda_blocks [in]: Number of CUDA blocks to launch the kernel
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @prev_flag_buf [in]: Previous GPU atomic fetch and add flag value address
 * @prev_flag_buf_mkey [in]: Previous GPU atomic fetch and add flag value memory key
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @dst_flag [in]: Destination GPU atomic fetch and add flag address
 * @dst_flag_mkey [in]: Destination GPU atomic fetch and add flag memory key
 * @scope [in]: Each put is called per CUDA thread or per CUDA warp
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_put_signal_bw(cudaStream_t stream,
					  struct doca_gpu_dev_verbs_qp *qp,
					  uint32_t num_iters,
					  uint32_t cuda_blocks,
					  uint32_t cuda_threads,
					  uint32_t data_size,
					  uint8_t *src_buf,
					  uint32_t src_buf_mkey,
					  uint64_t *prev_flag_buf,
					  uint32_t prev_flag_buf_mkey,
					  uint8_t *dst_buf,
					  uint32_t dst_buf_mkey,
					  uint64_t *dst_flag,
					  uint32_t dst_flag_mkey,
					  enum doca_gpu_dev_verbs_exec_scope scope);

/*
 * Launch a CUDA kernel with to measure One-Sided Put Signal Shared QP latency
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @start_iters [in]: Start iteration idx (after warmup)
 * @num_iters [in]: Total number of iterations
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel. Always 1.
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @src_flag [in]: Source GPU flag updated by fetch atomic, address
 * @src_flag_mkey [in]: Source GPU flag updated by fetch atomic, memory key
 * @prev_flag_buf [in]: Previous GPU atomic fetch and add flag value address
 * @prev_flag_buf_mkey [in]: Previous GPU atomic fetch and add flag value memory key
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @dst_flag [in]: Destination GPU atomic fetch and add flag address
 * @dst_flag_mkey [in]: Destination GPU atomic fetch and add flag memory key
 * @is_client [in]: This kernel should act like the client (true) or server (false)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_put_signal_lat(cudaStream_t stream,
					   struct doca_gpu_dev_verbs_qp *qp,
					   uint32_t start_iters,
					   uint32_t num_iters,
					   uint32_t cuda_threads,
					   uint32_t data_size,
					   uint8_t *src_buf,
					   uint32_t src_buf_mkey,
					   uint64_t *src_flag,
					   uint32_t src_flag_mkey,
					   uint64_t *prev_flag_buf,
					   uint32_t prev_flag_buf_mkey,
					   uint8_t *dst_buf,
					   uint32_t dst_buf_mkey,
					   uint64_t *dst_flag,
					   uint32_t dst_flag_mkey,
					   bool is_client);

/*
 * Launch a CUDA kernel with to measure One-Sided Put Counter Shared QP bandwidth
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @num_iters [in]: Total number of iterations
 * @cuda_blocks [in]: Number of CUDA blocks to launch the kernel
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @prev_flag_buf [in]: Previous GPU atomic fetch and add flag value address
 * @prev_flag_buf_mkey [in]: Previous GPU atomic fetch and add flag value memory key
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @dst_flag [in]: Destination GPU atomic fetch and add flag address
 * @dst_flag_mkey [in]: Destination GPU atomic fetch and add flag memory key
 * @scope [in]: Each put is called per CUDA thread or per CUDA warp
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_put_counter_bw(cudaStream_t stream,
					   struct doca_gpu_dev_verbs_qp *qp_main,
					   struct doca_gpu_dev_verbs_qp *qp_companion,
					   uint32_t num_iters,
					   uint32_t cuda_blocks,
					   uint32_t cuda_threads,
					   uint32_t data_size,
					   uint8_t *src_buf,
					   uint32_t src_buf_mkey,
					   uint64_t *prev_flag_buf,
					   uint32_t prev_flag_buf_mkey,
					   uint8_t *dst_buf,
					   uint32_t dst_buf_mkey,
					   uint64_t *dst_flag,
					   uint32_t dst_flag_mkey,
					   enum doca_gpu_dev_verbs_exec_scope scope);

/*
 * Launch a CUDA kernel with to measure One-Sided Put Counter Shared QP latency
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @qp [in]: Verbs GPU object
 * @start_iters [in]: Start iteration idx (after warmup)
 * @num_iters [in]: Total number of iterations
 * @cuda_threads [in]: Number of CUDA threads to launch the kernel. Always 1.
 * @data_size [in]: Data buffer size (number of bytes)
 * @src_buf [in]: Source GPU data buffer address
 * @src_buf_mkey [in]: Source GPU data buffer memory key
 * @src_flag [in]: Source GPU flag updated by fetch atomic, address
 * @src_flag_mkey [in]: Source GPU flag updated by fetch atomic, memory key
 * @prev_flag_buf [in]: Previous GPU atomic fetch and add flag value address
 * @prev_flag_buf_mkey [in]: Previous GPU atomic fetch and add flag value memory key
 * @dst_buf [in]: Destination GPU data buffer address
 * @dst_buf_mkey [in]: Destination GPU data buffer memory key
 * @dst_flag [in]: Destination GPU atomic fetch and add flag address
 * @dst_flag_mkey [in]: Destination GPU atomic fetch and add flag memory key
 * @is_client [in]: This kernel should act like the client (true) or server (false)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_verbs_put_counter_lat(cudaStream_t stream,
					    struct doca_gpu_dev_verbs_qp *qp_main,
					    struct doca_gpu_dev_verbs_qp *qp_companion,
					    uint32_t start_iters,
					    uint32_t num_iters,
					    uint32_t cuda_threads,
					    uint32_t data_size,
					    uint8_t *src_buf,
					    uint32_t src_buf_mkey,
					    uint64_t *src_flag,
					    uint32_t src_flag_mkey,
					    uint64_t *prev_flag_buf,
					    uint32_t prev_flag_buf_mkey,
					    uint8_t *dst_buf,
					    uint32_t dst_buf_mkey,
					    uint64_t *dst_flag,
					    uint32_t dst_flag_mkey,
					    bool is_client);
#if __cplusplus
}
#endif

#endif /* GPUNETIO_VERBS_COMMON_H_ */
