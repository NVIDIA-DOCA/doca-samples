/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef COMMON_UTILS_H
#define COMMON_UTILS_H

#include <stdint.h>

/**
 * This method aligns a uint64 value up
 *
 * @value [in]: value to align up
 * @alignment [in]: alignment value
 * @return: aligned value
 */
static inline uint64_t common_utils_align_up_uint64(uint64_t value, uint64_t alignment)
{
	uint64_t remainder = (value % alignment);

	if (remainder == 0)
		return value;

	return value + (alignment - remainder);
}

/**
 * This method aligns a uint32 value up
 *
 * @value [in]: value to align up
 * @alignment [in]: alignment value
 * @return: aligned value
 */
static inline uint32_t common_utils_align_up_uint32(uint32_t value, uint32_t alignment)
{
	uint64_t remainder = (value % alignment);
	if (remainder == 0)
		return value;
	return (uint32_t)(value + (alignment - remainder));
}

/**
 * This method returns the next power of two of a uint64 value
 *
 * @x [in]: value to get the next power of two
 * @return: next power of two
 */
static inline uint64_t common_utils_next_power_of_two(uint64_t x)
{
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x |= x >> 32;
	return x + 1;
}

#endif /* COMMON_UTILS_H */
