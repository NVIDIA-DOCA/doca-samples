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
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <doca_argp.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_version.h>

#include <storage_common/binary_content.hpp>
#include <storage_common/buffer_utils.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/doca_utils.hpp>
#include <storage_common/file_utils.hpp>
#include <storage_common/lz4_sw_context.hpp>
#include <storage_common/os_utils.hpp>

DOCA_LOG_REGISTER(SBC_GEN);

using namespace std::string_literals;

namespace {
auto constexpr app_name = "doca_storage_gga_offload_sbc_generator";
auto constexpr padding_byte = uint8_t{0};
auto constexpr num_ec_tasks = uint32_t{1};
auto constexpr num_ec_buffers = uint32_t{2} * num_ec_tasks;

struct gga_offload_sbc_gen_configuration {
	std::string device_id;
	std::string original_data_file_name;
	std::string data_1_file_name;
	std::string data_2_file_name;
	std::string data_p_file_name;
	std::string ec_matrix_type;
	uint32_t block_size;
};

struct gga_offload_sbc_gen_result {
	uint32_t block_count;
	std::vector<uint8_t> data_1_content;
	std::vector<uint8_t> data_2_content;
	std::vector<uint8_t> data_p_content;
};

class gga_offload_sbc_gen_app {
public:
	~gga_offload_sbc_gen_app();

	gga_offload_sbc_gen_app() = delete;

	gga_offload_sbc_gen_app(std::string const &device_id, std::string const &ec_matrix_type, uint32_t block_size);

	gga_offload_sbc_gen_app(gga_offload_sbc_gen_app const &) = delete;

	gga_offload_sbc_gen_app(gga_offload_sbc_gen_app &&) noexcept = delete;

	gga_offload_sbc_gen_app &operator=(gga_offload_sbc_gen_app const &) = delete;

	gga_offload_sbc_gen_app &operator=(gga_offload_sbc_gen_app &&) noexcept = delete;

	gga_offload_sbc_gen_result generate_binary_content(std::vector<uint8_t> input_data);

private:
	storage::lz4_sw_context m_lz4_ctx;
	doca_dev *m_dev;
	std::vector<uint8_t> m_compressed_bytes_buffer;
	std::vector<uint8_t> m_gga_output_buffer_bytes;
	doca_mmap *m_input_mmap;
	doca_mmap *m_output_mmap;
	doca_buf_inventory *m_buf_inv;
	doca_buf *m_input_buf;
	doca_buf *m_output_buf;
	doca_pe *m_pe;
	doca_ec *m_ec;
	doca_ec_matrix *m_ec_matrix;
	doca_ec_task_create *m_ec_task;
	uint32_t m_block_size;
	bool m_error_flag;
};

/*
 * Print the parsed configuration
 *
 * @cfg [in]: Configuration to display
 */
void print_config(gga_offload_sbc_gen_configuration const &cfg) noexcept;

/*
 * Parse command line arguments
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: Parsed configuration
 *
 * @throws: std::runtime_error If the configuration cannot pe parsed or contains invalid values
 */
gga_offload_sbc_gen_configuration parse_cli_args(int argc, char **argv);

/*
 * Pad the content of input to be a multiple of block_size length
 *
 * @input [in]: Vector to pad
 * @block_size [in]: Block size
 */
void pad_input_to_multiple_of_block_size(std::vector<uint8_t> &input, uint32_t block_size);
} /* namespace */

/*
 * Main
 *
 * @argc [in]: Number of arguments
 * @argv [in]: Array of argument values
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	int rc = EXIT_SUCCESS;
	storage::create_doca_logger_backend();
	printf("%s: v%s\n", app_name, doca_version());

	try {
		auto const cfg = parse_cli_args(argc, argv);
		print_config(cfg);

		gga_offload_sbc_gen_app app{cfg.device_id, cfg.ec_matrix_type, cfg.block_size};

		auto input_data = storage::load_file_bytes(cfg.original_data_file_name);
		pad_input_to_multiple_of_block_size(input_data, cfg.block_size);

		printf("Processing data...");
		auto const results = app.generate_binary_content(input_data);

		printf("Output info:\n");
		printf("\tBlock size: %u\n", cfg.block_size);
		printf("\tOut block count: %u\n", results.block_count);

		auto const half_block_size = cfg.block_size / 2;
		storage::write_binary_content_to_file(cfg.data_1_file_name,
						      storage::binary_content{half_block_size,
									      results.block_count,
									      std::move(results.data_1_content)});
		printf("\tData 1(%s) created successfully\n", cfg.data_1_file_name.c_str());
		storage::write_binary_content_to_file(cfg.data_2_file_name,
						      storage::binary_content{half_block_size,
									      results.block_count,
									      std::move(results.data_2_content)});
		printf("\tData 2(%s) created successfully\n", cfg.data_2_file_name.c_str());
		storage::write_binary_content_to_file(cfg.data_p_file_name,
						      storage::binary_content{half_block_size,
									      results.block_count,
									      std::move(results.data_p_content)});
		printf("\tData p(%s) created successfully\n", cfg.data_p_file_name.c_str());
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("EXCEPTION: %s\n", ex.what());

		rc = EXIT_FAILURE;
	}

	return rc;
}

namespace {
void print_config(gga_offload_sbc_gen_configuration const &cfg) noexcept
{
	printf("configuration: {\n");
	printf("\tdevice : \"%s\",\n", cfg.device_id.c_str());
	printf("\toriginal_data_file : \"%s\",\n", cfg.original_data_file_name.c_str());
	printf("\tblock_size : %u,\n", cfg.block_size);
	printf("\tec_matrix_type : \"%s\",\n", cfg.ec_matrix_type.c_str());
	printf("\tdata_1_file : \"%s\",\n", cfg.data_1_file_name.c_str());
	printf("\tdata_2_file : \"%s\",\n", cfg.data_2_file_name.c_str());
	printf("\tdata_p_file : \"%s\",\n", cfg.data_p_file_name.c_str());
	printf("}\n");
}

gga_offload_sbc_gen_configuration parse_cli_args(int argc, char **argv)
{
	doca_error_t ret;
	gga_offload_sbc_gen_configuration config{};
	config.block_size = 4096;
	config.ec_matrix_type = "vandermonde";

	ret = doca_argp_init(app_name, &config);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args: "s + doca_error_get_name(ret)};
	}

	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       "d",
				       "device",
				       "Device identifier",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_sbc_gen_configuration *>(cfg)->device_id =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(
		DOCA_ARGP_TYPE_STRING,
		nullptr,
		"original-input-data",
		"File containing the original data that is represented by the storage",
		storage::required_value,
		storage::single_value,
		[](void *value, void *cfg) noexcept {
			static_cast<gga_offload_sbc_gen_configuration *>(cfg)->original_data_file_name =
				static_cast<char const *>(value);
			return DOCA_SUCCESS;
		});
	storage::register_cli_argument(DOCA_ARGP_TYPE_INT,
				       nullptr,
				       "block-size",
				       "Size of each block. Default: 4096",
				       storage::optional_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_sbc_gen_configuration *>(cfg)->block_size =
						       *static_cast<int *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "matrix-type",
				       "Type of matrix to use. One of: cauchy, vandermonde Default: vandermonde",
				       storage::optional_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_sbc_gen_configuration *>(cfg)->ec_matrix_type =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "data-1",
				       "First half of the data in storage",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_sbc_gen_configuration *>(cfg)->data_1_file_name =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "data-2",
				       "Second half of the data in storage",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_sbc_gen_configuration *>(cfg)->data_2_file_name =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });
	storage::register_cli_argument(DOCA_ARGP_TYPE_STRING,
				       nullptr,
				       "data-p",
				       "Parity data (used to perform recovery flow)",
				       storage::required_value,
				       storage::single_value,
				       [](void *value, void *cfg) noexcept {
					       static_cast<gga_offload_sbc_gen_configuration *>(cfg)->data_p_file_name =
						       static_cast<char const *>(value);
					       return DOCA_SUCCESS;
				       });

	ret = doca_argp_start(argc, argv);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to parse CLI args: "s + doca_error_get_name(ret)};
	}

	static_cast<void>(doca_argp_destroy());

	if (config.block_size % 64 != 0) {
		// doca_ec requires buffers to be a multiple of 64 bytes of data
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE, "Block size must be a multiple of 64"};
	}
	return config;
}

void pad_input_to_multiple_of_block_size(std::vector<uint8_t> &input, uint32_t block_size)
{
	auto const padding_size = input.size() % block_size;
	if (padding_size != 0) {
		auto const block_count = 1 + (input.size() / block_size);
		input.resize(block_count * block_size, padding_byte);
	}
}

gga_offload_sbc_gen_app::~gga_offload_sbc_gen_app()
{
	doca_error_t ret;

	if (m_ec_task) {
		doca_task_free(doca_ec_task_create_as_task(m_ec_task));
		m_ec_task = nullptr;
	}

	if (m_ec_matrix) {
		ret = doca_ec_matrix_destroy(m_ec_matrix);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy ec matrix: %s\n", doca_error_get_name(ret));
		}
		m_ec_matrix = nullptr;
	}

	if (m_ec) {
		ret = doca_ctx_stop(doca_ec_as_ctx(m_ec));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop ec context: %s\n", doca_error_get_name(ret));
		}

		ret = doca_ec_destroy(m_ec);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy ec context: %s\n", doca_error_get_name(ret));
		}

		m_ec = nullptr;
	}

	if (m_pe) {
		ret = doca_pe_destroy(m_pe);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine: %s\n", doca_error_get_name(ret));
		}

		m_pe = nullptr;
	}

	if (m_input_buf) {
		ret = doca_buf_dec_refcount(m_input_buf, nullptr);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to release doca buf: %s\n", doca_error_get_name(ret));
		}
		m_input_buf = nullptr;
	}

	if (m_output_buf) {
		ret = doca_buf_dec_refcount(m_output_buf, nullptr);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to release doca buf: %s\n", doca_error_get_name(ret));
		}
		m_output_buf = nullptr;
	}

	if (m_buf_inv) {
		ret = doca_buf_inventory_destroy(m_buf_inv);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy buf inventory: %s\n", doca_error_get_name(ret));
		}

		m_buf_inv = nullptr;
	}

	if (m_input_mmap) {
		ret = doca_mmap_stop(m_input_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop input mmap: %s\n", doca_error_get_name(ret));
		}

		ret = doca_mmap_destroy(m_input_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy input mmap: %s\n", doca_error_get_name(ret));
		}

		m_input_mmap = nullptr;
	}

	if (m_output_mmap) {
		ret = doca_mmap_stop(m_output_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop output mmap: %s", doca_error_get_name(ret));
		}

		ret = doca_mmap_destroy(m_output_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy output mmap: %s\n", doca_error_get_name(ret));
		}

		m_output_mmap = nullptr;
	}

	if (m_dev) {
		ret = doca_dev_close(m_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device: %s\n", doca_error_get_name(ret));
		}

		m_dev = nullptr;
	}
}

gga_offload_sbc_gen_app::gga_offload_sbc_gen_app(std::string const &device_id,
						 std::string const &ec_matrix_type,
						 uint32_t block_size)
	: m_lz4_ctx{},
	  m_dev{nullptr},
	  m_compressed_bytes_buffer{},
	  m_gga_output_buffer_bytes{},
	  m_input_mmap{nullptr},
	  m_output_mmap{nullptr},
	  m_buf_inv{nullptr},
	  m_input_buf{nullptr},
	  m_output_buf{nullptr},
	  m_pe{nullptr},
	  m_ec{nullptr},
	  m_ec_matrix{nullptr},
	  m_ec_task{nullptr},
	  m_block_size{block_size},
	  m_error_flag{false}
{
	doca_error_t ret;

	DOCA_LOG_INFO("Open doca_dev: %s", device_id.c_str());
	m_dev = storage::open_device(device_id);

	ret = doca_pe_create(&m_pe);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_pe"};
	}

	ret = doca_ec_cap_task_create_is_supported(doca_dev_as_devinfo(m_dev));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Selected device does not support doca_ec"};
	}

	ret = doca_ec_create(m_dev, &m_ec);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_ec"};
	}

	ret = doca_ec_task_create_set_conf(
		m_ec,
		[](doca_ec_task_create *task, doca_data task_user_data, doca_data ctx_user_data) {
			static_cast<void>(task);
			static_cast<void>(task_user_data);
			static_cast<void>(ctx_user_data);
		},
		[](doca_ec_task_create *task, doca_data task_user_data, doca_data ctx_user_data) {
			static_cast<void>(task);
			static_cast<void>(task_user_data);
			reinterpret_cast<gga_offload_sbc_gen_app *>(ctx_user_data.ptr)->m_error_flag = true;
		},
		num_ec_tasks);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to configure doca_ec create task pool"};
	}

	static_cast<void>(doca_ctx_set_user_data(doca_ec_as_ctx(m_ec), doca_data{.ptr = this}));

	ret = doca_pe_connect_ctx(m_pe, doca_ec_as_ctx(m_ec));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to register doca_ec context with progress engine"};
	}

	ret = doca_ctx_start(doca_ec_as_ctx(m_ec));
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to start doca_ec context"};
	}

	// Create a matrix that creates one redundancy block per 2 data blocks
	ret = doca_ec_matrix_create(m_ec, storage::matrix_type_from_string(ec_matrix_type), 2, 1, &m_ec_matrix);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to create doca_ec matrix"};
	}

	/* LZ4 compression can result in the output being larger than the input in cases of non-compressible data.
	 * To keep things simple this buffer is over allocated so that the LZ4 compress will not fail and the higher
	 * level application logic can check for that and handle the error itself
	 */
	m_compressed_bytes_buffer.resize(m_block_size * 2, padding_byte);
	m_gga_output_buffer_bytes.resize(m_block_size, padding_byte);

	m_input_mmap = storage::make_mmap(m_dev,
					  reinterpret_cast<char *>(m_compressed_bytes_buffer.data()),
					  m_compressed_bytes_buffer.size(),
					  DOCA_ACCESS_FLAG_LOCAL_READ_WRITE,
					  storage::thread_safety::no);

	m_output_mmap = storage::make_mmap(m_dev,
					   reinterpret_cast<char *>(m_gga_output_buffer_bytes.data()),
					   m_gga_output_buffer_bytes.size(),
					   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE,
					   storage::thread_safety::no);

	m_buf_inv = storage::make_buf_inventory(num_ec_buffers);

	// Make a buffer that can access any area of the input data
	ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
						 m_input_mmap,
						 m_compressed_bytes_buffer.data(),
						 m_compressed_bytes_buffer.size(),
						 &m_input_buf);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Unable to init input buffer: "s + doca_error_get_name(ret)};
	}

	ret = doca_buf_inventory_buf_get_by_addr(m_buf_inv,
						 m_output_mmap,
						 m_gga_output_buffer_bytes.data(),
						 m_gga_output_buffer_bytes.size(),
						 &m_output_buf);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Unable to init output buffer: "s + doca_error_get_name(ret)};
	}

	ret = doca_ec_task_create_allocate_init(m_ec, m_ec_matrix, m_input_buf, m_output_buf, doca_data{}, &m_ec_task);
	if (ret != DOCA_SUCCESS) {
		throw storage::runtime_error{ret, "Failed to get doca_ec task"};
	}
}

uint32_t get_out_byte_count(doca_ec_task_create *task)
{
	size_t buf_len = 0;
	static_cast<void>(doca_buf_get_data_len(doca_ec_task_create_get_rdnc_blocks(task), &buf_len));
	return buf_len;
}

gga_offload_sbc_gen_result gga_offload_sbc_gen_app::generate_binary_content(std::vector<uint8_t> input_data)
{
	doca_error_t ret;
	auto const metadata_header_size = sizeof(storage::compressed_block_header);
	auto const metadata_trailer_size = sizeof(storage::compressed_block_trailer);
	auto const metadata_overhead_size = metadata_header_size + metadata_trailer_size;

	auto const total_block_count = input_data.size() / m_block_size;

	gga_offload_sbc_gen_result out_data;
	out_data.block_count = total_block_count;
	out_data.data_1_content.reserve(input_data.size() / 2);
	out_data.data_2_content.reserve(input_data.size() / 2);
	out_data.data_p_content.reserve(input_data.size() / 2);

	for (uint32_t ii = 0; ii != total_block_count; ++ii) {
		// Compress the data
		auto const compressed_size =
			m_lz4_ctx.compress(input_data.data() + (ii * m_block_size),
					   m_block_size,
					   m_compressed_bytes_buffer.data() + metadata_header_size,
					   m_compressed_bytes_buffer.size() - metadata_overhead_size);

		if (compressed_size + metadata_overhead_size > m_block_size) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"Data was not compressible enough to be held in internal storage format. Max compressed size of a block is : " +
					std::to_string(m_block_size - metadata_overhead_size) +
					". Block compressed to a size of: " + std::to_string(compressed_size)};
		}

		/* Set header padding and trailer into the compressed bytes buffer to prepare it for partitioning
		 * between data 1 and data 2 partitions */
		auto *data_position = m_compressed_bytes_buffer.data();
		auto remaining_bytes = m_block_size;

		/* Write the header */
		storage::compressed_block_header const hdr{
			htobe32(m_block_size),
			htobe32(compressed_size),
		};
		::memcpy(data_position, &hdr, sizeof(storage::compressed_block_header));
		data_position += sizeof(storage::compressed_block_header);
		remaining_bytes -= sizeof(storage::compressed_block_header);

		/* Skip past the data (it's already in place during compression above) */
		data_position += compressed_size;
		remaining_bytes -= compressed_size;

		/* Zero out the unused bytes */
		auto const padding_len = remaining_bytes - sizeof(storage::compressed_block_trailer);
		::memset(data_position, 0, padding_len);
		data_position += padding_len;
		remaining_bytes -= padding_len;

		/* Write the trailer */
		storage::compressed_block_trailer const tlr{0};
		::memcpy(data_position, &tlr, sizeof(storage::compressed_block_trailer));

		// Copy first half block to data 1 partition buffer
		std::copy(std::begin(m_compressed_bytes_buffer),
			  std::begin(m_compressed_bytes_buffer) + (m_block_size / 2),
			  std::back_inserter(out_data.data_1_content));

		// Copy second half block to data 2 partition buffer
		std::copy(std::begin(m_compressed_bytes_buffer) + (m_block_size / 2),
			  std::begin(m_compressed_bytes_buffer) + m_block_size,
			  std::back_inserter(out_data.data_2_content));

		// generate EC blocks
		static_cast<void>(doca_buf_set_data(m_input_buf, m_compressed_bytes_buffer.data(), m_block_size));
		static_cast<void>(doca_buf_reset_data_len(m_output_buf));

		ret = doca_task_submit(doca_ec_task_create_as_task(m_ec_task));
		if (ret != DOCA_SUCCESS) {
			throw std::runtime_error{"Failed to submit doca_ec task: "s + doca_error_get_name(ret)};
		}

		for (;;) {
			size_t in_flight_count = 0;
			static_cast<void>(doca_ctx_get_num_inflight_tasks(doca_ec_as_ctx(m_ec), &in_flight_count));
			if (in_flight_count) {
				static_cast<void>(doca_pe_progress(m_pe));
			} else {
				if (m_error_flag)
					throw std::runtime_error{"Failed to execute doca_ec task"};
				else
					break;
			}
		}

		if (get_out_byte_count(m_ec_task) != (m_block_size / 2)) {
			throw std::runtime_error{"doca_ec task return invalid result"};
		}

		std::copy(std::begin(m_gga_output_buffer_bytes),
			  std::begin(m_gga_output_buffer_bytes) + (m_block_size / 2),
			  std::back_inserter(out_data.data_p_content));
	}

	DOCA_LOG_TRC("Out data: { block_count: %u, d1_size: %zu, d2_size: %zu, p_size: %zu}",
		     out_data.block_count,
		     out_data.data_1_content.size(),
		     out_data.data_2_content.size(),
		     out_data.data_p_content.size());

	return out_data;
}

} // namespace
