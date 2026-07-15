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

#include <unistd.h>
#include <time.h>
#include <stdbool.h>

#include <doca_log.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "gpunetio_dma_common.h"

#define SLEEP_IN_NANOS (10 * 1000)
#define NUM_TASKS 1
#define NUM_BUFS 2
#define DEFAULT_VALUE 0

/* Global flag indicating the task status */
static uint8_t is_task_done = 0;

DOCA_LOG_REGISTER(GPU_DMA_MEMCPY::SAMPLE);

/*
 * Sample objects
 */
struct gpu_dma_sample_objects {
	struct program_core_objects core_objs;	       /* Core objects */
	struct doca_dma *dma;			       /* DOCA DMA instance */
	struct doca_gpu_dma *dma_gpu;		       /* DOCA DMA GPU instance */
	struct doca_gpu *gpu_dev;		       /* DOCA GPU device */
	struct doca_buf *src_doca_buf;		       /* src doca buffer - CPU memory */
	struct doca_buf *dst_doca_buf;		       /* dst doca buffer - GPU memory */
	struct doca_buf_arr *src_doca_buf_arr;	       /* src doca buffer - GPU memory */
	struct doca_buf_arr *dst_doca_buf_arr;	       /* dst doca buffer - CPU memory */
	struct doca_gpu_buf_arr *src_doca_gpu_buf_arr; /* src GPU doca buffer - GPU memory */
	struct doca_gpu_buf_arr *dst_doca_gpu_buf_arr; /* dst GPU doca buffer - CPU memory */
	char *src_buffer;			       /* src buffer address - CPU memory */
	char *dst_buffer;			       /* dst buffer address - GPU memory */
	bool gpu_datapath;			       /* Enable GPU datapath */
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
 * DMA memcpy task common callback
 *
 * @dma_task [in]: DMA task
 * @task_user_data [in]: Task user data
 * @ctx_user_data [in]: Context user data
 */
static void memcpy_task_common_callback(struct doca_dma_task_memcpy *dma_task,
					union doca_data task_user_data,
					union doca_data ctx_user_data)
{
	(void)dma_task;
	(void)task_user_data;
	(void)ctx_user_data;

	/* Set a flag to notify upon completion of a task */
	is_task_done = 1;
}

/*
 * Initialize DOCA device
 *
 * @nic_pcie_addr [in]: Network card PCIe address
 * @ddev [out]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_doca_device(char *nic_pcie_addr, struct doca_dev **ddev)
{
	doca_error_t status;

	if (nic_pcie_addr == NULL || ddev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	status = open_doca_device_with_pci(nic_pcie_addr, NULL, ddev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device based on NIC PCI address");
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize sample memory objects
 *
 * @state [in]: Sample objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_sample_mem_objs(struct gpu_dma_sample_objects *state)
{
	doca_error_t status;
	char *tmp_cpu;
	int dmabuf_fd;
	size_t size = DMA_MEMCPY_SIZE;

	ALIGN_SIZE(size, get_host_page_size());

	status = doca_mmap_create(&state->core_objs.src_mmap);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create source mmap: %s", doca_error_get_descr(status));
		return status;
	}
	status = doca_mmap_add_dev(state->core_objs.src_mmap, state->core_objs.dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to source mmap: %s", doca_error_get_descr(status));
		return status;
	}

	if (state->gpu_datapath) {
		/* Allocate GPU src buffer */
		status = doca_gpu_mem_alloc(state->gpu_dev,
					    size,
					    get_host_page_size(),
					    DOCA_GPU_MEM_TYPE_GPU_CPU, // GDRCopy
					    (void **)&state->src_buffer,
					    (void **)&tmp_cpu);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Unable to allocate gpu memory: %s",
				     doca_error_get_descr(status));
			return status;
		}

		/* Map GPU memory buffer used to receive packets with DMABuf */
		status = doca_gpu_dmabuf_fd(state->gpu_dev, state->src_buffer, size, &(dmabuf_fd));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_INFO("Mapping src buffer (0x%p size %zdB) with nvidia-peermem mode",
				      state->src_buffer,
				      size);

			/* If failed, use nvidia-peermem legacy method */
			status = doca_mmap_set_memrange(state->core_objs.src_mmap, state->src_buffer, size);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to initialize memory objects: Unable to set memrange to src mmap: %s",
					doca_error_get_descr(status));
				return status;
			}
		} else {
			DOCA_LOG_INFO("Mapping src buffer (0x%p size %zdB dmabuf fd %d) with dmabuf mode",
				      state->src_buffer,
				      size,
				      dmabuf_fd);

			status = doca_mmap_set_dmabuf_memrange(state->core_objs.src_mmap,
							       dmabuf_fd,
							       state->src_buffer,
							       0,
							       size);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to initialize dmabuf memory objects: Unable to set dmabuf memrange to src mmap: %s",
					doca_error_get_descr(status));
				return status;
			}
		}

		/* Copy data to src buffer */
		strcpy(tmp_cpu, "This is a sample piece of text from GPU");

		DOCA_LOG_INFO("The GPU source buffer value to be copied to CPU memory: %s", tmp_cpu);

		/* Allocate CPU dst buffer */
		state->dst_buffer = (char *)malloc(size);
		if (state->dst_buffer == NULL) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Unable to allocate cpu memory");
			return DOCA_ERROR_NO_MEMORY;
		}

		memset(state->dst_buffer, DEFAULT_VALUE, size);

		status = doca_mmap_create(&state->core_objs.dst_mmap);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create destination mmap: %s", doca_error_get_descr(status));
			return status;
		}
		status = doca_mmap_add_dev(state->core_objs.dst_mmap, state->core_objs.dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to add device to destination mmap: %s", doca_error_get_descr(status));
			return status;
		}

		status = doca_mmap_set_memrange(state->core_objs.dst_mmap, state->dst_buffer, size);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Unable to set memrange to dst mmap: %s",
				     doca_error_get_descr(status));
			return status;
		}

	} else {
		/* Create DOCA Core objects */
		status = create_core_objects(&(state->core_objs), NUM_BUFS);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Failed to create core objects: %s",
				     doca_error_get_descr(status));
			return status;
		}

		/* Allocate CPU src buffer */
		state->src_buffer = (char *)malloc(size);
		if (state->src_buffer == NULL) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Unable to allocate cpu memory");
			return DOCA_ERROR_NO_MEMORY;
		}

		/* Copy data to src buffer */
		strcpy(state->src_buffer, "This is a sample piece of text from CPU");

		/* Print the source buffer */
		DOCA_LOG_INFO("The CPU source buffer value to be copied to GPU memory: %s", state->src_buffer);

		/* Allocate GPU dst buffer */
		status = doca_gpu_mem_alloc(state->gpu_dev,
					    size,
					    get_host_page_size(),
					    DOCA_GPU_MEM_TYPE_GPU,
					    (void **)&state->dst_buffer,
					    NULL);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Unable to allocate gpu memory: %s",
				     doca_error_get_descr(status));
			return status;
		}

		/* Map GPU memory buffer used to receive packets with DMABuf */
		status = doca_gpu_dmabuf_fd(state->gpu_dev, state->dst_buffer, size, &(dmabuf_fd));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_INFO("Mapping dst buffer (0x%p size %zdB) with nvidia-peermem mode",
				      state->dst_buffer,
				      size);

			/* If failed, use nvidia-peermem legacy method */
			status = doca_mmap_set_memrange(state->core_objs.dst_mmap, state->dst_buffer, size);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to initialize memory objects: Unable to set memrange to dst mmap: %s",
					doca_error_get_descr(status));
				return status;
			}
		} else {
			DOCA_LOG_INFO("Mapping dst buffer (0x%p size %zdB dmabuf fd %d) with dmabuf mode",
				      state->dst_buffer,
				      size,
				      dmabuf_fd);

			status = doca_mmap_set_dmabuf_memrange(state->core_objs.dst_mmap,
							       dmabuf_fd,
							       state->dst_buffer,
							       0,
							       size);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR(
					"Failed to initialize dmabuf memory objects: Unable to set memrange to dst mmap: %s",
					doca_error_get_descr(status));
				return status;
			}
		}

		/* Set memory range in src mmap with CPU memory address */
		status = doca_mmap_set_memrange(state->core_objs.src_mmap, state->src_buffer, size);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize memory objects: Unable to set memrange to src mmap: %s",
				     doca_error_get_descr(status));
			return status;
		}
	}

	/* Start src mmap */
	status = doca_mmap_start(state->core_objs.src_mmap);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize memory objects: Unable to start src mmap: %s",
			     doca_error_get_descr(status));
		return status;
	}

	/* Start dst mmap */
	status = doca_mmap_start(state->core_objs.dst_mmap);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize memory objects: Unable to start dst mmap: %s",
			     doca_error_get_descr(status));
		return status;
	}

	if (state->gpu_datapath) {
		/* Create src GPU buffer array */
		status = doca_buf_arr_create(1, &state->src_doca_buf_arr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca src_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_set_target_gpu(state->src_doca_buf_arr, state->gpu_dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca src_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_set_params(state->src_doca_buf_arr, state->core_objs.src_mmap, size, 0);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca src_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_start(state->src_doca_buf_arr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca src_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_get_gpu_handle(state->src_doca_buf_arr, &(state->src_doca_gpu_buf_arr));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get buff_arr GPU handle: %s", doca_error_get_descr(status));
			return status;
		}

		/* Create dst GPU buffer array */
		status = doca_buf_arr_create(1, &state->dst_doca_buf_arr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca dst_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_set_target_gpu(state->dst_doca_buf_arr, state->gpu_dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca dst_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_set_params(state->dst_doca_buf_arr, state->core_objs.dst_mmap, size, 0);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca dst_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_start(state->dst_doca_buf_arr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buf: doca dst_doca_buf_arr internal error");
			return status;
		}

		status = doca_buf_arr_get_gpu_handle(state->dst_doca_buf_arr, &(state->dst_doca_gpu_buf_arr));
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get buff_arr GPU handle: %s", doca_error_get_descr(status));
			return status;
		}
	} else {
		/* Get a DOCA buffer from src mmap (CPU) */
		doca_buf_inventory_buf_get_by_data(state->core_objs.buf_inv,
						   state->core_objs.src_mmap,
						   state->src_buffer,
						   size,
						   &state->src_doca_buf);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to initialize memory objects: Unable to acquire DOCA buffer representing src buffer: %s",
				doca_error_get_descr(status));
			return status;
		}

		/* Get a DOCA buffer from dst mmap (GPU) */
		status = doca_buf_inventory_buf_get_by_addr(state->core_objs.buf_inv,
							    state->core_objs.dst_mmap,
							    state->dst_buffer,
							    size,
							    &state->dst_doca_buf);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to initialize memory objects: Unable to acquire DOCA buffer representing dst buffer: %s",
				doca_error_get_descr(status));
			return status;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Clean sample resources
 *
 * @state [in]: Sample objects to be destroyed
 */
static void gpu_dma_cleanup(struct gpu_dma_sample_objects *state)
{
	doca_error_t status;

	if (state->gpu_datapath) {
		DOCA_LOG_INFO("Cleanup DMA ctx with GPU data path");

		if (state->core_objs.ctx != NULL) {
			status = doca_ctx_stop(state->core_objs.ctx);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to stop dma ctx: %s",
					     doca_error_get_descr(status));
		}

		if (state->dma != NULL) {
			status = doca_dma_destroy(state->dma);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to destroy dma: %s",
					     doca_error_get_descr(status));
		}

		if (state->core_objs.src_mmap != NULL) {
			status = doca_mmap_destroy(state->core_objs.src_mmap);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to destroy src_mmap: %s",
					     doca_error_get_descr(status));
		}

		if (state->core_objs.dst_mmap != NULL) {
			status = doca_mmap_destroy(state->core_objs.dst_mmap);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to destroy dst_mmap: %s",
					     doca_error_get_descr(status));
		}

		if (state->dst_buffer != NULL) {
			free(state->dst_buffer);
			state->dst_buffer = NULL;
		}

		if (state->src_buffer != NULL) {
			doca_gpu_mem_free(state->gpu_dev, (void *)state->src_buffer);
			state->src_buffer = NULL;
		}
	} else {
		DOCA_LOG_INFO("Cleanup DMA ctx with CPU data path");
		if (state->core_objs.ctx != NULL) {
			status = doca_ctx_stop(state->core_objs.ctx);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to stop dma ctx: %s",
					     doca_error_get_descr(status));
		}

		if (state->dma != NULL) {
			status = doca_dma_destroy(state->dma);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to destroy dma: %s",
					     doca_error_get_descr(status));
		}

		if (state->dst_doca_buf != NULL) {
			status = doca_buf_dec_refcount(state->dst_doca_buf, NULL);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR(
					"Failed to clean sample objects: Failed to decrease DOCA dst buffer reference count: %s",
					doca_error_get_descr(status));
		}

		if (state->src_doca_buf != NULL) {
			status = doca_buf_dec_refcount(state->src_doca_buf, NULL);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR(
					"Failed to clean sample objects: Failed to decrease DOCA src buffer reference count: %s",
					doca_error_get_descr(status));
		}

		status = destroy_core_objects(&(state->core_objs));
		if (status != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to clean sample objects: Failed to destroy core objects: %s",
				     doca_error_get_descr(status));

		if (state->dst_buffer != NULL) {
			doca_gpu_mem_free(state->gpu_dev, (void *)state->dst_buffer);
			state->dst_buffer = NULL;
		}

		if (state->src_buffer != NULL) {
			free(state->src_buffer);
			state->src_buffer = NULL;
		}

		if (state->gpu_dev != NULL) {
			status = doca_gpu_destroy(state->gpu_dev);
			if (status != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to clean sample objects: Failed to destroy doca gpu: %s",
					     doca_error_get_descr(status));
		}
	}
}

/*
 * Initialize dma context
 *
 * @state [in]: Sample objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_dma_ctx(struct gpu_dma_sample_objects *state)
{
	doca_error_t status;

	/* Create dma ctx */
	status = doca_dma_create(state->core_objs.dev, &state->dma);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize dma ctx: Unable to create DMA engine: %s",
			     doca_error_get_descr(status));
		return status;
	}

	state->core_objs.ctx = doca_dma_as_ctx(state->dma);

	if (state->gpu_datapath) {
		status = doca_ctx_set_datapath_on_gpu(state->core_objs.ctx, state->gpu_dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set datapath on GPU: %s", doca_error_get_descr(status));
			return status;
		}
	} else {
		/* Connect context to progress engine */
		status = doca_pe_connect_ctx(state->core_objs.pe, state->core_objs.ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize dma ctx: Failed to connect PE to DMA: %s",
				     doca_error_get_descr(status));
			return status;
		}

		status = doca_dma_task_memcpy_set_conf(state->dma,
						       memcpy_task_common_callback,
						       memcpy_task_common_callback,
						       NUM_TASKS);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize dma ctx: Unable to config DMA task %s",
				     doca_error_get_descr(status));
			return status;
		}
	}

	/* Start doca ctx */
	status = doca_ctx_start(state->core_objs.ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize dma ctx: Unable to start dma context: %s",
			     doca_error_get_descr(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Submit DMA Memcpy task
 *
 * @state [in]: Sample objects
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t submit_dma_memcpy_task(struct gpu_dma_sample_objects *state)
{
	doca_error_t status;
	struct doca_dma_task_memcpy *memcpy_task;
	struct doca_task *task;
	union doca_data memcpy_task_user_data = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Construct DMA task */
	status = doca_dma_task_memcpy_alloc_init(state->dma,
						 state->src_doca_buf,
						 state->dst_doca_buf,
						 memcpy_task_user_data,
						 &memcpy_task);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit dma memcpy task: Failed to allocate task: %s",
			     doca_error_get_descr(status));
		return status;
	}

	/* Submit DMA task */
	task = doca_dma_task_memcpy_as_task(memcpy_task);
	status = doca_task_submit(task);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit dma memcpy task: %s", doca_error_get_descr(status));
		return status;
	}

	while (!is_task_done) {
		(void)doca_pe_progress(state->core_objs.pe);
		nanosleep(&ts, &ts);
	}

	/* Get task status */
	status = doca_task_get_status(task);

	/* Free task */
	doca_task_free(task);

	/* Check task status */
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DMA task event returned unsuccessfully: %s", doca_error_get_descr(status));
		return status;
	}

	DOCA_LOG_INFO("Success, DMA memcpy job done successfully");

	return DOCA_SUCCESS;
}

/*
 * Launch a CUDA kernel to read from the GPU destination buffer
 *
 * @gpu_dst_buffer [in]: The GPU destination buffer address
 * @dma_gpu [in]: The GPU DMA object
 * @src_gpu_buf_arr [in]: The GPU buff array src
 * @dst_gpu_buf_arr [in]: The GPU buff array dest
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t launch_cuda_kernel(uintptr_t gpu_dst_buffer,
				       struct doca_gpu_dma *dma_gpu,
				       struct doca_gpu_buf_arr *src_gpu_buf_arr,
				       struct doca_gpu_buf_arr *dst_gpu_buf_arr)
{
	doca_error_t status;
	cudaStream_t cuda_stream;
	cudaError_t res_rt = cudaSuccess;

	res_rt = cudaStreamCreateWithFlags(&cuda_stream, cudaStreamNonBlocking);
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaStreamCreateWithFlags error %d", res_rt);
		return DOCA_ERROR_DRIVER;
	}

	status = gpunetio_dma_memcpy_common_launch_kernel(cuda_stream,
							  gpu_dst_buffer,
							  dma_gpu,
							  src_gpu_buf_arr,
							  dst_gpu_buf_arr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function dma_gpu_copy_common_launch_kernel returned %s", doca_error_get_descr(status));
		return status;
	}

	res_rt = cudaDeviceSynchronize();
	if (res_rt != cudaSuccess) {
		DOCA_LOG_ERR("Function cudaDeviceSynchronize error %d", res_rt);
		return DOCA_ERROR_DRIVER;
	}

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
	doca_error_t status;
	struct gpu_dma_sample_objects state_cpu_gpu = {0};
	struct gpu_dma_sample_objects state_gpu_cpu = {0};

	status = init_doca_device(cfg->nic_pcie_addr, &state_cpu_gpu.core_objs.dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_doca_device returned %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_gpu_create(cfg->gpu_pcie_addr, &state_cpu_gpu.gpu_dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	state_cpu_gpu.gpu_datapath = false;
	state_gpu_cpu.core_objs.dev = state_cpu_gpu.core_objs.dev;
	state_gpu_cpu.gpu_datapath = true;
	state_gpu_cpu.gpu_dev = state_cpu_gpu.gpu_dev;

	status = init_sample_mem_objs(&state_cpu_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_sample_mem_objs returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	status = init_sample_mem_objs(&state_gpu_cpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_sample_mem_objs returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	status = init_dma_ctx(&state_cpu_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_dma_ctx returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	status = init_dma_ctx(&state_gpu_cpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function init_dma_ctx returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	status = submit_dma_memcpy_task(&state_cpu_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function submit_dma_memcpy_task returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	status = doca_dma_get_gpu_handle(state_gpu_cpu.dma, &state_gpu_cpu.dma_gpu);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function submit_dma_memcpy_task returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	status = launch_cuda_kernel((uintptr_t)state_cpu_gpu.dst_buffer,
				    state_gpu_cpu.dma_gpu,
				    state_gpu_cpu.src_doca_gpu_buf_arr,
				    state_gpu_cpu.dst_doca_gpu_buf_arr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function launch_cuda_kernel returned %s", doca_error_get_descr(status));
		goto gpu_dma_cleanup;
	}

	while (state_gpu_cpu.dst_buffer[0] == DEFAULT_VALUE)
		;

	printf("CPU received message from GPU: %s\n", state_gpu_cpu.dst_buffer);

gpu_dma_cleanup:
	gpu_dma_cleanup(&state_gpu_cpu);
	gpu_dma_cleanup(&state_cpu_gpu);

	return status;
}
