/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_mgmt.h>
#include <doca_mgmt_raw_hca_cap.h>
#include <doca_mgmt_raw_register.h>

DOCA_LOG_REGISTER(MGMT_RAW::SAMPLE);

#define DESCRIBE_FIELD_NAME "Field Name"
#define DESCRIBE_SIZE "Size (Bits)"
#define DESCRIBE_ACCESS "Access"

#define QUERY_NAME "Field Name"
#define QUERY_VALUE "Value"
#define QUERY_VALUE_WIDTH "0x12345678"

/*
 * Map raw field access enum to string for table output.
 *
 * @access [in]: Access enum value
 * @out_str [out]: Set to the display string on success
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE for unrecognized access enum value
 */
static doca_error_t field_access_to_string(enum doca_mgmt_raw_field_access access, const char **out_str)
{
	switch (access) {
	case DOCA_MGMT_RAW_FIELD_ACCESS_UNKNOWN:
		*out_str = "UNKNOWN";
		return DOCA_SUCCESS;
	case DOCA_MGMT_RAW_FIELD_ACCESS_RO:
		*out_str = "RO";
		return DOCA_SUCCESS;
	case DOCA_MGMT_RAW_FIELD_ACCESS_WO:
		*out_str = "WO";
		return DOCA_SUCCESS;
	case DOCA_MGMT_RAW_FIELD_ACCESS_RW:
		*out_str = "RW";
		return DOCA_SUCCESS;
	case DOCA_MGMT_RAW_FIELD_ACCESS_INDEX:
		*out_str = "INDEX";
		return DOCA_SUCCESS;
	case DOCA_MGMT_RAW_FIELD_ACCESS_OP:
		*out_str = "OP";
		return DOCA_SUCCESS;
	default:
		DOCA_LOG_ERR("Unrecognized field access enum value: %d", (int)access);
		return DOCA_ERROR_INVALID_VALUE;
	}
}

/*
 * Print a table of raw field descriptors (name, size, access).
 *
 * @fields [in]: Array of field descriptors
 * @count [in]: Number of descriptors
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_INVALID_VALUE if any field has an unrecognized access enum value
 */
static doca_error_t print_field_desc_table(const struct doca_mgmt_raw_field_desc *fields, size_t count)
{
	size_t name_width = sizeof(DESCRIBE_FIELD_NAME) - 1;
	const size_t size_width = sizeof(DESCRIBE_SIZE) - 1;
	size_t access_width = sizeof(DESCRIBE_ACCESS) - 1;
	size_t i;
	const char *access_str;
	doca_error_t result;

	for (i = 0; i < count; i++) {
		size_t len = strlen(fields[i].name);
		if (len > name_width)
			name_width = len;
		result = field_access_to_string(fields[i].access, &access_str);
		if (result != DOCA_SUCCESS)
			return result;
		len = strlen(access_str);
		if (len > access_width)
			access_width = len;
	}

	printf("%-*s  %-*s  %-*s\n",
	       (int)name_width,
	       DESCRIBE_FIELD_NAME,
	       (int)size_width,
	       DESCRIBE_SIZE,
	       (int)access_width,
	       DESCRIBE_ACCESS);
	for (i = 0; i < name_width + size_width + access_width + 4; i++)
		printf("-");
	printf("\n");

	for (i = 0; i < count; i++) {
		result = field_access_to_string(fields[i].access, &access_str);
		if (result != DOCA_SUCCESS)
			return result;

		printf("%-*s  %-*zu  %-*s\n",
		       (int)name_width,
		       fields[i].name,
		       (int)size_width,
		       fields[i].bit_size,
		       (int)access_width,
		       access_str);
	}

	return DOCA_SUCCESS;
}

/*
 * Print query results as a table (name, value).
 *
 * @fields [in]: Array of field name and value
 * @count [in]: Number of fields
 */
static void print_query_results(const struct doca_mgmt_raw_field *fields, size_t count)
{
	const size_t value_width = sizeof(QUERY_VALUE_WIDTH) - 1;
	size_t name_width = sizeof(QUERY_NAME) - 1;
	size_t i;
	size_t j;

	for (i = 0; i < count; i++) {
		size_t len = strlen(fields[i].name);
		if (len > name_width)
			name_width = len;
	}

	printf("%-*s  %-*s\n", (int)name_width, QUERY_NAME, (int)value_width, QUERY_VALUE);
	for (j = 0; j < name_width + value_width + 2; j++)
		printf("-");
	printf("\n");
	for (i = 0; i < count; i++)
		printf("%-*s  0x%08" PRIx32 "\n", (int)name_width, fields[i].name, fields[i].value);
}

/*
 * List raw register names supported by a device.
 *
 * @target_string [in]: The device management target string to list its raw registers
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_register_list(const char *target_string)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	char **registers = NULL;
	size_t count = 0;
	size_t i;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get the list of raw registers */
	result = doca_mgmt_raw_register_list(dev_ctx, &registers, &count);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get supported raw registers: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Print the list of raw registers */
	for (i = 0; i < count; i++)
		printf("%s\n", registers[i]);

	/* Free the list of raw registers */
	if (doca_mgmt_raw_str_list_free(registers) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to free supported raw registers list");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Describe fields of a raw register of a device.
 *
 * @target_string [in]: The device management target string to describe its raw register
 * @register_name [in]: The name of the raw register to describe its fields
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_register_describe(const char *target_string, const char *register_name)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_raw_field_desc *fields = NULL;
	size_t count = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Describe the raw register */
	result = doca_mgmt_raw_register_describe(dev_ctx, register_name, &fields, &count);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to describe fields of raw register %s: %s",
			     register_name,
			     doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	result = print_field_desc_table(fields, count);

	/* Free the field descriptors */
	if (doca_mgmt_raw_field_desc_list_free(fields) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to free supported field descriptor list");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Trim leading and trailing whitespace from a string in place.
 * Advances *pstr past leading whitespace and null-terminates at the last non-whitespace character.
 *
 * @pstr [in/out]: Pointer to the string start (must be writable; updated to trimmed start)
 */
static void trim_whitespace(char **pstr)
{
	char *p = *pstr;
	char *first_space;
	bool space_found;

	/* Remove leading whitespace */
	while (*p != '\0' && isspace((unsigned char)*p))
		p++;

	*pstr = p;

	/* Remove trailing whitespace */
	first_space = NULL;
	space_found = false;
	while (*p != '\0') {
		if (isspace((unsigned char)*p)) {
			if (!space_found) {
				first_space = p;
				space_found = true;
			}
		} else {
			space_found = false;
			first_space = NULL;
		}
		p++;
	}

	if (first_space != NULL)
		*first_space = '\0';
}

/*
 * Free an array of parsed fields as returned by parse_fields_string().
 * Safe to call with fields NULL or count 0.
 *
 * @fields [in]: Array of fields to free (may be NULL)
 * @count [in]: Number of elements in fields
 */
static void free_parsed_fields(struct doca_mgmt_raw_field *fields, size_t count)
{
	size_t i;

	if (fields == NULL)
		return;

	for (i = 0; i < count; i++)
		free((char *)fields[i].name);

	free(fields);
}

/*
 * Count comma-separated fields in a string (number of commas + 1).
 *
 * @str [in]: Null-terminated string
 * @return: Number of comma-separated segments
 */
static size_t count_fields(const char *str)
{
	size_t count = 1;
	const char *p;

	for (p = str; *p != '\0'; p++)
		if (*p == ',')
			count++;

	return count;
}

/*
 * Parse fields string.
 * For input fields, format is "field_name1=value1,field_name2=value2,...".
 * For output fields, format is "field_name1,field_name2,...".
 * Allocates the fields array and each field name; caller must free with free_parsed_fields().
 *
 * @fields_str [in]: The fields string to parse
 * @is_input [in]: If true, expect input format (field=value); if false, expect output format (field only)
 * @fields [out]: Allocated array of fields (caller frees with free_parsed_fields)
 * @fields_num [out]: Number of fields parsed
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_fields_string(const char *fields_str,
					bool is_input,
					struct doca_mgmt_raw_field **fields,
					size_t *fields_num)
{
	size_t num_fields;
	struct doca_mgmt_raw_field *parsed = NULL;
	struct doca_mgmt_raw_field *cur_field;
	char *fields_str_copy;
	size_t count = 0;
	char *saveptr;
	char *token;
	char *field_name;
	char *value_str;
	unsigned long value;
	char *endptr;
	doca_error_t result;

	if (fields_str == NULL) {
		*fields = NULL;
		*fields_num = 0;

		return DOCA_SUCCESS;
	}

	num_fields = count_fields(fields_str);
	parsed = calloc(num_fields, sizeof(*parsed));
	if (parsed == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for parsed fields");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Make a copy of the string for strtok_r */
	fields_str_copy = strdup(fields_str);
	if (fields_str_copy == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for fields string copy");
		result = DOCA_ERROR_NO_MEMORY;
		goto err;
	}

	token = strtok_r(fields_str_copy, ",", &saveptr);
	while (token != NULL && count < num_fields) {
		cur_field = &parsed[count];

		/* Find the '=' separator */
		value_str = strchr(token, '=');
		if (is_input && value_str == NULL) {
			fprintf(stderr, "Expected input fields (field=value), got output-style field\n");
			result = DOCA_ERROR_INVALID_VALUE;
			goto err;
		}
		if (!is_input && value_str != NULL) {
			fprintf(stderr, "Expected output fields (field only), got input-style field (field=value)\n");
			result = DOCA_ERROR_INVALID_VALUE;
			goto err;
		}

		if (value_str == NULL) {
			/* For output fields, there's no value */
			field_name = token;
			trim_whitespace(&field_name);
			if (strlen(field_name) == 0) {
				fprintf(stderr, "Field name can't be empty\n");
				result = DOCA_ERROR_INVALID_VALUE;
				goto err;
			}

			cur_field->name = strdup(field_name);
			if (cur_field->name == NULL) {
				DOCA_LOG_ERR("Failed to allocate memory for field name");
				result = DOCA_ERROR_NO_MEMORY;
				goto err;
			}
			cur_field->value = 0; /* Value not used for output fields */
		} else {
			/* Split field_name=value */
			*value_str = '\0';
			field_name = token;
			value_str++;
			trim_whitespace(&field_name);
			if (strlen(field_name) == 0) {
				fprintf(stderr, "Field name can't be empty\n");
				result = DOCA_ERROR_INVALID_VALUE;
				goto err;
			}

			trim_whitespace(&value_str);
			if (strlen(value_str) == 0) {
				fprintf(stderr, "Field value can't be empty\n");
				result = DOCA_ERROR_INVALID_VALUE;
				goto err;
			}

			/* Parse value */
			errno = 0;
			value = strtoul(value_str, &endptr, 0);
			if (errno != 0 || *endptr != '\0') {
				fprintf(stderr,
					"Failed to parse field value '%s' for field '%s'\n",
					value_str,
					field_name);
				result = DOCA_ERROR_INVALID_VALUE;
				goto err;
			}

			if (value > UINT32_MAX) {
				fprintf(stderr,
					"Field value '%s' for field '%s' exceeds UINT32_MAX\n",
					value_str,
					field_name);
				result = DOCA_ERROR_INVALID_VALUE;
				goto err;
			}

			cur_field->name = strdup(field_name);
			if (cur_field->name == NULL) {
				DOCA_LOG_ERR("Failed to allocate memory for field name");
				result = DOCA_ERROR_NO_MEMORY;
				goto err;
			}
			cur_field->value = (uint32_t)value;
		}

		count++;
		token = strtok_r(NULL, ",", &saveptr);
	}

	if (token != NULL || count != num_fields) {
		DOCA_LOG_ERR("Invalid fields string");
		result = DOCA_ERROR_INVALID_VALUE;
		goto err;
	}

	free(fields_str_copy);
	*fields = parsed;
	*fields_num = count;

	return DOCA_SUCCESS;

err:
	free(fields_str_copy);
	free_parsed_fields(parsed, num_fields);

	return result;
}

/*
 * Query a raw register of the device, returning only the specified output fields.
 *
 * @target_string [in]: The device management target string to query its register
 * @register_name [in]: The name of the register to query
 * @input_fields_str [in]: Optional input fields string in format "field_name1=value1,field_name2=value2,..."
 * @output_fields_str [in]: Output fields string in format "field_name1,field_name2,..."
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_register_get(const char *target_string,
				   const char *register_name,
				   const char *input_fields_str,
				   const char *output_fields_str)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_raw_field *fields_in = NULL;
	struct doca_mgmt_raw_field *fields_out = NULL;
	size_t count_in = 0;
	size_t count_out = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Parse input fields */
	result = parse_fields_string(input_fields_str, true, &fields_in, &count_in);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse input fields: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Parse output fields */
	result = parse_fields_string(output_fields_str, false, &fields_out, &count_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse output fields: %s", doca_error_get_descr(result));
		goto out_free_input;
	}

	/* Query the register */
	result = doca_mgmt_raw_register_query(dev_ctx,
					      register_name,
					      count_in > 0 ? fields_in : NULL,
					      count_in,
					      fields_out,
					      count_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query raw register %s: %s", register_name, doca_error_get_descr(result));
		goto out_free_output;
	}

	print_query_results(fields_out, count_out);

out_free_output:
	free_parsed_fields(fields_out, count_out);

out_free_input:
	free_parsed_fields(fields_in, count_in);

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Query all fields of a raw register of the device.
 *
 * @target_string [in]: The device management target string to query its register
 * @register_name [in]: The name of the register to query
 * @input_fields_str [in]: Optional input fields string in format "field_name1=value1,field_name2=value2,..."
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_register_get_all(const char *target_string,
				       const char *register_name,
				       const char *input_fields_str)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_raw_field *fields_in = NULL;
	struct doca_mgmt_raw_field *fields_out = NULL;
	size_t count_in = 0;
	size_t count_out = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Parse input fields */
	result = parse_fields_string(input_fields_str, true, &fields_in, &count_in);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse input fields: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Query all fields of the register */
	result = doca_mgmt_raw_register_query_all(dev_ctx,
						  register_name,
						  count_in > 0 ? fields_in : NULL,
						  count_in,
						  &fields_out,
						  &count_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query all raw register fields for %s: %s",
			     register_name,
			     doca_error_get_descr(result));
		goto out_free_input;
	}

	print_query_results(fields_out, count_out);

	/* Free the output fields */
	if (doca_mgmt_raw_field_list_free(fields_out) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to free queried fields");

out_free_input:
	free_parsed_fields(fields_in, count_in);

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Modify a raw register of the device.
 *
 * @target_string [in]: The device management target string to modify its register
 * @register_name [in]: The name of the register to modify
 * @input_fields_str [in]: Input fields string in format "field_name1=value1,field_name2=value2,..."
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_register_set(const char *target_string, const char *register_name, const char *input_fields_str)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_raw_field *fields_in = NULL;
	size_t count_in = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Parse input fields */
	result = parse_fields_string(input_fields_str, true, &fields_in, &count_in);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse input fields: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Modify the register */
	result = doca_mgmt_raw_register_modify(dev_ctx, register_name, fields_in, count_in);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to modify raw register %s: %s", register_name, doca_error_get_descr(result));

	free_parsed_fields(fields_in, count_in);

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * List all supported HCA capability types.
 *
 * @target_string [in]: The device management target string to list its HCA capability types
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_hca_cap_list(const char *target_string)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	char **cap_types = NULL;
	size_t count = 0;
	size_t i;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* List HCA capability types */
	result = doca_mgmt_raw_hca_cap_list(dev_ctx, &cap_types, &count);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get supported HCA capability types: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Print HCA capability types */
	for (i = 0; i < count; i++)
		printf("%s\n", cap_types[i]);

	/* Free HCA capability types list */
	if (doca_mgmt_raw_str_list_free(cap_types) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to free HCA capability types list");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Describe fields of a HCA capability type.
 *
 * @target_string [in]: The device management target string to describe its HCA capability type
 * @cap_type [in]: The name of the HCA capability type to describe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_hca_cap_describe(const char *target_string, const char *cap_type)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_raw_field_desc *fields = NULL;
	size_t count = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Describe HCA capability type */
	result = doca_mgmt_raw_hca_cap_describe(dev_ctx, cap_type, &fields, &count);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to describe fields of HCA capability type %s: %s",
			     cap_type,
			     doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	result = print_field_desc_table(fields, count);

	/* Free HCA capability field descriptor list */
	if (doca_mgmt_raw_field_desc_list_free(fields) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to free HCA capability field descriptor list");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Get HCA cap mode from string.
 *
 * @cap_mode_str [in]: The string representation of the HCA cap mode
 * @cap_mode [out]: Set to the HCA cap mode on success
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mgmt_raw_hca_cap_cap_mode_get(const char *cap_mode_str, enum doca_mgmt_raw_hca_cap_mode *cap_mode)
{
	if (strcasecmp(cap_mode_str, "current") == 0)
		*cap_mode = DOCA_MGMT_RAW_HCA_CAP_MODE_CUR;
	else if (strcasecmp(cap_mode_str, "max") == 0)
		*cap_mode = DOCA_MGMT_RAW_HCA_CAP_MODE_MAX;
	else
		return DOCA_ERROR_INVALID_VALUE;

	return DOCA_SUCCESS;
}

/*
 * Build raw HCA cap target from optional VHCA ID / function ID (mutually exclusive).
 *
 * @target [out]: Filled cap target
 * @has_vhca_id [in]: If true, target is OTHER_VHCA_ID
 * @vhca_id [in]: The function's VHCA ID
 * @has_function_id [in]: If true, target is OTHER_FUNCTION_ID
 * @function_id [in]: The function's ID
 */
static void mgmt_raw_hca_cap_target_init(struct doca_mgmt_raw_hca_cap_target *target,
					 bool has_vhca_id,
					 uint16_t vhca_id,
					 bool has_function_id,
					 uint16_t function_id)
{
	if (has_function_id) {
		target->type = DOCA_MGMT_RAW_HCA_CAP_TARGET_TYPE_OTHER_FUNCTION_ID;
		target->function_id = function_id;
	} else if (has_vhca_id) {
		target->type = DOCA_MGMT_RAW_HCA_CAP_TARGET_TYPE_OTHER_VHCA_ID;
		target->vhca_id = vhca_id;
	} else {
		target->type = DOCA_MGMT_RAW_HCA_CAP_TARGET_TYPE_SELF;
	}
}

/*
 * Query specific fields of a HCA capability type.
 *
 * @target_string [in]: The device management target string to query its HCA capability type
 * @cap_type [in]: The name of the HCA capability type to query
 * @cap_mode [in]: The capability mode for the query
 * @function_id [in]: The function ID for the query
 * @has_function_id [in]: If true, function_id is valid
 * @vhca_id [in]: The VHCA ID for the query
 * @has_vhca_id [in]: If true, vhca_id is valid
 * @output_fields_str [in]: Output fields string in format "field_name1,field_name2,..."
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_hca_cap_get(const char *target_string,
				  const char *cap_type,
				  const char *cap_mode,
				  uint16_t function_id,
				  bool has_function_id,
				  uint16_t vhca_id,
				  bool has_vhca_id,
				  const char *output_fields_str)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	enum doca_mgmt_raw_hca_cap_mode mode;
	struct doca_mgmt_raw_hca_cap_target target = {};
	struct doca_mgmt_raw_field *fields_out = NULL;
	size_t count_out = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get HCA cap mode */
	result = mgmt_raw_hca_cap_cap_mode_get(cap_mode, &mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get HCA cap mode: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Build HCA cap target */
	mgmt_raw_hca_cap_target_init(&target, has_vhca_id, vhca_id, has_function_id, function_id);

	/* Parse output fields */
	result = parse_fields_string(output_fields_str, false, &fields_out, &count_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse output fields: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Query HCA capability */
	result = doca_mgmt_raw_hca_cap_query(dev_ctx, cap_type, mode, &target, fields_out, count_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query HCA capability %s: %s", cap_type, doca_error_get_descr(result));
		goto out_free_output;
	}

	print_query_results(fields_out, count_out);

out_free_output:
	free_parsed_fields(fields_out, count_out);

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Query all fields of a HCA capability type.
 *
 * @target_string [in]: The device management target string to query its HCA capability type
 * @cap_type [in]: The name of the HCA capability type to query
 * @cap_mode [in]: The capability mode for the query
 * @function_id [in]: The function ID for the query
 * @has_function_id [in]: If true, function_id is valid
 * @vhca_id [in]: The VHCA ID for the query
 * @has_vhca_id [in]: If true, vhca_id is valid
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_hca_cap_get_all(const char *target_string,
				      const char *cap_type,
				      const char *cap_mode,
				      uint16_t function_id,
				      bool has_function_id,
				      uint16_t vhca_id,
				      bool has_vhca_id)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	enum doca_mgmt_raw_hca_cap_mode mode;
	struct doca_mgmt_raw_hca_cap_target target = {};
	struct doca_mgmt_raw_field *fields_out = NULL;
	size_t count_out = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get HCA cap mode */
	result = mgmt_raw_hca_cap_cap_mode_get(cap_mode, &mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get HCA cap mode: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Build HCA cap target */
	mgmt_raw_hca_cap_target_init(&target, has_vhca_id, vhca_id, has_function_id, function_id);

	/* Query all HCA capability fields */
	result = doca_mgmt_raw_hca_cap_query_all(dev_ctx, cap_type, mode, &target, &fields_out, &count_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query all HCA capability fields for %s: %s",
			     cap_type,
			     doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	print_query_results(fields_out, count_out);

	/* Free queried HCA capability fields */
	if (doca_mgmt_raw_field_list_free(fields_out) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to free queried HCA capability fields");

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}

/*
 * Modify a HCA capability type.
 *
 * @target_string [in]: The device management target string to modify its HCA capability type
 * @cap_type [in]: The name of the HCA capability type to modify
 * @function_id [in]: The function ID for the modification
 * @has_function_id [in]: If true, function_id is valid
 * @vhca_id [in]: The VHCA ID for the modification
 * @has_vhca_id [in]: If true, vhca_id is valid
 * @input_fields_str [in]: Input fields string in format "field_name1=value1,field_name2=value2,..."
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_raw_hca_cap_set(const char *target_string,
				  const char *cap_type,
				  uint16_t function_id,
				  bool has_function_id,
				  uint16_t vhca_id,
				  bool has_vhca_id,
				  const char *input_fields_str)
{
	struct doca_mgmt_dev_ctx *dev_ctx;
	struct doca_mgmt_raw_hca_cap_target target = {};
	struct doca_mgmt_raw_field *fields_in = NULL;
	size_t count_in = 0;
	doca_error_t result;

	/* Create the DOCA management device context */
	result = doca_mgmt_dev_ctx_create_from_target_string(target_string, &dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA management device context: %s", doca_error_get_descr(result));
		return result;
	}

	/* Build HCA cap target */
	mgmt_raw_hca_cap_target_init(&target, has_vhca_id, vhca_id, has_function_id, function_id);

	/* Parse input fields */
	result = parse_fields_string(input_fields_str, true, &fields_in, &count_in);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse input fields: %s", doca_error_get_descr(result));
		goto out_dev_ctx_destroy;
	}

	/* Modify HCA capability */
	result = doca_mgmt_raw_hca_cap_modify(dev_ctx, cap_type, &target, fields_in, count_in);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to modify HCA capability %s: %s", cap_type, doca_error_get_descr(result));

	free_parsed_fields(fields_in, count_in);

out_dev_ctx_destroy:
	if (doca_mgmt_dev_ctx_destroy(dev_ctx) != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy DOCA management device context");

	return result;
}
