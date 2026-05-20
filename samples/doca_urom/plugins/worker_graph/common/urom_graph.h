/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef UROM_GRAPH_H_
#define UROM_GRAPH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Graph command types */
enum urom_worker_graph_cmd_type {
	UROM_WORKER_CMD_GRAPH_LOOPBACK, /* Graph loopback command */
};

/* Graph loopback command structure */
struct urom_worker_graph_cmd_loopback {
	uint64_t data; /* Loopback data */
};

/* UROM Graph worker command structure */
struct urom_worker_graph_cmd {
	uint64_t type; /* Type of command as defined urom_worker_graph_cmd_loopback */
	union {
		struct urom_worker_graph_cmd_loopback loopback; /* Loopback command */
	};
};

/* Graph notification types */
enum urom_worker_graph_notify_type {
	UROM_WORKER_NOTIFY_GRAPH_LOOPBACK, /* Graph loopback notification */
};

/* Graph loopback notification structure */
struct urom_worker_graph_notify_loopback {
	uint64_t data; /* Loopback data */
};

/* UROM Graph worker notification structure */

struct urom_worker_notify_graph {
	uint64_t type; /* Notify type as defined by urom_worker_graph_notify_type */
	union {
		struct urom_worker_graph_notify_loopback loopback; /* Loopback notification */
	};
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
