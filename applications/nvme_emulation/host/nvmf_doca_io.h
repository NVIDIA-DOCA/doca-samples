/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef NVMF_DOCA_IO_H_
#define NVMF_DOCA_IO_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>

#include <spdk/nvmf_transport.h>

#include <doca_error.h>
#include <doca_dpa.h>
#include <doca_dev.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_buf_pool.h>
#include <doca_dma.h>
#include <doca_devemu_pci.h>
#include <doca_comch_msgq.h>
#include <doca_comch_producer.h>
#include <doca_comch_consumer.h>

#define NVMF_DOCA_CQE_SIZE 16
#define NVMF_DOCA_SQE_SIZE 64

#define DMA_POOL_DATA_BUFFER_SIZE (1UL << 12)

struct nvmf_doca_cqe {
	uint8_t data[NVMF_DOCA_CQE_SIZE]; /**< The contents of the CQE */
};

struct nvmf_doca_sqe {
	uint8_t data[NVMF_DOCA_SQE_SIZE]; /**< The contents of the SQE */
};

struct nvmf_doca_dpa_msgq {
	struct doca_comch_msgq *msgq;	      /**< The DOCA Comch MsgQ */
	struct doca_comch_producer *producer; /**< The DOCA Comch Producer */
	struct doca_comch_consumer *consumer; /**< The DOCA Comch Consumer */
	bool is_send;			      /**< Indicates if MsgQ is used for sending from DPU to DPA */
};

struct nvmf_doca_dpa_comch {
	struct nvmf_doca_dpa_msgq send;			      /**< MsgQ used to send message from DPU to DPA */
	struct doca_dpa_completion *producer_comp;	      /**< The producer completion context used by DPA */
	struct nvmf_doca_dpa_msgq recv;			      /**< MsgQ used to receive message DPA */
	struct doca_comch_consumer_completion *consumer_comp; /**< The consumer completion context used by DPA */
};

struct nvmf_doca_queue {
	struct doca_buf_inventory *inventory;	/**< Buffer inventory for allocating queue elements */
	struct doca_dma *dma;			/**< DMA context for copying data to/from Host */
	struct doca_mmap *local_queue_mmap;	/**< An mmap representing local memory where elements will be copied */
	void *local_queue_address;		/**< Local queue for copying elements to/from Host */
	struct doca_dma_task_memcpy **elements; /**< Element at given index represents a copy/write task for queue
					    element at same index */
	uint32_t num_elements;			/**< The max number of elements in the queue */
};

struct nvmf_doca_cq {
	uint32_t cq_id;		       /**< The ID of the CQ*/
	struct nvmf_doca_queue queue;  /**< Queue used for writing local CQEs to Host */
	struct doca_devemu_pci_db *db; /**< The DB associated with the CQ */
	struct nvmf_doca_io *io;       /**< Reference to the IO that contains this CQ */
	uint32_t ci;		       /**< The consumer index as provided by Host */
	uint32_t pi;		       /**< The producer index managed by the DPU */
};

typedef void (*nvmf_doca_cq_post_cqe_cb)(struct nvmf_doca_cq *cq, union doca_data user_data);

struct nvmf_doca_dma_pool {
	void *local_data_memory;			/**< Memory allocated for local data buffers */
	struct doca_mmap *local_data_mmap;		/**< The mmap for the local data buffers */
	struct doca_buf_pool *local_data_pool;		/**< Pool of local data buffers */
	struct doca_mmap *host_data_mmap;		/**< mmap granting access to Host data buffers */
	struct doca_buf_inventory *host_data_inventory; /**< Inventory for allocating Host data buffers */
	struct doca_dma *dma;				/**< DMA context used for copying data between Host and DPU */
};

struct nvmf_doca_io;

struct nvmf_doca_request;

typedef void (*nvmf_doca_req_cb)(struct nvmf_doca_request *doca_req, void *cb_arg);

struct nvmf_doca_request {
	struct spdk_nvmf_request request;		    /**< The SPDK NVMf request */
	struct nvmf_doca_sq *doca_sq;			    /**< The SQ handling the request */
	struct spdk_nvme_cpl cq_entry;			    /**< Completion queue entry */
	struct spdk_nvme_cmd command;			    /**< The NVMe command */
	struct doca_buf *dpu_buffer[NVMF_REQ_MAX_BUFFERS];  /**< Array of pointers to DPU data buffers */
	struct doca_buf *host_buffer[NVMF_REQ_MAX_BUFFERS]; /**< Array of pointers to host data buffers */
	struct doca_buf *prp_host_buf;
	struct doca_buf *prp_dpu_buf;
	uint32_t num_of_buffers;	     /**< Counter for the number of buffers full so far */
	uint32_t residual_length;	     /**< The remainder of the NVMe request for write or read operations */
	uint16_t sqe_idx;		     /**< The SQE index of this request*/
	bool data_from_alloc;		     /**< Indicates if spdk_nvmf_request::data is from allocation */
	nvmf_doca_req_cb doca_cb;	     /**< Doca request call back */
	void *cb_arg;			     /**< Doca request call back arguments */
	TAILQ_ENTRY(nvmf_doca_request) link; /**< Link to next doca request */
};

enum nvmf_doca_sq_state {
	NVMF_DOCA_SQ_STATE_INITIAL,
	NVMF_DOCA_SQ_STATE_BIND_DB_REQUESTED,
	NVMF_DOCA_SQ_STATE_BIND_DB_DONE,
	NVMF_DOCA_SQ_STATE_READY,
	NVMF_DOCA_SQ_STATE_UNBIND_DB_REQUESTED,
	NVMF_DOCA_SQ_STATE_UNBIND_DB_DONE,
};

enum nvmf_doca_sq_db_state {
	NVMF_DOCA_SQ_DB_UNBOUND,
	NVMF_DOCA_SQ_DB_BIND_REQUESTED,
	NVMF_DOCA_SQ_DB_BOUND,
	NVMF_DOCA_SQ_DB_UNBIND_REQUESTED,
};

typedef void (*nvmf_doca_sq_stop_cb)(struct nvmf_doca_sq *sq);

struct nvmf_doca_sq {
	struct spdk_nvmf_qpair spdk_qp;		       /**< The NVMf Target QPair */
	struct nvmf_doca_queue queue;		       /**< Queue used for reading SQEs from Host */
	struct nvmf_doca_dma_pool dma_pool;	       /**< Pool of DMA data copy operations */
	struct doca_devemu_pci_db *db;		       /**< The DB associated with the SQ */
	doca_dpa_dev_devemu_pci_db_t db_handle;	       /**< DPA handle of the DB */
	struct nvmf_doca_io *io;		       /**< Reference to the IO that contains this SQ */
	uint32_t pi;				       /**< The producer index as provided by Host */
	uint32_t sq_id;				       /**< The ID of the SQ */
	enum nvmf_doca_sq_state state;		       /**< The state of the SQ */
	void *ctx;				       /**< Opaque structure that can be set by user */
	enum nvmf_doca_sq_db_state db_state;	       /**< The state of the SQ DB */
	doca_error_t result;			       /**< Stored error in case add operation fails midway */
	struct nvmf_doca_request *request_pool_memory; /**< Pointer to NVMF doca request pool memory */
	TAILQ_HEAD(, nvmf_doca_request) request_pool;  /**< List of the NVMF doca requests */
	TAILQ_ENTRY(nvmf_doca_sq) link;		       /**< Pointer to next SQ in list */
	TAILQ_ENTRY(nvmf_doca_sq) pci_dev_admin_link;  /**< Pointer to next SQ in list */
};

typedef void (*nvmf_doca_sq_fetch_sqe_cb)(struct nvmf_doca_sq *sq, struct nvmf_doca_sqe *sqe, uint16_t sqe_idx);
typedef void (*nvmf_doca_sq_copy_data_cb)(struct nvmf_doca_sq *sq,
					  struct doca_buf *dst,
					  struct doca_buf *src,
					  union doca_data user_data);

struct nvmf_doca_dpa_thread {
	struct doca_dpa *dpa;		/**< DOCA DPA */
	struct doca_dpa_thread *thread; /**< DPA thread */
	doca_dpa_dev_uintptr_t arg;	/**< Argument to be used by the DPA thread (struct io_thread_arg) */
};
typedef void (*nvmf_doca_io_stop_cb)(struct nvmf_doca_io *io);

struct nvmf_doca_pci_dev_admin;

struct nvmf_doca_io {
	struct nvmf_doca_pci_dev_poll_group *poll_group; /**< Doca poll group this IO belongs to */
	struct nvmf_doca_pci_dev_admin *pci_dev_admin;	 /**< The PCI device admin context */
	struct nvmf_doca_dpa_thread dpa_thread;		 /**< DPA thread used for receiving DBs */
	struct nvmf_doca_dpa_comch comch;		 /**< Full-Duplex Communication channel with DPA thread */
	struct nvmf_doca_cq cq;				 /**< CQ for posting completions to Host */
	struct doca_devemu_pci_db_completion *db_comp;	 /**< DB completion to be polled by DPA thread */
	struct doca_devemu_pci_msix *msix;		 /**< MSI-X to be raised by DPA thread */
	nvmf_doca_cq_post_cqe_cb post_cqe_cb;		 /**< Callback invoked once a CQE is posted to Host */
	nvmf_doca_sq_fetch_sqe_cb fetch_sqe_cb;		 /**< Callback invoked once a SQE is fetched from host */
	nvmf_doca_sq_copy_data_cb copy_data_cb;		 /**< Callback invoked once data copy operation completes */
	nvmf_doca_sq_stop_cb stop_sq_cb;		 /**< Callback invoked once an SQ has been stopped */
	nvmf_doca_io_stop_cb stop_io_cb;		 /**< Callback invoked once an IO has been stopped */
	void *ctx;					 /**< Opaque structure that can be set by user */
	TAILQ_HEAD(, nvmf_doca_sq) sq_list;		 /**< List of the added SQs */
	TAILQ_ENTRY(nvmf_doca_io) pci_dev_admin_link;	 /**< Link to next doca io, used by PCI device NVMf context */
	TAILQ_ENTRY(nvmf_doca_io) pci_dev_pg_link;	 /**< Link to next doca io used by PCI device poll group */
};

struct nvmf_doca_io_create_attr {
	struct doca_pe *pe;		      /**< Progress engine to be used for any created contexts */
	struct doca_dev *dev;		      /**< A doca device representing the emulation manager */
	struct doca_devemu_pci_dev *nvme_dev; /**< The emulated NVMe device */
	struct doca_dpa *dpa;		      /**< DOCA DPA for accessing DPA resources */
	uint32_t cq_id;			      /**< The NVMe CQ ID that is associated with this IO */
	uint16_t cq_depth;		      /**< The size of the completeion queue */
	struct doca_mmap *host_cq_mmap;	      /**< mmap granting access to the Host CQ memory */
	uintptr_t host_cq_address;	      /**< I/O address of the CQ on the Host */
	bool enable_msix;		      /**< Whether CQ should raise MSI-X towards the Host after posting a CQE */
	uint32_t msix_idx;   /**< The MSI-X vector index to raise. Relevant only in case enable_msix=true */
	uint32_t max_num_sq; /**< The maximum number of SQs that can be associated with the CQ */
	nvmf_doca_cq_post_cqe_cb post_cqe_cb;	/**< Callback invoked once a CQE is posted to Host */
	nvmf_doca_sq_fetch_sqe_cb fetch_sqe_cb; /**< Callback invoked once a SQE is fetched from host */
	nvmf_doca_sq_copy_data_cb copy_data_cb; /**< Callback invoked once data copy operation completes */
	nvmf_doca_sq_stop_cb stop_sq_cb;	/**< Callback invoked once an SQ has been stopped */
	nvmf_doca_io_stop_cb stop_io_cb;	/**< Callback invoked once an IO has been stopped */
};

/*
 * Create NVMf DOCA IO
 *
 * Creates an IO, which can be used to receive DBs on CQ and associated SQs, read SQEs from Host, write CQEs to Host
 * and raise MSI-X.
 *
 * @attr [in]: The IO create attributes
 * @io [in]: The IO to be initialized
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t nvmf_doca_io_create(const struct nvmf_doca_io_create_attr *attr, struct nvmf_doca_io *io);

/*
 * Destroy NVMf DOCA IO
 *
 * @io [in]: The IO to destroy
 */
void nvmf_doca_io_destroy(struct nvmf_doca_io *io);

/*
 * Stop the NVMf DOCA IO canceling all in-flight requests
 *
 * Must be done before destroying the IO. Must remove all SQs prior to this operation
 * This operation is async, once operation completes then nvmf_doca_io::stop_io_cb will be invoked
 * Once complete then it becomes possible to destroy the IO using nvmf_doca_io_destroy()
 *
 * @io [in]: The IO to stop
 */
void nvmf_doca_io_stop(struct nvmf_doca_io *io);

struct nvmf_doca_io_add_sq_attr {
	struct doca_pe *pe;		       /**< Progress engine to be used by DMA context */
	struct doca_dev *dev;		       /**< A doca device representing the emulation manager */
	struct doca_devemu_pci_dev *nvme_dev;  /**< The emulated NVMe device used for creating the CQ DB */
	uint16_t sq_depth;		       /**< The size of the submission queue */
	struct doca_mmap *host_sq_mmap;	       /**< An mmap granting access to the Host CQ memory */
	uintptr_t host_sq_address;	       /**< I/O address of the CQ on the Host */
	uint32_t sq_id;			       /**< The NVMe CQ ID that is associated with this IO */
	struct spdk_nvmf_transport *transport; /**< The doca transport includes this IO */
	void *ctx;			       /**< Opaque structure that can be set by user */
};

/*
 * Add SQ to NVMf DOCA IO
 *
 * This operation is async, once operation completes then nvmf_doca_poll_group_add() will be invoked
 *
 * @io [in]: The IO to add the SQ to
 * @attr [in]: The SQ attributes
 * @sq [in]: The SQ to initialize
 */
void nvmf_doca_io_add_sq(struct nvmf_doca_io *io, const struct nvmf_doca_io_add_sq_attr *attr, struct nvmf_doca_sq *sq);

/*
 * Stop the NVMf DOCA SQ
 *
 * This operation is async, once operation completes then nvmf_doca_io::stop_sq_cb will be invoked
 * Once complete then it becomes possible to remove the SQ using nvmf_doca_io_rm_sq()
 *
 * @sq [in]: The SQ to stop
 */
void nvmf_doca_sq_stop(struct nvmf_doca_sq *sq);

/*
 * Remove SQ from NVMf DOCA IO
 *
 * Can only be done after nvmf_doca_sq_stop() completes
 *
 * @sq [in]: The SQ to remove
 */
void nvmf_doca_io_rm_sq(struct nvmf_doca_sq *sq);

/*
 * Post a CQE to the Host CQ
 *
 * This operation is async, once operation completes then nvmf_doca_io::post_cqe_cb will be invoked
 *
 * @io [in]: The IO that received the original SQE
 * @cqe [in]: Contents of the CQE
 * @user_data [in]: User data to associate with the operation, same data will be available on completion
 */
void nvmf_doca_io_post_cqe(struct nvmf_doca_io *io, const struct nvmf_doca_cqe *cqe, union doca_data user_data);

/*
 * Get buffer containing DPU memory, can be used to copy data between Host and DPU
 *
 * Buffer must be freed by caller using doca_buf_dec_refcount()
 *
 * @sq [in]: The SQ to be used for the copy operation
 * @return: Empty buffer to be used for copy operation
 */
struct doca_buf *nvmf_doca_sq_get_dpu_buffer(struct nvmf_doca_sq *sq);

/*
 * Get buffer pointing to Host memory, can be used to copy data between Host and DPU
 *
 * Buffer must be freed by caller using doca_buf_dec_refcount()
 *
 * @sq [in]: The SQ to be used for the copy operation
 * @host_io_address [in]: I/O address of Host buffer
 * @return: Buffer pointing to the given Host I/O address
 */
struct doca_buf *nvmf_doca_sq_get_host_buffer(struct nvmf_doca_sq *sq, uintptr_t host_io_address);

/*
 * Copy data between Host and DPU
 *
 * This operation is async, once operation completes then nvmf_doca_io::copy_data_cb will be invoked
 *
 * @sq [in]: The SQ used for the copy operation
 * @dst_buffer [in]: The destination buffer
 * @src_buffer [in]: The source buffer
 * @length [in]: The copy operation length
 * @user_data [in]: User data to associate with the operation, same data will be available on completion of the copy
 */
void nvmf_doca_sq_copy_data(struct nvmf_doca_sq *sq,
			    struct doca_buf *dst_buffer,
			    struct doca_buf *src_buffer,
			    size_t length,
			    union doca_data user_data);

/*
 * Get a request to be used with SPDK QP
 *
 * @sq [in]: The NVMf DOCA SQ containing the SPDK QP
 * @return: The request
 */
struct nvmf_doca_request *nvmf_doca_request_get(struct nvmf_doca_sq *sq);

/*
 * Complete the request and free it
 *
 * @request [in]: The request
 */
void nvmf_doca_request_complete(struct nvmf_doca_request *request);

/*
 * Free the request without completing it
 *
 * @request [in]: The request
 */
void nvmf_doca_request_free(struct nvmf_doca_request *request);

#endif // NVMF_DOCA_IO_H_
