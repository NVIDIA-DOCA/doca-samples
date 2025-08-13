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

#include <zero_copy/control_message.hpp>

#include <array>
#include <stdexcept>

#include <doca_log.h>

#include <storage_common/buffer_utils.hpp>
#include <storage_common/doca_utils.hpp>

DOCA_LOG_REGISTER(CONTROL_MESSAGE);

namespace storage::zero_copy {

namespace {

auto constexpr base_message_size =
	sizeof(control_message::wire_size) + sizeof(control_message::type) + sizeof(control_message::correlation_id);

/*
 * Get the wire size of a details message
 *
 * @details [in]: Details
 * @return: Wire size
 */
uint32_t details_wire_size(control_message::response const &details)
{
	return sizeof(details.result) + sizeof(uint32_t) + details.message.size();
}

/*
 * Get the wire size of a details message
 *
 * @details [in]: Details
 * @return: Wire size
 */
uint32_t details_wire_size(control_message::configure_data_path const &details)
{
	return sizeof(details.buffer_count) + sizeof(details.buffer_size) + sizeof(details.batch_size) +
	       sizeof(uint32_t) + details.mmap_export_blob.size();
}

/*
 * Get the wire size of a details message
 *
 * @details [in]: Details
 * @return: Wire size
 */
uint32_t details_wire_size(control_message::create_rdma_connection_request const &details)
{
	return sizeof(details.role) + sizeof(uint32_t) + details.connection_details.size();
}

/*
 * Get the wire size of a details message
 *
 * @details [in]: Details
 * @return: Wire size
 */
uint32_t details_wire_size(control_message::create_rdma_connection_response const &details)
{
	return sizeof(details.role) + sizeof(uint32_t) + details.connection_details.size();
}

/*
 * Get the wire size of a message
 *
 * @msg [in]: msg
 * @return: Wire size
 */
uint32_t wire_size(control_message const &msg)
{
	switch (msg.type) {
	case control_message_type::response:
		return base_message_size +
		       details_wire_size(dynamic_cast<control_message::response const &>(*msg.details));
	case control_message_type::configure_data_path:
		return base_message_size +
		       details_wire_size(dynamic_cast<control_message::configure_data_path const &>(*msg.details));
	case control_message_type::create_rdma_connection_request:
		return base_message_size +
		       details_wire_size(
			       dynamic_cast<control_message::create_rdma_connection_request const &>(*msg.details));
	case control_message_type::create_rdma_connection_response:
		return base_message_size +
		       details_wire_size(
			       dynamic_cast<control_message::create_rdma_connection_response const &>(*msg.details));
	case control_message_type::start_storage:   /* FALLTHROUGH */
	case control_message_type::destroy_objects: /* FALLTHROUGH */
	case control_message_type::start_data_path_connections:
		return base_message_size;
	default:
		DOCA_LOG_ERR("Unable to calculate wire size for unknown message type: %u",
			     static_cast<uint32_t>(msg.type));
		return base_message_size;
	}
}

} /* namespace */

control_message make_response_control_message(uint32_t correlation_id)
{
	control_message msg{};
	msg.type = control_message_type::response;
	msg.correlation_id = correlation_id;

	auto details = std::make_unique<control_message::response>();

	details->result = DOCA_SUCCESS;

	msg.details = std::move(details);
	return msg;
}

control_message make_response_control_message(uint32_t correlation_id, doca_error_t result, std::string message)
{
	control_message msg{};
	msg.type = control_message_type::response;
	msg.correlation_id = correlation_id;

	auto details = std::make_unique<control_message::response>();

	details->result = result;
	details->message = std::move(message);

	msg.details = std::move(details);
	return msg;
}

control_message make_configure_data_path_control_message(uint32_t correlation_id,
							 uint32_t buffer_count,
							 uint32_t buffer_size,
							 uint32_t batch_size,
							 std::vector<uint8_t> mmap_export_blob)
{
	control_message msg{};
	msg.type = control_message_type::configure_data_path;
	msg.correlation_id = correlation_id;

	auto details = std::make_unique<control_message::configure_data_path>();

	details->buffer_count = buffer_count;
	details->buffer_size = buffer_size;
	details->batch_size = batch_size;
	details->mmap_export_blob = std::move(mmap_export_blob);

	msg.details = std::move(details);
	return msg;
}

control_message make_start_storage_control_message(uint32_t correlation_id)
{
	control_message msg{};
	msg.type = control_message_type::start_storage;
	msg.correlation_id = correlation_id;

	return msg;
}

control_message make_destroy_objects_control_message(uint32_t correlation_id)
{
	control_message msg{};
	msg.type = control_message_type::destroy_objects;
	msg.correlation_id = correlation_id;

	return msg;
}

control_message make_create_rdma_connection_request_control_message(uint32_t correlation_id,
								    zero_copy::rdma_connection_role role,
								    std::vector<uint8_t> connection_details)
{
	control_message msg{};
	msg.type = control_message_type::create_rdma_connection_request;
	msg.correlation_id = correlation_id;

	auto details = std::make_unique<control_message::create_rdma_connection_request>();

	details->role = role;
	details->connection_details = std::move(connection_details);

	msg.details = std::move(details);
	return msg;
}

control_message make_create_rdma_connection_response_control_message(uint32_t correlation_id,
								     zero_copy::rdma_connection_role role,
								     std::vector<uint8_t> connection_details)
{
	control_message msg{};
	msg.type = control_message_type::create_rdma_connection_response;
	msg.correlation_id = correlation_id;

	auto details = std::make_unique<control_message::create_rdma_connection_response>();

	details->role = role;
	details->connection_details = std::move(connection_details);

	msg.details = std::move(details);
	return msg;
}

control_message make_start_data_path_connections_control_message(uint32_t correlation_id)
{
	control_message msg{};
	msg.type = control_message_type::start_data_path_connections;
	msg.correlation_id = correlation_id;

	return msg;
}

uint32_t to_buffer(zero_copy::control_message const &msg, char *buffer, uint32_t buffer_capacity) noexcept
{
	uint16_t message_wire_size = wire_size(msg);
	if (buffer_capacity < message_wire_size) {
		DOCA_LOG_ERR("Provided io message buffer is too small (%u bytes), needs to be at least %u bytes",
			     buffer_capacity,
			     message_wire_size);
		return 0;
	}

	buffer = storage::to_buffer(buffer, message_wire_size);
	buffer = storage::to_buffer(buffer, static_cast<uint16_t>(msg.type));
	buffer = storage::to_buffer(buffer, msg.correlation_id);

	switch (msg.type) {
	case control_message_type::response: {
		auto &details = dynamic_cast<control_message::response const &>(*msg.details);
		buffer = storage::to_buffer(buffer, static_cast<uint32_t>(details.result));
		storage::to_buffer(buffer, details.message);
	} break;
	case control_message_type::configure_data_path: {
		auto &details = dynamic_cast<control_message::configure_data_path const &>(*msg.details);
		buffer = storage::to_buffer(buffer, details.buffer_count);
		buffer = storage::to_buffer(buffer, details.buffer_size);
		buffer = storage::to_buffer(buffer, details.batch_size);
		storage::to_buffer(buffer, details.mmap_export_blob);
	} break;
	case control_message_type::create_rdma_connection_request: {
		auto &details = dynamic_cast<control_message::create_rdma_connection_request const &>(*msg.details);
		buffer = storage::to_buffer(buffer, static_cast<uint8_t>(details.role));
		storage::to_buffer(buffer, details.connection_details);
	} break;
	case control_message_type::create_rdma_connection_response: {
		auto &details = dynamic_cast<control_message::create_rdma_connection_response const &>(*msg.details);
		buffer = storage::to_buffer(buffer, static_cast<uint8_t>(details.role));
		storage::to_buffer(buffer, details.connection_details);
	} break;
	case control_message_type::start_storage:   /* FALLTHROUGH */
	case control_message_type::destroy_objects: /* FALLTHROUGH */
	case control_message_type::start_data_path_connections:
		break;
	default:
		DOCA_LOG_ERR("Unable to encode unknown message type: %u", static_cast<uint32_t>(msg.type));
		return 0;
	}

	return message_wire_size;
}

uint32_t from_buffer(char const *buffer, uint32_t buffer_size, zero_copy::control_message &msg) noexcept
{
	if (buffer_size < sizeof(base_message_size)) {
		DOCA_LOG_ERR("Unable to decode message from buffer containing %u bytes", buffer_size);
		return 0;
	}

	buffer = storage::from_buffer(buffer, msg.wire_size);
	if (buffer_size < msg.wire_size) {
		DOCA_LOG_ERR("Unable to decode message from buffer containing %u bytes. Expected %u bytes",
			     buffer_size,
			     msg.wire_size);
		return 0;
	}

	buffer = storage::from_buffer(buffer, reinterpret_cast<uint16_t &>(msg.type));
	buffer = storage::from_buffer(buffer, msg.correlation_id);

	switch (msg.type) {
	case control_message_type::response: {
		auto details = std::make_unique<control_message::response>();
		buffer = storage::from_buffer(buffer, reinterpret_cast<uint32_t &>(details->result));
		storage::from_buffer(buffer, details->message);
		msg.details = std::move(details);
	} break;
	case control_message_type::configure_data_path: {
		auto details = std::make_unique<control_message::configure_data_path>();
		buffer = storage::from_buffer(buffer, details->buffer_count);
		buffer = storage::from_buffer(buffer, details->buffer_size);
		buffer = storage::from_buffer(buffer, details->batch_size);
		storage::from_buffer(buffer, details->mmap_export_blob);
		msg.details = std::move(details);
	} break;
	case control_message_type::create_rdma_connection_request: {
		auto details = std::make_unique<control_message::create_rdma_connection_request>();
		buffer = storage::from_buffer(buffer, reinterpret_cast<uint8_t &>(details->role));
		storage::from_buffer(buffer, details->connection_details);
		msg.details = std::move(details);
	} break;
	case control_message_type::create_rdma_connection_response: {
		auto details = std::make_unique<control_message::create_rdma_connection_response>();
		buffer = storage::from_buffer(buffer, reinterpret_cast<uint8_t &>(details->role));
		storage::from_buffer(buffer, details->connection_details);
		msg.details = std::move(details);
	} break;
	case control_message_type::start_storage:   /* FALLTHROUGH */
	case control_message_type::destroy_objects: /* FALLTHROUGH */
	case control_message_type::start_data_path_connections:
		break;
	default:
		DOCA_LOG_ERR("Unable to encode unknown message type: %u", static_cast<uint32_t>(msg.type));
		return 0;
	}

	return msg.wire_size;
}

std::string to_string(zero_copy::control_message const &msg)
{
	std::string s;
	s.reserve(512);
	s += "control_message: {";
	s += "size: ";
	s += std::to_string(msg.wire_size);
	s += " cid: ";
	s += std::to_string(msg.correlation_id);
	s += " type: ";

	switch (msg.type) {
	case control_message_type::response: {
		auto &details = dynamic_cast<control_message::response const &>(*msg.details);
		s += "response, result: ";
		s += doca_error_get_name(details.result);
		s += ", message: ";
		s += details.message;
	} break;
	case control_message_type::configure_data_path: {
		auto &details = dynamic_cast<control_message::configure_data_path const &>(*msg.details);
		s += "configure_data_path, buffer_count: ";
		s += std::to_string(details.buffer_count);
		s += ", buffer_size: ";
		s += std::to_string(details.buffer_size);
		s += ", batch_size: ";
		s += std::to_string(details.batch_size);
		s += ", mmap_blob: ";
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(details.mmap_export_blob.data()),
					  details.mmap_export_blob.size(),
					  s);
	} break;
	case control_message_type::start_storage: {
		s += "start_storage";
	} break;
	case control_message_type::destroy_objects: {
		s += "destroy_objects";
	} break;
	case control_message_type::create_rdma_connection_request: {
		auto &details = dynamic_cast<control_message::create_rdma_connection_request const &>(*msg.details);
		s += "create_rdma_connection_request, role: ";
		s += (details.role == rdma_connection_role::data ? "data" : "ctrl");
		s += ", connection_details: ";
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(details.connection_details.data()),
					  details.connection_details.size(),
					  s);
	} break;
	case control_message_type::create_rdma_connection_response: {
		auto &details = dynamic_cast<control_message::create_rdma_connection_response const &>(*msg.details);
		s += "create_rdma_connection_response, role: ";
		s += (details.role == rdma_connection_role::data ? "data" : "ctrl");
		s += ", connection_details: ";
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(details.connection_details.data()),
					  details.connection_details.size(),
					  s);
	} break;
	case control_message_type::start_data_path_connections: {
		s += "start_data_path_connections";
	} break;
	default:
		s += "UNKNOWN(" + std::to_string(static_cast<uint16_t>(msg.type)) + ")";
	}

	s += "}";
	return s;
}

bool control_message_reassembler::append(char const *fragment, uint32_t fragment_size)
{
	if (recombined_buffer.empty()) {
		if (fragment_size == 0)
			return false;

		recombined_buffer.reserve(fragment_size);
		static_cast<void>(storage::from_buffer(fragment, message_byte_count));

		std::copy(fragment, fragment + fragment_size, std::back_inserter(recombined_buffer));

		DOCA_LOG_DBG("Start reading %u byte message. Have %lu bytes so far",
			     message_byte_count,
			     recombined_buffer.size());
	} else {
		std::copy(fragment, fragment + fragment_size, std::back_inserter(recombined_buffer));
	}

	return message_byte_count != 0 && message_byte_count <= recombined_buffer.size();
}

zero_copy::control_message control_message_reassembler::extract_message(void)
{
	zero_copy::control_message msg{};

	auto const extracted_bytes = from_buffer(recombined_buffer.data(), recombined_buffer.size(), msg);
	if (extracted_bytes == 0 || extracted_bytes != message_byte_count) {
		throw std::runtime_error{"Unable to extract control message from buffer"};
	}

	recombined_buffer.erase(recombined_buffer.begin(), std::begin(recombined_buffer) + message_byte_count);
	DOCA_LOG_DBG("Extracted %u byte message, Remaining: %lu bytes", message_byte_count, recombined_buffer.size());

	message_byte_count = 0;
	if (!recombined_buffer.empty()) {
		static_cast<void>(storage::from_buffer(recombined_buffer.data(), message_byte_count));
		DOCA_LOG_DBG("Start reading %u byte message. Have %lu bytes so far",
			     message_byte_count,
			     recombined_buffer.size());
	}

	return msg;
}

} /* namespace storage::zero_copy */