/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdlib.h>
#include <doca_log.h>

#include <doca_gpunetio_dev_buf.cuh>
#include <doca_gpunetio_dev_dma.cuh>

#include "gpunetio_dma_common.h"

DOCA_LOG_REGISTER(GPU_DMA_MEMCPY::KERNEL);

__global__ void cuda_kernel_print_gpu_buffer(uintptr_t gpu_buffer_addr,
					     struct doca_gpu_dma *dma_gpu,
					     struct doca_gpu_buf_arr *src_gpu_buf_arr,
					     struct doca_gpu_buf_arr *dst_gpu_buf_arr)
{
	doca_error_t result;
	struct doca_gpu_buf *src_buf;
	struct doca_gpu_buf *dst_buf;

	printf("CUDA KERNEL INFO: The GPU destination buffer value after the memcpy: %s \n", (char *)gpu_buffer_addr);

	doca_gpu_dev_buf_get_buf(src_gpu_buf_arr, threadIdx.x, &src_buf);
	doca_gpu_dev_buf_get_buf(dst_gpu_buf_arr, threadIdx.x, &dst_buf);
	doca_gpu_dev_dma_memcpy(dma_gpu, src_buf, 0, dst_buf, 0, DMA_MEMCPY_SIZE);
	/* Not really needed with only 1 CUDA thread */
	__syncthreads();

	if (threadIdx.x == 0) {
		result = doca_gpu_dev_dma_commit(dma_gpu);
		if (result != DOCA_SUCCESS) {
			printf("Error %d doca_gpu_dev_dma_commit\n", result);
			return;
		}
	}
}

extern "C" {

doca_error_t gpunetio_dma_memcpy_common_launch_kernel(cudaStream_t stream,
						      uintptr_t gpu_buffer_addr,
						      struct doca_gpu_dma *dma_gpu,
						      struct doca_gpu_buf_arr *src_gpu_buf_arr,
						      struct doca_gpu_buf_arr *dst_gpu_buf_arr)
{
	cudaError_t result = cudaSuccess;

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	cuda_kernel_print_gpu_buffer<<<1, 1, 0, stream>>>(gpu_buffer_addr, dma_gpu, src_gpu_buf_arr, dst_gpu_buf_arr);

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
