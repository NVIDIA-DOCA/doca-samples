/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_aes_gcm.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include "aes_gcm_common.h"

DOCA_LOG_REGISTER(AES_GCM_ENCRYPT);

/*
 * Run aes_gcm_encrypt sample
 *
 * @cfg [in]: Configuration parameters
 * @file_data [in]: file data for the encrypt task
 * @file_size [in]: file size
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t aes_gcm_encrypt(struct aes_gcm_cfg *cfg, char *file_data, size_t file_size)
{
	struct aes_gcm_resources resources = {0};
	struct program_core_objects *state = NULL;
	struct doca_buf *src_doca_buf = NULL;
	struct doca_buf *dst_doca_buf = NULL;
	uint32_t max_bufs = cfg->num_dst_buf + cfg->num_src_buf;
	char *dst_buffer = NULL;
	uint8_t *resp_head = NULL;
	size_t data_len = 0;
	char *dump = NULL;
	FILE *out_file = NULL;
	struct doca_aes_gcm_key *key = NULL;
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp_result = DOCA_SUCCESS;
	uint64_t max_encrypt_buf_size = 0;

	out_file = fopen(cfg->output_path, "wr");
	if (out_file == NULL) {
		DOCA_LOG_ERR("Unable to open output file: %s", cfg->output_path);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Allocate resources */
	resources.mode = AES_GCM_MODE_ENCRYPT;
	result = allocate_aes_gcm_resources(cfg->pci_address, max_bufs, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate AES-GCM resources: %s", doca_error_get_descr(result));
		goto close_file;
	}

	state = resources.state;

	result = doca_aes_gcm_cap_task_encrypt_get_max_buf_size(doca_dev_as_devinfo(state->dev), &max_encrypt_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query AES-GCM encrypt max buf size: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	if (file_size > max_encrypt_buf_size) {
		DOCA_LOG_ERR("File size %zu > max buffer size %zu", file_size, max_encrypt_buf_size);
		goto destroy_resources;
	}

	/* Start AES-GCM context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	dst_buffer = calloc(1, max_encrypt_buf_size);
	if (dst_buffer == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate memory: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, max_encrypt_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}
	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}

	result = doca_mmap_set_memrange(state->src_mmap, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}

	/* Construct DOCA buffer for each address range */
	result = allocat_doca_buf_list(state->buf_inv,
				       state->src_mmap,
				       file_data,
				       file_size,
				       cfg->num_src_buf,
				       true,
				       &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		goto free_dst_buf;
	}

	/* Construct DOCA buffer for each address range */
	result = allocat_doca_buf_list(state->buf_inv,
				       state->dst_mmap,
				       dst_buffer,
				       max_encrypt_buf_size,
				       cfg->num_dst_buf,
				       false,
				       &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		goto destroy_src_buf;
	}

	/* Create DOCA AES-GCM key */
	result = doca_aes_gcm_key_create(resources.aes_gcm, cfg->raw_key, cfg->raw_key_type, &key);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create DOCA AES-GCM key: %s", doca_error_get_descr(result));
		goto destroy_dst_buf;
	}

	/* Submit AES-GCM encrypt task */
	result = submit_aes_gcm_encrypt_task(&resources,
					     src_doca_buf,
					     dst_doca_buf,
					     key,
					     (uint8_t *)cfg->iv,
					     cfg->iv_length,
					     cfg->tag_size,
					     cfg->aad_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("AES-GCM encrypt task failed: %s", doca_error_get_descr(result));
		goto destroy_key;
	}

	/* Write the result to output file */
	doca_buf_get_head(dst_doca_buf, (void **)&resp_head);
	doca_buf_get_data_len(dst_doca_buf, &data_len);
	fwrite(resp_head, sizeof(uint8_t), data_len, out_file);
	DOCA_LOG_INFO("File was encrypted successfully and saved in: %s", cfg->output_path);

	/* Print destination buffer data */
	dump = hex_dump(resp_head, data_len);
	if (dump == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for printing buffer content");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_key;
	}

	DOCA_LOG_INFO("AES-GCM encrypted data:\n%s", dump);
	free(dump);

destroy_key:
	tmp_result = doca_aes_gcm_key_destroy(key);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA AES-GCM key: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_dst_buf:
	tmp_result = doca_buf_dec_refcount(dst_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease DOCA destination buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_src_buf:
	tmp_result = doca_buf_dec_refcount(src_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease DOCA source buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_dst_buf:
	free(dst_buffer);
destroy_resources:
	tmp_result = destroy_aes_gcm_resources(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy AES-GCM resources: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_file:
	fclose(out_file);

	return result;
}
