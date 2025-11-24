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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_DOCA_UTILS_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_DOCA_UTILS_HPP_

#include <string>
#include <vector>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

namespace remote_offload {

/*
 * Find and open a device with the given identifier. May match any one of:
 *  - InfiniBand device name(eg: mlx5_0)
 *  - Network interface name(eg: ens1f0np0)
 *  - PCI address(eg: 03:00.0)
 *
 * @throws remote_offload::runtime_error: If no device matches the given name / identifier OR if opening the device
 * failed
 *
 * @identifier [in]: Identifier to use when selecting a device to use
 * @return: The found and opened device
 */
doca_dev *open_device(std::string const &identifier);

/*
 * Find and open a device representor with the given identifier. May match any one of:
 *  - PCI address(eg: 03:00.0)
 *
 * @throws remote_offload::runtime_error: If no device representor matches the given identifier OR if opening the device
 * representor failed
 *
 * @dev [in]: Device to be represented. Must point to a valid open device
 * @identifier [in]: Identifier to use when selecting a device representor to use
 * @return: The found and opened device representor
 */
doca_dev_rep *open_representor(doca_dev *dev, std::string const &identifier);

/*
 * Stop the context and release tasks
 *
 * @ctx [in]: The context to stop
 * @pe [in]: The progress engine associated with the context
 * @ctx_tasks [in]: The set of tasks that are owned by the given context so they can be freed after they are flushed by
 * the context during the stop process
 * @return: DOCA_SUCCESS or error code upon failure
 */
doca_error_t stop_context(doca_ctx *ctx, doca_pe *pe, std::vector<doca_task *> &ctx_tasks) noexcept;

} /* namespace remote_offload */

#endif /* APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_DOCA_UTILS_HPP_ */
