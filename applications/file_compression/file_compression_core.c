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
#include <zlib.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <pack.h>
#include <utils.h>

#include "file_compression_core.h"

#define MAX_MSG 512			   /* Maximum number of messages in CC queue */
#define SW_MAX_FILE_SIZE 128 * 1024 * 1024 /* 128 MB */
#define SLEEP_IN_NANOS (10 * 1000)	   /* Sample the task every 10 microseconds */
#define DECOMPRESS_RATIO 1032		   /* Maximal decompress ratio size */
#define DEFAULT_TIMEOUT 10		   /* default timeout for receiving messages */

DOCA_LOG_REGISTER(FILE_COMPRESSION::Core);

struct file_info_message {
	uint64_t checksum; /* Checksum of file to be transferred */
	uint32_t num_segs; /* Number of comch segments to transfer file across */
};

/*
 * Get DOCA compress maximum buffer size allowed
 *
 * @resources [in]: DOCA compress resources pointer
 * @max_buf_size [out]: Maximum buffer size allowed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_compress_max_buf_size(struct compress_resources *resources, uint64_t *max_buf_size)
{
	struct doca_devinfo *compress_dev_info = doca_dev_as_devinfo(resources->state->dev);
	doca_error_t result;

	if (resources->mode == COMPRESS_MODE_COMPRESS_DEFLATE)
		result = doca_compress_cap_task_compress_deflate_get_max_buf_size(compress_dev_info, max_buf_size);
	else
		result = doca_compress_cap_task_decompress_deflate_get_max_buf_size(compress_dev_info, max_buf_size);

	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to retrieve maximum buffer size allowed from DOCA compress device");
	else
		DOCA_LOG_DBG("DOCA compress device supports maximum buffer size of %" PRIu64 " bytes", *max_buf_size);

	return result;
}

/*
 * Allocate DOCA compress needed resources with 2 buffers
 *
 * @mode [in]: Running mode
 * @resources [out]: DOCA compress resources pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_compress_resources(enum file_compression_mode mode, struct compress_resources *resources)
{
	uint32_t max_bufs = 2;
	doca_error_t result;

	if (mode == CLIENT)
		resources->mode = COMPRESS_MODE_COMPRESS_DEFLATE;
	else
		resources->mode = COMPRESS_MODE_DECOMPRESS_DEFLATE;

	result = allocate_compress_resources(NULL, max_bufs, resources);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to allocate compress resources: %s", doca_error_get_descr(result));

	return result;
}

/*
 * Initiate DOCA compress needed flows
 *
 * @compress_cfg [in]: application config struct
 * @resources [out]: DOCA compress resources pointer
 * @method [out]: Compress method to execute
 * @max_buf_size [out]: Maximum compress buffer size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_compress_resources(struct file_compression_config *compress_cfg,
					    struct compress_resources *resources,
					    enum file_compression_compress_method *method,
					    uint64_t *max_buf_size)
{
	doca_error_t result;

	/* Default is to use HW compress */
	*method = COMPRESS_DEFLATE_HW;

	/* Allocate compress resources */
	result = get_compress_resources(compress_cfg->mode, resources);
	if (result != DOCA_SUCCESS) {
		if (resources->mode == COMPRESS_MODE_COMPRESS_DEFLATE) {
			DOCA_LOG_INFO("Failed to find device for compress task, running SW compress with zlib");
			*method = COMPRESS_DEFLATE_SW;
			*max_buf_size = SW_MAX_FILE_SIZE;
			result = DOCA_SUCCESS;
		} else if (resources->mode == COMPRESS_MODE_DECOMPRESS_DEFLATE)
			DOCA_LOG_ERR("Failed to allocate compress resources: %s", doca_error_get_descr(result));
	} else {
		result = get_compress_max_buf_size(resources, max_buf_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to retrieve DOCA compress device maximum buffer size: %s",
				     doca_error_get_descr(result));
			result = destroy_compress_resources(resources);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to destroy DOCA compress resources: %s",
					     doca_error_get_descr(result));
		}
	}

	return result;
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
 * Populate destination doca buffer for compress tasks
 *
 * @state [in]: application configuration struct
 * @dst_buffer [in]: destination buffer
 * @dst_buf_size [in]: destination buffer size
 * @dst_doca_buf [out]: created doca buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t populate_dst_buf(struct program_core_objects *state,
				     uint8_t *dst_buffer,
				     size_t dst_buf_size,
				     struct doca_buf **dst_doca_buf)
{
	doca_error_t result;

	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, dst_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range destination memory map: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start destination memory map: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    state->dst_mmap,
						    dst_buffer,
						    dst_buf_size,
						    dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		return result;
	}
	return result;
}

/*
 * Allocate DOCA compress resources and submit compress/decompress task
 *
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @dst_buf_size [in]: allocated destination buffer length
 * @resources [in]: DOCA compress resources
 * @compressed_file [out]: destination buffer with the result
 * @compressed_file_len [out]: destination buffer size
 * @output_chksum [out]: the returned checksum
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t compress_file_hw(char *file_data,
				     size_t file_size,
				     size_t dst_buf_size,
				     struct compress_resources *resources,
				     uint8_t *compressed_file,
				     size_t *compressed_file_len,
				     uint64_t *output_chksum)
{
	struct doca_buf *dst_doca_buf;
	struct doca_buf *src_doca_buf;
	uint8_t *resp_head;
	struct program_core_objects *state = resources->state;
	doca_error_t result, tmp_result;

	/* Start compress context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_memrange(state->src_mmap, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set memory range of source memory map: %s", doca_error_get_descr(result));
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
		return result;
	}

	result =
		doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->src_mmap, file_data, file_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		return result;
	}

	doca_buf_get_data(src_doca_buf, (void **)&resp_head);
	doca_buf_set_data(src_doca_buf, resp_head, file_size);

	result = populate_dst_buf(state, compressed_file, dst_buf_size, &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to populate destination buffer: %s", doca_error_get_descr(result));
		goto dec_src_buf;
	}

	if (resources->mode == COMPRESS_MODE_COMPRESS_DEFLATE)
		result = submit_compress_deflate_task(resources, src_doca_buf, dst_doca_buf, output_chksum);
	else
		result = submit_decompress_deflate_task(resources, src_doca_buf, dst_doca_buf, output_chksum);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit %s task: %s",
			     (resources->mode == COMPRESS_MODE_COMPRESS_DEFLATE) ? "compress" : "decompress",
			     doca_error_get_descr(result));
		goto dec_dst_buf;
	}

	doca_buf_get_data_len(dst_doca_buf, compressed_file_len);

dec_dst_buf:
	tmp_result = doca_buf_dec_refcount(dst_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease DOCA destination buffer count: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
dec_src_buf:
	tmp_result = doca_buf_dec_refcount(src_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease DOCA source buffer count: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*
 * Compress the input file in SW
 *
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @dst_buf_size [in]: allocated destination buffer length
 * @compressed_file [out]: destination buffer with the result
 * @compressed_file_len [out]: destination buffer size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t compress_file_sw(char *file_data,
				     size_t file_size,
				     size_t dst_buf_size,
				     Byte **compressed_file,
				     uLong *compressed_file_len)
{
	z_stream c_stream; /* compression stream */
	int err;

	memset(&c_stream, 0, sizeof(c_stream));

	c_stream.zalloc = NULL;
	c_stream.zfree = NULL;

	err = deflateInit2(&c_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
	if (err < 0) {
		DOCA_LOG_ERR("Failed to initialize compression system");
		return DOCA_ERROR_BAD_STATE;
	}

	c_stream.next_in = (z_const unsigned char *)file_data;
	c_stream.next_out = *compressed_file;

	c_stream.avail_in = file_size;
	c_stream.avail_out = dst_buf_size;
	err = deflate(&c_stream, Z_NO_FLUSH);
	if (err < 0) {
		DOCA_LOG_ERR("Failed to compress file");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Finish the stream */
	err = deflate(&c_stream, Z_FINISH);
	if (err < 0 || err != Z_STREAM_END) {
		DOCA_LOG_ERR("Failed to compress file");
		return DOCA_ERROR_BAD_STATE;
	}

	err = deflateEnd(&c_stream);
	if (err < 0) {
		DOCA_LOG_ERR("Failed to compress file");
		return DOCA_ERROR_BAD_STATE;
	}
	*compressed_file_len = c_stream.total_out;
	return DOCA_SUCCESS;
}

/*
 * Calculate file checksum with zlib, where the lower 32 bits contain the CRC checksum result
 * and the upper 32 bits contain the Adler checksum result.
 *
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @output_chksum [out]: the calculated checksum
 */
static void calculate_checksum_sw(char *file_data, size_t file_size, uint64_t *output_chksum)
{
	uint32_t crc;
	uint32_t adler;
	uint64_t result_checksum;

	crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, (const unsigned char *)file_data, file_size);
	adler = adler32(0L, Z_NULL, 0);
	adler = adler32(adler, (const unsigned char *)file_data, file_size);

	result_checksum = adler;
	result_checksum <<= 32;
	result_checksum += crc;

	*output_chksum = result_checksum;
}

/*
 * Compress / decompress the input file data
 *
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @max_buf_size [in]: maximum compress buffer size allowed
 * @resources [in]: DOCA compress resources
 * @method [in]: Compression method to be used
 * @compressed_file [out]: destination buffer with the result
 * @compressed_file_len [out]: destination buffer size
 * @output_chksum [out]: the calculated checksum
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t compress_file(char *file_data,
				  size_t file_size,
				  uint64_t max_buf_size,
				  struct compress_resources *resources,
				  enum file_compression_compress_method method,
				  uint8_t **compressed_file,
				  size_t *compressed_file_len,
				  uint64_t *output_chksum)
{
	size_t dst_buf_size = 0;

	enum compress_mode;

	if (resources->mode == COMPRESS_MODE_COMPRESS_DEFLATE) {
		dst_buf_size = MAX(file_size + 16, file_size * 2);
		if (dst_buf_size > max_buf_size)
			dst_buf_size = max_buf_size;
	} else if (resources->mode == COMPRESS_MODE_DECOMPRESS_DEFLATE)
		dst_buf_size = MIN(max_buf_size, DECOMPRESS_RATIO * file_size);

	*compressed_file = calloc(1, dst_buf_size);
	if (*compressed_file == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	if (method == COMPRESS_DEFLATE_SW) {
		calculate_checksum_sw(file_data, file_size, output_chksum);
		return compress_file_sw(file_data, file_size, dst_buf_size, compressed_file, compressed_file_len);
	} else
		return compress_file_hw(file_data,
					file_size,
					dst_buf_size,
					resources,
					*compressed_file,
					compressed_file_len,
					output_chksum);
}

/*
 * Send the input file with comch to the server in segments of max_comch_msg length
 *
 * @compress_cfg [in]: compression configuration information
 * @comch_cfg [in]: comch configuration object for sending file across
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @checksum [in]: checksum of the file
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_file(struct file_compression_config *compress_cfg,
			      struct comch_cfg *comch_cfg,
			      char *file_data,
			      size_t file_size,
			      uint64_t checksum)
{
	struct file_info_message file_meta = {};
	uint32_t max_comch_msg;
	uint32_t total_msgs;
	size_t msg_len;
	uint32_t i;
	doca_error_t result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	max_comch_msg = comch_utils_get_max_buffer_size(comch_cfg);
	if (max_comch_msg == 0) {
		DOCA_LOG_ERR("Comch max buffer size is zero");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Send to the server the number of messages needed for receiving the file and its checksum */
	total_msgs = (file_size + max_comch_msg - 1) / max_comch_msg;
	file_meta.num_segs = htonl(total_msgs);
	file_meta.checksum = htonq(checksum);

	result = comch_utils_send(comch_util_get_connection(comch_cfg), &file_meta, sizeof(struct file_info_message));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send file info message: %s", doca_error_get_descr(result));
		return result;
	}

	/* Send file to the server */
	for (i = 0; i < total_msgs; i++) {
		/* Stop sending if the server is done receiving */
		if (compress_cfg->state == TRANSFER_COMPLETE)
			return DOCA_ERROR_IO_FAILED;

		msg_len = MIN(file_size, max_comch_msg);
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
	struct file_compression_config *cfg = comch_utils_get_user_data(comch_connection);

	/* A message is only expected from the server when it has read the compressed file successfully */
	(void)event;

	/* Print the completion message sent from the server */
	recv_buffer[msg_len] = '\0';
	DOCA_LOG_INFO("Received message: %s", recv_buffer);
	cfg->state = TRANSFER_COMPLETE;
}

doca_error_t file_compression_client(struct comch_cfg *comch_cfg,
				     struct file_compression_config *compress_cfg,
				     struct compress_resources *resources)
{
	char *file_data;
	struct stat statbuf;
	int fd;
	uint8_t *compressed_file;
	size_t compressed_file_len;
	uint64_t checksum;
	doca_error_t result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	fd = open(compress_cfg->file_path, O_RDWR);
	if (fd < 0) {
		DOCA_LOG_ERR("Failed to open %s", compress_cfg->file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (fstat(fd, &statbuf) < 0) {
		DOCA_LOG_ERR("Failed to get file information");
		close(fd);
		return DOCA_ERROR_IO_FAILED;
	}

	if (statbuf.st_size == 0 || (uint64_t)statbuf.st_size > compress_cfg->max_compress_file_len) {
		DOCA_LOG_ERR("Invalid file size. Should be greater then zero and smaller than %" PRIu64 " bytes",
			     compress_cfg->max_compress_file_len);
		close(fd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	file_data = mmap(NULL, statbuf.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (file_data == MAP_FAILED) {
		DOCA_LOG_ERR("Unable to map file content: %s", strerror(errno));
		close(fd);
		return DOCA_ERROR_NO_MEMORY;
	}

	DOCA_LOG_TRC("File size: %ld", statbuf.st_size);
	/* Send compress task */
	result = compress_file(file_data,
			       statbuf.st_size,
			       compress_cfg->max_compress_file_len,
			       resources,
			       compress_cfg->compress_method,
			       &compressed_file,
			       &compressed_file_len,
			       &checksum);
	if (result != DOCA_SUCCESS) {
		close(fd);
		free(compressed_file);
		return result;
	}
	close(fd);
	DOCA_LOG_TRC("Compressed file size: %ld", compressed_file_len);

	/* Send the file content to the server */
	result = send_file(compress_cfg, comch_cfg, (char *)compressed_file, compressed_file_len, checksum);
	if (result != DOCA_SUCCESS) {
		free(compressed_file);
		return result;
	}
	free(compressed_file);

	/* Wait for a signal that the transfer has complete */
	while (compress_cfg->state != TRANSFER_COMPLETE) {
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
	struct file_compression_config *cfg = comch_utils_get_user_data(comch_connection);
	struct server_runtime_data *server_data;

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
		struct file_info_message *file_info = (struct file_info_message *)recv_buffer;

		if (msg_len != sizeof(struct file_info_message)) {
			DOCA_LOG_ERR("Unexpected file info message received. Size %u, expected size %lu",
				     msg_len,
				     sizeof(struct file_info_message));
			cfg->state = TRANSFER_ERROR;
			return;
		}

		server_data->expected_file_chunks = ntohl(file_info->num_segs);
		server_data->expected_checksum = ntohq(file_info->checksum);
		cfg->state = TRANSFER_IN_PROGRESS;
		return;
	}

	if (server_data->received_file_length + msg_len > cfg->max_compress_file_len) {
		DOCA_LOG_ERR("Received file exceeded maximum file size. Received length: %u, maximum size: %lu",
			     server_data->received_file_length + msg_len,
			     cfg->max_compress_file_len);
		cfg->state = TRANSFER_ERROR;
		return;
	}
	memcpy(server_data->compressed_file + server_data->received_file_length, recv_buffer, msg_len);

	server_data->received_file_chunks += 1;
	server_data->received_file_length += msg_len;

	if (server_data->received_file_chunks == server_data->expected_file_chunks)
		cfg->state = TRANSFER_COMPLETE;
}

doca_error_t file_compression_server(struct comch_cfg *comch_cfg,
				     struct file_compression_config *compress_cfg,
				     struct compress_resources *resources)
{
	struct server_runtime_data *server_data;
	int fd;
	char finish_msg[] = "Server was done receiving messages";
	uint8_t *resp_head;
	uint64_t checksum;
	size_t data_len;
	int counter = 0;
	int num_of_iterations = (compress_cfg->timeout * 1000 * 1000) / (SLEEP_IN_NANOS / 1000);
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;

	server_data = (struct server_runtime_data *)&compress_cfg->server_data;

	/* Wait on comch to complete client to server transactions */
	while (compress_cfg->state != TRANSFER_COMPLETE && compress_cfg->state != TRANSFER_ERROR) {
		nanosleep(&ts, &ts);
		result = comch_utils_progress_connection(comch_util_get_connection(comch_cfg));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Comch connection unexpectedly dropped: %s", doca_error_get_descr(result));
			return result;
		}

		if (compress_cfg->state == TRANSFER_IDLE)
			continue;

		counter++;
		if (counter == num_of_iterations) {
			DOCA_LOG_ERR("Message was not received at the given timeout");
			result = DOCA_ERROR_BAD_STATE;
			goto finish_msg;
		}
	}

	if (compress_cfg->state == TRANSFER_ERROR) {
		DOCA_LOG_ERR("Error detected during comch exchange");
		result = DOCA_ERROR_BAD_STATE;
		goto finish_msg;
	}

	result = compress_file(server_data->compressed_file,
			       server_data->received_file_length,
			       compress_cfg->max_compress_file_len,
			       resources,
			       compress_cfg->compress_method,
			       &resp_head,
			       &data_len,
			       &checksum);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decompress the received file: %s", doca_error_get_descr(result));
		free(resp_head);
		goto finish_msg;
	}
	if (checksum == server_data->expected_checksum)
		DOCA_LOG_INFO("SUCCESS: file was received and decompressed successfully");
	else {
		DOCA_LOG_ERR("ERROR: file checksum is different. received: 0x%lx, calculated: 0x%lx",
			     server_data->expected_checksum,
			     checksum);
		free(resp_head);
		result = DOCA_ERROR_BAD_STATE;
		goto finish_msg;
	}

	fd = open(compress_cfg->file_path, O_CREAT | O_WRONLY, S_IRUSR | S_IRGRP);
	if (fd < 0) {
		DOCA_LOG_ERR("Failed to open %s", compress_cfg->file_path);
		free(resp_head);
		result = DOCA_ERROR_IO_FAILED;
		goto finish_msg;
	}

	if ((size_t)write(fd, resp_head, data_len) != data_len) {
		DOCA_LOG_ERR("Failed to write the decompressed into the input file");
		free(resp_head);
		close(fd);
		result = DOCA_ERROR_IO_FAILED;
		goto finish_msg;
	}
	free(resp_head);
	close(fd);

finish_msg:
	if (comch_utils_send(comch_util_get_connection(comch_cfg), finish_msg, sizeof(finish_msg)) != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to send finish message: %s", doca_error_get_descr(result));

	return result;
}

doca_error_t file_compression_init(struct file_compression_config *compress_cfg, struct compress_resources *resources)
{
	doca_error_t result;

	/* set default timeout */
	if (compress_cfg->timeout == 0)
		compress_cfg->timeout = DEFAULT_TIMEOUT;

	result = init_compress_resources(compress_cfg,
					 resources,
					 &compress_cfg->compress_method,
					 &compress_cfg->max_compress_file_len);
	if (result != DOCA_SUCCESS)
		return result;

	/* Server should preallocate memory to receive a file */
	if (compress_cfg->mode == SERVER) {
		compress_cfg->server_data.compressed_file = calloc(1, compress_cfg->max_compress_file_len);
		if (compress_cfg->server_data.compressed_file == NULL) {
			DOCA_LOG_ERR("Failed to allocate file memory");
			(void)destroy_compress_resources(resources);
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	return DOCA_SUCCESS;
}

void file_compression_cleanup(struct file_compression_config *compress_cfg, struct compress_resources *resources)
{
	doca_error_t result;

	(void)compress_cfg;

	result = destroy_compress_resources(resources);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy compress resources: %s", doca_error_get_descr(result));

	if (compress_cfg->mode == SERVER) {
		free(compress_cfg->server_data.compressed_file);
		compress_cfg->server_data.compressed_file = NULL;
	}
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
	struct file_compression_config *compress_cfg = (struct file_compression_config *)config;
	char *file_path = (char *)param;

	if (strnlen(file_path, MAX_FILE_NAME) == MAX_FILE_NAME) {
		DOCA_LOG_ERR("File name is too long - MAX=%d", MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(compress_cfg->file_path, file_path, MAX_FILE_NAME);
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
	struct file_compression_config *compress_cfg = (struct file_compression_config *)config;
	char *pci_addr = (char *)param;

	if (strnlen(pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE) == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(compress_cfg->cc_dev_pci_addr, pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);
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
	struct file_compression_config *compress_cfg = (struct file_compression_config *)config;
	const char *rep_pci_addr = (char *)param;

	if (compress_cfg->mode == SERVER) {
		if (strnlen(rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE) == DOCA_DEVINFO_REP_PCI_ADDR_SIZE) {
			DOCA_LOG_ERR("Entered device representor PCI address exceeding the maximum size of %d",
				     DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}

		strlcpy(compress_cfg->cc_dev_rep_pci_addr, rep_pci_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE);
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
	struct file_compression_config *compress_cfg = (struct file_compression_config *)config;
	int *timeout = (int *)param;

	if (*timeout <= 0) {
		DOCA_LOG_ERR("Timeout parameter must be positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}
	compress_cfg->timeout = *timeout;
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
	struct file_compression_config *compress_cfg = (struct file_compression_config *)cfg;

	if (compress_cfg->mode == CLIENT && (access(compress_cfg->file_path, F_OK) == -1)) {
		DOCA_LOG_ERR("File was not found %s", compress_cfg->file_path);
		return DOCA_ERROR_NOT_FOUND;
	} else if (compress_cfg->mode == SERVER && strlen(compress_cfg->cc_dev_rep_pci_addr) == 0) {
		DOCA_LOG_ERR("Missing representor PCI address for server");
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

doca_error_t register_file_compression_params(void)
{
	doca_error_t result;

	struct doca_argp_param *dev_pci_addr_param, *rep_pci_addr_param, *file_param, *timeout_param;

	/* Create and register pci param */
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

	/* Create and register rep PCI address param */
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
