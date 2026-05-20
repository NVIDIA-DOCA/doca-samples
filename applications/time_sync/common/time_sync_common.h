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

#ifndef TIME_SYNC_COMMON_H_
#define TIME_SYNC_COMMON_H_

#include <doca_clock.h>
#include <doca_comch.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_pe.h>

#define COMCH_NAME "time_sync_conn"
#define COMCH_NUM_TASKS 1

/* Function definition to check if capabilities are supported by a given device */
typedef doca_error_t (*caps_check)(struct doca_devinfo *);

/* Format of message to request time information */
struct time_sync_request {
	uint32_t delay; /* Delay in msecs to add between DPU events */
};

/* Format of response to time_sync_request */
struct time_sync_response {
	union doca_clock_timespec_t dpa_event_time;	/* Event time on DPA kernel */
	union doca_clock_timespec_t dpu_event_time;	/* Local event time on DPU cores */
	union doca_clock_timespec_t dpu_event_time_nic; /* NIC real-time of DPU event*/
	uint64_t dpu_event_error_margin;		/* Nanosecond margin for error in DPU and NIC time */
};

struct time_sync_cfg {
	char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];	/* Input pci address */
	char repr_addr[DOCA_DEVINFO_REP_PCI_ADDR_SIZE]; /* Input pci repr addr - DPU side only */
	struct doca_dev *doca_dev;			/* DOCA device opened for use in DOCA context */
	struct doca_dev_rep *repr_dev;	    /* DOCA device representor opened for use in DOCA comch server context */
	struct doca_clock *clock;	    /* DOCA clock context */
	struct doca_dpa *dpa_ctx;	    /* DOCA dpa context for handling time_sync dpa app */
	struct doca_pe *pe;		    /* Progress engine for use with comch */
	struct doca_comch_server *server;   /* Comch server to run on the DPU */
	struct doca_comch_client *client;   /* Comch client to run on the host */
	struct doca_ctx *comch_ctx;	    /* Comch context as DOCA context */
	struct doca_comch_connection *conn; /* Comch established connection between host and DPU */
	struct time_sync_response dpu_resp; /* Event timings recorded on DPU and passed to host */
	doca_error_t error;		    /* Error detected in datapath */
	uint32_t delay;			    /* Delay in msecs to add between events */
	uint8_t finished;		    /* Indication that all expected work has been completed successfully */
};

/*
 * Configure app arg parser with common parameters
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t time_sync_common_reg_params(void);

/*
 * Open a DOCA device for time_sync if capabilities are supported
 *
 * @ts_cfg [in/out]: contains PCIe address of device on input and opened device pointer on success
 * @cap_func [in]: pointer to a function that checks if the device has some task capabilities (Ignored if set to NULL)
 * @return: DOCA_SUCCESS with opened device on success and DOCA_ERROR otherwise
 */
doca_error_t time_sync_common_open_dev_with_caps(struct time_sync_cfg *ts_cfg, caps_check cap_func);

/*
 * Open a DOCA device repr for time_sync - assumes DOCA device is already processed
 *
 * @ts_cfg [in/out]: contains PCIe address of repr device on input and opened device pointer on success
 * @return: DOCA_SUCCESS with opened device on success and DOCA_ERROR otherwise
 */
doca_error_t time_sync_common_open_repr(struct time_sync_cfg *ts_cfg);

/*
 * Close any DOCA device or repr device that are open
 *
 * @ts_cfg[in]: config file containing pointers to devices
 * @return: DOCA_SUCCESS with opened device on success and DOCA_ERROR otherwise
 */
doca_error_t time_sync_common_close_devs(struct time_sync_cfg *ts_cfg);

/*
 * Create a DOCA clock context based on configured device
 *
 * @ts_cfg[in/out]: pointer to doca device for clock and new created context on success
 * @return: DOCA_SUCCESS with opened device on success and DOCA_ERROR otherwise
 */
doca_error_t time_sync_common_create_clock(struct time_sync_cfg *ts_cfg);

/*
 * Destroy clock context
 *
 * @ts_cfg[in]: config file containing clock context
 * @return: DOCA_SUCCESS with opened device on success and DOCA_ERROR otherwise
 */
doca_error_t time_sync_common_destroy_clock(struct time_sync_cfg *ts_cfg);

#endif /* TIME_SYNC_COMMON_H_ */
