/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef FLOW_EXTERNAL_ACTION_MUTEX_SAMPLE_H_
#define FLOW_EXTERNAL_ACTION_MUTEX_SAMPLE_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>

#include <flow_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime configuration for the mutex sample, driven by the CLI.
 *
 * The argp user-context points at this struct; @flow_devs is embedded so
 * register_flow_device_params() can recover it via the converter callback.
 */
struct flow_external_action_mutex_cfg {
	struct flow_dev_ctx flow_devs; /**< Device context populated by flow argp params. */
	bool use_mutex;		       /**< Wrap the ASO-memory RMW with LOCK/UNLOCK when true. */
	uint32_t duration_sec;	       /**< RX collection duration in seconds. */
};

/*
 * Sample entry point - runs the mutex/ASO-memory scenario.
 *
 * @nb_queues [in]: number of DPDK rx queues configured for the test.
 * @cfg [in]: parsed CLI configuration.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_external_action_mutex(int nb_queues, const struct flow_external_action_mutex_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* FLOW_EXTERNAL_ACTION_MUTEX_SAMPLE_H_ */
