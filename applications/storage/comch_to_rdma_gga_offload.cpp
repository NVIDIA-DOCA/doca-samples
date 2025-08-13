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
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <thread>

#include <doca_argp.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_compress.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
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
#include <storage_common/io_message.hpp>
#include <storage_common/os_utils.hpp>
#include <storage_common/doca_utils.hpp>

DOCA_LOG_REGISTER(gga_offload);

using namespace std::string_literals;

namespace {
auto constexpr app_name = "doca_storage_comch_to_rdma_gga_offload";

auto constexpr default_control_timeout_seconds = std::chrono::seconds{5};
auto constexpr default_command_channel_name = "doca_storage_comch";

static_assert(sizeof(void *) == 8, "Expected a pointer to occupy 8 bytes");

enum class connection_role : uint8_t {
	data_1 = 0,
	data_2 = 1,
	data_p = 2,
	client = 3,
};

template <typename T, size_t N>
class per_connection_t {
public:
	T &operator[](connection_role role)
	{
#ifdef DOCA_DEBUG
		if (static_cast<uint8_t>(role) > N) {
			throw std::range_error{std::to_string(static_cast<uint8_t>(role)) + " exceeds range " +
					       std::to_string(N)};
		}
#endif
		return m_items[static_cast<uint8_t>(role)];
	}

	T const &operator[](connection_role role) const
	{
#ifdef DOCA_DEBUG
		if (static_cast<uint8_t>(role) > N) {
			throw std::range_error{std::to_string(static_cast<uint8_t>(role)) + " exceeds range " +
					       std::to_string(N)};
		}
#endif
		return m_items[static_cast<uint8_t>(role)];
	}

	T *begin() noexcept
	{
		return m_items.data();
	}

	T *end() noexcept
	{
		return m_items.data() + m_items.size();
	}

	T const *begin() const noexcept
	{
		return m_items.data();
	}

	T const *end() const noexcept
	{
		return m_items.data() + m_items.size();
	}

private:
	std::array<T, N> m_items;
};

template <typename T>
using per_ctrl_connection = per_connection_t<T, 4>;

template <typename T>
using per_storage_connection = per_connection_t<T, 3>;

struct gga_offload_app_configuration {
	std::vector<uint32_t> cpu_set = {};
	std::string device_id = {};
	std::string representor_id = {};
	std::string command_channel_name = {};
	std::chrono::seconds control_timeout = {};
	per_storage_connection<storage::ip_address> storage_server_address = {};
	std::string ec_matrix_type = {};
	uint32_t recover_freq = {};
};

struct thread_stats {
	uint32_t core_idx = 0;
	uint64_t pe_hit_count = 0;
	uint64_t pe_miss_count = 0;
	uint64_t operation_count = 0;
	uint64_t recovery_count = 0;
};

enum class transaction_mode : uint8_t {
	read,
	recover_a,
	recover_b,
};

/*
 * IO memory layout:
 *
 * Host and each of the 3 storage servers have a full block size * block count amount of storage.
 *
 * A host read will be split into two parts were each part comes from two of the 3 storage servers. In the case of a
 * "normal" read, the top half of the host request is filled by data_1 and the bottom half by data_2. These two halfs
 * are then treated as one region surrounded by a header and trailer to know how much compressed data is in the middle
 * part. That middle part is then decompressed as a single buffer and the output 2 * block_size is returned to the host.
 *
 * In a recovery read data_1 or data_2 fills its part as per usual, the parity data is read
 * from data_p. This data_p is provided with the other data chunk to doca_ec to restore the missing part
 *
 */
class gga_offload_app_worker {
public:
	struct alignas(storage::cache_line_size) transaction_context {
		char *initiator_io_message = nullptr; /* Non owning pointer to io message received from the initiator,
							 re-used when replying to the initiator. Memory owned by the
							 worker m_io_message_region */
		std::array<char *, 2> storage_io_messages = {}; /* Non owning pointer to io messages to be sent to
								    storage targets. Memory owned by the worker
								    m_io_message_region */
		doca_comch_consumer_task_post_recv *host_request_task = nullptr; /* Consumer task that refers to this
										    transaction, resubmitted once the
										    response to the initiator is
										    completed */
		doca_comch_producer_task_send *host_response_task = nullptr; /* Response to the initiator, can re-use
										the same IO message received by the
										consumer task as the consumer task is
										not re-submitted until after this task
										completes */
		per_storage_connection<doca_rdma_task_send *> requests = {}; /* Storage request tasks */
		doca_ec_task_recover *ec_recover_task = nullptr;	     /* Task used to perform EC data recovery */
		doca_compress_task_decompress_lz4_stream *decompress_task = nullptr; /* task used to perform data
											decompression */
		uint32_t block_idx = 0;		/* Index used to calculate the offset into local storage memory. Each
						   transaction uses unique memory to prevent corruption */
		transaction_mode mode;		/* transaction mode, read, recover, etc */
		uint8_t remaining_op_count = 0; /* Counter to track when all storage operations have completed */
	};

	static_assert(sizeof(gga_offload_app_worker::transaction_context) == (storage::cache_line_size * 2),
		      "Expected thread_context::transaction_context to occupy two cache lines");

	struct alignas(storage::cache_line_size) hot_data {
		doca_pe *pe = nullptr;
		doca_buf_inventory *io_buf_inv = nullptr;
		doca_mmap *local_io_mmap = nullptr;
		doca_mmap *remote_io_mmap = nullptr;
		uint64_t remote_memory_start_addr = 0;
		uint64_t local_memory_start_addr = 0;
		uint64_t tmp_blocks_offset = 0; /* offset from local_memory_start_addr where the temporary data blocks
						  start */
		uint64_t pe_hit_count = 0;
		uint64_t pe_miss_count = 0;
		uint64_t recovery_flow_count = 0;
		uint64_t completed_transaction_count = 0;
		transaction_context *transactions = nullptr;
		uint16_t in_flight_transaction_count = 0;
		uint32_t block_size = 0;
		uint32_t half_block_size = 0;
		uint16_t num_transactions = 0;
		uint16_t core_idx = 0;
		uint16_t recover_drop_count = 0; /* Counter of read ops to perform until the next simulated recovery
						    event. When this reaches 0 a recovery event will be simulated if
						    recover_drop_freq has a non-zero value. when recover_drop_freq is
						    zero no simulated recovery events will be performed */
		uint16_t recover_drop_freq = 0;	 /* Reset / initial value for ops_until_next_recover */
		std::atomic_bool run_flag = false;
		bool error_flag = false;

		doca_error_t start_transaction(doca_comch_consumer_task_post_recv *task, char const *io_message);

		void process_result(gga_offload_app_worker::transaction_context &transaction);

		void start_decompress(gga_offload_app_worker::transaction_context &transaction);

		void start_recover(gga_offload_app_worker::transaction_context &transaction);
	};

	static_assert(sizeof(gga_offload_app_worker::hot_data) == (storage::cache_line_size * 2),
		      "Expected thread_context::hot_data to occupy two cache lines");

	~gga_offload_app_worker();

	void init(doca_dev *dev,
		  doca_comch_connection *comch_conn,
		  doca_mmap *local_io_mmap,
		  doca_mmap *remote_io_mmap,
		  uint32_t transaction_count,
		  uint32_t block_size,
		  std::string const &ec_matrix_type,
		  uint32_t recover_drop_freq);

	[[maybe_unused]] gga_offload_app_worker &operator=(gga_offload_app_worker &&) noexcept;

	std::vector<uint8_t> get_local_rdma_connection_blob(connection_role conn_role,
							    storage::control::rdma_connection_role rdma_role);

	void connect_rdma(connection_role conn_role,
			  storage::control::rdma_connection_role rdma_role,
			  std::vector<uint8_t> const &blob);

	doca_error_t get_connections_state() const noexcept;

	void stop_processing(void) noexcept;

	void destroy_comch_objects(void) noexcept;

	void prepare_data_path(uint32_t block_idx_start, uint32_t remote_consumer_id);

	/*
	 * Prepare thread proc
	 * @core_id [in]: Core to run on
	 */
	void prepare_thread_proc(uint32_t core_id);

	void start_thread_proc();

	[[nodiscard]] bool is_thread_proc_running() const noexcept;

	[[nodiscard]] hot_data const &get_hot_data() const noexcept;

private:
	struct rdma_context {
		storage::rdma_conn_pair ctrl = {};
		storage::rdma_conn_pair data = {};
		std::vector<doca_rdma_task_send *> storage_request_tasks = {};
		std::vector<doca_rdma_task_receive *> storage_response_tasks = {};
	};

	hot_data m_hot_data = {};
	uint8_t *m_io_message_region = nullptr;
	doca_mmap *m_io_message_mmap = nullptr;
	doca_buf_inventory *m_buf_inv = nullptr;
	std::vector<doca_buf *> m_io_message_bufs = {};
	doca_comch_consumer *m_consumer = nullptr;
	doca_comch_producer *m_producer = nullptr;
	doca_ec *m_ec = nullptr;
	doca_ec_matrix *m_ec_matrix = nullptr;
	doca_compress *m_compress = nullptr;
	per_storage_connection<rdma_context> m_rdma = {};
	std::vector<doca_comch_consumer_task_post_recv *> m_host_request_tasks = {};
	std::vector<doca_comch_producer_task_send *> m_host_response_tasks = {};
	std::thread m_thread = {};

	void cleanup(void) noexcept;

	void alloc_rdma_send_task(transaction_context &transaction,
				  uint32_t transaction_idx,
				  doca_buf *task_buf,
				  connection_role role);
	void prepare_transaction(uint32_t transaction_idx, uint32_t block_idx, uint32_t remote_consumer_id);
	void alloc_rdma_recv_task(uint8_t *addr, connection_role role);
	void prepare_storage_io_response_tasks();

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

	static void doca_ec_task_recover_cb(doca_ec_task_recover *task,
					    doca_data task_user_data,
					    doca_data ctx_user_data) noexcept;

	static void doca_ec_task_recover_error_cb(doca_ec_task_recover *task,
						  doca_data task_user_data,
						  doca_data ctx_user_data) noexcept;

	static void doca_compress_task_decompress_lz4_stream_cb(doca_compress_task_decompress_lz4_stream *task,
								doca_data task_user_data,
								doca_data ctx_user_data) noexcept;

	static void doca_compress_task_decompress_lz4_stream_error_cb(doca_compress_task_decompress_lz4_stream *task,
								      doca_data task_user_data,
								      doca_data ctx_user_data) noexcept;

	void thread_proc();
};

class gga_offload_app {
public:
	~gga_offload_app();

	gga_offload_app() = delete;

	explicit gga_offload_app(gga_offload_app_configuration const &cfg);

	gga_offload_app(gga_offload_app const &) = delete;

	gga_offload_app(gga_offload_app &&) noexcept = delete;

	gga_offload_app &operator=(gga_offload_app const &) = delete;

	gga_offload_app &operator=(gga_offload_app &&) noexcept = delete;

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
	gga_offload_app_configuration const m_cfg;
	doca_dev *m_dev;
	doca_dev_rep *m_dev_rep;
	doca_mmap *m_remote_io_mmap;
	uint8_t *m_local_io_region;
	doca_mmap *m_local_io_mmap;
	per_ctrl_connection<std::unique_ptr<storage::control::channel>> m_all_ctrl_channels;
	per_storage_connection<storage::control::channel *> m_storage_ctrl_channels;
	std::vector<storage::control::message> m_ctrl_messages;
	std::vector<uint32_t> m_remote_consumer_ids;
	gga_offload_app_worker *m_workers;
	std::vector<thread_stats> m_stats;
	uint64_t m_storage_capacity;
	uint32_t m_storage_block_size;
	uint32_t m_message_id_counter;
	uint32_t m_num_transactions;
	uint32_t m_core_count;
	bool m_abort_flag;

	static void new_comch_consumer_callback(void *user_data, uint32_t id) noexcept;

	static void expired_comch_consumer_callback(void *user_data, uint32_t id) noexcept;

	storage::control::message wait_for_control_message();

	void wait_for_responses(std::vector<storage::control::message_id> const &mids, std::chrono::seconds timeout);

	storage::control::message get_response(storage::control::message_id mids);

	void discard_responses(std::vector<storage::control::message_id> const &mids);

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
 * @return: Parsed gga_offload_app_configuration
 *
 * @throws: storage::runtime_error If the gga_offload_app_configuration cannot pe parsed or contains invalid values
 */
gga_offload_app_configuration parse_cli_args(int argc, char **argv);
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
		gga_offload_app app{parse_cli_args(argc, argv)};
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
 * Print the parsed gga_offload_app_configuration
 *
 * @cfg [in]: gga_offload_app_configuration to display
 */
void print_config(gga_offload_app_configuration const &cfg) noexcept
{
	printf("gga_offload_app_configuration: {\n");
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
	printf("\tstorage_server[data_1] : %s:%u\n",
	       cfg.storage_server_address[connection_role::data_1].get_address().c_str(),
	       cfg.storage_server_address[connection_role::data_1].get_port());
	printf("\tdata_2_storage_server : %s:%u\n",
	       cfg.storage_server_address[connection_role::data_2].get_address().c_str(),
	       cfg.storage_server_address[connection_role::data_2].get_port());
	printf("\tdata_p_storage_server : %s:%u\n",
	       cfg.storage_server_address[connection_role::data_p].get_address().c_str(),
	       cfg.storage_server_address[connection_role::data_p].get_port());
	printf("\trecover_freq : %u\n", cfg.recover_freq);
	printf("}\n");
}

/*
 * Validate gga_offload_app_configuration
 *
 * @cfg [in]: gga_offload_app_configuration
 */
void validate_gga_offload_app_configuration(gga_offload_app_configuration const &cfg)
{
	std::vector<std::string> errors;

	if (cfg.control_timeout.count() == 0) {
		errors.emplace_back("Invalid gga_offload_app_configuration: control-timeout must not be zero");
	}

	if (!errors.empty()) {
		for (auto const &err : errors) {
			printf("%s\n", err.c_str());
		}
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Invalid gga_offload_app_configuration detected"};
	}
}

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed gga_offload_app_configuration
 *
 * @throws: storage::runtime_error If the gga_offload_app_configuration cannot pe parsed or contains invalid values
 */
gga_offload_app_configuration parse_cli_args(int argc, char **argv)
{
	gga_offload_app_configuration config{};
	config.command_channel_name = default_command_channel_name;
	config.control_timeout = default_control_timeout_seconds;
	config.ec_matrix_type = "vandermonde";

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
					       static_cast<gga_offload_app_configuration *>(cfg)->device_id =
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
					       static_cast<gga_offload_app_configuration *>(cfg)->representor_id =
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
					       static_cast<gga_offload_app_configuration *>(cfg)->cpu_set.push_back(
						       *static_cast<int *>(value));
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "data-1-storage",
				       "Storage server addresses in <ip_addr>:<port> format",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       try {
						       static_cast<gga_offload_app_configuration *>(cfg)
							       ->storage_server_address[connection_role::data_1] =
							       storage::parse_ip_v4_address(
								       static_cast<char const *>(value));
						       return DOCA_SUCCESS;
					       } catch (storage::runtime_error const &ex) {
						       return DOCA_ERROR_INVALID_VALUE;
					       }
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "data-2-storage",
				       "Storage server addresses in <ip_addr>:<port> format",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       try {
						       static_cast<gga_offload_app_configuration *>(cfg)
							       ->storage_server_address[connection_role::data_2] =
							       storage::parse_ip_v4_address(
								       static_cast<char const *>(value));
						       return DOCA_SUCCESS;
					       } catch (storage::runtime_error const &ex) {
						       return DOCA_ERROR_INVALID_VALUE;
					       }
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "data-p-storage",
				       "Storage server addresses in <ip_addr>:<port> format",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       try {
						       static_cast<gga_offload_app_configuration *>(cfg)
							       ->storage_server_address[connection_role::data_p] =
							       storage::parse_ip_v4_address(
								       static_cast<char const *>(value));
						       return DOCA_SUCCESS;
					       } catch (storage::runtime_error const &ex) {
						       return DOCA_ERROR_INVALID_VALUE;
					       }
				       });

	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "matrix-type",
				       "Type of matrix to use. One of: cauchy, vandermonde Default: vandermonde",
				       storage::optional_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_app_configuration *>(cfg)->ec_matrix_type =
						       static_cast<char const *>(value);
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
			static_cast<gga_offload_app_configuration *>(cfg)->command_channel_name =
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
					       static_cast<gga_offload_app_configuration *>(cfg)->control_timeout =
						       std::chrono::seconds{*static_cast<int *>(value)};
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_INT,
				       nullptr,
				       "trigger-recovery-read-every-n",
				       "Trigger a recovery read flow every N th request. Default: 0 (disabled)",
				       storage::optional_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_app_configuration *>(cfg)->recover_freq =
						       *static_cast<int *>(value);
					       return DOCA_SUCCESS;
				       });
	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args"};
	}

	static_cast<void>(doca_argp_destroy());

	print_config(config);
	validate_gga_offload_app_configuration(config);

	return config;
}

storage::control::message make_error_response(storage::control::message_id const &request_mid,
					      storage::control::correlation_id request_cid,
					      storage::control::message response,
					      storage::control::message_type expected_response_type)
{
	doca_error_t err_code;
	std::string err_msg;
	if (response.message_type == storage::control::message_type::error_response) {
		auto *const err_details =
			dynamic_cast<storage::control::error_response_payload *>(response.payload.get());
		if (err_details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_UNEXPECTED, "[BUG] invalid error_response"};
		}

		err_code = err_details->error_code;
		err_msg = std::move(err_details->message);
	} else {
		err_code = DOCA_ERROR_UNEXPECTED;
		err_msg = "Unexpected " + to_string(response.message_type) + " while expecting a " +
			  to_string(expected_response_type);
	}

	return storage::control::message{
		storage::control::message_type::error_response,
		request_mid,
		request_cid,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),
	};
}

char *io_message_from_doca_buf(doca_buf const *buf)
{
	void *data;
	static_cast<void>(doca_buf_get_data(buf, &data));
	return static_cast<char *>(data);
}

doca_error_t gga_offload_app_worker::hot_data::start_transaction(doca_comch_consumer_task_post_recv *task,
								 char const *io_message)
{
	auto const type = storage::io_message_view::get_type(io_message);

	if (type != storage::io_message_type::read) {
		error_flag = true;
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	auto const transaction_idx = doca_task_get_user_data(doca_comch_consumer_task_post_recv_as_task(task)).u64;
	auto &transaction = transactions[transaction_idx];

	if (transaction.remaining_op_count != 0) {
		error_flag = true;
		return DOCA_ERROR_BAD_STATE;
	}

	transaction.host_request_task = task;
	transaction.remaining_op_count = 4; // 2 * rdma send + 2 * rdma recv

	connection_role part_a_conn = connection_role::data_1;
	connection_role part_b_conn = connection_role::data_2;

	transaction.mode = transaction_mode::read;

	uint32_t requestor_half_block_a_offset;
	uint32_t requestor_half_block_b_offset;

	if (recover_drop_freq != 0 && (--recover_drop_count) == 0) {
		recover_drop_count = recover_drop_freq;
		++recovery_flow_count;

		if (recovery_flow_count % 2 == 0) {
			transaction.mode = transaction_mode::recover_a;
			part_a_conn = connection_role::data_p;
			part_b_conn = connection_role::data_2;

			requestor_half_block_a_offset = tmp_blocks_offset + (transaction.block_idx * half_block_size);
			requestor_half_block_b_offset = (transaction.block_idx * block_size) + half_block_size;
		} else {
			transaction.mode = transaction_mode::recover_b;
			part_a_conn = connection_role::data_1;
			part_b_conn = connection_role::data_p;

			requestor_half_block_a_offset = transaction.block_idx * block_size;
			requestor_half_block_b_offset = tmp_blocks_offset + (transaction.block_idx * half_block_size);
		}
	} else {
		requestor_half_block_a_offset = transaction.block_idx * block_size;
		requestor_half_block_b_offset = requestor_half_block_a_offset + half_block_size;
	}

	auto *part_a_io_message = transaction.storage_io_messages[0];
	auto *part_b_io_message = transaction.storage_io_messages[1];
	auto *response_io_message =
		io_message_from_doca_buf(doca_comch_producer_task_send_get_buf(transaction.host_response_task));

	auto const user_data = storage::io_message_view::get_user_data(io_message);

	storage::io_message_view::set_correlation_id(transaction_idx, part_a_io_message);
	storage::io_message_view::set_correlation_id(transaction_idx, part_b_io_message);

	storage::io_message_view::set_type(type, part_a_io_message);
	storage::io_message_view::set_type(type, part_b_io_message);

	storage::io_message_view::set_user_data(user_data, part_a_io_message);
	storage::io_message_view::set_user_data(user_data, part_b_io_message);

	storage::io_message_view::set_requester_offset(requestor_half_block_a_offset, part_a_io_message);
	storage::io_message_view::set_requester_offset(requestor_half_block_b_offset, part_b_io_message);

	auto const storage_io_offset = storage::io_message_view::get_storage_offset(io_message);
	storage::io_message_view::set_storage_offset(storage_io_offset, part_a_io_message);
	storage::io_message_view::set_storage_offset(storage_io_offset + half_block_size, part_b_io_message);

	storage::io_message_view::set_io_size(half_block_size, part_a_io_message);
	storage::io_message_view::set_io_size(half_block_size, part_b_io_message);

	// Default the response to success, if any IO operation fails it will be changed
	storage::io_message_view::set_correlation_id(storage::io_message_view::get_correlation_id(io_message),
						     response_io_message);
	storage::io_message_view::set_type(storage::io_message_type::result, response_io_message);
	storage::io_message_view::set_result(DOCA_SUCCESS, response_io_message);

	doca_error_t ret;

	/*
	 * NOTE: if either send task fails intentionally leave things as they are (remaining_op_count for example) as
	 * any parts that were sent should NOT trigger and action upon their eventual completion
	 */
	doca_buf_set_data(const_cast<doca_buf *>(doca_rdma_task_send_get_src_buf(transaction.requests[part_a_conn])),
			  part_a_io_message,
			  storage::size_of_io_message);
	ret = doca_task_submit(doca_rdma_task_send_as_task(transaction.requests[part_a_conn]));
	if (ret != DOCA_SUCCESS) {
		error_flag = true;
		return ret;
	}

	doca_buf_set_data(const_cast<doca_buf *>(doca_rdma_task_send_get_src_buf(transaction.requests[part_b_conn])),
			  part_b_io_message,
			  storage::size_of_io_message);
	ret = doca_task_submit(doca_rdma_task_send_as_task(transaction.requests[part_b_conn]));
	if (ret != DOCA_SUCCESS) {
		error_flag = true;
		return ret;
	}

	++in_flight_transaction_count;

	return DOCA_SUCCESS;
}

void gga_offload_app_worker::hot_data::process_result(gga_offload_app_worker::transaction_context &transaction)
{
	if (transaction.mode == transaction_mode::read) {
		transaction.remaining_op_count = 1;
		start_decompress(transaction);
	} else {
		transaction.remaining_op_count = 2;
		start_recover(transaction);
	}
}

void gga_offload_app_worker::hot_data::start_decompress(gga_offload_app_worker::transaction_context &transaction)
{
	auto *const local_block_start =
		reinterpret_cast<char *>(local_memory_start_addr) + (transaction.block_idx * block_size);
	auto const *hdr = reinterpret_cast<storage::compressed_block_header const *>(local_block_start);

	doca_buf *src_buff;
	static_cast<void>(
		doca_buf_inventory_buf_get_by_data(io_buf_inv,
						   local_io_mmap,
						   local_block_start + sizeof(storage::compressed_block_header),
						   be32toh(hdr->compressed_size),
						   &src_buff));

	doca_buf *dst_buff;
	static_cast<void>(doca_buf_inventory_buf_get_by_addr(
		io_buf_inv,
		remote_io_mmap,
		reinterpret_cast<char *>(remote_memory_start_addr) +
			storage::io_message_view::get_requester_offset(transaction.initiator_io_message),
		block_size,
		&dst_buff));

	doca_compress_task_decompress_lz4_stream_set_src(transaction.decompress_task, src_buff);
	doca_compress_task_decompress_lz4_stream_set_dst(transaction.decompress_task, dst_buff);
	// do decompress
	auto const ret =
		doca_task_submit(doca_compress_task_decompress_lz4_stream_as_task(transaction.decompress_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit decompress task");
		error_flag = true;
		run_flag = false;
	}
}

void gga_offload_app_worker::hot_data::start_recover(gga_offload_app_worker::transaction_context &transaction)
{
	doca_buf *d1_buf;
	doca_buf *d2_buf;
	doca_buf *dp_buf;

	auto *const d1_addr = reinterpret_cast<char *>(local_memory_start_addr) + (transaction.block_idx * block_size);
	auto *const dp_addr = reinterpret_cast<char *>(local_memory_start_addr) + tmp_blocks_offset +
			      (transaction.block_idx * half_block_size);

	static_cast<void>(
		doca_buf_inventory_buf_get_by_data(io_buf_inv, local_io_mmap, d1_addr, half_block_size, &d1_buf));

	static_cast<void>(doca_buf_inventory_buf_get_by_data(io_buf_inv,
							     local_io_mmap,
							     d1_addr + half_block_size,
							     half_block_size,
							     &d2_buf));
	static_cast<void>(
		doca_buf_inventory_buf_get_by_data(io_buf_inv, local_io_mmap, dp_addr, half_block_size, &dp_buf));

	if (transaction.mode == transaction_mode::recover_a) {
		static_cast<void>(doca_buf_chain_list(dp_buf, d2_buf));
		doca_buf_reset_data_len(d1_buf);
		doca_ec_task_recover_set_available_blocks(transaction.ec_recover_task, dp_buf);
		doca_ec_task_recover_set_recovered_data_blocks(transaction.ec_recover_task, d1_buf);
	} else {
		static_cast<void>(doca_buf_chain_list(d1_buf, dp_buf));
		doca_buf_reset_data_len(d2_buf);
		doca_ec_task_recover_set_available_blocks(transaction.ec_recover_task, d1_buf);
		doca_ec_task_recover_set_recovered_data_blocks(transaction.ec_recover_task, d2_buf);
	}

	// do recover
	auto const ret = doca_task_submit(doca_ec_task_recover_as_task(transaction.ec_recover_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit decompress task");
		error_flag = true;
		run_flag = false;
	}
}

gga_offload_app_worker::~gga_offload_app_worker()
{
	if (m_thread.joinable()) {
		m_hot_data.run_flag = false;
		m_hot_data.error_flag = true;
		m_thread.join();
	}
	cleanup();
}

void gga_offload_app_worker::init(doca_dev *dev,
				  doca_comch_connection *comch_conn,
				  doca_mmap *local_io_mmap,
				  doca_mmap *remote_io_mmap,
				  uint32_t transaction_count,
				  uint32_t block_size,
				  std::string const &ec_matrix_type,
				  uint32_t recover_drop_freq)
{
	doca_error_t ret;

	auto const page_size = storage::get_system_page_size();

	m_hot_data.num_transactions = transaction_count;
	m_hot_data.transactions =
		storage::make_aligned<transaction_context>{}.object_array(m_hot_data.num_transactions);
	m_hot_data.local_io_mmap = local_io_mmap;
	m_hot_data.remote_io_mmap = remote_io_mmap;

	{
		char *io_local_region_begin = nullptr;
		char *io_remote_region_begin = nullptr;
		size_t io_local_region_size = 0;
		size_t io_remote_region_size = 0;
		ret = doca_mmap_get_memrange(local_io_mmap,
					     reinterpret_cast<void **>(&io_local_region_begin),
					     &io_local_region_size);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to query memrange for local mmap"};
		}

		ret = doca_mmap_get_memrange(remote_io_mmap,
					     reinterpret_cast<void **>(&io_remote_region_begin),
					     &io_remote_region_size);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to query memrange for remote mmap"};
		}

		m_hot_data.local_memory_start_addr = reinterpret_cast<uint64_t>(io_local_region_begin);
		m_hot_data.remote_memory_start_addr = reinterpret_cast<uint64_t>(io_remote_region_begin);
	}

	m_hot_data.tmp_blocks_offset = block_size * m_hot_data.num_transactions;
	m_hot_data.block_size = block_size;
	m_hot_data.half_block_size = block_size / 2;

	/* 6 IO messages per transaction:
	 *	- 1: initiator request / response
	 *	- 2: storage IO requests
	 *	- 3: storage IO responses
	 */
	auto constexpr io_messages_per_transaction = 6;

	auto const num_io_messages = io_messages_per_transaction * m_hot_data.num_transactions;
	auto const raw_io_messages_size = num_io_messages * storage::size_of_io_message;

	DOCA_LOG_DBG("Allocate io messages memory (%zu bytes, aligned to %u byte pages)",
		     raw_io_messages_size,
		     page_size);
	m_io_message_region = static_cast<uint8_t *>(
		storage::aligned_alloc(page_size, storage::aligned_size(page_size, raw_io_messages_size)));
	if (m_io_message_region == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate io messages"};
	}
	m_io_message_bufs.reserve(num_io_messages);

	m_io_message_mmap = storage::make_mmap(dev,
					       reinterpret_cast<char *>(m_io_message_region),
					       raw_io_messages_size,
					       DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);

	/* 10 doca_buf objects per transaction:
	 *	- 1: initiator request / response
	 *	- 3: IO request
	 *	- 3: IO response
	 *	- 3: GGA tasks
	 */
	auto constexpr doca_buf_objects_per_transaction = 10;
	ret = doca_buf_inventory_create(m_hot_data.num_transactions * doca_buf_objects_per_transaction, &m_buf_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_buf_inventory"};
	}

	ret = doca_buf_inventory_start(m_buf_inv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_buf_inventory"};
	}
	m_hot_data.io_buf_inv = m_buf_inv;

	DOCA_LOG_DBG("Create hot path progress engine");
	ret = doca_pe_create(std::addressof(m_hot_data.pe));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_pe"};
	}

	m_consumer = storage::make_comch_consumer(comch_conn,
						  m_io_message_mmap,
						  m_hot_data.pe,
						  m_hot_data.num_transactions,
						  doca_data{.ptr = std::addressof(m_hot_data)},
						  doca_comch_consumer_task_post_recv_cb,
						  doca_comch_consumer_task_post_recv_error_cb);

	m_producer = storage::make_comch_producer(comch_conn,
						  m_hot_data.pe,
						  m_hot_data.num_transactions,
						  doca_data{.ptr = std::addressof(m_hot_data)},
						  doca_comch_producer_task_send_cb,
						  doca_comch_producer_task_send_error_cb);

	ret = doca_ec_create(dev, &m_ec);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_ec"};
	}

	ret = doca_ctx_set_user_data(doca_ec_as_ctx(m_ec), doca_data{.ptr = std::addressof(m_hot_data)});
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to set doca_ec user data: "s + doca_error_get_name(ret)};
	}

	ret = doca_pe_connect_ctx(m_hot_data.pe, doca_ec_as_ctx(m_ec));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to connect doca_ec to progress engine"};
	}

	ret = doca_ec_task_recover_set_conf(m_ec,
					    doca_ec_task_recover_cb,
					    doca_ec_task_recover_error_cb,
					    m_hot_data.num_transactions);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_ec_task_recover task pool"};
	}

	ret = doca_ctx_start(doca_ec_as_ctx(m_ec));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_ec"};
	}

	// Create a matrix that creates one redundancy block per 2 data blocks
	ret = doca_ec_matrix_create(m_ec, storage::matrix_type_from_string(ec_matrix_type), 2, 1, &m_ec_matrix);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_ec matrix"};
	}

	ret = doca_compress_create(dev, &m_compress);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_compress"};
	}

	ret = doca_ctx_set_user_data(doca_compress_as_ctx(m_compress), doca_data{.ptr = std::addressof(m_hot_data)});
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret,
					     "Failed to set doca_compress user data: "s + doca_error_get_name(ret)};
	}

	ret = doca_pe_connect_ctx(m_hot_data.pe, doca_compress_as_ctx(m_compress));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to conncompresst doca_compress to progress engine"};
	}

	ret = doca_compress_task_decompress_lz4_stream_set_conf(m_compress,
								doca_compress_task_decompress_lz4_stream_cb,
								doca_compress_task_decompress_lz4_stream_error_cb,
								m_hot_data.num_transactions);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret,
					     "Failed to create doca_compress_task_decompress_lz4_stream task pool"};
	}

	ret = doca_ctx_start(doca_compress_as_ctx(m_compress));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_compress"};
	}

	auto constexpr rdma_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
					  DOCA_ACCESS_FLAG_RDMA_WRITE;

	for (auto &ctx : m_rdma) {
		ctx.ctrl.rdma = storage::make_rdma_context(dev,
							   m_hot_data.pe,
							   doca_data{.ptr = std::addressof(m_hot_data)},
							   rdma_permissions);

		ret = doca_rdma_task_receive_set_conf(ctx.ctrl.rdma,
						      doca_rdma_task_receive_cb,
						      doca_rdma_task_receive_error_cb,
						      m_hot_data.num_transactions);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to configure rdma receive task pool"};
		}

		ret = doca_rdma_task_send_set_conf(ctx.ctrl.rdma,
						   doca_rdma_task_send_cb,
						   doca_rdma_task_send_error_cb,
						   m_hot_data.num_transactions);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to configure rdma send task pool"};
		}

		ret = doca_ctx_start(doca_rdma_as_ctx(ctx.ctrl.rdma));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to start doca_rdma context"};
		}

		ctx.data.rdma = storage::make_rdma_context(dev,
							   m_hot_data.pe,
							   doca_data{.ptr = std::addressof(m_hot_data)},
							   rdma_permissions);

		ret = doca_ctx_start(doca_rdma_as_ctx(ctx.data.rdma));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to start doca_rdma context"};
		}
	}

	m_hot_data.run_flag = false;
	m_hot_data.error_flag = false;
	m_hot_data.pe_hit_count = 0;
	m_hot_data.pe_miss_count = 0;
	m_hot_data.completed_transaction_count = 0;
	m_hot_data.in_flight_transaction_count = 0;
	m_hot_data.recover_drop_count = recover_drop_freq;
	m_hot_data.recover_drop_freq = recover_drop_freq;
}

std::vector<uint8_t> gga_offload_app_worker::get_local_rdma_connection_blob(
	connection_role conn_role,
	storage::control::rdma_connection_role rdma_role)
{
	doca_error_t ret;
	uint8_t const *blob = nullptr;
	size_t blob_size = 0;

	auto &rdma_ctx = m_rdma[conn_role];
	auto &rdma_pair = rdma_role == storage::control::rdma_connection_role::io_data ? rdma_ctx.data : rdma_ctx.ctrl;
	ret = doca_rdma_export(rdma_pair.rdma,
			       reinterpret_cast<void const **>(&blob),
			       &blob_size,
			       std::addressof(rdma_pair.conn));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Core: %u RDMA export failed: %s", m_hot_data.core_idx, doca_error_get_name(ret));
		throw storage::runtime_error{ret, "Failed to export rdma connection"};
	}

	return std::vector<uint8_t>{blob, blob + blob_size};
}

void gga_offload_app_worker::connect_rdma(connection_role conn_role,
					  storage::control::rdma_connection_role rdma_role,
					  std::vector<uint8_t> const &blob)
{
	auto &rdma_ctx = m_rdma[conn_role];
	auto &rdma_pair = rdma_role == storage::control::rdma_connection_role::io_data ? rdma_ctx.data : rdma_ctx.ctrl;

	doca_error_t ret;
	ret = doca_rdma_connect(rdma_pair.rdma, blob.data(), blob.size(), rdma_pair.conn);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Core: %u RDMA connect failed: %s", m_hot_data.core_idx, doca_error_get_name(ret));
		throw storage::runtime_error{ret, "Failed to connect to rdma"};
	}
}

doca_error_t gga_offload_app_worker::get_connections_state() const noexcept
{
	doca_error_t ret;
	doca_ctx_states ctx_state;
	uint32_t pending_count = 0;

	ret = doca_ctx_get_state(doca_comch_producer_as_ctx(m_producer), &ctx_state);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query comch producer state: %s", doca_error_get_name(ret));
		return ret;
	}

	if (ctx_state != DOCA_CTX_STATE_RUNNING) {
		++pending_count;
		static_cast<void>(doca_pe_progress(m_hot_data.pe));
	}

	ret = doca_ctx_get_state(doca_comch_consumer_as_ctx(m_consumer), &ctx_state);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query comch consumer state: %s", doca_error_get_name(ret));
		return ret;
	}

	if (ctx_state != DOCA_CTX_STATE_RUNNING) {
		++pending_count;
		static_cast<void>(doca_pe_progress(m_hot_data.pe));
	}

	for (auto &ctx : m_rdma) {
		ret = doca_ctx_get_state(doca_rdma_as_ctx(ctx.data.rdma), &ctx_state);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(ret));
			return ret;
		}

		if (ctx_state != DOCA_CTX_STATE_RUNNING) {
			++pending_count;
			static_cast<void>(doca_pe_progress(m_hot_data.pe));
		}

		ret = doca_ctx_get_state(doca_rdma_as_ctx(ctx.ctrl.rdma), &ctx_state);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query rdma context state: %s", doca_error_get_name(ret));
			return ret;
		}

		if (ctx_state != DOCA_CTX_STATE_RUNNING) {
			++pending_count;
			static_cast<void>(doca_pe_progress(m_hot_data.pe));
		}
	}

	return (pending_count == 0) ? DOCA_SUCCESS : DOCA_ERROR_IN_PROGRESS;
}

void gga_offload_app_worker::stop_processing(void) noexcept
{
	m_hot_data.run_flag = false;
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void gga_offload_app_worker::destroy_comch_objects(void) noexcept
{
	doca_error_t ret;
	std::vector<doca_task *> tasks;

	if (m_consumer != nullptr) {
		tasks.reserve(m_host_request_tasks.size());
		std::transform(std::begin(m_host_request_tasks),
			       std::end(m_host_request_tasks),
			       std::back_inserter(tasks),
			       doca_comch_consumer_task_post_recv_as_task);
		ret = storage::stop_context(doca_comch_consumer_as_ctx(m_consumer), m_hot_data.pe, tasks);
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
		ret = storage::stop_context(doca_comch_producer_as_ctx(m_producer), m_hot_data.pe, tasks);
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

void gga_offload_app_worker::prepare_data_path(uint32_t block_idx_start, uint32_t remote_consumer_id)
{
	m_host_request_tasks.reserve(m_hot_data.num_transactions);
	m_host_response_tasks.reserve(m_hot_data.num_transactions);
	for (auto &ctx : m_rdma) {
		ctx.storage_request_tasks.reserve(m_hot_data.num_transactions);
		ctx.storage_response_tasks.reserve(m_hot_data.num_transactions);
	}

	for (uint32_t ii = 0; ii != m_hot_data.num_transactions; ++ii) {
		prepare_transaction(ii, block_idx_start + ii, remote_consumer_id);
	}

	prepare_storage_io_response_tasks();
}

void gga_offload_app_worker::prepare_thread_proc(uint32_t core_id)
{
	m_thread = std::thread{[this]() {
		try {
			thread_proc();
		} catch (std::exception const &ex) {
			DOCA_LOG_ERR("Core: %u Exception: %s", m_hot_data.core_idx, ex.what());
			m_hot_data.error_flag = true;
			m_hot_data.run_flag = false;
		}
	}};
	m_hot_data.core_idx = core_id;
	storage::set_thread_affinity(m_thread, m_hot_data.core_idx);
}

void gga_offload_app_worker::start_thread_proc(void)
{
	// Submit initial tasks
	doca_error_t ret;
	for (auto *task : m_host_request_tasks) {
		ret = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit initial doca_comch_consumer_task_post_recv task: %s",
				     doca_error_get_name(ret));
			throw storage::runtime_error{ret, "Failed to submit initial task"};
		}
	}

	for (auto &ctx : m_rdma) {
		for (auto *task : ctx.storage_response_tasks) {
			ret = doca_task_submit(doca_rdma_task_receive_as_task(task));
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit initial doca_rdma_task_receive task: %s",
					     doca_error_get_name(ret));
				throw storage::runtime_error{ret, "Failed to submit initial task"};
			}
		}
	}

	m_hot_data.run_flag = true;
}

gga_offload_app_worker::hot_data const &gga_offload_app_worker::get_hot_data(void) const noexcept
{
	return m_hot_data;
}

void gga_offload_app_worker::cleanup(void) noexcept
{
	doca_error_t ret;
	std::vector<doca_task *> tasks;

	for (auto &ctx : m_rdma) {
		if (ctx.ctrl.rdma != nullptr) {
			tasks.clear();
			tasks.reserve(ctx.storage_request_tasks.size() + ctx.storage_response_tasks.size());
			std::transform(std::begin(ctx.storage_request_tasks),
				       std::end(ctx.storage_request_tasks),
				       std::back_inserter(tasks),
				       doca_rdma_task_send_as_task);
			std::transform(std::begin(ctx.storage_response_tasks),
				       std::end(ctx.storage_response_tasks),
				       std::back_inserter(tasks),
				       doca_rdma_task_receive_as_task);

			/* stop context with tasks list (tasks must be destroyed to finish stopping process) */
			ret = storage::stop_context(doca_rdma_as_ctx(ctx.ctrl.rdma), m_hot_data.pe, tasks);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop rdma control context: %s", doca_error_get_name(ret));
			}

			ret = doca_rdma_destroy(ctx.ctrl.rdma);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy rdma control context: %s", doca_error_get_name(ret));
			}
		}

		if (ctx.data.rdma != nullptr) {
			// No tasks allocated on this side for the data context, all tasks are executed from the storage
			// side
			ret = doca_ctx_stop(doca_rdma_as_ctx(ctx.data.rdma));
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop rdma data context: %s", doca_error_get_name(ret));
			}

			ret = doca_rdma_destroy(ctx.data.rdma);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy rdma data context: %s", doca_error_get_name(ret));
			}
		}
	}

	destroy_comch_objects();

	if (m_hot_data.pe != nullptr) {
		ret = doca_pe_destroy(m_hot_data.pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine");
		}
	}

	for (auto *buf : m_io_message_bufs) {
		static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
	}

	if (m_buf_inv) {
		ret = doca_buf_inventory_stop(m_buf_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop buffer inventory");
		}
		ret = doca_buf_inventory_destroy(m_buf_inv);
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
}

void gga_offload_app_worker::alloc_rdma_send_task(transaction_context &transaction,
						  uint32_t transaction_idx,
						  doca_buf *task_buf,
						  connection_role role)
{
	auto const ret = doca_rdma_task_send_allocate_init(m_rdma[role].ctrl.rdma,
							   m_rdma[role].ctrl.conn,
							   task_buf,
							   doca_data{.u64 = transaction_idx},
							   std::addressof(transaction.requests[role]));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to allocate rdma doca_rdma_task_send"};
	}
	m_rdma[role].storage_request_tasks.push_back(transaction.requests[role]);
}

void gga_offload_app_worker::prepare_transaction(uint32_t transaction_idx,
						 uint32_t block_idx,
						 uint32_t remote_consumer_id)
{
	doca_error_t ret;

	auto &transaction = m_hot_data.transactions[transaction_idx];
	transaction.remaining_op_count = 0;
	transaction.block_idx = block_idx;

	{
		/* Note: Each transaction gets 3 IO messages worth of data associated with it. These are placed in
		 * memory beside each other, with all the response IO messages placed after them */
		auto *addr = reinterpret_cast<char *>(m_io_message_region +
						      (transaction.block_idx * storage::size_of_io_message * 3));
		transaction.initiator_io_message = addr;
		addr += storage::size_of_io_message;
		transaction.storage_io_messages[0] = addr;
		addr += storage::size_of_io_message;
		transaction.storage_io_messages[1] = addr;
	}

	/* Prepare consumer and producer tasks */
	{
		/* Note: This buffer is used for both the consumer and producer task, so the service just updates the
		 * type to result and places a status code and all other values are then returned to the initiator
		 * un-modified without spending any effort copying them, which it would have to do if there were two
		 * buffers */
		doca_buf *initiator_io_message_buf = nullptr;

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_io_message_mmap,
							 transaction.initiator_io_message,
							 storage::size_of_io_message,
							 &initiator_io_message_buf);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get consumer io message doca_buf"};
		}

		m_io_message_bufs.push_back(initiator_io_message_buf);

		doca_buf *io_response_message_buf = nullptr;
		ret = doca_buf_inventory_buf_get_by_data(m_buf_inv,
							 m_io_message_mmap,
							 transaction.initiator_io_message,
							 storage::size_of_io_message,
							 &io_response_message_buf);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get producer io message doca_buf"};
		}

		m_io_message_bufs.push_back(io_response_message_buf);

		doca_comch_consumer_task_post_recv *comch_consumer_task_post_recv = nullptr;
		ret = doca_comch_consumer_task_post_recv_alloc_init(m_consumer,
								    initiator_io_message_buf,
								    &comch_consumer_task_post_recv);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to allocate consumer task"};
		}
		doca_task_set_user_data(doca_comch_consumer_task_post_recv_as_task(comch_consumer_task_post_recv),
					doca_data{.u64 = transaction_idx});
		m_host_request_tasks.push_back(comch_consumer_task_post_recv);

		doca_comch_producer_task_send *comch_producer_task_send;
		ret = doca_comch_producer_task_send_alloc_init(m_producer,
							       io_response_message_buf,
							       nullptr,
							       0,
							       remote_consumer_id,
							       &comch_producer_task_send);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to allocate producer task"};
		}
		doca_task_set_user_data(doca_comch_producer_task_send_as_task(comch_producer_task_send),
					doca_data{.u64 = transaction_idx});
		transaction.host_response_task = comch_producer_task_send;
		m_host_response_tasks.push_back(comch_producer_task_send);
	}

	/* Storage IO requests */
	{
		/* Note: 2 IO messages of memory is allocated for storage requests, so each doca_buf to be used in
		 * storage request rdma send tasks is allocated so that it's data segment can be positioned to point to
		 * either of those 2 IO messages. */
		std::array<doca_buf *, 3> storage_io_message_bufs = {};

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_io_message_mmap,
							 transaction.storage_io_messages[0],
							 storage::size_of_io_message * 2,
							 std::addressof(storage_io_message_bufs[0]));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get storage request io message[0] doca_buf"};
		}
		m_io_message_bufs.push_back(storage_io_message_bufs[0]);

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_io_message_mmap,
							 transaction.storage_io_messages[0],
							 storage::size_of_io_message * 2,
							 std::addressof(storage_io_message_bufs[1]));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get storage request io message[1] doca_buf"};
		}
		m_io_message_bufs.push_back(storage_io_message_bufs[1]);

		ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
							 m_io_message_mmap,
							 transaction.storage_io_messages[0],
							 storage::size_of_io_message * 2,
							 std::addressof(storage_io_message_bufs[2]));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Unable to get storage request io message[2] doca_buf"};
		}
		m_io_message_bufs.push_back(storage_io_message_bufs[2]);

		alloc_rdma_send_task(transaction, transaction_idx, storage_io_message_bufs[0], connection_role::data_1);
		alloc_rdma_send_task(transaction, transaction_idx, storage_io_message_bufs[1], connection_role::data_2);
		alloc_rdma_send_task(transaction, transaction_idx, storage_io_message_bufs[2], connection_role::data_p);
	}

	/*
	 * Until the transaction runs we are not sure which buffer will be used with which GGA task, or what data
	 * addresses it would access. But we need a valid doca_buf to allocate tasks, so we allocate a temporary buffer
	 * here and release it before returning. Tasks will get the correct doca_bufs set at run-time
	 */
	doca_buf *tmp_buf;
	ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv, m_io_message_mmap, m_io_message_region, 1, &tmp_buf);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to get tmp io buf"};
	}

	std::unique_ptr<doca_buf, void (*)(doca_buf *)> const release_tmp_buff_on_fn_exit{
		tmp_buf,
		[](doca_buf *buf) {
			static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
		}};

	/* GGA tasks */
	{
		auto constexpr has_block_checksum = false;
		auto constexpr are_blocks_independent = true;

		ret = doca_compress_task_decompress_lz4_stream_alloc_init(m_compress,
									  has_block_checksum,
									  are_blocks_independent,
									  tmp_buf,
									  tmp_buf,
									  doca_data{.u64 = transaction_idx},
									  std::addressof(transaction.decompress_task));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate decompress task"};
		}

		ret = doca_ec_task_recover_allocate_init(m_ec,
							 m_ec_matrix,
							 tmp_buf,
							 tmp_buf,
							 doca_data{.u64 = transaction_idx},
							 std::addressof(transaction.ec_recover_task));
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate ec recover task"};
		}
	}
}

void gga_offload_app_worker::alloc_rdma_recv_task(uint8_t *addr, connection_role role)
{
	doca_error_t ret;
	doca_buf *buf = nullptr;

	ret = doca_buf_inventory_buf_get_by_addr(m_hot_data.io_buf_inv,
						 m_io_message_mmap,
						 addr,
						 storage::size_of_io_message,
						 &buf);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Unable to get rdma recv io message doca_buf"};
	}

	doca_rdma_task_receive *task = nullptr;
	ret = doca_rdma_task_receive_allocate_init(m_rdma[role].ctrl.rdma, buf, doca_data{.u64 = 0}, &task);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to allocate rdma doca_rdma_task_send"};
	}

	m_rdma[role].storage_response_tasks.push_back(task);
}

void gga_offload_app_worker::prepare_storage_io_response_tasks()
{
	/* Note: The responses IO messages are placed after the set of messages allocated per transaction. Each
	 * transaction gets 3 IO messages worth of data associated with it. Responses therefore start after that initial
	 * offset. */
	auto *addr = m_io_message_region + (m_hot_data.num_transactions * storage::size_of_io_message * 3);
	for (uint32_t ii = 0; ii != m_hot_data.num_transactions; ++ii) {
		alloc_rdma_recv_task(addr, connection_role::data_1);
		addr += storage::size_of_io_message;
		alloc_rdma_recv_task(addr, connection_role::data_2);
		addr += storage::size_of_io_message;
		alloc_rdma_recv_task(addr, connection_role::data_p);
		addr += storage::size_of_io_message;
	}
}

void gga_offload_app_worker::doca_comch_consumer_task_post_recv_cb(doca_comch_consumer_task_post_recv *task,
								   doca_data task_user_data,
								   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	char *io_message;
	static_cast<void>(doca_buf_get_data(doca_comch_consumer_task_post_recv_get_buf(task),
					    reinterpret_cast<void **>(&io_message)));

	auto const ret =
		static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr)->start_transaction(task, io_message);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start transaction: %s", doca_error_get_name(ret));
	}
}

void gga_offload_app_worker::doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
									 doca_data task_user_data,
									 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);

	if (hot_data->run_flag) {
		DOCA_LOG_ERR("Failed to complete doca_comch_consumer_task_post_recv");
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void gga_offload_app_worker::doca_comch_producer_task_send_cb(doca_comch_producer_task_send *task,
							      doca_data task_user_data,
							      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	auto &transaction = hot_data->transactions[task_user_data.u64];
	static_cast<void>(
		doca_buf_reset_data_len(doca_comch_consumer_task_post_recv_get_buf(transaction.host_request_task)));

	auto const ret = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(transaction.host_request_task));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_consumer_task_post_recv: %s", doca_error_get_name(ret));
		hot_data->error_flag = true;
		hot_data->run_flag = false;
	}
}

void gga_offload_app_worker::doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
								    doca_data task_user_data,
								    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_comch_producer_task_send");
	hot_data->run_flag = false;
	hot_data->error_flag = true;
}

void gga_offload_app_worker::doca_rdma_task_send_cb(doca_rdma_task_send *task,
						    doca_data task_user_data,
						    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);

	auto *hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	auto &transaction = hot_data->transactions[task_user_data.u64];

	--(transaction.remaining_op_count);
	if (transaction.remaining_op_count == 0) {
		hot_data->process_result(transaction);
	}
}

void gga_offload_app_worker::doca_rdma_task_send_error_cb(doca_rdma_task_send *task,
							  doca_data task_user_data,
							  doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_rdma_task_send");
	hot_data->run_flag = false;
	hot_data->error_flag = true;
}

void gga_offload_app_worker::doca_rdma_task_receive_cb(doca_rdma_task_receive *task,
						       doca_data task_user_data,
						       doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);

	auto *const rdma_io_message = storage::get_buffer_bytes(doca_rdma_task_receive_get_dst_buf(task));
	auto const cid = storage::io_message_view::get_correlation_id(rdma_io_message);

	auto &transaction = hot_data->transactions[cid];

	auto const io_result = storage::io_message_view::get_result(rdma_io_message);
	if (io_result != DOCA_SUCCESS) {
		// store error
		storage::io_message_view::set_result(io_result, transaction.initiator_io_message);
	}

	--(transaction.remaining_op_count);
	if (transaction.remaining_op_count == 0) {
		hot_data->process_result(transaction);
	}

	if (hot_data->run_flag) {
		static_cast<void>(doca_buf_reset_data_len(doca_rdma_task_receive_get_dst_buf(task)));
		auto const ret = doca_task_submit(doca_rdma_task_receive_as_task(task));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to resubmit doca_rdma_task_receive");
			hot_data->run_flag = false;
			hot_data->error_flag = true;
		}
	}
}

void gga_offload_app_worker::doca_rdma_task_receive_error_cb(doca_rdma_task_receive *task,
							     doca_data task_user_data,
							     doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
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

void gga_offload_app_worker::doca_ec_task_recover_cb(doca_ec_task_recover *task,
						     doca_data task_user_data,
						     doca_data ctx_user_data) noexcept
{
	auto *hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	auto const cid = task_user_data.u64;
	auto &transaction = hot_data->transactions[cid];

	doca_buf_dec_refcount(const_cast<doca_buf *>(doca_ec_task_recover_get_available_blocks(task)), nullptr);
	doca_buf_dec_refcount(doca_ec_task_recover_get_recovered_data(task), nullptr);

	--(transaction.remaining_op_count);
	hot_data->start_decompress(transaction);
}

void gga_offload_app_worker::doca_ec_task_recover_error_cb(doca_ec_task_recover *task,
							   doca_data task_user_data,
							   doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_ec_task_recover");
	hot_data->run_flag = false;
	hot_data->error_flag = true;

	doca_buf_dec_refcount(const_cast<doca_buf *>(doca_ec_task_recover_get_available_blocks(task)), nullptr);
	doca_buf_dec_refcount(doca_ec_task_recover_get_recovered_data(task), nullptr);
}

void gga_offload_app_worker::doca_compress_task_decompress_lz4_stream_cb(doca_compress_task_decompress_lz4_stream *task,
									 doca_data task_user_data,
									 doca_data ctx_user_data) noexcept
{
	auto *hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	auto &transaction = hot_data->transactions[task_user_data.u64];
	--(transaction.remaining_op_count);

	--(hot_data->in_flight_transaction_count);
	++(hot_data->completed_transaction_count);

	doca_buf_dec_refcount(const_cast<doca_buf *>(doca_compress_task_decompress_lz4_stream_get_src(task)), nullptr);
	doca_buf_dec_refcount(doca_compress_task_decompress_lz4_stream_get_dst(task), nullptr);

	doca_error_t ret;
	do {
		ret = doca_task_submit(doca_comch_producer_task_send_as_task(transaction.host_response_task));
	} while (ret == DOCA_ERROR_AGAIN);

	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_producer_task_send: %s", doca_error_get_name(ret));
		hot_data->run_flag = false;
		hot_data->error_flag = true;
	}
}

void gga_offload_app_worker::doca_compress_task_decompress_lz4_stream_error_cb(
	doca_compress_task_decompress_lz4_stream *task,
	doca_data task_user_data,
	doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const hot_data = static_cast<gga_offload_app_worker::hot_data *>(ctx_user_data.ptr);
	DOCA_LOG_ERR("Failed to complete doca_compress_task_decompress_lz4_stream");
	hot_data->run_flag = false;
	hot_data->error_flag = true;

	doca_buf_dec_refcount(const_cast<doca_buf *>(doca_compress_task_decompress_lz4_stream_get_src(task)), nullptr);
	doca_buf_dec_refcount(doca_compress_task_decompress_lz4_stream_get_dst(task), nullptr);
}

void gga_offload_app_worker::thread_proc()
{
	while (m_hot_data.run_flag == false) {
		std::this_thread::yield();
		if (m_hot_data.error_flag)
			return;
	}

	DOCA_LOG_INFO("Core: %u running", m_hot_data.core_idx);

	while (m_hot_data.run_flag) {
		doca_pe_progress(m_hot_data.pe) ? ++(m_hot_data.pe_hit_count) : ++(m_hot_data.pe_miss_count);
	}

	while (m_hot_data.error_flag == false && m_hot_data.in_flight_transaction_count != 0) {
		doca_pe_progress(m_hot_data.pe) ? ++(m_hot_data.pe_hit_count) : ++(m_hot_data.pe_miss_count);
	}

	DOCA_LOG_INFO("Core: %u complete", m_hot_data.core_idx);
}

gga_offload_app::~gga_offload_app()
{
	destroy_workers();
	for (auto &channel : m_all_ctrl_channels) {
		channel.reset();
	}

	doca_error_t ret;
	if (m_dev != nullptr) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {}
	}
}

gga_offload_app::gga_offload_app(gga_offload_app_configuration const &cfg)
	: m_cfg{cfg},
	  m_dev{nullptr},
	  m_dev_rep{nullptr},
	  m_remote_io_mmap{nullptr},
	  m_local_io_region{nullptr},
	  m_local_io_mmap{nullptr},
	  m_all_ctrl_channels{},
	  m_storage_ctrl_channels{},
	  m_ctrl_messages{},
	  m_remote_consumer_ids{},
	  m_workers{nullptr},
	  m_stats{},
	  m_storage_capacity{},
	  m_storage_block_size{},
	  m_message_id_counter{},
	  m_num_transactions{0},
	  m_core_count{0},
	  m_abort_flag{false}
{
	DOCA_LOG_INFO("Open doca_dev: %s", m_cfg.device_id.c_str());
	m_dev = storage::open_device(m_cfg.device_id);

	DOCA_LOG_INFO("Open doca_dev_rep: %s", m_cfg.representor_id.c_str());
	m_dev_rep = storage::open_representor(m_dev, m_cfg.representor_id);

	m_all_ctrl_channels[connection_role::data_1] = storage::control::make_tcp_client_control_channel(
		m_cfg.storage_server_address[connection_role::data_1]);
	m_storage_ctrl_channels[connection_role::data_1] = m_all_ctrl_channels[connection_role::data_1].get();

	m_all_ctrl_channels[connection_role::data_2] = storage::control::make_tcp_client_control_channel(
		m_cfg.storage_server_address[connection_role::data_2]);
	m_storage_ctrl_channels[connection_role::data_2] = m_all_ctrl_channels[connection_role::data_2].get();

	m_all_ctrl_channels[connection_role::data_p] = storage::control::make_tcp_client_control_channel(
		m_cfg.storage_server_address[connection_role::data_p]);
	m_storage_ctrl_channels[connection_role::data_p] = m_all_ctrl_channels[connection_role::data_p].get();

	m_all_ctrl_channels[connection_role::client] =
		storage::control::make_comch_server_control_channel(m_dev,
								    m_dev_rep,
								    m_cfg.command_channel_name.c_str(),
								    this,
								    new_comch_consumer_callback,
								    expired_comch_consumer_callback);
}

void gga_offload_app::abort(std::string const &reason)
{
	if (m_abort_flag)
		return;

	DOCA_LOG_ERR("Aborted: %s", reason.c_str());
	m_abort_flag = true;
}

void gga_offload_app::connect_to_storage(void)
{
	for (auto *storage_channel : m_storage_ctrl_channels) {
		DOCA_LOG_DBG("Connect control channel...");
		for (;;) {
			if (m_abort_flag) {
				throw storage::runtime_error{DOCA_ERROR_CONNECTION_ABORTED,
							     "Aborted while connecting to storage"};
			}

			if (storage_channel->is_connected())
				break;
		}
	}
}

void gga_offload_app::wait_for_comch_client_connection(void)
{
	while (!m_all_ctrl_channels[connection_role::client]->is_connected()) {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		if (m_abort_flag) {
			throw storage::runtime_error{DOCA_ERROR_CONNECTION_ABORTED,
						     "Aborted while connecting to client"};
		}
	}
}

void gga_offload_app::wait_for_and_process_query_storage(void)
{
	DOCA_LOG_INFO("Wait for query storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::query_storage_request) {
		try {
			m_all_ctrl_channels[connection_role::client]->send_message(
				process_query_storage(client_request));
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

	m_all_ctrl_channels[connection_role::client]->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void gga_offload_app::wait_for_and_process_init_storage(void)
{
	DOCA_LOG_INFO("Wait for init storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::init_storage_request) {
		try {
			m_all_ctrl_channels[connection_role::client]->send_message(
				process_init_storage(client_request));
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

	m_all_ctrl_channels[connection_role::client]->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void gga_offload_app::wait_for_and_process_start_storage(void)
{
	DOCA_LOG_INFO("Wait for start storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::start_storage_request) {
		try {
			m_all_ctrl_channels[connection_role::client]->send_message(
				process_start_storage(client_request));
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

	m_all_ctrl_channels[connection_role::client]->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void gga_offload_app::wait_for_and_process_stop_storage(void)
{
	DOCA_LOG_INFO("Wait for stop storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::stop_storage_request) {
		try {
			m_all_ctrl_channels[connection_role::client]->send_message(
				process_stop_storage(client_request));
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

	m_all_ctrl_channels[connection_role::client]->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void gga_offload_app::wait_for_and_process_shutdown(void)
{
	DOCA_LOG_INFO("Wait for shutdown storage...");
	auto const client_request = wait_for_control_message();

	doca_error_t err_code;
	std::string err_msg;

	if (client_request.message_type == storage::control::message_type::shutdown_request) {
		try {
			m_all_ctrl_channels[connection_role::client]->send_message(process_shutdown(client_request));
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

	m_all_ctrl_channels[connection_role::client]->send_message({
		storage::control::message_type::error_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::error_response_payload>(err_code, std::move(err_msg)),

	});
}

void gga_offload_app::display_stats(void) const
{
	for (auto const &stats : m_stats) {
		auto const pe_hit_rate_pct =
			(static_cast<double>(stats.pe_hit_count) /
			 (static_cast<double>(stats.pe_hit_count) + static_cast<double>(stats.pe_miss_count))) *
			100.;

		printf("+================================================+\n");
		printf("| Core: %u\n", stats.core_idx);
		printf("| Operation count: %lu\n", stats.operation_count);
		printf("| Recovery count: %lu\n", stats.recovery_count);
		printf("| PE hit rate: %2.03lf%% (%lu:%lu)\n", pe_hit_rate_pct, stats.pe_hit_count, stats.pe_miss_count);
	}
}

void gga_offload_app::new_comch_consumer_callback(void *user_data, uint32_t id) noexcept
{
	auto *self = reinterpret_cast<gga_offload_app *>(user_data);
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

void gga_offload_app::expired_comch_consumer_callback(void *user_data, uint32_t id) noexcept
{
	auto *self = reinterpret_cast<gga_offload_app *>(user_data);
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

storage::control::message gga_offload_app::wait_for_control_message()
{
	for (;;) {
		if (!m_ctrl_messages.empty()) {
			auto msg = std::move(m_ctrl_messages.front());
			m_ctrl_messages.erase(m_ctrl_messages.begin());
			return msg;
		}

		for (auto &channel : m_all_ctrl_channels) {
			// Poll for new messages
			auto *msg = channel->poll();
			if (msg) {
				m_ctrl_messages.push_back(std::move(*msg));
			}
		}

		if (m_abort_flag) {
			throw storage::runtime_error{
				DOCA_ERROR_CONNECTION_RESET,
				"User aborted the gga_offload_application while waiting on a control message"};
		}
	}
}

void gga_offload_app::wait_for_responses(std::vector<storage::control::message_id> const &mids,
					 std::chrono::seconds timeout)
{
	auto const expiry = std::chrono::steady_clock::now() + timeout;
	uint32_t match_count = 0;
	do {
		if (m_abort_flag) {
			throw storage::runtime_error{
				DOCA_ERROR_CONNECTION_RESET,
				"User aborted the gga_offload_application while waiting on a control message"};
		}

		for (auto &channel : m_all_ctrl_channels) {
			// Poll for new messages
			auto *msg = channel->poll();
			if (msg) {
				m_ctrl_messages.push_back(std::move(*msg));
			}
		}

		match_count = 0;
		for (auto mid : mids) {
			for (auto const &msg : m_ctrl_messages) {
				if (msg.message_id.value == mid.value) {
					++match_count;
					break;
				}
			}
		}

		if (expiry < std::chrono::steady_clock::now()) {
			std::stringstream ss;
			ss << "Timed out while waiting on a control messages[";
			for (auto &id : mids) {
				ss << id.value << " ";
			}
			ss << "] had available messages:[";
			for (auto &msg : m_ctrl_messages) {
				ss << msg.message_id.value << " ";
			}
			ss << "]";

			throw storage::runtime_error{
				DOCA_ERROR_TIME_OUT,
				ss.str(),
			};
		}
	} while (match_count != mids.size());
}

storage::control::message gga_offload_app::get_response(storage::control::message_id mid)
{
	auto found = std::find_if(std::begin(m_ctrl_messages), std::end(m_ctrl_messages), [mid](auto const &msg) {
		return msg.message_id.value == mid.value;
	});

	if (found != std::end(m_ctrl_messages)) {
		auto msg = std::move(*found);
		m_ctrl_messages.erase(found);
		return msg;
	}

	throw storage::runtime_error{DOCA_ERROR_BAD_STATE, "[BUG] Failed to get response from store"};
}

void gga_offload_app::discard_responses(std::vector<storage::control::message_id> const &mids)
{
	m_ctrl_messages.erase(std::remove_if(std::begin(m_ctrl_messages),
					     std::end(m_ctrl_messages),
					     [&mids](auto const &msg) {
						     return std::find(std::begin(mids),
								      std::end(mids),
								      msg.message_id) != std::end(mids);
					     }),
			      std::end(m_ctrl_messages));
}

storage::control::message gga_offload_app::process_query_storage(storage::control::message const &client_request)
{
	DOCA_LOG_DBG("Forward request to storage...");
	std::vector<storage::control::message_id> msg_ids;

	for (auto *storage_ctrl : m_storage_ctrl_channels) {
		auto storage_request = storage::control::message{
			storage::control::message_type::query_storage_request,
			storage::control::message_id{m_message_id_counter++},
			client_request.correlation_id,
			{},
		};

		msg_ids.push_back(storage_request.message_id);
		storage_ctrl->send_message(storage_request);
	}

	wait_for_responses(msg_ids, default_control_timeout_seconds);
	for (auto &id : msg_ids) {
		auto response = get_response(id);

		if (response.message_type != storage::control::message_type::query_storage_response) {
			discard_responses(msg_ids);
			return make_error_response(client_request.message_id,
						   client_request.correlation_id,
						   std::move(response),
						   storage::control::message_type::query_storage_response);
		}

		auto const *const storage_details =
			dynamic_cast<storage::control::storage_details_payload const *>(response.payload.get());
		if (storage_details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_UNEXPECTED, "[BUG] invalid query_storage_response"};
		}

		DOCA_LOG_INFO("Storage reports capacity of: %lu using a block size of: %u",
			      storage_details->total_size,
			      storage_details->block_size);
		if (m_storage_capacity == 0) {
			m_storage_capacity = storage_details->total_size;
			m_storage_block_size = storage_details->block_size;
		} else {
			if (m_storage_capacity != storage_details->total_size) {
				return storage::control::message{
					storage::control::message_type::error_response,
					client_request.message_id,
					client_request.correlation_id,
					std::make_unique<storage::control::error_response_payload>(
						DOCA_ERROR_BAD_STATE,
						"Mismatch in storage capacity: " + std::to_string(m_storage_capacity) +
							" vs " + std::to_string(storage_details->total_size)),
				};
			} else if (m_storage_block_size != storage_details->block_size) {
				return storage::control::message{
					storage::control::message_type::error_response,
					client_request.message_id,
					client_request.correlation_id,
					std::make_unique<storage::control::error_response_payload>(
						DOCA_ERROR_BAD_STATE,
						"Mismatch in block_size: " + std::to_string(m_storage_block_size) +
							" vs " + std::to_string(storage_details->block_size)),
				};
			}
		}
	}

	return storage::control::message{
		storage::control::message_type::query_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		std::make_unique<storage::control::storage_details_payload>(m_storage_capacity, m_storage_block_size),
	};
}

storage::control::message gga_offload_app::process_init_storage(storage::control::message const &client_request)
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

	m_num_transactions = init_storage_details->task_count * 2;
	m_core_count = init_storage_details->core_count;

	/* over allocate DPU storage to make space for EC data chunks */
	auto const local_storage_size =
		m_core_count * m_num_transactions * (m_storage_block_size + m_storage_block_size / 2);
	m_local_io_region =
		static_cast<uint8_t *>(storage::aligned_alloc(storage::get_system_page_size(), local_storage_size));
	if (m_local_io_region == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate local memory region"};
	}

	m_local_io_mmap = storage::make_mmap(m_dev,
					     reinterpret_cast<char *>(m_local_io_region),
					     local_storage_size,
					     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE |
						     DOCA_ACCESS_FLAG_RDMA_WRITE | DOCA_ACCESS_FLAG_RDMA_READ);

	m_remote_io_mmap = storage::make_mmap(m_dev,
					      init_storage_details->mmap_export_blob.data(),
					      init_storage_details->mmap_export_blob.size());
	std::vector<uint8_t> mmap_export_blob = [this]() {
		uint8_t const *reexport_blob = nullptr;
		size_t reexport_blob_size = 0;
		auto const ret = doca_mmap_export_rdma(m_local_io_mmap,
						       m_dev,
						       reinterpret_cast<void const **>(&reexport_blob),
						       &reexport_blob_size);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to re-export host mmap for rdma"};
		}

		return std::vector<uint8_t>{reexport_blob, reexport_blob + reexport_blob_size};
	}();

	DOCA_LOG_INFO("Configured storage: %u cores, %u transactions", m_core_count, m_num_transactions);

	DOCA_LOG_DBG("Forward request to storage...");
	std::vector<storage::control::message_id> msg_ids;

	for (auto *storage_ctrl : m_storage_ctrl_channels) {
		auto storage_request = storage::control::message{
			storage::control::message_type::init_storage_request,
			storage::control::message_id{m_message_id_counter++},
			client_request.correlation_id,
			std::make_unique<storage::control::init_storage_payload>(init_storage_details->task_count,
										 init_storage_details->core_count,
										 mmap_export_blob),
		};

		msg_ids.push_back(storage_request.message_id);
		storage_ctrl->send_message(storage_request);
	}

	wait_for_responses(msg_ids, default_control_timeout_seconds);
	for (auto &id : msg_ids) {
		auto response = get_response(id);

		if (response.message_type != storage::control::message_type::init_storage_response) {
			discard_responses(msg_ids);
			return make_error_response(client_request.message_id,
						   client_request.correlation_id,
						   std::move(response),
						   storage::control::message_type::init_storage_response);
		}
	}

	DOCA_LOG_DBG("prepare thread contexts...");
	prepare_thread_contexts(client_request.correlation_id);

	return storage::control::message{
		storage::control::message_type::init_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

storage::control::message gga_offload_app::process_start_storage(storage::control::message const &client_request)
{
	DOCA_LOG_DBG("Forward request to storage...");
	std::vector<storage::control::message_id> msg_ids;

	for (auto *storage_ctrl : m_storage_ctrl_channels) {
		auto storage_request = storage::control::message{
			storage::control::message_type::start_storage_request,
			storage::control::message_id{m_message_id_counter++},
			client_request.correlation_id,
			{},
		};

		msg_ids.push_back(storage_request.message_id);
		storage_ctrl->send_message(storage_request);
	}

	wait_for_responses(msg_ids, default_control_timeout_seconds);
	for (auto &id : msg_ids) {
		auto response = get_response(id);

		if (response.message_type != storage::control::message_type::start_storage_response) {
			discard_responses(msg_ids);
			return make_error_response(client_request.message_id,
						   client_request.correlation_id,
						   std::move(response),
						   storage::control::message_type::start_storage_response);
		}
	}

	verify_connections_are_ready();
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		m_workers[ii].prepare_data_path(ii * m_num_transactions, m_remote_consumer_ids[ii]);
		m_workers[ii].start_thread_proc();
	}

	return storage::control::message{
		storage::control::message_type::start_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

storage::control::message gga_offload_app::process_stop_storage(storage::control::message const &client_request)
{
	DOCA_LOG_DBG("Forward request to storage...");
	std::vector<storage::control::message_id> msg_ids;

	for (auto *storage_ctrl : m_storage_ctrl_channels) {
		auto storage_request = storage::control::message{
			storage::control::message_type::stop_storage_request,
			storage::control::message_id{m_message_id_counter++},
			client_request.correlation_id,
			{},
		};

		msg_ids.push_back(storage_request.message_id);
		storage_ctrl->send_message(storage_request);
	}

	wait_for_responses(msg_ids, default_control_timeout_seconds);
	for (auto &id : msg_ids) {
		auto response = get_response(id);

		if (response.message_type != storage::control::message_type::stop_storage_response) {
			discard_responses(msg_ids);
			return make_error_response(client_request.message_id,
						   client_request.correlation_id,
						   std::move(response),
						   storage::control::message_type::stop_storage_response);
		}
	}

	/* Stop all processing */
	m_stats.reserve(m_core_count);
	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		m_workers[ii].stop_processing();
		auto const &hot_data = m_workers[ii].get_hot_data();
		m_stats.push_back(thread_stats{
			m_cfg.cpu_set[ii],
			hot_data.pe_hit_count,
			hot_data.pe_miss_count,
			hot_data.completed_transaction_count,
			hot_data.recovery_flow_count,
		});
		m_workers[ii].destroy_comch_objects();
	}

	return storage::control::message{
		storage::control::message_type::stop_storage_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

storage::control::message gga_offload_app::process_shutdown(storage::control::message const &client_request)
{
	/* Wait for all remote comch objects to be destroyed and notified */
	while (!m_remote_consumer_ids.empty()) {
		auto *msg = m_all_ctrl_channels[connection_role::client]->poll();
		DOCA_LOG_DBG("Ignoring unexpected %s while processing %s",
			     to_string(msg->message_type).c_str(),
			     to_string(storage::control::message_type::shutdown_request).c_str());
	}

	DOCA_LOG_DBG("Forward request to storage...");
	std::vector<storage::control::message_id> msg_ids;

	for (auto *storage_ctrl : m_storage_ctrl_channels) {
		auto storage_request = storage::control::message{
			storage::control::message_type::shutdown_request,
			storage::control::message_id{m_message_id_counter++},
			client_request.correlation_id,
			{},
		};

		msg_ids.push_back(storage_request.message_id);
		storage_ctrl->send_message(storage_request);
	}

	wait_for_responses(msg_ids, default_control_timeout_seconds);
	for (auto &id : msg_ids) {
		auto response = get_response(id);

		if (response.message_type != storage::control::message_type::shutdown_response) {
			discard_responses(msg_ids);
			return make_error_response(client_request.message_id,
						   client_request.correlation_id,
						   std::move(response),
						   storage::control::message_type::shutdown_response);
		}
	}

	destroy_workers();
	return storage::control::message{
		storage::control::message_type::shutdown_response,
		client_request.message_id,
		client_request.correlation_id,
		{},
	};
}

void gga_offload_app::prepare_thread_contexts(storage::control::correlation_id cid)
{
	auto const *comch_channel =
		dynamic_cast<storage::control::comch_channel *>(m_all_ctrl_channels[connection_role::client].get());
	if (comch_channel == nullptr) {
		throw storage::runtime_error{DOCA_ERROR_UNEXPECTED, "[BUG] invalid control channel"};
	}

	m_workers = storage::make_aligned<gga_offload_app_worker>{}.object_array(m_core_count);

	for (uint32_t ii = 0; ii != m_core_count; ++ii) {
		m_workers[ii].init(m_dev,
				   comch_channel->get_comch_connection(),
				   m_local_io_mmap,
				   m_remote_io_mmap,
				   m_num_transactions,
				   m_storage_block_size,
				   m_cfg.ec_matrix_type,
				   m_cfg.recover_freq);
		connect_rdma(ii, storage::control::rdma_connection_role::io_data, cid);
		connect_rdma(ii, storage::control::rdma_connection_role::io_control, cid);
		m_workers[ii].prepare_thread_proc(m_cfg.cpu_set[ii]);
	}
}

void gga_offload_app::connect_rdma(uint32_t thread_idx,
				   storage::control::rdma_connection_role role,
				   storage::control::correlation_id cid)
{
	std::vector<storage::control::message_id> msg_ids;
	auto &tctx = m_workers[thread_idx];
	{
		auto storage_request = storage::control::message{
			storage::control::message_type::create_rdma_connection_request,
			storage::control::message_id{m_message_id_counter++},
			cid,
			std::make_unique<storage::control::rdma_connection_details_payload>(
				thread_idx,
				role,
				tctx.get_local_rdma_connection_blob(connection_role::data_1, role)),
		};

		msg_ids.push_back(storage_request.message_id);
		m_storage_ctrl_channels[connection_role::data_1]->send_message(storage_request);
	}
	{
		auto storage_request = storage::control::message{
			storage::control::message_type::create_rdma_connection_request,
			storage::control::message_id{m_message_id_counter++},
			cid,
			std::make_unique<storage::control::rdma_connection_details_payload>(
				thread_idx,
				role,
				tctx.get_local_rdma_connection_blob(connection_role::data_2, role)),
		};

		msg_ids.push_back(storage_request.message_id);
		m_storage_ctrl_channels[connection_role::data_2]->send_message(storage_request);
	}
	{
		auto storage_request = storage::control::message{
			storage::control::message_type::create_rdma_connection_request,
			storage::control::message_id{m_message_id_counter++},
			cid,
			std::make_unique<storage::control::rdma_connection_details_payload>(
				thread_idx,
				role,
				tctx.get_local_rdma_connection_blob(connection_role::data_p, role)),
		};

		msg_ids.push_back(storage_request.message_id);
		m_storage_ctrl_channels[connection_role::data_p]->send_message(storage_request);
	}

	wait_for_responses(msg_ids, default_control_timeout_seconds);
	auto response_role = connection_role::data_1;

	for (auto &id : msg_ids) {
		auto response = get_response(id);

		if (response.message_type != storage::control::message_type::create_rdma_connection_response) {
			discard_responses(msg_ids);
			if (response.message_type == storage::control::message_type::error_response) {
				auto *error_details =
					reinterpret_cast<storage::control::error_response_payload const *>(
						response.payload.get());
				throw storage::runtime_error{error_details->error_code, error_details->message};
			} else {
				throw storage::runtime_error{
					DOCA_ERROR_UNEXPECTED,
					"Unexpected " + to_string(response.message_type) + " while expecting a " +
						to_string(
							storage::control::message_type::create_rdma_connection_response),
				};
			}
		}

		auto *remote_details = reinterpret_cast<storage::control::rdma_connection_details_payload const *>(
			response.payload.get());
		tctx.connect_rdma(response_role, role, remote_details->connection_details);
		response_role = static_cast<connection_role>(static_cast<uint8_t>(response_role) + 1);
	}
}

void gga_offload_app::verify_connections_are_ready(void)
{
	uint32_t not_ready_count;

	do {
		not_ready_count = 0;
		if (m_remote_consumer_ids.size() != m_core_count) {
			++not_ready_count;
			auto *msg = m_all_ctrl_channels[connection_role::client]->poll();
			if (msg != nullptr) {
				throw storage::runtime_error{
					DOCA_ERROR_UNEXPECTED,
					"Unexpected " + to_string(msg->message_type) + " while processing " +
						to_string(storage::control::message_type::start_storage_request),
				};
			}
		}

		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			auto const ret = m_workers[ii].get_connections_state();
			if (ret == DOCA_ERROR_IN_PROGRESS) {
				++not_ready_count;
			} else if (ret != DOCA_SUCCESS) {
				throw storage::runtime_error{ret, "Failure while establishing RDMA connections"};
			}
		}

		if (m_abort_flag) {
			throw storage::runtime_error{DOCA_ERROR_INITIALIZATION,
						     "Aborted while establishing storage connections"};
		}
	} while (not_ready_count != 0);
}

void gga_offload_app::destroy_workers(void) noexcept
{
	if (m_workers != nullptr) {
		// Destroy all thread resources
		for (uint32_t ii = 0; ii != m_core_count; ++ii) {
			m_workers[ii].~gga_offload_app_worker();
		}
		storage::aligned_free(m_workers);
		m_workers = nullptr;
	}
}
} /* namespace */
