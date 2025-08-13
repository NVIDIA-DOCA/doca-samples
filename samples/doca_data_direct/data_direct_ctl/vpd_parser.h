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

#ifndef VPD_PARSER_H
#define VPD_PARSER_H

/**
 * vpd_parser:
 *
 * The VPD parser object parses PCI Vital Product Data as defined by the PCI
 * specification.
 */
struct vpd_parser;

/**
 * vpd_parser_resource_tags:
 *
 * This enum represents the possible VPD resource tags that can be parsed. The
 * NONE tag is the default tag which is set after the parser is created.
 */
enum vpd_parser_resource_tags {
	VPD_PARSER_RESOURCE_TAG_NONE,
	VPD_PARSER_RESOURCE_TAG_ID_STR,
	VPD_PARSER_RESOURCE_TAG_SMALL,
	VPD_PARSER_RESOURCE_TAG_LARGE_READ,
	VPD_PARSER_RESOURCE_TAG_LARGE_WRITE,
	VPD_PARSER_RESOURCE_TAG_END,
};

/**
 * vpd_parser_create:
 * @vpd_file_path: The path of a file that contians the VPD to parse
 *
 * Creates a VPD parser that parses the VPD contents of @vpd_file_path. After a
 * successful call to this function, the rest of the parsing functions can be
 * called with the returned VPD parser object.
 *
 * Returns a new VPD parser object on success, and on failure returns NULL and
 * sets errno with an appropriate error code.
 */
struct vpd_parser *vpd_parser_create(const char *vpd_file_path);

/**
 * vpd_parser_destroy:
 * @parser: The VPD parser to destroy
 *
 * Destroys the VPD parser @parser and frees all its resources.
 */
void vpd_parser_destroy(struct vpd_parser *parser);

/**
 * vpd_parser_next_tag:
 * @parser: The VPD parser to operate on
 *
 * Parses the next VPD tag of the VPD data. Calling this function again after
 * reaching the END tag has no effect.
 *
 * Returns 0 on success or an appropriate errno error code on failure.
 */
int vpd_parser_next_tag(struct vpd_parser *parser);

/**
 * vpd_parser_get_resource_tag:
 * @parser: The VPD parser to operate on
 *
 * Returns the currently parsed VPD tag.
 */
enum vpd_parser_resource_tags
vpd_parser_get_resource_tag(struct vpd_parser *parser);

#define VPD_PARSER_ITEMS_END -1

/**
 * vpd_parser_next_item:
 * @parser: The VPD parser to operate on
 *
 * Parses the next VPD item within the currently parsed large resource tag.
 *
 * Returns 0 on success, VPD_PARSER_ITEMS_END if there are no more items in the
 * currently parsed tag, or an appropriate errno error code on failure.
 */
int vpd_parser_next_item(struct vpd_parser *parser);

/**
 * vpd_parser_get_keyword:
 * @parser: The VPD parser to operate on
 *
 * Returns the currently parsed item keyword on success, and on failure returns
 * NULL and sets errno with an appropriate error code.
 */
const char *vpd_parser_get_item_keyword(struct vpd_parser *parser);

/**
 * vpd_parser_get_data:
 * @parser: The VPD parser to operate on
 *
 * Returns the currently parsed item data on success, and on failure returns
 * NULL and sets errno with an appropriate error code.
 */
const char *vpd_parser_get_item_data(struct vpd_parser *parser);

#endif
