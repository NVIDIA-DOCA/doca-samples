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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <thread>

#include <doca_argp.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_version.h>

#include <storage_common/aligned_new.hpp>
#include <storage_common/buffer_utils.hpp>
#include <storage_common/control_message.hpp>
#include <storage_common/control_channel.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/file_utils.hpp>
#include <storage_common/io_message.hpp>
#include <storage_common/os_utils.hpp>
#include <storage_common/doca_utils.hpp>

DOCA_LOG_REGISTER(INITIATOR_COMCH);

using namespace std::string_literals;

namespace {

auto constexpr app_name = "doca_storage_initiator_comch";

auto constexpr run_type_read_throughput_test = "read_throughput_test";
auto constexpr run_type_write_throughput_test = "write_throughput_test";
auto constexpr run_type_read_write_data_validity_test = "read_write_data_validity_test";
auto constexpr run_type_read_only_data_validity_test = "read_only_data_validity_test";

auto constexpr default_control_timeout_seconds = std::chrono::seconds{5};
auto constexpr default_task_count = 64;
auto constexpr default_command_channel_name = "doca_storage_comch";
auto constexpr default_run_limit_operation_count = 1'000'000;

static_assert(sizeof(void *) == 8, "Expected a pointer to occupy 8 bytes");
static_assert(sizeof(std::chrono::steady_clock::time_point) == 8,
	      "Expected std::chrono::steady_clock::time_point to occupy 8 bytes");

template <typename T>
struct ring_buffer {
	T *elements;
	uint32_t write_idx;
	uint32_t read_idx;
	uint32_t stored;
	uint32_t capacity;

	~ring_buffer()
	{
		delete[] elements;
	}

	ring_buffer() : elements{nullptr}, write_idx{0}, read_idx{0}, stored{0}, capacity{0}
	{
	}
	ring_buffer(ring_buffer const &) = delete;
	ring_buffer(ring_buffer &&) = delete;
	ring_buffer &operator=(ring_buffer const &) = delete;
	ring_buffer &operator=(ring_buffer &&) = delete;

	void init_ring(uint32_t capacity_)
	{
		elements = new T[capacity_];
		capacity = capacity_;
	}

	bool put(T value) noexcept
	{
		if (stored == capacity) {
			return false;
		}

		elements[write_idx] = value;
		++stored;
		write_idx = (write_idx + 1) % capacity;
		return true;
	}

	bool get(T &value) noexcept
	{
		if (stored == 0) {
			return false;
		}

		value = elements[read_idx];
		--stored;
		read_idx = (read_idx + 1) % capacity;
		return true;
	}

	bool is_full() const noexcept
	{
		return stored == capacity;
	}

	uint32_t stored_count() const noexcept
	{
		return stored;
	}

	uint32_t free_count() const noexcept
	{
		return capacity - stored;
	}
};

static_assert(sizeof(ring_buffer<uint64_t>) == 24, "Expected size of ring_buffer<uint64_t> to be 24 bytes");

/*
 * User configurable parameters for the initiator_comch_app
 */
struct initiator_comch_app_configuration {
	/* List of cores ids to use when launching threads. Each thread will have its affinity set to the corresponding
	 * core id in this list. */
	std::vector<uint32_t> core_list = {};
	/* Identifier for the DOCA device to use. One of: PCI address, infiniband name or interface name. */
	std::string device_id = {};
	/* Give the name of the ComCh server to connect to. The default (see default_command_channel_name) is usually
	 * fine, but a value can be provided to distinguish between ComCh servers on the same DPU. */
	std::string command_channel_name = {};
	/* Path to a file whose contents can be compared to the data read from the storage target to ensure the correct
	 * data is present in the storage target. Should only be used for read only validation run mode. */
	std::string storage_plain_content_file = {};
	/* Name of the execution strategy to execute. One of: run_type_read_throughput_test,
	 * run_type_write_throughput_test, run_type_read_write_data_validity_test or
	 * run_type_read_only_data_validity_test. */
	std::string run_type = {};
	/* A timeout value so that if for some reason a control operation takes too long the test can fail instead of
	 * hanging forever. Typically, nobody needs to modify this. */
	std::chrono::seconds control_timeout = {};
	/* Size of the local IO region. 0 means auto (match storage capacity) otherwise allocate a local storage of this
	 * size. If this value is not a multiple of the storage block size then any part that is less than one block
	 * will be ignored. */
	uint64_t local_io_region_size = 0;
	/* Number of parallel tasks (transactions) that may be in flight (per worker / thread). */
	uint32_t task_count = 0;
	/* Number of transactions to execute before finishing (each worker executes this many transactions, it is not
	 * shared across workers). */
	uint32_t run_limit_operation_count = 0;
};

/*
 * Statistics emitted by the application
 */
struct initiator_comch_app_stats {
	/* Timestamp for the start of the data path / hot path operations */
	std::chrono::steady_clock::time_point start_time = {};
	/* Timestamp when the last transaction completed (max of the values reported per worker)  */
	std::chrono::steady_clock::time_point end_time = {};
	/* Cumulative number of times a progress engine was polled and had one or more completed tasks to report */
	uint64_t pe_hit_count = 0;
	/* Cumulative number of times a progress engine was polled and had no completed tasks to report. The higher the
	 * ratio of misses to hits the more time the application is idle waiting for IO operations to complete. */
	uint64_t pe_miss_count = 0;
	/* Cumulative total of operations completed across all workers. Ie if each of 3 workers completed 100
	 * operations, this value would be 300. */
	uint64_t operation_count = 0;
	/* Minimum recorded latency value across all workers .*/
	uint32_t latency_min = 0;
	/* Maximum recorded latency value across all workers. */
	uint32_t latency_max = 0;
	/* Mean (average) recorded latency value across all workers. */
	uint32_t latency_mean = 0;
};

/*
 * Data that needs to be tracked per transaction
 */
struct alignas(storage::cache_line_size) transaction_context {
	/* The initial task that started the transaction */
	doca_comch_producer_task_send *request = nullptr;
	/* The start time of the transaction */
	std::chrono::steady_clock::time_point start_time{};
	/* The offset within the workers local IO range this transaction is using */
	uint64_t locaL_offset;
	/* The offset within the storage IO range this transaction is using */
	uint64_t storage_offset;
	/* A bit mask tracking the operations left for this transaction. A simplified state machine */
	uint32_t pending_actions = 0;
	/* A reference count, a transaction requires both the send and receive task to have their respective completion
	 * callbacks triggered before the transaction can be re-used
	 */
	uint16_t refcount = 0;
};

static_assert(sizeof(transaction_context) == storage::cache_line_size,
	      "Expected transaction_context to occupy one cache line");

enum class transaction_action : uint32_t {
	/**************************************************************************************************************
	 * General actions - Actual IO commands
	 */
	/* Read data from storage to initiator. */
	read = uint32_t{1} << 0,
	/* Write data from initiator to storage. */
	write = uint32_t{1} << 1,

	/**************************************************************************************************************
	 * Data manipulations - actions performed on local memory that is not an IO operation
	 */
	/* Select the addresses to use for this IO operation. */
	select_addresses = uint32_t{1} << 27,
	/* Initialise memory content with the "expected value". Can be from a file or random bytes. */
	copy_expected_memory_content = uint32_t{1} << 28,
	/* Clear the memory content so that the current value in initiator memory should NOT match what is in held by
	 * the storage target */
	clear_memory_content = uint32_t{1} << 29,
	/* Verify that the memory content in initiator memory matches the "expected value" */
	validate_memory_content = uint32_t{1} << 30,
	/* Finish the transaction */
	finish_transaction = uint32_t{1} << 31,
};

/*
 * Data required for a thread worker
 */
class initiator_comch_worker {
public:
	/*
	 * A set of data that can be used in the data path, NO OTHER MEMORY SHOULD BE ACCESSED in the main loop or task
	 * callbacks. This is done to keep the maximum amount of useful data resident in the cache while avoiding as
	 * many cache evictions as possible.
	 */
	struct alignas(storage::cache_line_size) hot_data {
		/* A copy of the content that is expected to be stored in the storage target. Used to validate the
		 * result of a read operation. */
		uint8_t const *expected_storage_content = nullptr;
		/* Pointer to the beginning of the local memory region from which data is read / written. */
		uint8_t *local_io_region_begin = nullptr;
		/* Virtual offset to the beginning of the storage region from which data is read / written. */
		uint64_t storage_io_region_begin = 0;
		/* Per worker progress engine. Used to monitor for task completions. */
		doca_pe *pe = nullptr;
		/* An Array of transactions_size transaction objects. */
		transaction_context *transactions = nullptr;
		/* A pool of addresses within the local IO region */
		ring_buffer<uint64_t> local_io_addr_pool;
		/* A pool of addresses within the storage IO region */
		ring_buffer<uint64_t> storage_io_addr_pool;
		/* Number of objects in the transactions array pointed to by transactions */
		uint32_t transactions_size = 0;
		/* Size of an IO data transfer */
		uint32_t io_block_size = 0;
		/* Timestamp for when the last transaction completes to avoid counting thread destruction time in the
		 * execution time. */
		std::chrono::steady_clock::time_point end_time = {};
		/* Counter of how many times the process engine was invoked and had at least one completed task to
		 * report */
		uint64_t pe_hit_count = 0;
		/* Counter of how many times the process engine was invoked and had no completed tasks to report. The
		 * higher the ratio of misses to hits the more time the worker is idle waiting for IO operations to
		 * complete. */
		uint64_t pe_miss_count = 0;
		/* Accumulated total latency of all transactions executed. */
		std::chrono::microseconds latency_accumulator = std::chrono::microseconds{0};
		/* Smallest recorded latency value. */
		uint32_t latency_min = std::numeric_limits<uint32_t>::max();
		/* Maximum recorded latency value. */
		uint32_t latency_max = 0;
		/* Target total number of transactions to execute before finishing. */
		uint32_t target_transaction_count = 0;
		/* Number of transactions that have been started. */
		uint32_t started_transaction_count = 0;
		/* Number of transactions that have completed so far. */
		uint32_t completed_transaction_count = 0;
		/* A bitmask to apply to a transactions pending_actions bitmask. Bits are defined by the run type. */
		uint32_t transaction_initial_ops_value = 0;
		/* Flag to indicate if the worker thread is currently running. Setting this to false while the thread is
		 * running will signal the tread to stop. */
		std::atomic_bool run_flag = false;
		/* Flag to indicate if the thread encountered any errors during execution */
		bool error_flag = false;

		/*
		 * Perform the next operation as required by the transaction
		 *
		 * @transaction [in]: Transaction to advance
		 *
		 * @return true if the next OP can be started immediately, or false if it's an asynchronous activity
		 * that will trigger a callback later to make more progress
		 */
		bool progress_transaction(transaction_context &transaction);

		/*
		 * Display the data mismatch
		 *
		 * @local_offset [in]: Offset within local IO region
		 * @storage_offset [in]: Offset within storage IO region
		 * @io_size [in]: IO size
		 */
		void display_memory_mismatch(uint32_t local_offset, uint32_t storage_offset, uint32_t io_size) const;
	};
	static_assert(sizeof(initiator_comch_worker::hot_data) == (3 * storage::cache_line_size),
		      "Expected initiator_comch_worker::hot_data to occupy three cache lines");

	using thread_proc_fn_t = void (*)(initiator_comch_worker::hot_data &hot_data);

	/*
	 * Destructor
	 */
	~initiator_comch_worker();

	/*
	 * Allocate and prepare resources for this object
	 *
	 * @dev [in]: Device to use
	 * @comch_conn [in]: Comch control channel to use
	 * @expected_storage_content [in]: View of the expected storage content storage
	 * @transaction_count [in]: Number of transactions to use
	 * @local_io_region_begin [in]: Start address of local IO region this worker will access
	 * @local_io_region_size [in]: Size of local IO region that this worker will access
	 * @storage_io_region_begin [in]: Start virtual address of storage remote IO region this worker will access
	 * @storage_io_region_size [in]: Size of storage IO region that this worker will access
	 * @io_block_size [in]: Block size to use
	 */
	void init(doca_dev *dev,
		  doca_comch_connection *comch_conn,
		  uint8_t const *expected_storage_content,
		  uint32_t transaction_count,
		  uint8_t *local_io_region_begin,
		  uint64_t local_io_region_size,
		  uint64_t storage_io_region_begin,
		  uint64_t storage_io_region_size,
		  uint32_t io_block_size);

	/*
	 * Prepare thread proc
	 * @initial_task_op_mask [in]: Initial value for each transactions' pending ops action mask
	 * @run_limit_op_count [in]: Number of tasks to execute
	 * @core_id [in]: Core to run on
	 */
	void prepare_thread_proc(uint32_t initial_task_op_mask, uint32_t run_limit_op_count, uint32_t core_id);

	/*
	 * Prepare tasks required for the data path
	 *
	 * @remote_consumer_id [in]: ID of remote consumer
	 */
	void prepare_tasks(uint32_t remote_consumer_id);

	/*
	 * Check that all contexts are ready to run
	 *
	 * @return: true if all contexts are ready to run
	 */
	[[nodiscard]] bool are_contexts_ready(void) const noexcept;

	/*
	 * Start the worker thread
	 */
	void start_thread_proc(void);

	/*
	 * Query if thr worker thead is still running
	 *
	 * @return: true the worker is still running
	 */
	[[nodiscard]] bool is_thread_proc_running(void) const noexcept;

	/*
	 * Join the work thread
	 */
	void join_thread_proc(void);

	/*
	 * Get a reference to the workers hot data
	 *
	 * @return: A reference to the workers hot data
	 */
	[[nodiscard]] hot_data const &get_hot_data(void) const noexcept;

	/*
	 * Get a reference to the workers hot data
	 *
	 * @return: A reference to the workers hot data
	 */
	void destroy_data_path_objects(void);

private:
	/* Hot data - Data which is used on the hot path. This data is positioned first and aligned to cache lines to
	 * maximise performance
	 */
	hot_data m_hot_data = {};
	/**************************************************************************************************************
	 * Control data / Ownership data
	 * The cold path data is used to condifure and prepare for the host path. These objects and data must be
	 * maintained to allow for teardown / destruction later
	 */

	/* IO message region. Owning pointer to the memory region used as IO messages */
	uint8_t *m_io_message_region = nullptr;
	/* The doca_mmap used to allow the memory referenced by m_io_message_region to be used by doca_buf objects */
	doca_mmap *m_io_message_mmap = nullptr;
	/* A pool from which to allocate doca_buf objects */
	doca_buf_inventory *m_io_message_inv = nullptr;
	/* A reference to doca_bufs allocated that need to be realeased during teardown / destruction */
	std::vector<doca_buf *> m_io_message_bufs = {};
	/* doca_comch_consumer context, used to allocate tasks */
	doca_comch_consumer *m_consumer = nullptr;
	/* doca_comch_producer context, used to allocate tasks */
	doca_comch_producer *m_producer = nullptr;
	/* Collection of response (doca_comch_producer_task_send) tasks */
	std::vector<doca_task *> m_io_responses = {};
	/* Collection of request (doca_comch_consumer_task_post_recv) tasks */
	std::vector<doca_task *> m_io_requests = {};
	/* Thread execution object */
	std::thread m_thread = {};

	/*
	 * Release all resources held by this object
	 */
	void cleanup(void) noexcept;

	/*
	 * ComCh consumer task callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
							  doca_data task_user_data,
							  doca_data ctx_user_data) noexcept;

	/*
	 * ComCh consumer task error callback
	 *
	 * @task [in]: Failed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
								doca_data task_user_data,
								doca_data ctx_user_data) noexcept;

	/*
	 * ComCh producer task callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
						     doca_data task_user_data,
						     doca_data ctx_user_data) noexcept;

	/*
	 * ComCh producer task error callback
	 *
	 * @task [in]: Failed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
							   doca_data task_user_data,
							   doca_data ctx_user_data) noexcept;
};

/*
 * Application
 */
class initiator_comch_app {
public:
	/*
	 * Destructor
	 */
	~initiator_comch_app();

	/*
	 * Deleted default constructor
	 */
	initiator_comch_app() = delete;

	/*
	 * Constructor
	 *
	 * @cfg [in]: Configuration
	 */
	explicit initiator_comch_app(initiator_comch_app_configuration const &cfg);

	/*
	 * Deleted copy constructor
	 */
	initiator_comch_app(initiator_comch_app const &) = delete;

	/*
	 * Deleted move constructor
	 */
	initiator_comch_app(initiator_comch_app &&) noexcept = delete;

	/*
	 * Deleted copy assignment operator
	 */
	initiator_comch_app &operator=(initiator_comch_app const &) = delete;

	/*
	 * Deleted move assignment operator
	 */
	initiator_comch_app &operator=(initiator_comch_app &&) noexcept = delete;

	/*
	 * Abort the application
	 *
	 * @reason [in]: Abort reason
	 */
	void abort(std::string const &reason);

	/*
	 * Connect to the storage service
	 */
	void connect_to_storage_service(void);

	/*
	 * Query storage details
	 */
	void query_storage(void);

	/*
	 * Initialise storage session
	 */
	void init_storage(void);

	/*
	 * Prepare worker threads
	 */
	void prepare_threads(void);

	/*
	 * Start storage session
	 */
	void start_storage(void);

	/*
	 * Run storage test
	 */
	bool run(void);

	/*
	 * Join worker threads
	 */
	void join_threads(void);

	/*
	 * Stop storage session
	 */
	void stop_storage(void);

	/*
	 * display statistics
	 */
	void display_stats(void) const;

	/*
	 * Shutdown storage resources
	 */
	void shutdown(void);

private:
	/* Application configuration. */
	initiator_comch_app_configuration const m_cfg;
	/* The data expected to be held in the remote storage target. Used for read only validation. */
	std::vector<uint8_t> m_expected_storage_content;
	/* The doca_dev to use. */
	doca_dev *m_dev;
	/* A region of memory which will be read from / written to the remote storage target. */
	uint8_t *m_io_region;
	/* The doca_mmap used to allow the memory referenced by m_io_region to be used by doca_buf objects. */
	doca_mmap *m_io_mmap;
	/* A control channel used to exchange control messages between the initiator and service. */
	std::unique_ptr<storage::control::comch_channel> m_service_control_channel;
	/* A collection of received but not yet processed control messages. */
	std::vector<storage::control::message> m_ctrl_messages;
	/* A collection of remote consumer IDs. Each worker will use one remote consumer ID to exchange messages on the
	 * data path between the initiator and the service. */
	std::vector<uint32_t> m_remote_consumer_ids;
	/* Collection of worker objects. There is one worker per configured CPU core. */
	initiator_comch_worker *m_workers;
	/* The capacity of the local IO region. */
	uint64_t m_local_capacity;
	/* The storage capacity of the remote storage target. */
	uint64_t m_storage_capacity;
	/* The block size to use when reading to or writing from the storage target. */
	uint32_t m_storage_block_size;
	/* Run statistics. */
	initiator_comch_app_stats m_stats;
	/* A counter used to give control messages a unique ID. */
	uint32_t m_message_id_counter;
	/* A counter used to give control messages a correlation ID. (Used to correlate requests and responses) */
	uint32_t m_correlation_id_counter;
	/* Flag to track if an abort has been requested. */
	bool m_abort_flag;

	/*
	 * Handle a new remote consumer becoming available
	 *
	 * @event [in]: ComCh Event
	 * @comch_connection [in]: Connection the new consumer belongs to
	 * @id [in]: ID of the new consumer
	 */
	static void new_comch_consumer_callback(void *user_data, uint32_t id) noexcept;

	/*
	 * Handle a remote consumer becoming unavailable
	 *
	 * @event [in]: ComCh Event
	 * @comch_connection [in]: Connection the new consumer belonged to
	 * @id [in]: ID of the expired consumer
	 */
	static void expired_comch_consumer_callback(void *user_data, uint32_t id) noexcept;

	/*
	 * Wait for a response to a control message
	 *
	 * @type [in]: Type of message to expect
	 * @msg_id [in]: ID of the request to match its response to
	 * @timeout [in]: Timeout to use
	 */
	storage::control::message wait_for_control_response(storage::control::message_type type,
							    storage::control::message_id msg_id,
							    std::chrono::seconds timeout);
};

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed initiator_comch_app_configuration
 *
 * @throws: storage::runtime_error If the initiator_comch_app_configuration cannot pe parsed or contains invalid values
 */
initiator_comch_app_configuration parse_cli_args(int argc, char **argv);

} // namespace

/*
 * Main
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	storage::create_doca_logger_backend();

	printf("%s: v%s\n", app_name, doca_version());

	int exit_value = EXIT_SUCCESS;

	try {
		initiator_comch_app app{parse_cli_args(argc, argv)};
		storage::install_ctrl_c_handler([&app]() {
			app.abort("User requested abort");
		});
		app.connect_to_storage_service();
		app.query_storage();
		app.init_storage();
		app.prepare_threads();
		app.start_storage();
		auto const run_success = app.run();
		app.join_threads();
		app.stop_storage();
		if (run_success) {
			app.display_stats();
		} else {
			exit_value = EXIT_FAILURE;
			fprintf(stderr, "+================================================+\n");
			fprintf(stderr, "| Test failed!!\n");
			fprintf(stderr, "+================================================+\n");
		}
		app.shutdown();
	} catch (std::exception const &ex) {
		fprintf(stderr, "EXCEPTION: %s\n", ex.what());
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	}

	storage::uninstall_ctrl_c_handler();

	fflush(stdout);
	fflush(stderr);
	return exit_value;
}

namespace {

/*
 * Print the parsed initiator_comch_app_configuration
 *
 * @cfg [in]: initiator_comch_app_configuration to display
 */
void print_config(initiator_comch_app_configuration const &cfg) noexcept
{
	printf("initiator_comch_app_configuration: {\n");
	printf("\tcore_list : [");
	bool first = true;
	for (auto cpu : cfg.core_list) {
		if (first)
			first = false;
		else
			printf(", ");
		printf("%u", cpu);
	}
	printf("]\n");
	printf("\texecution_strategy : \"%s\",\n", cfg.run_type.c_str());
	printf("\tdevice : \"%s\",\n", cfg.device_id.c_str());
	printf("\tcommand_channel_name : \"%s\",\n", cfg.command_channel_name.c_str());
	printf("\tstorage_plain_content : \"%s\",\n", cfg.storage_plain_content_file.c_str());
	printf("\ttask_count : %u,\n", cfg.task_count);
	printf("\trun_limit_operation_count : %u,\n", cfg.run_limit_operation_count);
	printf("\tcontrol_timeout : %u,\n", static_cast<uint32_t>(cfg.control_timeout.count()));
	printf("}\n");
}

/*
 * Validate initiator_comch_app_configuration
 *
 * @cfg [in]: initiator_comch_app_configuration
 */
void validate_initiator_comch_app_configuration(initiator_comch_app_configuration const &cfg)
{
	std::vector<std::string> errors;

	if (cfg.task_count == 0) {
		errors.emplace_back("Invalid initiator_comch_app_configuration: task-count must not be zero");
	}

	if (cfg.control_timeout.count() == 0) {
		errors.emplace_back("Invalid initiator_comch_app_configuration: control-timeout must not be zero");
	}

	if (cfg.run_type == run_type_read_write_data_validity_test && cfg.core_list.size() != 1) {
		errors.push_back("Invalid initiator_comch_app_configuration: "s +
				 run_type_read_write_data_validity_test + " Only supports one thread");
	}

	if (cfg.run_type == run_type_read_only_data_validity_test && cfg.storage_plain_content_file.empty()) {
		errors.push_back("Invalid initiator_comch_app_configuration: "s +
				 run_type_read_only_data_validity_test + " requires plain data file to be provided");
	}

	if ((cfg.run_type == run_type_read_throughput_test || cfg.run_type == run_type_write_throughput_test) &&
	    cfg.run_limit_operation_count == 0) {
		errors.push_back("Invalid initiator_comch_app_configuration: "s + cfg.run_type +
				 " requires run-limit-operation-count to be greater than 0");
	}

	if (cfg.run_type != run_type_read_throughput_test && cfg.run_type != run_type_write_throughput_test &&
	    cfg.run_type != run_type_read_only_data_validity_test &&
	    cfg.run_type != run_type_read_write_data_validity_test) {
		errors.push_back("Invalid initiator_comch_app_configuration: "s + cfg.run_type + " is unknown");
	}

	if (!errors.empty()) {
		for (auto const &err : errors) {
			printf("%s\n", err.c_str());
		}
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Invalid initiator_comch_app_configuration detected"};
	}
}

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed initiator_comch_app_configuration
 *
 * @throws: storage::runtime_error If the initiator_comch_app_configuration cannot pe parsed or contains invalid values
 */
initiator_comch_app_configuration parse_cli_args(int argc, char **argv)
{
	initiator_comch_app_configuration config{};
	config.task_count = default_task_count;
	config.command_channel_name = default_command_channel_name;
	config.control_timeout = default_control_timeout_seconds;
	config.run_limit_operation_count = default_run_limit_operation_count;

	doca_error_t ret;

	ret = doca_argp_init(app_name, &config);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args"};
	}

	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       "d",
				       "device",
				       "Device identifier",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<initiator_comch_app_configuration *>(cfg)->device_id =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"cpu",
		"CPU core to which the process affinity can be set",
		storage::required_value,
		storage::multiple_values,
		[](void *value, void *cfg) noexcept {
			static_cast<initiator_comch_app_configuration *>(cfg)->core_list.push_back(
				*static_cast<int *>(value));
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"storage-plain-content",
		"File containing the plain data that is represented by the storage",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<initiator_comch_app_configuration *>(cfg)->storage_plain_content_file =
				static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"execution-strategy",
		"Define what to run. One of: read_throughput_test | write_throughput_test | read_write_data_validity_test | read_only_data_validity_test",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<initiator_comch_app_configuration *>(cfg)->run_type =
				static_cast<char const *>(value);

			return DOCA_SUCCESS;
		});

	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"run-limit-operation-count",
		"Run N operations (per thread) then stop. Default: 1000000",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<initiator_comch_app_configuration *>(cfg)->run_limit_operation_count =
				*static_cast<int *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(DOCA_ARGP_TYPE_INT,
				       nullptr,
				       "task-count",
				       "Number of concurrent tasks (per thread) to use. Default: 64",
				       storage::optional_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<initiator_comch_app_configuration *>(cfg)->task_count =
						       *static_cast<int *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"command-channel-name",
		"Name of the channel used by the doca_comch_client. Default: \"doca_storage_comch\"",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<initiator_comch_app_configuration *>(cfg)->command_channel_name =
				static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(DOCA_ARGP_TYPE_INT,
				       nullptr,
				       "control-timeout",
				       "Time (in seconds) to wait while performing control operations. Default: 5",
				       storage::optional_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<initiator_comch_app_configuration *>(cfg)->control_timeout =
						       std::chrono::seconds{*static_cast<int *>(value)};
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"local-io-region-size",
		"Size in bytes of the local region. Value can be 0 meaning automatically size to the same size as the storage target. Default: 0",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<initiator_comch_app_configuration *>(cfg)->local_io_region_size =
				*static_cast<int *>(value);
			return DOCA_SUCCESS;
		});
	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args"};
	}

	static_cast<void>(doca_argp_destroy());

	print_config(config);
	validate_initiator_comch_app_configuration(config);

	return config;
}

void worker_thread_proc(initiator_comch_worker::hot_data &hot_data)
{
	/* wait to start */
	while (hot_data.run_flag == false) {
		std::this_thread::yield();
		if (hot_data.error_flag)
			return;
	}

	/* submit initial tasks */
	auto const initial_task_count = std::min(hot_data.transactions_size, hot_data.target_transaction_count);
	for (uint32_t ii = 0; ii != initial_task_count; ++ii) {
		while (hot_data.progress_transaction(hot_data.transactions[ii]))
			;
	}

	/* run until the test completes */
	while (hot_data.completed_transaction_count != hot_data.started_transaction_count) {
		doca_pe_progress(hot_data.pe) ? ++(hot_data.pe_hit_count) : ++(hot_data.pe_miss_count);
	}
}

void thread_proc_wrapper(initiator_comch_worker::hot_data *hot_data) noexcept
{
	try {
		worker_thread_proc(*hot_data);
	} catch (storage::runtime_error const &ex) {
		std::stringstream ss;
		ss << "[Thread: " << std::this_thread::get_id()
		   << "] Exception: " << doca_error_get_name(ex.get_doca_error()) << ":" << ex.what();
		DOCA_LOG_ERR("%s", ss.str().c_str());
		hot_data->error_flag = true;
		hot_data->run_flag = false;
	}
}

uint32_t effective_block_count(uint32_t base_number, uint32_t &remainder)
{
	if (remainder == 0)
		return base_number;
	--remainder;
	return base_number + 1;
};

bool initiator_comch_worker::hot_data::progress_transaction(transaction_context &transaction)
{
	char *io_request;
	static_cast<void>(doca_buf_get_data(doca_comch_producer_task_send_get_buf(transaction.request),
					    reinterpret_cast<void **>(&io_request)));

	if (transaction.pending_actions == 0 && started_transaction_count != target_transaction_count) {
		/* set the transaction refcount to 2 as the order of completions between the receive callback and the
		 * send callback are not guaranteed to be ordered. The task cannot be re-used until both callbacks have
		 * completed.
		 */
		transaction.pending_actions = transaction_initial_ops_value;
		transaction.start_time = std::chrono::steady_clock::now();

		++started_transaction_count;
		return true;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::select_addresses)) != 0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::select_addresses);

		uint64_t local_offset;
		uint64_t storage_offset;
		if (local_io_addr_pool.get(local_offset) && storage_io_addr_pool.get(storage_offset)) {
			storage::io_message_view::set_storage_offset(storage_offset, io_request);
			storage::io_message_view::set_requester_offset(local_offset, io_request);

			return true;
		}

		DOCA_LOG_ERR("Failed to get IO address from pool. Pool was empty");
		run_flag = false;
		error_flag = true;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::copy_expected_memory_content)) !=
	    0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::copy_expected_memory_content);

		uint64_t const local_offset = storage::io_message_view::get_requester_offset(io_request);
		uint64_t const storage_offset = storage::io_message_view::get_storage_offset(io_request);
		uint64_t const io_size = storage::io_message_view::get_io_size(io_request);
		::memcpy(local_io_region_begin + local_offset, expected_storage_content + storage_offset, io_size);
		return true;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::write)) != 0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::write);

		transaction.refcount = 2;
		storage::io_message_view::set_type(storage::io_message_type::write, io_request);

		doca_error_t ret;
		do {
			ret = doca_task_submit(doca_comch_producer_task_send_as_task(transaction.request));
		} while (ret == DOCA_ERROR_AGAIN);

		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit comch producer send task: %s", doca_error_get_name(ret));
			run_flag = false;
			error_flag = true;
		}
		return false;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::clear_memory_content)) != 0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::clear_memory_content);

		uint64_t const local_offset = storage::io_message_view::get_requester_offset(io_request);
		uint64_t const io_size = storage::io_message_view::get_io_size(io_request);
		::memset(local_io_region_begin + local_offset, 0, io_size);
		return true;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::read)) != 0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::read);

		transaction.refcount = 2;
		storage::io_message_view::set_type(storage::io_message_type::read, io_request);

		doca_error_t ret;
		do {
			ret = doca_task_submit(doca_comch_producer_task_send_as_task(transaction.request));
		} while (ret == DOCA_ERROR_AGAIN);

		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit comch producer send task: %s", doca_error_get_name(ret));
			run_flag = false;
			error_flag = true;
		}

		return false;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::validate_memory_content)) != 0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::validate_memory_content);
		uint64_t const local_offset = storage::io_message_view::get_requester_offset(io_request);
		uint64_t const storage_offset = storage::io_message_view::get_storage_offset(io_request);
		uint64_t const io_size = storage::io_message_view::get_io_size(io_request);

		auto const result = ::memcmp(local_io_region_begin + local_offset,
					     expected_storage_content + storage_offset,
					     io_size);
		if (result != 0) {
			display_memory_mismatch(local_offset, storage_offset, io_size);
			run_flag = false;
			error_flag = true;
		}

		return true;
	}

	if ((transaction.pending_actions & static_cast<uint32_t>(transaction_action::finish_transaction)) != 0) {
		transaction.pending_actions ^= static_cast<uint32_t>(transaction_action::finish_transaction);

		auto const now = std::chrono::steady_clock::now();
		auto const usecs = std::chrono::duration_cast<std::chrono::microseconds>(now - transaction.start_time);
		latency_accumulator += usecs;
		auto const usecs_u32 = static_cast<uint32_t>(usecs.count());
		latency_min = std::min(latency_min, usecs_u32);
		latency_max = std::max(latency_max, usecs_u32);

		local_io_addr_pool.put(storage::io_message_view::get_requester_offset(io_request));
		storage_io_addr_pool.put(storage::io_message_view::get_storage_offset(io_request));

		++completed_transaction_count;
		if (started_transaction_count < target_transaction_count) {
			return true;
		}

		if (completed_transaction_count == target_transaction_count) {
			run_flag = false;
			end_time = std::chrono::steady_clock::now();
		}

		return false;
	}

	return false;
}

void initiator_comch_worker::hot_data::display_memory_mismatch(uint32_t local_offset,
							       uint32_t storage_offset,
							       uint32_t io_size) const
{
	auto const expected_bytes_str =
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(expected_storage_content) + storage_offset,
					  io_size);
	auto const actual_bytes_str =
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(local_io_region_begin) + local_offset,
					  io_size);
	DOCA_LOG_ERR("Memory read from storage target does not match expected value. "
		     "\n\tExpected: [%s]"
		     "\n\tActual:   [%s]",
		     expected_bytes_str.c_str(),
		     actual_bytes_str.c_str());
}

initiator_comch_worker::~initiator_comch_worker()
{
	cleanup();
}

void initiator_comch_worker::init(doca_dev *dev,
				  doca_comch_connection *comch_conn,
				  uint8_t const *expected_storage_content,
				  uint32_t transaction_count,
				  uint8_t *local_io_region_begin,
				  uint64_t local_io_region_size,
				  uint64_t storage_io_region_begin,
				  uint64_t storage_io_region_size,
				  uint32_t io_block_size)
{
	doca_error_t ret;
	auto const page_size = storage::get_system_page_size();

	m_hot_data.expected_storage_content = expected_storage_content;
	m_hot_data.local_io_region_begin = local_io_region_begin;
	m_hot_data.storage_io_region_begin = storage_io_region_begin;
	m_hot_data.io_block_size = io_block_size;

	auto const local_io_block_count = local_io_region_size / io_block_size;
	m_hot_data.local_io_addr_pool.init_ring(local_io_block_count);
	for (uint32_t ii = 0; ii != local_io_block_count; ++ii) {
		static_cast<void>(m_hot_data.local_io_addr_pool.put(ii * io_block_size));
	}

	auto const storage_io_block_count = storage_io_region_size / io_block_size;
	m_hot_data.storage_io_addr_pool.init_ring(storage_io_block_count);
	for (uint32_t ii = 0; ii != local_io_block_count; ++ii) {
		static_cast<void>(m_hot_data.storage_io_addr_pool.put(ii * io_block_size));
	}

	auto const raw_io_messages_size = transaction_count * storage::size_of_io_message * 2;

	DOCA_LOG_DBG("Allocate comch buffers memory (%zu bytes, aligned to %u byte pages)",
		     raw_io_messages_size,
		     page_size);
	m_io_message_region = static_cast<uint8_t *>(
		storage::aligned_alloc(page_size, storage::aligned_size(page_size, raw_io_messages_size)));
	if (m_io_message_region == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate comch fast path buffers memory"};
	}

	try {
		m_hot_data.transactions_size = transaction_count;
		m_hot_data.transactions =
			storage::make_aligned<transaction_context>{}.object_array(m_hot_data.transactions_size);
	} catch (std::exception const &ex) {
		throw storage::runtime_error{DOCA_ERROR_NO_MEMORY,
					     "Failed to allocate transaction contexts memory: "s + ex.what()};
	}

	m_io_message_mmap = storage::make_mmap(dev,
					       reinterpret_cast<char *>(m_io_message_region),
					       raw_io_messages_size,
					       DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);

	ret = doca_buf_inventory_create(transaction_count * 2, &m_io_message_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create comch fast path doca_buf_inventory"};
	}

	ret = doca_buf_inventory_start(m_io_message_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start comch fast path doca_buf_inventory"};
	}

	DOCA_LOG_DBG("Create hot path progress engine");
	ret = doca_pe_create(std::addressof(m_hot_data.pe));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_pe"};
	}

	m_producer = storage::make_comch_producer(comch_conn,
						  m_hot_data.pe,
						  transaction_count,
						  doca_data{.ptr = std::addressof(m_hot_data)},
						  doca_comch_producer_task_send_cb,
						  doca_comch_producer_task_send_error_cb);
	m_io_requests.reserve(transaction_count);

	m_consumer = storage::make_comch_consumer(comch_conn,
						  m_io_message_mmap,
						  m_hot_data.pe,
						  transaction_count,
						  doca_data{.ptr = std::addressof(m_hot_data)},
						  doca_comch_consumer_task_post_recv_cb,
						  doca_comch_consumer_task_post_recv_error_cb);
	m_io_responses.reserve(transaction_count);
}

void initiator_comch_worker::prepare_thread_proc(uint32_t initial_task_op_mask,
						 uint32_t run_limit_op_count,
						 uint32_t core_id)
{
	m_hot_data.run_flag = false;
	m_hot_data.error_flag = false;
	m_hot_data.pe_hit_count = 0;
	m_hot_data.pe_miss_count = 0;
	m_hot_data.transaction_initial_ops_value = initial_task_op_mask;
	m_hot_data.target_transaction_count = run_limit_op_count;
	m_hot_data.started_transaction_count = 0;
	m_hot_data.completed_transaction_count = 0;

	m_thread = std::thread{thread_proc_wrapper, std::addressof(m_hot_data)};
	storage::set_thread_affinity(m_thread, core_id);
}

void initiator_comch_worker::prepare_tasks(uint32_t remote_consumer_id)
{
	doca_error_t ret;
	uint64_t io_offset = 0;
	uint8_t *msg_addr = m_io_message_region;

	for (uint32_t ii = 0; ii != m_hot_data.transactions_size; ++ii) {
		doca_buf *consumer_buf;
		doca_comch_consumer_task_post_recv *consumer_task;

		ret = doca_buf_inventory_buf_get_by_addr(m_io_message_inv,
							 m_io_message_mmap,
							 msg_addr,
							 storage::size_of_io_message,
							 &consumer_buf);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get doca_buf for consumer task"};
		}

		m_io_message_bufs.push_back(consumer_buf);
		msg_addr += storage::size_of_io_message;

		ret = doca_comch_consumer_task_post_recv_alloc_init(m_consumer, consumer_buf, &consumer_task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to allocate consumer task"};
		}

		m_io_responses.push_back(doca_comch_consumer_task_post_recv_as_task(consumer_task));
	}

	auto const op_type =
		(m_hot_data.transaction_initial_ops_value & static_cast<uint32_t>(transaction_action::write)) ?
			storage::io_message_type::write :
			storage::io_message_type::read;

	for (uint32_t ii = 0; ii != m_hot_data.transactions_size; ++ii) {
		doca_buf *producer_buf;
		doca_comch_producer_task_send *producer_task;

		ret = doca_buf_inventory_buf_get_by_data(m_io_message_inv,
							 m_io_message_mmap,
							 msg_addr,
							 storage::size_of_io_message,
							 &producer_buf);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get doca_buf for producer task"};
		}

		m_io_message_bufs.push_back(producer_buf);
		auto *const io_message = reinterpret_cast<char *>(msg_addr);
		msg_addr += storage::size_of_io_message;
		;

		ret = doca_comch_producer_task_send_alloc_init(m_producer,
							       producer_buf,
							       nullptr,
							       0,
							       remote_consumer_id,
							       &producer_task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get doca_buf for producer task"};
		}
		static_cast<void>(
			doca_task_set_user_data(doca_comch_producer_task_send_as_task(producer_task),
						doca_data{.ptr = std::addressof(m_hot_data.transactions[ii])}));
		m_io_requests.push_back(doca_comch_producer_task_send_as_task(producer_task));
		m_hot_data.transactions[ii].refcount = 0;
		m_hot_data.transactions[ii].request = producer_task;

		storage::io_message_view::set_type(op_type, io_message);
		storage::io_message_view::set_user_data(doca_data{.u64 = remote_consumer_id}, io_message);
		storage::io_message_view::set_correlation_id(ii, io_message);
		storage::io_message_view::set_storage_offset(io_offset, io_message);
		storage::io_message_view::set_requester_offset(io_offset, io_message);
		storage::io_message_view::set_io_size(m_hot_data.io_block_size, io_message);

		io_offset += m_hot_data.io_block_size;
	}

	for (auto *task : m_io_responses) {
		ret = doca_task_submit(task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get doca_buf for producer task"};
		}
	}
}

bool initiator_comch_worker::are_contexts_ready(void) const noexcept
{
	static_cast<void>(doca_pe_progress(m_hot_data.pe));
	auto const consumer_running = storage::is_ctx_running(doca_comch_consumer_as_ctx(m_consumer));
	auto const producer_running = storage::is_ctx_running(doca_comch_producer_as_ctx(m_producer));
	return consumer_running && producer_running;
}

void initiator_comch_worker::start_thread_proc(void)
{
	m_hot_data.run_flag = true;
}

bool initiator_comch_worker::is_thread_proc_running(void) const noexcept
{
	return m_hot_data.run_flag;
}

void initiator_comch_worker::join_thread_proc(void)
{
	if (m_thread.joinable())
		m_thread.join();
}

initiator_comch_worker::hot_data const &initiator_comch_worker::get_hot_data(void) const noexcept
{
	return m_hot_data;
}

void initiator_comch_worker::destroy_data_path_objects(void)
{
	doca_error_t ret;
	if (m_consumer != nullptr) {
		ret = storage::stop_context(doca_comch_consumer_as_ctx(m_consumer), m_hot_data.pe, m_io_responses);
		if (ret == DOCA_SUCCESS) {
			m_io_responses.clear();
		} else {
			DOCA_LOG_ERR("Failed to stop consumer context");
		}
		ret = doca_comch_consumer_destroy(m_consumer);
		if (ret == DOCA_SUCCESS) {
			m_consumer = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy consumer context");
		}
	}

	if (m_producer != nullptr) {
		ret = storage::stop_context(doca_comch_producer_as_ctx(m_producer), m_hot_data.pe, m_io_requests);
		if (ret == DOCA_SUCCESS) {
			m_io_requests.clear();
		} else {
			DOCA_LOG_ERR("Failed to stop producer context");
		}
		ret = doca_comch_producer_destroy(m_producer);
		if (ret == DOCA_SUCCESS) {
			m_producer = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy producer context");
		}
	}

	if (m_hot_data.pe != nullptr) {
		ret = doca_pe_destroy(m_hot_data.pe);
		if (ret == DOCA_SUCCESS) {
			m_hot_data.pe = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy progress engine");
		}
	}
}

void initiator_comch_worker::cleanup(void) noexcept
{
	doca_error_t ret;

	if (m_thread.joinable()) {
		m_hot_data.run_flag = false;
		m_hot_data.error_flag = true;
		m_thread.join();
	}

	destroy_data_path_objects();

	for (auto *buf : m_io_message_bufs) {
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
	}

	if (m_io_message_inv) {
		ret = doca_buf_inventory_stop(m_io_message_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop buffer inventory");
		}
		ret = doca_buf_inventory_destroy(m_io_message_inv);
		if (ret == DOCA_SUCCESS) {
			m_io_message_inv = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy buffer inventory");
		}
	}

	if (m_io_message_mmap) {
		ret = doca_mmap_stop(m_io_message_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop mmap");
		}
		ret = doca_mmap_destroy(m_io_message_mmap);
		if (ret == DOCA_SUCCESS) {
			m_io_message_mmap = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy mmap");
		}
	}

	if (m_hot_data.transactions != nullptr) {
		storage::aligned_free(m_hot_data.transactions);
		m_hot_data.transactions = nullptr;
	}

	if (m_io_message_region != nullptr) {
		storage::aligned_free(m_io_message_region);
		m_io_message_region = nullptr;
	}
}

void initiator_comch_worker::doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
								   doca_data task_user_data,
								   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<initiator_comch_worker::hot_data *>(ctx_user_data.ptr);
	char *io_message;
	auto *buf = doca_comch_consumer_task_post_recv_get_buf(task);
	static_cast<void>(doca_buf_get_data(buf, reinterpret_cast<void **>(&io_message)));
	auto const correlation_id = storage::io_message_view::get_correlation_id(io_message);
	if (correlation_id > hot_data->transactions_size) {
		DOCA_LOG_ERR("Received storage response with invalid async id: %u", correlation_id);
		hot_data->run_flag = false;
		hot_data->error_flag = true;
		return;
	}

	if (--(hot_data->transactions[correlation_id].refcount) == 0) {
		while (hot_data->progress_transaction(hot_data->transactions[correlation_id]))
			;
	}

	doca_buf_reset_data_len(doca_comch_consumer_task_post_recv_get_buf(task));
	auto const ret = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to resubmit doca_comch_consumer_task_post_recv: %s", doca_error_get_name(ret));
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void initiator_comch_worker::doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
									 doca_data task_user_data,
									 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<initiator_comch_worker::hot_data *>(ctx_user_data.ptr);

	if (hot_data->run_flag) {
		DOCA_LOG_ERR("Failed to complete doca_comch_consumer_task_post_recv");
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void initiator_comch_worker::doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
							      doca_data task_user_data,
							      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *const hot_data = static_cast<initiator_comch_worker::hot_data *>(ctx_user_data.ptr);
	auto &transaction = *static_cast<transaction_context *>(task_user_data.ptr);
	if (--(transaction.refcount) == 0) {
		while (hot_data->progress_transaction(transaction))
			;
	}
}

void initiator_comch_worker::doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
								    doca_data task_user_data,
								    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<initiator_comch_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_comch_producer_task_send");
	hot_data->run_flag = false;
	hot_data->error_flag = true;
}

initiator_comch_app::~initiator_comch_app()
{
	doca_error_t ret;

	if (m_workers != nullptr) {
		for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
			m_workers[ii].~initiator_comch_worker();
		}
		storage::aligned_free(m_workers);
	}

	if (m_io_mmap != nullptr) {
		ret = doca_mmap_stop(m_io_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_mmap(%p): %s", m_io_mmap, doca_error_get_name(ret));
		}

		ret = doca_mmap_destroy(m_io_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy doca_mmap: %s", doca_error_get_name(ret));
		}
	}

	if (m_io_region != nullptr) {
		storage::aligned_free(m_io_region);
	}

	m_service_control_channel.reset();

	if (m_dev != nullptr) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {}
	}
}

initiator_comch_app::initiator_comch_app(initiator_comch_app_configuration const &cfg)
	: m_cfg{cfg},
	  m_expected_storage_content{},
	  m_dev{nullptr},
	  m_io_region{nullptr},
	  m_io_mmap{nullptr},
	  m_service_control_channel{},
	  m_ctrl_messages{},
	  m_remote_consumer_ids{},
	  m_workers{nullptr},
	  m_local_capacity{0},
	  m_storage_capacity{0},
	  m_storage_block_size{0},
	  m_stats{},
	  m_message_id_counter{},
	  m_correlation_id_counter{0},
	  m_abort_flag{false}
{
	if (cfg.run_type == run_type_read_only_data_validity_test) {
		m_expected_storage_content = storage::load_file_bytes(cfg.storage_plain_content_file);
	}

	DOCA_LOG_INFO("Open doca_dev: %s", m_cfg.device_id.c_str());
	m_dev = storage::open_device(m_cfg.device_id);
	m_service_control_channel =
		storage::control::make_comch_client_control_channel(m_dev,
								    m_cfg.command_channel_name.c_str(),
								    this,
								    new_comch_consumer_callback,
								    expired_comch_consumer_callback);
}

void initiator_comch_app::abort(std::string const &reason)
{
	if (m_abort_flag)
		return;

	DOCA_LOG_ERR("Aborted: %s", reason.c_str());
	m_abort_flag = true;
}

void initiator_comch_app::connect_to_storage_service(void)
{
	auto const expiry = std::chrono::steady_clock::now() + m_cfg.control_timeout;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});

		if (m_service_control_channel->is_connected()) {
			break;
		}

		if (std::chrono::steady_clock::now() > expiry) {
			throw storage::runtime_error{
				DOCA_ERROR_TIME_OUT,
				"Timed out trying to connect to storage",

			};
		}
	}
}

void initiator_comch_app::query_storage(void)
{
	DOCA_LOG_INFO("Query storage...");
	auto const correlation_id = storage::control::correlation_id{m_correlation_id_counter++};
	auto const message_id = storage::control::message_id{m_message_id_counter++};
	m_service_control_channel->send_message(
		{storage::control::message_type::query_storage_request, message_id, correlation_id, {}});

	auto const response = wait_for_control_response(storage::control::message_type::query_storage_response,
							message_id,
							default_control_timeout_seconds);
	auto const *storage_details =
		dynamic_cast<storage::control::storage_details_payload const *>(response.payload.get());
	if (storage_details == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_UNEXPECTED, "[BUG] Invalid query_storage_response received"};
	}

	m_storage_capacity = storage_details->total_size;
	m_storage_block_size = storage_details->block_size;
	DOCA_LOG_INFO("Storage reports capacity of: %lu using a block size of: %u",
		      m_storage_capacity,
		      m_storage_block_size);

	if (m_cfg.run_type == run_type_read_only_data_validity_test) {
		if (m_storage_capacity != m_expected_storage_content.size()) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"Read only validation test requires that the provided plain data is the same size as the storage capacity"};
		}
	} else if (m_cfg.run_type == run_type_read_write_data_validity_test) {
		m_expected_storage_content.resize(m_storage_capacity);
		for (size_t ii = 0; ii != m_storage_capacity; ++ii) {
			m_expected_storage_content[ii] = static_cast<uint8_t>(ii);
		}
	}
}

void initiator_comch_app::init_storage(void)
{
	DOCA_LOG_INFO("Init storage...");
	auto const core_count = static_cast<uint32_t>(m_cfg.core_list.size());
	uint8_t const *plain_content = m_expected_storage_content.empty() ? nullptr : m_expected_storage_content.data();
	auto const per_worker_task_count = std::min(m_cfg.task_count, m_cfg.run_limit_operation_count);

	m_remote_consumer_ids.reserve(core_count);
	if (m_cfg.local_io_region_size != 0) {
		m_local_capacity = m_cfg.local_io_region_size;
	} else {
		m_local_capacity = m_storage_capacity;
	}

	if (!m_expected_storage_content.empty()) {
		if (m_expected_storage_content.size() != m_storage_capacity) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"Expected content size: "s + std::to_string(m_expected_storage_content.size()) +
					" MUST match storage capacity: " + std::to_string(m_storage_capacity)};
		}
	}

	m_io_region = static_cast<uint8_t *>(storage::aligned_alloc(storage::get_system_page_size(), m_local_capacity));
	m_io_mmap = storage::make_mmap(m_dev,
				       reinterpret_cast<char *>(m_io_region),
				       m_local_capacity,
				       DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE |
					       DOCA_ACCESS_FLAG_RDMA_WRITE | DOCA_ACCESS_FLAG_RDMA_READ);

	auto mmap_details = [this]() {
		void const *data;
		size_t len;
		auto const ret = doca_mmap_export_pci(m_io_mmap, m_dev, &data, &len);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to export mmap"};
		}

		return std::vector<uint8_t>(static_cast<uint8_t const *>(data),
					    static_cast<uint8_t const *>(data) + len);
	}();

	auto const correlation_id = storage::control::correlation_id{m_correlation_id_counter++};
	auto const message_id = storage::control::message_id{m_message_id_counter++};

	/* Over allocate transactions in the back end. This helps to prevent a race condition where the initiator wants
	 * to send a message to the service, but the service has not got its consumer task submitted before the
	 * initiator send data to an empty remote comch queue, this hurts performance, so we prefer to allocate extra
	 * tasks to prevent it from happening too often without wasting too much memory
	 */
	auto const remote_per_thread_block_count =
		per_worker_task_count + std::min(uint32_t{2}, (per_worker_task_count / 4));
	m_service_control_channel->send_message({
		storage::control::message_type::init_storage_request,
		message_id,
		correlation_id,
		std::make_unique<storage::control::init_storage_payload>(remote_per_thread_block_count,
									 core_count,
									 std::move(mmap_details)),
	});

	DOCA_LOG_INFO("Init storage to use using %u cores with %u tasks each", core_count, per_worker_task_count);

	static_cast<void>(wait_for_control_response(storage::control::message_type::init_storage_response,
						    message_id,
						    default_control_timeout_seconds));

	m_workers = storage::make_aligned<initiator_comch_worker>{}.object_array(m_cfg.core_list.size());
	uint32_t const local_complete_blocks_count = m_local_capacity / m_storage_block_size;
	uint32_t const per_worker_local_block_count = local_complete_blocks_count / m_cfg.core_list.size();
	uint32_t per_worker_local_block_count_remainder = local_complete_blocks_count % m_cfg.core_list.size();
	if (per_worker_local_block_count == 0) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Local IO region is to small to give each worker at least one block each"};
	}

	uint32_t const storage_complete_blocks_count = m_storage_capacity / m_storage_block_size;
	uint32_t const per_worker_storage_block_count = storage_complete_blocks_count / m_cfg.core_list.size();
	uint32_t per_worker_storage_block_count_remainder = storage_complete_blocks_count % m_cfg.core_list.size();
	if (per_worker_storage_block_count == 0) {
		throw storage::runtime_error{
			DOCA_ERROR_INVALID_VALUE,
			"Storage IO region is to small to give each worker at least one block each"};
	}

	auto *worker_local_start_addr = m_io_region;
	auto worker_storage_io_begin_addr = uint64_t{0};

	for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
		auto const this_worker_local_block_count =
			effective_block_count(per_worker_local_block_count, per_worker_local_block_count_remainder);
		auto const this_worker_storage_block_count =
			effective_block_count(per_worker_storage_block_count, per_worker_storage_block_count_remainder);

		auto const this_worker_task_count =
			std::min(per_worker_task_count,
				 std::min(this_worker_local_block_count, this_worker_storage_block_count));

		auto const this_worker_local_io_size = this_worker_local_block_count * m_storage_block_size;
		auto const this_worker_storage_io_size = this_worker_storage_block_count * m_storage_block_size;
		m_workers[ii].init(m_dev,
				   m_service_control_channel->get_comch_connection(),
				   plain_content,
				   this_worker_task_count,
				   worker_local_start_addr,
				   this_worker_local_io_size,
				   worker_storage_io_begin_addr,
				   this_worker_storage_io_size,
				   m_storage_block_size);

		worker_local_start_addr += this_worker_local_io_size;
		worker_storage_io_begin_addr += this_worker_storage_io_size;
	}

	DOCA_LOG_INFO("Wait for remote objects to be finish async init...");
	auto const expiry = std::chrono::steady_clock::now() + m_cfg.control_timeout;
	for (;;) {
		auto const *msg = m_service_control_channel->poll();
		if (msg != nullptr) {
			throw storage::runtime_error{DOCA_ERROR_BAD_STATE,
						     "Received unexpected " + to_string(msg->message_type) +
							     "while initialising"};
		}

		if (std::chrono::steady_clock::now() > expiry) {
			throw storage::runtime_error{DOCA_ERROR_TIME_OUT,
						     "Timed out waiting for remote objects to start"};
		}

		auto const ready_ctx_count = std::accumulate(m_workers,
							     m_workers + m_cfg.core_list.size(),
							     uint32_t{0},
							     [](uint32_t total, initiator_comch_worker &worker) {
								     return total + worker.are_contexts_ready();
							     });

		if (m_remote_consumer_ids.size() == m_cfg.core_list.size() &&
		    ready_ctx_count == m_cfg.core_list.size()) {
			DOCA_LOG_INFO("Async init complete");
			return;
		}
	}
}

void initiator_comch_app::prepare_threads(void)
{
	uint32_t op_mask = static_cast<uint32_t>(transaction_action::select_addresses) |
			   static_cast<uint32_t>(transaction_action::finish_transaction);

	if (m_cfg.run_type == run_type_read_throughput_test) {
		op_mask |= static_cast<uint32_t>(transaction_action::read);
	} else if (m_cfg.run_type == run_type_write_throughput_test) {
		op_mask |= static_cast<uint32_t>(transaction_action::write);
	} else if (m_cfg.run_type == run_type_read_only_data_validity_test) {
		op_mask |= static_cast<uint32_t>(transaction_action::clear_memory_content);
		op_mask |= static_cast<uint32_t>(transaction_action::read);
		op_mask |= static_cast<uint32_t>(transaction_action::validate_memory_content);
	} else if (m_cfg.run_type == run_type_read_write_data_validity_test) {
		op_mask |= static_cast<uint32_t>(transaction_action::copy_expected_memory_content);
		op_mask |= static_cast<uint32_t>(transaction_action::write);
		op_mask |= static_cast<uint32_t>(transaction_action::clear_memory_content);
		op_mask |= static_cast<uint32_t>(transaction_action::read);
		op_mask |= static_cast<uint32_t>(transaction_action::validate_memory_content);
	} else {
		throw storage::runtime_error{DOCA_ERROR_NOT_SUPPORTED, "Unhandled run mode: " + m_cfg.run_type};
	}

	for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
		m_workers[ii].prepare_thread_proc(op_mask, m_cfg.run_limit_operation_count, m_cfg.core_list[ii]);

		m_workers[ii].prepare_tasks(m_remote_consumer_ids[ii]);
	}
}

void initiator_comch_app::start_storage(void)
{
	DOCA_LOG_INFO("Start storage...");
	auto const correlation_id = storage::control::correlation_id{m_correlation_id_counter++};
	auto const message_id = storage::control::message_id{m_message_id_counter++};
	m_service_control_channel->send_message(
		{storage::control::message_type::start_storage_request, message_id, correlation_id, {}});

	static_cast<void>(wait_for_control_response(storage::control::message_type::start_storage_response,
						    message_id,
						    default_control_timeout_seconds));
}

bool initiator_comch_app::run(void)
{
	// Start threads
	m_stats.start_time = std::chrono::steady_clock::now();
	for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
		auto &tctx = m_workers[ii];
		tctx.start_thread_proc();
	}

	// Run to completion or user abort
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds{200});
		auto const running_workers = std::accumulate(m_workers,
							     m_workers + m_cfg.core_list.size(),
							     uint32_t{0},
							     [](uint32_t total, auto const &tctx) {
								     return total + tctx.is_thread_proc_running();
							     });

		if (running_workers == 0)
			break;
	}

	// Tally stats
	uint64_t latency_acc = 0;
	m_stats.end_time = m_stats.start_time;
	m_stats.operation_count = 0;
	m_stats.latency_max = 0;
	m_stats.latency_min = std::numeric_limits<uint32_t>::max();
	bool any_error = false;
	for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
		auto const &hot_data = m_workers[ii].get_hot_data();
		if (hot_data.error_flag)
			any_error = true;

		m_stats.end_time = std::max(m_stats.end_time, hot_data.end_time);
		m_stats.operation_count += hot_data.completed_transaction_count;
		m_stats.latency_min = std::min(m_stats.latency_min, hot_data.latency_min);
		m_stats.latency_max = std::max(m_stats.latency_max, hot_data.latency_max);
		m_stats.pe_hit_count += hot_data.pe_hit_count;
		m_stats.pe_miss_count += hot_data.pe_miss_count;

		latency_acc += hot_data.latency_accumulator.count();
	}

	if (m_stats.operation_count != 0) {
		m_stats.latency_mean = latency_acc / m_stats.operation_count;
	} else {
		m_stats.latency_mean = 0;
	}

	return any_error == false;
}

void initiator_comch_app::join_threads(void)
{
	for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
		m_workers[ii].join_thread_proc();
	}
}

void initiator_comch_app::stop_storage(void)
{
	DOCA_LOG_INFO("Stop storage...");

	auto const correlation_id = storage::control::correlation_id{m_correlation_id_counter++};
	auto const message_id = storage::control::message_id{m_message_id_counter++};
	m_service_control_channel->send_message(
		{storage::control::message_type::stop_storage_request, message_id, correlation_id, {}});

	static_cast<void>(wait_for_control_response(storage::control::message_type::stop_storage_response,
						    message_id,
						    default_control_timeout_seconds));
	DOCA_LOG_INFO("Storage stopped");
}

void initiator_comch_app::display_stats(void) const
{
	auto duration_secs_float = std::chrono::duration<double>{m_stats.end_time - m_stats.start_time}.count();
	auto const bytes = uint64_t{m_stats.operation_count} * m_storage_block_size;
	auto const GiBs = static_cast<double>(bytes) / (1024. * 1024. * 1024.);
	auto const miops = (static_cast<double>(m_stats.operation_count) / 1'000'000.) / duration_secs_float;
	auto const pe_hit_rate_pct =
		(static_cast<double>(m_stats.pe_hit_count) /
		 (static_cast<double>(m_stats.pe_hit_count) + static_cast<double>(m_stats.pe_miss_count))) *
		100.;

	printf("+================================================+\n");
	printf("| Stats\n");
	printf("+================================================+\n");
	printf("| Duration (seconds): %2.06lf\n", duration_secs_float);
	printf("| Operation count: %lu\n", m_stats.operation_count);
	printf("| Data rate: %.03lf GiB/s\n", GiBs / duration_secs_float);
	printf("| IO rate: %.03lf MIOP/s\n", miops);
	printf("| PE hit rate: %2.03lf%% (%lu:%lu)\n", pe_hit_rate_pct, m_stats.pe_hit_count, m_stats.pe_miss_count);
	printf("| Latency:\n");
	printf("| \tMin: %uus\n", m_stats.latency_min);
	printf("| \tMax: %uus\n", m_stats.latency_max);
	printf("| \tMean: %uus\n", m_stats.latency_mean);
	printf("+================================================+\n");
}

void initiator_comch_app::shutdown(void)
{
	DOCA_LOG_INFO("Shutdown storage...");

	// destroy local comch objects
	for (uint32_t ii = 0; ii != m_cfg.core_list.size(); ++ii) {
		m_workers[ii].destroy_data_path_objects();
	}

	// Destroy remote comch objects
	auto const correlation_id = storage::control::correlation_id{m_correlation_id_counter++};
	auto const message_id = storage::control::message_id{m_message_id_counter++};
	m_service_control_channel->send_message(
		{storage::control::message_type::shutdown_request, message_id, correlation_id, {}});

	static_cast<void>(wait_for_control_response(storage::control::message_type::shutdown_response,
						    message_id,
						    default_control_timeout_seconds));

	while (!m_remote_consumer_ids.empty()) {
		auto *msg = m_service_control_channel->poll();
		if (msg != nullptr) {
			DOCA_LOG_INFO("Ignoring unexpected message: %s", storage::control::to_string(*msg).c_str());
		}
	}

	DOCA_LOG_INFO("Storage shutdown");
}

void initiator_comch_app::new_comch_consumer_callback(void *user_data, uint32_t id) noexcept
{
	auto *self = reinterpret_cast<initiator_comch_app *>(user_data);
	if (self->m_remote_consumer_ids.capacity() == 0) {
		DOCA_LOG_ERR("[BUG] no space for new remote consumer ids");
		return;
	}

	auto found = std::find(std::begin(self->m_remote_consumer_ids), std::end(self->m_remote_consumer_ids), id);
	if (found == std::end(self->m_remote_consumer_ids)) {
		self->m_remote_consumer_ids.push_back(id);
		DOCA_LOG_DBG("Connected to remote consumer with id: %u. Consumer count is now: %zu",
			     id,
			     self->m_remote_consumer_ids.size());
	} else {
		DOCA_LOG_WARN("Ignoring duplicate remote consumer id: %u", id);
	}
}

void initiator_comch_app::expired_comch_consumer_callback(void *user_data, uint32_t id) noexcept
{
	auto *self = reinterpret_cast<initiator_comch_app *>(user_data);
	auto found = std::find(std::begin(self->m_remote_consumer_ids), std::end(self->m_remote_consumer_ids), id);
	if (found != std::end(self->m_remote_consumer_ids)) {
		self->m_remote_consumer_ids.erase(found);
		DOCA_LOG_DBG("Disconnected from remote consumer with id: %u. Consumer count is now: %zu",
			     id,
			     self->m_remote_consumer_ids.size());
	} else {
		DOCA_LOG_WARN("Ignoring disconnect of unexpected remote consumer id: %u", id);
	}
}

storage::control::message initiator_comch_app::wait_for_control_response(storage::control::message_type type,
									 storage::control::message_id msg_id,
									 std::chrono::seconds timeout)
{
	auto expiry_time = std::chrono::steady_clock::now() + timeout;
	do {
		// Poll for new messages
		auto *msg = m_service_control_channel->poll();
		if (msg) {
			m_ctrl_messages.push_back(std::move(*msg));
		}

		// Check for matching message
		auto iter = std::find_if(std::begin(m_ctrl_messages), std::end(m_ctrl_messages), [msg_id](auto &msg) {
			return msg.message_id == msg_id;
		});

		if (iter == std::end(m_ctrl_messages))
			continue;

		// Remove response from the queue
		auto response = std::move(*iter);
		m_ctrl_messages.erase(iter);

		// Handle remote failure
		if (response.message_type == storage::control::message_type::error_response) {
			auto const *const error_details =
				reinterpret_cast<storage::control::error_response_payload const *>(
					response.payload.get());
			throw storage::runtime_error{error_details->error_code,
						     to_string(type) + " id: " + std::to_string(msg_id.value) +
							     " failed!: " + error_details->message};
		}

		// Handle unexpected response
		if (response.message_type != type) {
			throw storage::runtime_error{DOCA_ERROR_UNEXPECTED,
						     "Received unexpected " + to_string(response.message_type) +
							     " While waiting for " + to_string(type) +
							     " id: " + std::to_string(msg_id.value)};
		}

		// Good response, let caller handle it
		return response;

	} while (std::chrono::steady_clock::now() < expiry_time);

	throw storage::runtime_error{DOCA_ERROR_TIME_OUT,
				     "Timed out waiting on: " + to_string(type) +
					     " id: " + std::to_string(msg_id.value)};
}

} /* namespace */
