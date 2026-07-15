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

#include <storage_common/buffer_utils.hpp>

namespace storage {

std::string bytes_to_hex_str(char const *bytes, size_t byte_count)
{
	std::string str{};
	str.reserve(byte_count * 2);
	bytes_to_hex_str(bytes, byte_count, str);
	return str;
}

void bytes_to_hex_str(char const *bytes, size_t byte_count, std::string &str)
{
	static char constexpr char_table
		[16]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
	for (auto *byte = bytes; byte != bytes + byte_count; ++byte) {
		uint8_t nibbles[2];
		nibbles[0] = static_cast<uint8_t>(*byte) >> 4;
		nibbles[1] = static_cast<uint8_t>(*byte) & 0xf;
		str.push_back(char_table[nibbles[0]]);
		str.push_back(char_table[nibbles[1]]);
	}
}

size_t aligned_size(size_t alignment, size_t size)
{
	if (alignment == 0)
		return size;

	auto remainder = size % alignment;
	return alignment * ((size / alignment) + (remainder == 0 ? 0 : 1));
}

} /* namespace storage */