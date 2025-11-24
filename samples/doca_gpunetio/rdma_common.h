/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef GPURDMA_COMMON_H_
#define GPURDMA_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_rdma.h>
#include <doca_gpunetio.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_error.h>
#include <doca_buf_array.h>

#define MAX_PCI_ADDRESS_LEN 32U
#define MAX_IP_ADDRESS_LEN 128
#define GPU_BUF_SIZE_A 256
#define GPU_BUF_SIZE_B 128
#define GPU_BUF_SIZE_C 128
#define GPU_BUF_SIZE_F sizeof(uint8_t)
#define GPU_BUF_NUM 4
#define GPU_NUM_OP_X_BUF 2
#define RDMA_SEND_QUEUE_SIZE 8192
#define RDMA_RECV_QUEUE_SIZE 8192
#define ROUND_UP(unaligned_mapping_size, align_val) ((unaligned_mapping_size) + (align_val)-1) & (~((align_val)-1))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define SERVER_ADDR_LEN (MAX(MAX(DOCA_DEVINFO_IPV4_ADDR_SIZE, DOCA_DEVINFO_IPV6_ADDR_SIZE), DOCA_GID_BYTE_LENGTH))
#define SERVER_ADDR_TYPE_LEN 6
#define DEFAULT_CM_PORT 13579

struct rdma_config {
	char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* DOCA device name */
	char gpu_pcie_addr[MAX_PCI_ADDRESS_LEN];	/* GPU PCIe address */
	char server_ip_addr[MAX_IP_ADDRESS_LEN];	/* DOCA device name */
	bool is_server;					/* Sample is acting as server or client */
	bool is_gid_index_set;				/* Is the set_index parameter passed */
	uint32_t gid_index;				/* GID index for DOCA RDMA */
	bool use_rdma_cm;				/* Use RDMA CM */
	uint32_t cm_port;				/* CM port */
	char cm_addr[SERVER_ADDR_LEN + 1];		/* RDMA_CM server IPv4/IPv6/GID address */
	enum doca_rdma_addr_type cm_addr_type;		/* RDMA_CM server address type, IPv4, IPv6 or GID,
							 * Only useful for client
							 **/
};

struct rdma_resources {
	struct rdma_config *cfg;	    /* RDMA samples configuration parameters */
	struct doca_dev *doca_device;	    /* DOCA device */
	struct doca_gpu *gpudev;	    /* DOCA GPU device */
	struct doca_rdma *rdma;		    /* DOCA RDMA instance */
	struct doca_gpu_dev_rdma *gpu_rdma; /* DOCA RDMA instance GPU handler */
	struct doca_ctx *rdma_ctx;	    /* DOCA context to be used with DOCA RDMA */
	struct doca_pe *pe;		    /* DOCA progress engine -- needed by server only */
	const void *connection_details;	    /* Remote peer connection details */
	size_t conn_det_len;		    /* Remote peer connection details data length */

	/* rdma_cm resources */
	struct doca_rdma_addr *cm_addr;		 /* CM server address to connect by a client */
	struct doca_rdma_connection *connection; /* The RDMA_CM connection instance */
	bool connection_established;		 /* Indicate whether connection is established */
	bool connection_error;			 /* Indicate connection error */
	bool server_listen_active;		 /* Indicate if server listen_to_port is active */

	struct doca_rdma_connection *connection2; /* The RDMA_CM connection instance */
	bool connection2_established;		  /* Indicate whether connection is established */
	bool connection2_error;			  /* Indicate connection error */
};

/* Sample rdma mmap object */
struct rdma_mmap_obj {
	struct doca_dev *doca_device; /* DOCA network device */
	uint32_t permissions;	      /* RDMA permission flags */
	void *memrange_addr;	      /* Memory mapped area address */
	size_t memrange_len;	      /* Memory mapped area size */
	struct doca_mmap *mmap;	      /* DOCA mmap obj */
	const void *rdma_export;      /* RDMA export object to share with remote peer */
	size_t export_len;	      /* RDMA export object size */
};

/* Sample buffer array object */
struct buf_arr_obj {
	struct doca_gpu *gpudev;	      /* DOCA GPU device */
	struct doca_mmap *mmap;		      /* DOCA mmap obj */
	uint32_t num_elem;		      /* Number of elements in buffer array */
	size_t elem_size;		      /* Size of each element in buffer array */
	struct doca_buf_arr *buf_arr;	      /* DOCA buffer array */
	struct doca_gpu_buf_arr *gpu_buf_arr; /* DOCA buffer array GPU obj */
};

/*
 * OOB connection to exchange RDMA info - server side
 *
 * @oob_sock_fd [out]: Socket FD
 * @oob_client_sock [out]: Client socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_connection_server_setup(int *oob_sock_fd, int *oob_client_sock);

/*
 * OOB connection to exchange RDMA info - server side closure
 *
 * @oob_sock_fd [in]: Socket FD
 * @oob_client_sock [in]: Client socket FD
 */
void oob_connection_server_close(int oob_sock_fd, int oob_client_sock);

/*
 * OOB connection to exchange RDMA info - client side
 *
 * @server_ip [in]: Server IP address to connect
 * @oob_sock_fd [out]: Socket FD
 * @return: positive integer on success and -1 otherwise
 */
int oob_connection_client_setup(const char *server_ip, int *oob_sock_fd);

/*
 * OOB connection to exchange RDMA info - client side closure
 *
 * @oob_sock_fd [in]: Socket FD
 */
void oob_connection_client_close(int oob_sock_fd);

/*
 * Wrapper to fix const type of doca_rdma_cap_task_write_is_supported
 *
 * @devinfo [in]: RDMA device info
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t wrapper_doca_rdma_cap_task_write_is_supported(struct doca_devinfo *devinfo);

/*
 * Create and initialize DOCA RDMA resources
 *
 * @cfg [in]: Configuration parameters
 * @rdma_permissions [in]: Access permission flags for DOCA RDMA
 * @resources [in/out]: DOCA RDMA resources to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_rdma_resources(struct rdma_config *cfg,
				   const uint32_t rdma_permissions,
				   struct rdma_resources *resources);

/*
 * Destroy DOCA RDMA resources
 *
 * @resources [in]: DOCA RDMA resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_rdma_resources(struct rdma_resources *resources);

/*
 * Create a DOCA mmap object
 *
 * @gpu_dev [in]: DOCA GPUNetIO handler
 * @mmap_obj [in]: mmap object to populate
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_mmap(struct doca_gpu *gpu_dev, struct rdma_mmap_obj *mmap_obj);

/*
 * Create a buffer array on GPU
 *
 * @buf_arr_obj [in]: buffer array object to populate
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_buf_arr_on_gpu(struct buf_arr_obj *buf_arr_obj);

/*
 * Server side of the RDMA write
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_write_server(struct rdma_config *cfg);

/*
 * Client side of the RDMA write
 *
 * @cfg [in]: Configuration parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t rdma_write_client(struct rdma_config *cfg);

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
doca_error_t kernel_write_client(cudaStream_t stream,
				 struct doca_gpu_dev_rdma *rdma_gpu,
				 struct doca_gpu_buf_arr *client_local_buf_arr_B,
				 struct doca_gpu_buf_arr *client_local_buf_arr_C,
				 struct doca_gpu_buf_arr *client_local_buf_arr_F,
				 struct doca_gpu_buf_arr *client_remote_buf_arr_A,
				 uint32_t connection_index,
				 uint32_t *exit_flag);

/*
 * Launch a CUDA kernel doing RDMA Write server
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @rdma_gpu [in]: RDMA GPU object
 * @server_local_buf_arr_A [in]: GPU buffer with local data A
 * @server_remote_buf_arr_F [in]: GPU buffer on remote server with data F
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t kernel_write_server(cudaStream_t stream,
				 struct doca_gpu_dev_rdma *rdma_gpu,
				 struct doca_gpu_buf_arr *server_local_buf_arr_A,
				 struct doca_gpu_buf_arr *server_remote_buf_arr_F,
				 uint32_t connection_index);

/*
 * Launch a CUDA kernel for RDMA Write Bandwidth benchmark
 *
 * @stream [in]: CUDA Stream to launch the kernel
 * @rdma_gpu [in]: RDMA GPU object
 * @num_iter [in]: Number of write iterations in the CUDA kernel
 * @num_cta [in]: Number CUDA kernel blocks
 * @num_threads_per_cta [in]: Number of CUDA threads per CUDA block
 * @msg_size [in]: Message size
 * @server_local_buf_arr [in]: GPU buffer with local data
 * @server_remote_buf_arr [in]: GPU buffer on remote server
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_rdma_write_bw(cudaStream_t stream,
				    struct doca_gpu_dev_rdma *rdma_gpu,
				    int num_iter,
				    int num_cta,
				    int num_threads_per_cta,
				    size_t msg_size,
				    struct doca_gpu_buf_arr *server_local_buf_arr,
				    struct doca_gpu_buf_arr *server_remote_buf_arr);

#if __cplusplus
}
#endif

#endif /* GPURDMA_COMMON_H_ */
