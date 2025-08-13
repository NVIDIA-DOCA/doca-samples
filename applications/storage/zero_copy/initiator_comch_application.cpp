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

#include <zero_copy/initiator_comch_application.hpp>

#include <algorithm>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <thread>
#include <limits>
#include <memory>
#include <vector>

#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <storage_common/aligned_new.hpp>
#include <storage_common/doca_utils.hpp>
#include <storage_common/os_utils.hpp>

#include <zero_copy/control_message.hpp>
#include <zero_copy/io_message.hpp>

DOCA_LOG_REGISTER(HOST_APPLICATION);

using namespace std::string_literals;

namespace storage::zero_copy {
namespace {

using timestamp = std::chrono::steady_clock::time_point;
static_assert(sizeof(std::chrono::steady_clock::time_point) == 8,
	      "Expected std::chrono::steady_clock::time_point to occupy 8 bytes");

/*
 * Data that needs to be tracked per transaction
 */
struct transaction_context {
	doca_comch_producer_task_send *request = nullptr;
	timestamp start_time{};
	/*
	 * doca_pe does not guarantee the order of responses so this field is used to know when both the request and
	 * response completions have been received to allow it to be re-submitted
	 */
	uint16_t refcount = 0;
};

static_assert(sizeof(transaction_context) == 24, "Expected transaction_context to occupy 24 bytes");

/*
 * A set of data that can be used in the datapath, NO OTHER MEMORY SHOULD BE ACCESSED in the main loop or task
 * callbacks. This is done to keep the maximum amount of useful data resident in the cache while avoiding as many cache
 * evictions as possible.
 */
struct alignas(storage::cache_line_size) thread_hot_data {
	uint32_t remaining_rx_ops;
	uint32_t remaining_tx_ops;
	uint32_t latency_min;
	uint32_t latency_max;
	uint64_t latency_accumulator;
	uint64_t pe_hit_count;
	uint64_t pe_miss_count;
	doca_pe *data_pe;
	transaction_context *transactions;
	uint32_t transactions_size;
	uint8_t batch_count;
	uint8_t batch_size;
	std::atomic_bool run_flag;
	bool errors_encountered;

	/*
	 * Destructor
	 */
	~thread_hot_data();

	/*
	 * Deleted default constructor
	 */
	thread_hot_data() = delete;

	/*
	 * Constructor
	 *
	 * @cfg [in]: Configuration
	 */
	explicit thread_hot_data(initiator_comch_application::configuration const &cfg);

	/*
	 * Deleted copy constructor
	 */
	thread_hot_data(thread_hot_data const &) = delete;

	/*
	 * Deleted move constructor
	 */
	thread_hot_data(thread_hot_data &&) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	thread_hot_data &operator=(thread_hot_data const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	thread_hot_data &operator=(thread_hot_data &&) noexcept = delete;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(std::string const &reason);

	/*
	 * ComCh post recv helper to control batch submission flags
	 *
	 * @task [in]: Task to submit
	 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	doca_error_t submit_recv_task(doca_task *task);

	/*
	 * Start a transaction
	 *
	 * @transaction [in]: Transaction to start
	 * @now [in]: The current time (to measure duration)
	 */
	void start_transaction(transaction_context &transaction, timestamp now);

	/*
	 * Process transaction completion
	 *
	 * @transaction [in]: The completed transaction
	 */
	void on_transaction_complete(transaction_context &transaction);

	/*
	 * Run a test which does not verify the correctness of the data transfers to measure peak speed
	 *
	 * This test will submit all transactions_size and immediately re-submit any completed transaction until the
	 * user specified run limit of operations have been submitted and completed.
	 */
	void non_validated_test(void);

	/*
	 * Run a test which validates data transfers, ensures correctness at the cost of speed.
	 *
	 * This test will run in multiple rounds where each round contains up to transactions_size operations until the
	 * user specified run limit has been reached. Each round will:
	 *  - Set the buffer context to a known fixed pattern
	 *  - Write that data to storage
	 *  - Set the buffer context to a new different fixed pattern
	 *  - Read the data back from storage
	 *  - Validate the value now in the local buffers match the original fixed pattern
	 */
	void validated_test(void);

private:
	/*
	 * Set the value of all data buffers to match one of the fixed_data_patterns based on the current iteration
	 * index
	 *
	 * @iteration [in]: Index to use to select which fixed_data pattern to set in the buffers content
	 */
	void set_initial_data_content(uint32_t iteration);

	/*
	 * Set the operation value of all transactions. Can be used to switch between read and write
	 *
	 * @operation [in]: The operation type to set
	 */
	void set_operation(io_message_type operation);

	/*
	 * Modify the content of all buffers to hold a value that is not equal to the value set by
	 * set_initial_data_content so that it can be verified to be overwritten by the read back phase to ensure the
	 * values held in storage are the ones that were sent and the data isn't simply stale
	 *
	 * @iteration [in]: Index to use to select which fixed_data pattern to set in the buffers content so that it is
	 * now different
	 */
	void modify_data_content(uint32_t iteration);

	/*
	 * Ensure all data in the buffers matches that set by set_initial_data_content
	 *
	 * @iteration [in]: Index to use to select which fixed_data pattern to compare with the buffer content
	 * @valid_task_count [in]: Number of tasks used this round that should be checked
	 * @return: true if all data was valid false otherwise
	 */
	bool validate_data(uint32_t iteration, uint32_t valid_task_count);
};

std::array<uint8_t, 4> fixed_data_patterns{
	0x13,
	0xAA,
	0x55,
	0xEE,
};

static_assert(sizeof(std::atomic_bool) == 1, "Expected atomic bool to occupy 1 byte");
static_assert(sizeof(thread_hot_data) == storage::cache_line_size, "Expected hot_data_context to occupy 1 cache line");
static_assert(std::alignment_of<thread_hot_data>::value == storage::cache_line_size,
	      "Expected hot_data_context to be cache aligned");

struct thread_context {
	thread_hot_data hot_context;
	char *raw_io_messages;
	doca_mmap *io_message_mmap;
	doca_buf_inventory *io_message_inv;
	doca_comch_consumer *consumer;
	doca_comch_producer *producer;
	std::vector<doca_buf *> io_message_bufs;
	std::vector<doca_task *> io_responses;
	std::vector<doca_task *> io_requests;
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
	 * @cfg [in]: Configuration
	 * @dev [in]: Device to use
	 * @comch_conn [in]: Connection to ComCh server
	 */
	thread_context(initiator_comch_application::configuration const &cfg,
		       doca_dev *dev,
		       doca_comch_connection *comch_conn);

	/*
	 * Deleted copy constructor
	 */
	thread_context(thread_context const &) = delete;

	/*
	 * Deleted move constructor
	 */
	thread_context(thread_context &&other) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	thread_context &operator=(thread_context const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	thread_context &operator=(thread_context &&other) noexcept = delete;

	/*
	 * Create tasks
	 *
	 * @io_memory_base [in]: Start address of memory that will be read or written to
	 * @io_buffer_size [in]: Size of a data transfer
	 * @remote_consumer_idx [in]: Id of the remote consumer to send tasks to
	 * @op_type [in]: The type of operation to use (read or write)
	 * @batch_size [in]: Batch size
	 */
	void create_tasks(char *const io_memory_base,
			  uint32_t io_buffer_size,
			  uint32_t remote_consumer_idx,
			  io_message_type op_type,
			  uint32_t batch_size);

	/*
	 * Delete the comch consumer and producer if they exist
	 */
	void destroy_consumer_and_producer(void);
};

class initiator_comch_application_impl : public storage::zero_copy::initiator_comch_application {
public:
	/*
	 * Deleted default constructor
	 */
	~initiator_comch_application_impl() override;

	/*
	 * Deleted default constructor
	 */
	initiator_comch_application_impl() = delete;

	/*
	 * constructor
	 *
	 * @cfg [in]: Configuration
	 */
	explicit initiator_comch_application_impl(initiator_comch_application::configuration const &cfg);

	/*
	 * Deleted copy constructor
	 */
	initiator_comch_application_impl(initiator_comch_application_impl const &) = delete;

	/*
	 * Deleted move constructor
	 */
	initiator_comch_application_impl(initiator_comch_application_impl &&) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	initiator_comch_application_impl &operator=(initiator_comch_application_impl const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	initiator_comch_application_impl &operator=(initiator_comch_application_impl &&) noexcept = delete;

	/*
	 * Run the application
	 *
	 * @return: true on success and false otherwise
	 */
	bool run(void) override;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	void abort(std::string const &reason) override;

	/*
	 * Get end of run statistics
	 *
	 * @return: Statistics
	 */
	storage::zero_copy::initiator_comch_application::stats get_stats(void) const noexcept override;

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
	void on_consumer_expired(uint32_t consumer_id) noexcept;

	/*
	 * Store a control message so it can be processed later
	 *
	 * @msg [in]: The message to store
	 */
	void store_control_message_response(storage::zero_copy::control_message msg);

private:
	initiator_comch_application::configuration m_cfg;
	char *m_raw_io_data;
	doca_dev *m_dev;
	doca_mmap *m_io_data_mmap;
	thread_context *m_thread_contexts;
	doca_pe *m_ctrl_pe;
	doca_comch_client *m_comch_client;
	doca_comch_connection *m_comch_conn;
	std::vector<uint32_t> m_remote_consumer_ids;
	std::vector<zero_copy::control_message> m_ctrl_msg_responses;
	timestamp m_start_time;
	timestamp m_end_time;
	uint32_t m_correlation_id;
	bool m_abort_flag;

	/*
	 * Send a control message to the DPU. Only meant to be used for messages that contain no data, otherwise use the
	 * alternative form
	 *
	 * @type [in]: Message type
	 * @return: Generated correlation ID
	 */
	uint32_t send_control_message(storage::zero_copy::control_message_type type);

	/*
	 * Send a control message to the DPU.
	 * alternative form
	 *
	 * @message [in]: Message to send
	 */
	void send_control_message(storage::zero_copy::control_message const &message);

	/*
	 * Wait for a control message (polling until it arrives)
	 *
	 * @throws std::runtime_error: Unable to retrieve message
	 *
	 * @correlation_id [in]: Correlation ID of message to wait for
	 * @return: received control message
	 */
	storage::zero_copy::control_message wait_for_control_response(uint32_t correlation_id);

	/*
	 * Create ComCh control context
	 *
	 * @throws std::runtime_error: Unable create ComCh control
	 */
	void create_comch_control(void);

	/*
	 * Connect to ComCh server (DPU)
	 *
	 * @throws std::runtime_error: Unable connect to ComCh control
	 */
	void connect_comch_control(void);

	/*
	 * Configure storage, locally, on the DPU and remotely
	 *
	 * @throws std::runtime_error: Failed to configure storage
	 */
	void configure_storage(void);

	/*
	 * Prepare data path objects
	 *
	 * @throws std::runtime_error: Failed to prepare fast path objects
	 */
	void prepare_data_path(void);

	/*
	 * Stop storage
	 *
	 * @throws std::runtime_error: Failed to stop storage
	 */
	void stop_storage(void);
};

/*
 *
 */
void doca_comch_task_send_cb(doca_comch_task_send *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);
	static_cast<void>(ctx_user_data);

	doca_task_free(doca_comch_task_send_as_task(task));
}

void doca_comch_task_send_error_cb(doca_comch_task_send *task,
				   doca_data task_user_data,
				   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	doca_task_free(doca_comch_task_send_as_task(task));
	static_cast<initiator_comch_application_impl *>(ctx_user_data.ptr)->abort("Failed to send ComCh send task");
}

void doca_comch_event_msg_recv_cb(doca_comch_event_msg_recv *event,
				  uint8_t *recv_buffer,
				  uint32_t msg_len,
				  doca_comch_connection *conn) noexcept
{
	static_cast<void>(event);

	storage::zero_copy::control_message msg{};
	auto const read_bytes = from_buffer(reinterpret_cast<char const *>(recv_buffer), msg_len, msg);
	if (read_bytes == 0) {
		DOCA_LOG_ERR("Failed to extract control message");
		return;
	}

	static_cast<initiator_comch_application_impl *>(doca_comch_connection_get_user_data(conn).ptr)
		->store_control_message_response(std::move(msg));
}

void doca_comch_event_consumer_connected_cb(doca_comch_event_consumer *event,
					    doca_comch_connection *conn,
					    uint32_t id) noexcept
{
	static_cast<void>(event);

	doca_data user_data{.ptr = nullptr};
	auto *const client = doca_comch_client_get_client_ctx(conn);
	auto const ret = doca_ctx_get_user_data(doca_comch_client_as_ctx(client), &user_data);
	if (ret != DOCA_SUCCESS || user_data.ptr == nullptr) {
		DOCA_LOG_ERR("[BUG] unable to extract user data");
		return;
	}

	static_cast<initiator_comch_application_impl *>(user_data.ptr)->on_consumer_connected(id);
}

void doca_comch_event_consumer_expired_cb(doca_comch_event_consumer *event,
					  doca_comch_connection *conn,
					  uint32_t id) noexcept
{
	static_cast<void>(event);
	static_cast<void>(id);

	doca_data user_data{.ptr = nullptr};

	auto *const client = doca_comch_client_get_client_ctx(conn);
	auto const ret = doca_ctx_get_user_data(doca_comch_client_as_ctx(client), &user_data);
	if (ret != DOCA_SUCCESS || user_data.ptr == nullptr) {
		DOCA_LOG_ERR("[BUG] unable to extract user data");
		return;
	}
	static_cast<initiator_comch_application_impl *>(user_data.ptr)->on_consumer_expired(id);
}

void doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
					   doca_data task_user_data,
					   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const hctx = static_cast<thread_hot_data *>(ctx_user_data.ptr);
	char *io_message;
	auto *buf = doca_comch_consumer_task_post_recv_get_buf(task);
	static_cast<void>(doca_buf_get_data(buf, reinterpret_cast<void **>(&io_message)));
	auto const correlation_id = io_message_view::get_correlation_id(io_message);
	if (correlation_id > hctx->transactions_size) {
		hctx->abort("Received storage response with invalid async id");
		return;
	}

	if (--(hctx->transactions[correlation_id].refcount) == 0)
		hctx->on_transaction_complete(hctx->transactions[correlation_id]);

	doca_buf_reset_data_len(doca_comch_consumer_task_post_recv_get_buf(task));
	auto const ret = hctx->submit_recv_task(doca_comch_consumer_task_post_recv_as_task(task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to resubmit doca_comch_consumer_task_post_recv: %s", doca_error_get_name(ret));
		hctx->abort("Failed to resubmit ComCh consumer receive task");
	}
}

void doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
						 doca_data task_user_data,
						 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hctx = static_cast<thread_hot_data *>(ctx_user_data.ptr);

	hctx->abort("Failed to complete ComCh consumer recv task");
}

void doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
				      doca_data task_user_data,
				      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto &transaction = *static_cast<transaction_context *>(task_user_data.ptr);
	if (--(transaction.refcount) == 0)
		static_cast<thread_hot_data *>(ctx_user_data.ptr)->on_transaction_complete(transaction);
}

void doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
					    doca_data task_user_data,
					    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	static_cast<thread_hot_data *>(ctx_user_data.ptr)->abort("Failed to complete ComCh producer send task");
}

/*
 * Destructor
 */
thread_hot_data::~thread_hot_data()
{
	if (transactions) {
		for (uint32_t ii = 0; ii != transactions_size; ++ii) {
			transactions[ii].~transaction_context();
		}
		free(transactions);
		transactions = nullptr;
	}

	if (data_pe) {
		DOCA_LOG_DBG("Destroy doca_pe(%p)", data_pe);
		auto const ret = doca_pe_destroy(data_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_pe: %s", doca_error_get_name(ret));
		}
	}
}

/*
 * Constructor
 *
 * @cfg [in]: Configuration
 */
thread_hot_data::thread_hot_data(initiator_comch_application::configuration const &cfg)
	: remaining_rx_ops{cfg.run_limit_operation_count / static_cast<uint32_t>(cfg.cpu_set.size())},
	  remaining_tx_ops{cfg.run_limit_operation_count / static_cast<uint32_t>(cfg.cpu_set.size())},
	  latency_min{std::numeric_limits<uint32_t>::max()},
	  latency_max{0},
	  latency_accumulator{0},
	  pe_hit_count{0},
	  pe_miss_count{0},
	  data_pe{nullptr},
	  transactions{nullptr},
	  transactions_size{0},
	  batch_count{static_cast<uint8_t>(cfg.batch_size)},
	  batch_size{0},
	  run_flag{false},
	  errors_encountered{false}
{
}

/*
 * Abort execution
 *
 * @reason [in]: The reason
 */
void thread_hot_data::abort(std::string const &reason)
{
	errors_encountered = true;
	if (run_flag) {
		run_flag = false;
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
doca_error_t thread_hot_data::submit_recv_task(doca_task *task)
{
	doca_task_submit_flag submit_flag = DOCA_TASK_SUBMIT_FLAG_NONE;
	if (--batch_count == 0) {
		submit_flag = DOCA_TASK_SUBMIT_FLAG_FLUSH;
		batch_count = batch_size;
	}

	return doca_task_submit_ex(task, submit_flag);
}

/*
 * Start a transaction
 *
 * @transaction [in]: Transaction to start
 * @now [in]: The current time (to measure duration)
 */
void thread_hot_data::start_transaction(transaction_context &transaction, timestamp now)
{
	/* set the transaction refcount to 2 as the order of completions between the receive callback and the send
	 * callback are not guaranteed to be ordered. The task cannot be re-used until both callbacks have completed.
	 */
	transaction.refcount = 2;
	transaction.start_time = now;
	doca_error_t ret;

	do {
		ret = doca_task_submit(doca_comch_producer_task_send_as_task(transaction.request));
	} while (ret == DOCA_ERROR_AGAIN);

	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit comch producer send task: %s", doca_error_get_name(ret));
		abort("Failed to submit comch producer send task");
	}

	--remaining_tx_ops;
}

/*
 * Process transaction completion
 *
 * @transaction [in]: The completed transaction
 */
void thread_hot_data::on_transaction_complete(transaction_context &transaction)
{
	auto const now = std::chrono::steady_clock::now();
	auto const usecs = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(now - transaction.start_time).count());
	latency_accumulator += usecs;
	latency_min = std::min(latency_min, usecs);
	latency_max = std::max(latency_max, usecs);

	--remaining_rx_ops;
	if (remaining_tx_ops) {
		start_transaction(transaction, now);
	} else if (remaining_rx_ops == 0) {
		run_flag = false;
	}
}

/*
 * Run a test which does not verify the correctness of the data transfers to measure peak speed
 *
 * This test will submit all transactions_size and immediately re-submit any completed transaction until the
 * user specified run limit of operations have been submitted and completed.
 */
void thread_hot_data::non_validated_test(void)
{
	pe_hit_count = 0;
	pe_miss_count = 0;

	/* wait to start */
	while (run_flag == false)
		std::this_thread::yield();

	if (errors_encountered) {
		DOCA_LOG_ERR("Thread %p aborted", this);
		return;
	}

	/* submit initial tasks */
	auto const initial_task_count = std::min(transactions_size, remaining_tx_ops);
	for (uint32_t ii = 0; ii != initial_task_count; ++ii)
		start_transaction(transactions[ii], std::chrono::steady_clock::now());

	/* run until the test completes */
	while (run_flag) {
		doca_pe_progress(data_pe) ? ++(pe_hit_count) : ++(pe_miss_count);
	}

	/* exit if anything went wrong */
	if (errors_encountered) {
		DOCA_LOG_ERR("Thread %p failed", this);
		return;
	}

	/* wait for any completions that are out-standing in the case of a user abort (control+C) */
	remaining_rx_ops = remaining_rx_ops - remaining_tx_ops;
	while (remaining_rx_ops != 0) {
		doca_pe_progress(data_pe) ? ++(pe_hit_count) : ++(pe_miss_count);
	}
}

/*
 * Run a test which validates data transfers, ensures correctness at the cost of speed.
 *
 * This test will run in multiple rounds where each round contains up to transactions_size operations until the
 * user specified run limit has been reached. Each round will:
 *  - Set the buffer context to a known fixed pattern
 *  - Write that data to storage
 *  - Set the buffer context to a new different fixed pattern
 *  - Read the data back from storage
 *  - Validate the value now in the local buffers match the original fixed pattern
 */
void thread_hot_data::validated_test(void)
{
	pe_hit_count = 0;
	pe_miss_count = 0;

	/* wait to start */
	while (run_flag == false)
		std::this_thread::yield();

	if (errors_encountered) {
		DOCA_LOG_ERR("Thread %p aborted", this);
		return;
	}

	/* Calculate number of rounds */
	uint32_t const iteration_count =
		(remaining_tx_ops / transactions_size) + ((remaining_tx_ops % transactions_size) == 0 ? 0 : 1);
	uint32_t overall_remaining_tasks = remaining_tx_ops;

	for (uint32_t iteration = 0; iteration != iteration_count; ++iteration) {
		/* do write phase */
		set_initial_data_content(iteration);
		set_operation(io_message_type::write);

		auto const round_task_count = std::min(overall_remaining_tasks, transactions_size);
		overall_remaining_tasks -= round_task_count;

		remaining_tx_ops = round_task_count;
		remaining_rx_ops = round_task_count;

		run_flag = true;
		for (uint32_t ii = 0; ii != round_task_count; ++ii)
			start_transaction(transactions[ii], std::chrono::steady_clock::now());

		while (run_flag) {
			doca_pe_progress(data_pe) ? ++(pe_hit_count) : ++(pe_miss_count);
		}

		if (errors_encountered || remaining_rx_ops != 0) {
			break;
		}

		/* do read phase */
		modify_data_content(iteration);
		set_operation(io_message_type::read);

		remaining_tx_ops = round_task_count;
		remaining_rx_ops = round_task_count;

		run_flag = true;
		for (uint32_t ii = 0; ii != round_task_count; ++ii)
			start_transaction(transactions[ii], std::chrono::steady_clock::now());

		while (run_flag) {
			doca_pe_progress(data_pe) ? ++(pe_hit_count) : ++(pe_miss_count);
		}

		if (errors_encountered || remaining_rx_ops != 0) {
			break;
		}

		if (!validate_data(iteration, round_task_count)) {
			errors_encountered = true;
			break;
		}
	}

	/* exit if anything went wrong */
	if (errors_encountered) {
		DOCA_LOG_ERR("Thread %p failed", this);
		return;
	} else {
		DOCA_LOG_INFO("All data transfers verified successfully");
	}

	/* wait for any completions that are out-standing in the case of a user abort (control+C) */
	remaining_rx_ops = remaining_rx_ops - remaining_tx_ops;
	while (remaining_rx_ops != 0) {
		doca_pe_progress(data_pe) ? ++(pe_hit_count) : ++(pe_miss_count);
	}
}

/*
 * Set the value of all data buffers to match one of the fixed_data_patterns based on the current iteration
 * index
 *
 * @iteration [in]: Index to use to select which fixed_data pattern to set in the buffers content
 */
void thread_hot_data::set_initial_data_content(uint32_t iteration)
{
	auto const pattern = fixed_data_patterns[iteration % fixed_data_patterns.size()];

	for (uint32_t ii = 0; ii != transactions_size; ++ii) {
		auto *io_message =
			storage::get_buffer_bytes(doca_comch_producer_task_send_get_buf(transactions[ii].request));
		auto *data_addr = reinterpret_cast<uint8_t *>(io_message_view::get_io_address(io_message));
		auto data_size = io_message_view::get_io_size(io_message);
		auto *const data_end = data_addr + data_size;

		for (; data_addr != data_end; ++data_addr)
			*data_addr = pattern;
	}
}

/*
 * Set the operation value of all transactions. Can be used to switch between read and write
 *
 * @operation [in]: The operation type to set
 */
void thread_hot_data::set_operation(io_message_type operation)
{
	for (uint32_t ii = 0; ii != transactions_size; ++ii) {
		auto *io_message = const_cast<char *>(
			storage::get_buffer_bytes(doca_comch_producer_task_send_get_buf(transactions[ii].request)));
		io_message_view::set_type(operation, io_message);
	}
}

/*
 * Modify the content of all buffers to hold a value that is not equal to the value set by
 * set_initial_data_content so that it can be verified to be overwritten by the read back phase to ensure the
 * values held in storage are the ones that were sent and the data isn't simply stale
 *
 * @iteration [in]: Index to use to select which fixed_data pattern to set in the buffers content so that it is
 * now different
 */
void thread_hot_data::modify_data_content(uint32_t iteration)
{
	auto const pattern = fixed_data_patterns[(iteration + 1) % fixed_data_patterns.size()];

	for (uint32_t ii = 0; ii != transactions_size; ++ii) {
		auto *io_message =
			storage::get_buffer_bytes(doca_comch_producer_task_send_get_buf(transactions[ii].request));
		auto *data_addr = reinterpret_cast<uint8_t *>(io_message_view::get_io_address(io_message));
		auto data_size = io_message_view::get_io_size(io_message);
		auto *const data_end = data_addr + data_size;

		for (; data_addr != data_end; ++data_addr)
			*data_addr = pattern;
	}
}

/*
 * Ensure all data in the buffers matches that set by set_initial_data_content
 *
 * @iteration [in]: Index to use to select which fixed_data pattern to compare with the buffer content
 * @valid_task_count [in]: Number of tasks used this round that should be checked
 * @return: true if all data was valid false otherwise
 */
bool thread_hot_data::validate_data(uint32_t iteration, uint32_t valid_task_count)
{
	auto const pattern = fixed_data_patterns[iteration % fixed_data_patterns.size()];

	bool is_valid_data = true;

	for (uint32_t ii = 0; ii != valid_task_count; ++ii) {
		auto *io_message =
			storage::get_buffer_bytes(doca_comch_producer_task_send_get_buf(transactions[ii].request));
		auto *data_addr = reinterpret_cast<uint8_t *>(io_message_view::get_io_address(io_message));
		auto data_size = io_message_view::get_io_size(io_message);
		auto *const data_end = data_addr + data_size;

		for (; data_addr != data_end; ++data_addr)
			if (*data_addr != pattern) {
				is_valid_data = false;
				break;
			}
	}

	return is_valid_data;
}

void thread_context::destroy_consumer_and_producer(void)
{
	doca_error_t ret;

	if (producer) {
		DOCA_LOG_DBG("Stop doca_comch_producer(%p)", producer);
		ret = storage::stop_context(doca_comch_producer_as_ctx(producer), hot_context.data_pe, io_requests);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_comch_producer: %s", doca_error_get_name(ret));
		}

		io_requests.clear();
		DOCA_LOG_DBG("Destroy doca_comch_producer(%p)", producer);
		ret = doca_comch_producer_destroy(producer);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_comch_producer: %s", doca_error_get_name(ret));
		} else {
			producer = nullptr;
		}
	}

	if (consumer) {
		DOCA_LOG_DBG("Stop doca_comch_consumer(%p)", consumer);
		ret = storage::stop_context(doca_comch_consumer_as_ctx(consumer), hot_context.data_pe, io_responses);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_comch_consumer: %s", doca_error_get_name(ret));
		}

		io_responses.clear();
		DOCA_LOG_DBG("Destroy doca_comch_consumer(%p)", consumer);
		ret = doca_comch_consumer_destroy(consumer);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_comch_consumer: %s", doca_error_get_name(ret));
		} else {
			consumer = nullptr;
		}
	}
}

/*
 * Destructor
 */
thread_context::~thread_context()
{
	doca_error_t ret;
	hot_context.run_flag = false;
	if (thread.joinable()) {
		thread.join();
	}

	destroy_consumer_and_producer();

	for (auto *buf : io_message_bufs) {
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
	}

	if (io_message_inv) {
		DOCA_LOG_DBG("Destroy doca_buf_inventory(%p)", io_message_inv);
		ret = doca_buf_inventory_destroy(io_message_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_buf_inventory: %s", doca_error_get_name(ret));
		} else {
			io_message_inv = nullptr;
		}
	}

	if (io_message_mmap) {
		DOCA_LOG_DBG("Destroy doca_mmap(%p)", io_message_mmap);
		ret = doca_mmap_destroy(io_message_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_mmap: %s", doca_error_get_name(ret));
		} else {
			io_message_mmap = nullptr;
		}
	}

	free(raw_io_messages);
}

/*
 * Constructor
 *
 * @cfg [in]: Configuration
 * @dev [in]: Device to use
 * @comch_conn [in]: Connection to ComCh server
 */
thread_context::thread_context(initiator_comch_application::configuration const &cfg,
			       doca_dev *dev,
			       doca_comch_connection *comch_conn)
	: hot_context{cfg},
	  raw_io_messages{nullptr},
	  io_message_mmap{nullptr},
	  io_message_inv{nullptr},
	  consumer{nullptr},
	  producer{nullptr},
	  io_message_bufs{},
	  io_responses{},
	  io_requests{},
	  thread{}
{
	doca_error_t ret;

	auto const page_size = storage::get_system_page_size();
	/*
	 * Allocate enough memory for at N tasks + cfg.batch_size where N is the smaller value of:
	 * cfg.buffer_count or cfg.run_limit_operation_count. cfg.batch_size tasks are over-allocated due to how
	 * batched receive tasks work. The requirement is to always be able to have N tasks in flight. While
	 * also lowering the cost of task submission by batching. So because upto cfg.batch_size -1 tasks could
	 * be submitted but not yet flushed means that a surplus of cfg.batch_size tasks are required to
	 * maintain always having N active tasks.
	 */
	auto const tasks_per_thread = std::min(cfg.buffer_count, cfg.run_limit_operation_count);
	hot_context.transactions_size = tasks_per_thread;
	auto const raw_io_messages_size = (tasks_per_thread + cfg.batch_size) * io_message_buffer_size * 2;

	DOCA_LOG_DBG("Allocate comch buffers memory (%lu bytes, aligned to %u byte pages)",
		     raw_io_messages_size,
		     page_size);

	raw_io_messages =
		static_cast<char *>(aligned_alloc(page_size, storage::aligned_size(page_size, raw_io_messages_size)));
	if (raw_io_messages == nullptr) {
		throw std::runtime_error{"Failed to allocate comch fast path buffers memory"};
	}

	try {
		hot_context.transactions =
			storage::make_aligned<transaction_context>{}.object_array(hot_context.transactions_size);
	} catch (std::exception const &ex) {
		throw std::runtime_error{"Failed to allocate transaction contexts memory: "s + ex.what()};
	}

	DOCA_LOG_DBG("Create hot path progress engine");
	ret = doca_pe_create(std::addressof(hot_context.data_pe));
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_pe: "s + doca_error_get_name(ret)};
	}

	io_message_mmap = storage::make_mmap(dev,
					     raw_io_messages,
					     raw_io_messages_size,
					     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);

	producer = storage::make_comch_producer(comch_conn,
						hot_context.data_pe,
						tasks_per_thread,
						doca_data{.ptr = std::addressof(hot_context)},
						doca_comch_producer_task_send_cb,
						doca_comch_producer_task_send_error_cb);
	io_requests.reserve(tasks_per_thread);

	consumer = storage::make_comch_consumer(comch_conn,
						io_message_mmap,
						hot_context.data_pe,
						tasks_per_thread + cfg.batch_size,
						doca_data{.ptr = std::addressof(hot_context)},
						doca_comch_consumer_task_post_recv_cb,
						doca_comch_consumer_task_post_recv_error_cb);
	io_responses.reserve(tasks_per_thread + cfg.batch_size);

	ret = doca_buf_inventory_create((tasks_per_thread * 2) + cfg.batch_size, &io_message_inv);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create comch fast path doca_buf_inventory: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_buf_inventory_start(io_message_inv);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to start comch fast path doca_buf_inventory: "s +
					 doca_error_get_name(ret)};
	}
}

/*
 * Create tasks
 *
 * @io_memory_base [in]: Start address of memory that will be read or written to
 * @io_buffer_size [in]: Size of a data transfer
 * @remote_consumer_idx [in]: Id of the remote consumer to send tasks to
 * @op_type [in]: The type of operation to use (read or write)
 * @batch_size [in]: Batch size
 */
void thread_context::create_tasks(char *const io_memory_base,
				  uint32_t io_buffer_size,
				  uint32_t remote_consumer_idx,
				  io_message_type op_type,
				  uint32_t batch_size)
{
	doca_error_t ret;

	char *task_data_addr = raw_io_messages;
	char *io_addr = io_memory_base;

	for (uint32_t ii = 0; ii != hot_context.transactions_size; ++ii) {
		doca_buf *consumer_buf;
		doca_buf *producer_buf;
		doca_comch_consumer_task_post_recv *consumer_task;
		doca_comch_producer_task_send *producer_task;

		ret = doca_buf_inventory_buf_get_by_addr(io_message_inv,
							 io_message_mmap,
							 task_data_addr,
							 io_message_buffer_size,
							 &consumer_buf);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for consumer task"};
		}
		io_message_bufs.push_back(consumer_buf);

		task_data_addr += io_message_buffer_size;
		auto *const io_message = task_data_addr;
		ret = doca_buf_inventory_buf_get_by_data(io_message_inv,
							 io_message_mmap,
							 task_data_addr,
							 io_message_buffer_size,
							 &producer_buf);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for producer task"};
		}
		io_message_bufs.push_back(producer_buf);

		task_data_addr += io_message_buffer_size;

		ret = doca_comch_consumer_task_post_recv_alloc_init(consumer, consumer_buf, &consumer_task);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for consumer task"};
		}

		io_responses.push_back(doca_comch_consumer_task_post_recv_as_task(consumer_task));

		ret = doca_comch_producer_task_send_alloc_init(producer,
							       producer_buf,
							       nullptr,
							       0,
							       remote_consumer_idx,
							       &producer_task);

		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for producer task"};
		}
		static_cast<void>(
			doca_task_set_user_data(doca_comch_producer_task_send_as_task(producer_task),
						doca_data{.ptr = std::addressof(hot_context.transactions[ii])}));
		io_requests.push_back(doca_comch_producer_task_send_as_task(producer_task));
		hot_context.transactions[ii].refcount = 0;
		hot_context.transactions[ii].request = producer_task;

		io_message_view::set_type(op_type, io_message);
		io_message_view::set_user_data(doca_data{.u64 = remote_consumer_idx}, io_message);
		io_message_view::set_correlation_id(ii, io_message);
		io_message_view::set_io_address(reinterpret_cast<uint64_t>(io_addr), io_message);
		io_message_view::set_io_size(io_buffer_size, io_message);

		io_addr += io_buffer_size;
	}

	/*
	 * Over allocate receive tasks so all while waiting for a full batch of tasks to submit there is always
	 * hot_context.transactions_size active tasks
	 */
	for (uint32_t ii = 0; ii != batch_size; ++ii) {
		doca_buf *consumer_buf;
		doca_comch_consumer_task_post_recv *consumer_task;

		ret = doca_buf_inventory_buf_get_by_addr(io_message_inv,
							 io_message_mmap,
							 task_data_addr,
							 io_message_buffer_size,
							 &consumer_buf);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for consumer task"};
		}
		io_message_bufs.push_back(consumer_buf);

		task_data_addr += io_message_buffer_size;

		ret = doca_comch_consumer_task_post_recv_alloc_init(consumer, consumer_buf, &consumer_task);
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Unable to get doca_buf for consumer task"};
		}

		io_responses.push_back(doca_comch_consumer_task_post_recv_as_task(consumer_task));
	}
}

/*
 * Destructor
 */
initiator_comch_application_impl::~initiator_comch_application_impl()
{
	doca_error_t ret;

	if (m_thread_contexts) {
		for (size_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
			m_thread_contexts[ii].~thread_context();
		}
		free(m_thread_contexts);
	}

	if (m_comch_client) {
		DOCA_LOG_DBG("Stop doca_comch_client(%p)", m_comch_client);
		ret = storage::stop_context(doca_comch_client_as_ctx(m_comch_client), m_ctrl_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_comch_client: %s", doca_error_get_name(ret));
		}

		DOCA_LOG_DBG("Destroy doca_comch_client(%p)", m_comch_client);
		ret = doca_comch_client_destroy(m_comch_client);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_comch_client: %s", doca_error_get_name(ret));
		}
	}

	if (m_ctrl_pe) {
		DOCA_LOG_DBG("Destroy doca_pe(%p)", m_ctrl_pe);
		ret = doca_pe_destroy(m_ctrl_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_pe: %s", doca_error_get_name(ret));
		}
	}

	if (m_io_data_mmap) {
		DOCA_LOG_DBG("Stop doca_mmap(%p)", m_io_data_mmap);
		ret = doca_mmap_stop(m_io_data_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_mmap(%p): %s", m_io_data_mmap, doca_error_get_name(ret));
		}

		DOCA_LOG_DBG("Destroy  doca_mmap(%p)", m_io_data_mmap);
		ret = doca_mmap_destroy(m_io_data_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_mmap: %s", doca_error_get_name(ret));
		}
	}

	if (m_dev) {
		DOCA_LOG_DBG("Close doca_dev(%p)", m_dev);
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca_dev: %s", doca_error_get_name(ret));
		}
	}

	free(m_raw_io_data);
}

/*
 * constructor
 *
 * @cfg [in]: Configuration
 */
initiator_comch_application_impl::initiator_comch_application_impl(initiator_comch_application::configuration const &cfg)
	: m_cfg{cfg},
	  m_raw_io_data{nullptr},
	  m_dev{nullptr},
	  m_io_data_mmap{nullptr},
	  m_thread_contexts{nullptr},
	  m_ctrl_pe{nullptr},
	  m_comch_client{nullptr},
	  m_comch_conn{nullptr},
	  m_remote_consumer_ids{},
	  m_ctrl_msg_responses{},
	  m_start_time{},
	  m_end_time{},
	  m_correlation_id{0},
	  m_abort_flag{false}
{
}

/*
 * Run the application
 *
 * @return: true on success and false otherwise
 */
bool initiator_comch_application_impl::run(void)
{
	doca_error_t ret;

	DOCA_LOG_INFO("Open doca_dev: %s", m_cfg.device_id.c_str());
	m_dev = storage::open_device(m_cfg.device_id);

	DOCA_LOG_DBG("Create control progress engine");
	ret = doca_pe_create(&m_ctrl_pe);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_pe: "s + doca_error_get_name(ret)};
	}

	create_comch_control();

	connect_comch_control();
	if (m_abort_flag)
		return false;

	configure_storage();
	if (m_abort_flag)
		return false;

	prepare_data_path();
	if (m_abort_flag)
		return false;

	io_message_type op_type;
	if (m_cfg.operation_type == "read") {
		op_type = io_message_type::read;
	} else if (m_cfg.operation_type == "write") {
		op_type = io_message_type::write;
	} else {
		throw std::runtime_error{"Unknown operation type: " + m_cfg.operation_type};
	}

	auto const per_cpu_memory_region_size = m_cfg.buffer_count * m_cfg.buffer_size;
	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		m_thread_contexts[ii].create_tasks(m_raw_io_data + (ii * per_cpu_memory_region_size),
						   m_cfg.buffer_size,
						   m_remote_consumer_ids[ii],
						   op_type,
						   m_cfg.batch_size);

		if (op_type == io_message_type::read) {
			m_thread_contexts[ii].thread = std::thread{&thread_hot_data::non_validated_test,
								   std::addressof(m_thread_contexts[ii].hot_context)};
		} else if (op_type == io_message_type::write) {
			if (m_cfg.validate_writes) {
				m_thread_contexts[ii].thread =
					std::thread{&thread_hot_data::validated_test,
						    std::addressof(m_thread_contexts[ii].hot_context)};
			} else {
				m_thread_contexts[ii].thread =
					std::thread{&thread_hot_data::non_validated_test,
						    std::addressof(m_thread_contexts[ii].hot_context)};
			}
		} else {
			throw std::runtime_error{"Unknown operation: \"" + m_cfg.operation_type + "\""};
		}

		try {
			storage::set_thread_affinity(m_thread_contexts[ii].thread, m_cfg.cpu_set[ii]);
		} catch (std::exception const &) {
			m_thread_contexts[ii].hot_context.abort("Failed to set affinity for thread to core: "s +
								std::to_string(m_cfg.cpu_set[ii]));
			m_thread_contexts[ii].hot_context.run_flag = false;
			throw;
		}
	}

	if (m_abort_flag)
		return false;

	wait_for_control_response(send_control_message(control_message_type::start_storage));

	m_start_time = std::chrono::steady_clock::now();
	/* Submit initial receive tasks */
	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		for (auto *rx_task : m_thread_contexts[ii].io_responses) {
			ret = m_thread_contexts[ii].hot_context.submit_recv_task(rx_task);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit comch consumer recv task: %s", doca_error_get_name(ret));
				abort("Failed to submit ComCh consumer receive task");
				break;
			}
		}
	}

	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		m_thread_contexts[ii].hot_context.run_flag = true;
	}

	/* Join worker threads they will exit when the test completes */
	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		m_thread_contexts[ii].thread.join();
	}

	m_end_time = std::chrono::steady_clock::now();

	stop_storage();

	if (m_abort_flag)
		return false;

	/* Teardown client consumer(s) and producer(s) before requesting server teardown */
	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		m_thread_contexts[ii].destroy_consumer_and_producer();
	}

	/* Signal teardown of remote applications */
	wait_for_control_response(send_control_message(control_message_type::destroy_objects));

	return true;
}

/*
 * Abort execution
 *
 * @reason [in]: The reason
 */
void initiator_comch_application_impl::abort(std::string const &reason)
{
	if (!m_abort_flag) {
		m_abort_flag = true;
		DOCA_LOG_ERR("Aborting benchmark: %s", reason.c_str());
		if (m_thread_contexts != nullptr) {
			for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
				m_thread_contexts[ii].hot_context.abort(reason);
			}
		}
	}
}

/*
 * Get end of run statistics
 *
 * @return: Statistics
 */
storage::zero_copy::initiator_comch_application::stats initiator_comch_application_impl::get_stats(void) const noexcept
{
	storage::zero_copy::initiator_comch_application::stats stats{};
	stats.duration = std::chrono::duration_cast<std::chrono::microseconds>(m_end_time - m_start_time);
	stats.operation_count = m_cfg.run_limit_operation_count;
	stats.latency_min = std::numeric_limits<uint32_t>::max();
	uint64_t latency_acc = 0;

	for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
		stats.latency_min = std::min(stats.latency_min, m_thread_contexts[ii].hot_context.latency_min);
		stats.latency_max = std::max(stats.latency_max, m_thread_contexts[ii].hot_context.latency_max);
		latency_acc += m_thread_contexts[ii].hot_context.latency_accumulator;
		stats.pe_hit_count += m_thread_contexts[ii].hot_context.pe_hit_count;
		stats.pe_miss_count += m_thread_contexts[ii].hot_context.pe_miss_count;
	}

	stats.latency_mean = latency_acc / m_cfg.run_limit_operation_count;

	return stats;
}

/*
 * Consumer connection callback
 *
 * @consumer_id [in]: ID of the connected consumer
 */
void initiator_comch_application_impl::on_consumer_connected(uint32_t consumer_id) noexcept
{
	auto found = std::find(std::begin(m_remote_consumer_ids), std::end(m_remote_consumer_ids), consumer_id);
	if (found == std::end(m_remote_consumer_ids)) {
		m_remote_consumer_ids.push_back(consumer_id);
		DOCA_LOG_DBG("Connected to remote consumer with id: %u. Consumer count is now: %zu",
			     consumer_id,
			     m_remote_consumer_ids.size());
	} else {
		DOCA_LOG_WARN("Ignoring duplicate remote consumer id: %u", consumer_id);
	}
}

/*
 * Consumer disconnection callback
 *
 * @consumer_id [in]: ID of the disconnected consumer
 */
void initiator_comch_application_impl::on_consumer_expired(uint32_t consumer_id) noexcept
{
	auto found = std::find(std::begin(m_remote_consumer_ids), std::end(m_remote_consumer_ids), consumer_id);
	if (found != std::end(m_remote_consumer_ids)) {
		m_remote_consumer_ids.erase(found);
		DOCA_LOG_DBG("Disconnected from remote consumer with id: %u. Consumer count is now: %zu",
			     consumer_id,
			     m_remote_consumer_ids.size());
	} else {
		DOCA_LOG_WARN("Ignoring disconnect of unexpected remote consumer id: %u", consumer_id);
	}
}

/*
 * Store a control message so it can be processed later
 *
 * @msg [in]: The message to store
 */
void initiator_comch_application_impl::store_control_message_response(storage::zero_copy::control_message msg)
{
	m_ctrl_msg_responses.push_back(std::move(msg));
}

/*
 * Send a control message to the DPU. Only meant to be used for messages that contain no data, otherwise use the
 * alternative form
 *
 * @type [in]: Message type
 * @return: Generated correlation ID
 */
uint32_t initiator_comch_application_impl::send_control_message(storage::zero_copy::control_message_type type)
{
	storage::zero_copy::control_message msg{};
	msg.type = type;
	msg.correlation_id = ++m_correlation_id;

	send_control_message(msg);

	return msg.correlation_id;
}

/*
 * Send a control message to the DPU.
 * alternative form
 *
 * @message [in]: Message to send
 */
void initiator_comch_application_impl::send_control_message(storage::zero_copy::control_message const &message)
{
	doca_error_t ret;
	std::array<char, control_message_buffer_size> buffer{};
	auto msg_size = to_buffer(message, buffer.data(), buffer.size());
	if (msg_size == 0) {
		throw std::runtime_error{"Failed to format io_start message"};
	}

	doca_comch_task_send *task;
	ret = doca_comch_client_task_send_alloc_init(m_comch_client, m_comch_conn, buffer.data(), msg_size, &task);
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
 * Wait for a control message (polling until it arrives)
 *
 * @throws std::runtime_error: Unable to retrieve message
 *
 * @correlation_id [in]: Correlation ID of message to wait for
 * @return: received control message
 */
storage::zero_copy::control_message initiator_comch_application_impl::wait_for_control_response(uint32_t correlation_id)
{
	auto const expiry_time = std::chrono::steady_clock::now() + m_cfg.control_timeout;
	for (;;) {
		static_cast<void>(doca_pe_progress(m_ctrl_pe));
		if (m_thread_contexts != nullptr) {
			for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
				static_cast<void>(doca_pe_progress(m_thread_contexts->hot_context.data_pe));
			}
		}

		auto found = std::find_if(std::begin(m_ctrl_msg_responses),
					  std::end(m_ctrl_msg_responses),
					  [correlation_id](auto &response) {
						  return response.correlation_id == correlation_id;
					  });

		if (found != std::end(m_ctrl_msg_responses)) {
			auto response = std::move(*found);
			m_ctrl_msg_responses.erase(found);
			return response;
		}

		if (std::chrono::steady_clock::now() > expiry_time) {
			throw std::runtime_error{"Timed out waiting on response from the DPU"};
		}
	}
}

/*
 * Create ComCh control context
 *
 * @throws std::runtime_error: Unable create ComCh control
 */
void initiator_comch_application_impl::create_comch_control(void)
{
	doca_error_t ret;

	DOCA_LOG_DBG("Create coma_comch_client(%s)", m_cfg.command_channel_name.c_str());
	ret = doca_comch_client_create(m_dev, m_cfg.command_channel_name.c_str(), &m_comch_client);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_comch_client: "s + doca_error_get_name(ret)};
	}

	ret = doca_comch_client_task_send_set_conf(m_comch_client,
						   doca_comch_task_send_cb,
						   doca_comch_task_send_error_cb,
						   storage::max_concurrent_control_messages);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to configure doca_comch_client send task pool: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_client_event_msg_recv_register(m_comch_client, doca_comch_event_msg_recv_cb);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to configure doca_comch_client receive task callback: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_client_event_consumer_register(m_comch_client,
							doca_comch_event_consumer_connected_cb,
							doca_comch_event_consumer_expired_cb);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to register for doca_comch_client consumer registration events: "s +
					 doca_error_get_name(ret)};
	}

	auto *comch_ctx = doca_comch_client_as_ctx(m_comch_client);

	ret = doca_ctx_set_user_data(comch_ctx, doca_data{.ptr = this});
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to set doca_comch_client user_data: "s + doca_error_get_name(ret)};
	}

	ret = doca_pe_connect_ctx(m_ctrl_pe, comch_ctx);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to connect doca_comch_client with doca_pe: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_ctx_start(comch_ctx);
	if (ret != DOCA_ERROR_IN_PROGRESS && ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to start doca_comch_client: "s + doca_error_get_name(ret)};
	}
}

/*
 * Connect to ComCh server (DPU)
 *
 * @throws std::runtime_error: Unable connect to ComCh control
 */
void initiator_comch_application_impl::connect_comch_control(void)
{
	auto const abort_time = std::chrono::steady_clock::now() + m_cfg.control_timeout;
	doca_ctx_states cur_state = DOCA_CTX_STATE_IDLE;

	while (!m_abort_flag) {
		static_cast<void>(doca_pe_progress(m_ctrl_pe));
		static_cast<void>(doca_ctx_get_state(doca_comch_client_as_ctx(m_comch_client), &cur_state));
		if (cur_state == DOCA_CTX_STATE_RUNNING) {
			auto const ret = doca_comch_client_get_connection(m_comch_client, &m_comch_conn);
			if (ret != DOCA_SUCCESS) {
				throw std::runtime_error{"Failed to get comch client connection: "s +
							 doca_error_get_name(ret)};
			}
			static_cast<void>(doca_comch_connection_set_user_data(m_comch_conn, doca_data{.ptr = this}));

			DOCA_LOG_DBG("Connected to comch server");
			return;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		if (abort_time < std::chrono::steady_clock::now()) {
			abort("Failed to connect doca_comch_client");
			throw std::runtime_error{"Failed to connect doca_comch_client after " +
						 std::to_string(m_cfg.control_timeout.count()) + " seconds"};
		}
	}

	DOCA_LOG_DBG("Failed to connect to comch server");
}

/*
 * Configure storage, locally, on the DPU and remotely
 *
 * @throws std::runtime_error: Failed to configure storage
 */
void initiator_comch_application_impl::configure_storage(void)
{
	auto const page_size = storage::get_system_page_size();
	auto const memory_region_size = m_cfg.buffer_count * m_cfg.buffer_size * m_cfg.cpu_set.size();
	DOCA_LOG_DBG("Allocate buffers memory (%u bytes, aligned to %u byte pages)",
		     m_cfg.buffer_count * m_cfg.buffer_size,
		     page_size);

	m_raw_io_data =
		static_cast<char *>(aligned_alloc(page_size, storage::aligned_size(page_size, memory_region_size)));
	if (m_raw_io_data == nullptr) {
		throw std::runtime_error{"Failed to allocate buffers memory"};
	}

	auto constexpr io_data_mmap_flags = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE |
					    DOCA_ACCESS_FLAG_RDMA_WRITE | DOCA_ACCESS_FLAG_RDMA_READ;

	m_io_data_mmap = storage::make_mmap(m_dev, m_raw_io_data, memory_region_size, io_data_mmap_flags);

	DOCA_LOG_INFO("Configuring storage using: %u buffers of %u bytes", m_cfg.buffer_count, m_cfg.buffer_size);
	auto const request = make_configure_data_path_control_message(
		++m_correlation_id,
		m_cfg.buffer_count,
		m_cfg.buffer_size,
		m_cfg.batch_size,
		[this]() {
			void const *data;
			size_t len;
			auto const ret = doca_mmap_export_pci(m_io_data_mmap, m_dev, &data, &len);
			if (ret != DOCA_SUCCESS) {
				throw std::runtime_error{"Failed to export mmap: "s + doca_error_get_name(ret)};
			}

			return std::vector<uint8_t>(static_cast<uint8_t const *>(data),
						    static_cast<uint8_t const *>(data) + len);
		}());
	send_control_message(request);

	auto const response_message = wait_for_control_response(request.correlation_id);

	auto &response_details = dynamic_cast<control_message::response const &>(*response_message.details);
	if (response_details.result != DOCA_SUCCESS) {
		throw std::runtime_error{"DPU failed during storage configuration: "s +
					 doca_error_get_name(response_details.result) +
					 ". Message: " + response_details.message};
	}
}

/*
 * Prepare data path objects
 *
 * @throws std::runtime_error: Failed to prepare fast path objects
 */
void initiator_comch_application_impl::prepare_data_path(void)
{
	try {
		m_thread_contexts = storage::make_aligned<thread_context>{}.object_array(m_cfg.cpu_set.size(),
											 m_cfg,
											 m_dev,
											 m_comch_conn);
	} catch (std::exception const &ex) {
		throw std::runtime_error{"Failed to allocate thread contexts: "s + ex.what()};
	}

	m_remote_consumer_ids.reserve(m_cfg.cpu_set.size());
	auto const request = make_start_data_path_connections_control_message(++m_correlation_id);
	send_control_message(request);

	auto const response_message = wait_for_control_response(request.correlation_id);

	if (response_message.type != control_message_type::response) {
		DOCA_LOG_ERR("Received %s while waiting for start_data_path_connections",
			     to_string(response_message).c_str());
		throw std::runtime_error{"Received unexpected response while waiting for start_data_path_connections"};
	}
	auto &response_details = dynamic_cast<control_message::response const &>(*response_message.details);
	if (response_details.result != DOCA_SUCCESS) {
		throw std::runtime_error{"DPU Failed to initialise comch objects: "s +
					 doca_error_get_name(response_details.result) +
					 ". Message: " + response_details.message};
	}

	auto const timeout = std::chrono::steady_clock::now() + m_cfg.control_timeout;
	for (;;) {
		static_cast<void>(doca_pe_progress(m_ctrl_pe));
		uint32_t num_ready_consumers = 0;
		uint32_t num_ready_producers = 0;
		for (uint32_t ii = 0; ii != m_cfg.cpu_set.size(); ++ii) {
			static_cast<void>(doca_pe_progress(m_thread_contexts[ii].hot_context.data_pe));
			num_ready_consumers +=
				storage::is_ctx_running(doca_comch_consumer_as_ctx(m_thread_contexts[ii].consumer));
			num_ready_producers +=
				storage::is_ctx_running(doca_comch_producer_as_ctx(m_thread_contexts[ii].producer));
		}

		if (m_remote_consumer_ids.size() == m_cfg.cpu_set.size() &&
		    num_ready_consumers == m_cfg.cpu_set.size() && num_ready_producers == m_cfg.cpu_set.size()) {
			break;
		}

		if (timeout < std::chrono::steady_clock::now()) {
			DOCA_LOG_ERR(
				"Timed out waiting comch consumers and producers to connect. %zu remote consumer ids received.  %u Consumers connected. %u Producers connected",
				m_remote_consumer_ids.size(),
				num_ready_consumers,
				num_ready_producers);
			throw std::runtime_error{"Timed out waiting for comch consumers and producers to connect"};
		}
	}
}

/*
 * Stop storage
 *
 * @throws std::runtime_error: Failed to stop storage
 */
void initiator_comch_application_impl::stop_storage(void)
{
	doca_error_t ret;

	DOCA_LOG_INFO("Stopping storage backend");

	/* To reach here the thread will have already completed so no concurrency issues to worry about */
	auto &thread_context = m_thread_contexts[0];

	auto &transaction = thread_context.hot_context.transactions[0];
	transaction.refcount = 2;
	auto *const io_message = storage::get_buffer_bytes(
		const_cast<doca_buf *>(doca_comch_producer_task_send_get_buf(transaction.request)));
	io_message_view::set_type(io_message_type::stop, io_message);

	do {
		ret = doca_task_submit(doca_comch_producer_task_send_as_task(transaction.request));
	} while (ret == DOCA_ERROR_AGAIN);

	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit comch producer send task: %s", doca_error_get_name(ret));
		abort("Failed to submit comch producer send task");
	}

	while (transaction.refcount != 0) {
		static_cast<void>(doca_pe_progress(thread_context.hot_context.data_pe));
	}
}

} /* namespace */

/*
 * Create a host application instance
 *
 * @throws std::bad_alloc if memory allocation fails
 * @throws std::runtime_error if any other error occurs
 *
 * @cfg [in]: Application configuration
 * @return: Application instance
 */
initiator_comch_application *make_initiator_comch_application(
	const storage::zero_copy::initiator_comch_application::configuration &cfg)
{
	return new initiator_comch_application_impl{cfg};
}

} /* namespace storage::zero_copy */
