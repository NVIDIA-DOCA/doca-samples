/*
 * Copyright (c) 2022-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef DMA_COPY_CORE_H_
#define DMA_COPY_CORE_H_

#include <stdbool.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "comch_utils.h"

#define MAX_ARG_SIZE 128	      /* PCI address and file path maximum length */
#define SERVER_NAME "dma copy server" /* Comm Channel service name */
#define NUM_DMA_TASKS (1)	      /* DMA tasks number */

enum dma_copy_mode {
	DMA_COPY_MODE_HOST, /* Run endpoint in Host */
	DMA_COPY_MODE_DPU   /* Run endpoint in DPU */
};

enum comch_msg_type {
	COMCH_MSG_DIRECTION = 1,	 /* Message type to negotiate file direction */
	COMCH_MSG_EXPORT_DESCRIPTOR = 2, /* Message type to export dma descriptor information */
	COMCH_MSG_STATUS = 3,		 /* Generic success/fail message type */
};

struct comch_msg_dma_direction {
	enum comch_msg_type type; /* COMCH_MSG_DIRECTION */
	bool file_in_host;	  /* Indicate where the source file is located */
	uint64_t file_size;	  /* File size in bytes */
};

struct comch_msg_dma_export_discriptor {
	enum comch_msg_type type; /* COMCH_MSG_EXPORT_DESCRIPTOR */
	uint64_t host_addr;	  /* Address of file on host side */
	size_t export_desc_len;	  /* Length of the exported mmap */
	uint8_t exported_mmap[];  /* Variable sized array containing exported mmap */
};

struct comch_msg_dma_status {
	enum comch_msg_type type; /* COMCH_MSG_STATUS */
	bool is_success;	  /* Indicate success or failure for last message sent */
};

struct comch_msg {
	enum comch_msg_type type; /* Indicator of message type */
	union {
		struct comch_msg_dma_direction dir_msg;		/* COMCH_MSG_DIRECTION type*/
		struct comch_msg_dma_export_discriptor exp_msg; /* COMCH_MSG_EXPORT_DESCRIPTOR type */
		struct comch_msg_dma_status status_msg;		/* COMCH_MSG_STATUS type */
	};
};

enum dma_comch_state {
	COMCH_NEGOTIATING, /* DMA metadata is being negotiated */
	COMCH_COMPLETE,	   /* DMA metadata successfully passed */
	COMCH_ERROR,	   /* An error was detected DMA metadata negotiation */
};

struct dma_copy_cfg {
	enum dma_copy_mode mode;      /* Node running mode {host, dpu} */
	char file_path[MAX_ARG_SIZE]; /* File path to copy from (host) or path the save DMA result (dpu) */
	char cc_dev_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];	  /* Comm Channel DOCA device PCI address */
	char cc_dev_rep_pci_addr[DOCA_DEVINFO_REP_PCI_ADDR_SIZE]; /* Comm Channel DOCA device representor PCI address */
	bool is_file_found_locally;				  /* Indicate DMA copy direction */
	uint64_t file_size;					  /* File size in bytes */
	char *file_buffer;					  /* Buffer to store field to send or file to receive */
	struct doca_mmap *file_mmap;				  /* Mmap associated with the file buffer */
	struct doca_dev *dev;					  /* Doca device used for DMA */
	uint64_t max_dma_buf_size;				  /* Max size DMA supported */

	/* DPU side only field */
	uint8_t *exported_mmap;	  /* Exported mmap sent from host to DPU */
	size_t exported_mmap_len; /* Length of exported mmap */
	uint8_t *host_addr;	  /* Host address of file to be used with exported mmap */

	/* Comch connection info */
	uint32_t max_comch_buffer;	  /* Max buffer size the comch is configure for */
	enum dma_comch_state comch_state; /* Current state of DMA metadata negotiation on the comch */
};

struct dma_copy_resources {
	struct program_core_objects *state; /* DOCA core objects */
	struct doca_dma *dma_ctx;	    /* DOCA DMA context */
};

/*
 * Register application arguments
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_dma_copy_params(void);

/*
 * Open DOCA device for DMA operation
 *
 * @dev [in]: DOCA DMA capable device to open
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_dma_device(struct doca_dev **dev);

/*
 * Start DMA operation on the Host
 *
 * @dma_cfg [in]: App configuration structure
 * @comch_cfg [in]: Doca comch initialized objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t host_start_dma_copy(struct dma_copy_cfg *dma_cfg, struct comch_cfg *comch_cfg);
/*
 * Start DMA operation on the DPU
 *
 * @dma_cfg [in]: App configuration structure
 * @comch_cfg [in]: Doca comch initialized objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpu_start_dma_copy(struct dma_copy_cfg *dma_cfg, struct comch_cfg *comch_cfg);

/*
 * Callback event for client messages
 *
 * @event [in]: message receive event
 * @recv_buffer [in]: array of bytes containing the message data
 * @msg_len [in]: number of bytes in the recv_buffer
 * @comch_connection [in]: comm channel connection over which the event occurred
 */
void host_recv_event_cb(struct doca_comch_event_msg_recv *event,
			uint8_t *recv_buffer,
			uint32_t msg_len,
			struct doca_comch_connection *comch_connection);

/*
 * Callback event for server messages
 *
 * @event [in]: message receive event
 * @recv_buffer [in]: array of bytes containing the message data
 * @msg_len [in]: number of bytes in the recv_buffer
 * @comch_connection [in]: comm channel connection over which the event occurred
 */
void dpu_recv_event_cb(struct doca_comch_event_msg_recv *event,
		       uint8_t *recv_buffer,
		       uint32_t msg_len,
		       struct doca_comch_connection *comch_connection);

#endif /* DMA_COPY_CORE_H_ */
