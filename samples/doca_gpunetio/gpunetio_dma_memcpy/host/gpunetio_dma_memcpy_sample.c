/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <doca_log.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "gpunetio_dma_common.h"

#define PE_POLL_INTERVAL_NS (10 * 1000) /* Sleep between two polls of the progress engine without progress */
#define NS_PER_MSEC 1000000
#define GPU_COPY_TIMEOUT_MS 1000 /* Time given to the copy from the GPU to complete */
#define NUM_TASKS 1
#define NUM_BUFS 2
#define CPU_TO_GPU_TEXT "This is a sample piece of text from CPU"
#define GPU_TO_CPU_TEXT "This is a sample piece of text from GPU"

DOCA_LOG_REGISTER(GPU_DMA_MEMCPY::SAMPLE);

/* Buffer registered with the DOCA device, in host or GPU memory */
struct dma_buffer {
	char *addr;		/* Buffer address */
	bool on_gpu;		/* Buffer in GPU memory, else in host memory */
	int dmabuf_fd;		/* GPU memory dmabuf file descriptor, -1 if not used */
	struct doca_mmap *mmap; /* DOCA mmap registering the buffer with the DOCA device */
};

/* Copy from host memory to GPU memory, run by the CPU with a DOCA DMA memcpy task */
struct cpu_to_gpu_copy {
	struct dma_buffer src;		    /* Source buffer, in host memory */
	struct dma_buffer dst;		    /* Destination buffer, in GPU memory */
	struct doca_buf_inventory *buf_inv; /* Inventory of the task DOCA buffers */
	struct doca_buf *src_doca_buf;	    /* DOCA buffer of the source data */
	struct doca_buf *dst_doca_buf;	    /* DOCA buffer of the destination */
	struct doca_pe *pe;		    /* Progress engine of the DMA context */
	struct doca_dma *dma;		    /* DOCA DMA context, CPU data path */
	bool ctx_started;		    /* DOCA DMA context started */
};

/* Copy from GPU memory to host memory, run by a CUDA kernel with the DOCA GPUNetIO DMA device functions */
struct gpu_to_cpu_copy {
	struct dma_buffer src;			  /* Source buffer, in GPU memory */
	struct dma_buffer dst;			  /* Destination buffer, in host memory */
	struct doca_buf_arr *src_buf_arr;	  /* DOCA buffer array of the source buffer */
	struct doca_buf_arr *dst_buf_arr;	  /* DOCA buffer array of the destination buffer */
	struct doca_gpu_buf_arr *src_gpu_buf_arr; /* GPU handle of the source buffer array */
	struct doca_gpu_buf_arr *dst_gpu_buf_arr; /* GPU handle of the destination buffer array */
	struct doca_dma *dma;			  /* DOCA DMA context, GPU data path */
	struct doca_gpu_dma *dma_gpu;		  /* GPU handle of the DMA context */
	bool ctx_started;			  /* DOCA DMA context started */
};

/*
 * Retrieve host page size
 *
 * @return: host page size
 */
static size_t get_host_page_size(void)
{
	long ret = sysconf(_SC_PAGESIZE);
	if (ret == -1)
		return 4096; // 4KB, default Linux page size
	return (size_t)ret;
}

/*
 * DMA memcpy task common callback, for both completion and error
 *
 * @dma_task [in]: DMA task
 * @task_user_data [in]: Task user data, the flag to raise once the task is done
 * @ctx_user_data [in]: Context user data
 */
static void memcpy_task_common_callback(struct doca_dma_task_memcpy *dma_task,
					union doca_data task_user_data,
					union doca_data ctx_user_data)
{
	bool *task_done = (bool *)task_user_data.ptr;

	(void)dma_task;
	(void)ctx_user_data;

	/* The submitter reads the task status once the task is done */
	*task_done = true;
}

/*
 * Open the DOCA device running the DMA copies
 *
 * @pcie_addr [in]: PCIe address of the DOCA device
 * @ddev [out]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_dma_device(const char *pcie_addr, struct doca_dev **ddev)
{
	doca_error_t result, tmp_result;

	result = open_doca_device_with_pci(pcie_addr, NULL, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device %s: %s", pcie_addr, doca_error_get_descr(result));
		return result;
	}

	/* The DMA engine is on BlueField devices, ConnectX NICs don't have it */
	result = doca_dma_cap_task_memcpy_is_supported(doca_dev_as_devinfo(*ddev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DOCA device %s doesn't support DMA memcpy, a BlueField device is required: %s",
			     pcie_addr,
			     doca_error_get_descr(result));
		tmp_result = doca_dev_close(*ddev);
		if (tmp_result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		*ddev = NULL;
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Allocate a buffer in host or GPU memory, write a text at its start and register it with the DOCA device
 *
 * @ddev [in]: DOCA device
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @name [in]: buffer name, for the logs
 * @size [in]: buffer size, multiple of the host page size
 * @on_gpu [in]: allocate the buffer in GPU memory, else in host memory
 * @text [in]: text to write at the start of the buffer, NULL for none. The rest of the buffer is filled with zeros
 * @buf [in/out]: buffer, with dmabuf_fd set to -1 by the caller. On failure, the caller releases what was created
 * with destroy_dma_buffer()
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_dma_buffer(struct doca_dev *ddev,
				      struct doca_gpu *gpu_dev,
				      const char *name,
				      size_t size,
				      bool on_gpu,
				      const char *text,
				      struct dma_buffer *buf)
{
	cudaError_t res_cuda;
	doca_error_t result;

	buf->on_gpu = on_gpu;

	if (on_gpu) {
		result = doca_gpu_mem_alloc(gpu_dev,
					    size,
					    get_host_page_size(),
					    DOCA_GPU_MEM_TYPE_GPU,
					    (void **)&buf->addr,
					    NULL);
		if (result != DOCA_SUCCESS || buf->addr == NULL) {
			if (result == DOCA_SUCCESS)
				result = DOCA_ERROR_NO_MEMORY;
			DOCA_LOG_ERR("Failed to allocate %s buffer in GPU memory: %s",
				     name,
				     doca_error_get_descr(result));
			return result;
		}

		res_cuda = cudaMemset(buf->addr, 0, size);
		if (res_cuda != cudaSuccess) {
			DOCA_LOG_ERR("Failed to clear %s buffer: %s", name, cudaGetErrorString(res_cuda));
			return DOCA_ERROR_DRIVER;
		}

		if (text != NULL) {
			res_cuda = cudaMemcpy(buf->addr, text, strlen(text) + 1, cudaMemcpyHostToDevice);
			if (res_cuda != cudaSuccess) {
				DOCA_LOG_ERR("Failed to write %s buffer: %s", name, cudaGetErrorString(res_cuda));
				return DOCA_ERROR_DRIVER;
			}
		}

		/*
		 * cudaMemset() and a cudaMemcpy() from pageable memory may return before the GPU memory is written, and
		 * the DMA engine accesses the buffer out of any CUDA stream order: wait for them to complete.
		 */
		res_cuda = cudaDeviceSynchronize();
		if (res_cuda != cudaSuccess) {
			DOCA_LOG_ERR("Function cudaDeviceSynchronize error %s", cudaGetErrorString(res_cuda));
			return DOCA_ERROR_DRIVER;
		}
	} else {
		/* Page aligned like the GPU buffers, for the best DMA performance */
		buf->addr = (char *)aligned_alloc(get_host_page_size(), size);
		if (buf->addr == NULL) {
			DOCA_LOG_ERR("Failed to allocate %s buffer in host memory", name);
			return DOCA_ERROR_NO_MEMORY;
		}

		memset(buf->addr, 0, size);
		if (text != NULL)
			strcpy(buf->addr, text);
	}

	result = doca_mmap_create(&buf->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create %s buffer mmap: %s", name, doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_add_dev(buf->mmap, ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to %s buffer mmap: %s", name, doca_error_get_descr(result));
		return result;
	}

	if (on_gpu) {
		/* Map GPU memory buffer with DMABuf */
		result = doca_gpu_dmabuf_fd(gpu_dev, buf->addr, size, &buf->dmabuf_fd);
		if (result != DOCA_SUCCESS) {
			buf->dmabuf_fd = -1;
			DOCA_LOG_INFO("Mapping %s buffer (%p size %zuB) with nvidia-peermem mode",
				      name,
				      buf->addr,
				      size);

			/* If failed, use nvidia-peermem legacy method */
			result = doca_mmap_set_memrange(buf->mmap, buf->addr, size);
		} else {
			DOCA_LOG_INFO("Mapping %s buffer (%p size %zuB dmabuf fd %d) with dmabuf mode",
				      name,
				      buf->addr,
				      size,
				      buf->dmabuf_fd);

			result = doca_mmap_set_dmabuf_memrange(buf->mmap, buf->dmabuf_fd, buf->addr, 0, size);
		}
	} else {
		result = doca_mmap_set_memrange(buf->mmap, buf->addr, size);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memrange to %s buffer mmap: %s", name, doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(buf->mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start %s buffer mmap: %s", name, doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy a buffer created with create_dma_buffer().
 * Every resource is released even if a previous step fails, the first error is returned.
 *
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @buf [in]: buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_dma_buffer(struct doca_gpu *gpu_dev, struct dma_buffer *buf)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	/* The mmap registers the buffer with the DOCA device: destroy it before releasing the memory */
	if (buf->mmap != NULL) {
		tmp_result = doca_mmap_destroy(buf->mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		buf->mmap = NULL;
	}

	if (buf->dmabuf_fd >= 0) {
		close(buf->dmabuf_fd);
		buf->dmabuf_fd = -1;
	}

	if (buf->addr != NULL) {
		if (buf->on_gpu) {
			tmp_result = doca_gpu_mem_free(gpu_dev, buf->addr);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(tmp_result));
				DOCA_ERROR_PROPAGATE(result, tmp_result);
			}
		} else {
			free(buf->addr);
		}
		buf->addr = NULL;
	}

	return result;
}

/*
 * Create a DOCA buffer array of one buffer and export it to the GPU
 *
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @buf [in]: buffer
 * @size [in]: buffer size
 * @buf_arr [out]: DOCA buffer array. On failure, the caller destroys it if not NULL
 * @gpu_buf_arr [out]: GPU handle of the DOCA buffer array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_gpu_buf_arr(struct doca_gpu *gpu_dev,
				       const struct dma_buffer *buf,
				       size_t size,
				       struct doca_buf_arr **buf_arr,
				       struct doca_gpu_buf_arr **gpu_buf_arr)
{
	doca_error_t result;

	result = doca_buf_arr_create(1, buf_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create buffer array: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_arr_set_target_gpu(*buf_arr, gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set buffer array target GPU: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_arr_set_params(*buf_arr, buf->mmap, size, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set buffer array params: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_arr_start(*buf_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start buffer array: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_arr_get_gpu_handle(*buf_arr, gpu_buf_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get buffer array GPU handle: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Check whether a buffer starts with a text, terminating null byte included
 *
 * @buffer [in]: buffer, read through a volatile pointer as the DMA engine writes it behind the compiler's back
 * @text [in]: text
 * @return: true if the buffer starts with the text, false otherwise
 */
static bool buffer_holds_text(const volatile char *buffer, const char *text)
{
	size_t idx = 0;

	do {
		if (buffer[idx] != text[idx])
			return false;
	} while (text[idx++] != '\0');

	return true;
}

/*
 * Wait for a DMA copy, which reports no completion, to write a text in a host buffer
 *
 * @buffer [in]: host buffer
 * @text [in]: text expected in the buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_TIME_OUT if the text didn't arrive in time
 */
static doca_error_t wait_for_text(const char *buffer, const char *text)
{
	const struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = NS_PER_MSEC};
	uint32_t elapsed_ms;

	/* Wait for the whole text: the DMA engine doesn't guarantee the order in which the bytes of a copy land */
	for (elapsed_ms = 0; !buffer_holds_text(buffer, text); elapsed_ms++) {
		if (elapsed_ms == GPU_COPY_TIMEOUT_MS) {
			DOCA_LOG_ERR("The DMA copy from the GPU didn't complete within %d ms", GPU_COPY_TIMEOUT_MS);
			return DOCA_ERROR_TIME_OUT;
		}
		nanosleep(&poll_interval, NULL);
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy the objects of the copy from host memory to GPU memory.
 * Every resource is released even if a previous step fails, the first error is returned.
 *
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @copy [in]: copy objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_cpu_to_gpu_copy(struct doca_gpu *gpu_dev, struct cpu_to_gpu_copy *copy)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	/*
	 * A context that was never started can't be stopped. A task error moves the context to the stopping state:
	 * the progress engine brings it back to idle.
	 */
	if (copy->ctx_started) {
		tmp_result = request_stop_ctx(copy->pe, doca_dma_as_ctx(copy->dma));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		copy->ctx_started = false;
	}

	if (copy->dma != NULL) {
		tmp_result = doca_dma_destroy(copy->dma);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DMA context: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->dma = NULL;
	}

	if (copy->pe != NULL) {
		tmp_result = doca_pe_destroy(copy->pe);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->pe = NULL;
	}

	if (copy->dst_doca_buf != NULL) {
		tmp_result = doca_buf_dec_refcount(copy->dst_doca_buf, NULL);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to decrease DOCA dst buffer reference count: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->dst_doca_buf = NULL;
	}

	if (copy->src_doca_buf != NULL) {
		tmp_result = doca_buf_dec_refcount(copy->src_doca_buf, NULL);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to decrease DOCA src buffer reference count: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->src_doca_buf = NULL;
	}

	if (copy->buf_inv != NULL) {
		tmp_result = doca_buf_inventory_destroy(copy->buf_inv);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy buffer inventory: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->buf_inv = NULL;
	}

	tmp_result = destroy_dma_buffer(gpu_dev, &copy->dst);
	DOCA_ERROR_PROPAGATE(result, tmp_result);

	tmp_result = destroy_dma_buffer(gpu_dev, &copy->src);
	DOCA_ERROR_PROPAGATE(result, tmp_result);

	return result;
}

/*
 * Create the objects of the copy from host memory to GPU memory
 *
 * @ddev [in]: DOCA device
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @buf_size [in]: size of the source and destination buffers
 * @copy [in/out]: copy objects. On failure, the caller releases what was created with destroy_cpu_to_gpu_copy()
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_cpu_to_gpu_copy(struct doca_dev *ddev,
					   struct doca_gpu *gpu_dev,
					   size_t buf_size,
					   struct cpu_to_gpu_copy *copy)
{
	doca_error_t result;

	result = create_dma_buffer(ddev, gpu_dev, "CPU source", buf_size, false, CPU_TO_GPU_TEXT, &copy->src);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("The CPU source buffer value to be copied to GPU memory: %s", copy->src.addr);

	result = create_dma_buffer(ddev, gpu_dev, "GPU destination", buf_size, true, NULL, &copy->dst);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_buf_inventory_create(NUM_BUFS, &copy->buf_inv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create buffer inventory: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_inventory_start(copy->buf_inv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start buffer inventory: %s", doca_error_get_descr(result));
		return result;
	}

	/* The task copies the DMA_MEMCPY_SIZE bytes of source data to the empty destination buffer */
	result = doca_buf_inventory_buf_get_by_data(copy->buf_inv,
						    copy->src.mmap,
						    copy->src.addr,
						    DMA_MEMCPY_SIZE,
						    &copy->src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to acquire DOCA buffer representing src buffer: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_inventory_buf_get_by_addr(copy->buf_inv,
						    copy->dst.mmap,
						    copy->dst.addr,
						    buf_size,
						    &copy->dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to acquire DOCA buffer representing dst buffer: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_create(&copy->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_dma_create(ddev, &copy->dma);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DMA context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(copy->pe, doca_dma_as_ctx(copy->dma));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect DMA context to progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_dma_task_memcpy_set_conf(copy->dma,
					       memcpy_task_common_callback,
					       memcpy_task_common_callback,
					       NUM_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure DMA memcpy task: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(doca_dma_as_ctx(copy->dma));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DMA context: %s", doca_error_get_descr(result));
		return result;
	}
	copy->ctx_started = true;

	return DOCA_SUCCESS;
}

/*
 * Copy the host source buffer to the GPU destination buffer with a DOCA DMA memcpy task
 *
 * @copy [in]: copy objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_cpu_to_gpu_copy(struct cpu_to_gpu_copy *copy)
{
	const struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = PE_POLL_INTERVAL_NS};
	struct doca_dma_task_memcpy *memcpy_task;
	union doca_data task_user_data = {0};
	struct doca_task *task;
	bool task_done = false;
	cudaError_t res_cuda;
	doca_error_t result;

	task_user_data.ptr = &task_done;

	result = doca_dma_task_memcpy_alloc_init(copy->dma,
						 copy->src_doca_buf,
						 copy->dst_doca_buf,
						 task_user_data,
						 &memcpy_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA memcpy task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_dma_task_memcpy_as_task(memcpy_task);
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit DMA memcpy task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		return result;
	}

	while (!task_done) {
		if (doca_pe_progress(copy->pe) == 0)
			nanosleep(&poll_interval, NULL);
	}

	result = doca_task_get_status(task);
	doca_task_free(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DMA memcpy task failed: %s", doca_error_get_descr(result));
		return result;
	}

	/* Make the DMA engine writes to GPU memory visible to the CUDA kernel which reads the buffer */
	res_cuda = cudaDeviceFlushGPUDirectRDMAWrites(cudaFlushGPUDirectRDMAWritesTargetCurrentDevice,
						      cudaFlushGPUDirectRDMAWritesToOwner);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Failed to flush GPUDirect RDMA writes: %s", cudaGetErrorString(res_cuda));
		return DOCA_ERROR_DRIVER;
	}

	DOCA_LOG_INFO("Success, DMA memcpy job done successfully");

	return DOCA_SUCCESS;
}

/*
 * Destroy the objects of the copy from GPU memory to host memory.
 * Every resource is released even if a previous step fails, the first error is returned.
 *
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @copy [in]: copy objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_gpu_to_cpu_copy(struct doca_gpu *gpu_dev, struct gpu_to_cpu_copy *copy)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	/* A context that was never started can't be stopped */
	if (copy->ctx_started) {
		tmp_result = doca_ctx_stop(doca_dma_as_ctx(copy->dma));
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DMA context: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->ctx_started = false;
	}

	if (copy->dma != NULL) {
		tmp_result = doca_dma_destroy(copy->dma);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DMA context: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->dma = NULL;
		copy->dma_gpu = NULL;
	}

	if (copy->dst_buf_arr != NULL) {
		tmp_result = doca_buf_arr_destroy(copy->dst_buf_arr);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy dst buffer array: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->dst_buf_arr = NULL;
		copy->dst_gpu_buf_arr = NULL;
	}

	if (copy->src_buf_arr != NULL) {
		tmp_result = doca_buf_arr_destroy(copy->src_buf_arr);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy src buffer array: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		copy->src_buf_arr = NULL;
		copy->src_gpu_buf_arr = NULL;
	}

	tmp_result = destroy_dma_buffer(gpu_dev, &copy->dst);
	DOCA_ERROR_PROPAGATE(result, tmp_result);

	tmp_result = destroy_dma_buffer(gpu_dev, &copy->src);
	DOCA_ERROR_PROPAGATE(result, tmp_result);

	return result;
}

/*
 * Create the objects of the copy from GPU memory to host memory
 *
 * @ddev [in]: DOCA device
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @buf_size [in]: size of the source and destination buffers
 * @copy [in/out]: copy objects. On failure, the caller releases what was created with destroy_gpu_to_cpu_copy()
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_gpu_to_cpu_copy(struct doca_dev *ddev,
					   struct doca_gpu *gpu_dev,
					   size_t buf_size,
					   struct gpu_to_cpu_copy *copy)
{
	doca_error_t result;

	result = create_dma_buffer(ddev, gpu_dev, "GPU source", buf_size, true, GPU_TO_CPU_TEXT, &copy->src);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("The GPU source buffer value to be copied to CPU memory: %s", GPU_TO_CPU_TEXT);

	result = create_dma_buffer(ddev, gpu_dev, "CPU destination", buf_size, false, NULL, &copy->dst);
	if (result != DOCA_SUCCESS)
		return result;

	result = create_gpu_buf_arr(gpu_dev, &copy->src, buf_size, &copy->src_buf_arr, &copy->src_gpu_buf_arr);
	if (result != DOCA_SUCCESS)
		return result;

	result = create_gpu_buf_arr(gpu_dev, &copy->dst, buf_size, &copy->dst_buf_arr, &copy->dst_gpu_buf_arr);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dma_create(ddev, &copy->dma);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DMA context: %s", doca_error_get_descr(result));
		return result;
	}

	/* The copies of this context are posted by a CUDA kernel */
	result = doca_ctx_set_datapath_on_gpu(doca_dma_as_ctx(copy->dma), gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set datapath on GPU: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(doca_dma_as_ctx(copy->dma));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DMA context: %s", doca_error_get_descr(result));
		return result;
	}
	copy->ctx_started = true;

	result = doca_dma_get_gpu_handle(copy->dma, &copy->dma_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DMA GPU handle: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Launch the CUDA kernel which prints the GPU buffer filled by the CPU copy, then copies the GPU source buffer to the
 * host destination buffer
 *
 * @copy [in]: copy objects
 * @gpu_buffer [in]: GPU buffer filled by the CPU copy
 * @stream [in]: CUDA stream to launch the kernel
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_gpu_to_cpu_copy(struct gpu_to_cpu_copy *copy, const char *gpu_buffer, cudaStream_t stream)
{
	cudaError_t res_cuda;
	doca_error_t result;

	result = gpunetio_dma_memcpy_common_launch_kernel(stream,
							  gpu_buffer,
							  copy->dma_gpu,
							  copy->src_gpu_buf_arr,
							  copy->dst_gpu_buf_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function gpunetio_dma_memcpy_common_launch_kernel returned %s",
			     doca_error_get_descr(result));
		return result;
	}

	res_cuda = cudaStreamSynchronize(stream);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamSynchronize error %s", cudaGetErrorString(res_cuda));
		return DOCA_ERROR_DRIVER;
	}

	/* The kernel returns once the copy is posted, without waiting for it */
	result = wait_for_text(copy->dst.addr, GPU_TO_CPU_TEXT);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("CPU received message from GPU: %s", copy->dst.addr);

	return DOCA_SUCCESS;
}

/*
 * GPU DMA Memcpy sample
 *
 * @cfg [in]: Sample config parameters
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t gpunetio_dma_memcpy(struct gpu_dma_config *cfg)
{
	struct cpu_to_gpu_copy cpu_to_gpu = {.src.dmabuf_fd = -1, .dst.dmabuf_fd = -1};
	struct gpu_to_cpu_copy gpu_to_cpu = {.src.dmabuf_fd = -1, .dst.dmabuf_fd = -1};
	const size_t page_size = get_host_page_size();
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	cudaStream_t stream = NULL;
	doca_error_t result, tmp_result;
	cudaError_t res_cuda;
	size_t buf_size;

	/* GPU memory registered with dmabuf must span whole host pages */
	buf_size = ((DMA_MEMCPY_SIZE + page_size - 1) / page_size) * page_size;

	result = open_dma_device(cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_gpu_create(cfg->gpu_pcie_addr, &gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = create_cpu_to_gpu_copy(ddev, gpu_dev, buf_size, &cpu_to_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_cpu_to_gpu_copy returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = create_gpu_to_cpu_copy(ddev, gpu_dev, buf_size, &gpu_to_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_gpu_to_cpu_copy returned %s", doca_error_get_descr(result));
		goto exit;
	}

	res_cuda = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
	if (res_cuda != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %s", cudaGetErrorString(res_cuda));
		result = DOCA_ERROR_DRIVER;
		goto exit;
	}

	result = run_cpu_to_gpu_copy(&cpu_to_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function run_cpu_to_gpu_copy returned %s", doca_error_get_descr(result));
		goto exit;
	}

	result = run_gpu_to_cpu_copy(&gpu_to_cpu, cpu_to_gpu.dst.addr, stream);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function run_gpu_to_cpu_copy returned %s", doca_error_get_descr(result));
		goto exit;
	}

exit:
	if (stream != NULL)
		cudaStreamDestroy(stream);

	tmp_result = destroy_gpu_to_cpu_copy(gpu_dev, &gpu_to_cpu);
	DOCA_ERROR_PROPAGATE(result, tmp_result);

	tmp_result = destroy_cpu_to_gpu_copy(gpu_dev, &cpu_to_gpu);
	DOCA_ERROR_PROPAGATE(result, tmp_result);

	if (gpu_dev != NULL) {
		tmp_result = doca_gpu_destroy(gpu_dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_gpu_destroy returned %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	tmp_result = doca_dev_close(ddev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dev_close returned %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}
