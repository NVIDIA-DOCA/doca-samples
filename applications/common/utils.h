/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef COMMON_UTILS_H_
#define COMMON_UTILS_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_types.h>

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y)) /* Return the minimum value between X and Y */
#endif

#ifndef MAX
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y)) /* Return the maximum value between X and Y */
#endif

/*
 * Prints DOCA SDK and runtime versions
 *
 * @param [in]: unused
 * @doca_config [in]: unused
 * @return: the function exit with EXIT_SUCCESS
 */
doca_error_t sdk_version_callback(void *param, void *doca_config);

/*
 * Read the entire content of a file into a buffer
 *
 * @path [in]: file path
 * @out_bytes [out]: file data buffer
 * @out_bytes_len [out]: file length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t read_file(char const *path, char **out_bytes, size_t *out_bytes_len);

/*
 * Init a uint16_t array with linear number start from zero
 *
 * @array [in]: pointer to array to init
 * @n [in]: number of element to init
 */
void linear_array_init_u16(uint16_t *array, uint16_t n);

#ifdef DOCA_USE_LIBBSD

#include <bsd/string.h>

#elif !defined(DOCA_USE_LIBC_STRING_L_FUNC)
/*
 * This method wraps our implementation of strlcpy when libbsd is missing
 * @dst [in]: destination string
 * @src [in]: source string
 * @size [in]: size, in bytes, of the destination buffer
 * @return: total length of the string (src) we tried to create
 */
size_t strlcpy(char *dst, const char *src, size_t size);

/*
 * This method wraps our implementation of strlcat when libbsd is missing
 * @dst [in]: destination string
 * @src [in]: source string
 * @size [in]: size, in bytes, of the destination buffer
 * @return: total length of the string (src) we tried to create
 */
size_t strlcat(char *dst, const char *src, size_t size);

#endif /* DOCA_USE_STRING_L_FUNC */

#endif /* COMMON_UTILS_H_ */
