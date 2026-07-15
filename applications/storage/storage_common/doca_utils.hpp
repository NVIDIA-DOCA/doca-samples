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

#ifndef APPLICATIONS_STORAGE_STORAGE_COMMON_DOCA_UTILS_HPP_
#define APPLICATIONS_STORAGE_STORAGE_COMMON_DOCA_UTILS_HPP_

#include <cstddef>
#include <chrono>
#include <string>
#include <vector>

#include <doca_argp.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

namespace storage {

enum class thread_safety {
	yes,
	no,
};

/*
 * Helper to keep a RDMA connection and its associated context together as pairs
 */
struct rdma_conn_pair {
	doca_rdma *rdma = nullptr;	      /* RDMA context */
	doca_rdma_connection *conn = nullptr; /* RDMA connection */
};

/*
 * Find and open a device with the given identifier. May match any one of:
 *  - InfiniBand device name(eg: mlx5_0)
 *  - Network interface name(eg: ens1f0np0)
 *  - PCI address(eg: 03:00.0)
 *
 * @throws std::runtime_error: If no device matches the given name / identifier OR if opening the device failed
 *
 * @identifier [in]: Identifier to use when selecting a device to use
 * @return: The found and opened device
 */
doca_dev *open_device(std::string const &identifier);

/*
 * Find and open a device representor with the given identifier. May match any one of:
 *  - PCI address(eg: 03:00.0)
 *
 * @throws std::runtime_error: If no device representor matches the given identifier OR if opening the device
 * representor failed
 *
 * @dev [in]: Device to be represented. Must point to a valid open device
 * @identifier [in]: Identifier to use when selecting a device representor to use
 * @return: The found and opened device representor
 */
doca_dev_rep *open_representor(doca_dev *dev, std::string const &identifier);

/*
 * Create, initialize and start a mmap to make the provided memory available to be used in doca_buf objects using the
 * provided device
 *
 * @throws std::runtime_error: If allocation, initialization or starting of the mmap fails
 *
 * @dev [in]: Device which the memory will be used with
 * @memory_region [in]: Start of the memory region to use
 * @memory_region_size[in]: Size (in bytes) of the memory region
 * @permissions [in]: Bit masked set of permissions to apply to the mmap. See enum doca_access_flag
 * @return: The newly created, initialized and started mmap
 */
doca_mmap *make_mmap(doca_dev *dev,
		     char *memory_region,
		     size_t memory_region_size,
		     uint32_t permissions,
		     storage::thread_safety ts);

/*
 * Create, initialize and start a mmap from a remote export. See doca_mmap_export_pci and doca_mmap_export_rdma
 *
 * @throws std::runtime_error :If allocation, initialization or starting of the mmap fails
 *
 * @dev [in]: Device which the memory will be used with
 * @mmap_export_blob [in]: The remote export blob buffer
 * @mmap_export_blob_size[in]: Size (in bytes) of the remote export blob buffer
 * @return: The newly created, initialized and started mmap
 */
doca_mmap *make_mmap(doca_dev *dev,
		     void const *mmap_export_blob,
		     size_t mmap_export_blob_size,
		     storage::thread_safety ts);

/*
 * Create, initialize and start a buffer inventory
 *
 * @throws std::runtime_error: If allocation, initialization or starting of the inventory fails
 *
 * @num_elements [in]: Number of doca_bufs this inventory can provide to the user
 * @return: The newly created, initialized and started inventory
 */
doca_buf_inventory *make_buf_inventory(size_t num_elements);

/*
 * Create, initialize and start a comch consumer. Ready to connect to a remote producer
 *
 * @throws std::runtime_error: If allocation, initialization or starting of the consumer fails
 *
 * @conn [in]: Established comch control path connection to use
 * @mmap [in]: Mmap from which the buffers submitted with receive tasks will be allocated from
 * @pe [in]: The progress engine that will be used to schedule work execution for the consumer
 * @task_pool_size [in]: Maximum number of consumer receive tasks that can be allocated
 * @ctx_user_data [in]: Data that will be used as the context user data for each invocation of this consumers task
 * callbacks
 * @task_cb [in]: The callback to be invoked when this consumer has received a message
 * @error_cb [in]: The callback to be invoked when a doca_comch_consumer_task_post_recv task fails
 * @return: The newly created, initialized and started consumer
 */
doca_comch_consumer *make_comch_consumer(doca_comch_connection *conn,
					 doca_mmap *mmap,
					 doca_pe *pe,
					 uint32_t task_pool_size,
					 doca_data ctx_user_data,
					 doca_comch_consumer_task_post_recv_completion_cb_t task_cb,
					 doca_comch_consumer_task_post_recv_completion_cb_t error_cb);

/*
 * Create, initialize and start a comch producer. Ready to connect to a remote consumer
 *
 * @throws std::runtime_error: If allocation, initialization or starting of the producer fails
 *
 * @conn [in]: Established comch control path connection to use
 * @pe [in]: The progress engine that will be used to schedule work execution for the producer
 * @task_pool_size [in]: Maximum number of producer send tasks that can be allocated
 * @ctx_user_data [in]: Data that will be used as the context user data for each invocation of this producers task
 * callbacks
 * @task_cb [in]: The callback to be invoked when this producer has sent a message
 * @error_cb [in]: The callback to be invoked when a doca_comch_producer_task_send task fails
 * @return: The newly created, initialized and started producer
 */
doca_comch_producer *make_comch_producer(doca_comch_connection *conn,
					 doca_pe *pe,
					 uint32_t task_pool_size,
					 doca_data ctx_user_data,
					 doca_comch_producer_task_send_completion_cb_t task_cb,
					 doca_comch_producer_task_send_completion_cb_t error_cb);

/*
 * Create and partially initialize an RDMA context. The user will still need to:
 *  - Configure task pools for the RDMA task types they want to use
 *  - Start the context
 *  - Export the connection details and provide these to the remote side
 *  - Retrieve the remote sides RDMA connection details
 *  - Connect using the provided connection details
 *  - Progress the progress engine until the RDMA context is ready to send / receive tasks
 *
 * @throws std::runtime_error: If allocation or partially initialization of the RDMA context fails
 *
 * @dev [in]: Device to use
 * @pe [in]: The progress engine that will be used to schedule work execution for the RDMA context
 * @ctx_user_data [in]: Data that will be used as the context user data for each invocation of this producers task
 * callbacks
 * @permissions [in]: Bitwise combination of RDMA access flags - see enum doca_access_flag
 * @return: The newly created and partially initialized RDMA context
 */
doca_rdma *make_rdma_context(doca_dev *dev, doca_pe *pe, doca_data ctx_user_data, uint32_t permissions);

/*
 * Check if a context is running / started
 *
 * @ctx [in]: Context to query
 * @return: true if the context is running, false otherwise
 */
bool is_ctx_running(doca_ctx *ctx) noexcept;

/*
 * Stop the context
 *
 * @ctx [in]: The context to stop
 * @pe [in]: The progress engine associated with the context
 * @return: DOCA_SUCCESS or error code upon failure
 */
doca_error_t stop_context(doca_ctx *ctx, doca_pe *pe) noexcept;

/*
 * Stop the context and release tasks
 *
 * @ctx [in]: The context to stop
 * @pe [in]: The progress engine associated with the context
 * @ctx_tasks [in]: The set of tasks that are owned by the given context so they can be freed after they are flushed by
 * the context during the stop process
 * @return: DOCA_SUCCESS or error code upon failure
 */
doca_error_t stop_context(doca_ctx *ctx, doca_pe *pe, std::vector<doca_task *> &ctx_tasks) noexcept;

/*
 * Strongly typed boolean to make usage clearer
 */
struct value_requirement {
	bool is_required; /* true if the value is mandatory, false if it is optional */
};

/*
 * Strongly typed boolean to make usage clearer
 */
struct value_multiplicity {
	bool support_multiple_values; /* true if a value can be specified multiple times, false when it can be specified
				       * at most once
				       */
};

static value_requirement constexpr required_value{true};  /* helper alias */
static value_requirement constexpr optional_value{false}; /* helper alias */

static value_multiplicity constexpr single_value{false};   /* helper alias */
static value_multiplicity constexpr multiple_values{true}; /* helper alias */

/*
 * Register a parameter with doca_argp
 *
 * @throws std::runtime_error: If something goes wrong
 *
 * @type [in]: value category
 * @short_name [in]: The short name for the value, or nullptr if not supported
 * @long_name [in]: The long name for the value, or nullptr if not supported
 * @description [in]: Description of the value
 * @requirement [in]: Is the value required
 * @multiplicity [in]: Value multiplicity
 * @callback [in]: Parameter value handler
 */
void register_cli_argument(doca_argp_type type,
			   char const *short_name,
			   char const *long_name,
			   char const *description,
			   value_requirement requirement,
			   value_multiplicity multiplicity,
			   doca_argp_param_cb_t callback);

/*
 * Helper to get a pointer to the contents of a doca buf
 *
 * @buf [in]: Pointer to doca_buf whose underlying bytes are desired
 * @return: Pointer to buffer bytes
 */
inline char *get_buffer_bytes(doca_buf *buf) noexcept
{
	void *data;
	static_cast<void>(doca_buf_get_data(buf, &data));
	return static_cast<char *>(data);
}

/*
 * Helper to get a pointer to the contents of a doca buf
 *
 * @buf [in]: Pointer to doca_buf whose underlying bytes are desired
 * @return: Pointer to buffer bytes
 */
inline char const *get_buffer_bytes(doca_buf const *buf) noexcept
{
	void *data;
	static_cast<void>(doca_buf_get_data(const_cast<doca_buf *>(buf), &data));
	return static_cast<char const *>(data);
}

/*
 * Create a logger backend. This only needs called one per application
 */
void create_doca_logger_backend(void) noexcept;

/*
 * String to enum conversion
 *
 * @matrix_type [in]: String representation
 * @return enum or throw if unknown
 */
doca_ec_matrix_type matrix_type_from_string(std::string const &matrix_type);

/*
 * Provides a holder for doca bufs which can be used to hold a doca buf until it's placed into another container so that
 * an allocated doca buf will not leak during task creation
 */
class doca_buf_raii_wrapper {
public:
	~doca_buf_raii_wrapper()
	{
		if (m_buf) {
			doca_buf_dec_refcount(m_buf, nullptr);
		}
	}

	doca_buf_raii_wrapper() = default;
	doca_buf_raii_wrapper(doca_buf_raii_wrapper const &) = delete;
	doca_buf_raii_wrapper(doca_buf_raii_wrapper &&) noexcept = delete;
	doca_buf_raii_wrapper &operator=(doca_buf_raii_wrapper const &) = delete;
	doca_buf_raii_wrapper &operator=(doca_buf_raii_wrapper &&) noexcept = delete;

	/* Allows the doca_buf_raii_wrapper to be populated from the out param of doca buf alocation routines */
	doca_buf **operator&()
	{
		return &m_buf;
	}

	/* Take the held doca_buf (if any) and release its ownership to the caller for them to destroy it later */
	doca_buf *release()
	{
		doca_buf *tmp{m_buf};
		m_buf = nullptr;
		return tmp;
	}

private:
	doca_buf *m_buf = nullptr;
};

} /* namespace storage */

#endif /* APPLICATIONS_STORAGE_STORAGE_COMMON_DOCA_UTILS_HPP_ */