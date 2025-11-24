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
#include <cstdlib>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <thread>

#include <doca_argp.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_version.h>

#include <storage_common/aligned_new.hpp>
#include <storage_common/binary_content.hpp>
#include <storage_common/buffer_utils.hpp>
#include <storage_common/control_message.hpp>
#include <storage_common/control_channel.hpp>
#include <storage_common/control_worker_async.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/doca_utils.hpp>
#include <storage_common/file_utils.hpp>
#include <storage_common/io_message.hpp>
#include <storage_common/os_utils.hpp>

DOCA_LOG_REGISTER(TARGET_RDMA);

using namespace std::string_literals;

namespace {

auto constexpr app_name = "doca_storage_target_rdma";

auto constexpr default_storage_block_size = 4096;
auto constexpr default_storage_block_count = 128;

static_assert(sizeof(void *) == 8, "Expected a pointer to occupy 8 bytes");

auto constexpr rdma_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
				  DOCA_ACCESS_FLAG_RDMA_WRITE;

/*
 * User configurable parameters for the target_rdma_app
 */
struct target_rdma_app_configuration {
	std::vector<uint32_t> core_set = {};
	std::string device_id = {};
	std::string storage_content_file_name = {};
	uint32_t block_count = {};
	uint32_t block_size = {};
	uint16_t listen_port = {};
	std::vector<uint8_t> content = {};
};

/*
 * Statistics emitted by each thread
 */
struct target_rdma_worker_stats {
	uint16_t core_idx = 0;
	uint64_t pe_hit_count = 0;
	uint64_t pe_miss_count = 0;
	uint64_t operation_count = 0;
};

/*
 * Context for a transaction (io request => data transfer => io response)
 */
struct alignas(storage::cache_line_size / 2) transfer_context {
	doca_rdma_task_write *write_task = nullptr;
	doca_rdma_task_read *read_task = nullptr;
	doca_buf *host_buf = nullptr;
	doca_buf *storage_buf = nullptr;
};

struct worker_control_command {
	enum class type {
		create_objects,
		create_rdma_connection,
		are_contexts_ready,
		prepare_tasks,
		start_data_path,
		abort_thread,
	};

	worker_control_command::type cmd_type;

	virtual ~worker_control_command() = default;
	worker_control_command() = delete;
	explicit worker_control_command(worker_control_command::type cmd_type_) : cmd_type{cmd_type_}
	{
	}
	worker_control_command(worker_control_command const &) = default;
	worker_control_command(worker_control_command &&) noexcept = default;
	worker_control_command &operator=(worker_control_command const &) = default;
	worker_control_command &operator=(worker_control_command &&) noexcept = default;
};

char const *to_string(worker_control_command::type cmd_type)
{
	switch (cmd_type) {
	case worker_control_command::type::create_objects:
		return "create_objects";
	case worker_control_command::type::create_rdma_connection:
		return "create_rdma_connection";
	case worker_control_command::type::prepare_tasks:
		return "prepare_tasks";
	case worker_control_command::type::abort_thread:
		return "abort_thread";
	default:
		return "UNKNOWN";
	}
}

struct worker_create_objects_control_command : public worker_control_command {
	/* Device to use */
	doca_dev *dev;
	/* Number of transactions to create */
	uint32_t transaction_count;
	/* Remote memory mmap to read from / store into */
	doca_mmap *remote_mmap;
	/* Local memory mmap representing the full local storage memory range */
	doca_mmap *local_mmap;

	~worker_create_objects_control_command() override = default;
	worker_create_objects_control_command() = delete;
	worker_create_objects_control_command(doca_dev *dev_,
					      uint32_t transaction_count_,
					      doca_mmap *remote_mmap_,
					      doca_mmap *local_mmap_)
		: worker_control_command{worker_control_command::type::create_objects},
		  dev{dev_},
		  transaction_count{transaction_count_},
		  remote_mmap{remote_mmap_},
		  local_mmap{local_mmap_}
	{
	}
	worker_create_objects_control_command(worker_create_objects_control_command const &) = default;
	worker_create_objects_control_command(worker_create_objects_control_command &&) noexcept = default;
	worker_create_objects_control_command &operator=(worker_create_objects_control_command const &) = default;
	worker_create_objects_control_command &operator=(worker_create_objects_control_command &&) noexcept = default;
};

struct worker_create_rdma_connection_command : public worker_control_command {
	/* The purpose of this connection */
	storage::control::rdma_connection_role rdma_role;
	/* Blob from the remote side of the RDMA connection to import */
	std::vector<uint8_t> import_blob;
	/* Blob exported from the local side of the RDMA connection to be imported by the remote side. (Value set during
	 * execution of the command, caller should only access it after the command completes successfully) */
	std::vector<uint8_t> out_exported_blob;

	~worker_create_rdma_connection_command() override = default;
	worker_create_rdma_connection_command() = delete;
	explicit worker_create_rdma_connection_command(storage::control::rdma_connection_role rdma_role_,
						       std::vector<uint8_t> import_blob_)
		: worker_control_command{worker_control_command::type::create_rdma_connection},
		  rdma_role{rdma_role_},
		  import_blob{std::move(import_blob_)},
		  out_exported_blob{}
	{
	}
	worker_create_rdma_connection_command(worker_create_rdma_connection_command const &) = default;
	worker_create_rdma_connection_command(worker_create_rdma_connection_command &&) noexcept = default;
	worker_create_rdma_connection_command &operator=(worker_create_rdma_connection_command const &) = default;
	worker_create_rdma_connection_command &operator=(worker_create_rdma_connection_command &&) noexcept = default;
};

struct worker_are_contexts_ready_control_command : public worker_control_command {
	/*
	 * Contexts status:
	 *  - DOCA_SUCCESS : When all contexts are ready to perform data path operations
	 *  - DOCA_ERROR_AGAIN : When one or more context is not ready yet
	 *  Any other DOCA_ERROR_XXX indicates an error has occurred.
	 */
	doca_error_t out_status = DOCA_ERROR_UNKNOWN;

	~worker_are_contexts_ready_control_command() override = default;
	worker_are_contexts_ready_control_command()
		: worker_control_command{worker_control_command::type::are_contexts_ready}
	{
	}
	worker_are_contexts_ready_control_command(worker_are_contexts_ready_control_command const &) = default;
	worker_are_contexts_ready_control_command(worker_are_contexts_ready_control_command &&) noexcept = default;
	worker_are_contexts_ready_control_command &operator=(worker_are_contexts_ready_control_command const &) =
		default;
	worker_are_contexts_ready_control_command &operator=(worker_are_contexts_ready_control_command &&) noexcept =
		default;
};

/*
 * Data required for a thread worker
 */
class target_rdma_worker {
public:
	/*
	 * A set of data that can be used in the data path, NO OTHER MEMORY SHOULD BE ACCESSED in the main loop or task
	 * callbacks. This is done to keep the maximum amount of useful data resident in the cache while avoiding as
	 * many cache evictions as possible.
	 */
	struct alignas(storage::cache_line_size) hot_data {
		doca_pe *pe = nullptr;
		doca_buf_inventory *buf_inv = nullptr;
		doca_mmap *local_mmap = nullptr;
		doca_mmap *remote_mmap = nullptr;
		uint64_t pe_hit_count = 0;
		uint64_t pe_miss_count = 0;
		char *remote_memory_start_addr = nullptr;
		char *local_memory_start_addr = nullptr;
		uint64_t completed_transaction_count = 0;
		uint32_t in_flight_transaction_count = 0;
		uint16_t core_idx = 0;
		std::atomic_bool run_flag = false;
		bool error_flag = false;
	};
	static_assert(sizeof(target_rdma_worker::hot_data) == storage::cache_line_size * 2,
		      "Expected target_rdma_worker::hot_data to occupy two cache lines");

	/*
	 * Destructor
	 */
	~target_rdma_worker();
	/*
	 * Create thread proc
	 * @core_idx [in]: Core to run on
	 */
	void create_thread_proc(uint16_t core_idx);

	/*
	 * Command interface, execute a given command.
	 *
	 * @cmd [in]: Command to execute
	 * @return Result of the command
	 */
	doca_error_t execute_control_command(worker_control_command &cmd);

	/*
	 * Join the work thread
	 */
	void join_thread_proc(void) noexcept;

	/*
	 * Get The workers stats
	 *
	 * @return stats;
	 */
	target_rdma_worker_stats get_stats() const noexcept;

private:
	/* Hot data - Cache aligned container of data and objects required for use on the data path. */
	hot_data *m_hot_data = nullptr;

	/**************************************************************************************************************
	 * Control data / Ownership data
	 * The cold path data is used to configure and prepare for the host path. These objects and data must be
	 * maintained to allow for teardown / destruction later
	 */
	/* Async controller. Used to allow execution of control commands within the worker thread proc */
	storage::control::worker_async<worker_control_command> m_async_ctrl;
	uint8_t *m_io_message_region = nullptr;
	doca_mmap *m_io_message_mmap = nullptr;
	doca_buf_inventory *m_buf_inv = nullptr;
	std::vector<doca_buf *> m_bufs = {};
	doca_pe *m_pe = nullptr;
	storage::rdma_conn_pair m_rdma_ctrl_ctx = {};
	storage::rdma_conn_pair m_rdma_data_ctx = {};
	doca_mmap *m_local_mmap = nullptr;
	doca_mmap *m_remote_mmap = nullptr;
	uint32_t m_transaction_count = 0;
	transfer_context *m_transfer_contexts = nullptr;
	std::vector<doca_task *> m_request_tasks = {};
	std::vector<doca_task *> m_response_tasks = {};
	std::vector<doca_task *> m_data_tasks = {};
	std::thread m_thread = {};

	/*
	 * Implementation of execute control command that runs within the worker thread proc
	 *
	 * @cmd [in]: Command to execute
	 *
	 * @return true if the worker is ready to start the data path, false otherwise
	 */
	bool execute_control_command_impl(worker_control_command &cmd) noexcept;

	/*
	 * Create the objects required by the worker.
	 *
	 * @cmd [in]: Command object describing the settings for the objects.
	 */
	void create_worker_objects(worker_create_objects_control_command const &cmd);

	/*
	 * Export the connection block for a given RDMA context
	 *
	 * @cmd [in/out]: Command containing the required input data to perfm the request, and storage for the output
	 * data
	 */
	void create_rdma_connection(worker_create_rdma_connection_command &cmd);

	/*
	 * Check that all contexts are ready to run
	 *
	 * @cmd [in]: Command object holding the out param to populate
	 */
	void are_contexts_ready(worker_are_contexts_ready_control_command &cmd) const noexcept;

	/*
	 * Prepare task objects
	 */
	void prepare_tasks(void);

	/*
	 * Start data path operations.
	 */
	void start_data_path(void);

	/*
	 * RDMA task send callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_rdma_task_send_cb(doca_rdma_task_send *task,
					   doca_data task_user_data,
					   doca_data ctx_user_data) noexcept;

	/*
	 * RDMA task send error callback
	 *
	 * @task [in]: Failed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_rdma_task_send_error_cb(doca_rdma_task_send *task,
						 doca_data task_user_data,
						 doca_data ctx_user_data) noexcept;

	/*
	 * RDMA task receive callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_rdma_task_receive_cb(doca_rdma_task_receive *task,
					      doca_data task_user_data,
					      doca_data ctx_user_data) noexcept;

	/*
	 * RDMA task receive error callback
	 *
	 * @task [in]: Failed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void doca_rdma_task_receive_error_cb(doca_rdma_task_receive *task,
						    doca_data task_user_data,
						    doca_data ctx_user_data) noexcept;

	/*
	 * Shared RDMA read/write callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void on_transfer_complete(doca_task *task, doca_data task_user_data, doca_data ctx_user_data) noexcept;

	/*
	 * Shared RDMA read/write error callback
	 *
	 * @task [in]: Failed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void on_transfer_error(doca_task *task, doca_data task_user_data, doca_data ctx_user_data) noexcept;

	/*
	 * Thread entry point
	 *
	 * @self [in]: Pointer to self
	 * @core_idx [in]: Core this thread will on
	 */
	static void thread_proc(target_rdma_worker *self, uint16_t core_idx) noexcept;

	/*
	 * Data path routine
	 *
	 * @hot_data [in]: Reference to hot_data
	 */
	static void run_data_path_ops(target_rdma_worker::hot_data &hot_data);
};

/*
 * top level target_rdma_app structure
 */
class target_rdma_app {
public:
	~target_rdma_app();
	target_rdma_app() = delete;
	explicit target_rdma_app(target_rdma_app_configuration const &cfg);
	target_rdma_app(target_rdma_app const &) = delete;
	target_rdma_app(target_rdma_app &&) noexcept = delete;
	target_rdma_app &operator=(target_rdma_app const &) = delete;
	target_rdma_app &operator=(target_rdma_app &&) noexcept = delete;

	void abort(std::string const &reason);

	void wait_for_client_connection(void);
	void wait_for_and_process_query_storage(void);
	void wait_for_and_process_init_storage(void);
	void wait_for_and_process_create_rdma_connections(void);
	void wait_for_and_process_start_storage(void);
	void wait_for_and_process_stop_storage(void);
	void wait_for_and_process_shutdown(void);
	void display_stats(void) const;

private:
	target_rdma_app_configuration const m_cfg;
	doca_dev *m_dev;
	std::unique_ptr<storage::control::channel> m_control_channel;
	std::vector<storage::control::message> m_ctrl_messages;
	uint8_t *m_local_io_region;
	uint64_t m_local_io_region_size;
	doca_mmap *m_local_io_mmap;
	doca_mmap *m_remote_io_mmap;
	target_rdma_worker *m_workers;
	std::vector<target_rdma_worker_stats> m_stats;
	uint32_t m_storage_block_count;
	uint32_t m_storage_block_size;
	uint32_t m_transaction_count;
	uint32_t m_core_count;
	bool m_abort_flag;

	/*
	 * Allocate and prepare resources
	 */
	void init(void);

	/*
	 * Release resources
	 */
	void cleanup(void) noexcept;

	/*
	 * Wait for a control message
	 *
	 * @return: control message
	 */
	storage::control::message wait_for_control_message();

	/*
	 * Process a query storage request
	 *
	 * @client_request [in]: Request
	 * @return: Response
	 */
	storage::control::message process_query_storage(storage::control::message const &client_request);
	/*
	 * Process a init storage request
	 *
	 * @client_request [in]: Request
	 * @return:Response
	 */
	storage::control::message process_init_storage(storage::control::message const &client_request);

	/*
	 * Process a create RDMA connection request
	 *
	 * @client_request [in]: Request
	 * @return:Response
	 */
	storage::control::message process_create_rdma_connection(storage::control::message const &client_request);

	/*
	 * Process a start storage request
	 *
	 * @client_request [in]: Request
	 * @return:Response
	 */
	storage::control::message process_start_storage(storage::control::message const &client_request);

	/*
	 * Process a stop storage request
	 *
	 * @client_request [in]: Request
	 * @return:Response
	 */
	storage::control::message process_stop_storage(storage::control::message const &client_request);

	/*
	 * Process a shutdown request
	 *
	 * @client_request [in]: Request
	 * @return:Response
	 */
	storage::control::message process_shutdown(storage::control::message const &client_request);

	/*
	 * Prepare worker objects
	 */
	void prepare_workers();

	/*
	 * Destroy workers
	 */
	void destroy_workers() noexcept;

	/*
	 * Verify that RDMA connections are ready
	 */
	void verify_connections_are_ready(void);
};

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed target_rdma_app_configuration
 *
 * @throws: storage::runtime_error If the target_rdma_app_configuration cannot pe parsed or contains invalid values
 */
target_rdma_app_configuration parse_target_rdma_app_cli_args(int argc, char **argv);

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

	try {
		target_rdma_app app{parse_target_rdma_app_cli_args(argc, argv)};
		storage::install_ctrl_c_handler([&app]() {
			app.abort("User requested abort");
		});

		app.wait_for_client_connection();
		app.wait_for_and_process_query_storage();
		app.wait_for_and_process_init_storage();
		app.wait_for_and_process_create_rdma_connections();
		app.wait_for_and_process_start_storage();
		app.wait_for_and_process_stop_storage();
		app.wait_for_and_process_shutdown();
		app.display_stats();
	} catch (std::exception const &ex) {
		fprintf(stderr, "EXCEPTION: %s\n", ex.what());
		fflush(stdout);
		fflush(stderr);
		return EXIT_FAILURE;
	}

	storage::uninstall_ctrl_c_handler();

	return EXIT_SUCCESS;
}

namespace {

/*
 * Print the parsed target_rdma_app_configuration
 *
 * @cfg [in]: target_rdma_app_configuration to display
 */
void print_config(target_rdma_app_configuration const &cfg) noexcept
{
	printf("target_rdma_app_configuration: {\n");
	printf("\tcore_set : [");
	bool first = true;
	for (auto cpu : cfg.core_set) {
		if (first)
			first = false;
		else
			printf(", ");
		printf("%u", cpu);
	}
	printf("]\n");
	printf("\tdevice : \"%s\",\n", cfg.device_id.c_str());
	printf("\tstorage_content_file_name : \"%s\",\n", cfg.storage_content_file_name.c_str());
	printf("\tlisten_port : %u\n", cfg.listen_port);
	printf("\tblock_count : %u\n", cfg.block_count);
	printf("\tblock_size : %u\n", cfg.block_size);
	printf("}\n");
}

/*
 * Validate target_rdma_app_configuration
 *
 * @cfg [in]: target_rdma_app_configuration
 */
void validate_target_rdma_app_configuration(target_rdma_app_configuration const &cfg)
{
	std::vector<std::string> errors;

	if (cfg.storage_content_file_name.empty() && (cfg.block_size == 0 || cfg.block_count == 0)) {
		errors.emplace_back(
			"Invalid target_rdma_app_configuration: block-size and block-count must be non zero when binary-content is not provided");
	}

	if (!errors.empty()) {
		for (auto const &err : errors) {
			printf("%s\n", err.c_str());
		}
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Invalid target_rdma_app_configuration detected"};
	}
}

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed target_rdma_app_configuration
 *
 * @throws: storage::runtime_error If the target_rdma_app_configuration cannot pe parsed or contains invalid values
 */
target_rdma_app_configuration parse_target_rdma_app_cli_args(int argc, char **argv)
{
	target_rdma_app_configuration config{};
	config.block_count = default_storage_block_count;
	config.block_size = default_storage_block_size;

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
					       static_cast<target_rdma_app_configuration *>(cfg)->device_id =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_INT,
				       nullptr,
				       "cpu",
				       "CPU core to which the process affinity can be set",
				       storage::required_value,
				       storage::multiple_values,
				       [](void *value, void *cfg) noexcept {
					       static_cast<target_rdma_app_configuration *>(cfg)->core_set.push_back(
						       *static_cast<int *>(value));
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_INT,
				       nullptr,
				       "listen-port",
				       "TCP listen port number",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       auto inv_val = *static_cast<int *>(value);
					       auto short_val = static_cast<uint16_t>(inv_val);

					       if (inv_val != short_val)
						       return DOCA_ERROR_INVALID_VALUE;

					       static_cast<target_rdma_app_configuration *>(cfg)->listen_port =
						       short_val;
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"binary-content",
		"Path to binary .sbc file containing the initial content to be represented by this storage instance",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<target_rdma_app_configuration *>(cfg)->storage_content_file_name =
				static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"block-count",
		"Number of available storage blocks. (Ignored when using content binary file) Default: 128",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<target_rdma_app_configuration *>(cfg)->block_count =
				*static_cast<uint32_t *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_INT,
		nullptr,
		"block-size",
		"Block size used by the storage. (Ignored when using content binary file) Default: 4096",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<target_rdma_app_configuration *>(cfg)->block_size = *static_cast<uint32_t *>(value);
			return DOCA_SUCCESS;
		});
	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args"};
	}

	static_cast<void>(doca_argp_destroy());

	if (!config.storage_content_file_name.empty()) {
		if (storage::file_has_binary_content_header(config.storage_content_file_name)) {
			auto sbc = storage::load_binary_content_from_file(config.storage_content_file_name);
			config.block_count = sbc.block_count;
			config.block_size = sbc.block_size;
			config.content = std::move(sbc.content);
		} else {
			config.content = storage::load_file_bytes(config.storage_content_file_name);
			auto const expected_content_size = uint64_t{config.block_size} * config.block_count;
			if (config.content.size() != expected_content_size) {
				throw storage::runtime_error{
					DOCA_ERROR_INVALID_VALUE,
					"Selected input data file content : " + config.storage_content_file_name +
						" : " + std::to_string(config.content.size()) +
						" bytes does not match the storage size of " +
						std::to_string(expected_content_size) + " bytes"};
			}
		}
	}

	print_config(config);
	validate_target_rdma_app_configuration(config);

	return config;
}

target_rdma_worker::~target_rdma_worker()
{
	if (m_thread.joinable()) {
		DOCA_LOG_WARN("Worker Data path thread was still running during destruction");
		if (m_hot_data != nullptr)
			m_hot_data->error_flag = true;
		join_thread_proc();
	}

	doca_error_t ret;

	if (m_rdma_ctrl_ctx.rdma != nullptr) {
		/* stop context with tasks list (tasks must be destroyed to finish stopping process) */
		std::vector<doca_task *> all_ctrl_tasks;
		all_ctrl_tasks.reserve(m_request_tasks.size() + m_response_tasks.size());
		std::copy(std::begin(m_request_tasks), std::end(m_request_tasks), std::back_inserter(all_ctrl_tasks));
		std::copy(std::begin(m_response_tasks), std::end(m_response_tasks), std::back_inserter(all_ctrl_tasks));
		ret = storage::stop_context(doca_rdma_as_ctx(m_rdma_ctrl_ctx.rdma), m_pe, all_ctrl_tasks);
		if (ret == DOCA_SUCCESS) {
			m_request_tasks.clear();
			m_response_tasks.clear();
		} else {
			DOCA_LOG_ERR("Failed to stop rdma control context: %s", doca_error_get_name(ret));
		}

		ret = doca_rdma_destroy(m_rdma_ctrl_ctx.rdma);
		if (ret == DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy rdma control context: %s", doca_error_get_name(ret));
		}
	}

	if (m_rdma_data_ctx.rdma != nullptr) {
		/* stop context with tasks list (tasks must be destroyed to finish stopping process) */
		ret = storage::stop_context(doca_rdma_as_ctx(m_rdma_data_ctx.rdma), m_pe, m_data_tasks);
		if (ret == DOCA_SUCCESS) {
			m_data_tasks.clear();
		} else {
			DOCA_LOG_ERR("Failed to stop rdma data context: %s", doca_error_get_name(ret));
		}

		ret = doca_rdma_destroy(m_rdma_data_ctx.rdma);
		if (ret == DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy rdma data context: %s", doca_error_get_name(ret));
		}
	}

	if (m_pe != nullptr) {
		ret = doca_pe_destroy(m_pe);
		if (ret == DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine");
		}
	}

	if (m_transfer_contexts != nullptr) {
		for (uint32_t ii = 0; ii != m_transaction_count; ++ii) {
			static_cast<void>(doca_buf_dec_refcount(m_transfer_contexts[ii].storage_buf, nullptr));
			static_cast<void>(doca_buf_dec_refcount(m_transfer_contexts[ii].host_buf, nullptr));
		}

		storage::aligned_free(m_transfer_contexts);
		m_transfer_contexts = nullptr;
	}

	for (auto *buf : m_bufs) {
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
	}

	if (m_buf_inv) {
		ret = doca_buf_inventory_stop(m_buf_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop buffer inventory");
		}
		ret = doca_buf_inventory_destroy(m_buf_inv);
		if (ret == DOCA_SUCCESS) {
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

	if (m_io_message_region != nullptr) {
		storage::aligned_free(m_io_message_region);
	}

	storage::aligned_free(m_hot_data);
}

void target_rdma_worker::create_thread_proc(uint16_t core_idx)
{
	m_thread = std::thread{thread_proc, this, core_idx};
}

doca_error_t target_rdma_worker::execute_control_command(worker_control_command &cmd)
{
	return storage::control::execute_worker_command(m_async_ctrl, &cmd, std::chrono::seconds{3});
}

void target_rdma_worker::join_thread_proc(void) noexcept
{
	if (!m_thread.joinable())
		return;

	if (m_hot_data == nullptr || m_hot_data->run_flag == false) {
		/* if the data path has not yet started it needs to receive a message to break out, so send an
		 * abort control message */
		worker_control_command cmd{worker_control_command::type::abort_thread};
		static_cast<void>(execute_control_command(cmd));
	} else {
		/* if the thread is running the data path setting the run flag to false will trigger it to stop
		 */
		m_hot_data->run_flag = false;
	}

	m_thread.join();
}

target_rdma_worker_stats target_rdma_worker::get_stats() const noexcept
{
	target_rdma_worker_stats stats{};

	if (m_hot_data != nullptr) {
		stats.core_idx = m_hot_data->core_idx;
		stats.pe_hit_count = m_hot_data->pe_hit_count;
		stats.pe_miss_count = m_hot_data->pe_miss_count;
		stats.operation_count = m_hot_data->completed_transaction_count;
	}

	return stats;
}

bool target_rdma_worker::execute_control_command_impl(worker_control_command &cmd) noexcept
{
	doca_error_t cmd_result;
	bool control_path_completed = false;

	try {
		cmd_result = DOCA_SUCCESS;
		switch (cmd.cmd_type) {
		case worker_control_command::type::create_objects:
			create_worker_objects(dynamic_cast<worker_create_objects_control_command &>(cmd));
			break;
		case worker_control_command::type::create_rdma_connection:
			create_rdma_connection(dynamic_cast<worker_create_rdma_connection_command &>(cmd));
			break;
		case worker_control_command::type::are_contexts_ready:
			are_contexts_ready(dynamic_cast<worker_are_contexts_ready_control_command &>(cmd));
			break;
		case worker_control_command::type::prepare_tasks:
			prepare_tasks();
			break;
		case worker_control_command::type::start_data_path:
			start_data_path();
			control_path_completed = true;
			break;
		default:
			DOCA_LOG_ERR("Received un handled command: %s", to_string(cmd.cmd_type));
			cmd_result = DOCA_ERROR_INVALID_VALUE;
		}
	} catch (storage::runtime_error const &ex) {
		DOCA_LOG_ERR("%s Failed: %s:%s",
			     to_string(cmd.cmd_type),
			     doca_error_get_name(ex.get_doca_error()),
			     ex.what());
		cmd_result = ex.get_doca_error();
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("%s Failed: %s", to_string(cmd.cmd_type), ex.what());
		cmd_result = DOCA_ERROR_UNEXPECTED;
	}

	m_async_ctrl.set_result(cmd_result);

	return control_path_completed;
}

void target_rdma_worker::create_worker_objects(worker_create_objects_control_command const &cmd)
{
	doca_error_t ret;
	auto const page_size = storage::get_system_page_size();

	m_transaction_count = cmd.transaction_count;
	m_remote_mmap = cmd.remote_mmap;
	m_local_mmap = cmd.local_mmap;

	auto const raw_io_messages_size = m_transaction_count * storage::size_of_io_message * 2;
	m_io_message_region = static_cast<uint8_t *>(
		storage::aligned_alloc(page_size, storage::aligned_size(page_size, raw_io_messages_size)));
	if (m_io_message_region == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate comch fast path buffers memory"};
	}

	m_io_message_mmap = storage::make_mmap(cmd.dev,
					       reinterpret_cast<char *>(m_io_message_region),
					       raw_io_messages_size,
					       rdma_permissions,
					       storage::thread_safety::no);

	ret = doca_buf_inventory_create(m_transaction_count * 3, &m_buf_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create comch fast path doca_buf_inventory"};
	}
	ret = doca_buf_inventory_start(m_buf_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start comch fast path doca_buf_inventory"};
	}

	ret = doca_pe_create(std::addressof(m_pe));
	m_hot_data->pe = m_pe;
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_pe"};
	}

	m_rdma_ctrl_ctx.rdma =
		storage::make_rdma_context(cmd.dev, m_pe, doca_data{.ptr = m_hot_data}, rdma_permissions);

	ret = doca_rdma_task_receive_set_conf(m_rdma_ctrl_ctx.rdma,
					      doca_rdma_task_receive_cb,
					      doca_rdma_task_receive_error_cb,
					      m_transaction_count);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to configure rdma receive task pool"};
	}

	ret = doca_rdma_task_send_set_conf(m_rdma_ctrl_ctx.rdma,
					   doca_rdma_task_send_cb,
					   doca_rdma_task_send_error_cb,
					   m_transaction_count);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to configure rdma send task pool"};
	}

	ret = doca_ctx_start(doca_rdma_as_ctx(m_rdma_ctrl_ctx.rdma));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_rdma context"};
	}

	m_rdma_data_ctx.rdma =
		storage::make_rdma_context(cmd.dev, m_pe, doca_data{.ptr = m_hot_data}, rdma_permissions);

	ret = doca_rdma_task_read_set_conf(m_rdma_data_ctx.rdma,
					   reinterpret_cast<doca_rdma_task_read_completion_cb_t>(on_transfer_complete),
					   reinterpret_cast<doca_rdma_task_read_completion_cb_t>(on_transfer_error),
					   m_transaction_count);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{
			ret,
			"Failed to set doca_rdma_task_read task pool target_rdma_app_configuration"};
	}

	ret = doca_rdma_task_write_set_conf(
		m_rdma_data_ctx.rdma,
		reinterpret_cast<doca_rdma_task_write_completion_cb_t>(on_transfer_complete),
		reinterpret_cast<doca_rdma_task_write_completion_cb_t>(on_transfer_error),
		m_transaction_count);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{
			ret,
			"Failed to set doca_rdma_task_write task pool target_rdma_app_configuration"};
	}

	ret = doca_ctx_start(doca_rdma_as_ctx(m_rdma_data_ctx.rdma));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_rdma context"};
	}
}

void target_rdma_worker::create_rdma_connection(worker_create_rdma_connection_command &cmd)
{
	auto &rdma_pair = cmd.rdma_role == storage::control::rdma_connection_role::io_data ? m_rdma_data_ctx :
											     m_rdma_ctrl_ctx;

	cmd.out_exported_blob = [this, &rdma_pair]() {
		uint8_t const *blob = nullptr;
		size_t blob_size = 0;

		auto const ret = doca_rdma_export(rdma_pair.rdma,
						  reinterpret_cast<void const **>(&blob),
						  &blob_size,
						  std::addressof(rdma_pair.conn));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Core: %u RDMA export failed: %s", m_hot_data->core_idx, doca_error_get_name(ret));
			throw storage::runtime_error{ret, "Failed to export rdma connection"};
		}
		return std::vector<uint8_t>{blob, blob + blob_size};
	}();

	auto const ret =
		doca_rdma_connect(rdma_pair.rdma, cmd.import_blob.data(), cmd.import_blob.size(), rdma_pair.conn);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Core: %u RDMA connect failed: %s", m_hot_data->core_idx, doca_error_get_name(ret));
		throw storage::runtime_error{ret, "Failed to connect to rdma"};
	}
}

void target_rdma_worker::are_contexts_ready(worker_are_contexts_ready_control_command &cmd) const noexcept
{
	doca_ctx_states rdma_state;
	bool ctrl_connected = false;
	bool data_connected = false;

	cmd.out_status = doca_ctx_get_state(doca_rdma_as_ctx(m_rdma_ctrl_ctx.rdma), &rdma_state);
	if (cmd.out_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(cmd.out_status));
		return;
	} else if (rdma_state == DOCA_CTX_STATE_RUNNING) {
		ctrl_connected = true;
	} else {
		static_cast<void>(doca_pe_progress(m_pe));
	}

	cmd.out_status = doca_ctx_get_state(doca_rdma_as_ctx(m_rdma_data_ctx.rdma), &rdma_state);
	if (cmd.out_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(cmd.out_status));
		return;
	} else if (rdma_state == DOCA_CTX_STATE_RUNNING) {
		data_connected = true;
	} else {
		static_cast<void>(doca_pe_progress(m_pe));
	}

	cmd.out_status = ctrl_connected && data_connected ? DOCA_SUCCESS : DOCA_ERROR_IN_PROGRESS;
}

void target_rdma_worker::prepare_tasks(void)
{
	doca_error_t ret;
	uint8_t *message_buffer_addr = m_io_message_region;
	size_t local_memory_size = 0;
	size_t remote_memory_size = 0;
	m_hot_data->buf_inv = m_buf_inv;
	m_hot_data->local_mmap = m_local_mmap;
	m_hot_data->remote_mmap = m_remote_mmap;
	static_cast<void>(doca_mmap_get_memrange(m_local_mmap,
						 reinterpret_cast<void **>(&m_hot_data->local_memory_start_addr),
						 &local_memory_size));
	static_cast<void>(doca_mmap_get_memrange(m_remote_mmap,
						 reinterpret_cast<void **>(&m_hot_data->remote_memory_start_addr),
						 &remote_memory_size));

	m_request_tasks.reserve(m_transaction_count);
	m_response_tasks.reserve(m_transaction_count);
	m_data_tasks.reserve(m_transaction_count * 2);
	m_bufs.reserve(m_transaction_count * 3);

	m_transfer_contexts = storage::make_aligned<transfer_context>{}.object_array(m_transaction_count);

	for (uint32_t ii = 0; ii != m_transaction_count; ++ii) {
		doca_buf *message_buf;

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_io_message_mmap,
							 message_buffer_addr,
							 storage::size_of_io_message,
							 &message_buf);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate io_message doca_buf"};
		}

		m_bufs.push_back(message_buf);
		message_buffer_addr += storage::size_of_io_message;

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_local_mmap,
							 m_hot_data->local_memory_start_addr,
							 local_memory_size,
							 std::addressof(m_transfer_contexts[ii].storage_buf));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate local storage doca_buf"};
		}

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_remote_mmap,
							 m_hot_data->remote_memory_start_addr,
							 remote_memory_size,
							 std::addressof(m_transfer_contexts[ii].host_buf));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate remote storage doca_buf"};
		}

		doca_rdma_task_receive *request_task = nullptr;
		ret = doca_rdma_task_receive_allocate_init(m_rdma_ctrl_ctx.rdma,
							   message_buf,
							   doca_data{.ptr = std::addressof(m_transfer_contexts[ii])},
							   &request_task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate doca_rdma_task_receive"};
		}
		m_request_tasks.push_back(doca_rdma_task_receive_as_task(request_task));

		doca_rdma_task_send *response_task = nullptr;
		ret = doca_rdma_task_send_allocate_init(m_rdma_ctrl_ctx.rdma,
							m_rdma_ctrl_ctx.conn,
							message_buf,
							doca_data{.ptr = request_task},
							&response_task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate doca_rdma_task_send"};
		}
		m_response_tasks.push_back(doca_rdma_task_send_as_task(response_task));

		ret = doca_rdma_task_write_allocate_init(m_rdma_data_ctx.rdma,
							 m_rdma_data_ctx.conn,
							 m_transfer_contexts[ii].storage_buf,
							 m_transfer_contexts[ii].host_buf,
							 doca_data{.ptr = response_task},
							 std::addressof(m_transfer_contexts[ii].write_task));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate doca_rdma_task_write"};
		}
		m_data_tasks.push_back(doca_rdma_task_write_as_task(m_transfer_contexts[ii].write_task));

		ret = doca_rdma_task_read_allocate_init(m_rdma_data_ctx.rdma,
							m_rdma_data_ctx.conn,
							m_transfer_contexts[ii].host_buf,
							m_transfer_contexts[ii].storage_buf,
							doca_data{.ptr = response_task},
							std::addressof(m_transfer_contexts[ii].read_task));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate doca_rdma_task_read"};
		}
		m_data_tasks.push_back(doca_rdma_task_read_as_task(m_transfer_contexts[ii].read_task));
	}
}

void target_rdma_worker::start_data_path()
{
	m_hot_data->run_flag = true;
}

void target_rdma_worker::doca_rdma_task_send_cb(doca_rdma_task_send *task,
						doca_data task_user_data,
						doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);

	auto *const request_task = static_cast<doca_rdma_task_receive *>(task_user_data.ptr);

	doca_buf_reset_data_len(doca_rdma_task_receive_get_dst_buf(request_task));
	auto const ret = doca_task_submit(doca_rdma_task_receive_as_task(request_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed re-submit request task: %s", doca_error_get_name(ret));
	}

	auto *const hot_data = static_cast<target_rdma_worker::hot_data *>(ctx_user_data.ptr);
	--(hot_data->in_flight_transaction_count);
}

void target_rdma_worker::doca_rdma_task_send_error_cb(doca_rdma_task_send *task,
						      doca_data task_user_data,
						      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	DOCA_LOG_ERR("Failed to complete doca_rdma_task_send");

	auto *const hot_data = static_cast<target_rdma_worker::hot_data *>(ctx_user_data.ptr);
	--(hot_data->in_flight_transaction_count);
	hot_data->run_flag = false;
	hot_data->error_flag = true;
}

void target_rdma_worker::doca_rdma_task_receive_cb(doca_rdma_task_receive *task,
						   doca_data task_user_data,
						   doca_data ctx_user_data) noexcept
{
	doca_error_t ret;
	auto *const hot_data = static_cast<target_rdma_worker::hot_data *>(ctx_user_data.ptr);

	auto *const io_message = storage::get_buffer_bytes(doca_rdma_task_receive_get_dst_buf(task));
	auto const message_type = storage::io_message_view::get_type(io_message);

	auto *const transfer_ctx = static_cast<transfer_context *>(task_user_data.ptr);

	char *const remote_addr =
		hot_data->remote_memory_start_addr + storage::io_message_view::get_requester_offset(io_message);
	char *const local_addr =
		hot_data->local_memory_start_addr + storage::io_message_view::get_storage_offset(io_message);
	uint32_t const transfer_size = storage::io_message_view::get_io_size(io_message);

	switch (message_type) {
	case storage::io_message_type::read: {
		static_cast<void>(
			doca_buf_inventory_buf_reuse_by_data(transfer_ctx->storage_buf, local_addr, transfer_size));
		static_cast<void>(
			doca_buf_inventory_buf_reuse_by_addr(transfer_ctx->host_buf, remote_addr, transfer_size));

		ret = doca_task_submit(doca_rdma_task_write_as_task(transfer_ctx->write_task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit doca_rdma_task_write: %s", doca_error_get_name(ret));
			break;
		}

		++(hot_data->in_flight_transaction_count);
		DOCA_LOG_TRC(
			"Start read(%p) of %u bytes from storage: %p to remote: %p (in_flight_transaction_count: %u)",
			transfer_ctx->write_task,
			transfer_size,
			local_addr,
			remote_addr,
			hot_data->in_flight_transaction_count);
	} break;
	case storage::io_message_type::write: {
		static_cast<void>(
			doca_buf_inventory_buf_reuse_by_data(transfer_ctx->host_buf, remote_addr, transfer_size));
		static_cast<void>(
			doca_buf_inventory_buf_reuse_by_addr(transfer_ctx->storage_buf, local_addr, transfer_size));

		ret = doca_task_submit(doca_rdma_task_read_as_task(transfer_ctx->read_task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit doca_rdma_task_read: %s", doca_error_get_name(ret));
			break;
		}

		++(hot_data->in_flight_transaction_count);
		DOCA_LOG_TRC(
			"Start write(%p) of %u bytes from remote: %p to storage: %p (in_flight_transaction_count: %u)",
			transfer_ctx->read_task,
			transfer_size,
			remote_addr,
			local_addr,
			hot_data->in_flight_transaction_count);
	} break;
	case storage::io_message_type::result:
	default:
		DOCA_LOG_ERR("Received message of unexpected type: %u", static_cast<uint32_t>(message_type));
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (ret == DOCA_SUCCESS)
		return;

	DOCA_LOG_ERR("Command error response: %s", storage::io_message_to_string(io_message).c_str());
	auto *const response_task = static_cast<doca_rdma_task_send *>(
		doca_task_get_user_data(doca_rdma_task_write_as_task(transfer_ctx->write_task)).ptr);

	storage::io_message_view::set_type(storage::io_message_type::result, io_message);
	storage::io_message_view::set_result(ret, io_message);

	ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
	}
}

void target_rdma_worker::doca_rdma_task_receive_error_cb(doca_rdma_task_receive *task,
							 doca_data task_user_data,
							 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<target_rdma_worker::hot_data *>(ctx_user_data.ptr);
	if (hot_data->run_flag) {
		DOCA_LOG_ERR("Failed to complete doca_rdma_task_receive");

		--(hot_data->in_flight_transaction_count);
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void target_rdma_worker::on_transfer_complete(doca_task *task,
					      doca_data task_user_data,
					      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);

	auto *const hot_data = static_cast<target_rdma_worker::hot_data *>(ctx_user_data.ptr);
	auto *const response_task = static_cast<doca_rdma_task_send *>(task_user_data.ptr);
	auto *const io_message =
		storage::get_buffer_bytes(const_cast<doca_buf *>(doca_rdma_task_send_get_src_buf(response_task)));

	++(hot_data->completed_transaction_count);

	storage::io_message_view::set_type(storage::io_message_type::result, io_message);
	storage::io_message_view::set_result(DOCA_SUCCESS, io_message);

	auto const ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
	}
}

void target_rdma_worker::on_transfer_error(doca_task *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *const hot_data = static_cast<target_rdma_worker::hot_data *>(ctx_user_data.ptr);
	auto *const response_task = static_cast<doca_rdma_task_send *>(task_user_data.ptr);
	auto *const io_message =
		storage::get_buffer_bytes(const_cast<doca_buf *>(doca_rdma_task_send_get_src_buf(response_task)));

	++(hot_data->completed_transaction_count);
	hot_data->error_flag = true;

	storage::io_message_view::set_type(storage::io_message_type::result, io_message);
	storage::io_message_view::set_result(DOCA_ERROR_IO_FAILED, io_message);

	auto const ret = doca_task_submit(doca_rdma_task_send_as_task(response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed submit response task: %s", doca_error_get_name(ret));
	}
}

void target_rdma_worker::thread_proc(target_rdma_worker *self, uint16_t core_idx) noexcept
{
	try {
		storage::set_thread_affinity(self->m_thread, core_idx);
		self->m_hot_data = storage::make_aligned<target_rdma_worker::hot_data>{}.object();
		DOCA_LOG_INFO("Worker: %p (core: %u) starting", self, core_idx);
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("Worker: %p (core: %u) Failed to initialise: %s", self, core_idx, ex.what());
		std::exit(EXIT_FAILURE);
	}
	self->m_hot_data->core_idx = core_idx;
	self->m_hot_data->run_flag = false;
	self->m_hot_data->error_flag = false;
	self->m_hot_data->pe_hit_count = 0;
	self->m_hot_data->pe_miss_count = 0;
	self->m_hot_data->completed_transaction_count = 0;
	self->m_hot_data->in_flight_transaction_count = 0;

	try {
		/* Configure, create objects and connect to remote objects */
		for (;;) {
			bool control_path_completed = false;
			self->m_async_ctrl.lock();
			worker_control_command *const cmd = self->m_async_ctrl.get_command();
			if (cmd != nullptr) {
				if (cmd->cmd_type == worker_control_command::type::abort_thread) {
					self->m_hot_data->error_flag = true;
					self->m_async_ctrl.unlock();
					return;
				}
				control_path_completed = self->execute_control_command_impl(*cmd);
				self->m_async_ctrl.unlock();
			} else {
				self->m_async_ctrl.unlock();
				std::this_thread::yield();
			}

			if (control_path_completed)
				break;
		}

		/* Submit initial tasks */
		for (auto *task : self->m_request_tasks) {
			auto const ret = doca_task_submit(task);
			if (ret != DOCA_SUCCESS) {
				throw storage::runtime_error{ret, "Unable to submit initial RDMA recv task"};
			}
		}

		/* Run data path operations */
		run_data_path_ops(*(self->m_hot_data));

	} catch (storage::runtime_error const &ex) {
		DOCA_LOG_ERR("Worker: %p, Exception: %s:%s", self, doca_error_get_name(ex.get_doca_error()), ex.what());
		self->m_hot_data->error_flag = true;
		self->m_hot_data->run_flag = false;
	}

	DOCA_LOG_INFO("Worker: %p exits", self);
}

void target_rdma_worker::run_data_path_ops(target_rdma_worker::hot_data &hot_data)
{
	DOCA_LOG_INFO("Core: %u running", hot_data.core_idx);

	while (hot_data.run_flag) {
		doca_pe_progress(hot_data.pe) ? ++(hot_data.pe_hit_count) : ++(hot_data.pe_miss_count);
	}

	while (hot_data.error_flag == false && hot_data.in_flight_transaction_count != 0) {
		doca_pe_progress(hot_data.pe) ? ++(hot_data.pe_hit_count) : ++(hot_data.pe_miss_count);
	}
}

target_rdma_app::~target_rdma_app()
{
	cleanup();
}

target_rdma_app::target_rdma_app(target_rdma_app_configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_control_channel{},
	  m_ctrl_messages{},
	  m_local_io_region{nullptr},
	  m_local_io_region_size{0},
	  m_local_io_mmap{nullptr},
	  m_remote_io_mmap{nullptr},
	  m_workers{},
	  m_stats{},
	  m_storage_block_count{},
	  m_storage_block_size{},
	  m_transaction_count{0},
	  m_core_count{0},
	  m_abort_flag{false}
{
	try {
		init();
	} catch (std::exception const &) {
		cleanup();
		throw;
	}
}

void target_rdma_app::abort(std::string const &reason)
{
	if (m_abort_flag)
		return;

	DOCA_LOG_ERR("Aborted: %s", reason.c_str());
	m_abort_flag = true;
}

void target_rdma_app::wait_for_client_connection(void)
{
	while (!m_control_channel->is_connected()) {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
	}
}

void target_rdma_app::wait_for_and_process_query_storage(void)
{
	DOCA_LOG_INFO("Wait for query storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::query_storage_request) {
		try {
			m_control_channel->send_message(process_query_storage(client_request));
			return;
		} catch (storage::runtime_error const &ex) {
			err_code = ex.get_doca_error();
			err_msg = ex.what();
		}
	} else {
		err_code = DOCA_ERROR_UNEXPECTED;
		err_msg = "Unexpected " + to_string(client_request.message_type) + " while expecting a " +
			  to_string(storage::control::message_type::query_storage_request);
	}

	m_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void target_rdma_app::wait_for_and_process_init_storage(void)
{
	DOCA_LOG_INFO("Wait for init storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::init_storage_request) {
		try {
			m_control_channel->send_message(process_init_storage(client_request));
			return;
		} catch (storage::runtime_error const &ex) {
			err_code = ex.get_doca_error();
			err_msg = ex.what();
		}
	} else {
		err_code = DOCA_ERROR_UNEXPECTED;
		err_msg = "Unexpected " + to_string(client_request.message_type) + " while expecting a " +
			  to_string(storage::control::message_type::init_storage_request);
	}

	m_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void target_rdma_app::wait_for_and_process_create_rdma_connections(void)
{
	DOCA_LOG_INFO("Wait for RDMA connections...");

	uint32_t remaining_connections = m_core_count * 2;
	while (remaining_connections != 0) {
		auto const client_request = wait_for_control_message();

		doca_error_t err_code = DOCA_ERROR_UNEXPECTED;
		std::string err_msg;

		if (client_request.message_type == storage::control::message_type::create_rdma_connection_request) {
			try {
				m_control_channel->send_message(process_create_rdma_connection(client_request));
				--remaining_connections;
				continue;
			} catch (storage::runtime_error const &ex) {
				err_code = ex.get_doca_error();
				err_msg = ex.what();
			}
		} else {
			err_code = DOCA_ERROR_UNEXPECTED;
			err_msg = "Unexpected " + to_string(client_request.message_type) + " while expecting a " +
				  to_string(storage::control::message_type::create_rdma_connection_request);
		}

		--remaining_connections;
		m_control_channel->send_message({
			storage::control::message_type::error_response,
			client_request.message_id,
			client_request.correlation_id,
			std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

		});

		if (m_abort_flag) {
			throw storage::runtime_error{DOCA_ERROR_INITIALIZATION,
						     "target_rdma_app aborted while preparing to start storage"};
		}
	}
}

void target_rdma_app::wait_for_and_process_start_storage(void)
{
	DOCA_LOG_INFO("Wait for start storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::start_storage_request) {
		try {
			m_control_channel->send_message(process_start_storage(client_request));
			return;
		} catch (storage::runtime_error const &ex) {
			err_code = ex.get_doca_error();
			err_msg = ex.what();
		}
	} else {
		err_code = DOCA_ERROR_UNEXPECTED;
		err_msg = "Unexpected " + to_string(client_request.message_type) + " while expecting a " +
			  to_string(storage::control::message_type::start_storage_request);
	}

	m_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void target_rdma_app::wait_for_and_process_stop_storage(void)
{
	DOCA_LOG_INFO("Wait for stop storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::stop_storage_request) {
		try {
			m_control_channel->send_message(process_stop_storage(client_request));
			return;
		} catch (storage::runtime_error const &ex) {
			err_code = ex.get_doca_error();
			err_msg = ex.what();
		}
	} else {
		err_code = DOCA_ERROR_UNEXPECTED;
		err_msg = "Unexpected " + to_string(client_request.message_type) + " while expecting a " +
			  to_string(storage::control::message_type::stop_storage_request);
	}

	m_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void target_rdma_app::wait_for_and_process_shutdown(void)
{
	DOCA_LOG_INFO("Wait for shutdown storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::shutdown_request) {
		try {
			m_control_channel->send_message(process_shutdown(client_request));
			return;
		} catch (storage::runtime_error const &ex) {
			err_code = ex.get_doca_error();
			err_msg = ex.what();
		}
	} else {
		err_code = DOCA_ERROR_UNEXPECTED;
		err_msg = "Unexpected " + to_string(client_request.message_type) + " while expecting a " +
			  to_string(storage::control::message_type::shutdown_request);
	}

	m_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void target_rdma_app::display_stats(void) const
{
	for (auto const &stats : m_stats) {
		auto const pe_hit_rate_pct =
			(static_cast<double>(stats.pe_hit_count) /
			 (static_cast<double>(stats.pe_hit_count) + static_cast<double>(stats.pe_miss_count))) *
			100.;

		printf("+================================================+\n");
		printf("| Core: %u\n", stats.core_idx);
		printf("| Operation count: %lu\n", stats.operation_count);
		printf("| PE hit rate: %2.03lf%% (%lu:%lu)\n", pe_hit_rate_pct, stats.pe_hit_count, stats.pe_miss_count);
	}
}

void target_rdma_app::init(void)
{
	m_dev = storage::open_device(m_cfg.device_id);

	m_storage_block_count = m_cfg.block_count;
	m_storage_block_size = m_cfg.block_size;

	auto const page_size = storage::get_system_page_size();
	m_local_io_region_size = uint64_t{m_storage_block_count} * m_storage_block_size;
	m_local_io_region = static_cast<uint8_t *>(storage::aligned_alloc(page_size, m_local_io_region_size));

	if (!m_cfg.content.empty()) {
		std::copy(std::begin(m_cfg.content), std::end(m_cfg.content), m_local_io_region);
	}

	m_control_channel = storage::control::make_tcp_server_control_channel(m_cfg.listen_port);
}

void target_rdma_app::cleanup(void) noexcept
{
	doca_error_t ret;

	m_control_channel.reset();
	destroy_workers();

	if (m_remote_io_mmap) {
		ret = doca_mmap_stop(m_remote_io_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop remote mmap");
		}
		ret = doca_mmap_destroy(m_remote_io_mmap);
		if (ret == DOCA_SUCCESS) {
			m_remote_io_mmap = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy remote mmap");
		}
	}

	if (m_local_io_mmap) {
		ret = doca_mmap_stop(m_local_io_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop local mmap");
		}
		ret = doca_mmap_destroy(m_local_io_mmap);
		if (ret == DOCA_SUCCESS) {
			m_local_io_mmap = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy local mmap");
		}
	}

	if (m_local_io_region != nullptr) {
		storage::aligned_free(m_local_io_region);
		m_local_io_region = nullptr;
	}

	if (m_dev != nullptr) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca_dev(%p): %s", m_dev, doca_error_get_name(ret));
		}
	}
}

storage::control::message target_rdma_app::wait_for_control_message()
{
	for (;;) {
		if (!m_ctrl_messages.empty()) {
			auto msg = std::move(m_ctrl_messages.front());
			m_ctrl_messages.erase(m_ctrl_messages.begin());
			return msg;
		}

		// Poll for new messages
		auto *new_msg = m_control_channel->poll();
		if (new_msg) {
			m_ctrl_messages.push_back(std::move(*new_msg));
		}

		if (m_abort_flag) {
			throw storage::runtime_error{
				DOCA_ERROR_CONNECTION_RESET,
				"User aborted the target_rdma_app while waiting on a control message"};
		}
	}
}

storage::control::message target_rdma_app::process_query_storage(storage::control::message const &client_request)
{
	return {
		storage::control::message_type::query_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::storage_details_payload>(uint64_t{m_storage_block_size} *
										    m_storage_block_count,
									    m_storage_block_size),
	};
}

storage::control::message target_rdma_app::process_init_storage(storage::control::message const &client_request)
{
	auto const *details =
		reinterpret_cast<storage::control::init_storage_payload const *>(client_request.payload.get());

	if (details->core_count > m_cfg.core_set.size()) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Requested storage to use " + std::to_string(details->core_count) +
						     " cores but only " + std::to_string(m_cfg.core_set.size()) +
						     " are configured"};
	}

	m_core_count = details->core_count;
	m_transaction_count = details->transaction_count;

	m_local_io_mmap = storage::make_mmap(m_dev,
					     reinterpret_cast<char *>(m_local_io_region),
					     m_local_io_region_size,
					     rdma_permissions,
					     storage::thread_safety::yes);
	m_remote_io_mmap = storage::make_mmap(m_dev,
					      details->mmap_export_blob.data(),
					      details->mmap_export_blob.size(),
					      storage::thread_safety::yes);

	prepare_workers();

	return {
		storage::control::message_type::init_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

storage::control::message target_rdma_app::process_create_rdma_connection(
	storage::control::message const &client_request)
{
	auto const *details = reinterpret_cast<storage::control::rdma_connection_details_payload const *>(
		client_request.payload.get());
	if (details->context_idx > m_core_count) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Unable to create RDMA connection for invalid context idx"};
	}

	worker_create_rdma_connection_command cmd{details->role, details->connection_details};
	auto const ret = m_workers[details->context_idx].execute_control_command(cmd);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create RDMA connection"};
	}

	return {
		storage::control::message_type::create_rdma_connection_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::rdma_connection_details_payload>(details->context_idx,
										    details->role,
										    std::move(cmd.out_exported_blob)),
	};
}

storage::control::message target_rdma_app::process_start_storage(storage::control::message const &client_request)
{
	verify_connections_are_ready();
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		worker_control_command cmd{worker_control_command::type::prepare_tasks};
		auto const ret = m_workers[ii].execute_control_command(cmd);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to create and submit tasks"};
		}
	}
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		worker_control_command cmd{worker_control_command::type::start_data_path};
		auto const ret = m_workers[ii].execute_control_command(cmd);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to start data path"};
		}
	}

	return {
		storage::control::message_type::start_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

storage::control::message target_rdma_app::process_stop_storage(storage::control::message const &client_request)
{
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		m_workers[ii].join_thread_proc();
	}

	return {
		storage::control::message_type::stop_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

storage::control::message target_rdma_app::process_shutdown(storage::control::message const &client_request)
{
	m_stats.reserve(m_core_count);
	std::transform(m_workers, m_workers + m_core_count, std::back_inserter(m_stats), [](auto const &worker) {
		return worker.get_stats();
	});

	destroy_workers();

	return {
		storage::control::message_type::shutdown_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

void target_rdma_app::prepare_workers()
{
	if (m_core_count > m_cfg.core_set.size()) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Unable to create " + std::to_string(m_core_count) + " threads as only " +
						     std::to_string(m_cfg.core_set.size()) + " cores were defined"};
	}

	m_workers = storage::make_aligned<target_rdma_worker>{}.object_array(m_core_count);
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		m_workers[ii].create_thread_proc(m_cfg.core_set[ii]);

		worker_create_objects_control_command cmd{m_dev, m_transaction_count, m_remote_io_mmap, m_local_io_mmap};
		auto const ret = m_workers[ii].execute_control_command(cmd);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to create worker objects"};
		}
	}
}

void target_rdma_app::destroy_workers(void) noexcept
{
	if (m_workers != nullptr) {
		// Destroy all thread resources
		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			m_workers[ii].~target_rdma_worker();
		}
		storage::aligned_free(m_workers);
		m_workers = nullptr;
	}
}

void target_rdma_app::verify_connections_are_ready(void)
{
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		bool ready = false;
		do {
			if (m_abort_flag) {
				throw storage::runtime_error{DOCA_ERROR_INITIALIZATION,
							     "Aborted while establishing storage connections"};
			}

			worker_are_contexts_ready_control_command cmd{};
			auto const ret = m_workers[ii].execute_control_command(cmd);
			if (ret != DOCA_SUCCESS) {
				throw storage::runtime_error{ret, "Failed to query connections state"};
			}

			if (cmd.out_status == DOCA_SUCCESS)
				ready = true;
			else if (cmd.out_status != DOCA_ERROR_IN_PROGRESS) {
				throw storage::runtime_error{cmd.out_status,
							     "Failure while establishing RDMA connections"};
			}
		} while (!ready);
	}
}

} /* namespace */
