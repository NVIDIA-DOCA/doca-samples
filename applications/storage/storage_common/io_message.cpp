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

#include <storage_common/io_message.hpp>

#include <doca_log.h>

#include <storage_common/buffer_utils.hpp>

DOCA_LOG_REGISTER(IO_MESSAGE);

namespace storage {

static std::string to_string(storage::io_message_type e)
{
	switch (e) {
	case io_message_type::result:
		return "result";
	case io_message_type::read:
		return "read";
	case io_message_type::write:
		return "write";
	default:
		return "unknown(" + std::to_string(static_cast<int>(e)) + ")";
	}
}

std::string io_message_to_string(char const *buf)
{
	using namespace std::string_literals;
	using std::to_string;

	auto const msg_type = io_message_view::get_type(buf);
	std::string s =
		"type: " + to_string(msg_type) + ", user_data: " + to_string(io_message_view::get_user_data(buf).u64);

	switch (msg_type) {
	case io_message_type::result:
		s += ", result: "s + doca_error_get_name(io_message_view::get_result(buf));
		break;
	case io_message_type::read:
	case io_message_type::write:
		s += ", storage_offset: " + to_string(io_message_view::get_storage_offset(buf)) +
		     ", requester_offset: " + to_string(io_message_view::get_requester_offset(buf)) +
		     ", length: " + to_string(io_message_view::get_io_size(buf));
		break;
	default:
		break;
	}

	return s;
}

} /* namespace storage */