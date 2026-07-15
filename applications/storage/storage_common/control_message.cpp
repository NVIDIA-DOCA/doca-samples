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

#include <storage_common/control_message.hpp>

#include <storage_common/buffer_utils.hpp>
#include <storage_common/definitions.hpp>

namespace storage::control {

uint32_t wire_size(storage::control::message_header const &hdr) noexcept
{
	static_cast<void>(hdr);
	return sizeof(message_header::wire_size);
}

uint32_t wire_size(storage::control::message const &msg)
{
	uint32_t size = sizeof(message::message_type) + sizeof(message::message_id) + sizeof(message::correlation_id);
	switch (msg.message_type) {
	case message_type::error_response: {
		size += sizeof(error_response_payload::error_code);
		auto const *details = dynamic_cast<error_response_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to calculate wire size for invalid error_response, no payload"};
		}

		size += sizeof(uint32_t);
		size += details->message.size();
	} break;
	case message_type::query_storage_response: {
		size += sizeof(storage_details_payload::total_size);
		size += sizeof(storage_details_payload::block_size);
	} break;
	case message_type::init_storage_request: {
		size += sizeof(init_storage_payload::transaction_count);
		size += sizeof(init_storage_payload::core_count);

		auto const *details = dynamic_cast<init_storage_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to calculate wire size for invalid init_storage_request, no payload"};
		}

		size += sizeof(uint32_t);
		size += details->mmap_export_blob.size();
	} break;
	case message_type::create_rdma_connection_response: // FALLTHROUGH
	case message_type::create_rdma_connection_request: {
		size += sizeof(rdma_connection_details_payload::context_idx);
		size += sizeof(rdma_connection_details_payload::role);

		auto const *details = dynamic_cast<rdma_connection_details_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to calculate wire size for invalid create_rdma_connection_xxx message, no payload"};
		}

		size += sizeof(uint32_t);
		size += details->connection_details.size();
	} break;
	case message_type::query_storage_request:
	case message_type::init_storage_response:
	case message_type::start_storage_request:
	case message_type::start_storage_response:
	case message_type::stop_storage_request:
	case message_type::stop_storage_response:
	case message_type::shutdown_request:
	case message_type::shutdown_response:
		break;
	default:
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Unable to calculate wire size for unhandled message type: " +
						     to_string(msg.message_type)};
	}

	return size;
}

char *encode(char *buffer, storage::control::message_header const &hdr) noexcept
{
	return storage::to_buffer(buffer, hdr.wire_size);
}

char *encode(char *buffer, storage::control::message const &msg)
{
	buffer = storage::to_buffer(buffer, static_cast<uint32_t>(msg.message_type));
	buffer = storage::to_buffer(buffer, msg.message_id.value);
	buffer = storage::to_buffer(buffer, msg.correlation_id.value);

	switch (msg.message_type) {
	case message_type::error_response: {
		auto const *details = dynamic_cast<error_response_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
						     "[bug] Unable to encode invalid error_response, no payload"};
		}

		buffer = storage::to_buffer(buffer, static_cast<uint32_t>(details->error_code));
		buffer = storage::to_buffer(buffer, details->message);
	} break;
	case message_type::query_storage_response: {
		auto const *details = dynamic_cast<storage_details_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to encode invalid query_storage_response, no payload"};
		}
		buffer = storage::to_buffer(buffer, details->total_size);
		buffer = storage::to_buffer(buffer, details->block_size);
	} break;
	case message_type::init_storage_request: {
		auto const *details = dynamic_cast<init_storage_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
						     "[bug] Unable to encode invalid init_storage_request, no payload"};
		}
		buffer = storage::to_buffer(buffer, details->transaction_count);
		buffer = storage::to_buffer(buffer, details->core_count);
		buffer = storage::to_buffer(buffer, details->mmap_export_blob);
	} break;
	case message_type::create_rdma_connection_request: // FALLTHROUGH
	case message_type::create_rdma_connection_response: {
		auto const *details = dynamic_cast<rdma_connection_details_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to encode invalid rdma_connection_details_payload, no payload"};
		}
		buffer = storage::to_buffer(buffer, details->context_idx);
		buffer = storage::to_buffer(buffer, static_cast<uint32_t>(details->role));
		buffer = storage::to_buffer(buffer, details->connection_details);
	} break;
	case message_type::query_storage_request:
	case message_type::init_storage_response:
	case message_type::start_storage_request:
	case message_type::start_storage_response:
	case message_type::stop_storage_request:
	case message_type::stop_storage_response:
	case message_type::shutdown_request:
	case message_type::shutdown_response:
		break;
	default: {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Unable to encode unhandled message type: " + to_string(msg.message_type)};
	}
	}

	return buffer;
}

char const *decode(char const *buffer, storage::control::message_header &hdr) noexcept
{
	return storage::from_buffer(buffer, hdr.wire_size);
}

char const *decode(char const *buffer, storage::control::message &msg)
{
	buffer = storage::from_buffer(buffer, reinterpret_cast<uint32_t &>(msg.message_type));
	buffer = storage::from_buffer(buffer, msg.message_id.value);
	buffer = storage::from_buffer(buffer, msg.correlation_id.value);

	switch (msg.message_type) {
	case message_type::error_response: {
		auto details = std::make_unique<error_response_payload>();
		buffer = storage::from_buffer(buffer, reinterpret_cast<uint32_t &>(details->error_code));
		buffer = storage::from_buffer(buffer, details->message);
		msg.payload = std::move(details);
	} break;
	case message_type::query_storage_response: {
		auto details = std::make_unique<storage_details_payload>();
		buffer = storage::from_buffer(buffer, details->total_size);
		buffer = storage::from_buffer(buffer, details->block_size);
		msg.payload = std::move(details);
	} break;
	case message_type::init_storage_request: {
		auto details = std::make_unique<init_storage_payload>();
		buffer = storage::from_buffer(buffer, details->transaction_count);
		buffer = storage::from_buffer(buffer, details->core_count);
		buffer = storage::from_buffer(buffer, details->mmap_export_blob);
		msg.payload = std::move(details);
	} break;
	case message_type::create_rdma_connection_request: // FALLTHROUGH
	case message_type::create_rdma_connection_response: {
		auto details = std::make_unique<rdma_connection_details_payload>();

		buffer = storage::from_buffer(buffer, details->context_idx);
		buffer = storage::from_buffer(buffer, reinterpret_cast<uint32_t &>(details->role));
		buffer = storage::from_buffer(buffer, details->connection_details);
		msg.payload = std::move(details);
	} break;
	case message_type::query_storage_request:
	case message_type::init_storage_response:
	case message_type::start_storage_request:
	case message_type::start_storage_response:
	case message_type::stop_storage_request:
	case message_type::stop_storage_response:
	case message_type::shutdown_request:
	case message_type::shutdown_response:
		break;
	default: {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Unable to decode unhandled message type: " + to_string(msg.message_type)};
	}
	}

	return buffer;
}

std::string to_string(storage::control::message_type type)
{
	switch (type) {
	case message_type::error_response:
		return "error_response";
	case message_type::query_storage_request:
		return "query_storage_request";
	case message_type::query_storage_response:
		return "query_storage_response";
	case message_type::init_storage_request:
		return "init_storage_request";
	case message_type::init_storage_response:
		return "init_storage_response";
	case message_type::create_rdma_connection_request:
		return "create_rdma_connection_request";
	case message_type::create_rdma_connection_response:
		return "create_rdma_connection_response";
	case message_type::start_storage_request:
		return "start_storage_request";
	case message_type::start_storage_response:
		return "start_storage_response";
	case message_type::stop_storage_request:
		return "stop_storage_request";
	case message_type::stop_storage_response:
		return "stop_storage_response";
	case message_type::shutdown_request:
		return "shutdown_request";
	case message_type::shutdown_response:
		return "shutdown_response";
	}

	return "UNKNOWN(" + std::to_string(static_cast<uint32_t>(type)) + ")";
}

std::string to_string(storage::control::rdma_connection_role role)
{
	switch (role) {
	case rdma_connection_role::io_control:
		return "io_control";
	case rdma_connection_role::io_data:
		return "io_data";
	}

	return "UNKNOWN(" + std::to_string(static_cast<uint32_t>(role)) + ")";
}

std::string to_string(storage::control::message const &msg)
{
	std::string s;
	s.reserve(512);
	s += "control_message: {";
	s += " cid: ";
	s += std::to_string(msg.correlation_id.value);
	s += ", mid: ";
	s += std::to_string(msg.message_id.value);
	s += ", ";
	s += to_string(msg.message_type);
	s += ": {";
	switch (msg.message_type) {
	case message_type::error_response: {
		auto const *details = dynamic_cast<error_response_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
						     "[bug] Unable to encode invalid error_response, no payload"};
		}
		s += "error_code: ";
		s += doca_error_get_name(details->error_code);
		s += ", message: \"";
		s += details->message;
		s += "\"";
	} break;
	case message_type::query_storage_response: {
		auto const *details = dynamic_cast<storage_details_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to encode invalid query_storage_response, no payload"};
		}
		s += "total_size: ";
		s += std::to_string(details->total_size);
		s += ", block_size: ";
		s += std::to_string(details->block_size);
	} break;
	case message_type::init_storage_request: {
		auto const *details = dynamic_cast<init_storage_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
						     "[bug] Unable to encode invalid init_storage_request, no payload"};
		}
		s += "transaction_count: ";
		s += std::to_string(details->transaction_count);
		s += ", core_count: ";
		s += std::to_string(details->core_count);
		s += ", mmap_export_blob: [";
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(details->mmap_export_blob.data()),
					  details->mmap_export_blob.size(),
					  s);
		s += "]";
	} break;
	case message_type::create_rdma_connection_request: {
		auto const *details = dynamic_cast<rdma_connection_details_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to encode invalid create_rdma_connection_request, no payload"};
		}
		s += "context_idx: ";
		s += std::to_string(details->context_idx);
		s += ", role: ";
		s += to_string(details->role);
		s += ", connection_details: [";
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(details->connection_details.data()),
					  details->connection_details.size(),
					  s);
		s += "]";
	} break;
	case message_type::create_rdma_connection_response: {
		auto const *details = dynamic_cast<rdma_connection_details_payload const *>(msg.payload.get());
		if (details == nullptr) {
			throw storage::runtime_error{
				DOCA_ERROR_INVALID_VALUE,
				"[bug] Unable to encode invalid create_rdma_connection_response, no payload"};
		}
		s += "context_idx: ";
		s += std::to_string(details->context_idx);
		s += ", role: ";
		s += to_string(details->role);
		s += ", connection_details: [";
		storage::bytes_to_hex_str(reinterpret_cast<char const *>(details->connection_details.data()),
					  details->connection_details.size(),
					  s);
		s += "]";
	} break;
	case message_type::query_storage_request:
	case message_type::init_storage_response:
	case message_type::start_storage_request:
	case message_type::start_storage_response:
	case message_type::stop_storage_request:
	case message_type::stop_storage_response:
	case message_type::shutdown_request:
	case message_type::shutdown_response:
		break;
	default:
		s += "UNKNOWN(" + std::to_string(static_cast<uint32_t>(msg.message_type)) + "): {}";
	}

	s += "}}";
	return s;
}

} // namespace storage::control