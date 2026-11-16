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

/*
 * Shared DOCA Flow Tune helpers for the DOCA Flow samples and applications.
 *
 * A consumer owns how the profile is chosen - its own ARGP parameter and callback, or a fixed
 * profile - so that the profile is stored in whichever configuration struct the consumer already
 * has. These helpers cover the parts that are identical everywhere: setting the profile on the DOCA
 * Flow configuration, and the tune server lifecycle.
 */

#ifndef FLOW_TUNE_COMMON_H_
#define FLOW_TUNE_COMMON_H_

#include <doca_error.h>
#include <doca_flow.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configure a tune profile on a DOCA Flow configuration.
 *
 * Must be called before doca_flow_init(). DOCA_FLOW_TUNE_PROFILE_NONE leaves flow_cfg untouched,
 * so that a consumer that was not asked for tune data never reaches the tune API.
 *
 * @flow_cfg [in]: the DOCA Flow configuration to set the profile on
 * @profile [in]: the profile to collect data for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_tune_configure(struct doca_flow_cfg *flow_cfg, enum doca_flow_tune_profile profile);

/*
 * Start the DOCA Flow Tune server.
 *
 * Must be called after doca_flow_init(). DOCA_FLOW_TUNE_PROFILE_NONE starts nothing, so that a
 * consumer that was not asked for tune data never reaches the tune API.
 *
 * @profile [in]: the profile that was set on the DOCA Flow configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_tune_server_start(enum doca_flow_tune_profile profile);

/*
 * Stop the DOCA Flow Tune server.
 *
 * Must be called before doca_flow_destroy(). Does nothing unless flow_tune_server_start()
 * succeeded, so a consumer can call it on every teardown path without tracking that itself.
 */
void flow_tune_server_stop(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLOW_TUNE_COMMON_H_ */
