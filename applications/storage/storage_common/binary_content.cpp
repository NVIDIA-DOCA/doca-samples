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

#include <storage_common/binary_content.hpp>

#include <cstdio>

#include <doca_error.h>

#include <storage_common/definitions.hpp>
#include <storage_common/os_utils.hpp>

namespace storage {

namespace {

uint64_t constexpr sbc_magic_value = 0xDEADF00D1337FADE;

auto constexpr max_sbc_content_size = uint32_t{2} * 1024 * 1024 * 1024; /* 2 GB */

struct file_handle {
	~file_handle()
	{
		if (m_f != nullptr) {
			fclose(m_f);
			m_f = nullptr;
		}
	}

	file_handle() = delete;

	explicit file_handle(FILE *f) : m_f{f}
	{
	}

	file_handle(file_handle const &) = delete;
	file_handle(file_handle &&) noexcept = delete;
	file_handle &operator=(file_handle const &) = delete;
	file_handle &operator=(file_handle &&) noexcept = delete;

	operator FILE *()
	{
		return m_f;
	}

private:
	FILE *m_f;
};

} // namespace

bool file_has_binary_content_header(std::string const &file_name)
{
	auto fh = file_handle{[&file_name]() {
		auto *f = fopen(file_name.c_str(), "rb");
		if (f == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_NOT_FOUND, "Unable to open file: " + file_name};
		}
		return f;
	}()};

	uint64_t magic;
	if (fread(&magic, 1, sizeof(magic), fh) != sizeof(magic)) {
		return false;
	}

	magic = be64toh(magic);

	return magic == sbc_magic_value;
}

storage::binary_content load_binary_content_from_file(std::string const &file_name)
{
	auto fh = file_handle{[&file_name]() {
		auto *f = fopen(file_name.c_str(), "rb");
		if (f == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_NOT_FOUND, "Unable to open sbc file: " + file_name};
		}
		return f;
	}()};
	storage::binary_content sbc{};

	uint64_t magic;
	if (fread(&magic, 1, sizeof(magic), fh) != sizeof(magic)) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to read magic from sbc file"};
	}

	magic = be64toh(magic);
	if (magic != sbc_magic_value) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE, "Expected magic value did not match"};
	}

	if (fread(&sbc.block_size, 1, sizeof(sbc.block_size), fh) != sizeof(sbc.block_size)) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to read block size from sbc file"};
	}

	if (fread(&sbc.block_count, 1, sizeof(sbc.block_count), fh) != sizeof(sbc.block_count)) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to read block count from sbc file"};
	}

	sbc.block_size = be32toh(sbc.block_size);
	sbc.block_count = be32toh(sbc.block_count);

	auto const content_size = size_t{sbc.block_size} * sbc.block_count;
	if (content_size > max_sbc_content_size) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Failed to read content from sbc file. Exceeded limit: " +
						     std::to_string(max_sbc_content_size)};
	}

	sbc.content.resize(content_size);
	if (fread(sbc.content.data(), 1, sbc.content.size(), fh) != sbc.content.size()) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to read content from sbc file"};
	}

	return sbc;
}

void write_binary_content_to_file(std::string const &file_name, storage::binary_content const &sbc)
{
	auto fh = file_handle{[&file_name]() {
		auto *f = fopen(file_name.c_str(), "wb");
		if (f == nullptr) {
			throw storage::runtime_error{DOCA_ERROR_NOT_FOUND, "Unable to open sbc file: " + file_name};
		}
		return f;
	}()};

	auto const sbc_size = size_t{sbc.block_count} * sbc.block_size;
	if (sbc_size > max_sbc_content_size) {
		throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE,
					     "Failed to write content to sbc file. Exceeded limit: " +
						     std::to_string(max_sbc_content_size)};
	}

	uint32_t const block_size = htobe32(sbc.block_size);
	uint32_t const block_count = htobe32(sbc.block_count);

	uint64_t const magic = htobe64(sbc_magic_value);
	if (fwrite(&magic, 1, sizeof(magic), fh) != sizeof(magic)) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to write magic to sbc file"};
	}

	if (fwrite(&block_size, 1, sizeof(block_size), fh) != sizeof(block_size)) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to write block size to sbc file"};
	}

	if (fwrite(&block_count, 1, sizeof(block_count), fh) != sizeof(block_count)) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to write block count to sbc file"};
	}

	if (fwrite(sbc.content.data(), 1, sbc.content.size(), fh) != sbc.content.size()) {
		throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to write content to sbc file"};
	}
}

} // namespace storage