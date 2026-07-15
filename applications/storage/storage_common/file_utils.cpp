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

#include <storage_common/file_utils.hpp>

#include <fstream>
#include <stdexcept>

namespace storage {

namespace {

template <typename Container>
Container load_file_bytes(std::string const &file_name)
{
	static_assert(sizeof(typename Container::value_type) == sizeof(char),
		      "load_file_bytes requires container to contain byte sized objects");
	std::ifstream file{file_name, std::ios::binary | std::ios::ate};
	if (!file) {
		throw std::runtime_error{"Unable to open file: \"" + file_name + "\""};
	}

	Container result;
	result.resize(file.tellg());

	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(result.data()), result.size())) {
		throw std::runtime_error{"Failed to read content of file: \"" + file_name + "\""};
	}

	return result;
}

} // namespace

std::vector<uint8_t> load_file_bytes(std::string const &file_name)
{
	return load_file_bytes<std::vector<uint8_t>>(file_name);
}

} // namespace storage