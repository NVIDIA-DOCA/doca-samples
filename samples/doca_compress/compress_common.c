/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_log.h>
#include <doca_error.h>
#include <doca_pe.h>
#include <doca_argp.h>
#include <doca_compress.h>

#include "../common.h"
#include "compress_common.h"

DOCA_LOG_REGISTER(COMPRESS::COMMON);

/*
 * Compression Method and Flags (CMF) defines for the Zlib header
 */
#define ZLIB_CMF_CM 8 /* Compression Method - DEFLATE compression */
#define ZLIB_CMF_CINFO \
	7			 /* For DEFLATE compression (CM=8), CINFO is the base-2 logarithm of the LZ77 \
				  * window size, minus eight. CINFO=7 indicates a 32K window size \
				  */
#define ZLIB_CMF_CM_MASK 0x0F	 /* Mask for Compression Method bits in the CMF byte */
#define ZLIB_CMF_CINFO_MASK 0xF0 /* Mask for Compression Info bits in the CMF byte */
#define ZLIB_CMF_CINFO_SHIFT 4	 /* Shift for Compression Info bits in the CMF byte */

/*
 *  Flags (FLG) defines for the Zlib header
 */
#define ZLIB_FLG_FLEVEL 2 /* Use the default algorithm for the DEFLATE compression method (CM=8) */
#define ZLIB_FLG_FDICT \
	0			  /* Represents whether the DICT dictionary identifier is present immediately \
				   * after the FLG byte. It indicates that no preset dictionary is used \
				   */
#define ZLIB_FLG_FCHECK_MASK 0x1F /* Mask for FCHECK bits in the FLG byte */
#define ZLIB_FLG_FDICT_MASK 0x20  /* Mask for FDICT flag in the FLG byte */
#define ZLIB_FLG_FDICT_SHIFT 5	  /* Shift for FDICT flag in the FLG byte */
#define ZLIB_FLG_FLEVEL_MASK 0xC0 /* Mask for FLEVEL bits in the FLG byte */
#define ZLIB_FLG_FLEVEL_SHIFT 6	  /* Shift for FLEVEL bits in the FLG byte */

/*
 * LZ4 frame format defines
 */
#define LZ4_MAGIC_NUMBER 0x184D2204 /* Magic number for LZ4 files */
#define LZ4_VERSION_NUMER 0x01	    /* The LZ4 frame format version */

#define LZ4_FLAGS_DICT_ID_MASK 0x1	      /* Mask for dictionary ID bits in the FLG byte */
#define LZ4_FLAGS_CONTENT_CHECKSUM_SHIFT 2    /* Shift for content checksum flag bit in the FLG byte */
#define LZ4_FLAGS_CONTENT_CHECKSUM_MASK 0x4   /* Mask for content checksum flag bit in the FLG byte */
#define LZ4_FLAGS_CONTENT_SIZE_SHIFT 3	      /* Shift for content size flag bit in the FLG byte */
#define LZ4_FLAGS_CONTENT_SIZE_MASK 0x8	      /* Mask for content size flag bit in the FLG byte */
#define LZ4_FLAGS_BLOCK_CHECKSUM_SHIFT 4      /* Shift for block checksum flag bit in the FLG byte */
#define LZ4_FLAGS_BLOCK_CHECKSUM_MASK 0x10    /* Mask for block checksum flag bit in the FLG byte */
#define LZ4_FLAGS_BLOCK_INDEPENDENT_SHIFT 5   /* Shift for block independent flag bit in the FLG byte */
#define LZ4_FLAGS_BLOCK_INDEPENDENT_MASK 0x20 /* Mask for block independent flag bit in the FLG byte */
#define LZ4_FLAGS_VERSION_NUMBER_SHIFT 6      /* Shift for version number bits in the FLG byte */
#define LZ4_FLAGS_VERSION_NUMBER_MASK 0xC0    /* Mask for version number bits in the FLG byte */

#define LZ4_END_MARK_LEN 4 /* The length of the end mark */

/* Describes result of a compress/decompress deflate task */
struct compress_deflate_result {
	doca_error_t status; /**< The completion status */
	uint32_t crc_cs;     /**< The CRC checksum */
	uint32_t adler_cs;   /**< The Adler Checksum */
};

/* Describes result of a decompress lz4 tasks */
struct compress_lz4_result {
	doca_error_t status; /**< The completion status */
	uint32_t crc_cs;     /**< The CRC checksum */
	uint32_t xxh_cs;     /**< The xxHash Checksum */
};

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(compress_cfg->pci_address, pci_address, len + 1);
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
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, MAX_FILE_NAME);
	if (len == MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid file name length, max %d", USER_MAX_FILE_NAME);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(compress_cfg->file_path, file);
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
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, MAX_FILE_NAME);
	if (len == MAX_FILE_NAME) {
		DOCA_LOG_ERR("Invalid file name length, max %d", USER_MAX_FILE_NAME);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(compress_cfg->output_path, file);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle output checksum parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t output_checksum_callback(void *param, void *config)
{
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	bool output_checksum = *((bool *)param);

	compress_cfg->output_checksum = output_checksum;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_compress_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *file_param, *output_param, *output_checksum_param;

	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI device address");
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
	doca_argp_param_set_description(file_param, "Input file to compress/decompress");
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
	doca_argp_param_set_description(output_param, "Output file");
	doca_argp_param_set_callback(output_param, output_callback);
	doca_argp_param_set_type(output_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(output_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&output_checksum_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(output_checksum_param, "c");
	doca_argp_param_set_long_name(output_checksum_param, "output-checksum");
	doca_argp_param_set_description(output_checksum_param, "Output checksum");
	doca_argp_param_set_callback(output_checksum_param, output_checksum_callback);
	doca_argp_param_set_type(output_checksum_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(output_checksum_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle with-frame parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t is_with_frame_callback(void *param, void *config)
{
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	bool is_with_frame = *((bool *)param);

	compress_cfg->is_with_frame = is_with_frame;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_deflate_params(void)
{
	doca_error_t result;
	struct doca_argp_param *is_with_frame;

	result = doca_argp_param_create(&is_with_frame);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(is_with_frame, "wf");
	doca_argp_param_set_long_name(is_with_frame, "with-frame");
	doca_argp_param_set_description(is_with_frame,
					"Write/read a file with a frame, compatible with default zlib settings");
	doca_argp_param_set_callback(is_with_frame, is_with_frame_callback);
	doca_argp_param_set_type(is_with_frame, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(is_with_frame);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle has_block_checksum parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t has_block_checksum_callback(void *param, void *config)
{
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	bool has_block_checksum = *((bool *)param);

	compress_cfg->has_block_checksum = has_block_checksum;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle are_blocks_independent parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t are_blocks_independent_callback(void *param, void *config)
{
	struct compress_cfg *compress_cfg = (struct compress_cfg *)config;
	bool are_blocks_independent = *((bool *)param);

	compress_cfg->are_blocks_independent = are_blocks_independent;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_lz4_stream_params(void)
{
	doca_error_t result;
	struct doca_argp_param *is_with_frame;
	struct doca_argp_param *has_block_checksum;
	struct doca_argp_param *are_blocks_independent;

	result = doca_argp_param_create(&is_with_frame);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(is_with_frame, "wf");
	doca_argp_param_set_long_name(is_with_frame, "with-frame");
	doca_argp_param_set_description(is_with_frame, "Read a file compatible with an LZ4 frame");
	doca_argp_param_set_callback(is_with_frame, is_with_frame_callback);
	doca_argp_param_set_type(is_with_frame, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(is_with_frame);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&has_block_checksum);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(has_block_checksum, "bc");
	doca_argp_param_set_long_name(has_block_checksum, "has-block-checksum");
	doca_argp_param_set_description(
		has_block_checksum,
		"Flag to indicate if blocks have a checksum. Valid only when the sample is run without the with-frame(wf) flag");
	doca_argp_param_set_callback(has_block_checksum, has_block_checksum_callback);
	doca_argp_param_set_type(has_block_checksum, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(has_block_checksum);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&are_blocks_independent);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(are_blocks_independent, "bi");
	doca_argp_param_set_long_name(are_blocks_independent, "are-blocks-independent");
	doca_argp_param_set_description(
		are_blocks_independent,
		"Flag to indicate if blocks are independent. Valid only when the sample is run without the with-frame(wf) flag");
	doca_argp_param_set_callback(are_blocks_independent, are_blocks_independent_callback);
	doca_argp_param_set_type(are_blocks_independent, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(are_blocks_independent);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Compute the FCHECK value for the filled zlib header.
 * The FCHECK value must be such that CMF and FLG, when viewed as a 16-bit unsigned integer stored in MSB order
 * (CMF*256 + FLG), is a multiple of 31
 *
 * @zlib_header [in]: A Zlib header to initiate with DOCA Compress default settings
 * @return: The FCHECK value of a given zlib header
 */
static inline uint8_t compute_zlib_header_fcheck(struct compress_zlib_header *zlib_header)
{
	uint16_t sum = htobe16(*(uint16_t *)zlib_header) & ~ZLIB_FLG_FCHECK_MASK;
	uint8_t fcheck = (31 - (sum % 31));

	return fcheck;
}

/*
 * Initiate the fields of the zlib header with default values
 */
void init_compress_zlib_header(struct compress_zlib_header *zlib_header)
{
	zlib_header->cmf = (ZLIB_CMF_CINFO << ZLIB_CMF_CINFO_SHIFT);
	zlib_header->cmf |= ZLIB_CMF_CM;

	zlib_header->flg = (ZLIB_FLG_FLEVEL << ZLIB_FLG_FLEVEL_SHIFT);
	zlib_header->flg |= (ZLIB_FLG_FDICT << ZLIB_FLG_FDICT_SHIFT);
	zlib_header->flg |= compute_zlib_header_fcheck(zlib_header);
}

/*
 * Verify the header values are valid and compatible with DOCA compress
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t verify_compress_zlib_header(struct compress_zlib_header *zlib_header)
{
	uint8_t computed_fcheck = compute_zlib_header_fcheck(zlib_header);

	if ((zlib_header->flg & ZLIB_FLG_FCHECK_MASK) != computed_fcheck) {
		DOCA_LOG_ERR("Invalid header: header FCHECK=%u doesn't match expected FCHECK=%u",
			     (zlib_header->flg & ZLIB_FLG_FCHECK_MASK),
			     computed_fcheck);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (((zlib_header->flg & ZLIB_FLG_FDICT_MASK) >> ZLIB_FLG_FDICT_SHIFT) != ZLIB_FLG_FDICT) {
		DOCA_LOG_ERR("Invalid header: DOCA compress doesn't support the use of dictionary identifiers");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (((zlib_header->flg & ZLIB_FLG_FLEVEL_MASK) >> ZLIB_FLG_FLEVEL_SHIFT) != ZLIB_FLG_FLEVEL) {
		DOCA_LOG_ERR(
			"Invalid header: DOCA compress supports only a default algorithm (FLEVEL=%u) yet FLEVEL=%u was given",
			ZLIB_FLG_FLEVEL,
			((zlib_header->flg & ZLIB_FLG_FLEVEL_MASK) >> ZLIB_FLG_FLEVEL_SHIFT));
		return DOCA_ERROR_INVALID_VALUE;
	}

	if ((zlib_header->cmf & ZLIB_CMF_CM_MASK) != ZLIB_CMF_CM) {
		DOCA_LOG_ERR(
			"Invalid header: DOCA compress supports only DEFLATE compress method (CM=%u) yet CM=%u was given",
			ZLIB_CMF_CM,
			(zlib_header->cmf & ZLIB_CMF_CM_MASK));
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (((zlib_header->cmf & ZLIB_CMF_CINFO_MASK) >> ZLIB_CMF_CINFO_SHIFT) > ZLIB_CMF_CINFO) {
		DOCA_LOG_ERR(
			"Invalid header: the given window size (CINFO=%u) may not be smaller than the window size used to compress with DOCA (CINFO=%u)",
			((zlib_header->cmf & ZLIB_CMF_CINFO_MASK) >> ZLIB_CMF_CINFO_SHIFT),
			ZLIB_CMF_CINFO);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

doca_error_t parse_lz4_frame(struct doca_buf *src_buf,
			     struct compress_cfg *cfg,
			     bool *has_content_checksum,
			     uint32_t *content_checksum)
{
	doca_error_t result = DOCA_SUCCESS;
	bool has_content_size = false;
	bool has_content_checksum_flag = false;
	bool has_dict_id = false;
	uint8_t flags = 0;
	uint8_t version_number = 0;
	uint32_t magic_number = 0;
	uint64_t content_size = 0;
	size_t src_buf_len = 0;
	size_t total_frame_length = 0;
	void *src_buf_addr = NULL;
	uint8_t *start_addr = NULL;

	uint32_t frame_header_length = 0;
	uint32_t frame_footer_length = 0;

	result = doca_buf_get_head(src_buf, &src_buf_addr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get buffer's address: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_buf_get_len(src_buf, &src_buf_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get buffer's length: %s", doca_error_get_descr(result));
		return result;
	}

	start_addr = (uint8_t *)src_buf_addr;

	magic_number = (*(uint32_t *)start_addr);
	frame_header_length += sizeof(uint32_t);
	if (le32toh(magic_number) != LZ4_MAGIC_NUMBER) {
		DOCA_LOG_ERR("Invalid header: magic number=%x doesn't match the expected LZ4 magic number=%x",
			     le32toh(magic_number),
			     LZ4_MAGIC_NUMBER);
		return DOCA_ERROR_INVALID_VALUE;
	}

	flags = *(start_addr + frame_header_length);
	frame_header_length += sizeof(uint8_t);

	version_number = (flags & LZ4_FLAGS_VERSION_NUMBER_MASK) >> LZ4_FLAGS_VERSION_NUMBER_SHIFT;
	if (version_number != LZ4_VERSION_NUMER) {
		DOCA_LOG_ERR("Invalid header: version=%x doesn't match the expected LZ4 version=%x",
			     version_number,
			     LZ4_VERSION_NUMER);
		return DOCA_ERROR_INVALID_VALUE;
	}

	cfg->are_blocks_independent = (flags & LZ4_FLAGS_BLOCK_INDEPENDENT_MASK) >> LZ4_FLAGS_BLOCK_INDEPENDENT_SHIFT;
	cfg->has_block_checksum = (flags & LZ4_FLAGS_BLOCK_CHECKSUM_MASK) >> LZ4_FLAGS_BLOCK_CHECKSUM_SHIFT;

	has_content_size = (flags & LZ4_FLAGS_CONTENT_SIZE_MASK) >> LZ4_FLAGS_CONTENT_SIZE_SHIFT;
	has_content_checksum_flag = (flags & LZ4_FLAGS_CONTENT_CHECKSUM_MASK) >> LZ4_FLAGS_CONTENT_CHECKSUM_SHIFT;
	if (has_content_checksum != NULL)
		*has_content_checksum = has_content_checksum_flag;

	has_dict_id = flags & LZ4_FLAGS_DICT_ID_MASK;
	if (has_dict_id) {
		DOCA_LOG_ERR("Data compressed using a dictionary is not supported");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	frame_header_length += sizeof(uint8_t); // BD byte - value is not utilized by DOCA Compress

	if (has_content_size) {
		content_size = *((uint64_t *)(start_addr + frame_header_length));
		frame_header_length += sizeof(uint64_t);
	}

	// Header checksum - For the simplicity of this sample, the value is not checked
	frame_header_length += sizeof(uint8_t);

	frame_footer_length += LZ4_END_MARK_LEN;
	if (has_content_checksum_flag) {
		if (content_checksum != NULL)
			*content_checksum = htobe32(*((uint32_t *)(start_addr + src_buf_len - frame_footer_length)));

		frame_footer_length += has_content_checksum_flag * sizeof(uint32_t);
	}

	src_buf_addr = (void *)(start_addr + frame_header_length);

	total_frame_length = (frame_header_length + frame_footer_length);
	if (total_frame_length > src_buf_len) { // Check file includes content Frame
		DOCA_LOG_ERR("Invalid header: Frame expected size=%lu is longer than file size =%lu",
			     total_frame_length,
			     src_buf_len);
		return DOCA_ERROR_INVALID_VALUE;
	}
	src_buf_len -= total_frame_length;
	// Check file includes at least 1 block frame
	if ((sizeof(uint32_t) * (1 + cfg->has_block_checksum)) > src_buf_len) {
		DOCA_LOG_ERR("File size=%lu is smaller than the minimum expected size =%lu",
			     src_buf_len + total_frame_length,
			     (sizeof(uint32_t) * (1 + cfg->has_block_checksum)) + total_frame_length);
		return DOCA_ERROR_INVALID_VALUE;
	}

	// Check content file is as expected
	if (has_content_size && (src_buf_len != content_size)) {
		DOCA_LOG_ERR("Invalid header: content size=%lu doesn't match the expected=%lu",
			     content_size,
			     src_buf_len);
		return DOCA_ERROR_INVALID_VALUE;
	}

	// Set buffer data section to include only the content, without the frame
	result = doca_buf_set_data(src_buf, src_buf_addr, src_buf_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set buffer's data address and length: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * Callback triggered whenever Compress context state changes
 *
 * @user_data [in]: User data associated with the Compress context. Will hold struct compress_resources *
 * @ctx [in]: The Compress context that had a state change
 * @prev_state [in]: Previous context state
 * @next_state [in]: Next context state (context is already in this state when the callback is called)
 */
static void compress_state_changed_callback(const union doca_data user_data,
					    struct doca_ctx *ctx,
					    enum doca_ctx_states prev_state,
					    enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct compress_resources *resources = (struct compress_resources *)user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		DOCA_LOG_INFO("Compress context has been stopped");
		/* We can stop progressing the PE */
		resources->run_pe_progress = false;
		break;
	case DOCA_CTX_STATE_STARTING:
		/**
		 * The context is in starting state, this is unexpected for Compress.
		 */
		DOCA_LOG_ERR("Compress context entered into starting state. Unexpected transition");
		break;
	case DOCA_CTX_STATE_RUNNING:
		DOCA_LOG_INFO("Compress context is running");
		break;
	case DOCA_CTX_STATE_STOPPING:
		/**
		 * doca_ctx_stop() has been called.
		 * In this sample, this happens either due to a failure encountered, in which case doca_pe_progress()
		 * will cause any inflight task to be flushed, or due to the successful compilation of the sample flow.
		 * In both cases, in this sample, doca_pe_progress() will eventually transition the context to idle
		 * state.
		 */
		DOCA_LOG_INFO("Compress context entered into stopping state. Any inflight tasks will be flushed");

		break;
	default:
		break;
	}
}

doca_error_t allocate_compress_resources(const char *pci_addr, uint32_t max_bufs, struct compress_resources *resources)
{
	struct program_core_objects *state = NULL;
	union doca_data ctx_user_data = {0};
	doca_error_t result, tmp_result;
	tasks_check supported_check_func;

	resources->state = malloc(sizeof(*resources->state));
	if (resources->state == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate DOCA program core objects: %s", doca_error_get_descr(result));
		return result;
	}
	resources->num_remaining_tasks = 0;

	state = resources->state;

	switch (resources->mode) {
	case COMPRESS_MODE_COMPRESS_DEFLATE:
		supported_check_func = compress_task_compress_deflate_is_supported;
		break;
	case COMPRESS_MODE_DECOMPRESS_DEFLATE:
		supported_check_func = compress_task_decompress_deflate_is_supported;
		break;
	case COMPRESS_MODE_DECOMPRESS_LZ4_STREAM:
		supported_check_func = compress_task_decompress_lz4_stream_is_supported;
		break;
	default:
		DOCA_LOG_ERR("Unknown compress mode: %d", resources->mode);
		result = DOCA_ERROR_INVALID_VALUE;
		goto free_state;
	}

	/* Open DOCA device */
	if (pci_addr != NULL) {
		/* If pci_addr was provided then open using it */
		result = open_doca_device_with_pci(pci_addr, supported_check_func, &state->dev);
	} else {
		/* If pci_addr was not provided then look for DOCA device */
		result = open_doca_device_with_capabilities(supported_check_func, &state->dev);
	}

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device for DOCA compress: %s", doca_error_get_descr(result));
		goto free_state;
	}

	result = doca_compress_create(state->dev, &resources->compress);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create compress engine: %s", doca_error_get_descr(result));
		goto close_device;
	}

	state->ctx = doca_compress_as_ctx(resources->compress);

	result = create_core_objects(state, max_bufs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create DOCA core objects: %s", doca_error_get_descr(result));
		goto destroy_compress;
	}

	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set progress engine for PE: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	result = doca_ctx_set_state_changed_cb(state->ctx, compress_state_changed_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set Compress state change callback: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	switch (resources->mode) {
	case COMPRESS_MODE_COMPRESS_DEFLATE:
		result = doca_compress_task_compress_deflate_set_conf(resources->compress,
								      compress_completed_callback,
								      compress_error_callback,
								      NUM_COMPRESS_TASKS);
		break;
	case COMPRESS_MODE_DECOMPRESS_DEFLATE:
		result = doca_compress_task_decompress_deflate_set_conf(resources->compress,
									decompress_deflate_completed_callback,
									decompress_deflate_error_callback,
									NUM_COMPRESS_TASKS);
		break;
	case COMPRESS_MODE_DECOMPRESS_LZ4_STREAM:
		result = doca_compress_task_decompress_lz4_stream_set_conf(resources->compress,
									   decompress_lz4_stream_completed_callback,
									   decompress_lz4_stream_error_callback,
									   NUM_COMPRESS_TASKS);
		break;
	default:
		DOCA_LOG_ERR("Unknown compress mode: %d", resources->mode);
		result = DOCA_ERROR_INVALID_VALUE;
		goto free_state;
	}

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set configurations for compress task: %s", doca_error_get_descr(result));
		goto destroy_core_objects;
	}

	/* Include resources in user data of context to be used in callbacks */
	ctx_user_data.ptr = resources;
	doca_ctx_set_user_data(state->ctx, ctx_user_data);

	return result;

destroy_core_objects:
	tmp_result = destroy_core_objects(state);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA core objects: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_compress:
	tmp_result = doca_compress_destroy(resources->compress);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA compress: %s", doca_error_get_descr(tmp_result));
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
	resources->state = NULL;
	return result;
}

doca_error_t destroy_compress_resources(struct compress_resources *resources)
{
	struct program_core_objects *state = resources->state;
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	if (resources->compress != NULL) {
		result = doca_ctx_stop(state->ctx);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Unable to stop context: %s", doca_error_get_descr(result));
		state->ctx = NULL;

		tmp_result = doca_compress_destroy(resources->compress);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA compress: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	if (resources->state != NULL) {
		tmp_result = destroy_core_objects(state);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA core objects: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
		free(state);
		resources->state = NULL;
	}

	return result;
}

/*
 * Calculate the checksum where the lower 32 bits contain the CRC checksum result
 * and the upper 32 bits contain the Adler checksum result.
 *
 * @crc_checksum [in]: DOCA compress resources
 * @adler_checksum [in]: Source buffer
 * @return: The calculated checksum
 */
static uint64_t calculate_checksum(uint32_t crc_checksum, uint32_t adler_checksum)
{
	uint64_t checksum;

	checksum = (uint64_t)adler_checksum;
	checksum <<= ADLER_CHECKSUM_SHIFT;
	checksum += (uint64_t)crc_checksum;

	return checksum;
}

doca_error_t submit_compress_deflate_task(struct compress_resources *resources,
					  struct doca_buf *src_buf,
					  struct doca_buf *dst_buf,
					  uint64_t *output_checksum)
{
	struct doca_compress_task_compress_deflate *compress_task;
	struct program_core_objects *state = resources->state;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct compress_deflate_result task_result = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct compress task */
	result = doca_compress_task_compress_deflate_alloc_init(resources->compress,
								src_buf,
								dst_buf,
								task_user_data,
								&compress_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate compress task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_compress_task_compress_deflate_as_task(compress_task);

	/* Submit compress task */
	resources->num_remaining_tasks++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit compress task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		return result;
	}

	resources->run_pe_progress = true;

	/* Wait for all tasks to be completed */
	while (resources->run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Check result of task according to the result we update in the callbacks */
	if (task_result.status != DOCA_SUCCESS)
		return task_result.status;

	/* Calculate checksum if needed */
	if (output_checksum != NULL)
		*output_checksum = calculate_checksum(task_result.crc_cs, task_result.adler_cs);

	return result;
}

doca_error_t submit_decompress_deflate_task(struct compress_resources *resources,
					    struct doca_buf *src_buf,
					    struct doca_buf *dst_buf,
					    uint64_t *output_checksum)
{
	struct doca_compress_task_decompress_deflate *decompress_task;
	struct program_core_objects *state = resources->state;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct compress_deflate_result task_result = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct decompress task */
	result = doca_compress_task_decompress_deflate_alloc_init(resources->compress,
								  src_buf,
								  dst_buf,
								  task_user_data,
								  &decompress_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate decompress task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_compress_task_decompress_deflate_as_task(decompress_task);

	/* Submit decompress task */
	resources->num_remaining_tasks++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit decompress task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		return result;
	}

	resources->run_pe_progress = true;

	/* Wait for all tasks to be completed */
	while (resources->run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Check result of task according to the result we update in the callbacks */
	if (task_result.status != DOCA_SUCCESS)
		return task_result.status;

	/* Calculate checksum if needed */
	if (output_checksum != NULL)
		*output_checksum = calculate_checksum(task_result.crc_cs, task_result.adler_cs);

	return result;
}

doca_error_t submit_decompress_lz4_stream_task(struct compress_resources *resources,
					       uint8_t has_block_checksum,
					       uint8_t are_blocks_independent,
					       struct doca_buf *src_buf,
					       struct doca_buf *dst_buf,
					       uint32_t *output_crc_checksum,
					       uint32_t *output_xxh_checksum)
{
	struct doca_compress_task_decompress_lz4_stream *decompress_task;
	struct program_core_objects *state = resources->state;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct compress_lz4_result task_result = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result;

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct decompress task */
	result = doca_compress_task_decompress_lz4_stream_alloc_init(resources->compress,
								     has_block_checksum,
								     are_blocks_independent,
								     src_buf,
								     dst_buf,
								     task_user_data,
								     &decompress_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate decompress task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_compress_task_decompress_lz4_stream_as_task(decompress_task);

	/* Submit decompress task */
	resources->num_remaining_tasks++;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit decompress task: %s", doca_error_get_descr(result));
		doca_task_free(task);
		return result;
	}

	resources->run_pe_progress = true;

	/* Wait for all tasks to be completed */
	while (resources->run_pe_progress) {
		if (doca_pe_progress(state->pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Check result of task according to the result we update in the callbacks */
	if (task_result.status != DOCA_SUCCESS)
		return task_result.status;

	/* Calculate checksum if needed */
	if (output_crc_checksum != NULL)
		*output_crc_checksum = task_result.crc_cs;

	if (output_xxh_checksum != NULL)
		*output_xxh_checksum = task_result.xxh_cs;

	return result;
}

doca_error_t compress_task_compress_deflate_is_supported(struct doca_devinfo *devinfo)
{
	return doca_compress_cap_task_compress_deflate_is_supported(devinfo);
}

doca_error_t compress_task_decompress_deflate_is_supported(struct doca_devinfo *devinfo)
{
	return doca_compress_cap_task_decompress_deflate_is_supported(devinfo);
}

doca_error_t compress_task_decompress_lz4_stream_is_supported(struct doca_devinfo *devinfo)
{
	return doca_compress_cap_task_decompress_lz4_stream_is_supported(devinfo);
}

void compress_completed_callback(struct doca_compress_task_compress_deflate *compress_task,
				 union doca_data task_user_data,
				 union doca_data ctx_user_data)
{
	struct compress_resources *resources = (struct compress_resources *)ctx_user_data.ptr;
	struct compress_deflate_result *result = (struct compress_deflate_result *)task_user_data.ptr;

	DOCA_LOG_INFO("Compress task was done successfully");

	/* Prepare task result */
	result->crc_cs = doca_compress_task_compress_deflate_get_crc_cs(compress_task);
	result->adler_cs = doca_compress_task_compress_deflate_get_adler_cs(compress_task);
	result->status = DOCA_SUCCESS;

	/* Free task */
	doca_task_free(doca_compress_task_compress_deflate_as_task(compress_task));
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void compress_error_callback(struct doca_compress_task_compress_deflate *compress_task,
			     union doca_data task_user_data,
			     union doca_data ctx_user_data)
{
	struct compress_resources *resources = (struct compress_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_compress_task_compress_deflate_as_task(compress_task);
	struct compress_deflate_result *result = (struct compress_deflate_result *)task_user_data.ptr;

	/* Get the result of the task */
	result->status = doca_task_get_status(task);
	DOCA_LOG_ERR("Compress task failed: %s", doca_error_get_descr(result->status));
	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void decompress_deflate_completed_callback(struct doca_compress_task_decompress_deflate *decompress_task,
					   union doca_data task_user_data,
					   union doca_data ctx_user_data)
{
	struct compress_resources *resources = (struct compress_resources *)ctx_user_data.ptr;
	struct compress_deflate_result *result = (struct compress_deflate_result *)task_user_data.ptr;

	DOCA_LOG_INFO("Decompress task was done successfully");

	/* Prepare task result */
	result->crc_cs = doca_compress_task_decompress_deflate_get_crc_cs(decompress_task);
	result->adler_cs = doca_compress_task_decompress_deflate_get_adler_cs(decompress_task);
	result->status = DOCA_SUCCESS;

	/* Free task */
	doca_task_free(doca_compress_task_decompress_deflate_as_task(decompress_task));
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void decompress_deflate_error_callback(struct doca_compress_task_decompress_deflate *decompress_task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	struct compress_resources *resources = (struct compress_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_compress_task_decompress_deflate_as_task(decompress_task);
	struct compress_deflate_result *result = (struct compress_deflate_result *)task_user_data.ptr;

	/* Get the result of the task */
	result->status = doca_task_get_status(task);
	DOCA_LOG_ERR("Decompress task failed: %s", doca_error_get_descr(result->status));
	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void decompress_lz4_stream_completed_callback(struct doca_compress_task_decompress_lz4_stream *decompress_task,
					      union doca_data task_user_data,
					      union doca_data ctx_user_data)
{
	struct compress_resources *resources = (struct compress_resources *)ctx_user_data.ptr;
	struct compress_lz4_result *result = (struct compress_lz4_result *)task_user_data.ptr;

	DOCA_LOG_INFO("Decompress task was done successfully");

	/* Prepare task result */
	result->crc_cs = doca_compress_task_decompress_lz4_stream_get_crc_cs(decompress_task);
	result->xxh_cs = doca_compress_task_decompress_lz4_stream_get_xxh_cs(decompress_task);
	result->status = DOCA_SUCCESS;

	/* Free task */
	doca_task_free(doca_compress_task_decompress_lz4_stream_as_task(decompress_task));
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}

void decompress_lz4_stream_error_callback(struct doca_compress_task_decompress_lz4_stream *decompress_task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	struct compress_resources *resources = (struct compress_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_compress_task_decompress_lz4_stream_as_task(decompress_task);
	struct compress_lz4_result *result = (struct compress_lz4_result *)task_user_data.ptr;

	/* Get the result of the task */
	result->status = doca_task_get_status(task);
	DOCA_LOG_ERR("Decompress task failed: %s", doca_error_get_descr(result->status));
	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state->ctx);
}
