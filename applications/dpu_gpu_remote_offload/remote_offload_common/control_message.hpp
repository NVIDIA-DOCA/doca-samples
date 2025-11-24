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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_CONTROL_MESSAGE_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_CONTROL_MESSAGE_HPP_

#include <cstdint>
#include <string>

namespace remote_offload {
namespace control {

enum class message_id : uint32_t {
	start_shutdown,
	shutdown_complete,
	exchange_consumer_id_request,
	exchange_consumer_id_response,
	client_request,
	client_response,
};

struct message_header {
	uint32_t wire_size;
	message_id msg_id;
};

struct exchange_consumer_id_data {
	uint32_t consumer_id;
};

struct client_data {
	std::string message;
};

/*
 * Calculate the wire size of a control message header
 *
 * @hdr [in]: Message header
 * @return: Wire size (in bytes) required to encode the header
 */
uint32_t wire_size(remote_offload::control::message_header const &hdr) noexcept;

/*
 * Calculate the wire size of an exchange_consumer_id_data payload
 *
 * @payload [in]: Message payload
 * @return: Wire size (in bytes) required to encode the message
 */
uint32_t wire_size(remote_offload::control::exchange_consumer_id_data const &payload);

/*
 * Calculate the wire size of a client_data payload
 *
 * @payload [in]: Message payload
 * @return: Wire size (in bytes) required to encode the message
 */
uint32_t wire_size(remote_offload::control::client_data const &payload);

/*
 * Encode a message header into a buffer using network byte order
 *
 * @buffer [in]: Buffer to fill
 * @hdr [in]: Header to encode
 * @return: New position of the write buffer (ie buffer + wire_size(hdr)
 */
uint8_t *encode(uint8_t *buffer, remote_offload::control::message_header const &hdr) noexcept;

/*
 * Encode an exchange_consumer_id_data payload into a buffer using network byte order
 *
 * @buffer [in]: Buffer to fill
 * @payload [in]: Message payload to encode
 * @return: New position of the write buffer (ie buffer + wire_size(payload)
 */
uint8_t *encode(uint8_t *buffer, remote_offload::control::exchange_consumer_id_data const &payload) noexcept;

/*
 * Encode a client_data payload into a buffer using network byte order
 *
 * @buffer [in]: Buffer to fill
 * @payload [in]: Message payload to encode
 * @return: New position of the write buffer (ie buffer + wire_size(payload)
 */
uint8_t *encode(uint8_t *buffer, remote_offload::control::client_data const &payload) noexcept;

/*
 * Decode a message header from a network byte order buffer
 *
 * @buffer [in]: Buffer to read from
 * @hdr [in]: Header to decode into
 * @return: New position of the read buffer (ie buffer + wire_size(hdr)
 */
uint8_t const *decode(uint8_t const *buffer, remote_offload::control::message_header &hdr) noexcept;

/*
 * Decode an exchange_consumer_id_data payload from a network byte order buffer
 *
 * @buffer [in]: Buffer to read from
 * @payload [in]: Message payload to decode into
 * @return: New position of the read buffer (ie buffer + wire_size(payload)
 */
uint8_t const *decode(uint8_t const *buffer, remote_offload::control::exchange_consumer_id_data &payload) noexcept;

/*
 * Decode a client_data payload from a network byte order buffer
 *
 * @buffer [in]: Buffer to read from
 * @payload [in]: Message payload to decode into
 * @return: New position of the read buffer (ie buffer + wire_size(payload)
 */
uint8_t const *decode(uint8_t const *buffer, remote_offload::control::client_data &payload) noexcept;

} /* namespace control */
} /* namespace remote_offload */

#endif /* APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_CONTROL_MESSAGE_HPP_ */
