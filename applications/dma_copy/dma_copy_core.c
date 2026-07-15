/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_mmap.h>

#include <samples/common.h>

#include "pack.h"
#include "utils.h"

#include "dma_copy_core.h"

#define CC_MAX_QUEUE_SIZE 10	   /* Max number of messages on Comm Channel queue */
#define SLEEP_IN_NANOS (10 * 1000) /* Sample the task every 10 microseconds  */
#define STATUS_SUCCESS true	   /* Successful status */
#define STATUS_FAILURE false	   /* Unsuccessful status */

DOCA_LOG_REGISTER(DMA_COPY_CORE);

/*
 * Get DOCA DMA maximum buffer size allowed
 *
 * @resources [in]: DOCA DMA resources pointer
 * @max_buf_size [out]: Maximum buffer size allowed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_dma_max_buf_size(struct dma_copy_resources *resources, uint64_t *max_buf_size)
{
	struct doca_devinfo *dma_dev_info = doca_dev_as_devinfo(resources->state->dev);
	doca_error_t result;

	result = doca_dma_cap_task_memcpy_get_max_buf_size(dma_dev_info, max_buf_size);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to retrieve maximum buffer size allowed from DOCA DMA device: %s",
			     doca_error_get_descr(result));
	else
		DOCA_LOG_DBG("DOCA DMA device supports maximum buffer size of %" PRIu64 " bytes", *max_buf_size);

	return result;
}

/*
 * Validate file size
 *
 * @file_path [in]: File to validate
 * @file_size [out]: File size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t validate_file_size(const char *file_path, uint64_t *file_size)
{
	FILE *fp;
	long size;

	fp = fopen(file_path, "r");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to open %s", file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	size = ftell(fp);
	if (size == -1) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	fclose(fp);

	DOCA_LOG_INFO("The file size is %ld", size);

	*file_size = size;

	return DOCA_SUCCESS;
}

/*
 * ARGP validation Callback - check if input file exists
 *
 * @config [in]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t args_validation_callback(void *config)
{
	struct dma_copy_cfg *cfg = (struct dma_copy_cfg *)config;

	if (access(cfg->file_path, F_OK | R_OK) == 0) {
		cfg->is_file_found_locally = true;
		return validate_file_size(cfg->file_path, &cfg->file_size);
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Comch DOCA device PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dev_pci_addr_callback(void *param, void *config)
{
	struct dma_copy_cfg *cfg = (struct dma_copy_cfg *)config;
	const char *dev_pci_addr = (char *)param;

	if (strnlen(dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE) == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(cfg->cc_dev_pci_addr, dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t file_path_callback(void *param, void *config)
{
	struct dma_copy_cfg *cfg = (struct dma_copy_cfg *)config;
	char *file_path = (char *)param;
	int file_path_len = strnlen(file_path, MAX_ARG_SIZE);

	if (file_path_len == MAX_ARG_SIZE) {
		DOCA_LOG_ERR("Entered file path exceeded buffer size - MAX=%d", MAX_ARG_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(cfg->file_path, file_path, MAX_ARG_SIZE);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Comch DOCA device representor PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rep_pci_addr_callback(void *param, void *config)
{
	struct dma_copy_cfg *cfg = (struct dma_copy_cfg *)config;
	const char *rep_pci_addr = (char *)param;

	if (cfg->mode == DMA_COPY_MODE_DPU) {
		if (strnlen(rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE) == DOCA_DEVINFO_REP_PCI_ADDR_SIZE) {
			DOCA_LOG_ERR("Entered device representor PCI address exceeding the maximum size of %d",
				     DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}

		strlcpy(cfg->cc_dev_rep_pci_addr, rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE);
	}

	return DOCA_SUCCESS;
}

/*
 * Save remote buffer information into a file
 *
 * @cfg [in]: Application configuration
 * @buffer [in]: Buffer to read information from
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t save_buffer_into_a_file(struct dma_copy_cfg *cfg, const char *buffer)
{
	FILE *fp;

	fp = fopen(cfg->file_path, "w");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to create the DMA copy file");
		return DOCA_ERROR_IO_FAILED;
	}

	if (fwrite(buffer, 1, cfg->file_size, fp) != cfg->file_size) {
		DOCA_LOG_ERR("Failed to write full content into the output file");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	fclose(fp);

	return DOCA_SUCCESS;
}

/*
 * Fill local buffer with file content
 *
 * @cfg [in]: Application configuration
 * @buffer [out]: Buffer to save information into
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t fill_buffer_with_file_content(struct dma_copy_cfg *cfg, char *buffer)
{
	FILE *fp;

	fp = fopen(cfg->file_path, "r");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to open %s", cfg->file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Read file content and store it in the local buffer which will be exported */
	if (fread(buffer, 1, cfg->file_size, fp) != cfg->file_size) {
		DOCA_LOG_ERR("Failed to read content from file: %s", cfg->file_path);
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}
	fclose(fp);

	return DOCA_SUCCESS;
}

/*
 * Allocate memory and populate it into the memory map
 *
 * @mmap [in]: DOCA memory map
 * @buffer_len [in]: Allocated buffer length
 * @access_flags [in]: The access permissions of the mmap
 * @buffer [out]: Allocated buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t memory_alloc_and_populate(struct doca_mmap *mmap,
					      size_t buffer_len,
					      uint32_t access_flags,
					      char **buffer)
{
	doca_error_t result;

	result = doca_mmap_set_permissions(mmap, access_flags);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set access permissions of memory map: %s", doca_error_get_descr(result));
		return result;
	}

	*buffer = (char *)malloc(buffer_len);
	if (*buffer == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for source buffer");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_set_memrange(mmap, *buffer, buffer_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memrange of memory map: %s", doca_error_get_descr(result));
		free(*buffer);
		return result;
	}

	/* Populate local buffer into memory map to allow access from DPU side after exporting */
	result = doca_mmap_start(mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to populate memory map: %s", doca_error_get_descr(result));
		free(*buffer);
	}

	return result;
}

/*
 * DPU side function for submitting DMA task into the progress engine, wait for its completion and save it into a file
 * if needed.
 *
 * @cfg [in]: Application configuration
 * @resources [in]: DMA copy resources
 * @bytes_to_copy [in]: Number of bytes to DMA copy
 * @buffer [in]: local DMA buffer
 * @local_doca_buf [in]: local DOCA buffer
 * @remote_doca_buf [in]: remote DOCA buffer
 * @num_remaining_tasks [in]: Number of remaining tasks
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpu_submit_dma_task(struct dma_copy_cfg *cfg,
					struct dma_copy_resources *resources,
					size_t bytes_to_copy,
					char *buffer,
					struct doca_buf *local_doca_buf,
					struct doca_buf *remote_doca_buf,
					size_t *num_remaining_tasks)
{
	struct program_core_objects *state = resources->state;
	struct doca_dma_task_memcpy *dma_task;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	void *data;
	struct doca_buf *src_buf;
	struct doca_buf *dst_buf;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;
	doca_error_t task_result;

	/* Determine DMA copy direction */
	if (cfg->is_file_found_locally) {
		src_buf = local_doca_buf;
		dst_buf = remote_doca_buf;
	} else {
		src_buf = remote_doca_buf;
		dst_buf = local_doca_buf;
	}

	/* Set data position in src_buf */
	result = doca_buf_get_data(src_buf, &data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get data address from DOCA buffer: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_buf_set_data(src_buf, data, bytes_to_copy);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set data for DOCA buffer: %s", doca_error_get_descr(result));
		return result;
	}

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct DMA task */
	result = doca_dma_task_memcpy_alloc_init(resources->dma_ctx, src_buf, dst_buf, task_user_data, &dma_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA memcpy task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_dma_task_memcpy_as_task(dma_task);

	/* Submit DMA task */
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit DMA task: %s", doca_error_get_descr(result));
		goto free_task;
	}

	/* Wait for all tasks to be completed */
	while (*num_remaining_tasks > 0) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Check result of task according to the result we update in the callbacks */
	if (task_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DMA copy failed: %s", doca_error_get_descr(task_result));
		result = task_result;
		goto free_task;
	}

	DOCA_LOG_INFO("DMA copy was done Successfully");

	/* If the buffer was copied into to DPU, save it as a file */
	if (!cfg->is_file_found_locally) {
		DOCA_LOG_INFO("Writing DMA buffer into a file on %s", cfg->file_path);
		result = save_buffer_into_a_file(cfg, buffer);
		if (result != DOCA_SUCCESS)
			return result;
	}

free_task:
	doca_task_free(task);
	return result;
}

/*
 * Check if DOCA device is DMA capable
 *
 * @devinfo [in]: Device to check
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t check_dev_dma_capable(struct doca_devinfo *devinfo)
{
	return doca_dma_cap_task_memcpy_is_supported(devinfo);
}

doca_error_t register_dma_copy_params(void)
{
	doca_error_t result;
	struct doca_argp_param *file_path_param, *dev_pci_addr_param, *rep_pci_addr_param;

	/* Create and register string to dma copy param */
	result = doca_argp_param_create(&file_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(file_path_param, "f");
	doca_argp_param_set_long_name(file_path_param, "file");
	doca_argp_param_set_description(file_path_param,
					"Full path to file to be copied/created after a successful DMA copy");
	doca_argp_param_set_callback(file_path_param, file_path_callback);
	doca_argp_param_set_type(file_path_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(file_path_param);
	result = doca_argp_register_param(file_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DOCA Comch device PCI address */
	result = doca_argp_param_create(&dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dev_pci_addr_param, "p");
	doca_argp_param_set_long_name(dev_pci_addr_param, "pci-addr");
	doca_argp_param_set_description(dev_pci_addr_param, "DOCA Comch device PCI address");
	doca_argp_param_set_callback(dev_pci_addr_param, dev_pci_addr_callback);
	doca_argp_param_set_type(dev_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(dev_pci_addr_param);
	result = doca_argp_register_param(dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DOCA Comch device representor PCI address */
	result = doca_argp_param_create(&rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rep_pci_addr_param, "r");
	doca_argp_param_set_long_name(rep_pci_addr_param, "rep-pci");
	doca_argp_param_set_description(rep_pci_addr_param,
					"DOCA Comch device representor PCI address (needed only on DPU)");
	doca_argp_param_set_callback(rep_pci_addr_param, rep_pci_addr_callback);
	doca_argp_param_set_type(rep_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register validation callback */
	result = doca_argp_register_validation_callback(args_validation_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program validation callback: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register version callback for DOCA SDK & RUNTIME */
	result = doca_argp_register_version_callback(sdk_version_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register version callback: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t open_dma_device(struct doca_dev **dev)
{
	doca_error_t result;

	result = open_doca_device_with_capabilities(check_dev_dma_capable, dev);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to open DOCA DMA capable device: %s", doca_error_get_descr(result));

	return result;
}

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
	size_t *num_remaining_tasks = (size_t *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	(void)dma_task;
	/* Decrement number of remaining tasks */
	--*num_remaining_tasks;
	/* Assign success to the result */
	*result = DOCA_SUCCESS;
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
	size_t *num_remaining_tasks = (size_t *)ctx_user_data.ptr;
	struct doca_task *task = doca_dma_task_memcpy_as_task(dma_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Decrement number of remaining tasks */
	--*num_remaining_tasks;
	/* Get the result of the task */
	*result = doca_task_get_status(task);
}

/*
 * Destroy copy resources
 *
 * @resources [in]: DMA copy resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_dma_copy_resources(struct dma_copy_resources *resources)
{
	struct program_core_objects *state = resources->state;
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	if (resources->dma_ctx != NULL) {
		tmp_result = doca_dma_destroy(resources->dma_ctx);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy DOCA DMA context: %s", doca_error_get_descr(tmp_result));
		}
	}

	tmp_result = destroy_core_objects(state);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DOCA core objects: %s", doca_error_get_descr(tmp_result));
	}

	free(resources->state);

	return result;
}

/*
 * Allocate DMA copy resources
 *
 * @resources [out]: DOCA DMA copy resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t allocate_dma_copy_resources(struct dma_copy_resources *resources)
{
	struct program_core_objects *state = NULL;
	doca_error_t result, tmp_result;
	/* Two buffers for source and destination */
	uint32_t max_bufs = 2;

	resources->state = malloc(sizeof(*(resources->state)));
	if (resources->state == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate DOCA program core objects: %s", doca_error_get_descr(result));
		return result;
	}
	state = resources->state;

	/* Open DOCA dma device */
	result = open_dma_device(&state->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DMA device: %s", doca_error_get_descr(result));
		goto free_state;
	}

	result = create_core_objects(state, max_bufs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create core objects: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	result = doca_dma_create(state->dev, &resources->dma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create DOCA DMA context: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	state->ctx = doca_dma_as_ctx(resources->dma_ctx);

	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set DOCA progress engine to DOCA DMA: %s", doca_error_get_descr(result));
		goto destroy_dma;
	}

	result = doca_dma_task_memcpy_set_conf(resources->dma_ctx,
					       dma_memcpy_completed_callback,
					       dma_memcpy_error_callback,
					       NUM_DMA_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set configurations for DMA memcpy task: %s", doca_error_get_descr(result));
		goto destroy_dma;
	}

	return result;

destroy_dma:
	tmp_result = doca_dma_destroy(resources->dma_ctx);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DOCA DMA context: %s", doca_error_get_descr(tmp_result));
	}
destroy_core_objects:
	tmp_result = destroy_core_objects(state);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DOCA core objects: %s", doca_error_get_descr(tmp_result));
	}
free_state:
	free(resources->state);

	return result;
}

/*
 * Helper function to send a status message across the comch
 *
 * @comch_connection [in]: comch connection to send the status message across
 * @status [in]: true means the status is success, false means a failure
 */
static void send_status_msg(struct doca_comch_connection *comch_connection, bool status)
{
	struct comch_msg_dma_status status_msg = {.type = COMCH_MSG_STATUS};
	doca_error_t result;

	status_msg.is_success = status;

	result = comch_utils_send(comch_connection, &status_msg, sizeof(struct comch_msg_dma_status));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send status message: %s", doca_error_get_descr(result));
	}
}

/*
 * Process and respond to a DMA direction negotiation message on the host
 *
 * @cfg [in]: dma copy configuration information
 * @comch_connection [in]: comch connection the message was received on
 * @dir_msg [in]: the direction message received
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t host_process_dma_direction_and_size(struct dma_copy_cfg *cfg,
							struct doca_comch_connection *comch_connection,
							struct comch_msg_dma_direction *dir_msg)
{
	struct comch_msg_dma_export_discriptor *exp_msg;
	char export_msg[cfg->max_comch_buffer];
	size_t exp_msg_len;
	const void *export_desc;
	size_t export_desc_len;
	doca_error_t result;

	if (!cfg->is_file_found_locally) {
		cfg->file_size = ntohq(dir_msg->file_size);

		/* Allocate a buffer to receive the file data */
		result = memory_alloc_and_populate(cfg->file_mmap,
						   cfg->file_size,
						   DOCA_ACCESS_FLAG_PCI_READ_WRITE,
						   &cfg->file_buffer);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate recv buffer: %s", doca_error_get_descr(result));
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	/* Export the host mmap to the DPU to start the DMA */
	exp_msg = (struct comch_msg_dma_export_discriptor *)export_msg;
	exp_msg->type = COMCH_MSG_EXPORT_DESCRIPTOR;
	exp_msg->host_addr = htonq((uintptr_t)cfg->file_buffer);

	result = doca_mmap_export_pci(cfg->file_mmap, cfg->dev, &export_desc, &export_desc_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to export DOCA mmap: %s", doca_error_get_descr(result));
		return result;
	}

	exp_msg_len = export_desc_len + sizeof(struct comch_msg_dma_export_discriptor);
	if (exp_msg_len > cfg->max_comch_buffer) {
		DOCA_LOG_ERR("Export message exceeds max length of comch. Message len: %lu, Max len: %u",
			     exp_msg_len,
			     cfg->max_comch_buffer);
		return DOCA_ERROR_INVALID_VALUE;
	}

	exp_msg->export_desc_len = htonq(export_desc_len);
	memcpy(exp_msg->exported_mmap, export_desc, export_desc_len);

	/* It is assumed there are enough tasks for message to succeed - progress should not be called from callback */
	return comch_utils_send(comch_connection, exp_msg, exp_msg_len);
}

void host_recv_event_cb(struct doca_comch_event_msg_recv *event,
			uint8_t *recv_buffer,
			uint32_t msg_len,
			struct doca_comch_connection *comch_connection)
{
	struct dma_copy_cfg *cfg = comch_utils_get_user_data(comch_connection);
	struct comch_msg_dma_status *status = NULL;
	struct comch_msg *comch_msg = NULL;
	doca_error_t result = DOCA_ERROR_UNKNOWN;

	(void)event;

	if (cfg == NULL) {
		DOCA_LOG_ERR("Cannot get configuration information");
		return;
	}

	/* Message must at least contain a message type */
	if (msg_len < sizeof(enum comch_msg_type)) {
		DOCA_LOG_ERR("Received a message that is too small. Length: %u", msg_len);
		send_status_msg(comch_connection, STATUS_FAILURE);
		cfg->comch_state = COMCH_ERROR;
		return;
	}

	/* All messages should take the format of a comch_msg struct */
	comch_msg = (struct comch_msg *)recv_buffer;

	/*
	 * The host will have started the DMA negotiation by sending a dma_direction message.
	 * It should receive the same format message back from the DPU containing file size if file in on DPU.
	 * The host will allocate space to receive the file or use preallocated memory if the file is on the host.
	 * The file location data is exported to the DPU.
	 * When DMA has completed, a status message should be received.
	 */

	switch (comch_msg->type) {
	case COMCH_MSG_DIRECTION:
		if (msg_len != sizeof(struct comch_msg_dma_direction)) {
			DOCA_LOG_ERR("Direction message has bad length. Length: %u, expected: %lu",
				     msg_len,
				     sizeof(struct comch_msg_dma_direction));
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		result = host_process_dma_direction_and_size(cfg,
							     comch_connection,
							     (struct comch_msg_dma_direction *)recv_buffer);
		if (result != DOCA_SUCCESS) {
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		break;
	case COMCH_MSG_STATUS:
		if (msg_len != sizeof(struct comch_msg_dma_status)) {
			DOCA_LOG_ERR("Status message has bad length. Length: %u, expected: %lu",
				     msg_len,
				     sizeof(struct comch_msg_dma_status));
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		status = (struct comch_msg_dma_status *)recv_buffer;
		if (status->is_success == STATUS_FAILURE)
			cfg->comch_state = COMCH_ERROR;
		else
			cfg->comch_state = COMCH_COMPLETE;

		break;
	default:
		DOCA_LOG_ERR("Received bad message type. Type: %u", comch_msg->type);
		send_status_msg(comch_connection, STATUS_FAILURE);
		cfg->comch_state = COMCH_ERROR;
		return;
	}
}

/*
 * Helper to send a DMA direction notification request
 *
 * @dma_cfg [in]: dma copy configuration information
 * @comch_cfg [in]: comch object to send message across
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_file_direction_request(struct dma_copy_cfg *dma_cfg, struct comch_cfg *comch_cfg)
{
	struct comch_msg_dma_direction dir_msg = {.type = COMCH_MSG_DIRECTION};

	if (dma_cfg->is_file_found_locally) {
		DOCA_LOG_INFO("File was found locally, it will be DMA copied to the DPU");
		dir_msg.file_in_host = true;
		dir_msg.file_size = htonq(dma_cfg->file_size);
	} else {
		DOCA_LOG_INFO("File was not found locally, it will be DMA copied from the DPU");
		dir_msg.file_in_host = false;
	}

	return comch_utils_send(comch_util_get_connection(comch_cfg), &dir_msg, sizeof(struct comch_msg_dma_direction));
}

doca_error_t host_start_dma_copy(struct dma_copy_cfg *dma_cfg, struct comch_cfg *comch_cfg)
{
	doca_error_t result, tmp_result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	dma_cfg->max_comch_buffer = comch_utils_get_max_buffer_size(comch_cfg);
	if (dma_cfg->max_comch_buffer == 0) {
		DOCA_LOG_ERR("Comch max buffer length is 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Open DOCA dma device */
	result = open_dma_device(&dma_cfg->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA DMA device: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_create(&dma_cfg->file_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create mmap: %s", doca_error_get_descr(result));
		goto close_device;
	}

	result = doca_mmap_add_dev(dma_cfg->file_mmap, dma_cfg->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/*
	 * If the file is local, allocate a DMA buffer and populate it now.
	 * If file is remote, the buffer can be allocated in the callback when the size if known.
	 */
	if (dma_cfg->is_file_found_locally == true) {
		result = memory_alloc_and_populate(dma_cfg->file_mmap,
						   dma_cfg->file_size,
						   DOCA_ACCESS_FLAG_PCI_READ_ONLY,
						   &dma_cfg->file_buffer);
		if (result != DOCA_SUCCESS)
			goto destroy_mmap;

		result = fill_buffer_with_file_content(dma_cfg, dma_cfg->file_buffer);
		if (result != DOCA_SUCCESS)
			goto free_buffer;
	}

	dma_cfg->comch_state = COMCH_NEGOTIATING;
	result = send_file_direction_request(dma_cfg, comch_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send direction negotiation message: %s", doca_error_get_descr(result));
		goto free_buffer;
	}

	/* Wait for a signal that the DPU has completed the DMA copy */
	while (dma_cfg->comch_state == COMCH_NEGOTIATING) {
		nanosleep(&ts, &ts);
		result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Comch connection unexpectedly dropped: %s", doca_error_get_descr(result));
			goto free_buffer;
		}
	}

	if (dma_cfg->comch_state == COMCH_ERROR) {
		DOCA_LOG_ERR("Failure was detected in dma copy");
		result = DOCA_ERROR_BAD_STATE;
		goto free_buffer;
	}

	DOCA_LOG_INFO("Final status message was successfully received");

	if (!dma_cfg->is_file_found_locally) {
		/*  File was copied successfully into the buffer, save it into file */
		DOCA_LOG_INFO("Writing DMA buffer into a file on %s", dma_cfg->file_path);
		result = save_buffer_into_a_file(dma_cfg, dma_cfg->file_buffer);
	}

free_buffer:
	free(dma_cfg->file_buffer);
destroy_mmap:
	tmp_result = doca_mmap_destroy(dma_cfg->file_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_device:
	tmp_result = doca_dev_close(dma_cfg->dev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Process and respond to a DMA direction negotiation message on the DPU
 *
 * @cfg [in]: dma copy configuration information
 * @comch_connection [in]: comch connection the message was received on
 * @dir_msg [in]: the direction message received
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpu_process_dma_direction_and_size(struct dma_copy_cfg *cfg,
						       struct doca_comch_connection *comch_connection,
						       struct comch_msg_dma_direction *dir_msg)
{
	struct comch_msg_dma_direction resp_dir_msg = {.type = COMCH_MSG_DIRECTION};
	doca_error_t result;

	/* Make sure file is located only on one side */
	if (cfg->is_file_found_locally && dir_msg->file_in_host == true) {
		DOCA_LOG_ERR("Error - File was found on both Host and DPU");
		return DOCA_ERROR_INVALID_VALUE;

	} else if (!cfg->is_file_found_locally) {
		if (!dir_msg->file_in_host) {
			DOCA_LOG_ERR("Error - File was not found on both Host and DPU");
			return DOCA_ERROR_INVALID_VALUE;
		}
		cfg->file_size = ntohq(dir_msg->file_size);
	}

	/* Verify file size against the HW limitation */
	if (cfg->file_size > cfg->max_dma_buf_size) {
		DOCA_LOG_ERR("DMA device maximum allowed file size in bytes is %" PRIu64
			     ", received file size is %" PRIu64 " bytes",
			     cfg->max_dma_buf_size,
			     cfg->file_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Populate and send response to host */
	if (cfg->is_file_found_locally) {
		DOCA_LOG_INFO("File was found locally, it will be DMA copied to the Host");
		resp_dir_msg.file_in_host = false;
		resp_dir_msg.file_size = htonq(cfg->file_size);
	} else {
		DOCA_LOG_INFO("File was not found locally, it will be DMA copied from the Host");
		resp_dir_msg.file_in_host = true;
	}

	result = comch_utils_send(comch_connection, &resp_dir_msg, sizeof(struct comch_msg_dma_direction));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send DMA direction message: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Process an export descriptor message on the DPU
 *
 * @cfg [in]: dma copy configuration information
 * @des_msg [in]: the export descriptor message received
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpu_process_export_descriptor(struct dma_copy_cfg *cfg,
						  struct comch_msg_dma_export_discriptor *des_msg)
{
	size_t desc_len = ntohq(des_msg->export_desc_len);

	/* desc_len bounded by msg_len validation in dpu_recv_event_cb() */
	cfg->exported_mmap = malloc(desc_len);
	if (cfg->exported_mmap == NULL) {
		DOCA_LOG_ERR("Failed to allocate export descriptor memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	memcpy(cfg->exported_mmap, des_msg->exported_mmap, desc_len);
	cfg->exported_mmap_len = desc_len;
	cfg->host_addr = (uint8_t *)ntohq(des_msg->host_addr);

	return DOCA_SUCCESS;
}

void dpu_recv_event_cb(struct doca_comch_event_msg_recv *event,
		       uint8_t *recv_buffer,
		       uint32_t msg_len,
		       struct doca_comch_connection *comch_connection)
{
	struct dma_copy_cfg *cfg = comch_utils_get_user_data(comch_connection);
	struct comch_msg_dma_export_discriptor *comch_msg_export = NULL;
	struct comch_msg_dma_status *status = NULL;
	struct comch_msg *comch_msg = NULL;
	doca_error_t result = DOCA_ERROR_UNKNOWN;

	(void)event;

	if (cfg == NULL) {
		DOCA_LOG_ERR("Cannot get configuration information");
		return;
	}

	/* Message must at least contain a message type */
	if (msg_len < sizeof(enum comch_msg_type)) {
		DOCA_LOG_ERR("Received a message that is too small. Length: %u", msg_len);
		send_status_msg(comch_connection, STATUS_FAILURE);
		cfg->comch_state = COMCH_ERROR;
		return;
	}

	/* All messages should take the format of a comch_msg struct */
	comch_msg = (struct comch_msg *)recv_buffer;

	/*
	 * The first message a DPU receives should be a direction negotiation request.
	 * This should be responded to as an ack or containing the file information if file is local to the DPU.
	 * The host will respond will memory information the DPU can read from or write to.
	 * At this stage the DMA can be triggered.
	 */

	switch (comch_msg->type) {
	case COMCH_MSG_DIRECTION:
		if (msg_len != sizeof(struct comch_msg_dma_direction)) {
			DOCA_LOG_ERR("Direction message has bad length. Length: %u, expected: %lu",
				     msg_len,
				     sizeof(struct comch_msg_dma_direction));
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		result = dpu_process_dma_direction_and_size(cfg,
							    comch_connection,
							    (struct comch_msg_dma_direction *)recv_buffer);
		if (result != DOCA_SUCCESS) {
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}
		break;
	case COMCH_MSG_EXPORT_DESCRIPTOR:
		if (msg_len <= sizeof(struct comch_msg_dma_export_discriptor)) {
			DOCA_LOG_ERR("Export descriptor message has bad length. Length: %u, expected at least: %lu",
				     msg_len,
				     sizeof(struct comch_msg_dma_direction));
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		comch_msg_export = (struct comch_msg_dma_export_discriptor *)recv_buffer;
		if (ntohq(comch_msg_export->export_desc_len) > msg_len) {
			DOCA_LOG_ERR(
				"Export descriptor message validation failed: declared length: %lu exceeds message buffer size :%u",
				ntohq(comch_msg_export->export_desc_len),
				msg_len);
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		result = dpu_process_export_descriptor(cfg, comch_msg_export);
		if (result != DOCA_SUCCESS) {
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		/* All information is successfully received to do DMA */
		cfg->comch_state = COMCH_COMPLETE;
		break;
	case COMCH_MSG_STATUS:
		if (msg_len != sizeof(struct comch_msg_dma_status)) {
			DOCA_LOG_ERR("Status message has bad length. Length: %u, expected: %lu",
				     msg_len,
				     sizeof(struct comch_msg_dma_status));
			send_status_msg(comch_connection, STATUS_FAILURE);
			cfg->comch_state = COMCH_ERROR;
			return;
		}

		status = (struct comch_msg_dma_status *)recv_buffer;
		if (status->is_success == STATUS_FAILURE)
			cfg->comch_state = COMCH_ERROR;

		break;
	default:
		DOCA_LOG_ERR("Received bad message type. Type: %u", comch_msg->type);
		send_status_msg(comch_connection, STATUS_FAILURE);
		cfg->comch_state = COMCH_ERROR;
		return;
	}
}

doca_error_t dpu_start_dma_copy(struct dma_copy_cfg *dma_cfg, struct comch_cfg *comch_cfg)
{
	struct dma_copy_resources resources = {0};
	struct program_core_objects *state = NULL;
	/* Allocate memory to be used for read operation in case file is found locally, otherwise grant write access */
	uint32_t access_flags = dma_cfg->is_file_found_locally ? DOCA_ACCESS_FLAG_LOCAL_READ_ONLY :
								 DOCA_ACCESS_FLAG_LOCAL_READ_WRITE;
	struct doca_buf *remote_doca_buf = NULL;
	struct doca_buf *local_doca_buf = NULL;
	struct doca_mmap *remote_mmap = NULL;
	union doca_data ctx_user_data = {0};
	/* Number of tasks submitted to progress engine */
	size_t num_remaining_tasks = 1;
	doca_error_t result, tmp_result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Allocate DMA copy resources */
	result = allocate_dma_copy_resources(&resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA copy resources: %s", doca_error_get_descr(result));
		return result;
	}
	state = resources.state;

	result = get_dma_max_buf_size(&resources, &dma_cfg->max_dma_buf_size);
	if (result != DOCA_SUCCESS)
		goto destroy_dma_resources;

	/* Include tasks counter in user data of context to be decremented in callbacks */
	ctx_user_data.ptr = &num_remaining_tasks;
	result = doca_ctx_set_user_data(state->ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set user data: %s", doca_error_get_descr(result));
		goto destroy_dma_resources;
	}

	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start DMA context: %s", doca_error_get_descr(result));
		goto destroy_dma_resources;
	}

	/* Wait until all DMA metadata is received on comch */
	while (dma_cfg->comch_state == COMCH_NEGOTIATING) {
		nanosleep(&ts, &ts);
		result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Comch connection unexpectedly dropped: %s", doca_error_get_descr(result));
			goto stop_dma;
		}
	}

	if (dma_cfg->comch_state == COMCH_ERROR) {
		DOCA_LOG_ERR("Comch DMA metadata negotiation failed");
		result = DOCA_ERROR_BAD_STATE;
		goto stop_dma;
	}

	/* Configure buffer to send/recv file on */
	result = memory_alloc_and_populate(state->src_mmap, dma_cfg->file_size, access_flags, &dma_cfg->file_buffer);
	if (result != DOCA_SUCCESS)
		goto stop_dma;

	/* Create a local DOCA mmap from export descriptor */
	result = doca_mmap_create_from_export(NULL,
					      (const void *)dma_cfg->exported_mmap,
					      dma_cfg->exported_mmap_len,
					      state->dev,
					      &remote_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create memory map from export: %s", doca_error_get_descr(result));
		goto free_buffer;
	}

	/* Construct DOCA buffer for remote (Host) address range */
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    remote_mmap,
						    dma_cfg->host_addr,
						    dma_cfg->file_size,
						    &remote_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA remote buffer: %s", doca_error_get_descr(result));
		send_status_msg(comch_util_get_connection(comch_cfg), STATUS_FAILURE);
		goto destroy_remote_mmap;
	}

	/* Construct DOCA buffer for local (DPU) address range */
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    state->src_mmap,
						    dma_cfg->file_buffer,
						    dma_cfg->file_size,
						    &local_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA local buffer: %s", doca_error_get_descr(result));
		send_status_msg(comch_util_get_connection(comch_cfg), STATUS_FAILURE);
		goto destroy_remote_buf;
	}

	/* Fill buffer in file content if relevant */
	if (dma_cfg->is_file_found_locally) {
		result = fill_buffer_with_file_content(dma_cfg, dma_cfg->file_buffer);
		if (result != DOCA_SUCCESS) {
			send_status_msg(comch_util_get_connection(comch_cfg), STATUS_FAILURE);
			goto destroy_local_buf;
		}
	}

	/* Submit DMA task into the progress engine and wait until task completion */
	result = dpu_submit_dma_task(dma_cfg,
				     &resources,
				     dma_cfg->file_size,
				     dma_cfg->file_buffer,
				     local_doca_buf,
				     remote_doca_buf,
				     &num_remaining_tasks);
	if (result != DOCA_SUCCESS) {
		send_status_msg(comch_util_get_connection(comch_cfg), STATUS_FAILURE);
		goto destroy_local_buf;
	}

	send_status_msg(comch_util_get_connection(comch_cfg), STATUS_SUCCESS);

destroy_local_buf:
	tmp_result = doca_buf_dec_refcount(local_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy local DOCA buffer: %s", doca_error_get_descr(tmp_result));
	}
destroy_remote_buf:
	tmp_result = doca_buf_dec_refcount(remote_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy remote DOCA buffer: %s", doca_error_get_descr(tmp_result));
	}
destroy_remote_mmap:
	tmp_result = doca_mmap_destroy(remote_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy remote DOCA mmap: %s", doca_error_get_descr(tmp_result));
	}
free_buffer:
	free(dma_cfg->file_buffer);
stop_dma:
	tmp_result = request_stop_ctx(state->pe, state->ctx);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Unable to stop context: %s", doca_error_get_descr(tmp_result));
	}
	state->ctx = NULL;
destroy_dma_resources:
	if (dma_cfg->exported_mmap != NULL)
		free(dma_cfg->exported_mmap);
	tmp_result = destroy_dma_copy_resources(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DMA copy resources: %s", doca_error_get_descr(tmp_result));
	}
	return result;
}
