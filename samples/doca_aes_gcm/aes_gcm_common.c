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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include <doca_ctx.h>
#include <doca_log.h>
#include <doca_error.h>
#include <doca_pe.h>
#include <doca_argp.h>
#include <doca_aes_gcm.h>

#include "../common.h"
#include "aes_gcm_common.h"

DOCA_LOG_REGISTER(AES_GCM::COMMON);

/*
 * Initialize AES-GCM parameters for the sample.
 *
 * @file_path [in]: Default file path name
 * @aes_gcm_cfg [in]: AES-GCM configuration struct
 */
void init_aes_gcm_params(struct aes_gcm_cfg *aes_gcm_cfg)
{
	strcpy(aes_gcm_cfg->output_path, "/tmp/out.txt");
	strcpy(aes_gcm_cfg->pci_address, "03:00.0");
	memset(aes_gcm_cfg->raw_key, 0, MAX_AES_GCM_KEY_SIZE);
	aes_gcm_cfg->raw_key_type = DOCA_AES_GCM_KEY_256;
	memset(aes_gcm_cfg->iv, 0, MAX_AES_GCM_IV_LENGTH);
	aes_gcm_cfg->iv_length = MAX_AES_GCM_IV_LENGTH;
	aes_gcm_cfg->tag_size = AES_GCM_AUTH_TAG_96_SIZE_IN_BYTES;
	aes_gcm_cfg->aad_size = 0;
	aes_gcm_cfg->num_dst_buf = 1;
	aes_gcm_cfg->num_src_buf = 1;
}

/*
 * Parse hex string to array of uint8_t
 *
 * @hex_str [in]: hex format string
 * @hex_str_size [in]: the hex string length
 * @bytes_arr [out]: the parsed bytes array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_hex_to_bytes(const char *hex_str, size_t hex_str_size, uint8_t *bytes_arr)
{
	uint8_t digit;
	size_t i;

	/* Parse every digit (nibble) and translate it to the matching numeric value */
	for (i = 0; i < hex_str_size; i++) {
		/* Must be alpha-numeric */
		if ('0' <= hex_str[i] && hex_str[i] <= '9')
			digit = hex_str[i] - '0';
		else if ('a' <= tolower(hex_str[i]) && tolower(hex_str[i]) <= 'f')
			digit = tolower(hex_str[i]) - 'a' + 10;
		else {
			DOCA_LOG_ERR("Wrong format for input (%s) - need to be in hex format (1-9) or (a-f) values",
				     hex_str);
			return DOCA_ERROR_INVALID_VALUE;
		}
		/* There are 2 nibbles (digits) in each byte, place them at their numeric place */
		bytes_arr[i / 2] = (bytes_arr[i / 2] << 4) + digit;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(aes_gcm_cfg->pci_address, pci_address, len + 1);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle user file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t file_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, MAX_FILE_NAME);
	if (len >= MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid file name length, max %d", USER_MAX_FILE_NAME);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(aes_gcm_cfg->file_path, file);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle output file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t output_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, MAX_FILE_NAME);
	if (len >= MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid file name length, max %d", USER_MAX_FILE_NAME);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(aes_gcm_cfg->output_path, file);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle raw key parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t raw_key_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;
	char *raw_key = (char *)param;
	doca_error_t result;
	int len;

	len = strnlen(raw_key, MAX_AES_GCM_KEY_STR_SIZE);
	if ((len != AES_GCM_KEY_128_STR_SIZE) && (len != AES_GCM_KEY_256_STR_SIZE)) {
		DOCA_LOG_ERR(
			"Invalid string length %d to represent a key, string length should be %d or %d characters long",
			len,
			AES_GCM_KEY_128_STR_SIZE,
			AES_GCM_KEY_256_STR_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	result = parse_hex_to_bytes(raw_key, len, aes_gcm_cfg->raw_key);
	if (result != DOCA_SUCCESS)
		return result;
	aes_gcm_cfg->raw_key_type = (len == AES_GCM_KEY_128_STR_SIZE) ? DOCA_AES_GCM_KEY_128 : DOCA_AES_GCM_KEY_256;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle initialization vector parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t iv_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;
	char *iv = (char *)param;
	doca_error_t result;
	int len;

	len = strnlen(iv, MAX_AES_GCM_IV_STR_LENGTH);
	if (len == MAX_AES_GCM_IV_STR_LENGTH) {
		DOCA_LOG_ERR(
			"Invalid string length %d to represent the initialization vector, max string length should be %d",
			len,
			(MAX_AES_GCM_IV_STR_LENGTH - 1));
		return DOCA_ERROR_INVALID_VALUE;
	}
	result = parse_hex_to_bytes(iv, len, aes_gcm_cfg->iv);
	if (result != DOCA_SUCCESS)
		return result;
	aes_gcm_cfg->iv_length = (len / 2) + (len % 2);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle authentication tag parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t tag_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;
	uint32_t tag_size = *(uint32_t *)param;

	if ((tag_size != AES_GCM_AUTH_TAG_96_SIZE_IN_BYTES) && (tag_size != AES_GCM_AUTH_TAG_128_SIZE_IN_BYTES)) {
		DOCA_LOG_ERR("Invalid authentication tag size %d, tag size can be %d bytes or %d bytes",
			     tag_size,
			     AES_GCM_AUTH_TAG_96_SIZE_IN_BYTES,
			     AES_GCM_AUTH_TAG_128_SIZE_IN_BYTES);
		return DOCA_ERROR_INVALID_VALUE;
	}
	aes_gcm_cfg->tag_size = tag_size;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle additional authenticated data parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t aad_callback(void *param, void *config)
{
	struct aes_gcm_cfg *aes_gcm_cfg = (struct aes_gcm_cfg *)config;

	aes_gcm_cfg->aad_size = *(uint32_t *)param;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle number of source doca_buf parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_src_buf_callback(void *param, void *config)
{
	struct aes_gcm_cfg *conf = (struct aes_gcm_cfg *)config;
	const int *num = (int *)param;

	if (*num <= 0) {
		DOCA_LOG_ERR("Entered number of source doca_buf: %d, valid value must be >= 1", *num);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->num_src_buf = *num;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle number of destination doca_buf parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_dst_buf_callback(void *param, void *config)
{
	struct aes_gcm_cfg *conf = (struct aes_gcm_cfg *)config;
	const int *num = (int *)param;

	if (*num <= 0) {
		DOCA_LOG_ERR("Entered number of destination doca_buf: %d, valid value must be >= 1", *num);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->num_dst_buf = *num;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_aes_gcm_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *file_param, *output_param, *raw_key_param, *iv_param, *tag_size_param,
		*aad_size_param;
	struct doca_argp_param *num_src_buf_param, *num_dst_buf_param;

	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI device address - default: 03:00.0");
	doca_argp_param_set_callback(pci_param, pci_address_callback);
	doca_argp_param_set_type(pci_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(file_param, "f");
	doca_argp_param_set_long_name(file_param, "file");
	doca_argp_param_set_description(file_param, "Input file to encrypt/decrypt");
	doca_argp_param_set_mandatory(file_param);
	doca_argp_param_set_callback(file_param, file_callback);
	doca_argp_param_set_type(file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&output_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(output_param, "o");
	doca_argp_param_set_long_name(output_param, "output");
	doca_argp_param_set_description(output_param, "Output file - default: /tmp/out.txt");
	doca_argp_param_set_callback(output_param, output_callback);
	doca_argp_param_set_type(output_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(output_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&raw_key_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(raw_key_param, "k");
	doca_argp_param_set_long_name(raw_key_param, "key");
	doca_argp_param_set_description(
		raw_key_param,
		"Raw key to encrypt/decrypt with, represented in hex format (32 characters for 128-bit key, and 64 for 256-bit key) - default: 256-bit key, equals to zero");
	doca_argp_param_set_callback(raw_key_param, raw_key_callback);
	doca_argp_param_set_type(raw_key_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(raw_key_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&iv_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(iv_param, "i");
	doca_argp_param_set_long_name(iv_param, "iv");
	doca_argp_param_set_description(
		iv_param,
		"Initialization vector, represented in hex format (0-24 characters for 0-96-bit IV) - default: 96-bit IV, equals to zero");
	doca_argp_param_set_callback(iv_param, iv_callback);
	doca_argp_param_set_type(iv_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(iv_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&tag_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(tag_size_param, "t");
	doca_argp_param_set_long_name(tag_size_param, "tag-size");
	doca_argp_param_set_description(
		tag_size_param,
		"Authentication tag size. Tag size is in bytes and can be 12B or 16B - default: 12");
	doca_argp_param_set_callback(tag_size_param, tag_callback);
	doca_argp_param_set_type(tag_size_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(tag_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&aad_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(aad_size_param, "a");
	doca_argp_param_set_long_name(aad_size_param, "aad-size");
	doca_argp_param_set_description(aad_size_param, "Additional authenticated data size - default: 0");
	doca_argp_param_set_callback(aad_size_param, aad_callback);
	doca_argp_param_set_type(aad_size_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(aad_size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of source doca_buf param */
	result = doca_argp_param_create(&num_src_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(num_src_buf_param, "ns");
	doca_argp_param_set_long_name(num_src_buf_param, "num-src-buf");
	doca_argp_param_set_description(num_src_buf_param, "number of doca_buf for source buffer");
	doca_argp_param_set_callback(num_src_buf_param, num_src_buf_callback);
	doca_argp_param_set_type(num_src_buf_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_src_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of destination doca_buf param */
	result = doca_argp_param_create(&num_dst_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(num_dst_buf_param, "nd");
	doca_argp_param_set_long_name(num_dst_buf_param, "num-dst-buf");
	doca_argp_param_set_description(num_dst_buf_param, "number of doca_buf for destination buffer");
	doca_argp_param_set_callback(num_dst_buf_param, num_dst_buf_callback);
	doca_argp_param_set_type(num_dst_buf_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_dst_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Callback triggered whenever AES-GCM state changes
 *
 * @user_data [in]: User data associated with the AES-GCM context. Will hold struct aes_gcm_resources *
 * @ctx [in]: The AES-GCM context that had a state change
 * @prev_state [in]: Previous context state
 * @next_state [in]: Next context state (context is already in this state when the callback is called)
 */
static void aes_gcm_state_changed_callback(const union doca_data user_data,
					   struct doca_ctx *ctx,
					   enum doca_ctx_states prev_state,
					   enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct aes_gcm_resources *resources = (struct aes_gcm_resources *)user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		DOCA_LOG_INFO("AES-GCM context has been stopped");
		/* We can stop progressing the PE */
		resources->run_pe_progress = false;
		break;
	case DOCA_CTX_STATE_STARTING:
		/**
		 * The context is in starting state, this is unexpected for AES-GCM.
		 */
		DOCA_LOG_ERR("AES-GCM context entered into starting state. Unexpected transition");
		break;
	case DOCA_CTX_STATE_RUNNING:
		DOCA_LOG_INFO("AES-GCM context is running");
		break;
	case DOCA_CTX_STATE_STOPPING:
		/**
		 * doca_ctx_stop() has been called.
		 * In this sample, this happens either due to a failure encountered, in which case doca_pe_progress()
		 * will cause any inflight task to be flushed, or due to the successful compilation of the sample flow.
		 * In both cases, in this sample, doca_pe_progress() will eventually transition the context to idle
		 * state.
		 */
		DOCA_LOG_INFO("AES-GCM context entered into stopping state. Any inflight tasks will be flushed");
		break;
	default:
		break;
	}
}

doca_error_t allocate_aes_gcm_resources(const char *pci_addr, uint32_t max_bufs, struct aes_gcm_resources *resources)
{
	struct program_core_objects *state = NULL;
	union doca_data ctx_user_data = {0};
	doca_error_t result, tmp_result;
	uint32_t max_buf_list_len = 0;

	resources->state = malloc(sizeof(*resources->state));
	if (resources->state == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate DOCA program core objects: %s", doca_error_get_descr(result));
		return result;
	}
	resources->num_remaining_tasks = 0;

	state = resources->state;

	/* Open DOCA device */
	if (pci_addr != NULL) {
		/* If pci_addr was provided then open using it */
		if (resources->mode == AES_GCM_MODE_ENCRYPT)
			result = open_doca_device_with_pci(pci_addr, &aes_gcm_task_encrypt_is_supported, &state->dev);
		else
			result = open_doca_device_with_pci(pci_addr, &aes_gcm_task_decrypt_is_supported, &state->dev);
	} else {
		/* If pci_addr was not provided then look for DOCA device */
		if (resources->mode == AES_GCM_MODE_ENCRYPT)
			result = open_doca_device_with_capabilities(&aes_gcm_task_encrypt_is_supported, &state->dev);
		else
			result = open_doca_device_with_capabilities(&aes_gcm_task_decrypt_is_supported, &state->dev);
	}

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device for DOCA AES-GCM: %s", doca_error_get_descr(result));
		goto free_state;
	}

	if (resources->mode == AES_GCM_MODE_ENCRYPT) {
		result = doca_aes_gcm_cap_task_encrypt_get_max_list_buf_num_elem(doca_dev_as_devinfo(state->dev),
										 &max_buf_list_len);
	} else {
		result = doca_aes_gcm_cap_task_decrypt_get_max_list_buf_num_elem(doca_dev_as_devinfo(state->dev),
										 &max_buf_list_len);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get task max_buf_list_len capability: %s", doca_error_get_descr(result));
		return result;
	}
	if (max_bufs > max_buf_list_len) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Number of doca_buf [%d] exceed the limitation [%u]: %s",
			     max_bufs,
			     max_buf_list_len,
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_aes_gcm_create(state->dev, &resources->aes_gcm);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create AES-GCM engine: %s", doca_error_get_descr(result));
		goto close_device;
	}

	state->ctx = doca_aes_gcm_as_ctx(resources->aes_gcm);

	result = create_core_objects(state, max_bufs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create DOCA core objects: %s", doca_error_get_descr(result));
		goto destroy_aes_gcm;
	}

	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set progress engine for PE: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	result = doca_ctx_set_state_changed_cb(state->ctx, aes_gcm_state_changed_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set AES-GCM state change callback: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	if (resources->mode == AES_GCM_MODE_ENCRYPT)
		result = doca_aes_gcm_task_encrypt_set_conf(resources->aes_gcm,
							    encrypt_completed_callback,
							    encrypt_error_callback,
							    NUM_AES_GCM_TASKS);
	else
		result = doca_aes_gcm_task_decrypt_set_conf(resources->aes_gcm,
							    decrypt_completed_callback,
							    decrypt_error_callback,
							    NUM_AES_GCM_TASKS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set configurations for AES-GCM task: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	/* Include resources in user data of context to be used in callbacks */
	ctx_user_data.ptr = resources;
	result = doca_ctx_set_user_data(state->ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set user data for AES-GCM ctx: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	return result;

destroy_core_objects:
	tmp_result = destroy_core_objects(state);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA core objects: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_aes_gcm:
	tmp_result = doca_aes_gcm_destroy(resources->aes_gcm);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA AES-GCM: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_device:
	tmp_result = doca_dev_close(state->dev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(tmp_result));
	}
free_state:
	free(resources->state);

	return result;
}

doca_error_t destroy_aes_gcm_resources(struct aes_gcm_resources *resources)
{
	struct program_core_objects *state = resources->state;
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	if (resources->aes_gcm != NULL) {
		result = doca_ctx_stop(state->ctx);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Unable to stop context: %s", doca_error_get_descr(result));
		state->ctx = NULL;

		tmp_result = doca_aes_gcm_destroy(resources->aes_gcm);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA AES-GCM: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	tmp_result = destroy_core_objects(state);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA core objects: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	free(state);
	resources->state = NULL;

	return result;
}

doca_error_t submit_aes_gcm_encrypt_task(struct aes_gcm_resources *resources,
					 struct doca_buf *src_buf,
					 struct doca_buf *dst_buf,
					 struct doca_aes_gcm_key *key,
					 const uint8_t *iv,
					 uint32_t iv_length,
					 uint32_t tag_size,
					 uint32_t aad_size)
{
	struct doca_aes_gcm_task_encrypt *encrypt_task;
	struct program_core_objects *state = resources->state;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result, task_result;

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct encrypt task */
	result = doca_aes_gcm_task_encrypt_alloc_init(resources->aes_gcm,
						      src_buf,
						      dst_buf,
						      key,
						      iv,
						      iv_length,
						      tag_size,
						      aad_size,
						      task_user_data,
						      &encrypt_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate encrypt task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_aes_gcm_task_encrypt_as_task(encrypt_task);

	/* Submit encrypt task */
	resources->num_remaining_tasks++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit encrypt task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		return result;
	}

	resources->run_pe_progress = true;

	/* Wait for all tasks to be completed and context to stop */
	while (resources->run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	return task_result;
}

doca_error_t submit_aes_gcm_decrypt_task(struct aes_gcm_resources *resources,
					 struct doca_buf *src_buf,
					 struct doca_buf *dst_buf,
					 struct doca_aes_gcm_key *key,
					 const uint8_t *iv,
					 uint32_t iv_length,
					 uint32_t tag_size,
					 uint32_t aad_size)
{
	struct doca_aes_gcm_task_decrypt *decrypt_task;
	struct program_core_objects *state = resources->state;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result, task_result;

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct decrypt task */
	result = doca_aes_gcm_task_decrypt_alloc_init(resources->aes_gcm,
						      src_buf,
						      dst_buf,
						      key,
						      iv,
						      iv_length,
						      tag_size,
						      aad_size,
						      task_user_data,
						      &decrypt_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate decrypt task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_aes_gcm_task_decrypt_as_task(decrypt_task);

	/* Submit decrypt task */
	resources->num_remaining_tasks++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit decrypt task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		return result;
	}

	resources->run_pe_progress = true;

	/* Wait for all tasks to be completed and context to stop */
	while (resources->run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	return task_result;
}

doca_error_t aes_gcm_task_encrypt_is_supported(struct doca_devinfo *devinfo)
{
	return doca_aes_gcm_cap_task_encrypt_is_supported(devinfo);
}

doca_error_t aes_gcm_task_decrypt_is_supported(struct doca_devinfo *devinfo)
{
	return doca_aes_gcm_cap_task_decrypt_is_supported(devinfo);
}

void encrypt_completed_callback(struct doca_aes_gcm_task_encrypt *encrypt_task,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	struct aes_gcm_resources *resources = (struct aes_gcm_resources *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	DOCA_LOG_INFO("Encrypt task was done successfully");

	/* Assign success to the result */
	*result = DOCA_SUCCESS;
	/* Free task */
	doca_task_free(doca_aes_gcm_task_encrypt_as_task(encrypt_task));
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void encrypt_error_callback(struct doca_aes_gcm_task_encrypt *encrypt_task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data)
{
	struct aes_gcm_resources *resources = (struct aes_gcm_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_aes_gcm_task_encrypt_as_task(encrypt_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Get the result of the task */
	*result = doca_task_get_status(task);
	DOCA_LOG_ERR("Encrypt task failed: %s", doca_error_get_descr(*result));
	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void decrypt_completed_callback(struct doca_aes_gcm_task_decrypt *decrypt_task,
				union doca_data task_user_data,
				union doca_data ctx_user_data)
{
	struct aes_gcm_resources *resources = (struct aes_gcm_resources *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	DOCA_LOG_INFO("Decrypt task was done successfully");

	/* Assign success to the result */
	*result = DOCA_SUCCESS;
	/* Free task */
	doca_task_free(doca_aes_gcm_task_decrypt_as_task(decrypt_task));
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void decrypt_error_callback(struct doca_aes_gcm_task_decrypt *decrypt_task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data)
{
	struct aes_gcm_resources *resources = (struct aes_gcm_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_aes_gcm_task_decrypt_as_task(decrypt_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Get the result of the task */
	*result = doca_task_get_status(task);
	DOCA_LOG_ERR("Decrypt task failed: %s", doca_error_get_descr(*result));
	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}
