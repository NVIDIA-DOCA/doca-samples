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

#ifndef APPLICATIONS_STORAGE_STORAGE_COMMON_IO_MESSAGE_HPP_
#define APPLICATIONS_STORAGE_STORAGE_COMMON_IO_MESSAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include <doca_error.h>
#include <doca_types.h>

#include <storage_common/definitions.hpp>
#include <storage_common/buffer_utils.hpp>

namespace storage {

/*
 * Types of io message
 */
enum class io_message_type : uint32_t {
	result = 0,
	read,
	write,
};

/*
 * Absolute size of an io_message
 */
size_t constexpr size_of_io_message = 64;

/*
 * Utility to allow for easy access and manipulation of each io_message field without having to process the full message
 */
class io_message_view {
public:
	/*
	 * Get the message type
	 *
	 * @buf [in]: Pointer to the message buffer
	 * @return: Type of the message
	 */
	static inline io_message_type get_type(char const *buf)
	{
		io_message_type ret{};
		static_cast<void>(storage::from_buffer(buf, reinterpret_cast<uint32_t &>(ret)));
		return ret;
	}

	/*
	 * Set the message type
	 *
	 * @type [in]: Type of the message
	 * @buf [in/out]: Pointer to the message buffer
	 */
	static inline void set_type(io_message_type type, char *buf)
	{
		storage::to_buffer(buf, static_cast<uint32_t>(type));
	}

	/*
	 * Get the message user data
	 *
	 * @buf [in]: Pointer to the message buffer
	 * @return: Message user data
	 */
	static inline doca_data get_user_data(char const *buf)
	{
		doca_data ret{};
		static_cast<void>(storage::from_buffer(buf + offsetof(layout, user_data), ret.u64));
		return ret;
	}

	/*
	 * Set the message user data
	 *
	 * @user_data [in]: User data value
	 * @buf [in/out]: Pointer to the message buffer
	 */
	static inline void set_user_data(doca_data user_data, char *buf)
	{
		storage::to_buffer(buf + offsetof(layout, user_data), user_data.u64);
	}

	/*
	 * Get the message result
	 *
	 * @buf [in]: Pointer to the message buffer
	 * @return: Returned status code. Typically: DOCA_SUCCESS on success and DOCA_ERROR otherwise
	 */
	static inline doca_error_t get_result(char const *buf)
	{
		doca_error_t ret{};
		static_cast<void>(
			storage::from_buffer(buf + offsetof(layout, result), reinterpret_cast<uint32_t &>(ret)));
		return ret;
	}

	/*
	 * Set the message result
	 *
	 * @result [in]: Result value
	 * @buf [in/out]: Pointer to the message buffer
	 */
	static inline void set_result(doca_error_t result, char *buf)
	{
		storage::to_buffer(buf + offsetof(layout, result), static_cast<uint32_t>(result));
	}

	/*
	 * Get the message io storage offset (offset within storage memory map)
	 *
	 * @buf [in]: Pointer to the message buffer
	 * @return: Message io address
	 */
	static inline uint64_t get_storage_offset(char const *buf)
	{
		uint64_t ret{};
		static_cast<void>(storage::from_buffer(buf + offsetof(layout, storage_offset), ret));
		return ret;
	}

	/*
	 * Set the message io storage offset (offset within storage memory map)
	 *
	 * @storage_offset [in]: Storage offset value
	 * @buf [in/out]: Pointer to the message buffer
	 */
	static inline void set_storage_offset(uint64_t storage_offset, char *buf)
	{
		storage::to_buffer(buf + offsetof(layout, storage_offset), storage_offset);
	}

	/*
	 * Get the message io size
	 *
	 * @buf [in]: Pointer to the message buffer
	 * @return: Message io size
	 */
	static inline uint32_t get_io_size(char const *buf)
	{
		uint32_t ret{};
		static_cast<void>(storage::from_buffer(buf + offsetof(layout, io_size), ret));
		return ret;
	}

	/*
	 * Set the message io size
	 *
	 * @io_size [in]: Size value
	 * @buf [in/out]: Pointer to the message buffer
	 */
	static inline void set_io_size(uint32_t io_size, char *buf)
	{
		storage::to_buffer(buf + offsetof(layout, io_size), io_size);
	}

	/*
	 * Get the message io requester offset (offset within requester memory map)
	 *
	 * @buf [in]: Pointer to the message buffer
	 * @return: Requester offset
	 */
	static inline uint64_t get_requester_offset(char const *buf)
	{
		uint64_t ret{};
		static_cast<void>(storage::from_buffer(buf + offsetof(layout, requester_offset), ret));
		return ret;
	}

	/*
	 * Set the message io requester offset (offset within requester memory map)
	 *
	 * @requester_offset [in]: Requester offset value
	 * @buf [in/out]: Pointer to the message buffer
	 */
	static inline void set_requester_offset(uint64_t requester_offset, char *buf)
	{
		storage::to_buffer(buf + offsetof(layout, requester_offset), requester_offset);
	}

private:
	/*
	 * Physical model of the message to automate the calculation of value offsets
	 */
	struct alignas(storage::cache_line_size) layout {
		uint32_t type; /* use 4 bytes for the layout to keep data members aligned */
		doca_data user_data;
		doca_error_t result;
		uint32_t io_size;
		uint64_t requester_offset;
		uint64_t storage_offset;
	};

	static_assert(sizeof(layout) == storage::size_of_io_message,
		      "io_message_buffer size is supposed to be a single cache line");
};

/*
 * Convert an io_message to a string a user can read
 *
 * @io_message [in]: Buffer holding the io message
 * @return: User readable string
 */
std::string io_message_to_string(char const *io_message);

} /* namespace storage */

#endif /* APPLICATIONS_STORAGE_STORAGE_COMMON_IO_MESSAGE_HPP_ */