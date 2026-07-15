/*
 * Copyright (c) 2022-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <utils.h>

#include "file_integrity_core.h"

#define SLEEP_IN_NANOS (10 * 1000)		  /* Sample the task every 10 microseconds */
#define DEFAULT_TIMEOUT (10)			  /* default timeout for receiving messages */
#define SHA_ALGORITHM (DOCA_SHA_ALGORITHM_SHA256) /* doca_sha_algorithm for the sample */
#define LOG_NUM_SHA_TASKS (0)			  /* Log of SHA tasks number */

DOCA_LOG_REGISTER(FILE_INTEGRITY::Core);

/*
 * Free callback - free doca_buf allocated pointer
 *
 * @addr [in]: Memory range pointer
 * @len [in]: Memory range length
 * @opaque [in]: An opaque pointer passed to iterator
 */
static void free_cb(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;

	if (addr != NULL)
		free(addr);
}

/*
 * Unmap callback - free doca_buf allocated pointer
 *
 * @addr [in]: Memory range pointer
 * @len [in]: Memory range length
 * @opaque [in]: An opaque pointer passed to iterator
 */
static void unmap_cb(void *addr, size_t len, void *opaque)
{
	(void)opaque;

	if (addr != NULL)
		munmap(addr, len);
}

/*
 * Populate destination doca buffer for SHA tasks
 *
 * @state [in]: application configuration struct
 * @dst_doca_buf [out]: created doca buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t populate_dst_buf(struct program_core_objects *state, struct doca_buf **dst_doca_buf)
{
	char *dst_buffer = NULL;
	uint32_t min_dst_sha_buffer_size;
	doca_error_t result;

	result = doca_sha_cap_get_min_dst_buf_size(doca_dev_as_devinfo(state->dev),
						   SHA_ALGORITHM,
						   &min_dst_sha_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get minimum destination buffer size for DOCA SHA: %s",
			     doca_error_get_descr(result));
		return result;
	}

	dst_buffer = calloc(1, min_dst_sha_buffer_size);
	if (dst_buffer == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, min_dst_sha_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range destination memory map: %s", doca_error_get_descr(result));
		free(dst_buffer);
		return result;
	}
	result = doca_mmap_set_free_cb(state->dst_mmap, &free_cb, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set free callback of destination memory map: %s", doca_error_get_descr(result));
		free(dst_buffer);
		return result;
	}
	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start destination memory map: %s", doca_error_get_descr(result));
		free(dst_buffer);
		return result;
	}

	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    state->dst_mmap,
						    dst_buffer,
						    min_dst_sha_buffer_size,
						    dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		return result;
	}
	return result;
}

/*
 * Construct DOCA SHA task, submit it and print the result
 *
 * @state [in]: application configuration struct
 * @sha_ctx [in]: context of SHA library
 * @dst_doca_buf [in]: destination doca buffer
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t calculate_sha(struct program_core_objects *state,
				  struct doca_sha *sha_ctx,
				  struct doca_buf **dst_doca_buf,
				  char *file_data,
				  size_t file_size)
{
	struct doca_buf *src_doca_buf;
	struct doca_sha_task_hash *sha_hash_task = NULL;
	struct doca_task *task = NULL;
	size_t num_remaining_tasks = 0;
	union doca_data ctx_user_data = {0};
	union doca_data task_user_data = {0};
	doca_error_t result, task_result;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	result = doca_mmap_set_memrange(state->src_mmap, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range of source memory map: %s", doca_error_get_descr(result));
		munmap(file_data, file_size);
		return result;
	}
	result = doca_mmap_set_free_cb(state->src_mmap, &unmap_cb, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set free callback of source memory map: %s", doca_error_get_descr(result));
		munmap(file_data, file_size);
		return result;
	}
	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start source memory map: %s", doca_error_get_descr(result));
		munmap(file_data, file_size);
		return result;
	}

	result =
		doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->src_mmap, file_data, file_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_set_data(src_doca_buf, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_buf_set_data() for request doca_buf failure");
		doca_buf_dec_refcount(src_doca_buf, NULL);
		return result;
	}

	result = populate_dst_buf(state, dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		doca_buf_dec_refcount(src_doca_buf, NULL);
		return result;
	}

	/* Include tasks counter in user data of context to be decremented in callbacks */
	ctx_user_data.ptr = &num_remaining_tasks;
	result = doca_ctx_set_user_data(state->ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		doca_buf_dec_refcount(src_doca_buf, NULL);
		return result;
	}

	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		doca_buf_dec_refcount(src_doca_buf, NULL);
		return result;
	}

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct SHA hash task */
	result = doca_sha_task_hash_alloc_init(sha_ctx,
					       SHA_ALGORITHM,
					       src_doca_buf,
					       *dst_doca_buf,
					       task_user_data,
					       &sha_hash_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate SHA hash task: %s", doca_error_get_descr(result));
		doca_buf_dec_refcount(src_doca_buf, NULL);
		return result;
	}

	task = doca_sha_task_hash_as_task(sha_hash_task);

	/* Submit SHA hash task */
	num_remaining_tasks++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit SHA hash task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		doca_buf_dec_refcount(src_doca_buf, NULL);
		return result;
	}

	/* Wait for all tasks to be completed */
	while (num_remaining_tasks > 0) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	result = task_result;

	/* Check result of task according to the result we update in the callbacks */
	if (task_result != DOCA_SUCCESS)
		DOCA_LOG_ERR("SHA hash task failed: %s", doca_error_get_descr(task_result));

	doca_task_free(task);
	doca_buf_dec_refcount(src_doca_buf, NULL);

	return result;
}

/*
 * Send the input file over comch to the server in segments of that can be handled by SHA
 *
 * @comch_cfg [in]: comch configuration object to send file across
 * @app_cfg [in]: app configuration
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @file_sha [in]: SHA of the file to send
 * @sha_len [in]: length of the sha array
 * @min_partial_block_size [in]: minimum size of a SHA partial block
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_file(struct comch_cfg *comch_cfg,
			      struct file_integrity_config *app_cfg,
			      char *file_data,
			      size_t file_size,
			      uint8_t *file_sha,
			      size_t sha_len,
			      uint32_t min_partial_block_size)
{
	struct file_integrity_metadata_msg *meta_msg;
	size_t meta_msg_len;
	uint32_t total_msgs;
	uint32_t max_comch_msg;
	size_t msg_len;
	uint32_t i, partial_block_size;
	doca_error_t result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	meta_msg_len = sizeof(struct file_integrity_metadata_msg) + sha_len;
	meta_msg = (struct file_integrity_metadata_msg *)malloc(meta_msg_len);
	if (meta_msg == NULL) {
		DOCA_LOG_ERR("Failed allocate memory for file metadata message");
		return DOCA_ERROR_NO_MEMORY;
	}

	max_comch_msg = comch_utils_get_max_buffer_size(comch_cfg);
	if (max_comch_msg == 0 || min_partial_block_size > max_comch_msg) {
		DOCA_LOG_ERR("Comch message size too small for minimum SHA block. Comch size: %u, partial SHA min: %u",
			     max_comch_msg,
			     min_partial_block_size);
		free(meta_msg);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * Calculate the biggest partial block size that is smaller than the max_comch_msg
	 * A valid partial block size must be a multiple of min_partial_block_size
	 */
	partial_block_size = max_comch_msg - (max_comch_msg % min_partial_block_size);

	/*
	 * Send to the server the number of messages needed for receiving the file
	 * The number of messages is equal to the size of the file divided by the size of the partial block size
	 * of the partial hash task, and rounding up
	 */
	total_msgs = 1 + ((file_size - 1) / partial_block_size);
	meta_msg->total_file_chunks = htonl(total_msgs);

	memcpy(&meta_msg->sha_data[0], file_sha, sha_len);

	/* This is the first message so assume there's room to send it */
	result = comch_utils_send(comch_util_get_connection(comch_cfg), meta_msg, meta_msg_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send file meta message: %s", doca_error_get_descr(result));
		free(meta_msg);
		return result;
	}

	free(meta_msg);

	/* Send file to the server */
	for (i = 0; i < total_msgs; i++) {
		msg_len = MIN(file_size, partial_block_size);

		/* Verify that the other side has not signalled it is done */
		if (app_cfg->state == TRANSFER_COMPLETE)
			break;

		result = comch_utils_send(comch_util_get_connection(comch_cfg), file_data, msg_len);
		while (result == DOCA_ERROR_AGAIN) {
			nanosleep(&ts, &ts);
			result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
			if (result != DOCA_SUCCESS)
				break;
			result = comch_utils_send(comch_util_get_connection(comch_cfg), file_data, msg_len);
		}

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("File data was not sent: %s", doca_error_get_descr(result));
			return result;
		}
		file_data += msg_len;
		file_size -= msg_len;
	}
	return DOCA_SUCCESS;
}

void client_recv_event_cb(struct doca_comch_event_msg_recv *event,
			  uint8_t *recv_buffer,
			  uint32_t msg_len,
			  struct doca_comch_connection *comch_connection)
{
	struct file_integrity_config *cfg = comch_utils_get_user_data(comch_connection);

	/* A message is only expected from the server when it has finished processing the file */
	(void)event;

	/* Print the completion message sent from the server */
	recv_buffer[msg_len] = '\0';
	DOCA_LOG_INFO("Received message: %s", recv_buffer);
	cfg->state = TRANSFER_COMPLETE;
}

doca_error_t file_integrity_client(struct comch_cfg *comch_cfg,
				   struct file_integrity_config *app_cfg,
				   struct program_core_objects *state,
				   struct doca_sha *sha_ctx)
{
	struct doca_buf *dst_doca_buf;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};
	char *file_data, *sha_output;
	uint8_t *sha_msg;
	size_t hash_length, i;
	struct stat statbuf;
	int fd;
	uint64_t max_source_buffer_size;
	uint32_t min_partial_block_size;
	doca_error_t result;

	fd = open(app_cfg->file_path, O_RDWR);
	if (fd < 0) {
		DOCA_LOG_ERR("Failed to open %s", app_cfg->file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (fstat(fd, &statbuf) < 0) {
		DOCA_LOG_ERR("Failed to get file information");
		close(fd);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Get the partial block size */
	result = doca_sha_cap_get_partial_hash_block_size(doca_dev_as_devinfo(state->dev),
							  SHA_ALGORITHM,
							  &min_partial_block_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the partial hash block size for DOCA SHA: %s",
			     doca_error_get_descr(result));
		close(fd);
		return result;
	}

	result = doca_sha_cap_get_max_src_buf_size(doca_dev_as_devinfo(state->dev), &max_source_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get maximum source buffer size for DOCA SHA: %s", doca_error_get_descr(result));
		close(fd);
		return result;
	}

	if (statbuf.st_size <= 0 || (uint64_t)statbuf.st_size > max_source_buffer_size) {
		DOCA_LOG_ERR("Invalid file size. Should be greater then zero and smaller than %lu",
			     max_source_buffer_size);
		close(fd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	file_data = mmap(NULL, statbuf.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (file_data == MAP_FAILED) {
		DOCA_LOG_ERR("Unable to map file content: %s", strerror(errno));
		close(fd);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Calculate SHA */
	result = calculate_sha(state, sha_ctx, &dst_doca_buf, file_data, statbuf.st_size);
	if (result != DOCA_SUCCESS) {
		close(fd);
		return result;
	}

	result = doca_buf_get_data_len(dst_doca_buf, &hash_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the data length of DOCA buffer: %s", doca_error_get_descr(result));
		doca_buf_dec_refcount(dst_doca_buf, NULL);
		close(fd);
		return result;
	}
	result = doca_buf_get_data(dst_doca_buf, (void **)&sha_msg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the data of DOCA buffer: %s", doca_error_get_descr(result));
		doca_buf_dec_refcount(dst_doca_buf, NULL);
		close(fd);
		return result;
	}

	/* Engine outputs hex format. For char format output, we need double the length */
	sha_output = calloc(1, (hash_length * 2) + 1);
	if (sha_output == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		doca_buf_dec_refcount(dst_doca_buf, NULL);
		close(fd);
		return DOCA_ERROR_NO_MEMORY;
	}

	for (i = 0; i < hash_length; i++)
		snprintf(sha_output + (2 * i), 3, "%02x", sha_msg[i]);
	DOCA_LOG_INFO("SHA256 output is: %s", sha_output);

	free(sha_output);

	/* Send the file content to the server */
	result =
		send_file(comch_cfg, app_cfg, file_data, statbuf.st_size, sha_msg, hash_length, min_partial_block_size);
	if (result != DOCA_SUCCESS) {
		doca_buf_dec_refcount(dst_doca_buf, NULL);
		close(fd);
		return result;
	}

	doca_buf_dec_refcount(dst_doca_buf, NULL);
	close(fd);

	/* Receive finish message when file was completely read by the server */
	while (app_cfg->state != TRANSFER_COMPLETE) {
		nanosleep(&ts, &ts);
		result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Comch connection unexpectedly dropped: %s", doca_error_get_descr(result));
			return result;
		}
	}

	return result;
}

void server_recv_event_cb(struct doca_comch_event_msg_recv *event,
			  uint8_t *recv_buffer,
			  uint32_t msg_len,
			  struct doca_comch_connection *comch_connection)
{
	struct file_integrity_config *cfg = comch_utils_get_user_data(comch_connection);
	struct server_runtime_data *server_data;
	union doca_data task_user_data = {0};
	struct doca_task *task;
	doca_error_t result, task_result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	(void)event;

	if (cfg == NULL) {
		DOCA_LOG_ERR("Cannot get configuration information");
		return;
	}

	/* Ignore any events occurring after transfer is complete */
	if (cfg->state == TRANSFER_COMPLETE || cfg->state == TRANSFER_ERROR)
		return;

	server_data = &cfg->server_data;

	/* First received message should contain file metadata */
	if (server_data->expected_file_chunks == 0) {
		struct file_integrity_metadata_msg *file_meta = (struct file_integrity_metadata_msg *)recv_buffer;

		if (msg_len < sizeof(struct file_integrity_metadata_msg)) {
			DOCA_LOG_ERR("Unexpected file metadata message received. Size %u, min expected size %lu",
				     msg_len,
				     sizeof(struct file_integrity_metadata_msg));
			cfg->state = TRANSFER_ERROR;
			return;
		}

		server_data->expected_file_chunks = ntohl(file_meta->total_file_chunks);
		server_data->expected_sha_len = msg_len - (sizeof(struct file_integrity_metadata_msg));
		memcpy(server_data->expected_sha, file_meta->sha_data, server_data->expected_sha_len);

		cfg->state = TRANSFER_IN_PROGRESS;
		return;
	}

	/* At this point all messages must be file data */

	/* Put the new message into the buffer data */
	memcpy(server_data->sha_src_data, recv_buffer, msg_len);

	/* Set data address and length in the doca_buf */
	result = doca_buf_set_data(server_data->sha_src_buf, server_data->sha_src_data, msg_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_buf_set_data() for request doca_buf failure: %s", doca_error_get_descr(result));
		cfg->state = TRANSFER_ERROR;
		return;
	}

	/* If there is only one chunk then send it as a complete SHA request */
	if (server_data->expected_file_chunks == 1) {
		result = doca_sha_task_hash_set_src(server_data->sha_hash_task, server_data->sha_src_buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set source for SHA hash task: %s", doca_error_get_descr(result));
			cfg->state = TRANSFER_ERROR;
			return;
		}

		task = doca_sha_task_hash_as_task(server_data->sha_hash_task);
	} else {
		/* If multiple file chunks, send as partial SHA */

		result = doca_sha_task_partial_hash_set_src(server_data->sha_partial_hash_task,
							    server_data->sha_src_buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set source for SHA partial hash task: %s",
				     doca_error_get_descr(result));
			cfg->state = TRANSFER_ERROR;
			return;
		}

		/* If it is the last chunk then mark it as the final buffer */
		if (server_data->received_file_chunks == server_data->expected_file_chunks - 1) {
			result = doca_sha_task_partial_hash_set_is_final_buf(server_data->sha_partial_hash_task);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set final buffer for SHA partial hash task: %s",
					     doca_error_get_descr(result));
				cfg->state = TRANSFER_ERROR;
				return;
			}
		}

		task = doca_sha_task_partial_hash_as_task(server_data->sha_partial_hash_task);
	}

	task_user_data.ptr = &task_result;
	doca_task_set_user_data(task, task_user_data);

	(server_data->active_sha_tasks)++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit SHA partial hash task: %s", doca_error_get_descr(result));
		cfg->state = TRANSFER_ERROR;
		return;
	}

	/* Wait for the task to be completed */
	while (server_data->active_sha_tasks > 0) {
		if (doca_pe_progress(server_data->sha_state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Check result of task according to the result we update in the callbacks */
	if (task_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SHA partial hash task failed: %s", doca_error_get_descr(task_result));
		cfg->state = TRANSFER_ERROR;
		return;
	}

	/* Write the received chunk to file */
	if ((size_t)write(server_data->fd, recv_buffer, msg_len) != msg_len) {
		DOCA_LOG_ERR("Failed to write the received message into the input file");
		cfg->state = TRANSFER_ERROR;
		return;
	}

	server_data->received_file_chunks += 1;
	server_data->received_file_length += msg_len;

	if (server_data->received_file_chunks == server_data->expected_file_chunks)
		cfg->state = TRANSFER_COMPLETE;
}

/*
 * Initiate SHA specific data in the app config
 *
 * This data can be accessed by comch callbacks to send data directly to the SHA engine.
 *
 * @sha_ctx [in]: doca context to use for SHA offload
 * @state [in]: core objects configured for SHA offload
 * @server_data [in]: app global information that can be shared in callbacks
 * @dst_doca_buf [in]: preallocated buffer to receive SHA output
 * @max_comch_msg [in]: maximum size message across comch
 * @fd [in]: file descriptor to write data to
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_async_sha_recv_data(struct doca_sha *sha_ctx,
					     struct program_core_objects *state,
					     struct server_runtime_data *server_data,
					     struct doca_buf *dst_doca_buf,
					     uint32_t max_comch_msg,
					     int fd)
{
	union doca_data task_user_data = {0};
	union doca_data ctx_user_data = {0};
	uint32_t received_sha_msg_size;
	doca_error_t result;

	/* Get size of SHA output */
	result = doca_sha_cap_get_min_dst_buf_size(doca_dev_as_devinfo(state->dev),
						   SHA_ALGORITHM,
						   &received_sha_msg_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get minimum destination buffer size for DOCA SHA: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Allocate a buffer to copy the expect SHA to */
	server_data->expected_sha = calloc(1, received_sha_msg_size);
	if (server_data->expected_sha == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for received sha");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Allocate buffer to register for SHA and handle incoming messages */
	server_data->sha_src_data = calloc(1, max_comch_msg);
	if (server_data->sha_src_data == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for SHA data buffer");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_expected_sha;
	}

	/* Include the allocated data buffer in the source mmap */
	result = doca_mmap_set_memrange(state->src_mmap, server_data->sha_src_data, max_comch_msg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range of source memory map: %s", doca_error_get_descr(result));
		goto free_sha_src_buf;
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start source memory map: %s", doca_error_get_descr(result));
		goto free_sha_src_buf;
	}

	/* Get a doca_buf associated with the data buffer for sending to the SHA engine */
	result = doca_buf_inventory_buf_get_by_data(state->buf_inv,
						    state->src_mmap,
						    server_data->sha_src_data,
						    max_comch_msg,
						    &server_data->sha_src_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		goto stop_mmap;
	}

	/* Set the user data of the context to the number of active tasks variable - task completion decrements
	 * this */
	server_data->active_sha_tasks = 0;
	ctx_user_data.ptr = &server_data->active_sha_tasks;
	result = doca_ctx_set_user_data(state->ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DOCA context user data: %s", doca_error_get_descr(result));
		goto free_doca_buf;
	}

	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA context: %s", doca_error_get_descr(result));
		goto free_doca_buf;
	}

	/* Allocate a single task for a full SHA offload */
	result = doca_sha_task_hash_alloc_init(sha_ctx,
					       SHA_ALGORITHM,
					       server_data->sha_src_buf,
					       dst_doca_buf,
					       task_user_data,
					       &server_data->sha_hash_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate SHA hash task: %s", doca_error_get_descr(result));
		goto stop_ctx;
	}

	/* Allocate a single task for use in partial SHA */
	result = doca_sha_task_partial_hash_alloc_init(sha_ctx,
						       SHA_ALGORITHM,
						       server_data->sha_src_buf,
						       dst_doca_buf,
						       task_user_data,
						       &server_data->sha_partial_hash_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate SHA partial hash task: %s", doca_error_get_descr(result));
		goto free_sha_task;
	}

	server_data->sha_state = state;
	server_data->fd = fd;

	return DOCA_SUCCESS;

free_sha_task:
	doca_task_free(doca_sha_task_hash_as_task(server_data->sha_hash_task));
stop_ctx:
	doca_ctx_stop(state->ctx);
free_doca_buf:
	doca_buf_dec_refcount(server_data->sha_src_buf, NULL);
stop_mmap:
	doca_mmap_stop(state->src_mmap);
free_sha_src_buf:
	free(server_data->sha_src_data);
free_expected_sha:
	free(server_data->expected_sha);

	return result;
}

/*
 * Undo the allocations made in init_async_sha_recv_data()
 *
 * @server_data [in]: app global information that can be shared in callbacks
 */
static void uninit_async_sha_recv_data(struct server_runtime_data *server_data)
{
	/* Note, the SHA context will be stopped in file_integrity_cleanup() */
	doca_task_free(doca_sha_task_partial_hash_as_task(server_data->sha_partial_hash_task));
	doca_task_free(doca_sha_task_hash_as_task(server_data->sha_hash_task));
	doca_buf_dec_refcount(server_data->sha_src_buf, NULL);
	doca_mmap_stop(server_data->sha_state->src_mmap);
	free(server_data->sha_src_data);
	server_data->sha_src_data = NULL;
	free(server_data->expected_sha);
	server_data->expected_sha = NULL;

	server_data->sha_state = NULL;
	server_data->fd = 0;
}

doca_error_t file_integrity_server(struct comch_cfg *comch_cfg,
				   struct file_integrity_config *app_cfg,
				   struct program_core_objects *state,
				   struct doca_sha *sha_ctx)
{
	struct doca_buf *dst_doca_buf = NULL;
	uint32_t i, max_comch_msg;
	size_t hash_length;
	char *sha_output;
	int fd;
	uint8_t *file_sha;
	char finish_msg[] = "Server was done receiving messages";
	int counter = 0;
	int num_of_iterations = (app_cfg->timeout * 1000 * 1000) / (SLEEP_IN_NANOS / 1000);
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;

	result = populate_dst_buf(state, &dst_doca_buf);
	if (result != DOCA_SUCCESS)
		goto finish_msg;

	fd = open(app_cfg->file_path, O_CREAT | O_WRONLY, S_IRUSR | S_IRGRP);
	if (fd < 0) {
		DOCA_LOG_ERR("Failed to open %s", app_cfg->file_path);
		result = DOCA_ERROR_IO_FAILED;
		goto free_dst_buf;
	}

	max_comch_msg = comch_utils_get_max_buffer_size(comch_cfg);
	result = init_async_sha_recv_data(sha_ctx, state, &app_cfg->server_data, dst_doca_buf, max_comch_msg, fd);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init async data: %s", doca_error_get_descr(result));
		goto close_fd;
	}

	/* Wait on comch to complete client to server transactions */
	while (app_cfg->state != TRANSFER_COMPLETE && app_cfg->state != TRANSFER_ERROR) {
		nanosleep(&ts, &ts);
		result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Comch connection unexpectedly dropped: %s", doca_error_get_descr(result));
			goto uninit_async;
		}

		if (app_cfg->state == TRANSFER_IDLE)
			continue;

		counter++;
		if (counter == num_of_iterations) {
			DOCA_LOG_ERR("Message was not received at the given timeout");
			result = DOCA_ERROR_BAD_STATE;
			goto uninit_async;
		}
	}

	if (app_cfg->state == TRANSFER_ERROR) {
		DOCA_LOG_ERR("Error detected during comch exchange");
		result = DOCA_ERROR_BAD_STATE;
		goto uninit_async;
	}

	/* compare received SHA with calculated SHA */
	result = doca_buf_get_data_len(dst_doca_buf, &hash_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the data length of DOCA buffer: %s", doca_error_get_descr(result));
		goto uninit_async;
	}

	if (hash_length == 0) {
		DOCA_LOG_ERR("Error in calculating SHA - output length is 0");
		result = DOCA_ERROR_INVALID_VALUE;
		goto uninit_async;
	}

	result = doca_buf_get_data(dst_doca_buf, (void **)&file_sha);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the data of DOCA buffer: %s", doca_error_get_descr(result));
		goto uninit_async;
	}

	/* Engine outputs hex format. For char format output, we need double the length */
	sha_output = calloc(1, (hash_length * 2) + 1);
	if (sha_output == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory to display SHA");
		result = DOCA_ERROR_NO_MEMORY;
		goto uninit_async;
	}

	for (i = 0; i < hash_length; i++)
		snprintf(sha_output + (2 * i), 3, "%02x", file_sha[i]);
	DOCA_LOG_INFO("SHA256 output is: %s", sha_output);

	free(sha_output);

	if (memcmp(file_sha, app_cfg->server_data.expected_sha, hash_length) == 0)
		DOCA_LOG_INFO("SUCCESS: file SHA is identical to received SHA");
	else {
		DOCA_LOG_ERR("ERROR: SHA is not identical, file was compromised");
		if (remove(app_cfg->file_path) < 0)
			DOCA_LOG_ERR("Failed to remove %s", app_cfg->file_path);
	}

uninit_async:
	uninit_async_sha_recv_data(&app_cfg->server_data);
close_fd:
	close(fd);
free_dst_buf:
	doca_buf_dec_refcount(dst_doca_buf, NULL);
finish_msg:
	if (comch_utils_send(comch_util_get_connection(comch_cfg), finish_msg, sizeof(finish_msg)) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to send finish message: %s", doca_error_get_descr(result));

	return result;
}

/*
 * SHA hash task completed callback
 *
 * @sha_hash_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void sha_hash_completed_callback(struct doca_sha_task_hash *sha_hash_task,
					union doca_data task_user_data,
					union doca_data ctx_user_data)
{
	size_t *num_remaining_tasks = (size_t *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	(void)sha_hash_task;

	/* Decrement number of remaining tasks */
	--*num_remaining_tasks;
	/* Assign success to the result */
	*result = DOCA_SUCCESS;
}

/*
 * SHA hash task error callback
 *
 * @sha_hash_task [in]: Failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void sha_hash_error_callback(struct doca_sha_task_hash *sha_hash_task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
	size_t *num_remaining_tasks = (size_t *)ctx_user_data.ptr;
	struct doca_task *task = doca_sha_task_hash_as_task(sha_hash_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Decrement number of remaining tasks */
	--*num_remaining_tasks;
	/* Get the result of the task */
	*result = doca_task_get_status(task);
}

/*
 * SHA partial hash task completed callback
 *
 * @sha_partial_hash_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void sha_partial_hash_completed_callback(struct doca_sha_task_partial_hash *sha_partial_hash_task,
						union doca_data task_user_data,
						union doca_data ctx_user_data)
{
	size_t *num_remaining_tasks = (size_t *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	(void)sha_partial_hash_task;

	/* Decrement number of remaining tasks */
	--*num_remaining_tasks;
	/* Assign success to the result */
	*result = DOCA_SUCCESS;
}

/*
 * SHA partial hash task error callback
 *
 * @sha_partial_hash_task [in]: Failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void sha_partial_hash_error_callback(struct doca_sha_task_partial_hash *sha_partial_hash_task,
					    union doca_data task_user_data,
					    union doca_data ctx_user_data)
{
	size_t *num_remaining_tasks = (size_t *)ctx_user_data.ptr;
	struct doca_task *task = doca_sha_task_partial_hash_as_task(sha_partial_hash_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Decrement number of remaining tasks */
	--*num_remaining_tasks;
	/* Get the result of the task */
	*result = doca_task_get_status(task);
}

/*
 * Check if given device is capable of executing a SHA partial hash task.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS if the device supports SHA hash task and DOCA_ERROR otherwise.
 */
static doca_error_t sha_partial_hash_is_supported(struct doca_devinfo *devinfo)
{
	return doca_sha_cap_task_partial_hash_get_supported(devinfo, SHA_ALGORITHM);
}

doca_error_t file_integrity_init(struct file_integrity_config *app_cfg,
				 struct program_core_objects *state,
				 struct doca_sha **sha_ctx)
{
	uint32_t max_bufs = 2; /* The app will use 2 doca buffers */
	doca_error_t result;

	/* set default timeout */
	if (app_cfg->timeout == 0)
		app_cfg->timeout = DEFAULT_TIMEOUT;

	/* Open device for partial SHA tasks */
	result = open_doca_device_with_capabilities(&sha_partial_hash_is_supported, &state->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA device with SHA capabilities: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sha_create(state->dev, sha_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init sha library: %s", doca_error_get_descr(result));
		/* Destroying core objects closes state->dev, ignoring objects not yet created */
		goto destroy_core_objs;
	}

	state->ctx = doca_sha_as_ctx(*sha_ctx);

	result = create_core_objects(state, max_bufs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA core objects: %s", doca_error_get_descr(result));
		goto destroy_sha;
	}

	/* Connect context to progress engine */
	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to context: %s", doca_error_get_descr(result));
		goto destroy_sha;
	}

	if (app_cfg->mode == CLIENT) {
		/* Set SHA hash task configuration for the client */
		result = doca_sha_task_hash_set_conf(*sha_ctx,
						     sha_hash_completed_callback,
						     sha_hash_error_callback,
						     LOG_NUM_SHA_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set configuration for SHA hash task: %s", doca_error_get_descr(result));
			goto destroy_sha;
		}

	} else {
		result = doca_sha_task_hash_set_conf(*sha_ctx,
						     sha_hash_completed_callback,
						     sha_hash_error_callback,
						     LOG_NUM_SHA_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set configuration for SHA hash task: %s", doca_error_get_descr(result));
			goto destroy_sha;
		}

		/* Set SHA partial hash task configuration for the client */
		result = doca_sha_task_partial_hash_set_conf(*sha_ctx,
							     sha_partial_hash_completed_callback,
							     sha_partial_hash_error_callback,
							     LOG_NUM_SHA_TASKS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set configuration for SHA partial hash task: %s",
				     doca_error_get_descr(result));
			goto destroy_sha;
		}
	}

	return DOCA_SUCCESS;

destroy_sha:
	(void)doca_sha_destroy(*sha_ctx);
	state->ctx = NULL;
destroy_core_objs:
	destroy_core_objects(state);

	return result;
}

void file_integrity_cleanup(struct program_core_objects *state, struct doca_sha *sha_ctx)
{
	doca_error_t result;

	if (state->pe != NULL && state->ctx != NULL) {
		result = request_stop_ctx(state->pe, state->ctx);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA SHA: %s", doca_error_get_descr(result));
		state->ctx = NULL;
	}

	if (sha_ctx != NULL) {
		result = doca_sha_destroy(sha_ctx);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA SHA: %s", doca_error_get_descr(result));
	}

	result = destroy_core_objects(state);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy core objects: %s", doca_error_get_descr(result));
}

/*
 * ARGP Callback - Handle file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t file_callback(void *param, void *config)
{
	struct file_integrity_config *app_cfg = (struct file_integrity_config *)config;
	char *file_path = (char *)param;

	if (strnlen(file_path, MAX_FILE_NAME) == MAX_FILE_NAME) {
		DOCA_LOG_ERR("File name is too long - MAX=%d", MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->file_path, file_path, MAX_FILE_NAME);
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
	struct file_integrity_config *app_cfg = (struct file_integrity_config *)config;
	char *dev_pci_addr = (char *)param;

	if (strnlen(dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE) == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->cc_dev_pci_addr, dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);
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
	struct file_integrity_config *app_cfg = (struct file_integrity_config *)config;
	const char *rep_pci_addr = (char *)param;

	if (app_cfg->mode == SERVER) {
		if (strnlen(rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE) == DOCA_DEVINFO_REP_PCI_ADDR_SIZE) {
			DOCA_LOG_ERR("Entered device representor PCI address exceeding the maximum size of %d",
				     DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}

		strlcpy(app_cfg->cc_dev_rep_pci_addr, rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE);
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle timeout parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t timeout_callback(void *param, void *config)
{
	struct file_integrity_config *app_cfg = (struct file_integrity_config *)config;
	int *timeout = (int *)param;

	if (*timeout <= 0) {
		DOCA_LOG_ERR("Timeout parameter must be positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}
	app_cfg->timeout = *timeout;
	return DOCA_SUCCESS;
}

/*
 * ARGP validation Callback - check if the running mode is valid and that the input file exists in client mode
 *
 * @cfg [in]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t args_validation_callback(void *cfg)
{
	struct file_integrity_config *app_cfg = (struct file_integrity_config *)cfg;

	if (app_cfg->mode == CLIENT && (access(app_cfg->file_path, F_OK) == -1)) {
		DOCA_LOG_ERR("File was not found %s", app_cfg->file_path);
		return DOCA_ERROR_NOT_FOUND;
	} else if (app_cfg->mode == SERVER && strlen(app_cfg->cc_dev_rep_pci_addr) == 0) {
		DOCA_LOG_ERR("Missing PCI address for server");
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

doca_error_t register_file_integrity_params(void)
{
	doca_error_t result;

	struct doca_argp_param *dev_pci_addr_param, *rep_pci_addr_param, *file_param, *timeout_param;

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
	doca_argp_param_set_description(rep_pci_addr_param, "DOCA Comch device representor PCI address");
	doca_argp_param_set_callback(rep_pci_addr_param, rep_pci_addr_callback);
	doca_argp_param_set_type(rep_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(rep_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register message to send param */
	result = doca_argp_param_create(&file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(file_param, "f");
	doca_argp_param_set_long_name(file_param, "file");
	doca_argp_param_set_description(file_param, "File to send by the client / File to write by the server");
	doca_argp_param_set_callback(file_param, file_callback);
	doca_argp_param_set_type(file_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(file_param);
	result = doca_argp_register_param(file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register timeout */
	result = doca_argp_param_create(&timeout_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(timeout_param, "t");
	doca_argp_param_set_long_name(timeout_param, "timeout");
	doca_argp_param_set_description(timeout_param,
					"Application timeout for receiving file content messages, default is 5 sec");
	doca_argp_param_set_callback(timeout_param, timeout_callback);
	doca_argp_param_set_type(timeout_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(timeout_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register version callback for DOCA SDK & RUNTIME */
	result = doca_argp_register_version_callback(sdk_version_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register version callback: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register application callback */
	result = doca_argp_register_validation_callback(args_validation_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program validation callback: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}
