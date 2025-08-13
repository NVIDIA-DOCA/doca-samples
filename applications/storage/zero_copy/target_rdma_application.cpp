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

#include "target_rdma_application.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <storage_common/aligned_new.hpp>
#include <storage_common/buffer_utils.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/doca_utils.hpp>
#include <storage_common/os_utils.hpp>
#include <storage_common/tcp_socket.hpp>

#include <zero_copy/control_message.hpp>
#include <zero_copy/io_message.hpp>

DOCA_LOG_REGISTER(APPLICATION);

using namespace std::string_literals;

namespace storage::zero_copy {
namespace {

struct alignas(storage::cache_line_size / 2) transfer_context {
	doca_rdma_task_write *write_task = nullptr;
	doca_rdma_task_read *read_task = nullptr;
	doca_buf *host_buf = nullptr;
	doca_buf *storage_buf = nullptr;
};

static_assert(sizeof(transfer_context) == storage::cache_line_size / 2,
	      "Expected transfer_context to occupy half a cache line");

class target_rdma_application_impl;

struct alignas(storage::cache_line_size) thread_hot_data {
	uint64_t pe_hit_count = 0;
	uint64_t pe_miss_count = 0;
	char *host_memory_start_addr = nullptr;
	char *storage_memory_start_addr = nullptr;
	transfer_context *transfer_contexts = nullptr;
	target_rdma_application_impl *app_impl = nullptr;
	uint32_t id = 0;
	uint32_t transfer_contexts_size = 0;
	uint32_t in_flight_ops_count = 0;
	std::atomic_bool wait_flag = true;
	std::atomic_bool running_flag = true;
	bool encountered_errors = false;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(std::string const &reason);
};

static_assert(sizeof(thread_hot_data) == storage::cache_line_size, "Expected thread_hot_data to occupy one cache line");

struct thread_context {
	doca_dev *dev;
	uint32_t thread_id;
	uint32_t runtime_task_count;
	uint32_t runtime_buffer_size;
	doca_pe *pe;
	thread_hot_data *hot_data;
	char *io_messages_memory;
	char *storage_memory;
	doca_mmap *io_message_mmap;
	doca_mmap *storage_mmap;
	doca_mmap *host_mmap;
	doca_buf_inventory *buf_inv;
	storage::rdma_conn_pair ctrl_rdma;
	storage::rdma_conn_pair data_rdma;
	std::vector<doca_buf *> bufs;
	std::vector<doca_task *> ctrl_tasks;
	std::vector<doca_task *> data_tasks;
	std::thread thread;

	/*
	 * Destructor
	 */
	~thread_context();

	/*
	 * Deleted default constructor
	 */
	thread_context() = delete;

	/*
	 * Constructor
	 *
	 * @app_impl_ [in]: Pointer to parent impl
	 * @dev_ [in]: Pointer to dev
	 * @thread_id_ [in]: Logical id of this thread
	 * @runtime_task_count_ [in]: Number of task sets this thread should have
	 * @runtime_buffer_size_ [in]: Size of an io transfer
	 */
	thread_context(target_rdma_application_impl *app_impl_,
		       doca_dev *dev_,
		       uint32_t thread_id_,
		       uint32_t runtime_task_count_,
		       uint32_t runtime_buffer_size_);

	/*
	 * Deleted copy constructor
	 */
	thread_context(thread_context const &) = delete;

	/*
	 * Deleted move constructor
	 */
	thread_context(thread_context &&) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	thread_context &operator=(thread_context const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	thread_context &operator=(thread_context &&) noexcept = delete;

	/*
	 * Handle a configure storage request
	 *
	 * @mmap_export [in] Remote mmap definition to allow access to end user memory
	 */
	void configure_storage(std::vector<uint8_t> const &mmap_export);

	/*
	 * Create a rdma connection
	 *
	 * @request [in]: Request details
	 * @return: rdma export blob to send back to the initiator
	 */
	std::vector<uint8_t> create_rdma_connection(
		storage::zero_copy::control_message::create_rdma_connection_request const &request);

	/*
	 * Create and submit tasks
	 */
	void create_and_submit_tasks(void);

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(const std::string &reason);

	/*
	 * Join the held thread
	 */
	void join(void);
};

class target_rdma_application_impl : public ::storage::zero_copy::target_rdma_application {
public:
	/*
	 * Default destructor
	 */
	~target_rdma_application_impl() override;

	/*
	 * Deleted default constructor
	 */
	target_rdma_application_impl() = delete;

	/*
	 * Constructor
	 *
	 * @cfg [in]: Application configuration
	 */
	explicit target_rdma_application_impl(storage::zero_copy::target_rdma_application::configuration const &cfg);

	/*
	 * Deleted copy constructor
	 */
	target_rdma_application_impl(target_rdma_application_impl const &) = delete;

	/*
	 * Deleted move constructor
	 */
	target_rdma_application_impl(target_rdma_application_impl &&) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	target_rdma_application_impl &operator=(target_rdma_application_impl const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	target_rdma_application_impl &operator=(target_rdma_application_impl &&) noexcept = delete;

	/*
	 * Run the application
	 */
	void run(void) override;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(std::string const &reason) override;

	/*
	 * Get the final statistics
	 *
	 * @return: The statistics
	 */
	std::vector<storage::zero_copy::target_rdma_application::thread_stats> get_stats(void) const override;

	/*
	 * Stop all running data path threads
	 */
	void stop_all_threads(void);

private:
	configuration m_cfg;
	doca_dev *m_dev;
	storage::tcp_socket m_listen_socket;
	storage::tcp_socket m_client_connection;
	std::array<char, control_message_buffer_size> m_tcp_rx_buffer;
	storage::zero_copy::control_message_reassembler m_control_message_reassembler;
	std::vector<storage::zero_copy::control_message> m_ctrl_msgs;
	std::vector<std::shared_ptr<thread_context>> m_thread_contexts;
	std::vector<storage::zero_copy::target_rdma_application::thread_stats> m_stats;
	bool m_abort_flag;

	/*
	 * Wait (and poll) for a control message to arrive
	 *
	 * @return: The received control message
	 */
	storage::zero_copy::control_message wait_for_control_message(void);

	/*
	 * Send a control message
	 *
	 * @message [in]: The message to send
	 */
	void send_control_message(storage::zero_copy::control_message message);

	/*
	 * Start listening for TCP connections
	 */
	void start_listening(void);

	/*
	 * Wait for a TCP client to connect
	 */
	void wait_for_tcp_client(void);

	/*
	 * Process configure storage request
	 *
	 * @request [in]: Request
	 */
	void configure_storage(storage::zero_copy::control_message::configure_data_path const &request);

	/*
	 * Establish rdma connections
	 */
	void establish_rdma_connections(void);

	/*
	 * Destroy objects
	 */
	void destroy_objects(void);
};

/*
 * Get the start address of an mmap
 *
 * @mmap [in]: mmap to query
 * @out_mmap_size [out]: Length of the mmap region
 * @return: Pointer to the start of the mmap region
 */
char *get_mmap_memrange_start(doca_mmap *mmap, size_t &out_mmap_size)
{
	char *mmap_start_addr = nullptr;
	size_t mmap_size = 0;
	auto const ret = doca_mmap_get_memrange(mmap, reinterpret_cast<void **>(&mmap_start_addr), &mmap_size);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to query remote mmap memory range: "s + doca_error_get_name(ret)};
	}

	out_mmap_size = mmap_size;

	return mmap_start_addr;
}

/*
 * RDMA task receive callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_rdma_task_receive_cb(doca_rdma_task_receive *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	doca_error_t ret;
	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	auto *const io_message = storage::get_buffer_bytes(doca_rdma_task_receive_get_dst_buf(task));
	auto const message_type = io_message_view::get_type(io_message);

	auto *const transfer_ctx = static_cast<transfer_context *>(task_user_data.ptr);

	switch (message_type) {
	case io_message_type::read: {
		size_t const offset = reinterpret_cast<char *>(io_message_view::get_io_address(io_message)) -
				      hot_data->host_memory_start_addr;

		char *const host_addr = hot_data->host_memory_start_addr + offset;
		char *const storage_addr = hot_data->storage_memory_start_addr + offset;
		uint32_t const transfer_size = io_message_view::get_io_size(io_message);

		DOCA_LOG_TRC("Read %u bytes from storage: %p to host: %p", transfer_size, storage_addr, host_addr);

		ret = doca_buf_set_data(transfer_ctx->host_buf, host_addr, 0);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set transfer host memory range: %s", doca_error_get_name(ret));
			break;
		}

		ret = doca_buf_set_data(transfer_ctx->storage_buf, storage_addr, transfer_size);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set transfer storage memory range: %s", doca_error_get_name(ret));
			break;
		}
		doca_rdma_task_write_set_dst_buf(transfer_ctx->write_task, transfer_ctx->host_buf);
		doca_rdma_task_write_set_src_buf(transfer_ctx->write_task, transfer_ctx->storage_buf);

		ret = doca_task_submit(doca_rdma_task_write_as_task(transfer_ctx->write_task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit doca_rdma_task_write: %s", doca_error_get_name(ret));
			break;
		}

		++(hot_data->in_flight_ops_count);
	} break;
	case io_message_type::write: {
		size_t const offset = reinterpret_cast<char *>(io_message_view::get_io_address(io_message)) -
				      hot_data->host_memory_start_addr;

		char *const host_addr = hot_data->host_memory_start_addr + offset;
		char *const storage_addr = hot_data->storage_memory_start_addr + offset;
		uint32_t const transfer_size = io_message_view::get_io_size(io_message);

		DOCA_LOG_TRC("Write %u bytes from host: %p to storage: %p ", transfer_size, host_addr, storage_addr);

		ret = doca_buf_set_data(transfer_ctx->host_buf, host_addr, transfer_size);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set transfer host memory range: %s", doca_error_get_name(ret));
			break;
		}

		ret = doca_buf_set_data(transfer_ctx->storage_buf, storage_addr, 0);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set transfer storage memory range: %s", doca_error_get_name(ret));
			break;
		}
		doca_rdma_task_read_set_dst_buf(transfer_ctx->read_task, transfer_ctx->storage_buf);
		doca_rdma_task_read_set_src_buf(transfer_ctx->read_task, transfer_ctx->host_buf);

		ret = doca_task_submit(doca_rdma_task_read_as_task(transfer_ctx->read_task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit doca_rdma_task_read: %s", doca_error_get_name(ret));
			break;
		}

		++(hot_data->in_flight_ops_count);
	} break;
	case io_message_type::stop: {
		++(hot_data->in_flight_ops_count);
		hot_data->app_impl->stop_all_threads();

		auto *const response_task = static_cast<doca_rdma_task_send *>(
			doca_task_get_user_data(doca_rdma_task_write_as_task(transfer_ctx->write_task)).ptr);

		ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
		}

		return;
	}
	case io_message_type::result:
	default:
		DOCA_LOG_ERR("Received message of unexpected type: %u", static_cast<uint32_t>(message_type));
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (ret == DOCA_SUCCESS)
		return;

	DOCA_LOG_ERR("Command error response: %s", io_message_to_string(io_message).c_str());
	auto *const response_task = static_cast<doca_rdma_task_send *>(
		doca_task_get_user_data(doca_rdma_task_write_as_task(transfer_ctx->write_task)).ptr);

	io_message_view::set_type(io_message_type::result, io_message);
	io_message_view::set_result(ret, io_message);

	ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
	}
}

/*
 * RDMA task receive error callback
 *
 * @task [in]: Failed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_rdma_task_receive_error_cb(doca_rdma_task_receive *task,
				     doca_data task_user_data,
				     doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	if (!hot_data->running_flag) {
		hot_data->abort("Failed to complete doca_rdma_task_receive");
	}
}

/*
 * RDMA task send callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_rdma_task_send_cb(doca_rdma_task_send *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);

	auto *const request_task = static_cast<doca_rdma_task_receive *>(task_user_data.ptr);

	doca_buf_reset_data_len(doca_rdma_task_receive_get_dst_buf(request_task));
	auto const ret = doca_task_submit(doca_rdma_task_receive_as_task(request_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed re-submit request task: %s", doca_error_get_name(ret));
	}

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);
	--(hot_data->in_flight_ops_count);
}

/*
 * RDMA task send error callback
 *
 * @task [in]: Failed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_rdma_task_send_error_cb(doca_rdma_task_send *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	if (!hot_data->running_flag) {
		hot_data->abort("Failed to complete doca_rdma_task_send");
	}
}

/*
 * Shared RDMA read/write callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void on_transfer_complete(doca_task *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);

	auto *const response_task = static_cast<doca_rdma_task_send *>(task_user_data.ptr);

	auto *const io_message =
		storage::get_buffer_bytes(const_cast<doca_buf *>(doca_rdma_task_send_get_src_buf(response_task)));

	io_message_view::set_type(io_message_type::result, io_message);
	io_message_view::set_result(DOCA_SUCCESS, io_message);

	auto const ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
	}
}

/*
 * Shared RDMA read/write error callback
 *
 * @task [in]: Failed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void on_transfer_error(doca_task *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	hot_data->abort("Failed to complete rdma data transfer");

	auto *const response_task = static_cast<doca_rdma_task_send *>(task_user_data.ptr);

	auto *const io_message =
		storage::get_buffer_bytes(const_cast<doca_buf *>(doca_rdma_task_send_get_src_buf(response_task)));

	io_message_view::set_type(io_message_type::result, io_message);
	io_message_view::set_result(DOCA_ERROR_IO_FAILED, io_message);

	auto const ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
	}
}

/*
 * Abort execution
 *
 * @reason [in]: The reason
 */
void thread_hot_data::abort(const std::string &reason)
{
	encountered_errors = true;

	if (running_flag) {
		running_flag = false;
		DOCA_LOG_ERR("Aborting due to: %s", reason.c_str());
		fflush(stdout);
		fflush(stderr);
	}
}

/*
 * Destructor
 */
thread_context::~thread_context()
{
	hot_data->running_flag = false;
	join();

	doca_error_t ret;

	if (data_rdma.rdma) {
		ret = storage::stop_context(doca_rdma_as_ctx(data_rdma.rdma), pe, data_tasks);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_rdma(%p): %s", data_rdma.rdma, doca_error_get_name(ret));
		}
		data_tasks.clear();
		ret = doca_rdma_destroy(data_rdma.rdma);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_rdma(%p): %s", data_rdma.rdma, doca_error_get_name(ret));
		}
	}

	if (ctrl_rdma.rdma) {
		ret = storage::stop_context(doca_rdma_as_ctx(ctrl_rdma.rdma), pe, ctrl_tasks);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_rdma(%p): %s", ctrl_rdma.rdma, doca_error_get_name(ret));
		}
		ctrl_tasks.clear();
		ret = doca_rdma_destroy(ctrl_rdma.rdma);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_rdma(%p): %s", ctrl_rdma.rdma, doca_error_get_name(ret));
		}
	}

	for (auto *buf : bufs)
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
	bufs.clear();

	if (buf_inv) {
		ret = doca_buf_inventory_destroy(buf_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_buf_inventory(%p):  %s",
				     buf_inv,
				     doca_error_get_name(ret));
		}
	}

	if (pe) {
		ret = doca_pe_destroy(pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_pe_destroy(%p):  %s", pe, doca_error_get_name(ret));
		}
	}

	if (storage_mmap) {
		ret = doca_mmap_stop(storage_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop storage doca_mmap(%p): %s",
				     storage_mmap,
				     doca_error_get_name(ret));
		}

		ret = doca_mmap_destroy(storage_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy storage doca_mmap(%p): %s",
				     storage_mmap,
				     doca_error_get_name(ret));
		}
	}

	if (host_mmap) {
		ret = doca_mmap_stop(host_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop remote doca_mmap(%p): %s", host_mmap, doca_error_get_name(ret));
		}

		ret = doca_mmap_destroy(host_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy remote doca_mmap(%p): %s", host_mmap, doca_error_get_name(ret));
		}
	}

	if (io_message_mmap) {
		ret = doca_mmap_stop(io_message_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop tasks doca_mmap(%p): %s",
				     io_message_mmap,
				     doca_error_get_name(ret));
		}

		ret = doca_mmap_destroy(io_message_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy tasks doca_mmap(%p): %s",
				     io_message_mmap,
				     doca_error_get_name(ret));
		}
	}

	if (io_messages_memory) {
		free(io_messages_memory);
	}

	if (storage_memory) {
		free(storage_memory);
	}

	free(hot_data->transfer_contexts);
	free(hot_data);
}

/*
 * Constructor
 *
 * @app_impl_ [in]: Pointer to parent impl
 * @dev_ [in]: Pointer to dev
 * @thread_id_ [in]: Logical id of this thread
 * @runtime_task_count_ [in]: Number of task sets this thread should have
 * @runtime_buffer_size_ [in]: Size of an io transfer
 */
thread_context::thread_context(target_rdma_application_impl *app_impl_,
			       doca_dev *dev_,
			       uint32_t thread_id_,
			       uint32_t runtime_task_count_,
			       uint32_t runtime_buffer_size_)
	: dev{dev_},
	  thread_id{thread_id_},
	  runtime_task_count{runtime_task_count_},
	  runtime_buffer_size{runtime_buffer_size_},
	  pe{nullptr},
	  hot_data{nullptr},
	  io_messages_memory{nullptr},
	  storage_memory{nullptr},
	  io_message_mmap{nullptr},
	  storage_mmap{nullptr},
	  host_mmap{nullptr},
	  buf_inv{nullptr},
	  ctrl_rdma{},
	  data_rdma{},
	  bufs{},
	  ctrl_tasks{},
	  data_tasks{},
	  thread{}
{
	try {
		hot_data = storage::make_aligned<thread_hot_data>{}.object();
	} catch (std::exception const &ex) {
		throw std::runtime_error{"Failed to allocate thread context hot data: "s + ex.what()};
	}

	hot_data->id = thread_id;
	hot_data->app_impl = app_impl_;

	try {
		hot_data->transfer_contexts =
			storage::make_aligned<transfer_context>{}.object_array(runtime_task_count_);
	} catch (std::exception const &ex) {
		throw std::runtime_error{"Failed to allocate thread context transfer contexts: "s + ex.what()};
	}

	hot_data->transfer_contexts_size = runtime_task_count_;
}

/*
 * Handle a configure storage request
 *
 * @mmap_export [in]: Remote mmap definition to allow access to end user memory
 *
 */
void thread_context::configure_storage(std::vector<uint8_t> const &mmap_export)
{
	auto const page_size = storage::get_system_page_size();
	auto const message_memory_size = runtime_task_count * io_message_buffer_size;

	host_mmap = storage::make_mmap(dev, mmap_export.data(), mmap_export.size());

	size_t remote_mmap_size = 0;
	hot_data->host_memory_start_addr = get_mmap_memrange_start(host_mmap, remote_mmap_size);

	io_messages_memory =
		static_cast<char *>(aligned_alloc(page_size, storage::aligned_size(page_size, message_memory_size)));
	if (io_messages_memory == nullptr) {
		throw std::runtime_error{"Failed to allocate buffers memory"};
	}

	storage_memory =
		static_cast<char *>(aligned_alloc(page_size, storage::aligned_size(page_size, remote_mmap_size)));
	if (storage_memory == nullptr) {
		throw std::runtime_error{"Failed to allocate buffers memory"};
	}

	io_message_mmap = storage::make_mmap(dev,
					     io_messages_memory,
					     message_memory_size,
					     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
						     DOCA_ACCESS_FLAG_RDMA_READ);

	storage_mmap = storage::make_mmap(dev,
					  storage_memory,
					  remote_mmap_size,
					  DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE |
						  DOCA_ACCESS_FLAG_RDMA_READ);

	/*
	 * Create 3x the number of doca_buf as we have tasks, one buf for the command memory (shared between read and
	 * write), two for the data buffers (src and dst)
	 */
	auto const doca_buf_count = runtime_task_count * 3;
	bufs.reserve(doca_buf_count);

	buf_inv = storage::make_buf_inventory(doca_buf_count);

	hot_data->storage_memory_start_addr = storage_memory;
}

/*
 * Create a rdma connection
 *
 * @request [in]: Request details
 * @return: rdma export blob to send back to the initiator
 */
std::vector<uint8_t> thread_context::create_rdma_connection(
	storage::zero_copy::control_message::create_rdma_connection_request const &request)
{
	doca_error_t ret;

	auto unq_rdma = std::unique_ptr<doca_rdma, void (*)(doca_rdma *)>{
		[this]() {
			return storage::make_rdma_context(dev,
							  pe,
							  doca_data{.ptr = hot_data},
							  DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
								  DOCA_ACCESS_FLAG_RDMA_READ |
								  DOCA_ACCESS_FLAG_RDMA_WRITE);
		}(),
		[](doca_rdma *obj) {
			static_cast<void>(doca_rdma_destroy(obj));
		}};

	if (request.role == rdma_connection_role::ctrl) {
		ret = doca_rdma_task_receive_set_conf(unq_rdma.get(),
						      doca_rdma_task_receive_cb,
						      doca_rdma_task_receive_error_cb,
						      runtime_task_count);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to set doca_rdma_task_receive task pool configuration: "s +
						 doca_error_get_name(ret)};
		}

		ret = doca_rdma_task_send_set_conf(unq_rdma.get(),
						   doca_rdma_task_send_cb,
						   doca_rdma_task_send_error_cb,
						   runtime_task_count);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to set doca_rdma_task_send task pool configuration: "s +
						 doca_error_get_name(ret)};
		}
	}

	if (request.role == rdma_connection_role::data) {
		ret = doca_rdma_task_read_set_conf(
			unq_rdma.get(),
			reinterpret_cast<doca_rdma_task_read_completion_cb_t>(on_transfer_complete),
			reinterpret_cast<doca_rdma_task_read_completion_cb_t>(on_transfer_error),
			runtime_task_count);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to set doca_rdma_task_read task pool configuration: "s +
						 doca_error_get_name(ret)};
		}

		ret = doca_rdma_task_write_set_conf(
			unq_rdma.get(),
			reinterpret_cast<doca_rdma_task_write_completion_cb_t>(on_transfer_complete),
			reinterpret_cast<doca_rdma_task_write_completion_cb_t>(on_transfer_error),
			runtime_task_count);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to set doca_rdma_task_write task pool configuration: "s +
						 doca_error_get_name(ret)};
		}
	}

	ret = doca_ctx_start(doca_rdma_as_ctx(unq_rdma.get()));
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to start doca_rdma context: "s + doca_error_get_name(ret)};
	}

	std::vector<uint8_t> export_blob;
	doca_rdma_connection *conn;
	{
		uint8_t const *conn_details;
		size_t conn_details_size;
		ret = doca_rdma_export(unq_rdma.get(),
				       reinterpret_cast<void const **>(&conn_details),
				       &conn_details_size,
				       &conn);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to export RDMA connection: "s + doca_error_get_name(ret)};
		}

		export_blob = std::vector<uint8_t>{conn_details, conn_details + conn_details_size};
	}

	ret = doca_rdma_connect(unq_rdma.get(),
				request.connection_details.data(),
				request.connection_details.size(),
				conn);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to connect using rdma connection details: "s +
					 doca_error_get_name(ret)};
	}

	if (request.role == rdma_connection_role::ctrl) {
		ctrl_rdma.conn = conn;
		ctrl_rdma.rdma = unq_rdma.release();
	} else {
		data_rdma.conn = conn;
		data_rdma.rdma = unq_rdma.release();
	}

	return export_blob;
}

/*
 * Create and submit tasks
 */
void thread_context::create_and_submit_tasks()
{
	doca_error_t ret;

	char *message_buffer_addr = io_messages_memory;

	std::vector<doca_task *> request_tasks;
	request_tasks.reserve(runtime_task_count);
	ctrl_tasks.reserve(runtime_task_count * 2);
	data_tasks.reserve(runtime_task_count);
	bufs.reserve(runtime_task_count * 3);

	void *mmap_start_addr;
	size_t mmap_size = 0;
	static_cast<void>(doca_mmap_get_memrange(host_mmap, &mmap_start_addr, &mmap_size));

	for (uint32_t ii = 0; ii != runtime_task_count; ++ii) {
		doca_buf *message_buf;

		ret = doca_buf_inventory_buf_get_by_addr(buf_inv,
							 io_message_mmap,
							 message_buffer_addr,
							 io_message_buffer_size,
							 &message_buf);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate message doca_buf: "s + doca_error_get_name(ret)};
		}

		bufs.push_back(message_buf);
		message_buffer_addr += io_message_buffer_size;

		ret = doca_buf_inventory_buf_get_by_addr(buf_inv,
							 storage_mmap,
							 hot_data->storage_memory_start_addr,
							 mmap_size,
							 std::addressof(hot_data->transfer_contexts[ii].storage_buf));
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate local storage doca_buf: "s +
						 doca_error_get_name(ret)};
		}

		bufs.push_back(hot_data->transfer_contexts[ii].storage_buf);

		ret = doca_buf_inventory_buf_get_by_addr(buf_inv,
							 host_mmap,
							 hot_data->host_memory_start_addr,
							 mmap_size,
							 std::addressof(hot_data->transfer_contexts[ii].host_buf));
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate remote storage doca_buf: "s +
						 doca_error_get_name(ret)};
		}

		bufs.push_back(hot_data->transfer_contexts[ii].host_buf);

		doca_rdma_task_receive *request_task = nullptr;
		ret = doca_rdma_task_receive_allocate_init(
			ctrl_rdma.rdma,
			message_buf,
			doca_data{.ptr = std::addressof(hot_data->transfer_contexts[ii])},
			&request_task);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate doca_rdma_task_receive: "s +
						 doca_error_get_name(ret)};
		}
		ctrl_tasks.push_back(doca_rdma_task_receive_as_task(request_task));
		request_tasks.push_back(doca_rdma_task_receive_as_task(request_task));

		doca_rdma_task_send *response_task = nullptr;
		ret = doca_rdma_task_send_allocate_init(ctrl_rdma.rdma,
							ctrl_rdma.conn,
							message_buf,
							doca_data{.ptr = request_task},
							&response_task);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate doca_rdma_task_send: "s +
						 doca_error_get_name(ret)};
		}
		ctrl_tasks.push_back(doca_rdma_task_send_as_task(response_task));

		ret = doca_rdma_task_write_allocate_init(data_rdma.rdma,
							 data_rdma.conn,
							 hot_data->transfer_contexts[ii].storage_buf,
							 hot_data->transfer_contexts[ii].host_buf,
							 doca_data{.ptr = response_task},
							 std::addressof(hot_data->transfer_contexts[ii].write_task));
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate doca_rdma_task_write: "s +
						 doca_error_get_name(ret)};
		}
		data_tasks.push_back(doca_rdma_task_write_as_task(hot_data->transfer_contexts[ii].write_task));

		ret = doca_rdma_task_read_allocate_init(data_rdma.rdma,
							data_rdma.conn,
							hot_data->transfer_contexts[ii].host_buf,
							hot_data->transfer_contexts[ii].storage_buf,
							doca_data{.ptr = response_task},
							std::addressof(hot_data->transfer_contexts[ii].read_task));
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate doca_rdma_task_read: "s +
						 doca_error_get_name(ret)};
		}
		data_tasks.push_back(doca_rdma_task_read_as_task(hot_data->transfer_contexts[ii].read_task));
	}

	DOCA_LOG_DBG("Submitting initial request tasks");
	for (auto *task : request_tasks) {
		ret = doca_task_submit(task);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to submit initial doca_rdma_task_receive: "s +
						 doca_error_get_name(ret)};
		}
	}
}

/*
 * Abort execution
 *
 * @reason [in]: The reason
 */
void thread_context::abort(const std::string &reason)
{
	hot_data->abort(reason);
}

/*
 * Join the held thread
 */
void thread_context::join(void)
{
	if (thread.joinable()) {
		thread.join();
	}
}

/*
 * Data path thread process
 *
 * @thread_id [in]: ID of the thread
 * @hot_data [in]: Data to use
 * @pe [in]: Progress engine to use
 */
void thread_proc(uint32_t thread_id, thread_hot_data *hot_data, doca_pe *pe)
{
	while (hot_data->wait_flag) {
		std::this_thread::yield();
	}

	DOCA_LOG_INFO("T[%u] running", thread_id);
	while (hot_data->running_flag) {
		doca_pe_progress(pe) ? ++(hot_data->pe_hit_count) : ++(hot_data->pe_miss_count);
	}

	while (hot_data->in_flight_ops_count != 0) {
		doca_pe_progress(pe) ? ++(hot_data->pe_hit_count) : ++(hot_data->pe_miss_count);
	}

	DOCA_LOG_INFO("T[%u] Completed", thread_id);
}

/*
 * Wrapper for the thread proc that will catch any thrown exceptions
 *
 * @weak_self [in]: Weak pointer to self to avoid cycles while still allowing the thread to ensure the associated data
 * object outlives the thread execution.
 */
void thread_proc_catch_wrapper(std::weak_ptr<thread_context> weak_self)
{
	auto self = weak_self.lock();
	if (!self) {
		DOCA_LOG_ERR("[BUG] Thread unable to launch");
		return;
	}

	try {
		thread_proc(self->thread_id, self->hot_data, self->pe);
	} catch (std::runtime_error const &ex) {
		self->hot_data->encountered_errors = true;
		DOCA_LOG_ERR("T[%u] encountered an error: %s", self->thread_id, ex.what());
	}

	if (self->hot_data->encountered_errors) {
		DOCA_LOG_ERR("T[%u] failed", self->thread_id);
	}
}

/*
 * Default destructor
 */
target_rdma_application_impl::~target_rdma_application_impl()
{
	doca_error_t ret;

	try {
		m_client_connection.close();
	} catch (std::runtime_error const &err) {
		DOCA_LOG_ERR("Failed to close client socket: %s", err.what());
	}
	try {
		m_listen_socket.close();
	} catch (std::runtime_error const &err) {
		DOCA_LOG_ERR("Failed to close listen socket: %s", err.what());
	}

	m_thread_contexts.clear();

	if (m_dev != nullptr) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca_dev(%p): %s", m_dev, doca_error_get_name(ret));
		}
	}
}

/*
 * Constructor
 *
 * @cfg [in]: Application configuration
 */
target_rdma_application_impl::target_rdma_application_impl(
	storage::zero_copy::target_rdma_application::configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_listen_socket{},
	  m_client_connection{},
	  m_tcp_rx_buffer{},
	  m_control_message_reassembler{},
	  m_ctrl_msgs{},
	  m_thread_contexts{},
	  m_stats{},
	  m_abort_flag{false}
{
}

/*
 * Run the application
 */
void target_rdma_application_impl::run(void)
{
	uint32_t err_correlation_id = 0;

	DOCA_LOG_DBG("Open doca_dev: %s", m_cfg.device_id.c_str());
	m_dev = storage::open_device(m_cfg.device_id);

	start_listening();

	wait_for_tcp_client();

	try {
		auto const msg = wait_for_control_message();
		err_correlation_id = msg.correlation_id;
		if (msg.type != control_message_type::configure_data_path) {
			DOCA_LOG_ERR("Received %s message while waiting for configure_storage request",
				     to_string(msg).c_str());
			throw std::runtime_error{
				"Received unexpected message while waiting for configure_storage message"};
		}

		auto const &configuration =
			reinterpret_cast<storage::zero_copy::control_message::configure_data_path &>(*msg.details);

		configure_storage(configuration);

		send_control_message(make_response_control_message(msg.correlation_id));

	} catch (std::runtime_error const &ex) {
		abort("Failed during configure storage process");
		send_control_message(make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
		throw;
	}

	try {
		for (auto &thread_context : m_thread_contexts) {
			auto const ret = doca_pe_create(std::addressof(thread_context->pe));
			if (ret != DOCA_SUCCESS) {
				throw std::runtime_error{"Failed to create doca_pe: "s + doca_error_get_name(ret)};
			}

			for (uint32_t ii = 0; ii != 2; ++ii) {
				auto const msg = wait_for_control_message();
				err_correlation_id = msg.correlation_id;
				if (msg.type != control_message_type::create_rdma_connection_request) {
					DOCA_LOG_ERR(
						"Received %s message while waiting for create_rdma_connection_request",
						to_string(msg).c_str());
					throw std::runtime_error{
						"Received unexpected message while waiting for create_rdma_connection_request message"};
				}

				auto const &request_conn_details = reinterpret_cast<
					storage::zero_copy::control_message::create_rdma_connection_request &>(
					*msg.details);
				auto response_conn_details =
					thread_context->create_rdma_connection(request_conn_details);

				send_control_message(make_create_rdma_connection_response_control_message(
					msg.correlation_id,
					request_conn_details.role,
					std::move(response_conn_details)));
			}
		}

		auto const msg = wait_for_control_message();
		err_correlation_id = msg.correlation_id;
		if (msg.type != control_message_type::start_data_path_connections) {
			DOCA_LOG_ERR("Received %s message while waiting for establish_rdma_connections",
				     to_string(msg).c_str());
			throw std::runtime_error{
				"Received unexpected message while waiting for establish_rdma_connections message"};
		}

		establish_rdma_connections();

		send_control_message(make_response_control_message(msg.correlation_id));

	} catch (std::runtime_error const &ex) {
		abort("Failed during connection establishment process");
		send_control_message(make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
		throw;
	}

	try {
		auto const msg = wait_for_control_message();
		err_correlation_id = msg.correlation_id;
		if (msg.type != control_message_type::start_storage) {
			DOCA_LOG_ERR("Received %s message while waiting for start_storage request",
				     to_string(msg).c_str());
			throw std::runtime_error{"Received unexpected message while waiting for start_storage message"};
		}

		/* create worker threads */
		for (auto &thread_context : m_thread_contexts) {
			thread_context->create_and_submit_tasks();
			thread_context->thread = std::thread{thread_proc_catch_wrapper, std::weak_ptr{thread_context}};
			try {
				storage::set_thread_affinity(thread_context->thread,
							     m_cfg.cpu_set[thread_context->thread_id]);
			} catch (std::exception const &) {
				thread_context->abort("Failed to set affinity for thread to core: "s +
						      std::to_string(m_cfg.cpu_set[thread_context->thread_id]));
				thread_context->hot_data->running_flag = false;
				thread_context->hot_data->wait_flag = false;
				throw;
			}
		}

		/* start worker threads */
		for (auto &thread_context : m_thread_contexts) {
			thread_context->hot_data->wait_flag = false;
		}

		/* Notify ready to run */
		send_control_message(make_response_control_message(msg.correlation_id));
	} catch (std::runtime_error const &ex) {
		abort("Failed during start storage establishment process");
		send_control_message(make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
		throw;
	}

	if (m_abort_flag)
		return;

	/* Wait for all threads to complete */
	for (auto &thread_context : m_thread_contexts) {
		thread_context->join();
	}

	try {
		auto const msg = wait_for_control_message();
		err_correlation_id = msg.correlation_id;
		if (msg.type != control_message_type::destroy_objects)
			throw std::runtime_error{
				"Received unexpected message while waiting for destroy_objects message"};

		destroy_objects();
		send_control_message(make_response_control_message(msg.correlation_id));
	} catch (std::runtime_error const &ex) {
		send_control_message(make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
		throw;
	}
}

/*
 * Abort execution
 *
 * @reason [in]: The reason
 */
void target_rdma_application_impl::abort(std::string const &reason)
{
	m_abort_flag = true;
	for (auto &thread_context : m_thread_contexts) {
		thread_context->abort(reason);
	}
}

/*
 * Get the final statistics
 *
 * @return: The statistics
 */
std::vector<storage::zero_copy::target_rdma_application::thread_stats> target_rdma_application_impl::get_stats(
	void) const
{
	return m_stats;
}

/*
 * Stop all running data path threads
 */
void target_rdma_application_impl::stop_all_threads(void)
{
	for (auto &thread_context : m_thread_contexts) {
		thread_context->hot_data->running_flag = false;
	}
}

/*
 * Wait (and poll) for a control message to arrive
 *
 * @return: The received control message
 */
storage::zero_copy::control_message target_rdma_application_impl::wait_for_control_message(void)
{
	control_message msg;

	for (;;) {
		if (m_control_message_reassembler.append(m_tcp_rx_buffer.data(),
							 m_client_connection.read(m_tcp_rx_buffer.data(),
										  m_tcp_rx_buffer.size()))) {
			msg = m_control_message_reassembler.extract_message();
			break;
		}

		if (m_abort_flag) {
			throw std::runtime_error{"Aborted while waiting for control message"};
		}
	}

	return msg;
}

/*
 * Send a control message
 *
 * @message [in]: The message to send
 */
void target_rdma_application_impl::send_control_message(storage::zero_copy::control_message message)
{
	std::array<char, control_message_buffer_size> buffer{};
	auto msg_size = to_buffer(message, buffer.data(), buffer.size());
	if (msg_size == 0) {
		throw std::runtime_error{"Failed to format io_start message"};
	}

	if (m_client_connection.write(buffer.data(), msg_size) != msg_size) {
		throw std::runtime_error{"Failed to send start_storage to storage server"};
	}
}

/*
 * Start listening for TCP connections
 */
void target_rdma_application_impl::start_listening(void)
{
	m_listen_socket.listen(m_cfg.listen_port);
}

/*
 * Wait for a TCP client to connect
 */
void target_rdma_application_impl::wait_for_tcp_client(void)
{
	DOCA_LOG_INFO("Start listening on TCP port: %u", m_cfg.listen_port);
	fflush(stdout);
	fflush(stderr);
	do {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		if (m_abort_flag) {
			throw std::runtime_error{"Aborted while waiting for client TCP connection"};
		}

		m_client_connection = m_listen_socket.accept();

	} while (!m_client_connection.is_valid());

	DOCA_LOG_INFO("DPU connected");
}

/*
 * Process configure storage request
 *
 * @request [in]: Request
 */
void target_rdma_application_impl::configure_storage(
	storage::zero_copy::control_message::configure_data_path const &request)
{
	m_thread_contexts.reserve(m_cfg.cpu_set.size());
	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		m_thread_contexts.push_back(
			std::make_shared<thread_context>(this, m_dev, ii, request.buffer_count, request.buffer_size));
		m_thread_contexts.back()->configure_storage(request.mmap_export_blob);
	}

	size_t remote_mmap_size = 0;
	static_cast<void>(get_mmap_memrange_start(m_thread_contexts[0]->host_mmap, remote_mmap_size));
	if (remote_mmap_size != request.buffer_count * request.buffer_size * m_cfg.cpu_set.size()) {
		throw std::runtime_error{
			"[BUG] Expected remote memory map to consist of " +
			std::to_string(request.buffer_count * request.buffer_size * m_cfg.cpu_set.size()) +
			" bytes but it actually consists of " + std::to_string(remote_mmap_size) + " bytes"};
	}
}

/*
 * Establish rdma connections
 */
void target_rdma_application_impl::establish_rdma_connections(void)
{
	doca_error_t ret;
	doca_ctx_states rdma_state;
	uint32_t pending_rdma_connection_count;
	for (;;) {
		pending_rdma_connection_count = 0;
		for (auto &thread_context : m_thread_contexts) {
			static_cast<void>(doca_pe_progress(thread_context->pe));

			ret = doca_ctx_get_state(doca_rdma_as_ctx(thread_context->ctrl_rdma.rdma), &rdma_state);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(ret));
				throw std::runtime_error{"Failed to query rdma context state"};
			} else if (rdma_state != DOCA_CTX_STATE_RUNNING) {
				++pending_rdma_connection_count;
			}

			ret = doca_ctx_get_state(doca_rdma_as_ctx(thread_context->data_rdma.rdma), &rdma_state);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(ret));
				throw std::runtime_error{"Failed to query rdma context state"};
			} else if (rdma_state != DOCA_CTX_STATE_RUNNING) {
				++pending_rdma_connection_count;
			}
		}

		if (pending_rdma_connection_count == 0) {
			DOCA_LOG_INFO("All connections established");
			break;
		}

		if (m_abort_flag == false) {
			throw std::runtime_error{"Aborted waiting for rdma and comch connections to establish"};
		}
	}
}

/*
 * Destroy objects
 */
void target_rdma_application_impl::destroy_objects(void)
{
	m_stats.reserve(m_thread_contexts.size());
	std::transform(std::begin(m_thread_contexts), std::end(m_thread_contexts), std::back_inserter(m_stats), [](auto const &thread_context) {
		return thread_stats{thread_context->hot_data->pe_hit_count, thread_context->hot_data->pe_miss_count};
	});

	m_thread_contexts.clear();
}

} /* namespace */

storage::zero_copy::target_rdma_application *make_target_rdma_application(
	storage::zero_copy::target_rdma_application::configuration const &cfg)
{
	return new target_rdma_application_impl{cfg};
}

} /* namespace storage::zero_copy */
