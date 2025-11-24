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

#include <remote_offload_common/control_message.hpp>

#ifdef __linux__
#include <endian.h>
#else // ifdef __linux__
#error UNSUPPORTED OS
// FUTURE: Define windows suitable impls for these
#define htobe16
#define htobe32
#define htobe64
#define betoh16
#define betoh32
#define betoh64
#endif // ifdef __linux__

namespace {

/*
 * Emplace an uint32_t value as a big endian byte order value into a byte array
 *
 * @buffer [in]: buffer storage
 * @value [in]: value to set
 * @return: Pointer to next byte after the written value
 */
uint8_t *to_buffer(uint8_t *buffer, uint32_t value) noexcept
{
	value = htobe32(value);
	std::copy(reinterpret_cast<uint8_t const *>(&value),
		  reinterpret_cast<uint8_t const *>(&value) + sizeof(value),
		  buffer);
	return buffer + sizeof(value);
}

/*
 * Emplace a std::string into a byte array
 *
 * @buffer [in]: buffer storage
 * @value [in]: value to set
 * @return: Pointer to next byte after the written value
 */
uint8_t *to_buffer(uint8_t *buffer, std::string const &value) noexcept
{
	buffer = to_buffer(buffer, static_cast<uint32_t>(value.size()));
	std::copy(value.data(), value.data() + value.size(), buffer);

	return buffer + value.size();
}

/*
 * Extracts a uint32_t from the given buffer
 *
 * @buffer [in]: buffer to read from
 * @value [out]: Extracted value in native byte order
 * @return: Pointer to next byte after the read value
 */
uint8_t const *from_buffer(uint8_t const *buffer, uint32_t &value)
{
	std::copy(buffer, buffer + sizeof(value), reinterpret_cast<uint8_t *>(&value));
	value = be32toh(value);
	return buffer + sizeof(value);
}

/*
 * Extracts a std::string from the given buffer
 *
 * @buffer [in]: buffer to read from
 * @value [out]: Extracted value in native byte order
 * @return: Pointer to next byte after the read value
 */
uint8_t const *from_buffer(uint8_t const *buffer, std::string &value)
{
	uint32_t byte_count = 0;
	buffer = from_buffer(buffer, byte_count);
	value = std::string{buffer, buffer + byte_count};
	return buffer + byte_count;
}

} // namespace

namespace remote_offload {
namespace control {

uint32_t wire_size(remote_offload::control::message_header const &hdr) noexcept
{
	static_cast<void>(hdr);
	return sizeof(message_header::wire_size) + sizeof(message_header::msg_id);
}

uint32_t wire_size(remote_offload::control::exchange_consumer_id_data const &payload)
{
	static_cast<void>(payload);
	return sizeof(exchange_consumer_id_data::consumer_id);
}

uint32_t wire_size(remote_offload::control::client_data const &payload)
{
	return sizeof(uint32_t) + payload.message.size();
}

uint8_t *encode(uint8_t *buffer, remote_offload::control::message_header const &hdr) noexcept
{
	buffer = to_buffer(buffer, hdr.wire_size);
	return to_buffer(buffer, static_cast<uint32_t>(hdr.msg_id));
}

uint8_t *encode(uint8_t *buffer, remote_offload::control::exchange_consumer_id_data const &payload) noexcept
{
	return to_buffer(buffer, payload.consumer_id);
}

uint8_t *encode(uint8_t *buffer, remote_offload::control::client_data const &payload) noexcept
{
	return to_buffer(buffer, payload.message);
}

uint8_t const *decode(uint8_t const *buffer, remote_offload::control::message_header &hdr) noexcept
{
	buffer = from_buffer(buffer, hdr.wire_size);
	return from_buffer(buffer, reinterpret_cast<uint32_t &>(hdr.msg_id));
}

uint8_t const *decode(uint8_t const *buffer, remote_offload::control::exchange_consumer_id_data &payload) noexcept
{
	return from_buffer(buffer, payload.consumer_id);
}

uint8_t const *decode(uint8_t const *buffer, remote_offload::control::client_data &payload) noexcept
{
	return from_buffer(buffer, payload.message);
}

} // namespace control
} // namespace remote_offload
