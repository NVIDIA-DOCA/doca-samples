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
#include <storage_common/control_worker_async.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/file_utils.hpp>
#include <storage_common/io_message.hpp>
#include <storage_common/os_utils.hpp>
#include <storage_common/doca_utils.hpp>

DOCA_LOG_REGISTER(ZERO_COPY);

using namespace std::string_literals;

namespace {

auto constexpr app_name = "doca_storage_comch_to_rdma_zero_copy";

auto constexpr default_control_timeout_seconds = std::chrono::seconds{5};
auto constexpr default_command_channel_name = "doca_storage_comch";

static_assert(sizeof(void *) == 8, "Expected a pointer to occupy 8 bytes");

struct zero_copy_app_configuration {
	std::vector<uint32_t> cpu_set = {};
	std::string device_id = {};
	std::string representor_id = {};
	std::string command_channel_name = {};
	std::chrono::seconds control_timeout = {};
	storage::ip_address storage_server_address = {};
};

struct thread_stats {
	uint16_t core_idx = 0;
	uint64_t pe_hit_count = 0;
	uint64_t pe_miss_count = 0;
	uint64_t operation_count = 0;
};

struct worker_control_command {
	enum class type {
		create_objects,
		export_local_rdma_connection,
		import_remote_rdma_connection,
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
	case worker_control_command::type::export_local_rdma_connection:
		return "export_local_rdma_connection";
	case worker_control_command::type::import_remote_rdma_connection:
		return "import_remote_rdma_connection";
	case worker_control_command::type::are_contexts_ready:
		return "are_contexts_ready";
	case worker_control_command::type::prepare_tasks:
		return "prepare_tasks";
	case worker_control_command::type::start_data_path:
		return "start_data_path";
	case worker_control_command::type::abort_thread:
		return "abort_thread";
	default:
		return "UNKNOWN";
	}
}

struct worker_create_objects_control_command : public worker_control_command {
	/* Device to use */
	doca_dev *dev;
	/* Comch control channel to use */
	doca_comch_connection *comch_conn;
	/* Number of transactions to create */
	uint32_t transaction_count;

	~worker_create_objects_control_command() override = default;
	worker_create_objects_control_command() = delete;
	worker_create_objects_control_command(doca_dev *dev_,
					      doca_comch_connection *comch_conn_,
					      uint32_t transaction_count_)
		: worker_control_command{worker_control_command::type::create_objects},
		  dev{dev_},
		  comch_conn{comch_conn_},
		  transaction_count{transaction_count_}
	{
	}
	worker_create_objects_control_command(worker_create_objects_control_command const &) = default;
	worker_create_objects_control_command(worker_create_objects_control_command &&) noexcept = default;
	worker_create_objects_control_command &operator=(worker_create_objects_control_command const &) = default;
	worker_create_objects_control_command &operator=(worker_create_objects_control_command &&) noexcept = default;
};

struct worker_export_local_rdma_connection_command : public worker_control_command {
	/* The purpose of this connection */
	storage::control::rdma_connection_role rdma_role;
	/* Blob exported from the local side of the RDMA connection to be imported by the remote side. (Value set during
	 * execution of the command, caller should only access it after the command completes successfully) */
	std::vector<uint8_t> out_exported_blob;

	~worker_export_local_rdma_connection_command() override = default;
	worker_export_local_rdma_connection_command() = delete;
	explicit worker_export_local_rdma_connection_command(storage::control::rdma_connection_role rdma_role_)
		: worker_control_command{worker_control_command::type::export_local_rdma_connection},
		  rdma_role{rdma_role_},
		  out_exported_blob{}
	{
	}
	worker_export_local_rdma_connection_command(worker_export_local_rdma_connection_command const &) = default;
	worker_export_local_rdma_connection_command(worker_export_local_rdma_connection_command &&) noexcept = default;
	worker_export_local_rdma_connection_command &operator=(worker_export_local_rdma_connection_command const &) =
		default;
	worker_export_local_rdma_connection_command &operator=(
		worker_export_local_rdma_connection_command &&) noexcept = default;
};

struct worker_import_local_rdma_connection_command : public worker_control_command {
	/* The purpose of this connection */
	storage::control::rdma_connection_role rdma_role;
	/* Blob from the remote side of the RDMA connection to import */
	std::vector<uint8_t> import_blob;

	~worker_import_local_rdma_connection_command() override = default;
	worker_import_local_rdma_connection_command() = delete;
	worker_import_local_rdma_connection_command(storage::control::rdma_connection_role rdma_role_,
						    std::vector<uint8_t> import_blob_)
		: worker_control_command{worker_control_command::type::import_remote_rdma_connection},
		  rdma_role{rdma_role_},
		  import_blob{import_blob_}
	{
	}
	worker_import_local_rdma_connection_command(worker_import_local_rdma_connection_command const &) = default;
	worker_import_local_rdma_connection_command(worker_import_local_rdma_connection_command &&) noexcept = default;
	worker_import_local_rdma_connection_command &operator=(worker_import_local_rdma_connection_command const &) =
		default;
	worker_import_local_rdma_connection_command &operator=(
		worker_import_local_rdma_connection_command &&) noexcept = default;
};

struct worker_are_contexts_ready_control_command : public worker_control_command {
	/*
	 * Contexts status:
	 *  - DOCA_SUCCESS : When all contexts are ready to perform data path operations
	 *  - DOCA_ERROR_IN_PROGRESS : When one or more context is not ready yet
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

struct worker_prepare_tasks_control_command : public worker_control_command {
	/* Number of transactions to prepare. MUST equal worker_create_objects_control_command::transaction_count */
	uint32_t transaction_count;
	/* ID of the consumer on the initiator side this worker will send messages to */
	uint32_t remote_consumer_id;

	~worker_prepare_tasks_control_command() override = default;
	worker_prepare_tasks_control_command() = delete;
	worker_prepare_tasks_control_command(uint32_t transaction_count_, uint32_t remote_consumer_id_)
		: worker_control_command{worker_control_command::type::prepare_tasks},
		  transaction_count{transaction_count_},
		  remote_consumer_id{remote_consumer_id_}
	{
	}
	worker_prepare_tasks_control_command(worker_prepare_tasks_control_command const &) = default;
	worker_prepare_tasks_control_command(worker_prepare_tasks_control_command &&) noexcept = default;
	worker_prepare_tasks_control_command &operator=(worker_prepare_tasks_control_command const &) = default;
	worker_prepare_tasks_control_command &operator=(worker_prepare_tasks_control_command &&) noexcept = default;
};

class zero_copy_app_worker {
public:
	struct alignas(storage::cache_line_size) hot_data {
		doca_pe *pe = nullptr;
		uint64_t pe_hit_count = 0;
		uint64_t pe_miss_count = 0;
		uint64_t completed_transaction_count = 0;
		uint32_t in_flight_transaction_count = 0;
		uint16_t core_idx = 0;
		std::atomic_bool run_flag = false;
		bool error_flag = false;
	};
	static_assert(sizeof(zero_copy_app_worker::hot_data) == storage::cache_line_size,
		      "Expected thread_context::hot_data to occupy one cache line");

	~zero_copy_app_worker();

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
	void join_thread_proc(void);

	/*
	 * Get The workers stats
	 *
	 * @return stats;
	 */
	thread_stats get_stats() const noexcept;

	/*
	 * Destroy comch consumer and producer
	 */
	void destroy_comch_objects(void) noexcept;

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
	doca_buf_inventory *m_io_message_inv = nullptr;
	std::vector<doca_buf *> m_io_message_bufs = {};
	doca_pe *m_pe = nullptr;
	doca_comch_consumer *m_consumer = nullptr;
	doca_comch_producer *m_producer = nullptr;
	storage::rdma_conn_pair m_rdma_ctrl_ctx = {};
	storage::rdma_conn_pair m_rdma_data_ctx = {};
	std::vector<doca_comch_consumer_task_post_recv *> m_host_request_tasks = {};
	std::vector<doca_comch_producer_task_send *> m_host_response_tasks = {};
	std::vector<doca_rdma_task_send *> m_storage_request_tasks = {};
	std::vector<doca_rdma_task_receive *> m_storage_response_tasks = {};
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
	void export_local_rdma_connection_blob(worker_export_local_rdma_connection_command &cmd);

	/*
	 * Import a remote blob to a given rdma context and begin connecting
	 *
	 * @cmd [in/out]: Command containing the required input data to perfm the request, and storage for the output
	 * data
	 */
	void import_remote_rdma_connection_blob(worker_import_local_rdma_connection_command const &cmd);

	/*
	 * Check that all contexts are ready to run
	 *
	 * @cmd [in]: Command object holding the out param to populate
	 */
	void are_contexts_ready(worker_are_contexts_ready_control_command &cmd) const noexcept;

	/*
	 * Create and prepare task objects
	 *
	 * @cmd [in]: Command with data required to prepare tasks
	 */
	void prepare_tasks(worker_prepare_tasks_control_command const &cmd);

	/*
	 * Start data path operations.
	 */
	void start_data_path();

	static void doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
							  doca_data task_user_data,
							  doca_data ctx_user_data) noexcept;
	static void doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
								doca_data task_user_data,
								doca_data ctx_user_data) noexcept;
	static void doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
						     doca_data task_user_data,
						     doca_data ctx_user_data) noexcept;
	static void doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
							   doca_data task_user_data,
							   doca_data ctx_user_data) noexcept;
	static void doca_rdma_task_send_cb(doca_rdma_task_send *task,
					   doca_data task_user_data,
					   doca_data ctx_user_data) noexcept;
	static void doca_rdma_task_send_error_cb(doca_rdma_task_send *task,
						 doca_data task_user_data,
						 doca_data ctx_user_data) noexcept;
	static void doca_rdma_task_receive_cb(doca_rdma_task_receive *task,
					      doca_data task_user_data,
					      doca_data ctx_user_data) noexcept;
	static void doca_rdma_task_receive_error_cb(doca_rdma_task_receive *task,
						    doca_data task_user_data,
						    doca_data ctx_user_data) noexcept;

	/*
	 * Thread entry point
	 *
	 * @self [in]: Pointer to self
	 * @core_idx [in]: Core this thread will on
	 */
	static void thread_proc(zero_copy_app_worker *self, uint16_t core_idx) noexcept;

	/*
	 * Data path routine
	 *
	 * @hot_data [in]: Reference to hot_data
	 */
	static void run_data_path_ops(zero_copy_app_worker::hot_data &hot_data);
};

class zero_copy_app {
public:
	~zero_copy_app();
	zero_copy_app() = delete;
	explicit zero_copy_app(zero_copy_app_configuration const &cfg);
	zero_copy_app(zero_copy_app const &) = delete;
	zero_copy_app(zero_copy_app &&) noexcept = delete;
	zero_copy_app &operator=(zero_copy_app const &) = delete;
	zero_copy_app &operator=(zero_copy_app &&) noexcept = delete;

	void abort(std::string const &reason);

	void connect_to_storage(void);
	void wait_for_comch_client_connection(void);
	void wait_for_and_process_query_storage(void);
	void wait_for_and_process_init_storage(void);
	void wait_for_and_process_start_storage(void);
	void wait_for_and_process_stop_storage(void);
	void wait_for_and_process_shutdown(void);
	void display_stats(void) const;

private:
	zero_copy_app_configuration const m_cfg;
	doca_dev *m_dev;
	doca_dev_rep *m_dev_rep;
	doca_mmap *m_remote_io_mmap;
	std::unique_ptr<storage::control::comch_channel> m_client_control_channel;
	std::unique_ptr<storage::control::channel> m_storage_control_channel;
	std::vector<storage::control::channel *> m_ctrl_channels;
	std::vector<storage::control::message> m_ctrl_messages;
	std::vector<uint32_t> m_remote_consumer_ids;
	zero_copy_app_worker *m_workers;
	std::vector<thread_stats> m_stats;
	uint64_t m_storage_capacity;
	uint32_t m_storage_block_size;
	uint32_t m_message_id_counter;
	uint32_t m_transaction_count;
	uint32_t m_core_count;
	bool m_abort_flag;

	static void new_comch_consumer_callback(void *user_data, uint32_t id) noexcept;
	static void expired_comch_consumer_callback(void *user_data, uint32_t id) noexcept;

	storage::control::message wait_for_control_message();
	storage::control::message wait_for_control_message(storage::control::message_id mid,
							   std::chrono::seconds timeout);

	storage::control::message process_query_storage(storage::control::message const &client_request);
	storage::control::message process_init_storage(storage::control::message const &client_request);
	storage::control::message process_start_storage(storage::control::message const &client_request);
	storage::control::message process_stop_storage(storage::control::message const &client_request);
	storage::control::message process_shutdown(storage::control::message const &client_requeste);

	void prepare_thread_contexts(storage::control::correlation_id cid);

	void connect_rdma(uint32_t thread_idx,
			  storage::control::rdma_connection_role role,
			  storage::control::correlation_id cid);

	void verify_connections_are_ready(void);

	void destroy_workers(void) noexcept;
};

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed zero_copy_app_configuration
 *
 * @throws: storage::runtime_error If the zero_copy_app_configuration cannot pe parsed or contains invalid values
 */
zero_copy_app_configuration parse_cli_args(int argc, char **argv);

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
		zero_copy_app app{parse_cli_args(argc, argv)};
		storage::install_ctrl_c_handler([&app]() {
			app.abort("User requested abort");
		});

		app.connect_to_storage();
		app.wait_for_comch_client_connection();
		app.wait_for_and_process_query_storage();
		app.wait_for_and_process_init_storage();
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
 * Print the parsed zero_copy_app_configuration
 *
 * @cfg [in]: zero_copy_app_configuration to display
 */
void print_config(zero_copy_app_configuration const &cfg) noexcept
{
	printf("zero_copy_app_configuration: {\n");
	printf("\tcpu_set : [");
	bool first = true;
	for (auto cpu : cfg.cpu_set) {
		if (first)
			first = false;
		else
			printf(", ");
		printf("%u", cpu);
	}
	printf("]\n");
	printf("\tdevice : \"%s\",\n", cfg.device_id.c_str());
	printf("\trepresentor : \"%s\",\n", cfg.representor_id.c_str());
	printf("\tcommand_channel_name : \"%s\",\n", cfg.command_channel_name.c_str());
	printf("\tcontrol_timeout : %u,\n", static_cast<uint32_t>(cfg.control_timeout.count()));
	printf("\tstorage_server : %s:%u\n",
	       cfg.storage_server_address.get_address().c_str(),
	       cfg.storage_server_address.get_port());
	printf("}\n");
}

/*
 * Validate zero_copy_app_configuration
 *
 * @cfg [in]: zero_copy_app_configuration
 */
void validate_zero_copy_app_configuration(zero_copy_app_configuration const &cfg)
{
	std::vector<std::string> errors;

	if (cfg.control_timeout.count() == 0) {
		errors.emplace_back("Invalid zero_copy_app_configuration: control-timeout must not be zero");
	}

	if (!errors.empty()) {
		for (auto const &err : errors) {
			printf("%s\n", err.c_str());
		}
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE, "Invalid zero_copy_app_configuration detected"};
	}
}

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed zero_copy_app_configuration
 *
 * @throws: storage::runtime_error If the zero_copy_app_configuration cannot pe parsed or contains invalid values
 */
zero_copy_app_configuration parse_cli_args(int argc, char **argv)
{
	zero_copy_app_configuration config{};
	config.command_channel_name = default_command_channel_name;
	config.control_timeout = default_control_timeout_seconds;

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
					       static_cast<zero_copy_app_configuration *>(cfg)->device_id =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       "r",
				       "representor",
				       "Device host side representor identifier",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<zero_copy_app_configuration *>(cfg)->representor_id =
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
					       static_cast<zero_copy_app_configuration *>(cfg)->cpu_set.push_back(
						       *static_cast<int *>(value));
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"storage-server",
		"Storage server addresses in <ip_addr>:<port> format",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			try {
				static_cast<zero_copy_app_configuration *>(cfg)->storage_server_address =
					storage::parse_ip_v4_address(static_cast<char const *>(value));
				return DOCA_SUCCESS;
			} catch (std::runtime_error const &ex) {
				return DOCA_ERROR_INVALID_VALUE;
			}
		});
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"command-channel-name",
		"Name of the channel used by the doca_comch_client. Default: \"doca_storage_comch\"",
		storage::optional_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<zero_copy_app_configuration *>(cfg)->command_channel_name =
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
					       static_cast<zero_copy_app_configuration *>(cfg)->control_timeout =
						       std::chrono::seconds{*static_cast<int *>(value)};
					       return DOCA_SUCCESS;
				       });
	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args"};
	}

	static_cast<void>(doca_argp_destroy());

	print_config(config);
	validate_zero_copy_app_configuration(config);

	return config;
}

zero_copy_app_worker::~zero_copy_app_worker()
{
	if (m_thread.joinable()) {
		DOCA_LOG_WARN("Worker Data path thread was still running during destruction");
		if (m_hot_data != nullptr)
			m_hot_data->error_flag = true;

		join_thread_proc();
	}

	doca_error_t ret;
	std::vector<doca_task *> tasks;

	if (m_rdma_ctrl_ctx.rdma != nullptr) {
		tasks.clear();
		tasks.reserve(m_storage_request_tasks.size() + m_storage_response_tasks.size());
		std::transform(std::begin(m_storage_request_tasks),
			       std::end(m_storage_request_tasks),
			       std::back_inserter(tasks),
			       doca_rdma_task_send_as_task);
		std::transform(std::begin(m_storage_response_tasks),
			       std::end(m_storage_response_tasks),
			       std::back_inserter(tasks),
			       doca_rdma_task_receive_as_task);

		/* stop context with tasks list (tasks must be destroyed to finish stopping process) */
		ret = storage::stop_context(doca_rdma_as_ctx(m_rdma_ctrl_ctx.rdma), m_pe, tasks);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop rdma control context: %s", doca_error_get_name(ret));
		}

		ret = doca_rdma_destroy(m_rdma_ctrl_ctx.rdma);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy rdma control context: %s", doca_error_get_name(ret));
		}
	}

	if (m_rdma_data_ctx.rdma != nullptr) {
		// No tasks allocated on this side for the data context, all tasks are executed from the storage side
		ret = doca_ctx_stop(doca_rdma_as_ctx(m_rdma_data_ctx.rdma));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop rdma data context: %s", doca_error_get_name(ret));
		}

		ret = doca_rdma_destroy(m_rdma_data_ctx.rdma);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy rdma data context: %s", doca_error_get_name(ret));
		}
	}

	destroy_comch_objects();

	if (m_pe != nullptr) {
		ret = doca_pe_destroy(m_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine");
		}
	}

	for (auto *buf : m_io_message_bufs) {
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
	}

	if (m_io_message_inv) {
		ret = doca_buf_inventory_stop(m_io_message_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop buffer inventory");
		}
		ret = doca_buf_inventory_destroy(m_io_message_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy buffer inventory");
		}
	}

	if (m_io_message_mmap) {
		ret = doca_mmap_stop(m_io_message_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop mmap");
		}
		ret = doca_mmap_destroy(m_io_message_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap");
		}
	}

	if (m_io_message_region != nullptr) {
		storage::aligned_free(m_io_message_region);
	}

	storage::aligned_free(m_hot_data);
}

void zero_copy_app_worker::create_thread_proc(uint16_t core_idx)
{
	m_thread = std::thread{thread_proc, this, core_idx};
}

doca_error_t zero_copy_app_worker::execute_control_command(worker_control_command &cmd)
{
	return storage::control::execute_worker_command(m_async_ctrl, &cmd, std::chrono::seconds{3});
}

void zero_copy_app_worker::join_thread_proc(void)
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

thread_stats zero_copy_app_worker::get_stats() const noexcept
{
	thread_stats stats{};

	if (m_hot_data != nullptr) {
		stats.core_idx = m_hot_data->core_idx;
		stats.pe_hit_count = m_hot_data->pe_hit_count;
		stats.pe_miss_count = m_hot_data->pe_miss_count;
		stats.operation_count = m_hot_data->completed_transaction_count;
	}

	return stats;
}

void zero_copy_app_worker::destroy_comch_objects(void) noexcept
{
	doca_error_t ret;
	std::vector<doca_task *> tasks;

	if (m_consumer != nullptr) {
		tasks.reserve(m_host_request_tasks.size());
		std::transform(std::begin(m_host_request_tasks),
			       std::end(m_host_request_tasks),
			       std::back_inserter(tasks),
			       doca_comch_consumer_task_post_recv_as_task);
		ret = storage::stop_context(doca_comch_consumer_as_ctx(m_consumer), m_pe, tasks);
		tasks.clear();
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop consumer context");
		} else {
			m_host_request_tasks.clear();
		}
		ret = doca_comch_consumer_destroy(m_consumer);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy consumer context");
		} else {
			m_consumer = nullptr;
		}
	}

	if (m_producer != nullptr) {
		tasks.reserve(m_host_response_tasks.size());
		std::transform(std::begin(m_host_response_tasks),
			       std::end(m_host_response_tasks),
			       std::back_inserter(tasks),
			       doca_comch_producer_task_send_as_task);
		ret = storage::stop_context(doca_comch_producer_as_ctx(m_producer), m_pe, tasks);
		tasks.clear();
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop producer context");
		} else {
			m_host_response_tasks.clear();
		}
		ret = doca_comch_producer_destroy(m_producer);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy producer context");
		} else {
			m_producer = nullptr;
		}
	}
}

bool zero_copy_app_worker::execute_control_command_impl(worker_control_command &cmd) noexcept
{
	doca_error_t cmd_result;
	bool control_path_completed = false;

	try {
		cmd_result = DOCA_SUCCESS;
		switch (cmd.cmd_type) {
		case worker_control_command::type::create_objects:
			create_worker_objects(dynamic_cast<worker_create_objects_control_command &>(cmd));
			break;
		case worker_control_command::type::export_local_rdma_connection:
			export_local_rdma_connection_blob(
				dynamic_cast<worker_export_local_rdma_connection_command &>(cmd));
			break;
		case worker_control_command::type::import_remote_rdma_connection:
			import_remote_rdma_connection_blob(
				dynamic_cast<worker_import_local_rdma_connection_command &>(cmd));
			break;
		case worker_control_command::type::are_contexts_ready:
			are_contexts_ready(dynamic_cast<worker_are_contexts_ready_control_command &>(cmd));
			break;
		case worker_control_command::type::prepare_tasks:
			prepare_tasks(dynamic_cast<worker_prepare_tasks_control_command &>(cmd));
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

void zero_copy_app_worker::create_worker_objects(worker_create_objects_control_command const &cmd)
{
	doca_error_t ret;
	auto const page_size = storage::get_system_page_size();

	auto const raw_io_messages_size = cmd.transaction_count * storage::size_of_io_message * 2;

	DOCA_LOG_DBG("Allocate comch buffers memory (%zu bytes, aligned to %u byte pages)",
		     raw_io_messages_size,
		     page_size);
	m_io_message_region = static_cast<uint8_t *>(
		storage::aligned_alloc(page_size, storage::aligned_size(page_size, raw_io_messages_size)));
	if (m_io_message_region == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate comch fast path buffers memory"};
	}

	m_io_message_mmap = storage::make_mmap(cmd.dev,
					       reinterpret_cast<char *>(m_io_message_region),
					       raw_io_messages_size,
					       DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE,
					       storage::thread_safety::no);

	ret = doca_buf_inventory_create(cmd.transaction_count * 2, &m_io_message_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create comch fast path doca_buf_inventory"};
	}

	ret = doca_buf_inventory_start(m_io_message_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start comch fast path doca_buf_inventory"};
	}

	DOCA_LOG_DBG("Create hot path progress engine");
	ret = doca_pe_create(std::addressof(m_pe));
	m_hot_data->pe = m_pe;
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_pe"};
	}

	m_consumer = storage::make_comch_consumer(cmd.comch_conn,
						  m_io_message_mmap,
						  m_pe,
						  cmd.transaction_count,
						  doca_data{.ptr = m_hot_data},
						  doca_comch_consumer_task_post_recv_cb,
						  doca_comch_consumer_task_post_recv_error_cb);

	m_producer = storage::make_comch_producer(cmd.comch_conn,
						  m_pe,
						  cmd.transaction_count,
						  doca_data{.ptr = m_hot_data},
						  doca_comch_producer_task_send_cb,
						  doca_comch_producer_task_send_error_cb);
	auto const rdma_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
				      DOCA_ACCESS_FLAG_RDMA_WRITE;

	m_rdma_ctrl_ctx.rdma =
		storage::make_rdma_context(cmd.dev, m_pe, doca_data{.ptr = m_hot_data}, rdma_permissions);

	ret = doca_rdma_task_receive_set_conf(m_rdma_ctrl_ctx.rdma,
					      doca_rdma_task_receive_cb,
					      doca_rdma_task_receive_error_cb,
					      cmd.transaction_count);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to configure rdma receive task pool"};
	}

	ret = doca_rdma_task_send_set_conf(m_rdma_ctrl_ctx.rdma,
					   doca_rdma_task_send_cb,
					   doca_rdma_task_send_error_cb,
					   cmd.transaction_count);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to configure rdma send task pool"};
	}

	ret = doca_ctx_start(doca_rdma_as_ctx(m_rdma_ctrl_ctx.rdma));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_rdma context"};
	}

	m_rdma_data_ctx.rdma =
		storage::make_rdma_context(cmd.dev, m_pe, doca_data{.ptr = m_hot_data}, rdma_permissions);

	ret = doca_ctx_start(doca_rdma_as_ctx(m_rdma_data_ctx.rdma));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_rdma context"};
	}
}

void zero_copy_app_worker::export_local_rdma_connection_blob(worker_export_local_rdma_connection_command &cmd)
{
	doca_error_t ret;
	uint8_t const *blob = nullptr;
	size_t blob_size = 0;

	auto &rdma_pair = cmd.rdma_role == storage::control::rdma_connection_role::io_data ? m_rdma_data_ctx :
											     m_rdma_ctrl_ctx;
	ret = doca_rdma_export(rdma_pair.rdma,
			       reinterpret_cast<void const **>(&blob),
			       &blob_size,
			       std::addressof(rdma_pair.conn));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Core: %u RDMA export failed: %s", m_hot_data->core_idx, doca_error_get_name(ret));
		throw storage::runtime_error{ret, "Failed to export rdma connection"};
	}

	cmd.out_exported_blob = std::vector<uint8_t>{blob, blob + blob_size};
}

void zero_copy_app_worker::import_remote_rdma_connection_blob(worker_import_local_rdma_connection_command const &cmd)
{
	auto &rdma_pair = cmd.rdma_role == storage::control::rdma_connection_role::io_data ? m_rdma_data_ctx :
											     m_rdma_ctrl_ctx;

	doca_error_t ret;
	ret = doca_rdma_connect(rdma_pair.rdma, cmd.import_blob.data(), cmd.import_blob.size(), rdma_pair.conn);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Core: %u RDMA connect failed: %s", m_hot_data->core_idx, doca_error_get_name(ret));
		throw storage::runtime_error{ret, "Failed to connect to rdma"};
	}
}

void zero_copy_app_worker::are_contexts_ready(worker_are_contexts_ready_control_command &cmd) const noexcept
{
	doca_ctx_states ctx_state;
	uint32_t pending_count = 0;

	cmd.out_status = doca_ctx_get_state(doca_comch_producer_as_ctx(m_producer), &ctx_state);
	if (cmd.out_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query comch producer state: %s", doca_error_get_name(cmd.out_status));
		return;
	}

	if (ctx_state != DOCA_CTX_STATE_RUNNING) {
		++pending_count;
		static_cast<void>(doca_pe_progress(m_pe));
	}

	cmd.out_status = doca_ctx_get_state(doca_comch_consumer_as_ctx(m_consumer), &ctx_state);
	if (cmd.out_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query comch consumer state: %s", doca_error_get_name(cmd.out_status));
		return;
	}

	if (ctx_state != DOCA_CTX_STATE_RUNNING) {
		++pending_count;
		static_cast<void>(doca_pe_progress(m_pe));
	}

	cmd.out_status = doca_ctx_get_state(doca_rdma_as_ctx(m_rdma_ctrl_ctx.rdma), &ctx_state);
	if (cmd.out_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(cmd.out_status));
		return;
	}

	if (ctx_state != DOCA_CTX_STATE_RUNNING) {
		++pending_count;
		static_cast<void>(doca_pe_progress(m_pe));
	}

	cmd.out_status = doca_ctx_get_state(doca_rdma_as_ctx(m_rdma_data_ctx.rdma), &ctx_state);
	if (cmd.out_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(cmd.out_status));
		return;
	}

	if (ctx_state != DOCA_CTX_STATE_RUNNING) {
		++pending_count;
		static_cast<void>(doca_pe_progress(m_pe));
	}

	cmd.out_status = (pending_count == 0) ? DOCA_SUCCESS : DOCA_ERROR_IN_PROGRESS;
}

void zero_copy_app_worker::prepare_tasks(worker_prepare_tasks_control_command const &cmd)
{
	doca_error_t ret;

	auto *buf_addr = m_io_message_region;

	m_io_message_bufs.reserve(cmd.transaction_count * 2);
	m_host_request_tasks.reserve(cmd.transaction_count);
	m_host_response_tasks.reserve(cmd.transaction_count);
	m_storage_request_tasks.reserve(cmd.transaction_count);
	m_storage_request_tasks.reserve(cmd.transaction_count);

	for (uint32_t ii = 0; ii != cmd.transaction_count; ++ii) {
		doca_buf *storage_request_buff = nullptr;

		ret = doca_buf_inventory_buf_get_by_addr(m_io_message_inv,
							 m_io_message_mmap,
							 buf_addr,
							 storage::size_of_io_message,
							 &storage_request_buff);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get io message doca_buf"};
		}

		buf_addr += storage::size_of_io_message;
		m_io_message_bufs.push_back(storage_request_buff);

		doca_rdma_task_send *rdma_task_send = nullptr;
		ret = doca_rdma_task_send_allocate_init(m_rdma_ctrl_ctx.rdma,
							m_rdma_ctrl_ctx.conn,
							storage_request_buff,
							doca_data{.ptr = nullptr},
							&rdma_task_send);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate rdma doca_rdma_task_send"};
		}
		m_storage_request_tasks.push_back(rdma_task_send);

		doca_comch_consumer_task_post_recv *comch_consumer_task_post_recv = nullptr;
		ret = doca_comch_consumer_task_post_recv_alloc_init(m_consumer,
								    storage_request_buff,
								    &comch_consumer_task_post_recv);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get doca_buf for producer task"};
		}
		m_host_request_tasks.push_back(comch_consumer_task_post_recv);

		/* link task pair - comch recv <-> rdma send */
		static_cast<void>(doca_task_set_user_data(
			doca_comch_consumer_task_post_recv_as_task(comch_consumer_task_post_recv),
			doca_data{.ptr = doca_rdma_task_send_as_task(rdma_task_send)}));
		static_cast<void>(doca_task_set_user_data(doca_rdma_task_send_as_task(rdma_task_send),
							  doca_data{.ptr = comch_consumer_task_post_recv}));

		doca_buf *storage_recv_buf = nullptr;

		ret = doca_buf_inventory_buf_get_by_addr(m_io_message_inv,
							 m_io_message_mmap,
							 buf_addr,
							 storage::size_of_io_message,
							 &storage_recv_buf);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get io message doca_buf"};
		}
		buf_addr += storage::size_of_io_message;
		m_io_message_bufs.push_back(storage_recv_buf);

		doca_rdma_task_receive *rdma_task_receive = nullptr;
		ret = doca_rdma_task_receive_allocate_init(m_rdma_ctrl_ctx.rdma,
							   storage_recv_buf,
							   doca_data{.ptr = nullptr},
							   &rdma_task_receive);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate rdma doca_rdma_task_receive"};
		}
		m_storage_response_tasks.push_back(rdma_task_receive);

		doca_comch_producer_task_send *comch_producer_task_send;
		ret = doca_comch_producer_task_send_alloc_init(m_producer,
							       storage_recv_buf,
							       nullptr,
							       0,
							       cmd.remote_consumer_id,
							       &comch_producer_task_send);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get doca_buf for producer task"};
		}
		m_host_response_tasks.push_back(comch_producer_task_send);

		/* link task pair - rdma recv <-> comch send */
		static_cast<void>(
			doca_task_set_user_data(doca_comch_producer_task_send_as_task(comch_producer_task_send),
						doca_data{.ptr = rdma_task_receive}));
		static_cast<void>(doca_task_set_user_data(
			doca_rdma_task_receive_as_task(rdma_task_receive),
			doca_data{.ptr = doca_comch_producer_task_send_as_task(comch_producer_task_send)}));
	}
}

void zero_copy_app_worker::start_data_path(void)
{
	m_hot_data->run_flag = true;
}

void zero_copy_app_worker::doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
								 doca_data task_user_data,
								 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	doca_error_t ret;

	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);

	/*
	 * Submit send of the data to the storage backend. Note: both tasks share the same doca buf so the data message
	 * is forwarded verbatim without any action on the users part.
	 */
	ret = doca_task_submit(static_cast<doca_task *>(task_user_data.ptr));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_rdma_task_send: %s", doca_error_get_name(ret));
		hot_data->error_flag = true;
		hot_data->run_flag = false;
	}
}

void zero_copy_app_worker::doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
								       doca_data task_user_data,
								       doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);

	if (hot_data->run_flag) {
		DOCA_LOG_ERR("Failed to complete doca_comch_consumer_task_post_recv");
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void zero_copy_app_worker::doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
							    doca_data task_user_data,
							    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);

	auto *storage_response_task = static_cast<doca_rdma_task_receive *>(task_user_data.ptr);

	static_cast<void>(doca_buf_reset_data_len(doca_rdma_task_receive_get_dst_buf(storage_response_task)));

	auto ret = doca_task_submit(doca_rdma_task_receive_as_task(storage_response_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_rdma_task_receive: %s", doca_error_get_name(ret));
		hot_data->error_flag = true;
		hot_data->run_flag = false;
	}

	++(hot_data->completed_transaction_count);
}

void zero_copy_app_worker::doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
								  doca_data task_user_data,
								  doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_comch_producer_task_send");
	hot_data->run_flag = false;
	hot_data->error_flag = true;
}

void zero_copy_app_worker::doca_rdma_task_send_cb(doca_rdma_task_send *task,
						  doca_data task_user_data,
						  doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);

	auto *host_request_task = static_cast<doca_comch_consumer_task_post_recv *>(task_user_data.ptr);

	static_cast<void>(doca_buf_reset_data_len(doca_comch_consumer_task_post_recv_get_buf(host_request_task)));

	auto ret = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(host_request_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_consumer_task_post_recv: %s", doca_error_get_name(ret));
		hot_data->error_flag = true;
		hot_data->run_flag = false;
	}
}

void zero_copy_app_worker::doca_rdma_task_send_error_cb(doca_rdma_task_send *task,
							doca_data task_user_data,
							doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_rdma_task_send");
	hot_data->run_flag = false;
	hot_data->error_flag = true;
}

void zero_copy_app_worker::doca_rdma_task_receive_cb(doca_rdma_task_receive *task,
						     doca_data task_user_data,
						     doca_data ctx_user_data) noexcept
{
	doca_error_t ret;
	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);

	auto *const io_message = storage::get_buffer_bytes(doca_rdma_task_receive_get_dst_buf(task));

	storage::io_message_view::set_type(storage::io_message_type::result, io_message);
	storage::io_message_view::set_result(DOCA_SUCCESS, io_message);

	do {
		ret = doca_task_submit(static_cast<doca_task *>(task_user_data.ptr));
	} while (ret == DOCA_ERROR_AGAIN);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_producer_task_send: %s", doca_error_get_name(ret));
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void zero_copy_app_worker::doca_rdma_task_receive_error_cb(doca_rdma_task_receive *task,
							   doca_data task_user_data,
							   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<zero_copy_app_worker::hot_data *>(ctx_user_data.ptr);
	if (hot_data->run_flag) {
		/*
		 * Only consider it a failure when this callback triggers while running. This callback will be triggered
		 * as part of teardown as the submitted receive tasks that were never filled by requests from the host
		 * get flushed out.
		 */
		DOCA_LOG_ERR("Failed to complete doca_rdma_task_send");
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void zero_copy_app_worker::thread_proc(zero_copy_app_worker *self, uint16_t core_idx) noexcept
{
	try {
		storage::set_thread_affinity(self->m_thread, core_idx);
		self->m_hot_data = storage::make_aligned<zero_copy_app_worker::hot_data>{}.object();
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
		doca_error_t ret;
		for (auto *task : self->m_host_request_tasks) {
			ret = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task));
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit initial doca_comch_consumer_task_post_recv task: %s",
					     doca_error_get_name(ret));
				throw storage::runtime_error{ret, "Failed to submit initial task"};
			}
		}

		for (auto *task : self->m_storage_response_tasks) {
			ret = doca_task_submit(doca_rdma_task_receive_as_task(task));
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit initial doca_rdma_task_receive task: %s",
					     doca_error_get_name(ret));
				throw storage::runtime_error{ret, "Failed to submit initial task"};
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

void zero_copy_app_worker::run_data_path_ops(zero_copy_app_worker::hot_data &hot_data)
{
	DOCA_LOG_INFO("Core: %u running", hot_data.core_idx);

	while (hot_data.run_flag) {
		doca_pe_progress(hot_data.pe) ? ++(hot_data.pe_hit_count) : ++(hot_data.pe_miss_count);
	}

	while (hot_data.error_flag == false && hot_data.in_flight_transaction_count != 0) {
		doca_pe_progress(hot_data.pe) ? ++(hot_data.pe_hit_count) : ++(hot_data.pe_miss_count);
	}

	DOCA_LOG_INFO("Core: %u complete", hot_data.core_idx);
}

zero_copy_app::~zero_copy_app()
{
	destroy_workers();
	m_storage_control_channel.reset();
	m_client_control_channel.reset();

	doca_error_t ret;

	if (m_remote_io_mmap) {
		ret = doca_mmap_stop(m_remote_io_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop remote mmap");
		}
		ret = doca_mmap_destroy(m_remote_io_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy remote mmap");
		}
	}

	if (m_dev_rep != nullptr) {
		ret = doca_dev_rep_close(m_dev_rep);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close dev rep");
		}
	}

	if (m_dev != nullptr) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close dev");
		}
	}
}

zero_copy_app::zero_copy_app(zero_copy_app_configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_dev_rep{nullptr},
	  m_remote_io_mmap{nullptr},
	  m_client_control_channel{},
	  m_storage_control_channel{},
	  m_ctrl_channels{},
	  m_ctrl_messages{},
	  m_remote_consumer_ids{},
	  m_workers{nullptr},
	  m_stats{},
	  m_storage_capacity{},
	  m_storage_block_size{},
	  m_message_id_counter{},
	  m_transaction_count{0},
	  m_core_count{0},
	  m_abort_flag{false}
{
	DOCA_LOG_INFO("Open doca_dev: %s", m_cfg.device_id.c_str());
	m_dev = storage::open_device(m_cfg.device_id);

	DOCA_LOG_INFO("Open doca_dev_rep: %s", m_cfg.representor_id.c_str());
	m_dev_rep = storage::open_representor(m_dev, m_cfg.representor_id);

	m_client_control_channel =
		storage::control::make_comch_server_control_channel(m_dev,
								    m_dev_rep,
								    m_cfg.command_channel_name.c_str(),
								    this,
								    new_comch_consumer_callback,
								    expired_comch_consumer_callback);

	m_storage_control_channel = storage::control::make_tcp_client_control_channel(m_cfg.storage_server_address);
	m_ctrl_channels.reserve(2);
	m_ctrl_channels.push_back(m_client_control_channel.get());
	m_ctrl_channels.push_back(m_storage_control_channel.get());
}

void zero_copy_app::abort(std::string const &reason)
{
	if (m_abort_flag)
		return;

	DOCA_LOG_ERR("Aborted: %s", reason.c_str());
	m_abort_flag = true;
}

void zero_copy_app::connect_to_storage(void)
{
	while (!m_storage_control_channel->is_connected()) {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		if (m_abort_flag) {
			throw storage::runtime_error{DOCA_ERROR_CONNECTION_ABORTED,
						     "Aborted while connecting to storage"};
		}
	}
}

void zero_copy_app::wait_for_comch_client_connection(void)
{
	while (!m_client_control_channel->is_connected()) {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		if (m_abort_flag) {
			throw storage::runtime_error{DOCA_ERROR_CONNECTION_ABORTED,
						     "Aborted while connecting to client"};
		}
	}
}

void zero_copy_app::wait_for_and_process_query_storage(void)
{
	DOCA_LOG_INFO("Wait for query storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::query_storage_request) {
		try {
			m_client_control_channel->send_message(process_query_storage(client_request));
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

	m_client_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void zero_copy_app::wait_for_and_process_init_storage(void)
{
	DOCA_LOG_INFO("Wait for init storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::init_storage_request) {
		try {
			m_client_control_channel->send_message(process_init_storage(client_request));
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

	m_client_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void zero_copy_app::wait_for_and_process_start_storage(void)
{
	DOCA_LOG_INFO("Wait for start storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::start_storage_request) {
		try {
			m_client_control_channel->send_message(process_start_storage(client_request));
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

	m_client_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void zero_copy_app::wait_for_and_process_stop_storage(void)
{
	DOCA_LOG_INFO("Wait for stop storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::stop_storage_request) {
		try {
			m_client_control_channel->send_message(process_stop_storage(client_request));
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

	m_client_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void zero_copy_app::wait_for_and_process_shutdown(void)
{
	DOCA_LOG_INFO("Wait for shutdown storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::shutdown_request) {
		try {
			m_client_control_channel->send_message(process_shutdown(client_request));
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

	m_client_control_channel->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void zero_copy_app::display_stats(void) const
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

void zero_copy_app::new_comch_consumer_callback(void *user_data, uint32_t id) noexcept
{
	auto *self = reinterpret_cast<zero_copy_app *>(user_data);
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

void zero_copy_app::expired_comch_consumer_callback(void *user_data, uint32_t id) noexcept
{
	auto *self = reinterpret_cast<zero_copy_app *>(user_data);
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

storage::control::message zero_copy_app::wait_for_control_message()
{
	for (;;) {
		if (!m_ctrl_messages.empty()) {
			auto msg = std::move(m_ctrl_messages.front());
			m_ctrl_messages.erase(m_ctrl_messages.begin());
			return msg;
		}

		for (auto *channel : m_ctrl_channels) {
			// Poll for new messages
			auto *msg = channel->poll();
			if (msg) {
				m_ctrl_messages.push_back(std::move(*msg));
			}
		}

		if (m_abort_flag) {
			throw storage::runtime_error{
				DOCA_ERROR_CONNECTION_RESET,
				"User aborted the zero_copy_application while waiting on a control message"};
		}
	}
}

storage::control::message zero_copy_app::wait_for_control_message(storage::control::message_id mid,
								  std::chrono::seconds timeout)
{
	auto const expiry = std::chrono::steady_clock::now() + timeout;
	do {
		if (m_abort_flag) {
			throw storage::runtime_error{
				DOCA_ERROR_CONNECTION_RESET,
				"User aborted the zero_copy_application while waiting on a control message"};
		}

		for (auto *channel : m_ctrl_channels) {
			// Poll for new messages
			auto *msg = channel->poll();
			if (msg) {
				m_ctrl_messages.push_back(std::move(*msg));
			}
		}

		auto found =
			std::find_if(std::begin(m_ctrl_messages), std::end(m_ctrl_messages), [mid](auto const &msg) {
				return msg.message_id.value == mid.value;
			});

		if (found != std::end(m_ctrl_messages)) {
			auto msg = std::move(*found);
			m_ctrl_messages.erase(found);
			return msg;
		}

	} while (expiry > std::chrono::steady_clock::now());

	throw storage::runtime_error{DOCA_ERROR_TIME_OUT, "Timed out while waiting on a control message"};
}

storage::control::message zero_copy_app::process_query_storage(storage::control::message const &client_request)
{
	DOCA_LOG_DBG("Forward request to storage...");
	auto storage_request = storage::control::message{
		storage::control::message_type::query_storage_request,
		storage::control::message_id{m_message_id_counter++},
		client_request.correlation_id,
		{},
	};
	m_storage_control_channel->send_message(storage_request);

	// Wait for storage response
	auto storage_response = wait_for_control_message(storage_request.message_id, default_control_timeout_seconds);

	if (storage_response.message_type == storage::control::message_type::query_storage_response) {
		auto const *const storage_details =
			dynamic_cast<storage::control::storage_details_payload const *>(storage_response.payload.get());
		if (storage_details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_UNEXPECTED,
						     "[BUG] Invalid query_storage_response received"};
		}

		m_storage_capacity = storage_details->total_size;
		m_storage_block_size = storage_details->block_size;
		DOCA_LOG_INFO("Storage reports capacity of: %lu using a block size of: %u",
			      m_storage_capacity,
			      m_storage_block_size);
		return storage::control::message{
			storage::control::message_type::query_storage_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else if (storage_response.message_type == storage::control::message_type::error_response) {
		return storage::control::message{
			storage::control::message_type::error_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else {
		throw storage::runtime_error{
			DOCA_ERROR_UNEXPECTED,
			"Unexpected " + to_string(storage_response.message_type) + " while expecting a " +
				to_string(storage::control::message_type::query_storage_response),
		};
	}
}

storage::control::message zero_copy_app::process_init_storage(storage::control::message const &client_request)
{
	auto const *init_storage_details =
		reinterpret_cast<storage::control::init_storage_payload const *>(client_request.payload.get());

	if (init_storage_details->core_count > m_cfg.cpu_set.size()) {
		throw storage::runtime_error{
			DOCA_ERROR_INVALID_VALUE,
			"Unable to create " + std::to_string(m_core_count) + " threads as only " +
				std::to_string(m_cfg.cpu_set.size()) + " were defined",
		};
	}

	m_remote_consumer_ids.reserve(init_storage_details->core_count);
	m_transaction_count = init_storage_details->transaction_count * 2;
	m_core_count = init_storage_details->core_count;
	m_remote_io_mmap = storage::make_mmap(m_dev,
					      init_storage_details->mmap_export_blob.data(),
					      init_storage_details->mmap_export_blob.size(),
					      storage::thread_safety::no);
	std::vector<uint8_t> mmap_export_blob = [this]() {
		uint8_t const *reexport_blob = nullptr;
		size_t reexport_blob_size = 0;
		auto const ret = doca_mmap_export_rdma(m_remote_io_mmap,
						       m_dev,
						       reinterpret_cast<void const **>(&reexport_blob),
						       &reexport_blob_size);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to re-export host mmap for rdma"};
		}

		return std::vector<uint8_t>{reexport_blob, reexport_blob + reexport_blob_size};
	}();

	DOCA_LOG_INFO("Configured storage: %u cores, %u transactions", m_core_count, m_transaction_count);

	DOCA_LOG_DBG("Forward request to storage...");
	auto storage_request = storage::control::message{
		storage::control::message_type::init_storage_request,
		storage::control::message_id{m_message_id_counter++},
		client_request.correlation_id,
		std::make_unique<storage::control::init_storage_payload>(m_transaction_count,
									 init_storage_details->core_count,
									 std::move(mmap_export_blob)),
	};
	m_storage_control_channel->send_message(storage_request);

	// Wait for storage response
	auto storage_response = wait_for_control_message(storage_request.message_id, default_control_timeout_seconds);

	if (storage_response.message_type == storage::control::message_type::init_storage_response) {
		DOCA_LOG_DBG("prepare thread contexts...");
		prepare_thread_contexts(client_request.correlation_id);
		return storage::control::message{
			storage::control::message_type::init_storage_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else if (storage_response.message_type == storage::control::message_type::error_response) {
		return storage::control::message{
			storage::control::message_type::error_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else {
		throw storage::runtime_error{
			DOCA_ERROR_UNEXPECTED,
			"Unexpected " + to_string(storage_response.message_type) + " while expecting a " +
				to_string(storage::control::message_type::init_storage_response),
		};
	}
}

storage::control::message zero_copy_app::process_start_storage(storage::control::message const &client_request)
{
	DOCA_LOG_DBG("Forward request to storage...");
	auto storage_request = storage::control::message{
		storage::control::message_type::start_storage_request,
		storage::control::message_id{m_message_id_counter++},
		client_request.correlation_id,
		{},
	};
	m_storage_control_channel->send_message(storage_request);

	// Wait for storage response
	auto storage_response = wait_for_control_message(storage_request.message_id, default_control_timeout_seconds);

	if (storage_response.message_type == storage::control::message_type::start_storage_response) {
		verify_connections_are_ready();
		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			worker_prepare_tasks_control_command cmd{m_transaction_count, m_remote_consumer_ids[ii]};
			m_workers[ii].execute_control_command(cmd);
		}
		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			worker_control_command cmd{worker_control_command::type::start_data_path};
			m_workers[ii].execute_control_command(cmd);
		}
		return storage::control::message{
			storage::control::message_type::start_storage_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else if (storage_response.message_type == storage::control::message_type::error_response) {
		return storage::control::message{
			storage::control::message_type::error_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else {
		throw storage::runtime_error{
			DOCA_ERROR_UNEXPECTED,
			"Unexpected " + to_string(storage_response.message_type) + " while expecting a " +
				to_string(storage::control::message_type::start_storage_response),
		};
	}
}

storage::control::message zero_copy_app::process_stop_storage(storage::control::message const &client_request)
{
	DOCA_LOG_DBG("Forward request to storage...");
	auto storage_request = storage::control::message{
		storage::control::message_type::stop_storage_request,
		storage::control::message_id{m_message_id_counter++},
		client_request.correlation_id,
		{},
	};
	m_storage_control_channel->send_message(storage_request);

	// Wait for storage response
	auto storage_response = wait_for_control_message(storage_request.message_id, default_control_timeout_seconds);

	if (storage_response.message_type == storage::control::message_type::stop_storage_response) {
		/* Stop all processing */
		m_stats.reserve(m_core_count);
		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			m_workers[ii].join_thread_proc();
			m_stats.push_back(m_workers[ii].get_stats());
			m_workers[ii].destroy_comch_objects();
		}
		return storage::control::message{
			storage::control::message_type::stop_storage_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else if (storage_response.message_type == storage::control::message_type::error_response) {
		return storage::control::message{
			storage::control::message_type::error_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else {
		throw storage::runtime_error{
			DOCA_ERROR_UNEXPECTED,
			"Unexpected " + to_string(storage_response.message_type) + " while expecting a " +
				to_string(storage::control::message_type::stop_storage_response),
		};
	}
}

storage::control::message zero_copy_app::process_shutdown(storage::control::message const &client_request)
{
	/* Wait for all remote comch objects to be destroyed and notified */
	while (!m_remote_consumer_ids.empty()) {
		auto *msg = m_client_control_channel->poll();
		DOCA_LOG_DBG("Ignoring unexpected %s while processing %s",
			     to_string(msg->message_type).c_str(),
			     to_string(storage::control::message_type::shutdown_request).c_str());
	}

	DOCA_LOG_DBG("Forward request to storage...");
	auto storage_request = storage::control::message{
		storage::control::message_type::shutdown_request,
		storage::control::message_id{m_message_id_counter++},
		client_request.correlation_id,
		{},
	};
	m_storage_control_channel->send_message(storage_request);

	// Wait for storage response
	auto storage_response = wait_for_control_message(storage_request.message_id, default_control_timeout_seconds);

	if (storage_response.message_type == storage::control::message_type::shutdown_response) {
		destroy_workers();
		return storage::control::message{
			storage::control::message_type::shutdown_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else if (storage_response.message_type == storage::control::message_type::error_response) {
		return storage::control::message{
			storage::control::message_type::error_response,
			client_request.message_id,
			client_request.correlation_id,
			std::move(storage_response.payload),
		};
	} else {
		throw storage::runtime_error{
			DOCA_ERROR_UNEXPECTED,
			"Unexpected " + to_string(storage_response.message_type) + " while expecting a " +
				to_string(storage::control::message_type::shutdown_response),
		};
	}
}

void zero_copy_app::prepare_thread_contexts(storage::control::correlation_id cid)
{
	m_workers = storage::make_aligned<zero_copy_app_worker>{}.object_array(m_core_count);

	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		m_workers[ii].create_thread_proc(m_cfg.cpu_set[ii]);

		worker_create_objects_control_command cmd{m_dev,
							  m_client_control_channel->get_comch_connection(),
							  m_transaction_count};
		m_workers[ii].execute_control_command(cmd);
		connect_rdma(ii, storage::control::rdma_connection_role::io_data, cid);
		connect_rdma(ii, storage::control::rdma_connection_role::io_control, cid);
	}
}

void zero_copy_app::connect_rdma(uint32_t thread_idx,
				 storage::control::rdma_connection_role role,
				 storage::control::correlation_id cid)
{
	auto &tctx = m_workers[thread_idx];
	doca_error_t ret;

	worker_export_local_rdma_connection_command export_cmd{role};
	ret = tctx.execute_control_command(export_cmd);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to export local RDMA connection"};
	}

	storage::control::message connect_rdma_request{
		storage::control::message_type::create_rdma_connection_request,
		storage::control::message_id{m_message_id_counter++},
		cid,
		std::make_unique<storage::control::rdma_connection_details_payload>(thread_idx,
										    role,
										    export_cmd.out_exported_blob),
	};

	m_storage_control_channel->send_message(connect_rdma_request);

	// Wait for storage response
	auto connect_rdma_response =
		wait_for_control_message(connect_rdma_request.message_id, default_control_timeout_seconds);

	if (connect_rdma_response.message_type == storage::control::message_type::create_rdma_connection_response) {
		auto *remote_details = reinterpret_cast<storage::control::rdma_connection_details_payload const *>(
			connect_rdma_response.payload.get());
		worker_import_local_rdma_connection_command input_cmd{role, remote_details->connection_details};
		ret = tctx.execute_control_command(input_cmd);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to import remote RDMA connection"};
		}
	} else if (connect_rdma_response.message_type == storage::control::message_type::error_response) {
		auto *error_details = reinterpret_cast<storage::control::error_response_payload const *>(
			connect_rdma_response.payload.get());
		throw storage::runtime_error{error_details->error_code, error_details->message};
	} else {
		throw storage::runtime_error{
			DOCA_ERROR_UNEXPECTED,
			"Unexpected " + to_string(connect_rdma_response.message_type) + " while expecting a " +
				to_string(storage::control::message_type::init_storage_response),
		};
	}
}

void zero_copy_app::verify_connections_are_ready(void)
{
	uint32_t not_ready_count;

	do {
		not_ready_count = 0;
		if (m_remote_consumer_ids.size() != m_core_count) {
			++not_ready_count;
			auto *msg = m_client_control_channel->poll();
			if (msg != nullptr) {
				throw storage::runtime_error{
					DOCA_ERROR_UNEXPECTED,
					"Unexpected " + to_string(msg->message_type) + " while processing " +
						to_string(storage::control::message_type::start_storage_request),
				};
			}
		}

		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			worker_are_contexts_ready_control_command cmd{};
			auto const ret = m_workers[ii].execute_control_command(cmd);
			if (ret != DOCA_SUCCESS) {
				throw storage::runtime_error{ret, "Failed to query connections status"};
			}

			if (cmd.out_status == DOCA_ERROR_IN_PROGRESS) {
				++not_ready_count;
			} else if (cmd.out_status != DOCA_SUCCESS) {
				throw storage::runtime_error{cmd.out_status,
							     "Failure while establishing RDMA connections"};
			}
		}

		if (m_abort_flag) {
			throw storage::runtime_error{DOCA_ERROR_INITIALIZATION,
						     "Aborted while establishing storage connections"};
		}
	} while (not_ready_count != 0);
}

void zero_copy_app::destroy_workers(void) noexcept
{
	if (m_workers != nullptr) {
		// Destroy all thread resources
		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			m_workers[ii].~zero_copy_app_worker();
		}
		storage::aligned_free(m_workers);
		m_workers = nullptr;
	}
}

} /* namespace */
