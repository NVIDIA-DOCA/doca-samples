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

#ifndef APPLICATIONS_STORAGE_STORAGE_COMMON_LZ4_SW_CONTEXT_HPP_
#define APPLICATIONS_STORAGE_STORAGE_COMMON_LZ4_SW_CONTEXT_HPP_

#include <cstdint>

#if STORAGE_APP_LZ4_SW_LIB_AVAILABLE
#include <lz4frame.h>
#endif

namespace storage {

class lz4_sw_context {
public:
	~lz4_sw_context();

	lz4_sw_context();

	lz4_sw_context(lz4_sw_context const &) = delete;

	lz4_sw_context(lz4_sw_context &&) noexcept = delete;

	lz4_sw_context &operator=(lz4_sw_context const &) = delete;

	lz4_sw_context &operator=(lz4_sw_context &&) noexcept = delete;

	uint32_t compress(uint8_t const *in_bytes, uint32_t in_byte_count, uint8_t *out_bytes, uint32_t out_bytes_size);

private:
#if STORAGE_APP_LZ4_SW_LIB_AVAILABLE
	LZ4F_preferences_t m_cfg;
	LZ4F_cctx *m_ctx;
#endif
};

} /* namespace storage */

#endif /* APPLICATIONS_STORAGE_STORAGE_COMMON_LZ4_SW_CONTEXT_HPP_ */
