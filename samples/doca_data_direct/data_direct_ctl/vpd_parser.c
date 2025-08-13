/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vpd_parser.h"

#include "util.h"

struct vpd_parser {
	char *file_path;
	int fd;
	uint8_t *buf;
	size_t buf_len;
	size_t cur_pos;
	enum vpd_parser_resource_tags cur_tag;
	uint16_t cur_tag_len;
	bool item_parsed;
	size_t cur_item_pos;
	char cur_item_keyword[3];
	uint8_t cur_item_len;
	char cur_item_data[256];
};

#define VPD_FILE_SIZE_DEFAULT 4096
struct vpd_parser *vpd_parser_create(const char *vpd_file_path)
{
	struct vpd_parser *parser;
	size_t buf_size;
	size_t pos;
	ssize_t rc;
	int ret;

	parser = calloc(1, sizeof(*parser));
	if (!parser) {
		log_debug("Failed to allocate VPD parser");
		return NULL;
	}

	parser->file_path = strdup(vpd_file_path);
	if (!parser->file_path) {
		ret = errno;
		log_debug("Failed to allocate VPD file path for %s",
			  vpd_file_path);
		goto err;
	}

	parser->fd = open(parser->file_path, O_RDONLY);
	if (parser->fd < 0) {
		ret = errno;
		log_debug("Failed to open VPD file %s (errno %d)",
			  parser->file_path, ret);
		goto err_free_file;
	}

	buf_size = VPD_FILE_SIZE_DEFAULT;
	parser->buf = calloc(1, buf_size);
	if (!parser->buf) {
		ret = errno;
		log_debug("Failed to allocate VPD buffer for %s",
			  parser->file_path);
		goto err_close_fd;
	}

	pos = 0;
	while ((rc = read(parser->fd, parser->buf + pos, buf_size - pos)) > 0) {
		pos += rc;
		if (pos >= buf_size) {
			uint8_t *new_buf = calloc(1, buf_size * 2);

			if (!new_buf) {
				ret = errno;
				log_debug(
					"Failed to enlarge buffer to %zu for %s",
					buf_size * 2, parser->file_path);
				goto err_free_buf;
			}

			memcpy(new_buf, parser->buf, buf_size);
			free(parser->buf);
			parser->buf = new_buf;
			buf_size *= 2;
		}
	}

	if (rc < 0) {
		ret = errno;
		log_debug("Failed to read VPD file %s (errno %d)",
			  parser->file_path, ret);
		goto err_free_buf;
	}

	parser->buf_len = pos;
	parser->cur_pos = 0;
	parser->cur_tag = VPD_PARSER_RESOURCE_TAG_NONE;

	return parser;

err_free_buf:
	free(parser->buf);

err_close_fd:
	close(parser->fd);

err_free_file:
	free(parser->file_path);

err:
	free(parser);
	errno = ret;

	return NULL;
}

void vpd_parser_destroy(struct vpd_parser *parser)
{
	free(parser->buf);
	close(parser->fd);
	free(parser->file_path);
	free(parser);
}

static bool vpd_parser_is_buf_space_valid(struct vpd_parser *parser, size_t len)
{
	if (parser->buf_len < parser->cur_pos + parser->cur_item_pos + len) {
		log_debug(
			"VPD parser attempting to parse buffer at pos %zu but buffer length is %zu",
			parser->cur_pos + parser->cur_item_pos + len,
			parser->buf_len);
		return false;
	}

	return true;
}

#define TAG_MASK 0x80
static bool vpd_parser_is_small_tag(uint8_t tag)
{
	return !(tag & TAG_MASK);
}

static bool vpd_parser_is_large_tag(uint8_t tag)
{
	return tag & TAG_MASK;
}

static void vpd_parser_parse_large_tag_len(struct vpd_parser *parser)
{
	uint8_t *buf = parser->buf;
	uint16_t *large_tag_len = (uint16_t *)(buf + parser->cur_pos);

	parser->cur_tag_len = *large_tag_len;
	parser->cur_pos += 2;
}

#define ID_STR_TAG 0x82
static int vpd_parser_parse_id_str_tag(struct vpd_parser *parser)
{
	uint8_t *buf = parser->buf;

	/* ID string tag must be the first tag */
	assert(!parser->cur_pos);

	if (!vpd_parser_is_buf_space_valid(parser, 3))
		return EINVAL;

	if (buf[parser->cur_pos] != ID_STR_TAG) {
		log_debug("VPD ID string resource tag is not first");
		return EINVAL;
	}

	parser->cur_pos++;
	parser->cur_tag = VPD_PARSER_RESOURCE_TAG_ID_STR;

	vpd_parser_parse_large_tag_len(parser);

	return 0;
}

#define END_TAG 0x78
#define SMALL_TAG_LEN_MASK 0x3
static int vpd_parser_parse_small_tag(struct vpd_parser *parser)
{
	uint8_t *buf = parser->buf;

	if (buf[parser->cur_pos] == END_TAG) {
		parser->cur_tag = VPD_PARSER_RESOURCE_TAG_END;
		parser->cur_tag_len = 0;
		parser->cur_pos++;

		return 0;
	}

	parser->cur_tag = VPD_PARSER_RESOURCE_TAG_SMALL;
	parser->cur_tag_len = buf[parser->cur_pos] & SMALL_TAG_LEN_MASK;
	parser->cur_pos++;

	return 0;
}

#define LARGE_READ_TAG 0x90
#define LARGE_WRITE_TAG 0x91
static int vpd_parser_parse_large_tag(struct vpd_parser *parser)
{
	uint8_t *buf = parser->buf;

	if (!vpd_parser_is_buf_space_valid(parser, 3))
		return EINVAL;

	if (buf[parser->cur_pos] == LARGE_READ_TAG) {
		parser->cur_tag = VPD_PARSER_RESOURCE_TAG_LARGE_READ;
	} else if (buf[parser->cur_pos] == LARGE_WRITE_TAG) {
		parser->cur_tag = VPD_PARSER_RESOURCE_TAG_LARGE_WRITE;
	} else {
		log_debug("Unknown VPD resource tag 0x%hhx at pos %zu",
			buf[parser->cur_pos], parser->cur_pos);
		return EINVAL;
	}
	parser->cur_pos++;

	vpd_parser_parse_large_tag_len(parser);

	return 0;
}

int vpd_parser_next_tag(struct vpd_parser *parser)
{
	uint8_t *buf = parser->buf;
	uint8_t next_tag;
	int ret;


	parser->item_parsed = false;
	parser->cur_item_pos = 0;
	parser->cur_item_len = 0;

	if (parser->cur_tag == VPD_PARSER_RESOURCE_TAG_NONE)
		return vpd_parser_parse_id_str_tag(parser);
	else if (parser->cur_tag == VPD_PARSER_RESOURCE_TAG_END)
		return 0;

	/* + 1 for END tag */
	if (!vpd_parser_is_buf_space_valid(parser, parser->cur_tag_len + 1))
		return EINVAL;

	parser->cur_pos += parser->cur_tag_len;
	next_tag = buf[parser->cur_pos];
	if (vpd_parser_is_small_tag(next_tag)) {
		ret = vpd_parser_parse_small_tag(parser);
	} else if (vpd_parser_is_large_tag(next_tag)) {
		ret = vpd_parser_parse_large_tag(parser);
	} else {
		ret = EINVAL;
		log_debug("Unknown tag 0x%hhx at pos %zu", next_tag,
			parser->cur_pos);
	}

	return ret;
}

enum vpd_parser_resource_tags
vpd_parser_get_resource_tag(struct vpd_parser *parser)
{
	return parser->cur_tag;
}

int vpd_parser_next_item(struct vpd_parser *parser)
{
	uint8_t *buf = parser->buf + parser->cur_pos;

	if (parser->cur_tag != VPD_PARSER_RESOURCE_TAG_LARGE_READ &&
	    parser->cur_tag != VPD_PARSER_RESOURCE_TAG_LARGE_WRITE) {
		log_debug("VPD parse next item was called on invalid tag %d",
			parser->cur_tag);
		return EINVAL;
	}

	if (parser->cur_item_pos >= parser->cur_tag_len)
		return VPD_PARSER_ITEMS_END;

	if (!vpd_parser_is_buf_space_valid(parser, 3))
		return EINVAL;

	memcpy(parser->cur_item_keyword, buf + parser->cur_item_pos, 2);
	parser->cur_item_keyword[2] = '\0';
	parser->cur_item_pos += 2;

	parser->cur_item_len = buf[parser->cur_item_pos];
	parser->cur_item_pos++;

	if (!vpd_parser_is_buf_space_valid(parser, parser->cur_item_len))
		return EINVAL;

	memcpy(parser->cur_item_data, buf + parser->cur_item_pos,
	       parser->cur_item_len);
	parser->cur_item_data[parser->cur_item_len] = '\0';
	parser->cur_item_pos += parser->cur_item_len;

	parser->item_parsed = true;

	return 0;
}

const char *vpd_parser_get_item_keyword(struct vpd_parser *parser)
{
	if (!parser->item_parsed) {
		errno = EINVAL;
		log_debug("Get item keyword called without parsing an item");
		return NULL;
	}

	return parser->cur_item_keyword;
}

const char *vpd_parser_get_item_data(struct vpd_parser *parser)
{
	if (!parser->item_parsed) {
		errno = EINVAL;
		log_debug("Get item data called without parsing an item");
		return NULL;
	}

	return parser->cur_item_data;
}
