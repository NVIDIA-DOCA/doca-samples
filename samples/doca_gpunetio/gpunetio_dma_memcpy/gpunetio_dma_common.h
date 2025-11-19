/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef GPUNETIO_DMA_COMMON_H_
#define GPUNETIO_DMA_COMMON_H_

#include <stdio.h>
#include <stdint.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_dev.h>
#include <doca_gpunetio.h>
#include <doca_error.h>
#include <doca_buf_array.h>
#include <doca_dma.h>

#include "common.h"

#define DMA_MEMCPY_SIZE 1024
#define ALIGN_SIZE(size, align) size = ((size + (align)-1) / (align)) * (align);

/* Sample configuration structure */
struct gpu_dma_config {
	char nic_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* NIC PCIe address */
	char gpu_pcie_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* GPU PCIe address */
};

/*
 * GPUNetIO DMA Memcpy sample functionality
 *
 * @gpu_dma_cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_dma_memcpy(struct gpu_dma_config *gpu_dma_cfg);

#if __cplusplus
extern "C" {
#endif

/*
 * Launch a CUDA kernel to read the GPU destination buffer.
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @gpu_buffer_addr [in]: GPU destination buffer address
 * @dma_gpu [in]: DMA GPU exported object
 * @src_gpu_buf_arr [in]: DOCA GPU src buffer array for DMA copy
 * @dst_gpu_buf_arr [in]: DOCA GPU dst buffer array for DMA copy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_dma_memcpy_common_launch_kernel(cudaStream_t stream,
						      uintptr_t gpu_buffer_addr,
						      struct doca_gpu_dma *dma_gpu,
						      struct doca_gpu_buf_arr *src_gpu_buf_arr,
						      struct doca_gpu_buf_arr *dst_gpu_buf_arr);

#if __cplusplus
}
#endif

#endif /* GPUNETIO_DMA_COMMON_H_ */
