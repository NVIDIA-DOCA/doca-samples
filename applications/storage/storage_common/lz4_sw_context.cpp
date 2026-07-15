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

#if !STORAGE_APP_LZ4_SW_LIB_AVAILABLE
#error Attempt to use LZ4 SW lib when it is not enabled
#endif

#include <storage_common/lz4_sw_context.hpp>

#include <doca_log.h>

#include <storage_common/definitions.hpp>

DOCA_LOG_REGISTER(LZ4_SW_CTX);

using namespace std::string_literals;

namespace storage {
namespace {
LZ4F_preferences_t make_lz4_cfg()
{
	LZ4F_preferences_t cfg{};
	cfg.frameInfo.blockSizeID = LZ4F_max64KB;
	cfg.frameInfo.blockMode = LZ4F_blockIndependent;
	cfg.frameInfo.contentChecksumFlag = LZ4F_noContentChecksum;
	cfg.frameInfo.frameType = LZ4F_frame;
	cfg.frameInfo.blockChecksumFlag = LZ4F_noBlockChecksum;
	cfg.compressionLevel = 1;
	cfg.autoFlush = 1;

	return cfg;
}
} // namespace

lz4_sw_context::~lz4_sw_context()
{
	auto const ret = LZ4F_freeCompressionContext(m_ctx);
	if (LZ4F_isError(ret)) {
		DOCA_LOG_ERR("Failed to release LZ4 compression context: %s\n", LZ4F_getErrorName(ret));
	}
}

lz4_sw_context::lz4_sw_context() : m_cfg{make_lz4_cfg()}, m_ctx{nullptr}
{
	auto const ret = LZ4F_createCompressionContext(&m_ctx, LZ4F_VERSION);
	if (LZ4F_isError(ret)) {
		throw storage::runtime_error{DOCA_ERROR_UNKNOWN,
					     "Failed to create LZ4 compression context: "s + LZ4F_getErrorName(ret)};
	}
}

uint32_t lz4_sw_context::compress(uint8_t const *in_bytes,
				  uint32_t in_byte_count,
				  uint8_t *out_bytes,
				  uint32_t out_bytes_size)
{
	uint32_t constexpr trailer_byte_count = 4; // doca_compress does not want the 4 byte trailer at the end of the
	// data

	// Create header
	auto const header_len = LZ4F_compressBegin(m_ctx, out_bytes, out_bytes_size, &m_cfg);
	if (LZ4F_isError(header_len)) {
		throw storage::runtime_error{DOCA_ERROR_UNKNOWN,
					     "Failed to start compression: "s +
						     LZ4F_getErrorName(static_cast<LZ4F_errorCode_t>(header_len))};
	}

	// Skip writing header bytes to any output as doca_compress does not want them

	// do the compression
	auto const compressed_byte_count =
		LZ4F_compressUpdate(m_ctx, out_bytes, out_bytes_size, in_bytes, in_byte_count, nullptr);
	if (LZ4F_isError(compressed_byte_count)) {
		throw storage::runtime_error{DOCA_ERROR_UNKNOWN,
					     "Failed to compress: "s + LZ4F_getErrorName(static_cast<LZ4F_errorCode_t>(
									       compressed_byte_count))};
	}

	// Finalise (may flush any remaining out bytes)
	auto const final_byte_count = LZ4F_compressEnd(m_ctx,
						       out_bytes + compressed_byte_count,
						       out_bytes_size - compressed_byte_count,
						       nullptr);
	if (LZ4F_isError(final_byte_count)) {
		throw storage::runtime_error{
			DOCA_ERROR_UNKNOWN,
			"Failed to complete compression. Error: "s +
				LZ4F_getErrorName(static_cast<LZ4F_errorCode_t>(final_byte_count))};
	}

	return (compressed_byte_count + final_byte_count) - trailer_byte_count;
}

} // namespace storage