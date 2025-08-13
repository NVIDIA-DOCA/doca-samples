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

#ifndef UPF_ACCEL_FLOW_PROCESSING_H_
#define UPF_ACCEL_FLOW_PROCESSING_H_

#include <assert.h>

#include <rte_hash.h>
#include <rte_mbuf.h>

#include "upf_accel.h"

#define UPF_ACCEL_SW_AGING_LL_INVALID_NODE (-1)

struct upf_accel_packet_byte_counter {
	uint64_t pkts;	/* Counter packets */
	uint64_t bytes; /* Counter bytes */
};

struct upf_accel_fp_sw_counters {
	struct upf_accel_packet_byte_counter new_conn; /* New connections */
	struct upf_accel_packet_byte_counter ex_conn;  /* Existing connections */
	struct upf_accel_packet_byte_counter err;      /* Errors */
};

struct upf_accel_fp_accel_counters {
	uint64_t current;      /* Number of currently accelerated flows */
	uint64_t total;	       /* Total number of accelerated flows since app startup */
	uint64_t accel_errors; /* Acceleration errors */
	uint64_t aging_errors; /* Number of failed aging cases */
	uint64_t del_errors;   /* Number of failed delete cases */
};

struct upf_accel_fp_data {
	struct upf_accel_ctx *ctx;						  /* UPF Acceleration context */
	uint16_t queue_id;							  /* Queue id */
	struct rte_hash *dyn_tbl;						  /* Dynamic connection table */
	struct upf_accel_entry_ctx *dyn_tbl_data;				  /* Dynamic connection table data */
	struct rte_hash *dyn_ext_tbl;						  /* Dynamic extension table */
	struct upf_accel_entry_ctx *dyn_ext_tbl_data;				  /* Dynamic extension table data */
	struct upf_accel_fp_sw_counters sw_counters;				  /* SW DP counters */
	struct app_shared_counter_ids quota_cntrs;				  /* Quota counters to handle */
	struct upf_accel_fp_accel_counters accel_counters[PARSER_PKT_TYPE_NUM];	  /* Port acceleration counters */
	struct upf_accel_fp_accel_counters unaccel_counters[PARSER_PKT_TYPE_NUM]; /* Port not accelerated counters */
	struct upf_accel_fp_accel_counters accel_failed_counters[PARSER_PKT_TYPE_NUM]; /* Port acceleration failed
											  counters */
	struct upf_accel_sw_aging_ll sw_aging_ll[PARSER_PKT_TYPE_NUM];		       /* SW Aging linked list */
	uint64_t last_hw_aging_tsc[UPF_ACCEL_PORTS_MAX]; /* Last HW aging iteration timestamp */
	bool hw_aging_in_progress[UPF_ACCEL_PORTS_MAX];	 /* HW Aging in progress, more entries pending */
} __rte_aligned(RTE_CACHE_LINE_SIZE);

/*
 * Entry processing callback
 *
 * @entry [in]: DOCA Flow entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
void upf_accel_check_for_valid_entry_aging(struct doca_flow_pipe_entry *entry,
					   uint16_t pipe_queue,
					   enum doca_flow_entry_status status,
					   enum doca_flow_entry_op op,
					   void *user_ctx);

/*
 * UPF Acceleration flow processing main loop
 *
 * @fp_data [in]: flow processing data
 */
void upf_accel_fp_loop(struct upf_accel_fp_data *fp_data);

#endif /* UPF_ACCEL_FLOW_PROCESSING_H_ */
