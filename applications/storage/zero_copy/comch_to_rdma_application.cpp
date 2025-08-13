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

#include <zero_copy/comch_to_rdma_application.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <thread>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <storage_common/aligned_new.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/doca_utils.hpp>
#include <storage_common/ip_address.hpp>
#include <storage_common/os_utils.hpp>
#include <storage_common/tcp_socket.hpp>

#include <zero_copy/control_message.hpp>
#include <zero_copy/io_message.hpp>

DOCA_LOG_REGISTER(DPU_APPLICATION);

using namespace std::string_literals;

namespace storage::zero_copy {

namespace {

class comch_to_rdma_application_impl;
struct alignas(storage::cache_line_size) thread_hot_data {
	uint64_t pe_hit_count = 0;
	uint64_t pe_miss_count = 0;
	comch_to_rdma_application_impl *app_impl;
	uint32_t in_flight_ops = 0;
	uint32_t id = 0;
	std::atomic_bool wait_flag = true;
	std::atomic_bool running_flag = true;
	bool encountered_errors = false;
	uint8_t batch_count = 1;
	uint8_t batch_size = 1;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(const std::string &reason);

	/*
	 * ComCh post recv helper to control batch submission flags
	 *
	 * @task [in]: Task to submit
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t submit_comch_recv_task(doca_comch_consumer_task_post_recv *task);
};

static_assert(sizeof(std::atomic_bool) == 1, "Expected atomic bool to occupy 1 byte");
static_assert(sizeof(thread_hot_data) == storage::cache_line_size, "Expected cache_line_size to occupy one cache line");

struct thread_context {
	thread_hot_data *hot_data;
	doca_pe *data_pe;
	doca_pe *ctrl_pe;
	uint32_t thread_id;
	uint32_t datapath_buffer_count;
	uint32_t datapath_buffer_size;
	uint32_t batch_size;
	char *io_messages_memory;
	doca_mmap *io_messages_mmap;
	doca_buf_inventory *buf_inv;
	std::vector<doca_buf *> doca_bufs;
	doca_comch_consumer *consumer;
	doca_comch_producer *producer;
	storage::rdma_conn_pair rdma_ctrl_ctx;
	storage::rdma_conn_pair rdma_data_ctx;
	std::vector<doca_comch_consumer_task_post_recv *> host_request_tasks;
	std::vector<doca_comch_producer_task_send *> host_response_tasks;
	std::vector<doca_rdma_task_send *> storage_request_tasks;
	std::vector<doca_rdma_task_receive *> storage_response_tasks;
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
	 * @app_impl_ [in]: Pointer to parent application
	 * @thread_id_ [in]: Thread ID
	 * @ctrl_pe_ [in]: Control progress engine
	 * @datapath_buffer_count_ [in]: Number of buffers
	 * @datapath_buffer_size_ [in]: Size of buffers
	 * @batch_size_ [in]: Batch size
	 */
	thread_context(comch_to_rdma_application_impl *app_impl_,
		       uint32_t thread_id_,
		       doca_pe *ctrl_pe_,
		       uint32_t datapath_buffer_count_,
		       uint32_t datapath_buffer_size_,
		       uint32_t batch_size_);

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
	 * Create runtime objects
	 *
	 * @dev [in]: Device to use
	 * @comm [in]: Client connection
	 */
	void create_objects(doca_dev *dev, doca_comch_connection *conn);

	/*
	 * Allocate and submit tasks
	 *
	 * @remote_consumer_id [in]: ID of the consumer this thread should send responses to
	 */
	void allocate_and_submit_tasks(uint32_t remote_consumer_id);

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(const std::string &reason);

	/*
	 * Join the work thread
	 */
	void join(void);

private:
	/*
	 * Create a RDMA context
	 *
	 * @dev [in]: Device to use
	 * @role [in]: Role of the connection
	 * @pe [in]: Progress engine to use
	 */
	doca_rdma *create_rdma_context(doca_dev *dev, rdma_connection_role role, doca_pe *pe);
};

class comch_to_rdma_application_impl : public comch_to_rdma_application {
public:
	/*
	 * Destructor
	 */
	~comch_to_rdma_application_impl() override;

	/*
	 * Deleted default constructor
	 */
	comch_to_rdma_application_impl() = delete;

	/*
	 * Constructor
	 *
	 * @cfg [in]: Configuration
	 */
	explicit comch_to_rdma_application_impl(comch_to_rdma_application::configuration const &cfg);

	/*
	 * Deleted copy constructor
	 */
	comch_to_rdma_application_impl(comch_to_rdma_application_impl const &) = delete;

	/*
	 * Deleted move constructor
	 */
	comch_to_rdma_application_impl(comch_to_rdma_application_impl &&) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	comch_to_rdma_application_impl &operator=(comch_to_rdma_application_impl const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	comch_to_rdma_application_impl &operator=(comch_to_rdma_application_impl &&) noexcept = delete;

	/*
	 * Run the application
	 */
	void run(void) override;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(const std::string &reason) override;

	/*
	 * Get end of run statistics
	 *
	 * @return: Statistics
	 */
	std::vector<storage::zero_copy::comch_to_rdma_application::thread_stats> get_stats(void) const override;

	/*
	 * Store a control message so it can be processed later
	 *
	 * @msg [in]: The message to store
	 */
	void store_control_message_response(storage::zero_copy::control_message msg) noexcept;

	/*
	 * Client connection callback
	 *
	 * @conn [in]: The new connection
	 */
	void on_client_connected(doca_comch_connection *conn) noexcept;

	/*
	 * Client disconnection callback
	 *
	 * @conn [in]: The disconnected connection
	 */
	void on_client_disconnected(doca_comch_connection *conn) noexcept;

	/*
	 * Consumer connection callback
	 *
	 * @consumer_id [in]: ID of the connected consumer
	 */
	void on_consumer_connected(uint32_t consumer_id) noexcept;

	/*
	 * Consumer disconnection callback
	 *
	 * @consumer_id [in]: ID of the disconnected consumer
	 */
	void on_consumer_disconnected(uint32_t consumer_id) noexcept;

	/*
	 * Stop all data threads
	 */
	void stop_all_threads(void);

private:
	comch_to_rdma_application::configuration m_cfg;
	doca_dev *m_dev;
	doca_dev_rep *m_dev_rep;
	doca_pe *m_ctrl_pe;
	storage::tcp_socket m_storage_connection;
	doca_comch_server *m_comch_server;
	doca_comch_connection *m_client_connection;
	doca_mmap *m_host_mmap;
	std::vector<uint8_t> m_mmap_export_blob;
	std::vector<std::shared_ptr<thread_context>> m_thread_contexts;
	std::vector<storage::zero_copy::control_message> m_ctrl_msgs;
	std::array<char, control_message_buffer_size> m_tcp_rx_buffer;
	storage::zero_copy::control_message_reassembler m_control_message_reassembler;
	std::vector<storage::zero_copy::comch_to_rdma_application::thread_stats> m_stats;
	std::vector<uint32_t> m_remote_consumer_ids;
	uint32_t m_datapath_buffer_count;
	uint32_t m_datapath_buffer_size;
	uint32_t m_datapath_batch_size;
	bool m_abort_flag;

	/*
	 * Wait for a control message (polling until it arrives)
	 *
	 * @throws std::runtime_error: Unable to retrieve message
	 *
	 * @return: received control message
	 */
	storage::zero_copy::control_message wait_for_control_message(void);

	/*
	 * Send a control message to the host side
	 *
	 * @throws std::runtime_error: Unable to send message
	 *
	 * @message [in]: Message to send
	 */
	void send_host_control_message(storage::zero_copy::control_message message);

	/*
	 * Send a control message to the storage side
	 *
	 * @throws std::runtime_error: Unable to send message
	 *
	 * @message [in]: Message to send
	 */
	void send_storage_control_message(storage::zero_copy::control_message message);

	/*
	 * Configure storage
	 *
	 * @throws std::runtime_error: An error occurred
	 *
	 * @request [in]: Storage configuration
	 */
	void configure_storage(storage::zero_copy::control_message::configure_data_path const &request);

	/*
	 * Connect to storage server
	 *
	 * @throws std::runtime_error: An error occurred
	 */
	void connect_storage_server(void);

	/*
	 * Create comch server
	 *
	 * @throws std::runtime_error: An error occurred
	 */
	void create_comch_server(void);

	/*
	 * Prepare storage context
	 *
	 * @thread_id [in]: ID of the thread to create
	 * @correlation_id [in]: Correlation id to use for requests and responses to storage server
	 */
	void prepare_storage_context(uint32_t thread_id, uint32_t correlation_id);

	/*
	 * Create RDMA connection
	 *
	 * @thread_id [in]: ID of the thread
	 * @role [in]: RDMA role
	 * @correlation_id [in]: Correlation id to use for requests and responses to storage server
	 * @rdma_conn_pair [in/out]: Connection pair with existing RDMA context to populate the connection part
	 */
	void connect_rdma(uint32_t thread_id,
			  rdma_connection_role role,
			  uint32_t correlation_id,
			  storage::rdma_conn_pair &rdma_conn_pair);

	/*
	 * Wait for all RDMA and ComCh connections to be fully connected and ready for data path operations
	 */
	void wait_for_connections_to_establish(void);

	/*
	 * Tear down data path objects
	 */
	void destroy_objects(void);
};

/*
 * ComCh control task send callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_task_send_cb(doca_comch_task_send *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);
	static_cast<void>(ctx_user_data);

	doca_task_free(doca_comch_task_send_as_task(task));
}

/*
 * ComCh control task send error callback
 *
 * @task [in]: Failed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_task_send_error_cb(doca_comch_task_send *task,
				   doca_data task_user_data,
				   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);
	static_cast<comch_to_rdma_application_impl *>(ctx_user_data.ptr)
		->abort("Failed to complete doca_comch_task_send");

	doca_task_free(doca_comch_task_send_as_task(task));
}

/*
 * ComCh control message received callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_event_msg_recv_cb(doca_comch_event_msg_recv *event,
				  uint8_t *recv_buffer,
				  uint32_t msg_len,
				  doca_comch_connection *comch_connection) noexcept
{
	static_cast<void>(event);

	control_message msg{};
	auto *const app = static_cast<comch_to_rdma_application_impl *>(
		doca_comch_connection_get_user_data(comch_connection).ptr);

	if (from_buffer(reinterpret_cast<char const *>(recv_buffer), msg_len, msg) != 0) {
		app->store_control_message_response(std::move(msg));
	} else {
		app->abort("Unable to parse control message");
	}
}

/*
 * ComCh consumer message received callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
					   doca_data task_user_data,
					   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);
	doca_error_t ret;

	/*
	 * Submit send of the data to the storage backend. Note: both tasks share the same doca buf so the data message
	 * is forwarded verbatim without any action on the users part.
	 */
	ret = doca_task_submit(static_cast<doca_task *>(task_user_data.ptr));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_rdma_task_send: %s", doca_error_get_name(ret));
		hot_data->abort("Failed to submit doca_rdma_task_send");
		return;
	}
}

/*
 * ComCh consumer error received callback
 *
 * @task [in]: Failed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
						 doca_data task_user_data,
						 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	if (!hot_data->running_flag) {
		/*
		 * Only consider it a failure when this callback triggers while running. This callback will be triggered
		 * as part of teardown as the submitted receive tasks that were never filled by requests from the host
		 * get flushed out.
		 */
		hot_data->abort("Failed to complete doca_comch_consumer_task_post_recv");
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

	auto *host_request_task = static_cast<doca_comch_consumer_task_post_recv *>(task_user_data.ptr);
	auto *hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);
	static_cast<void>(doca_buf_reset_data_len(doca_comch_consumer_task_post_recv_get_buf(host_request_task)));
	auto ret = hot_data->submit_comch_recv_task(host_request_task);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_consumer_task_post_recv: %s", doca_error_get_name(ret));
		hot_data->abort("Failed to submit doca_comch_consumer_task_post_recv");
	}
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

	static_cast<thread_hot_data *>(ctx_user_data.ptr)->abort("Failed to complete doca_rdma_task_send");
}

/*
 * ComCh producer task send callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
				      doca_data task_user_data,
				      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	auto *storage_response_task = static_cast<doca_rdma_task_receive *>(task_user_data.ptr);

	static_cast<void>(doca_buf_reset_data_len(doca_rdma_task_receive_get_dst_buf(storage_response_task)));
	auto ret = doca_task_submit(doca_rdma_task_receive_as_task(storage_response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_rdma_task_receive: %s", doca_error_get_name(ret));
		hot_data->abort("Failed to submit doca_rdma_task_receive");
	}
}

/*
 * ComCh producer task send error callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
					    doca_data task_user_data,
					    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	static_cast<thread_hot_data *>(ctx_user_data.ptr)->abort("Failed to complete doca_comch_producer_task_send");
}

/*
 * RDMA message received callback
 *
 * @task [in]: Completed task
 * @task_user_data [in]: Data associated with the task
 * @ctx_user_data [in]: Data associated with the context
 */
void doca_rdma_task_receive_cb(doca_rdma_task_receive *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	auto *const hot_data = static_cast<thread_hot_data *>(ctx_user_data.ptr);
	doca_error_t ret;

	auto *const io_message = storage::get_buffer_bytes(doca_rdma_task_receive_get_dst_buf(task));

	if (io_message_view::get_type(io_message) != io_message_type::stop) {
		io_message_view::set_type(io_message_type::result, io_message);
		io_message_view::set_result(DOCA_SUCCESS, io_message);
	} else {
		hot_data->app_impl->stop_all_threads();
	}

	do {
		ret = doca_task_submit(static_cast<doca_task *>(task_user_data.ptr));
	} while (ret == DOCA_ERROR_AGAIN);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_producer_task_send: %s", doca_error_get_name(ret));
		hot_data->abort("Failed to submit doca_comch_producer_task_send");
	}
}

/*
 * RDMA error received callback
 *
 * @task [in]: Completed task
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
		/*
		 * Only consider it a failure when this callback triggers while running. This callback will be triggered
		 * as part of teardown as the submitted receive tasks that were never filled by requests from the host
		 * get flushed out.
		 */
		hot_data->abort("Failed to complete doca_rdma_task_receive");
	}
}

/*
 * ComCh client connection callback
 *
 * @event [in]: Event
 * @conn [in]: The new client connection
 * @change_successful [in]: 1 if the connection was accepted successfully and 0 otherwise.
 */
void doca_comch_event_connection_connected_cb(doca_comch_event_connection_status_changed *event,
					      doca_comch_connection *conn,
					      uint8_t change_successful) noexcept
{
	static_cast<void>(event);

	DOCA_LOG_DBG("Connection %p %s", conn, (change_successful ? "connected" : "refused"));

	if (change_successful == 0) {
		DOCA_LOG_ERR("Failed to accept new client connection");
		return;
	}

	doca_data user_data{.ptr = nullptr};
	auto const ret =
		doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(conn)), &user_data);
	if (ret != DOCA_SUCCESS || user_data.ptr == nullptr) {
		DOCA_LOG_ERR("[BUG] unable to extract user data");
		return;
	}

	static_cast<comch_to_rdma_application_impl *>(user_data.ptr)->on_client_connected(conn);
}

/*
 * ComCh client disconnection callback
 *
 * @event [in]: Event
 * @conn [in]: The disconnected client connection
 * @change_successful [in]: 1 if the connection was disconnected successfully and 0 otherwise.
 */
void doca_comch_event_connection_disconnected_cb(doca_comch_event_connection_status_changed *event,
						 doca_comch_connection *conn,
						 uint8_t change_successful) noexcept
{
	static_cast<void>(event);
	static_cast<void>(change_successful);

	doca_data user_data{.ptr = nullptr};
	auto const ret =
		doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(conn)), &user_data);
	if (ret != DOCA_SUCCESS || user_data.ptr == nullptr) {
		DOCA_LOG_ERR("[BUG] unable to extract user data");
		return;
	}

	static_cast<comch_to_rdma_application_impl *>(user_data.ptr)->on_client_disconnected(conn);
}

/*
 * ComCh consumer connection callback
 *
 * @event [in]: Event
 * @conn [in]: The connection the remote consumer is using
 * @id [in]: The remote consumers ID
 */
void doca_comch_event_consumer_connected_cb(doca_comch_event_consumer *event,
					    doca_comch_connection *conn,
					    uint32_t id) noexcept
{
	static_cast<void>(event);

	static_cast<comch_to_rdma_application_impl *>(doca_comch_connection_get_user_data(conn).ptr)
		->on_consumer_connected(id);
}

/*
 * ComCh consumer disconnection callback
 *
 * @event [in]: Event
 * @conn [in]: The connection the remote consumer was using
 * @id [in]: The remote consumers ID
 */
void doca_comch_event_consumer_expired_cb(doca_comch_event_consumer *event,
					  doca_comch_connection *conn,
					  uint32_t id) noexcept
{
	static_cast<void>(event);

	static_cast<comch_to_rdma_application_impl *>(doca_comch_connection_get_user_data(conn).ptr)
		->on_consumer_disconnected(id);
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
 * ComCh post recv helper to control batch submission flags
 *
 * @task [in]: Task to submit
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t thread_hot_data::submit_comch_recv_task(doca_comch_consumer_task_post_recv *task)
{
	doca_task_submit_flag submit_flag = DOCA_TASK_SUBMIT_FLAG_NONE;
	if (--batch_count == 0) {
		submit_flag = DOCA_TASK_SUBMIT_FLAG_FLUSH;
		batch_count = batch_size;
	}

	return doca_task_submit_ex(doca_comch_consumer_task_post_recv_as_task(task), submit_flag);
}

/*
 * Destructor
 */
thread_context::~thread_context()
{
	doca_error_t ret;
	hot_data->running_flag = false;
	join();

	if (rdma_ctrl_ctx.rdma != nullptr) {
		/* gather all rdma tasks */
		std::vector<doca_task *> rdma_tasks;
		rdma_tasks.reserve(datapath_buffer_size * 2);
		std::transform(std::begin(storage_request_tasks),
			       std::end(storage_request_tasks),
			       std::back_inserter(rdma_tasks),
			       doca_rdma_task_send_as_task);
		std::transform(std::begin(storage_response_tasks),
			       std::end(storage_response_tasks),
			       std::back_inserter(rdma_tasks),
			       doca_rdma_task_receive_as_task);

		/* stop context with tasks list (tasks must be destroyed to finish stopping process) */
		ret = storage::stop_context(doca_rdma_as_ctx(rdma_ctrl_ctx.rdma), data_pe, rdma_tasks);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to stop rdma control context: %s",
				     thread_id,
				     doca_error_get_name(ret));
		}

		ret = doca_rdma_destroy(rdma_ctrl_ctx.rdma);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy rdma control context: %s",
				     thread_id,
				     doca_error_get_name(ret));
		}
	}

	if (rdma_data_ctx.rdma != nullptr) {
		ret = storage::stop_context(doca_rdma_as_ctx(rdma_data_ctx.rdma), data_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to stop rdma data context: %s", thread_id, doca_error_get_name(ret));
		}

		ret = doca_rdma_destroy(rdma_data_ctx.rdma);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy rdma data context: %s",
				     thread_id,
				     doca_error_get_name(ret));
		}
	}

	if (consumer != nullptr) {
		std::vector<doca_task *> consumer_tasks;
		consumer_tasks.reserve(datapath_buffer_size);
		std::transform(std::begin(host_request_tasks),
			       std::end(host_request_tasks),
			       std::back_inserter(consumer_tasks),
			       doca_comch_consumer_task_post_recv_as_task);

		ret = storage::stop_context(doca_comch_consumer_as_ctx(consumer), data_pe, consumer_tasks);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to stop comch consumer: %s", thread_id, doca_error_get_name(ret));
		}

		ret = doca_comch_consumer_destroy(consumer);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy comch consumer: %s", thread_id, doca_error_get_name(ret));
		}
	}

	if (producer != nullptr) {
		std::vector<doca_task *> producer_tasks;
		producer_tasks.reserve(datapath_buffer_size);
		std::transform(std::begin(host_response_tasks),
			       std::end(host_response_tasks),
			       std::back_inserter(producer_tasks),
			       doca_comch_producer_task_send_as_task);

		ret = storage::stop_context(doca_comch_producer_as_ctx(producer), data_pe, producer_tasks);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to stop comch producer: %s", thread_id, doca_error_get_name(ret));
		}

		ret = doca_comch_producer_destroy(producer);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy comch producer: %s", thread_id, doca_error_get_name(ret));
		}
	}

	if (data_pe != nullptr) {
		ret = doca_pe_destroy(data_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy progress engine: %s",
				     thread_id,
				     doca_error_get_name(ret));
		}
	}

	for (auto *buf : doca_bufs)
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));

	if (buf_inv != nullptr) {
		ret = doca_buf_inventory_destroy(buf_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy inventory: %s", thread_id, doca_error_get_name(ret));
		}
	}

	if (io_messages_mmap != nullptr) {
		ret = doca_mmap_destroy(io_messages_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("T[%u] Failed to destroy mmap: %s", thread_id, doca_error_get_name(ret));
		}
	}

	free(io_messages_memory);

	free(hot_data);
}

/*
 * Constructor
 *
 * @app_impl_ [in]: Pointer to parent application
 * @thread_id_ [in]: Thread ID
 * @ctrl_pe_ [in]: Control progress engine
 * @datapath_buffer_count_ [in]: Number of buffers
 * @datapath_buffer_size_ [in]: Size of buffers
 * @batch_size_ [in]: Batch size
 */
thread_context::thread_context(comch_to_rdma_application_impl *app_impl_,
			       uint32_t thread_id_,
			       doca_pe *ctrl_pe_,
			       uint32_t datapath_buffer_count_,
			       uint32_t datapath_buffer_size_,
			       uint32_t batch_size_)
	: hot_data{nullptr},
	  data_pe{nullptr},
	  ctrl_pe{ctrl_pe_},
	  thread_id{thread_id_},
	  datapath_buffer_count{datapath_buffer_count_},
	  datapath_buffer_size{datapath_buffer_size_},
	  batch_size{batch_size_},
	  io_messages_memory{nullptr},
	  io_messages_mmap{nullptr},
	  buf_inv{nullptr},
	  doca_bufs{},
	  consumer{nullptr},
	  producer{nullptr},
	  rdma_ctrl_ctx{},
	  rdma_data_ctx{},
	  host_request_tasks{},
	  host_response_tasks{},
	  storage_request_tasks{},
	  storage_response_tasks{},
	  thread{}
{
	try {
		hot_data = storage::make_aligned<thread_hot_data>{}.object();
	} catch (std::exception const &ex) {
		throw std::runtime_error{"Failed to allocate thread hot data: "s + ex.what()};
	}

	hot_data->id = thread_id;
	hot_data->app_impl = app_impl_;
	hot_data->batch_size = batch_size_;
}

/*
 * Create runtime objects
 *
 * @dev [in]: Device to use
 * @comch_conn [in]: Client connection
 */
void thread_context::create_objects(doca_dev *dev, doca_comch_connection *comch_conn)
{
	/*
	 * Allocate enough memory for at datapath_buffer_count + cfg.batch_size receive tasks. cfg.batch_size receive
	 * tasks are over-allocated due to how batched receive tasks work. The requirement is to always be able to have
	 * N tasks in flight. While also lowering the cost of task submission by batching. So because upto
	 * cfg.batch_size -1 tasks could be submitted but not yet flushed means that a surplus of cfg.batch_size tasks
	 * are required to maintain always having N active tasks.
	 */
	auto const page_size = storage::get_system_page_size();
	auto const message_qty = (datapath_buffer_count * 2) + batch_size;
	auto const memory_qty = message_qty * io_message_buffer_size;

	doca_error_t ret;

	ret = doca_pe_create(std::addressof(data_pe));
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_pe: "s + doca_error_get_name(ret)};
	}

	DOCA_LOG_DBG("T[%u] Allocate message buffer memory (%lu bytes, aligned to %u byte pages)",
		     thread_id,
		     memory_qty,
		     page_size);
	io_messages_memory =
		static_cast<char *>(aligned_alloc(page_size, storage::aligned_size(page_size, memory_qty)));
	if (io_messages_memory == nullptr) {
		throw std::runtime_error{"Failed to allocate message buffer memory"};
	}

	io_messages_mmap = storage::make_mmap(dev,
					      io_messages_memory,
					      memory_qty,
					      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);

	ret = doca_buf_inventory_create(message_qty, &(buf_inv));
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create messages doca_buf_inventory: "s + doca_error_get_name(ret)};
	}

	ret = doca_buf_inventory_start(buf_inv);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to start messages doca_buf_inventory: "s + doca_error_get_name(ret)};
	}

	consumer = storage::make_comch_consumer(comch_conn,
						io_messages_mmap,
						data_pe,
						datapath_buffer_count + batch_size,
						doca_data{.ptr = hot_data},
						doca_comch_consumer_task_post_recv_cb,
						doca_comch_consumer_task_post_recv_error_cb);
	DOCA_LOG_DBG("T[%u] Created consumer %p", thread_id, consumer);

	producer = storage::make_comch_producer(comch_conn,
						data_pe,
						datapath_buffer_count,
						doca_data{.ptr = hot_data},
						doca_comch_producer_task_send_cb,
						doca_comch_producer_task_send_error_cb);
	DOCA_LOG_DBG("T[%u] Created producer %p", thread_id, producer);

	rdma_ctrl_ctx.rdma = create_rdma_context(dev, rdma_connection_role::ctrl, data_pe);
	rdma_data_ctx.rdma = create_rdma_context(dev, rdma_connection_role::data, ctrl_pe);
}

/*
 * Allocate and submit tasks
 *
 * @remote_consumer_id [in]: ID of the consumer this thread should send responses to
 */
void thread_context::allocate_and_submit_tasks(uint32_t remote_consumer_id)
{
	doca_error_t ret;

	char *buf_addr = io_messages_memory;
	doca_bufs.reserve((datapath_buffer_count * 2) + batch_size);
	host_request_tasks.reserve(datapath_buffer_count + batch_size);
	host_response_tasks.reserve(datapath_buffer_count);
	storage_request_tasks.reserve(datapath_buffer_count + batch_size);
	storage_request_tasks.reserve(datapath_buffer_count);

	for (uint32_t ii = 0; ii != datapath_buffer_count; ++ii) {
		doca_buf *storage_request_buff = nullptr;

		ret = doca_buf_inventory_buf_get_by_addr(buf_inv,
							 io_messages_mmap,
							 buf_addr,
							 io_message_buffer_size,
							 &storage_request_buff);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get io message doca_buf"};
		}

		buf_addr += io_message_buffer_size;
		doca_bufs.push_back(storage_request_buff);

		doca_buf *storage_recv_buf = nullptr;

		ret = doca_buf_inventory_buf_get_by_addr(buf_inv,
							 io_messages_mmap,
							 buf_addr,
							 io_message_buffer_size,
							 &storage_recv_buf);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get io message doca_buf"};
		}
		buf_addr += io_message_buffer_size;
		doca_bufs.push_back(storage_recv_buf);

		doca_rdma_task_receive *rdma_task_receive = nullptr;
		ret = doca_rdma_task_receive_allocate_init(rdma_ctrl_ctx.rdma,
							   storage_recv_buf,
							   doca_data{.ptr = nullptr},
							   &rdma_task_receive);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate rdma doca_rdma_task_receive: "s +
						 doca_error_get_name(ret)};
		}
		storage_response_tasks.push_back(rdma_task_receive);

		doca_rdma_task_send *rdma_task_send = nullptr;
		ret = doca_rdma_task_send_allocate_init(rdma_ctrl_ctx.rdma,
							rdma_ctrl_ctx.conn,
							storage_request_buff,
							doca_data{.ptr = nullptr},
							&rdma_task_send);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate rdma doca_rdma_task_send: "s +
						 doca_error_get_name(ret)};
		}
		storage_request_tasks.push_back(rdma_task_send);

		doca_comch_consumer_task_post_recv *comch_consumer_task_post_recv = nullptr;
		ret = doca_comch_consumer_task_post_recv_alloc_init(consumer,
								    storage_request_buff,
								    &comch_consumer_task_post_recv);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for producer task"};
		}
		host_request_tasks.push_back(comch_consumer_task_post_recv);

		doca_comch_producer_task_send *comch_producer_task_send;
		ret = doca_comch_producer_task_send_alloc_init(producer,
							       storage_recv_buf,
							       nullptr,
							       0,
							       remote_consumer_id,
							       &comch_producer_task_send);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for producer task"};
		}
		host_response_tasks.push_back(comch_producer_task_send);

		/* link task pair - comch recv <-> rdma send */
		static_cast<void>(doca_task_set_user_data(
			doca_comch_consumer_task_post_recv_as_task(comch_consumer_task_post_recv),
			doca_data{.ptr = doca_rdma_task_send_as_task(rdma_task_send)}));
		static_cast<void>(doca_task_set_user_data(doca_rdma_task_send_as_task(rdma_task_send),
							  doca_data{.ptr = comch_consumer_task_post_recv}));

		/* link task pair - rdma recv <-> comch send */
		static_cast<void>(
			doca_task_set_user_data(doca_comch_producer_task_send_as_task(comch_producer_task_send),
						doca_data{.ptr = rdma_task_receive}));
		static_cast<void>(doca_task_set_user_data(
			doca_rdma_task_receive_as_task(rdma_task_receive),
			doca_data{.ptr = doca_comch_producer_task_send_as_task(comch_producer_task_send)}));
	}
	for (uint32_t ii = 0; ii != batch_size; ++ii) {
		doca_buf *storage_request_buff = nullptr;

		ret = doca_buf_inventory_buf_get_by_addr(buf_inv,
							 io_messages_mmap,
							 buf_addr,
							 io_message_buffer_size,
							 &storage_request_buff);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get io message doca_buf"};
		}

		buf_addr += io_message_buffer_size;
		doca_bufs.push_back(storage_request_buff);

		doca_rdma_task_send *rdma_task_send = nullptr;
		ret = doca_rdma_task_send_allocate_init(rdma_ctrl_ctx.rdma,
							rdma_ctrl_ctx.conn,
							storage_request_buff,
							doca_data{.ptr = nullptr},
							&rdma_task_send);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to allocate rdma doca_rdma_task_send: "s +
						 doca_error_get_name(ret)};
		}
		storage_request_tasks.push_back(rdma_task_send);

		doca_comch_consumer_task_post_recv *comch_consumer_task_post_recv = nullptr;
		ret = doca_comch_consumer_task_post_recv_alloc_init(consumer,
								    storage_request_buff,
								    &comch_consumer_task_post_recv);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for producer task"};
		}
		host_request_tasks.push_back(comch_consumer_task_post_recv);

		/* link task pair - comch recv <-> rdma send */
		static_cast<void>(doca_task_set_user_data(
			doca_comch_consumer_task_post_recv_as_task(comch_consumer_task_post_recv),
			doca_data{.ptr = doca_rdma_task_send_as_task(rdma_task_send)}));
		static_cast<void>(doca_task_set_user_data(doca_rdma_task_send_as_task(rdma_task_send),
							  doca_data{.ptr = comch_consumer_task_post_recv}));
	}

	DOCA_LOG_DBG("Submit recv tasks");

	for (auto *task : host_request_tasks) {
		ret = hot_data->submit_comch_recv_task(task);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to submit initial host request task: "s +
						 doca_error_get_name(ret)};
		}
	}

	for (auto *task : storage_response_tasks) {
		ret = doca_task_submit(doca_rdma_task_receive_as_task(task));
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to submit initial storage response task: "s +
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
 * Join the work thread
 */
void thread_context::join(void)
{
	if (thread.joinable()) {
		thread.join();
	}
}

/*
 * Create a RDMA context
 *
 * @dev [in]: Device to use
 * @role [in]: Role of the connection
 * @pe [in]: Progress engine to use
 */
doca_rdma *thread_context::create_rdma_context(doca_dev *dev, rdma_connection_role role, doca_pe *pe)
{
	doca_error_t ret;
	auto *const rdma = storage::make_rdma_context(dev,
						      pe,
						      doca_data{.ptr = hot_data},
						      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
							      DOCA_ACCESS_FLAG_RDMA_WRITE);

	if (role == rdma_connection_role::ctrl) {
		ret = doca_rdma_task_receive_set_conf(rdma,
						      doca_rdma_task_receive_cb,
						      doca_rdma_task_receive_error_cb,
						      datapath_buffer_count);
		if (ret != DOCA_SUCCESS) {
			static_cast<void>(doca_rdma_destroy(rdma));
			throw std::runtime_error{"Failed to configure rdma receive task pool: "s +
						 doca_error_get_name(ret)};
		}

		ret = doca_rdma_task_send_set_conf(rdma,
						   doca_rdma_task_send_cb,
						   doca_rdma_task_send_error_cb,
						   datapath_buffer_count + batch_size);
		if (ret != DOCA_SUCCESS) {
			static_cast<void>(doca_rdma_destroy(rdma));
			throw std::runtime_error{"Failed to configure rdma send task pool: "s +
						 doca_error_get_name(ret)};
		}
	}

	ret = doca_ctx_start(doca_rdma_as_ctx(rdma));
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_rdma_destroy(rdma));
		throw std::runtime_error{"Failed to start doca_rdma context: "s + doca_error_get_name(ret)};
	}

	return rdma;
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
	while (hot_data->wait_flag && hot_data->encountered_errors == false) {
		std::this_thread::yield();
	}

	DOCA_LOG_INFO("T[%u] running", thread_id);
	while (hot_data->running_flag) {
		doca_pe_progress(pe) ? ++(hot_data->pe_hit_count) : ++(hot_data->pe_miss_count);
	}

	while (hot_data->in_flight_ops != 0) {
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
		thread_proc(self->thread_id, self->hot_data, self->data_pe);
	} catch (std::runtime_error const &ex) {
		self->hot_data->encountered_errors = true;
		DOCA_LOG_ERR("T[%u] encountered an error: %s", self->thread_id, ex.what());
	}

	if (self->hot_data->encountered_errors) {
		DOCA_LOG_ERR("T[%u] failed", self->thread_id);
	}
}

/*
 * Destructor
 */
comch_to_rdma_application_impl::~comch_to_rdma_application_impl()
{
	doca_error_t ret;

	m_thread_contexts.clear();

	if (m_host_mmap != nullptr) {
		ret = doca_mmap_destroy(m_host_mmap);
		if (ret == DOCA_SUCCESS) {
			m_host_mmap = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy doca_mmap: %s", doca_error_get_name(ret));
		}
	}

	if (m_comch_server != nullptr) {
		ret = storage::stop_context(doca_comch_server_as_ctx(m_comch_server), m_ctrl_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_comch_server: %s", doca_error_get_name(ret));
		}

		ret = doca_comch_server_destroy(m_comch_server);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_comch_server: %s", doca_error_get_name(ret));
		}
	}

	if (m_ctrl_pe != nullptr) {
		ret = doca_pe_destroy(m_ctrl_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_name(ret));
		}
	}

	if (m_dev_rep != nullptr) {
		ret = doca_dev_rep_close(m_dev_rep);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca_dev_rep: %s", doca_error_get_name(ret));
		}
	}

	if (m_dev != nullptr) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca_dev: %s", doca_error_get_name(ret));
		}
	}

	try {
		m_storage_connection.close();
	} catch (std::runtime_error const &ex) {
		DOCA_LOG_ERR("Failed to close storage socket: %s", ex.what());
	}
}

/*
 * Constructor
 *
 * @cfg [in]: Configuration
 */
comch_to_rdma_application_impl::comch_to_rdma_application_impl(
	storage::zero_copy::comch_to_rdma_application::configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_dev_rep{nullptr},
	  m_ctrl_pe{nullptr},
	  m_storage_connection{},
	  m_comch_server{nullptr},
	  m_client_connection{nullptr},
	  m_host_mmap{nullptr},
	  m_mmap_export_blob{},
	  m_thread_contexts{},
	  m_ctrl_msgs{},
	  m_tcp_rx_buffer{},
	  m_control_message_reassembler{},
	  m_stats{},
	  m_remote_consumer_ids{},
	  m_datapath_buffer_count{0},
	  m_datapath_buffer_size{0},
	  m_datapath_batch_size{0},
	  m_abort_flag{false}
{
	m_ctrl_msgs.reserve(storage::max_concurrent_control_messages);
}

/*
 * Run the application
 */
void comch_to_rdma_application_impl::run(void)
{
	doca_error_t ret;
	uint32_t err_correlation_id = 0;

	DOCA_LOG_INFO("Open doca_dev: %s", m_cfg.device_id.c_str());
	m_dev = storage::open_device(m_cfg.device_id);

	DOCA_LOG_INFO("Open doca_dev_rep: %s", m_cfg.representor_id.c_str());
	m_dev_rep = storage::open_representor(m_dev, m_cfg.representor_id);

	DOCA_LOG_DBG("Create control progress engine");
	ret = doca_pe_create(&m_ctrl_pe);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_pe: "s + doca_error_get_name(ret)};
	}

	connect_storage_server();

	create_comch_server();

	/* Wait for client to connect */
	while (m_client_connection == nullptr) {
		static_cast<void>(doca_pe_progress(m_ctrl_pe));

		if (m_abort_flag)
			return;
	}

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

		/* forward request to storage backend */
		send_storage_control_message(make_configure_data_path_control_message(msg.correlation_id,
										      configuration.buffer_count,
										      configuration.buffer_size,
										      configuration.batch_size,
										      m_mmap_export_blob));
		auto response = wait_for_control_message();
		if (response.correlation_id != msg.correlation_id) {
			DOCA_LOG_ERR("Received %s message while waiting for configure_storage response: %u",
				     to_string(msg).c_str(),
				     msg.correlation_id);
			throw std::runtime_error{
				"Received unexpected control message while awaiting configure_storage response"};
		}

		send_host_control_message(make_response_control_message(msg.correlation_id));

	} catch (std::runtime_error const &ex) {
		abort("Failed during configure storage process");
		send_host_control_message(
			make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
		throw;
	}

	try {
		auto const msg = wait_for_control_message();
		err_correlation_id = msg.correlation_id;
		if (msg.type != control_message_type::start_data_path_connections) {
			DOCA_LOG_ERR("Received %s message while waiting for create_comch_objects_request",
				     to_string(msg).c_str());
			throw std::runtime_error{
				"Received unexpected message while waiting for create_comch_objects_request message"};
		}

		/* Create all connection related objects */
		DOCA_LOG_INFO("Preparing %zu storage contexts", m_cfg.cpu_set.size());
		m_thread_contexts.reserve(m_cfg.cpu_set.size());
		for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
			prepare_storage_context(ii, msg.correlation_id);
		}

		/* Notify storage that all rdma connections has been requested */
		send_storage_control_message(make_start_data_path_connections_control_message(msg.correlation_id));

		/* Wait for storage to confirm everything looks good as well */
		auto const response = wait_for_control_message();
		if (response.correlation_id != msg.correlation_id || response.type != control_message_type::response) {
			DOCA_LOG_ERR("Received %s message while waiting for establish_rdma_connections response: %u",
				     to_string(response).c_str(),
				     msg.correlation_id);
			throw std::runtime_error{
				"Received unexpected message while waiting for establish_rdma_connections response"};
		}

		auto const &response_details = dynamic_cast<control_message::response const &>(*response.details);
		if (response_details.result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Storage failed to establish all rdma connections: %s:%s",
				     doca_error_get_name(response_details.result),
				     response_details.message.c_str());
			throw std::runtime_error{"Storage failed to establish all rdma connections"};
		}

		/* Notify the host that everything is created and connected */
		send_host_control_message(make_response_control_message(msg.correlation_id));
	} catch (std::runtime_error const &ex) {
		abort("Failed during connection establishment process");
		send_host_control_message(
			make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
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

		/* Wait for local connections to show as ready */
		wait_for_connections_to_establish();

		/* Forward request to storage */
		send_storage_control_message(make_start_storage_control_message(msg.correlation_id));

		/* create worker threads */
		for (uint32_t ii = 0; ii != m_thread_contexts.size(); ++ii) {
			m_thread_contexts[ii]->thread = std::thread{&thread_proc_catch_wrapper, m_thread_contexts[ii]};
			try {
				storage::set_thread_affinity(m_thread_contexts[ii]->thread,
							     m_cfg.cpu_set[m_thread_contexts[ii]->thread_id]);
			} catch (std::exception const &) {
				m_thread_contexts[ii]->hot_data->abort(
					"Failed to set affinity for thread to core: "s +
					std::to_string(m_cfg.cpu_set[m_thread_contexts[ii]->thread_id]));
				m_thread_contexts[ii]->hot_data->running_flag = false;
				m_thread_contexts[ii]->hot_data->wait_flag = false;
				throw;
			}
			m_thread_contexts[ii]->allocate_and_submit_tasks(m_remote_consumer_ids[ii]);
		}

		auto response = wait_for_control_message();
		if (response.correlation_id != msg.correlation_id) {
			DOCA_LOG_ERR("Received %s message while waiting for start_storage response: %u",
				     to_string(msg).c_str(),
				     msg.correlation_id);
			throw std::runtime_error{
				"Received unexpected control message while awaiting start_storage response"};
		}

		/* start worker threads */
		for (auto &thread_context : m_thread_contexts) {
			thread_context->hot_data->wait_flag = false;
		}

		/* Notify ready to run */
		send_host_control_message(make_response_control_message(msg.correlation_id));
	} catch (std::runtime_error const &ex) {
		abort("Failed during start storage establishment process");
		send_host_control_message(
			make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
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

		/* Forward request to storage */
		send_storage_control_message(make_destroy_objects_control_message(msg.correlation_id));

		auto response = wait_for_control_message();
		if (response.correlation_id != msg.correlation_id) {
			DOCA_LOG_ERR("Received %s message while waiting for destroy_objects response: %u",
				     to_string(msg).c_str(),
				     msg.correlation_id);
			throw std::runtime_error{
				"Received unexpected control message while awaiting destroy_objects response"};
		}

		destroy_objects();
		send_host_control_message(make_response_control_message(msg.correlation_id));
	} catch (std::runtime_error const &ex) {
		send_host_control_message(
			make_response_control_message(err_correlation_id, DOCA_ERROR_UNKNOWN, ex.what()));
		throw;
	}
}

/*
 * Abort execution
 *
 * @reason [in]: The reason
 */
void comch_to_rdma_application_impl::abort(const std::string &reason)
{
	m_abort_flag = true;
	for (auto &thread_context : m_thread_contexts) {
		thread_context->abort(reason);
	}
}

/*
 * Get end of run statistics
 *
 * @return: Statistics
 */
std::vector<storage::zero_copy::comch_to_rdma_application::thread_stats> comch_to_rdma_application_impl::get_stats(
	void) const
{
	return m_stats;
}

/*
 * Store a control message so it can be processed later
 *
 * @msg [in]: The message to store
 */
void comch_to_rdma_application_impl::store_control_message_response(storage::zero_copy::control_message msg) noexcept
{
	if (m_ctrl_msgs.capacity() == 0) {
		abort("No storage available for new control message");
		return;
	}

	m_ctrl_msgs.push_back(std::move(msg));
}

/*
 * Client connection callback
 *
 * @conn [in]: The new connection
 */
void comch_to_rdma_application_impl::on_client_connected(doca_comch_connection *conn) noexcept
{
	if (m_client_connection != nullptr) {
		DOCA_LOG_WARN("Ignoring unexpected additional comch connection");
		return;
	}

	m_client_connection = conn;
	static_cast<void>(doca_comch_connection_set_user_data(conn, doca_data{.ptr = this}));
}

/*
 * Client disconnection callback
 *
 * @conn [in]: The disconnected connection
 */
void comch_to_rdma_application_impl::on_client_disconnected(doca_comch_connection *conn) noexcept
{
	if (m_client_connection != conn) {
		DOCA_LOG_WARN("Ignoring disconnect of non-connected connection");
		return;
	}

	m_client_connection = nullptr;
	m_remote_consumer_ids.clear();
}

/*
 * Consumer connection callback
 *
 * @consumer_id [in]: ID of the connected consumer
 */
void comch_to_rdma_application_impl::on_consumer_connected(uint32_t consumer_id) noexcept
{
	auto found = std::find(std::begin(m_remote_consumer_ids), std::end(m_remote_consumer_ids), consumer_id);
	if (found == std::end(m_remote_consumer_ids)) {
		m_remote_consumer_ids.push_back(consumer_id);
	} else {
		DOCA_LOG_WARN("Ignoring duplicate consumer id: %u", consumer_id);
	}
}

/*
 * Consumer disconnection callback
 *
 * @consumer_id [in]: ID of the disconnected consumer
 */
void comch_to_rdma_application_impl::on_consumer_disconnected(uint32_t consumer_id) noexcept
{
	auto found = std::find(std::begin(m_remote_consumer_ids), std::end(m_remote_consumer_ids), consumer_id);
	if (found != std::end(m_remote_consumer_ids)) {
		m_remote_consumer_ids.erase(found);
	} else {
		DOCA_LOG_WARN("Ignoring remove of non registered consumer id: %u", consumer_id);
	}
}

/*
 * Stop all data threads
 */
void comch_to_rdma_application_impl::stop_all_threads(void)
{
	for (auto &thread_context : m_thread_contexts) {
		thread_context->hot_data->running_flag = false;
	}
}

/*
 * Wait for a control message (polling until it arrives)
 *
 * @throws std::runtime_error: Unable to retrieve message
 *
 * @return: received control message
 */
storage::zero_copy::control_message comch_to_rdma_application_impl::wait_for_control_message(void)
{
	control_message msg;

	for (;;) {
		if (!m_ctrl_msgs.empty()) {
			msg = std::move(m_ctrl_msgs[0]);
			m_ctrl_msgs.erase(m_ctrl_msgs.begin());
			break;
		}

		static_cast<void>(doca_pe_progress(m_ctrl_pe));
		for (auto &thread_context : m_thread_contexts) {
			if (thread_context->data_pe)
				static_cast<void>(doca_pe_progress(thread_context->data_pe));
		}

		if (m_control_message_reassembler.append(m_tcp_rx_buffer.data(),
							 m_storage_connection.read(m_tcp_rx_buffer.data(),
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
 * Send a control message to the host side
 *
 * @throws std::runtime_error: Unable to send message
 *
 * @message [in]: Message to send
 */
void comch_to_rdma_application_impl::send_host_control_message(storage::zero_copy::control_message message)
{
	doca_error_t ret;
	std::array<char, control_message_buffer_size> buffer{};
	auto msg_size = to_buffer(message, buffer.data(), buffer.size());
	if (msg_size == 0) {
		throw std::runtime_error{"Failed to format io_start message"};
	}

	doca_comch_task_send *task;
	ret = doca_comch_server_task_send_alloc_init(m_comch_server,
						     m_client_connection,
						     buffer.data(),
						     msg_size,
						     &task);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to allocate comch task to send io_start message: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_task_submit(doca_comch_task_send_as_task(task));
	if (ret != DOCA_SUCCESS) {
		doca_task_free(doca_comch_task_send_as_task(task));
		throw std::runtime_error{"Failed to send io_start message: "s + doca_error_get_name(ret)};
	}
}

/*
 * Send a control message to the storage side
 *
 * @throws std::runtime_error: Unable to send message
 *
 * @message [in]: Message to send
 */
void comch_to_rdma_application_impl::send_storage_control_message(storage::zero_copy::control_message message)
{
	std::array<char, control_message_buffer_size> buffer{};
	auto msg_size = to_buffer(message, buffer.data(), buffer.size());
	if (msg_size == 0) {
		throw std::runtime_error{"Failed to format io_start message"};
	}

	if (m_storage_connection.write(buffer.data(), msg_size) != msg_size) {
		throw std::runtime_error{"Failed to send start_storage to storage server"};
	}
}

/*
 * Configure storage
 *
 * @throws std::runtime_error: An error occurred
 *
 * @request [in]: Storage configuration
 */
void comch_to_rdma_application_impl::configure_storage(
	storage::zero_copy::control_message::configure_data_path const &request)
{
	m_datapath_buffer_count = request.buffer_count;
	m_datapath_buffer_size = request.buffer_size;
	m_datapath_batch_size = request.batch_size;
	m_host_mmap = storage::make_mmap(m_dev, request.mmap_export_blob.data(), request.mmap_export_blob.size());
	m_mmap_export_blob = [this]() {
		uint8_t const *reexport_blob = nullptr;
		size_t reexport_blob_size = 0;
		auto const ret = doca_mmap_export_rdma(m_host_mmap,
						       m_dev,
						       reinterpret_cast<void const **>(&reexport_blob),
						       &reexport_blob_size);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to re-export host mmap for rdma"};
		}

		return std::vector<uint8_t>{reexport_blob, reexport_blob + reexport_blob_size};
	}();
}

/*
 * Connect to storage server
 *
 * @throws std::runtime_error: An error occurred
 */
void comch_to_rdma_application_impl::connect_storage_server(void)
{
	DOCA_LOG_DBG("Connect to storage server %s:%u",
		     m_cfg.storage_server_address.get_address().c_str(),
		     m_cfg.storage_server_address.get_port());
	auto const expiry_time = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	storage::tcp_socket socket;
	socket.connect(m_cfg.storage_server_address);
	while (!m_abort_flag) {
		switch (socket.poll_is_connected()) {
		case storage::tcp_socket::connection_status::connected:
			DOCA_LOG_INFO("Connected to storage service %s:%u",
				      m_cfg.storage_server_address.get_address().c_str(),
				      m_cfg.storage_server_address.get_port());
			m_storage_connection = std::move(socket);
			return;
		case storage::tcp_socket::connection_status::establishing:
			break;
		case storage::tcp_socket::connection_status::refused:
			socket = storage::tcp_socket{}; /* reset the socket */
			socket.connect(m_cfg.storage_server_address);
			break;
		case storage::tcp_socket::connection_status::failed:
			throw std::runtime_error{"Unable to connect via TCP to \"" +
						 m_cfg.storage_server_address.get_address() +
						 "\":" + std::to_string(m_cfg.storage_server_address.get_port())};
		}

		std::this_thread::sleep_for(std::chrono::milliseconds{500});
		if (std::chrono::steady_clock::now() > expiry_time)
			throw std::runtime_error{
				"Timed out trying to connect to: " + m_cfg.storage_server_address.get_address() + ":" +
				std::to_string(m_cfg.storage_server_address.get_port())};
	}

	throw std::runtime_error{"Connection aborted"};
}

/*
 * Wait for host client connection
 *
 * @throws std::runtime_error: An error occurred
 */
void comch_to_rdma_application_impl::create_comch_server(void)
{
	doca_error_t ret;
	ret = doca_comch_server_create(m_dev, m_dev_rep, m_cfg.command_channel_name.c_str(), &m_comch_server);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_comch_server: "s + doca_error_get_name(ret)};
	}

	auto *comch_ctx = doca_comch_server_as_ctx(m_comch_server);

	ret = doca_pe_connect_ctx(m_ctrl_pe, comch_ctx);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to connect doca_comch_server with doca_pe: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_server_task_send_set_conf(m_comch_server,
						   doca_comch_task_send_cb,
						   doca_comch_task_send_error_cb,
						   storage::max_concurrent_control_messages);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to configure doca_comch_server send task pool: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_server_event_msg_recv_register(m_comch_server, doca_comch_event_msg_recv_cb);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to configure doca_comch_server receive task callback: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_server_event_connection_status_changed_register(m_comch_server,
									 doca_comch_event_connection_connected_cb,
									 doca_comch_event_connection_disconnected_cb);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to configure doca_comch_server connection callbacks: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_ctx_set_user_data(comch_ctx, doca_data{.ptr = this});
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to set doca_comch_server user data: "s + doca_error_get_name(ret)};
	}

	ret = doca_comch_server_event_consumer_register(m_comch_server,
							doca_comch_event_consumer_connected_cb,
							doca_comch_event_consumer_expired_cb);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to register for doca_comch_client consumer registration events: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_ctx_start(comch_ctx);
	if (ret != DOCA_ERROR_IN_PROGRESS && ret != DOCA_SUCCESS) {
		throw std::runtime_error{"[application::application] Failed to start doca_comch_server: "s +
					 doca_error_get_name(ret)};
	}
}

/*
 * Prepare storage context
 *
 * @thread_id [in]: ID of the thread to create
 * @correlation_id [in]: Correlation id to use for requests and responses to storage server
 * @batch_size [in]: Batch size
 */
void comch_to_rdma_application_impl::prepare_storage_context(uint32_t thread_id, uint32_t correlation_id)
{
	auto tctx = std::make_shared<thread_context>(this,
						     thread_id,
						     m_ctrl_pe,
						     m_datapath_buffer_count,
						     m_datapath_buffer_size,
						     m_datapath_batch_size);
	tctx->create_objects(m_dev, m_client_connection);
	connect_rdma(thread_id, rdma_connection_role::ctrl, correlation_id, tctx->rdma_ctrl_ctx);
	connect_rdma(thread_id, rdma_connection_role::data, correlation_id, tctx->rdma_data_ctx);
	m_thread_contexts.push_back(std::move(tctx));
}

/*
 * Create RDMA connection
 *
 * @thread_id [in]: ID of the thread
 * @role [in]: RDMA role
 * @correlation_id [in]: Correlation id to use for requests and responses to storage server
 * @rdma_conn_pair [in/out]: Connection pair with existing RDMA context to populate the connection part
 */
void comch_to_rdma_application_impl::connect_rdma(uint32_t thread_id,
						  rdma_connection_role role,
						  uint32_t correlation_id,
						  storage::rdma_conn_pair &rdma_conn_pair)
{
	doca_error_t ret;
	uint8_t const *blob = nullptr;
	size_t blob_size = 0;

	ret = doca_rdma_export(rdma_conn_pair.rdma,
			       reinterpret_cast<void const **>(&blob),
			       &blob_size,
			       std::addressof(rdma_conn_pair.conn));
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to export rdma connection: "s + doca_error_get_name(ret)};
	}

	send_storage_control_message(
		make_create_rdma_connection_request_control_message(correlation_id,
								    role,
								    std::vector<uint8_t>{blob, blob + blob_size}));
	auto const response = wait_for_control_message();
	if (response.correlation_id != correlation_id) {
		DOCA_LOG_ERR("T[%u] Received %s message while waiting for create_rdma_connection_response: %u",
			     thread_id,
			     to_string(response).c_str(),
			     correlation_id);
		throw std::runtime_error{
			"Received unexpected message while waiting for create_rdma_connection_response"};
	}

	if (response.type == control_message_type::response) {
		auto const *details = dynamic_cast<control_message::response const *>(response.details.get());
		if (details != nullptr) {
			DOCA_LOG_ERR("T[%u] Storage failed to create rdma connection: %s:%s",
				     thread_id,
				     doca_error_get_name(details->result),
				     details->message.c_str());
		}
		throw std::runtime_error{"Storage failed to create rdma connection"};
	}

	auto const *details =
		dynamic_cast<control_message::create_rdma_connection_response const *>(response.details.get());
	if (details == nullptr || response.type != control_message_type::create_rdma_connection_response) {
		DOCA_LOG_ERR("T[%u] Received %s message while waiting for create_rdma_connection_response: %u",
			     thread_id,
			     to_string(response).c_str(),
			     correlation_id);
		throw std::runtime_error{
			"Received unexpected message while waiting for create_rdma_connection_response"};
	}

	ret = doca_rdma_connect(rdma_conn_pair.rdma,
				details->connection_details.data(),
				details->connection_details.size(),
				rdma_conn_pair.conn);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("T[%u] RDMA connect failed: %s", thread_id, doca_error_get_name(ret));
		throw std::runtime_error{"Failed to connect to rdma"};
	}
}

/*
 * Wait for all RDMA and ComCh connection to be fully connected and ready for data path operations
 */
void comch_to_rdma_application_impl::wait_for_connections_to_establish(void)
{
	doca_error_t ret;
	uint32_t pending_rdma_connection_count;

	for (;;) {
		pending_rdma_connection_count = 0;
		static_cast<void>(doca_pe_progress(m_ctrl_pe));
		for (auto &thread_context : m_thread_contexts) {
			static_cast<void>(doca_pe_progress(thread_context->data_pe));

			doca_ctx_states rdma_state;

			ret = doca_ctx_get_state(doca_rdma_as_ctx(thread_context->rdma_ctrl_ctx.rdma), &rdma_state);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(ret));
				throw std::runtime_error{"Failed to query rdma context state"};
			} else if (rdma_state != DOCA_CTX_STATE_RUNNING) {
				++pending_rdma_connection_count;
			}

			ret = doca_ctx_get_state(doca_rdma_as_ctx(thread_context->rdma_data_ctx.rdma), &rdma_state);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(ret));
				throw std::runtime_error{"Failed to query rdma context state"};
			} else if (rdma_state != DOCA_CTX_STATE_RUNNING) {
				++pending_rdma_connection_count;
			}
		}

		if (pending_rdma_connection_count == 0 && m_remote_consumer_ids.size() == m_thread_contexts.size()) {
			DOCA_LOG_INFO("All connections established");
			break;
		}

		if (m_abort_flag) {
			throw std::runtime_error{"Aborted waiting for rdma and comch connections to establish"};
		}
	}
}

/*
 * Tear down data path objects
 */
void comch_to_rdma_application_impl::destroy_objects(void)
{
	m_stats.reserve(m_thread_contexts.size());
	std::transform(std::begin(m_thread_contexts),
		       std::end(m_thread_contexts),
		       std::back_inserter(m_stats),
		       [](auto const &thread_context) {
			       return thread_stats{
				       thread_context->hot_data->pe_hit_count,
				       thread_context->hot_data->pe_miss_count,
			       };
		       });

	/* Just destroy the fast path objects for now */
	m_thread_contexts.clear();
}

} /* namespace */

/*
 * Create a DPU application instance
 *
 * @throws std::bad_alloc if memory allocation fails
 * @throws std::runtime_error if any other error occurs
 *
 * @cfg [in]: Application configuration
 * @return: Application instance
 */
storage::zero_copy::comch_to_rdma_application *make_comch_to_rdma_application(
	storage::zero_copy::comch_to_rdma_application::configuration const &cfg)
{
	return new comch_to_rdma_application_impl{cfg};
}

} /* namespace storage::zero_copy */
