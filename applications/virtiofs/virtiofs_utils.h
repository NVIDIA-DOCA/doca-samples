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

#ifndef VIRTIOFS_UTILS_H
#define VIRTIOFS_UTILS_H

#ifndef CL_ALIGNED
#define CL_ALIGNED __attribute__((aligned(64)))
#endif

#ifndef LOG2_FLOOR
#define LOG2_FLOOR(x) (sizeof(((size_t)x)) * 8 - __builtin_clzll(((size_t)x)) - 1)
#endif

#ifndef LOG2_CEIL
#define LOG2_CEIL(x) (sizeof(((size_t)x)) * 8 - __builtin_clzll((((size_t)x)) - 1))
#endif

#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
	for ((var) = SLIST_FIRST((head)); (var) && ((tvar) = SLIST_NEXT((var), field), 1); (var) = (tvar))

#ifndef STAILQ_FOREACH_SAFE
#define STAILQ_FOREACH_SAFE(var, head, field, tvar) \
	for ((var) = STAILQ_FIRST((head)); (var) && ((tvar) = STAILQ_NEXT((var), field), 1); (var) = (tvar))
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
	({ \
		_Generic((ptr),										\
			const __typeof__(((const type *)0)->member) * : 					\
				((const type *)(((void const *)(ptr)) - offsetof(type, member))),	\
			default : 									\
				((type *)((void *)(ptr) - offsetof(type, member)))			\
		); \
	})
#endif

#endif /* VIRTIOFS_UTILS_H */