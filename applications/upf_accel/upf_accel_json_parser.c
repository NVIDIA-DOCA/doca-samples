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
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <json-c/json.h>
#include <rte_byteorder.h>

#include <doca_error.h>
#include <doca_log.h>

#include "upf_accel.h"

DOCA_LOG_REGISTER(UPF_ACCEL::JSON_PARSER);

/*
 * Parse uint64_t property from a json object
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_u64_parse(struct json_object *container, const char *name, uint64_t *val)
{
	struct json_object *field;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_int) {
		DOCA_LOG_ERR("Failed to parse JSON object u64 field '%s' from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	assert(val);
	*val = json_object_get_int64(field);
	return DOCA_SUCCESS;
}

/*
 * Parse uint64_t property from a json object, such that the value is encoded
 * as a string
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_u64_from_string_parse(struct json_object *container, const char *name, uint64_t *val)
{
	struct json_object *field;
	uint64_t tmp;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_string) {
		DOCA_LOG_ERR("Failed to parse JSON object string field %s from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	if (sscanf(json_object_get_string(field), "%lu", &tmp) != 1) {
		DOCA_LOG_ERR("Failed to convert JSON string object to u64 from field %s object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	assert(val);
	*val = tmp;
	return DOCA_SUCCESS;
}

/*
 * Parse uint32_t property from a json object
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_u32_parse(struct json_object *container, const char *name, uint32_t *val)
{
	struct json_object *field;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_int) {
		DOCA_LOG_ERR("Failed to parse JSON object u32 field '%s' from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	assert(val);
	*val = json_object_get_int(field);
	return DOCA_SUCCESS;
}

/*
 * Parse array property from a json object
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val_size [in]: array maximum size
 * @val [out]: pointer to store the result at
 * @val_num [out]: number of elements in the result
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_u32_arr_parse(struct json_object *container,
						 const char *name,
						 size_t val_size,
						 uint32_t *val,
						 uint32_t *val_num)
{
	struct json_object *elem;
	struct json_object *arr;
	size_t elems_num;
	size_t i;

	if (!json_object_object_get_ex(container, name, &arr) || json_object_get_type(arr) != json_type_array) {
		DOCA_LOG_ERR("Failed to parse JSON object u32 array field '%s' from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	elems_num = json_object_array_length(arr);
	if (elems_num > val_size) {
		DOCA_LOG_ERR("JSON object u32 array field '%s' too big: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_NO_MEMORY;
	}

	assert(val);
	assert(val_num);
	for (i = 0; i < elems_num; i++) {
		elem = json_object_array_get_idx(arr, i);
		if (!elem || json_object_get_type(elem) != json_type_int) {
			DOCA_LOG_ERR("Failed to parse JSON object u32 array field '%s' from object: %s",
				     name,
				     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
			return DOCA_ERROR_UNEXPECTED;
		}

		val[i] = json_object_get_int(elem);
	}

	*val_num = elems_num;
	return DOCA_SUCCESS;
}

/*
 * Parse uint8_t property from a json object
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_u8_parse(struct json_object *container, const char *name, uint8_t *val)
{
	struct json_object *field;
	int tmp;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_int) {
		DOCA_LOG_ERR("Failed to parse JSON object u8 field '%s' from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	assert(val);
	tmp = json_object_get_int(field);
	if (tmp > UINT8_MAX) {
		DOCA_LOG_ERR("JSON object u8 field '%s' is too big, object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_TOO_BIG;
	}
	*val = tmp;
	return DOCA_SUCCESS;
}

/*
 * Parse uint8_t property from a json object, such that the value is encoded
 * as a string
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_u8_from_string_parse(struct json_object *container, const char *name, uint8_t *val)
{
	struct json_object *field;
	uint8_t tmp;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_string) {
		DOCA_LOG_ERR("Failed to parse JSON object string field %s from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	if (sscanf(json_object_get_string(field), "%hhu", &tmp) != 1) {
		DOCA_LOG_ERR("Failed to convert JSON string object to integer from field %s  object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	assert(val);
	*val = tmp;
	return DOCA_SUCCESS;
}

/*
 * Parse mac property from a json object, such that the value is encoded
 * as a string
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_mac_from_string_parse(struct json_object *container,
							 const char *name,
							 uint8_t val[DOCA_FLOW_ETHER_ADDR_LEN])
{
	uint8_t tmp[DOCA_FLOW_ETHER_ADDR_LEN];
	struct json_object *field;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_string) {
		DOCA_LOG_ERR("Failed to parse JSON object string field %s from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	if (sscanf(json_object_get_string(field),
		   "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx%*c",
		   &tmp[0],
		   &tmp[1],
		   &tmp[2],
		   &tmp[3],
		   &tmp[4],
		   &tmp[5]) != DOCA_FLOW_ETHER_ADDR_LEN) {
		DOCA_LOG_ERR("Failed to convert JSON string object to integer array from field %s object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	assert(val);
	val[0] = tmp[0];
	val[1] = tmp[1];
	val[2] = tmp[2];
	val[3] = tmp[3];
	val[4] = tmp[4];
	val[5] = tmp[5];

	return DOCA_SUCCESS;
}

/*
 * Parse string property from a json object
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @val_len [out]: result length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_string_parse(struct json_object *container,
						const char *name,
						char *val,
						size_t val_len)
{
	struct json_object *field;

	if (!json_object_object_get_ex(container, name, &field) || json_object_get_type(field) != json_type_string) {
		DOCA_LOG_ERR("Failed to parse JSON object string field '%s' from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	if ((uint32_t)json_object_get_string_len(field) >= val_len) {
		DOCA_LOG_ERR("Failed to copy JSON object string field '%s': string too big", name);
		return DOCA_ERROR_TOO_BIG;
	}

	assert(val);
	strncpy(val, json_object_get_string(field), val_len);
	return DOCA_SUCCESS;
}

/*
 * Sets a mask with a given number of ones in MSBs and zeroes after them
 *
 * @prefix [in]: number of bits to set
 * @mask [out]: mask to set
 */
static inline void ipv6_netmask_get(uint8_t prefix, uint8_t mask[UPF_ACCEL_NUM_BYTES_IPV6])
{
	uint8_t byte_prefix;
	int i;

	for (i = 0; prefix && i < UPF_ACCEL_NUM_BYTES_IPV6; ++i) {
		if (prefix < 8) {
			byte_prefix = prefix;
			prefix = 0;
		} else {
			prefix -= 8;
			byte_prefix = 8;
		}

		mask[i] = ~((1ul << (8 - byte_prefix)) - 1);
	}
}

/*
 * Parse IPv6 and netmask
 *
 * @str_addr_mask [in]: IPv6 and netmask string
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_str_to_ipv6_netmask_parse(const char *str_addr_mask, struct upf_accel_ip_addr *val)
{
	uint8_t out[UPF_ACCEL_NUM_BYTES_IPV6];
	char str_addr[128] = {0};
	uint8_t netmask = 0;

	if (sscanf(str_addr_mask, "%[^/]/%hhd", str_addr, &netmask) < 1)
		return DOCA_ERROR_UNEXPECTED;
	if (inet_pton(AF_INET6, str_addr, out) != 1)
		return DOCA_ERROR_UNEXPECTED;

	memcpy(val->addr.v6, out, UPF_ACCEL_NUM_BYTES_IPV6);

	netmask = netmask ? netmask : 128;
	ipv6_netmask_get(netmask, val->mask.v6);
	val->ip_version = DOCA_FLOW_L3_TYPE_IP6;

	return DOCA_SUCCESS;
}

/*
 * Returns a mask with `mask` number of set MSBs
 *
 * @mask [in]: number of MSbits to set.
 * @return: netmask
 */
static inline uint32_t ipv4_netmask_get(uint8_t mask)
{
	return ~((1ul << (32 - mask)) - 1);
}

/*
 * Parse IPv4 and netmask
 *
 * @str_addr [in]: IPv4 and netmask string in the form of xxx.xxx.xxx.xxx/xx
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_str_to_ipv4_netmask_parse(const char *str_addr, struct upf_accel_ip_addr *val)
{
	uint8_t o1, o2, o3, o4;
	uint8_t netmask = 0;

	if (sscanf(str_addr, "%hhd.%hhd.%hhd.%hhd/%hhd", &o1, &o2, &o3, &o4, &netmask) < 4)
		return DOCA_ERROR_UNEXPECTED;

	val->addr.v4 = ((uint32_t)o1 << 24) | ((uint32_t)o2 << 16) | ((uint32_t)o3 << 8) | (uint32_t)o4;
	netmask = netmask ? netmask : 32;
	val->mask.v4 = ipv4_netmask_get(netmask);
	val->ip_version = DOCA_FLOW_L3_TYPE_IP4;
	return DOCA_SUCCESS;
}

/*
 * Parse IP and netmask
 *
 * @str_addr [in]: IP and netmask string
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_str_to_ip_netmask_parse(const char *str_addr, struct upf_accel_ip_addr *val)
{
	if (upf_accel_str_to_ipv6_netmask_parse(str_addr, val) != DOCA_SUCCESS) {
		if (upf_accel_str_to_ipv4_netmask_parse(str_addr, val) != DOCA_SUCCESS)
			return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse port range
 *
 * @str_port [in]: port range in form of xxx-xxx
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_str_to_port_range_parse(const char *str_port, struct upf_accel_ip_port_range *val)
{
	uint16_t from_port;
	uint16_t to_port;
	int ret;

	ret = sscanf(str_port, "%hu-%hu", &from_port, &to_port);
	if (!ret)
		return DOCA_ERROR_UNEXPECTED;
	else if (ret == 1)
		to_port = from_port;

	val->from = from_port;
	val->to = to_port;
	return DOCA_SUCCESS;
}

/*
 * Parse IP property from a json object
 *
 * @container [in]: json container
 * @name [in]: property name
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_json_ip_parse(struct json_object *container,
					    const char *name,
					    struct upf_accel_ip_addr *val)
{
	struct json_object *field;
	struct json_object *ip;
	const char *str_addr;

	if (!json_object_object_get_ex(container, name, &ip)) {
		DOCA_LOG_ERR("Failed to parse JSON object IP field '%s' from object: %s",
			     name,
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	if (json_object_object_get_ex(ip, "v6", &field) && json_object_get_type(field) == json_type_string) {
		str_addr = json_object_get_string(field);
		assert(val);
		if (upf_accel_str_to_ipv6_netmask_parse(str_addr, val) != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to parse JSON object IPv6 from object: %s",
				     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
			return DOCA_ERROR_UNEXPECTED;
		}

		return DOCA_SUCCESS;
	} else if (json_object_object_get_ex(ip, "v4", &field) && json_object_get_type(field) == json_type_string) {
		str_addr = json_object_get_string(field);
		assert(val);
		if (upf_accel_str_to_ipv4_netmask_parse(str_addr, val) != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to parse JSON object IPv4 from object: %s",
				     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
			return DOCA_ERROR_UNEXPECTED;
		}

		return DOCA_SUCCESS;
	} else {
		DOCA_LOG_ERR("Failed to parse JSON IP object from object: %s",
			     json_object_to_json_string_ext(container, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
}

/*
 * Parse FTEID group
 *
 * @local_fteid [in]: json object of the group
 * @upf_accel_pdr [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_local_fteid_parse(struct json_object *local_fteid, struct upf_accel_pdr *upf_accel_pdr)
{
	if (upf_accel_json_u32_parse(local_fteid, "teid_start", &upf_accel_pdr->pdi_local_teid_start) != DOCA_SUCCESS ||
	    upf_accel_json_u32_parse(local_fteid, "teid_end", &upf_accel_pdr->pdi_local_teid_end) != DOCA_SUCCESS ||
	    upf_accel_json_ip_parse(local_fteid, "ip", &upf_accel_pdr->pdi_local_teid_ip) != DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	return DOCA_SUCCESS;
}

/*
 * Parse user equipment group
 *
 * @ue [in]: json object of the group
 * @upf_accel_pdr [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_ue_parse(struct json_object *ue, struct upf_accel_pdr *upf_accel_pdr)
{
	return upf_accel_json_ip_parse(ue, "ip", &upf_accel_pdr->pdi_ueip);
}

/*
 * Parse protocol
 *
 * @str_proto [in]: protocol in form of ip/tcp/udp
 * @val [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_str_to_proto_parse(const char *str_proto, uint16_t *val)
{
	uint16_t tmp;

	if (!strcmp(str_proto, "ip"))
		*val = 0;
	else if (!strcmp(str_proto, "tcp"))
		*val = 6;
	else if (!strcmp(str_proto, "udp"))
		*val = 17;
	else if (sscanf(str_proto, "%hu", &tmp) == 1)
		*val = tmp;
	else
		return DOCA_ERROR_UNEXPECTED;

	return DOCA_SUCCESS;
}

/*
 * Parse SDF filter group
 *
 * @sdf_arr [in]: json object of the group
 * @upf_accel_pdr [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_sdf_parse(struct json_object *sdf_arr, struct upf_accel_pdr *upf_accel_pdr)
{
	char str_description[129] = {0};
	struct json_object *sdf;
	char *parse_ctx;
	char *token;

	if (json_object_array_length(sdf_arr) != 1) {
		DOCA_LOG_ERR("Failed to parse PDR SDF - unexpected size of object array: %s",
			     json_object_to_json_string_ext(sdf_arr, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	sdf = json_object_array_get_idx(sdf_arr, 0);
	if (!sdf) {
		DOCA_LOG_ERR("Failed to parse JSON PDR SDF object: %s",
			     json_object_to_json_string_ext(sdf_arr, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	if (upf_accel_json_string_parse(sdf, "description", str_description, sizeof(str_description) - 1) !=
	    DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	token = strtok_r(str_description, " ", &parse_ctx);
	if (!token || strcmp(token, "permit")) {
		DOCA_LOG_ERR("Only 'permit' action is supported in JSON PDR SDF FD, got token '%s' from object: %s",
			     token ? token : "NULL",
			     json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (!token || strcmp(token, "out")) {
		DOCA_LOG_ERR(
			"Only outer header description is supported in JSON PDR SDF FD, got token '%s' from object: %s",
			token ? token : "NULL",
			json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (!token || upf_accel_str_to_proto_parse(token, &upf_accel_pdr->pdi_sdf_proto) != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse protocol in JSON PDR SDF FD, got token '%s' from object: %s",
			     token ? token : "NULL",
			     json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (!token || strcmp(token, "from")) {
		DOCA_LOG_ERR("Unexpected value JSON PDR SDF FD 'from', got token '%s' from object: %s",
			     token ? token : "NULL",
			     json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (!token || (strcmp(token, "any") &&
		       upf_accel_str_to_ip_netmask_parse(token, &upf_accel_pdr->pdi_sdf_from_ip) != DOCA_SUCCESS)) {
		DOCA_LOG_ERR(
			"Failed to parse IP address in JSON PDR SDF FD 'from' position, got token '%s' from object: %s",
			token ? token : "NULL",
			json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (!token) {
		DOCA_LOG_ERR("Empty port range in JSON PDR SDF FD 'from' position in object: %s",
			     json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	} else if (upf_accel_str_to_port_range_parse(token, &upf_accel_pdr->pdi_sdf_from_port_range) == DOCA_SUCCESS) {
		token = strtok_r(NULL, " ", &parse_ctx);
	} else {
		upf_accel_pdr->pdi_sdf_from_port_range.from = 0;
		upf_accel_pdr->pdi_sdf_from_port_range.to = UINT16_MAX;
	}

	if (!token || strcmp(token, "to")) {
		DOCA_LOG_ERR("Unexpected value JSON PDR SDF FD 'to', got token '%s' from object: %s",
			     token ? token : "NULL",
			     json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (!token || (strcmp(token, "assigned") &&
		       upf_accel_str_to_ip_netmask_parse(token, &upf_accel_pdr->pdi_sdf_to_ip) != DOCA_SUCCESS)) {
		DOCA_LOG_ERR(
			"Failed to parse IP address in JSON PDR SDF FD 'to' position, got token '%s' from object: %s",
			token ? token : "NULL",
			json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}
	token = strtok_r(NULL, " ", &parse_ctx);
	if (token) {
		if (upf_accel_str_to_port_range_parse(token, &upf_accel_pdr->pdi_sdf_to_port_range) != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to parse port range in JSON PDR SDF FD 'to' position, got token '%s' from object: %s",
				token,
				json_object_to_json_string_ext(sdf, JSON_C_TO_STRING_PRETTY));
			return DOCA_ERROR_UNEXPECTED;
		}
	} else {
		upf_accel_pdr->pdi_sdf_to_port_range.from = 0;
		upf_accel_pdr->pdi_sdf_to_port_range.to = UINT16_MAX;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse PDI group
 *
 * @pdi [in]: json object of the group
 * @upf_accel_pdr [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pdi_parse(struct json_object *pdi, struct upf_accel_pdr *upf_accel_pdr)
{
	struct json_object *local_fteid;
	struct json_object *ue;
	struct json_object *si;
	doca_error_t err;
	uint8_t pdi_si;

	if (!json_object_object_get_ex(pdi, "sourceInterface", &si)) {
		DOCA_LOG_ERR("Failed to parse JSON PDR SI child object");
		return DOCA_ERROR_UNEXPECTED;
	}

	if (upf_accel_json_u8_from_string_parse(si, "type", &pdi_si) != DOCA_SUCCESS ||
	    (pdi_si != UPF_ACCEL_PDR_PDI_SI_UL && pdi_si != UPF_ACCEL_PDR_PDI_SI_DL)) {
		DOCA_LOG_ERR("Unexpected pdi si %u", pdi_si);
		return DOCA_ERROR_UNEXPECTED;
	}
	upf_accel_pdr->pdi_si = pdi_si;

	if (json_object_object_get_ex(pdi, "localFT", &local_fteid)) {
		err = upf_accel_local_fteid_parse(local_fteid, upf_accel_pdr);
		if (err != DOCA_SUCCESS)
			return err;
	}

	if (json_object_object_get(pdi, "qfi") &&
	    upf_accel_json_u8_parse(pdi, "qfi", &upf_accel_pdr->pdi_qfi) != DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	if (!json_object_object_get_ex(pdi, "userEquipment", &ue) ||
	    upf_accel_ue_parse(ue, upf_accel_pdr) != DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	if (!json_object_object_get_ex(pdi, "sdf", &ue) || upf_accel_sdf_parse(ue, upf_accel_pdr) != DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	return DOCA_SUCCESS;
}

/*
 * Get the IP version print string
 *
 * @doca_ip_version [in]: the doca enum for IP version
 * @return: the IP version printable. (IPv4 or IPv6)
 */
static inline uint8_t upf_accel_ip_version_print_string_get(enum doca_flow_l3_type doca_ip_version)
{
	return (doca_ip_version == DOCA_FLOW_L3_TYPE_IP6) ? 6 : 4;
}

/*
 * Parse list of CreatePDR nodes from json
 *
 * @pdr_arr [in]: list of CreaePDR json nodes
 * @cfg [out]: the result is stored inside cfg
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pdr_parse(struct json_object *pdr_arr, struct upf_accel_config *cfg)
{
	struct json_object *pdr;
	struct json_object *pdi;
	struct upf_accel_pdr *upf_accel_pdr;
	struct upf_accel_pdrs *pdrs;
	doca_error_t err;
	size_t num_pdrs;
	size_t i;

	num_pdrs = json_object_array_length(pdr_arr);
	pdrs = rte_zmalloc("UPF PDRs", sizeof(*pdrs) + sizeof(pdrs->arr_pdrs[0]) * num_pdrs, RTE_CACHE_LINE_SIZE);
	if (!pdrs) {
		DOCA_LOG_ERR("Failed to allocate PDR memory");
		return DOCA_ERROR_NO_MEMORY;
	}
	pdrs->num_pdrs = num_pdrs;

	for (i = 0; i < num_pdrs; i++) {
		pdr = json_object_array_get_idx(pdr_arr, i);
		upf_accel_pdr = &pdrs->arr_pdrs[i];
		if (!pdr) {
			DOCA_LOG_ERR("Failed to parse JSON PDR object id %lu", i);
			err = DOCA_ERROR_UNEXPECTED;
			goto err_pdr;
		}
		DOCA_LOG_DBG("PDR:\n%s", json_object_to_json_string_ext(pdr, JSON_C_TO_STRING_PRETTY));

		if (upf_accel_json_u32_parse(pdr, "pdrId", &upf_accel_pdr->id) != DOCA_SUCCESS ||
		    upf_accel_pdr->id >= UPF_ACCEL_MAX_NUM_PDR ||
		    upf_accel_json_u32_parse(pdr, "farId", &upf_accel_pdr->farid) != DOCA_SUCCESS ||
		    upf_accel_json_u32_arr_parse(pdr,
						 "urrIds",
						 UPF_ACCEL_PDR_URRIDS_LEN,
						 upf_accel_pdr->urrids,
						 &upf_accel_pdr->urrids_num) != DOCA_SUCCESS ||
		    upf_accel_json_u32_arr_parse(pdr,
						 "qerIds",
						 UPF_ACCEL_PDR_QERIDS_LEN,
						 upf_accel_pdr->qerids,
						 &upf_accel_pdr->qerids_num) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_pdr;
		}

		if (upf_accel_pdr->qerids_num > UPF_ACCEL_MAX_PDR_NUM_RATE_METERS) {
			DOCA_LOG_ERR("Max Supported Rate Meters Num is: %lu", UPF_ACCEL_MAX_PDR_NUM_RATE_METERS);
			err = DOCA_ERROR_INVALID_VALUE;
			goto err_pdr;
		}

		if ((upf_accel_pdr->qerids_num == 0) || (upf_accel_pdr->urrids_num != 1)) {
			DOCA_LOG_ERR("Each PDR must have at least one QER (has %u) and exactly one URR (has %u)",
				     upf_accel_pdr->qerids_num,
				     upf_accel_pdr->urrids_num);
			err = DOCA_ERROR_INVALID_VALUE;
			goto err_pdr;
		}

		if (!json_object_object_get_ex(pdr, "pdi", &pdi)) {
			DOCA_LOG_ERR("Failed to parse JSON PDR PDI child object");
			err = DOCA_ERROR_UNEXPECTED;
			goto err_pdr;
		}
		err = upf_accel_pdi_parse(pdi, upf_accel_pdr);
		if (err != DOCA_SUCCESS)
			goto err_pdr;

		DOCA_LOG_INFO(
			"Parsed PDR id=%u\n\tfarId=%u first_urrid=%u first_qerid=%u\n\tPDI SI=%u QFI=%hhu teid_start=%u teid_end=%u IP=version %d, %x/%hhu UEIP=version %d, %x/%hhu\n\t\tSDF proto=%d from=IP version %d, %x/%hhu:%hu-%hu to= IP version %d, %x/%hhu:%hu-%hu",
			upf_accel_pdr->id,
			upf_accel_pdr->farid,
			upf_accel_pdr->urrids[0],
			upf_accel_pdr->qerids[0],
			upf_accel_pdr->pdi_si,
			upf_accel_pdr->pdi_qfi,
			upf_accel_pdr->pdi_local_teid_start,
			upf_accel_pdr->pdi_local_teid_end,
			upf_accel_ip_version_print_string_get(upf_accel_pdr->pdi_local_teid_ip.ip_version),
			rte_be_to_cpu_32(upf_accel_pdr->pdi_local_teid_ip.addr.v4),
			upf_accel_pdr->pdi_local_teid_ip.mask.v4,
			upf_accel_ip_version_print_string_get(upf_accel_pdr->pdi_ueip.ip_version),
			rte_be_to_cpu_32(upf_accel_pdr->pdi_ueip.addr.v4),
			upf_accel_pdr->pdi_ueip.mask.v4,
			upf_accel_pdr->pdi_sdf_proto,
			upf_accel_ip_version_print_string_get(upf_accel_pdr->pdi_sdf_from_ip.ip_version),
			rte_be_to_cpu_32(upf_accel_pdr->pdi_sdf_from_ip.addr.v4),
			upf_accel_pdr->pdi_sdf_from_ip.mask.v4,
			rte_be_to_cpu_16(upf_accel_pdr->pdi_sdf_from_port_range.from),
			rte_be_to_cpu_16(upf_accel_pdr->pdi_sdf_from_port_range.to),
			upf_accel_ip_version_print_string_get(upf_accel_pdr->pdi_sdf_to_ip.ip_version),
			rte_be_to_cpu_32(upf_accel_pdr->pdi_sdf_to_ip.addr.v4),
			upf_accel_pdr->pdi_sdf_to_ip.mask.v4,
			rte_be_to_cpu_16(upf_accel_pdr->pdi_sdf_to_port_range.from),
			rte_be_to_cpu_16(upf_accel_pdr->pdi_sdf_to_port_range.to));
	}

	cfg->pdrs = pdrs;
	return DOCA_SUCCESS;

err_pdr:
	rte_free(pdrs);
	return err;
}

/*
 * Cleanup items were created by upf_accel_pdr_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
static void upf_accel_pdr_cleanup(struct upf_accel_config *cfg)
{
	rte_free(cfg->pdrs);
	cfg->pdrs = NULL;
}

/*
 * Parse OH (outer header) group
 *
 * @oh [in]: json object of the group
 * @upf_accel_far [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_oh_parse(struct json_object *oh, struct upf_accel_far *upf_accel_far)
{
	if (upf_accel_json_u32_parse(oh, "teid", &upf_accel_far->fp_oh_teid) != DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	if (upf_accel_json_ip_parse(oh, "ip", &upf_accel_far->fp_oh_ip) != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse JSON object IP from object: %s",
			     json_object_to_json_string_ext(oh, JSON_C_TO_STRING_PRETTY));
		return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse FP (Forwarding Policy) group
 *
 * @fp [in]: json object of the group
 * @upf_accel_far [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_parse(struct json_object *fp, struct upf_accel_far *upf_accel_far)
{
	struct json_object *oh;

	if (json_object_object_get_ex(fp, "outerHeader", &oh)) {
		if (upf_accel_oh_parse(oh, upf_accel_far) != DOCA_SUCCESS)
			return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse list of CreateFAR nodes from json
 *
 * @far_arr [in]: list of CreaeFAR json nodes
 * @cfg [out]: the result is stored inside cfg
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_far_parse(struct json_object *far_arr, struct upf_accel_config *cfg)
{
	struct json_object *far;
	struct upf_accel_far *upf_accel_far;
	struct json_object *fp;
	struct upf_accel_fars *fars;
	doca_error_t err;
	size_t num_fars;
	size_t i;

	num_fars = json_object_array_length(far_arr);
	fars = rte_zmalloc("UPF FARs", sizeof(*fars) + sizeof(fars->arr_fars[0]) * num_fars, RTE_CACHE_LINE_SIZE);
	if (!fars) {
		DOCA_LOG_ERR("Failed to allocate FAR memory");
		return DOCA_ERROR_NO_MEMORY;
	}
	fars->num_fars = num_fars;

	for (i = 0; i < num_fars; i++) {
		far = json_object_array_get_idx(far_arr, i);
		upf_accel_far = &fars->arr_fars[i];
		if (!far) {
			DOCA_LOG_ERR("Failed to parse JSON FAR object id %lu", i);
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}
		DOCA_LOG_DBG("FAR:\n%s", json_object_to_json_string_ext(far, JSON_C_TO_STRING_PRETTY));

		if (upf_accel_json_u32_parse(far, "farId", &upf_accel_far->id) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}

		if (json_object_object_get_ex(far, "fp", &fp)) {
			err = upf_accel_fp_parse(fp, upf_accel_far);
			if (err != DOCA_SUCCESS)
				goto err_far;
		}

		DOCA_LOG_INFO("Parsed FAR id=%u:\n"
			      "\tOuter Header ip=%x/%hhu",
			      upf_accel_far->id,
			      rte_be_to_cpu_32(upf_accel_far->fp_oh_ip.addr.v4),
			      upf_accel_far->fp_oh_ip.mask.v4);
	}

	cfg->fars = fars;
	return DOCA_SUCCESS;

err_far:
	rte_free(fars);
	return err;
}

/*
 * Cleanup items were created by upf_accel_far_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
static void upf_accel_far_cleanup(struct upf_accel_config *cfg)
{
	rte_free(cfg->fars);
	cfg->fars = NULL;
}

/*
 * Parse volume quota group
 *
 * @volume_quota [in]: json object of the group
 * @upf_accel_urr [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_volume_quota_parse(struct json_object *volume_quota, struct upf_accel_urr *upf_accel_urr)
{
	if (upf_accel_json_u64_parse(volume_quota, "totalVolume", &upf_accel_urr->volume_quota_total_volume) !=
	    DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	return DOCA_SUCCESS;
}

/*
 * Parse list of CreateURR nodes from json
 *
 * @urr_arr [in]: list of CreaeURR json nodes
 * @cfg [out]: the result is stored inside cfg
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_urr_parse(struct json_object *urr_arr, struct upf_accel_config *cfg)
{
	struct json_object *volume_quota;
	struct json_object *urr;
	struct upf_accel_urr *upf_accel_urr;
	struct upf_accel_urrs *urrs;
	doca_error_t err;
	size_t num_urrs;
	size_t i;

	num_urrs = json_object_array_length(urr_arr);
	urrs = rte_zmalloc("UPF URRs", sizeof(*urrs) + sizeof(urrs->arr_urrs[0]) * num_urrs, RTE_CACHE_LINE_SIZE);
	if (!urrs) {
		DOCA_LOG_ERR("Failed to allocate URR memory");
		return DOCA_ERROR_NO_MEMORY;
	}
	urrs->num_urrs = num_urrs;

	for (i = 0; i < num_urrs; i++) {
		urr = json_object_array_get_idx(urr_arr, i);
		upf_accel_urr = &urrs->arr_urrs[i];
		if (!urr) {
			DOCA_LOG_ERR("Failed to parse JSON URR object id %lu", i);
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}
		DOCA_LOG_DBG("URR:\n%s", json_object_to_json_string_ext(urr, JSON_C_TO_STRING_PRETTY));

		if (upf_accel_json_u32_parse(urr, "urrId", &upf_accel_urr->id) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}

		if (!json_object_object_get_ex(urr, "volumeQuota", &volume_quota)) {
			DOCA_LOG_ERR("Failed to parse JSON URR volume quota child object");
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}
		err = upf_accel_volume_quota_parse(volume_quota, upf_accel_urr);
		if (err != DOCA_SUCCESS)
			goto err_far;

		DOCA_LOG_INFO("Parsed URR id=%u volume_quota_total_volume=%lu\n",
			      upf_accel_urr->id,
			      upf_accel_urr->volume_quota_total_volume);
	}

	cfg->urrs = urrs;
	return DOCA_SUCCESS;

err_far:
	rte_free(urrs);
	return err;
}

/*
 * Cleanup items were created by upf_accel_urr_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
static void upf_accel_urr_cleanup(struct upf_accel_config *cfg)
{
	rte_free(cfg->urrs);
	cfg->urrs = NULL;
}

/*
 * Parse MBR group
 *
 * @mbr [in]: json object of the group
 * @upf_accel_qer [out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_mbr_parse(struct json_object *mbr, struct upf_accel_qer *upf_accel_qer)
{
	if (upf_accel_json_u64_from_string_parse(mbr, "dlMBR", &upf_accel_qer->mbr_dl_mbr) != DOCA_SUCCESS ||
	    upf_accel_json_u64_from_string_parse(mbr, "ulMBR", &upf_accel_qer->mbr_ul_mbr) != DOCA_SUCCESS)
		return DOCA_ERROR_UNEXPECTED;

	return DOCA_SUCCESS;
}

/*
 * Parse list of CreateQER nodes from json
 *
 * @qer_arr [in]: list of CreaeQER json nodes
 * @cfg [out]: the result is stored inside cfg
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_qer_parse(struct json_object *qer_arr, struct upf_accel_config *cfg)
{
	struct json_object *mbr;
	struct json_object *qer;
	struct upf_accel_qer *upf_accel_qer;
	struct upf_accel_qers *qers;
	doca_error_t err;
	size_t num_qers;
	size_t i;

	num_qers = json_object_array_length(qer_arr);
	qers = rte_zmalloc("UPF QERs", sizeof(*qers) + sizeof(qers->arr_qers[0]) * num_qers, RTE_CACHE_LINE_SIZE);
	if (!qers) {
		DOCA_LOG_ERR("Failed to allocate QER memory");
		return DOCA_ERROR_NO_MEMORY;
	}
	qers->num_qers = num_qers;
	if (num_qers > UPF_ACCEL_MAX_PDR_NUM_RATE_METERS) {
		DOCA_LOG_ERR("Max Supported Meters Tables Num is: %lu", UPF_ACCEL_MAX_PDR_NUM_RATE_METERS);
		err = DOCA_ERROR_INVALID_VALUE;
		goto err_far;
	}

	for (i = 0; i < num_qers; i++) {
		qer = json_object_array_get_idx(qer_arr, i);
		upf_accel_qer = &qers->arr_qers[i];
		if (!qer) {
			DOCA_LOG_ERR("Failed to parse JSON QER object id %lu", i);
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}
		DOCA_LOG_DBG("QER:\n%s", json_object_to_json_string_ext(qer, JSON_C_TO_STRING_PRETTY));

		if (upf_accel_json_u32_parse(qer, "qerId", &upf_accel_qer->id) != DOCA_SUCCESS ||
		    upf_accel_json_u8_from_string_parse(qer, "qfi", &upf_accel_qer->qfi) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}

		if (!json_object_object_get_ex(qer, "maxBitRate", &mbr)) {
			DOCA_LOG_ERR("Failed to parse JSON QER mbr child object");
			err = DOCA_ERROR_UNEXPECTED;
			goto err_far;
		}
		err = upf_accel_mbr_parse(mbr, upf_accel_qer);
		if (err != DOCA_SUCCESS)
			goto err_far;

		DOCA_LOG_INFO("Parsed QER id=%u\n"
			      "\tqfi=%hhu\n"
			      "\tMBR dl=%lu ul=%lu\n",
			      upf_accel_qer->id,
			      upf_accel_qer->qfi,
			      upf_accel_qer->mbr_dl_mbr,
			      upf_accel_qer->mbr_ul_mbr);
	}

	cfg->qers = qers;
	return DOCA_SUCCESS;

err_far:
	rte_free(qers);
	return err;
}

/*
 * Cleanup items were created by upf_accel_qer_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
static void upf_accel_qer_cleanup(struct upf_accel_config *cfg)
{
	rte_free(cfg->qers);
	cfg->qers = NULL;
}

/*
 * Parse SMF input
 *
 * @cfg [out]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t upf_accel_smf_parse(struct upf_accel_config *cfg)
{
	struct json_object *pdr_arr;
	struct json_object *far_arr;
	struct json_object *urr_arr;
	struct json_object *qer_arr;
	struct json_object *root;
	doca_error_t err;

	root = json_object_from_file(cfg->smf_config_file_path);
	if (!root) {
		DOCA_LOG_ERR("Failed to parse JSON SMF config: %s", cfg->smf_config_file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (!json_object_object_get_ex(root, "createPdr", &pdr_arr) ||
	    json_object_get_type(pdr_arr) != json_type_array) {
		DOCA_LOG_ERR("Failed to parse JSON PDR array");
		err = DOCA_ERROR_EMPTY;
		goto err_json;
	}

	if (!json_object_object_get_ex(root, "createFar", &far_arr) ||
	    json_object_get_type(far_arr) != json_type_array) {
		DOCA_LOG_ERR("Failed to parse JSON FAR array");
		err = DOCA_ERROR_EMPTY;
		goto err_json;
	}

	if (!json_object_object_get_ex(root, "createUrr", &urr_arr) ||
	    json_object_get_type(urr_arr) != json_type_array) {
		DOCA_LOG_ERR("Failed to parse JSON URR array");
		err = DOCA_ERROR_EMPTY;
		goto err_json;
	}

	if (!json_object_object_get_ex(root, "createQer", &qer_arr) ||
	    json_object_get_type(qer_arr) != json_type_array) {
		DOCA_LOG_ERR("Failed to parse JSON QER array");
		err = DOCA_ERROR_EMPTY;
		goto err_json;
	}

	err = upf_accel_pdr_parse(pdr_arr, cfg);
	if (err != DOCA_SUCCESS)
		goto err_json;

	err = upf_accel_far_parse(far_arr, cfg);
	if (err != DOCA_SUCCESS)
		goto err_pdr;

	err = upf_accel_urr_parse(urr_arr, cfg);
	if (err != DOCA_SUCCESS)
		goto err_far;

	err = upf_accel_qer_parse(qer_arr, cfg);
	if (err != DOCA_SUCCESS)
		goto err_urr;

	json_object_put(root);
	return DOCA_SUCCESS;

err_urr:
	upf_accel_urr_cleanup(cfg);
err_far:
	upf_accel_far_cleanup(cfg);
err_pdr:
	upf_accel_pdr_cleanup(cfg);
err_json:
	json_object_put(root);
	return err;
}

/*
 * Cleanup items were created by upf_accel_smf_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
void upf_accel_smf_cleanup(struct upf_accel_config *cfg)
{
	upf_accel_qer_cleanup(cfg);
	upf_accel_urr_cleanup(cfg);
	upf_accel_far_cleanup(cfg);
	upf_accel_pdr_cleanup(cfg);
}

/*
 * Parse list of CreateVXLAN nodes from json
 *
 * @vxlan_arr [in]: list of CreaeVXLAN json nodes
 * @cfg [out]: the result is stored inside cfg
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_vxlan_arr_parse(struct json_object *vxlan_arr, struct upf_accel_config *cfg)
{
	struct upf_accel_vxlan *upf_accel_vxlan;
	struct upf_accel_vxlans *vxlans;
	struct json_object *vxlan;
	size_t num_vxlans;
	doca_error_t err;
	size_t i;

	num_vxlans = json_object_array_length(vxlan_arr);
	vxlans = rte_zmalloc("UPF VXLANs",
			     sizeof(*vxlans) + sizeof(vxlans->arr_vxlans[0]) * num_vxlans,
			     RTE_CACHE_LINE_SIZE);
	if (!vxlans) {
		DOCA_LOG_ERR("Failed to allocate VXLAN memory");
		return DOCA_ERROR_NO_MEMORY;
	}
	vxlans->num_vxlans = num_vxlans;

	for (i = 0; i < num_vxlans; i++) {
		vxlan = json_object_array_get_idx(vxlan_arr, i);
		upf_accel_vxlan = &vxlans->arr_vxlans[i];
		if (!vxlan) {
			DOCA_LOG_ERR("Failed to parse JSON VXLAN object id %lu", i);
			err = DOCA_ERROR_UNEXPECTED;
			goto err_vxlan;
		}
		DOCA_LOG_DBG("VXLAN:\n%s", json_object_to_json_string_ext(vxlan, JSON_C_TO_STRING_PRETTY));

		if (upf_accel_json_u32_parse(vxlan, "vxlanId", &upf_accel_vxlan->id) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_vxlan;
		}

		if (upf_accel_json_u32_parse(vxlan, "vni", &upf_accel_vxlan->vni) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_vxlan;
		}

		if (upf_accel_json_mac_from_string_parse(vxlan, "mac", upf_accel_vxlan->mac) != DOCA_SUCCESS) {
			err = DOCA_ERROR_UNEXPECTED;
			goto err_vxlan;
		}

		DOCA_LOG_INFO("Parsed VXLAN id=%u:\n"
			      "\tMAC=%hhu:%hhu:%hhu:%hhu:%hhu:%hhu\n"
			      "\tVNI=%u\n",
			      upf_accel_vxlan->id,
			      upf_accel_vxlan->mac[0],
			      upf_accel_vxlan->mac[1],
			      upf_accel_vxlan->mac[2],
			      upf_accel_vxlan->mac[3],
			      upf_accel_vxlan->mac[4],
			      upf_accel_vxlan->mac[5],
			      upf_accel_vxlan->vni);
	}

	cfg->vxlans = vxlans;
	return DOCA_SUCCESS;

err_vxlan:
	rte_free(vxlans);
	return err;
}

/*
 * Cleanup items created by upf_accel_vxlan_arr_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
static void upf_accel_vxlan_arr_cleanup(struct upf_accel_config *cfg)
{
	rte_free(cfg->vxlans);
	cfg->vxlans = NULL;
}

/*
 * Parse VXLAN input
 *
 * @cfg [out]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t upf_accel_vxlan_parse(struct upf_accel_config *cfg)
{
	struct json_object *vxlan_arr;
	struct json_object *root;
	doca_error_t err;

	root = json_object_from_file(cfg->vxlan_config_file_path);
	if (!root) {
		DOCA_LOG_ERR("Failed to parse JSON VXLAN config: %s", cfg->vxlan_config_file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (!json_object_object_get_ex(root, "CreateVXLAN", &vxlan_arr) ||
	    json_object_get_type(vxlan_arr) != json_type_array) {
		DOCA_LOG_ERR("Failed to parse JSON VXLAN array");
		err = DOCA_ERROR_EMPTY;
		goto err_json;
	}

	err = upf_accel_vxlan_arr_parse(vxlan_arr, cfg);
	if (err != DOCA_SUCCESS)
		goto err_json;

	json_object_put(root);
	return DOCA_SUCCESS;

err_json:
	json_object_put(root);
	return err;
}

/*
 * Cleanup items created by upf_accel_vxlan_parse
 *
 * @cfg [out]: UPF Acceleration configuration
 */
void upf_accel_vxlan_cleanup(struct upf_accel_config *cfg)
{
	upf_accel_vxlan_arr_cleanup(cfg);
}
