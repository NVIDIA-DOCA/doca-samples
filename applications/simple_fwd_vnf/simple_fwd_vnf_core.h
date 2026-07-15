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

#ifndef SIMPLE_FWD_VNF_CORE_H_
#define SIMPLE_FWD_VNF_CORE_H_

#include <flow_common.h>

#include <dpdk_utils.h>

#include "app_vnf.h"

/* Simple FWD VNF application configuration */
struct simple_fwd_config {
	struct application_dpdk_config *dpdk_cfg; /* DPDK configurations */
	struct flow_dev_ctx flow_devs;		  /* Flow device context */
	uint16_t rx_only; /* Whether or not to work in "receive mode" only, where the application does not send received
			     packets */
	uint16_t hw_offload;  /* Whether or not HW steering is used */
	uint64_t stats_timer; /* The time between periodic stats prints */
	bool is_hairpin;      /* Number of hairpin queues */
	bool age_thread;      /* Whther or not to use a dedicated thread to handle aged flows */
};

/* Simple FWD VNF parameters to be passed when starting processing packets */
struct simple_fwd_process_pkts_params {
	struct simple_fwd_config *cfg; /* Application configuration */
	struct app_vnf *vnf;	       /* Holder for all functions pointers used by the application */
};

/*
 * Registers all flags used bu the users when running the application, such as "aging-thread" flag.
 * This is needed so that the parsing by DOCA argument parser work as expected.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_simple_fwd_params(void);

/*
 * Process received packets, mainly retrieving packet's key, then checking if there is an entry found
 * matching the generated key, in the entries table.
 * If no entry found, the function will create and add new one.
 * In addition, this function handles aging as well
 *
 * @process_pkts_params [in]: an argument containing the mapping  between queues and cores/lcores
 * @return: 0 on success and non-zero value on failure
 *
 * @NOTE: This function is a thread safe
 */
int simple_fwd_process_pkts(void *process_pkts_params);

/*
 * Stops the application from processing further packets
 */
void simple_fwd_process_pkts_stop(void);

/*
 * Maps queues to cores/lcores and vice versa
 *
 * @nb_queues [in]: number of queues to map
 */
void simple_fwd_map_queue(uint16_t nb_queues);

/*
 * Destroys all allocated resources used by the application
 *
 * @vnf [in]: application allocated resources such as ports, pipes and entries.
 */
void simple_fwd_destroy(struct app_vnf *vnf);

#endif /* SIMPLE_FWD_VNF_CORE_H_ */
