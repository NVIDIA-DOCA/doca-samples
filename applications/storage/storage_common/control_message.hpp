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

#ifndef APPLICATIONS_STORAGE_STORAGE_COMMON_CONTROL_MESSAGE_HPP_
#define APPLICATIONS_STORAGE_STORAGE_COMMON_CONTROL_MESSAGE_HPP_

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#include <doca_error.h>

namespace storage::control {

enum class rdma_connection_role {
	io_data,    /* Connection to transfer data to or from the storage */
	io_control, /* Connection to exchange io commands and io responses */
};

struct message_id {
	uint32_t value;
};

inline bool operator==(message_id const &lhs, message_id const &rhs)
{
	return lhs.value == rhs.value;
}

struct correlation_id {
	uint32_t value;
};

enum class message_type : uint32_t {
	error_response = 0,
	query_storage_request,
	query_storage_response,
	init_storage_request,
	init_storage_response,
	create_rdma_connection_request,
	create_rdma_connection_response,
	start_storage_request,
	start_storage_response,
	stop_storage_request,
	stop_storage_response,
	shutdown_request,
	shutdown_response,
};

struct message_header {
	uint32_t wire_size;
};

struct message {
	struct payload {
		virtual ~payload() = default;
	};

	// Type of message, so that the type of payload (if any) is known
	storage::control::message_type message_type;
	// Message ID uniquely identifies a pair of request / response messages
	storage::control::message_id message_id;
	// Correlation id allows to track "sets" of request / response pairs as related to each other
	storage::control::correlation_id correlation_id;
	std::unique_ptr<storage::control::message::payload> payload;
};

/*
 * Error details
 */
struct error_response_payload : public storage::control::message::payload {
	doca_error_t error_code;
	std::string message; /* A message to give the caller some context / understanding of the error */

	~error_response_payload() override = default;
	error_response_payload() = default;
	error_response_payload(doca_error_t error_code_, std::string message_)
		: error_code{error_code_},
		  message{std::move(message_)}
	{
	}
	error_response_payload(error_response_payload const &) = default;
	error_response_payload(error_response_payload &&) noexcept = default;
	error_response_payload &operator=(error_response_payload const &) = default;
	error_response_payload &operator=(error_response_payload &&) noexcept = default;
};

/*
 * Storage query
 */
struct storage_details_payload : public storage::control::message::payload {
	uint64_t total_size; /* Total size of the storage */
	uint32_t block_size; /* Block size used by the storage */

	~storage_details_payload() override = default;
	storage_details_payload() = default;
	storage_details_payload(uint64_t total_size_, uint32_t block_size_)
		: total_size{total_size_},
		  block_size{block_size_}
	{
	}
	storage_details_payload(storage_details_payload const &) = default;
	storage_details_payload(storage_details_payload &&) noexcept = default;
	storage_details_payload &operator=(storage_details_payload const &) = default;
	storage_details_payload &operator=(storage_details_payload &&) noexcept = default;
};

/*
 * Storage initialisation
 */
struct init_storage_payload : public storage::control::message::payload {
	uint32_t task_count;		       /* Number of tasks to use */
	uint32_t core_count;		       /* Number of cores to use */
	std::vector<uint8_t> mmap_export_blob; /* Remote memory the storage will read from / write to */

	~init_storage_payload() override = default;
	init_storage_payload() = default;
	init_storage_payload(uint32_t task_count_, uint32_t core_count_, std::vector<uint8_t> mmap_export_blob_)
		: task_count{task_count_},
		  core_count{core_count_},
		  mmap_export_blob{std::move(mmap_export_blob_)}
	{
	}
	init_storage_payload(init_storage_payload const &) = default;
	init_storage_payload(init_storage_payload &&) noexcept = default;
	init_storage_payload &operator=(init_storage_payload const &) = default;
	init_storage_payload &operator=(init_storage_payload &&) noexcept = default;
};

/*
 * RDMA connection details
 */
struct rdma_connection_details_payload : public storage::control::message::payload {
	uint32_t context_idx;			     /* Which context the connection should belong to */
	storage::control::rdma_connection_role role; /* Role of the rdma connection */
	std::vector<uint8_t> connection_details;     /* Sender connection details */

	~rdma_connection_details_payload() override = default;
	rdma_connection_details_payload() = default;
	explicit rdma_connection_details_payload(uint32_t context_idx_,
						 rdma_connection_role role_,
						 std::vector<uint8_t> connection_details_)
		: context_idx{context_idx_},
		  role{role_},
		  connection_details{std::move(connection_details_)}
	{
	}
	rdma_connection_details_payload(rdma_connection_details_payload const &) = default;
	rdma_connection_details_payload(rdma_connection_details_payload &&) noexcept = default;
	rdma_connection_details_payload &operator=(rdma_connection_details_payload const &) = default;
	rdma_connection_details_payload &operator=(rdma_connection_details_payload &&) noexcept = default;
};

/*
 * Calculate the wire size of a control message header
 *
 * @message_header [in]: Message header
 * @return: Wire size (in bytes) required to encode the header
 */
uint32_t wire_size(storage::control::message_header const &hdr) noexcept;

/*
 * Calculate the wire size of a control message including its payload
 *
 * @message_header [in]: Message header
 * @return: Wire size (in bytes) required to encode the message
 */
uint32_t wire_size(storage::control::message const &msg);

/*
 * Encode a message header into a network byte order buffer
 *
 * @buffer [in]: Buffer to fill
 * @hdr [in]: Header to encode
 * @return: New position of the write buffer (ie buffer + wire_size(hdr)
 */
char *encode(char *buffer, storage::control::message_header const &hdr) noexcept;

/*
 * Encode a message including its payload into a network byte order buffer
 *
 * @buffer [in]: Buffer to fill
 * @msg [in]: Message to encode
 * @return: New position of the write buffer (ie buffer + wire_size(msg)
 */
char *encode(char *buffer, storage::control::message const &msg);

/*
 * Decode a message header from a network byte order buffer
 *
 * @buffer [in]: Buffer to read from
 * @msg [in]: Header to decode into
 * @return: New position of the read buffer (ie buffer + wire_size(msg)
 */
char const *decode(char const *buffer, storage::control::message_header &hdr) noexcept;

/*
 * Decode a message including its payload from a network byte order buffer
 *
 * @buffer [in]: Buffer to read from
 * @msg [in]: Message to decode into
 * @return: New position of the read buffer (ie buffer + wire_size(msg)
 */
char const *decode(char const *buffer, storage::control::message &msg);

/*
 * Convert a message_type enum to a string a user can read
 *
 * @type [in]: Enum to convert
 * @return: User readable string
 */
std::string to_string(storage::control::message_type type);

/*
 * Convert a rdma_connection_role enum to a string a user can read
 *
 * @role [in]: Enum to convert
 * @return: User readable string
 */
std::string to_string(storage::control::rdma_connection_role role);

/*
 * Convert a message to a string a user can read
 *
 * @msg [in]: Message to convert
 * @return: User readable string
 */
std::string to_string(storage::control::message const &msg);

} // namespace storage::control

#endif /* APPLICATIONS_STORAGE_STORAGE_COMMON_CONTROL_MESSAGE_HPP_ */