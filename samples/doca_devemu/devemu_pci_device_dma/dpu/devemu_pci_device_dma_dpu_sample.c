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

#include <devemu_pci_common.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_dma.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_DMA_DPU);

#define NUM_DMA_TASKS (1)      /* DMA tasks number */
#define MEM_BUF_LEN (4 * 1024) /* Mem buffer size. It's the same as Host side */

struct dma_resources {
	struct devemu_resources devemu_res; /* DOCA devemu resources*/
	struct doca_dma *dma_ctx;	    /* DOCA DMA context */
	struct doca_mmap *remote_mmap;	    /* DOCA mmap for remote buffer */
	struct doca_mmap *local_mmap;	    /* DOCA mmap for local buffer */
	struct doca_buf_inventory *buf_inv; /* DOCA buffer inventory */
	struct doca_dev *dma_dev;	    /* Dma device */
	char *local_mem_buf;		    /* Local memory buf for DMA operation */
	size_t num_remaining_tasks;	    /* Number of remaining tasks to process */
};

/*
 * DMA Memcpy task completed callback
 *
 * @dma_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void dma_memcpy_completed_callback(struct doca_dma_task_memcpy *dma_task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	struct dma_resources *resources = (struct dma_resources *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Assign success to the result */
	*result = DOCA_SUCCESS;
	DOCA_LOG_INFO("DMA task was completed successfully");

	/* Free task */
	doca_task_free(doca_dma_task_memcpy_as_task(dma_task));
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
}

/*
 * Memcpy task error callback
 *
 * @dma_task [in]: failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void dma_memcpy_error_callback(struct doca_dma_task_memcpy *dma_task,
				      union doca_data task_user_data,
				      union doca_data ctx_user_data)
{
	struct dma_resources *resources = (struct dma_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_dma_task_memcpy_as_task(dma_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Get the result of the task */
	*result = doca_task_get_status(task);
	DOCA_LOG_ERR("DMA task failed: %s", doca_error_get_descr(*result));

	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
}

/**
 * Setup DOCA buf inventory
 *
 * @resources [in]: struct dma_resources
 * @max_bufs [in]: The max number of buffers
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t setup_buf_inventory(struct dma_resources *resources, int max_bufs)
{
	doca_error_t result = DOCA_SUCCESS;
	if (max_bufs != 0) {
		result = doca_buf_inventory_create(max_bufs, &resources->buf_inv);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create buffer inventory: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_buf_inventory_start(resources->buf_inv);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buffer inventory: %s", doca_error_get_descr(result));
			return result;
		}
	}
	return result;
}

/*
 * Function to check DMA device capabilities
 *
 * @devinfo [in]: DMA device info
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dma_device_caps_is_supported(struct doca_devinfo *devinfo)
{
	doca_error_t result;

	uint8_t supported = 0;
	result = doca_devemu_pci_cap_is_mmap_add_dev_supported((const struct doca_devinfo *)devinfo, &supported);
	if (result != DOCA_SUCCESS || supported == 0)
		return DOCA_ERROR_NOT_SUPPORTED;

	return doca_dma_cap_task_memcpy_is_supported((const struct doca_devinfo *)devinfo);
}

/**
 * Setup DOCA DMA context
 *
 * @resources [in]: struct dma_resources
 * @devemu_name [in]: The DMA device name
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t setup_dma_ctx(struct dma_resources *resources, const char *devemu_name)
{
	doca_error_t result;
	union doca_data ctx_user_data = {0};
	struct doca_ctx *ctx;

	/* If device name wasn't provided, use the device emulation's device */
	if (*devemu_name == 0) {
		result = doca_dev_open(doca_dev_as_devinfo(resources->devemu_res.dev), &(resources->dma_dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		/* Open DOCA device */
		result = open_doca_device_with_ibdev_name((const uint8_t *)(devemu_name),
							  strlen(devemu_name),
							  dma_device_caps_is_supported,
							  &(resources->dma_dev));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
			return result;
		}
	}

	result = doca_dma_create(resources->dma_dev, &resources->dma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DMA context: %s", doca_error_get_descr(result));
		return result;
	}
	ctx = doca_dma_as_ctx(resources->dma_ctx);

	result = doca_dma_task_memcpy_set_conf(resources->dma_ctx,
					       dma_memcpy_completed_callback,
					       dma_memcpy_error_callback,
					       NUM_DMA_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set configurations for DMA memcpy task: %s", doca_error_get_descr(result));
		return result;
	}

	/* Include resources in user data of context to be used in callbacks */
	ctx_user_data.ptr = resources;
	result = doca_ctx_set_user_data(ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set context user data: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_pe_connect_ctx(resources->devemu_res.pe, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/**
 * Setup remote mmap
 *
 * @resources [in]: dma_resources
 * @buf [in]: address of remote memory buffer
 * @len [in]: len of remote memory buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t setup_remote_mmap(struct dma_resources *resources, char *buf, int len)
{
	doca_error_t result;
	result = doca_devemu_pci_mmap_create(resources->devemu_res.pci_dev, &resources->remote_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap for devemu pci device: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_max_num_devices(resources->remote_mmap, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max_num devices: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_add_dev(resources->remote_mmap, resources->dma_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_permissions(resources->remote_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permission: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_memrange(resources->remote_mmap, buf, len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set dst_mmap range: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(resources->remote_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start remote mmap: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/**
 * Setup local mmap
 *
 * @resources [in]: struct dma_resources
 * @buf [in]: Local memory buffer
 * @len [in]: Length of local memory buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t setup_local_mmap(struct dma_resources *resources, char *buf, int len)
{
	doca_error_t result;
	result = doca_mmap_create(&resources->local_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create destination mmap: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_add_dev(resources->local_mmap, resources->dma_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to destination mmap: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_memrange(resources->local_mmap, buf, len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set dst_mmap range: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(resources->local_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create dst_mmap: %s", doca_error_get_descr(result));
		return result;
	}
	return DOCA_SUCCESS;
}

/**
 * Do DMA copy task
 *
 * @resources [in]: struct dma_resources
 * @len [in]: The length of data to copy
 * @src_mmap [in]: Source DOCA mmap
 * @src_addr [in]: Source memory address
 * @dst_mmap [in]: Destination DOCA mmap
 * @dst_addr [in]: Destination memory address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t do_dma_copy(struct dma_resources *resources,
				int len,
				struct doca_mmap *src_mmap,
				void *src_addr,
				struct doca_mmap *dst_mmap,
				void *dst_addr)
{
	doca_error_t result, task_result;
	struct doca_buf *src_doca_buf, *dst_doca_buf;
	union doca_data task_user_data = {0};
	struct doca_task *task;
	struct doca_dma_task_memcpy *dma_task;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	result = doca_buf_inventory_buf_get_by_addr(resources->buf_inv, src_mmap, src_addr, len, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing src buffer: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_inventory_buf_get_by_addr(resources->buf_inv, dst_mmap, dst_addr, len, &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing dst buffer: %s", doca_error_get_descr(result));
		goto destroy_src_buf;
	}

	task_user_data.ptr = &task_result;
	result = doca_dma_task_memcpy_alloc_init(resources->dma_ctx,
						 src_doca_buf,
						 dst_doca_buf,
						 task_user_data,
						 &dma_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA memcpy task: %s", doca_error_get_descr(result));
		goto destroy_dst_buf;
	}

	resources->num_remaining_tasks = 1;
	task = doca_dma_task_memcpy_as_task(dma_task);

	result = doca_buf_set_data(src_doca_buf, src_addr, len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set data for DOCA buffer: %s", doca_error_get_descr(result));
		goto cleanup_dma_task;
	}

	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit DMA task: %s", doca_error_get_descr(result));
		goto cleanup_dma_task;
	}

	/* Wait DMA done */
	while (resources->num_remaining_tasks != 0) {
		if (doca_pe_progress(resources->devemu_res.pe) == 0)
			nanosleep(&ts, &ts);
	}
	result = task_result;
	goto destroy_dst_buf;

cleanup_dma_task:
	doca_task_free(task);
destroy_dst_buf:
	doca_buf_dec_refcount(dst_doca_buf, NULL);
destroy_src_buf:
	doca_buf_dec_refcount(src_doca_buf, NULL);

	return result;
}

/*
 * Setup devemu resources
 *
 * @resources [in]: General resources of DOCA devemu PCI
 * @pci_address [in]: Device PCI address
 * @emulated_dev_vuid [in]: VUID of the emulated device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t setup_devemu_resources(struct devemu_resources *resources,
					   const char *pci_address,
					   const char *emulated_dev_vuid)
{
	doca_error_t result;
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = PCI_TYPE_NAME;

	result = doca_pe_create(&resources->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_type_create(pci_type_name, &resources->pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	result = find_supported_device(pci_address,
				       resources->pci_type,
				       doca_devemu_pci_cap_type_is_hotplug_supported,
				       &resources->dev);
	if (result != DOCA_SUCCESS)
		return result;

	/* Set PCIe configuration space values */
	result = configure_and_start_pci_type(resources->pci_type, resources->dev);
	if (result != DOCA_SUCCESS)
		return result;

	/* Find existing emulated device */
	result = find_emulated_device(resources->pci_type, emulated_dev_vuid, &resources->rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to find PCI emulated device representor: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create emulated device context */
	result = doca_devemu_pci_dev_create(resources->pci_type, resources->rep, resources->pe, &resources->pci_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI emulated device context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(doca_devemu_pci_dev_as_ctx(resources->pci_dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI emulated device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Defer assignment so that cleanup does not stop the context in case it was not started */
	resources->ctx = doca_devemu_pci_dev_as_ctx(resources->pci_dev);

	return DOCA_SUCCESS;
}

/*
 * Cleanup DMA resources
 *
 * @resources [in]: DMA sample resources
 */
static void dma_resources_cleanup(struct dma_resources *resources)
{
	doca_error_t res;

	if (resources->buf_inv != NULL) {
		res = doca_buf_inventory_destroy(resources->buf_inv);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA buffer inventory: %s", doca_error_get_descr(res));
		resources->buf_inv = NULL;
	}

	if (resources->local_mmap != NULL) {
		res = doca_mmap_stop(resources->local_mmap);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop local mmap: %s", doca_error_get_descr(res));
	}

	if (resources->local_mmap != NULL) {
		res = doca_mmap_destroy(resources->local_mmap);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy local mmap: %s", doca_error_get_descr(res));
		resources->local_mmap = NULL;
	}

	if (resources->local_mem_buf != NULL) {
		free(resources->local_mem_buf);
		resources->local_mem_buf = NULL;
	}

	if (resources->remote_mmap != NULL) {
		res = doca_mmap_stop(resources->remote_mmap);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop remote mmap: %s", doca_error_get_descr(res));
	}

	if (resources->remote_mmap != NULL) {
		res = doca_mmap_destroy(resources->remote_mmap);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy remote mmap: %s", doca_error_get_descr(res));
		resources->remote_mmap = NULL;
	}

	if (resources->dma_ctx != NULL) {
		res = doca_ctx_stop(doca_dma_as_ctx(resources->dma_ctx));
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop dma ctx: %s", doca_error_get_descr(res));
	}

	if (resources->dma_ctx != NULL) {
		res = doca_dma_destroy(resources->dma_ctx);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy dma ctx: %s", doca_error_get_descr(res));
		resources->dma_ctx = NULL;
	}

	if (resources->dma_dev != NULL) {
		res = doca_dev_close(resources->dma_dev);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy dma device: %s", doca_error_get_descr(res));
		resources->dma_dev = NULL;
	}

	devemu_resources_cleanup(&resources->devemu_res, false);
}

/*
 * Run DOCA Device Emulation DMA DPU sample
 *
 * @pci_address [in]: DMA device PCI address
 * @devemu_name [in]: Device emulation manager name
 * @emulated_dev_vuid [in]: VUID of the emulated device
 * @host_dma_mem_iova [in]: Host DMA memory IOVA
 * @write_data [in]: Data write to host memory
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_dma_dpu(const char *pci_address,
				       const char *devemu_name,
				       const char *emulated_dev_vuid,
				       uint64_t host_dma_mem_iova,
				       const char *write_data)
{
	doca_error_t result;
	struct dma_resources resources = {0};
	size_t len = MEM_BUF_LEN;
	void *remote_addr = (void *)host_dma_mem_iova;

	result = setup_devemu_resources(&resources.devemu_res, pci_address, emulated_dev_vuid);
	if (result != DOCA_SUCCESS) {
		return result;
	}

	result = setup_dma_ctx(&resources, devemu_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to setup dma ctx: %s", doca_error_get_descr(result));
		dma_resources_cleanup(&resources);
		return result;
	}

	result = setup_remote_mmap(&resources, remote_addr, len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to setup remote mmap: %s", doca_error_get_descr(result));
		dma_resources_cleanup(&resources);
		return result;
	}

	resources.local_mem_buf = (char *)calloc(1, len);
	if (resources.local_mem_buf == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		result = DOCA_ERROR_NO_MEMORY;
		dma_resources_cleanup(&resources);
		return result;
	}

	result = setup_local_mmap(&resources, resources.local_mem_buf, len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to setup remote mmap: %s", doca_error_get_descr(result));
		dma_resources_cleanup(&resources);
		return result;
	}

	result = setup_buf_inventory(&resources, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to allocated buf inventory: %s", doca_error_get_descr(result));
		dma_resources_cleanup(&resources);
		return result;
	}

	/* DMA read data from host */
	result = do_dma_copy(&resources,
			     len,
			     resources.remote_mmap,
			     remote_addr,
			     resources.local_mmap,
			     resources.local_mem_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to DMA read data from host: %s", doca_error_get_descr(result));
		dma_resources_cleanup(&resources);
		return result;
	}
	DOCA_LOG_INFO("Success, DMA memory copied from host: %s", resources.local_mem_buf);

	/* DMA write data to host (if any)*/
	if (strnlen(write_data, MEM_BUF_LEN) > 0) {
		strncpy(resources.local_mem_buf, write_data, MEM_BUF_LEN);
		result = do_dma_copy(&resources,
				     len,
				     resources.local_mmap,
				     resources.local_mem_buf,
				     resources.remote_mmap,
				     remote_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to DMA write data to host: %s", doca_error_get_descr(result));
			dma_resources_cleanup(&resources);
			return result;
		}
		DOCA_LOG_INFO("Success, DMA memory copied to host: %s", resources.local_mem_buf);
	}

	dma_resources_cleanup(&resources);
	return result;
}
