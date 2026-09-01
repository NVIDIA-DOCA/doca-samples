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

#ifndef UROM_SANDBOX_H_
#define UROM_SANDBOX_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "ucp/api/ucp.h"

/* Sandbox serializing next raw, iter points to the offset place and returns the buffer start */
#define urom_sandbox_serialize_next_raw(_iter, _type, _offset) \
	({ \
		_type *_result = (_type *)(*(_iter)); \
		*(_iter) = UCS_PTR_BYTE_OFFSET(*(_iter), _offset); \
		_result; \
	})

/* Sandbox command types */
enum urom_worker_sandbox_cmd_type {
	UROM_WORKER_CMD_SANDBOX_MEM_MAP,  /* Sandbox memory map command */
	UROM_WORKER_CMD_SANDBOX_TAG_SEND, /* Sandbox tag send command */
	UROM_WORKER_CMD_SANDBOX_TAG_RECV, /* Sandbox tag recv command */
};

/* Sandbox memory map command structure */
struct urom_worker_sandbox_cmd_mem_map {
	uint64_t context;		 /* User context returned in notify */
	ucp_mem_map_params_t map_params; /* UCX memory map params */
	size_t exported_memh_buffer_len; /* Mapped memory handle length */
};

/* Sandbox tag send command structure */
struct urom_worker_sandbox_cmd_tag_send {
	uint64_t context; /* User context returned in notify */
	uint64_t dest;	  /* Domain id of destination */
	uint64_t buffer;  /* Buffer address */
	uint64_t count;	  /* Buffer bytes count */
	uint64_t tag;	  /* Buffer UCX tag send */
	uint64_t memh_id; /* Buffer memh id, 0 if inline send */
};

/* Sandbox tag recv command structure */
struct urom_worker_sandbox_cmd_tag_recv {
	uint64_t context;  /* User context returned in notify */
	uint64_t buffer;   /* Buffer address */
	uint64_t count;	   /* Buffer bytes count */
	uint64_t tag;	   /* Buffer UCX tag recv */
	uint64_t tag_mask; /* Buffer UCX tag recv mask */
	uint64_t memh_id;  /* Buffer memh id, 0 if inline recv */
};

/* UROM sandbox worker command structure */
struct urom_worker_sandbox_cmd {
	uint64_t type; /* Type of command as defined urom_worker_sandbox_cmd_type */
	union {
		struct urom_worker_sandbox_cmd_mem_map mem_map;	  /* Memory map command */
		struct urom_worker_sandbox_cmd_tag_send tag_send; /* Tag send command */
		struct urom_worker_sandbox_cmd_tag_recv tag_recv; /* Tag recv command */
	};
};

/* Sandbox notification types */
enum urom_worker_sandbox_notify_type {
	UROM_WORKER_NOTIFY_SANDBOX_MEM_MAP,  /* Sandbox memory map notification */
	UROM_WORKER_NOTIFY_SANDBOX_TAG_SEND, /* Sandbox tag send notification */
	UROM_WORKER_NOTIFY_SANDBOX_TAG_RECV, /* Sandbox tag recv notification */
};

/* Sandbox memory map notification structure */
struct urom_worker_sandbox_notify_mem_map {
	uint64_t context; /* User context from map command */
	uint64_t memh_id; /* Buffer memh id */
};

/* Sandbox tag send notification structure */
struct urom_worker_sandbox_notify_tag_send {
	uint64_t context;    /* User context from send command */
	ucs_status_t status; /* Tag send command execution status */
};

/* Sandbox tag recv notification structure */
struct urom_worker_sandbox_notify_tag_recv {
	uint64_t context;    /* User context from recv command */
	void *buffer;	     /* Inline receive data, NULL if RDMA */
	uint64_t count;	     /* Received data bytes count */
	uint64_t sender_tag; /* Sender tag */
	ucs_status_t status; /* Tag recv command execution status */
};

/* UROM sandbox worker notification structure */
struct urom_worker_notify_sandbox {
	uint64_t type; /* Notify type as defined by urom_worker_sandbox_notify_type */
	union {
		struct urom_worker_sandbox_notify_mem_map mem_map;   /* Memory map notification */
		struct urom_worker_sandbox_notify_tag_send tag_send; /* Tag send notification */
		struct urom_worker_sandbox_notify_tag_recv tag_recv; /* Tag recv notification */
	};
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
