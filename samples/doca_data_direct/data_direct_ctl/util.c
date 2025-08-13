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

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "util.h"

int util_log_level = UTIL_LOG_LEVEL_INFO;

__attribute__((format(printf, 2, 3)))
void util_log(enum util_log_level log_level, const char *format, ...)
{
	va_list args;
	int tmp_errno;

	if (util_log_level < log_level)
		return;

	tmp_errno = errno;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	errno = tmp_errno;
}

#define BDF_SIZE sizeof("0000:00:00.0")
#define BDF_LEN (BDF_SIZE - 1)
bool util_is_bdf_format(const char *str)
{
	return strlen(str) == BDF_LEN && isxdigit(str[0]) && isxdigit(str[1]) &&
	       isxdigit(str[2]) && isxdigit(str[3]) && str[4] == ':' &&
	       isxdigit(str[5]) && isxdigit(str[6]) && str[7] == ':' &&
	       isxdigit(str[8]) && isxdigit(str[9]) && str[10] == '.' &&
	       isdigit(str[11]);
}
