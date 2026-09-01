/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <string.h>

#include "pack.h"

uint64_t ntohq(uint64_t value)
{
	const int numeric_one = 1;

	/* If we are in a Big-Endian architecture, we don't need to do anything */
	if (*(const uint8_t *)&numeric_one != 1)
		return value;

	/* Swap the 8 bytes of our value */
	value = SET_BYTE((uint64_t)GET_BYTE(value, 0), 7) | SET_BYTE((uint64_t)GET_BYTE(value, 1), 6) |
		SET_BYTE((uint64_t)GET_BYTE(value, 2), 5) | SET_BYTE((uint64_t)GET_BYTE(value, 3), 4) |
		SET_BYTE((uint64_t)GET_BYTE(value, 4), 3) | SET_BYTE((uint64_t)GET_BYTE(value, 5), 2) |
		SET_BYTE((uint64_t)GET_BYTE(value, 6), 1) | SET_BYTE((uint64_t)GET_BYTE(value, 7), 0);

	return value;
}

void pack_uint8(uint8_t **buffer, uint8_t value)
{
	uint8_t *write_head = *buffer;

	*write_head++ = value;
	*buffer = write_head;
}

void pack_uint16(uint8_t **buffer, uint16_t value)
{
	uint8_t *write_head = *buffer;

	*write_head++ = GET_BYTE(value, 1);
	*write_head++ = GET_BYTE(value, 0);
	*buffer = write_head;
}

void pack_uint32(uint8_t **buffer, uint32_t value)
{
	uint8_t *write_head = *buffer;

	*write_head++ = GET_BYTE(value, 3);
	*write_head++ = GET_BYTE(value, 2);
	*write_head++ = GET_BYTE(value, 1);
	*write_head++ = GET_BYTE(value, 0);
	*buffer = write_head;
}

void pack_uint64(uint8_t **buffer, uint64_t value)
{
	uint8_t *write_head = *buffer;

	*write_head++ = GET_BYTE(value, 7);
	*write_head++ = GET_BYTE(value, 6);
	*write_head++ = GET_BYTE(value, 5);
	*write_head++ = GET_BYTE(value, 4);
	*write_head++ = GET_BYTE(value, 3);
	*write_head++ = GET_BYTE(value, 2);
	*write_head++ = GET_BYTE(value, 1);
	*write_head++ = GET_BYTE(value, 0);
	*buffer = write_head;
}

void pack_blob(uint8_t **buffer, size_t length, uint8_t *object)
{
	uint8_t *write_head = *buffer;

	memcpy(write_head, object, length);
	write_head += length;
	*buffer = write_head;
}

uint8_t unpack_uint8(uint8_t **buffer)
{
	uint8_t value = **buffer;

	*buffer += 1;

	return value;
}

uint16_t unpack_uint16(uint8_t **buffer)
{
	uint16_t value = 0;
	uint8_t *read_head = *buffer;

	value |= SET_BYTE(*read_head++, 1);
	value |= SET_BYTE(*read_head++, 0);
	*buffer = read_head;

	return value;
}

uint32_t unpack_uint32(uint8_t **buffer)
{
	uint32_t value = 0;
	uint8_t *read_head = *buffer;

	value |= SET_BYTE((uint32_t)(*read_head++), 3);
	value |= SET_BYTE((uint32_t)(*read_head++), 2);
	value |= SET_BYTE((uint32_t)(*read_head++), 1);
	value |= SET_BYTE((uint32_t)(*read_head++), 0);
	*buffer = read_head;

	return value;
}

uint64_t unpack_uint64(uint8_t **buffer)
{
	uint64_t value = 0;
	uint8_t *read_head = *buffer;

	value |= SET_BYTE((uint64_t)(*read_head++), 7);
	value |= SET_BYTE((uint64_t)(*read_head++), 6);
	value |= SET_BYTE((uint64_t)(*read_head++), 5);
	value |= SET_BYTE((uint64_t)(*read_head++), 4);
	value |= SET_BYTE((uint64_t)(*read_head++), 3);
	value |= SET_BYTE((uint64_t)(*read_head++), 2);
	value |= SET_BYTE((uint64_t)(*read_head++), 1);
	value |= SET_BYTE((uint64_t)(*read_head++), 0);
	*buffer = read_head;

	return value;
}

void unpack_blob(uint8_t **buffer, size_t length, uint8_t *object)
{
	uint8_t *read_head = *buffer;

	memcpy(object, read_head, length);
	read_head += length;
	*buffer = read_head;
}
