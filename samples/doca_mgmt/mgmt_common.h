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

#ifndef MGMT_COMMON_H_
#define MGMT_COMMON_H_

#include <stdbool.h>

#include <doca_argp.h>
#include <doca_error.h>

/*
 * Maximum size of a management target string.
 */
#define MGMT_COMMON_TARGET_STR_LEN 512

/*
 * Enumerate the management targets on the system and print them.
 *
 * Each device management target string is printed. When @include_reps is true, each device's representor target strings
 * are printed nested beneath it. By default only the target strings are printed; if the "--verbose" flag registered by
 * mgmt_common_register_list_targets_cmd() was passed, every attribute of each target is printed, one per row.
 *
 * The listing is best-effort: a failure to enumerate a specific device's representors is reported inline and does not
 * abort the whole listing.
 *
 * @include_reps [in]: whether to also list each device's representor targets
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_common_list_targets(bool include_reps);

/*
 * Register a top-level "list-targets" command.
 *
 * Registers a "list-targets" command with the given callback, which should record the sample's own "list targets"
 * command enum in its configuration. This function also registers the command's "-v/--verbose" boolean flag, which is
 * handled internally, so the sample only needs to provide the command callback and later call
 * mgmt_common_list_targets() when the command is selected.
 *
 * @callback [in]: the command callback to invoke when "list-targets" is selected
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_common_register_list_targets_cmd(doca_argp_cmd_cb_t callback);

/*
 * Create and register a device management target string parameter ("-d/--device") for a command.
 *
 * @cmd [in]: the command to register the parameter for
 * @callback [in]: the parameter's callback, which should copy the target string into the sample's configuration
 *                 (typically via mgmt_common_target_string_copy())
 * @mandatory [in]: whether the parameter is mandatory
 * @description [in]: optional description override; if NULL a standard description is used. If provided, it must have
 *                    static storage duration, as ARGP stores the pointer and does not copy the string.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_common_register_target_dev_param(struct doca_argp_cmd *cmd,
						   doca_argp_param_cb_t callback,
						   bool mandatory,
						   const char *description);

/*
 * Create and register a device representor management target string parameter ("-r/--rep") for a command.
 *
 * @cmd [in]: the command to register the parameter for
 * @callback [in]: the parameter's callback, which should copy the target string into the sample's configuration
 *                 (typically via mgmt_common_target_string_copy())
 * @mandatory [in]: whether the parameter is mandatory
 * @description [in]: optional description override; if NULL a standard description is used. If provided, it must have
 *                    static storage duration, as ARGP stores the pointer and does not copy the string.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_common_register_target_dev_rep_param(struct doca_argp_cmd *cmd,
						       doca_argp_param_cb_t callback,
						       bool mandatory,
						       const char *description);

/*
 * Validate a management target string and copy it into a destination buffer.
 *
 * Intended to be used from a sample's target string ARGP callback to store the value into the sample's configuration.
 *
 * @target_string [in]: the target string to validate and copy
 * @dst [out]: destination buffer, must be at least MGMT_COMMON_TARGET_STR_LEN bytes long
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if the target string is too long
 */
doca_error_t mgmt_common_target_string_copy(const char *target_string, char *dst);

#endif /* MGMT_COMMON_H_ */
