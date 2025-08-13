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

#include <ctype.h>
#include <json-c/json.h>

#include <rte_hash_crc.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <pack.h>
#include <utils.h>
#include <flow_parser.h>

#include "config.h"

DOCA_LOG_REGISTER(IPSEC_SECURITY_GW::config);

#define MAX_CORES (32)

/*
 * Parse hex key string to array of uint8_t
 *
 * @key_hex [in]: hex format key string
 * @key_size [in]: the hex string length
 * @key [out]: the parsed key array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_hex_to_bytes(const char *key_hex, size_t key_size, uint8_t *key)
{
	uint8_t digit;
	size_t i;

	/* Parse every digit (nibble and translate it to the matching numeric value) */
	for (i = 0; i < key_size; i++) {
		/* Must be alpha-numeric */
		if ('0' <= key_hex[i] && key_hex[i] <= '9')
			digit = key_hex[i] - '0';
		else if ('a' <= tolower(key_hex[i]) && tolower(key_hex[i]) <= 'f')
			digit = tolower(key_hex[i]) - 'a' + 10;
		else {
			DOCA_LOG_ERR("Wrong format for key (%s) - not alpha-numeric", key_hex);
			return DOCA_ERROR_INVALID_VALUE;
		}
		/* There are 2 nibbles (digits) in each byte, place them at their numeric place */
		key[i / 2] = (key[i / 2] << 4) + digit;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse key from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @key_type [in]: key type
 * @key [out]: the parsed key value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_key(struct json_object *cur_rule, enum doca_flow_crypto_key_type key_type, uint8_t *key)
{
	struct json_object *json_key;
	doca_error_t result;
	const char *key_str;
	size_t max_key_size;
	size_t key_len;

	if (!json_object_object_get_ex(cur_rule, "key", &json_key)) {
		DOCA_LOG_ERR("Missing key");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (json_object_get_type(json_key) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"key\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	key_str = json_object_get_string(json_key);
	if (key_type == DOCA_FLOW_CRYPTO_KEY_128)
		max_key_size = 32;
	else
		max_key_size = 64;
	key_len = strnlen(key_str, max_key_size + 1);
	if (key_len == max_key_size + 1) {
		DOCA_LOG_ERR("Key string is too long - MAX=%ld", max_key_size);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (key_len % 2 != 0) {
		DOCA_LOG_ERR("Key string should be in hexadecimal format, length must be even");
		return DOCA_ERROR_INVALID_VALUE;
	}
	result = parse_hex_to_bytes(key_str, key_len, key);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Parse key type from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @key_type [out]: the parse key type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_key_type(struct json_object *cur_rule, enum doca_flow_crypto_key_type *key_type)
{
	struct json_object *json_key_type;
	int key_len;

	if (!json_object_object_get_ex(cur_rule, "key-type", &json_key_type)) {
		DOCA_LOG_DBG("Missing key type, default is DOCA_ENCRYPTION_KEY_AESGCM_256");
		*key_type = DOCA_FLOW_CRYPTO_KEY_256;
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(json_key_type) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"key-type\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	key_len = json_object_get_int(json_key_type);
	if (key_len == 128)
		*key_type = DOCA_FLOW_CRYPTO_KEY_128;
	else if (key_len == 256)
		*key_type = DOCA_FLOW_CRYPTO_KEY_256;
	else {
		DOCA_LOG_ERR("Key type should be 128 / 256");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse IV from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @iv [out]: the parsed iv value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_iv(struct json_object *cur_rule, uint64_t *iv)
{
	struct json_object *json_iv;
	doca_error_t result;
	const char *iv_str;
	int iv_len;
	const int max_iv_size = 8 * 2;
	uint8_t iv_bytes[8] = {0};
	uint8_t *read_head = iv_bytes;

	if (!json_object_object_get_ex(cur_rule, "iv", &json_iv)) {
		DOCA_LOG_DBG("Missing IV, will use the default value");
		*iv = unpack_uint64(&read_head);
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(json_iv) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"iv\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	iv_str = json_object_get_string(json_iv);
	iv_len = strnlen(iv_str, max_iv_size + 1);
	if (iv_len == max_iv_size + 1) {
		DOCA_LOG_ERR("IV string is too long - MAX=%d", max_iv_size);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (iv_len % 2 != 0) {
		DOCA_LOG_ERR("IV string should be in hexadecimal format, length must be even");
		return DOCA_ERROR_INVALID_VALUE;
	}
	result = parse_hex_to_bytes(iv_str, iv_len, iv_bytes);
	if (result != DOCA_SUCCESS)
		return result;

	*iv = unpack_uint64(&read_head);
	return DOCA_SUCCESS;
}

/*
 * Parse json object for salt length
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @salt [out]: the parse salt
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_salt(struct json_object *cur_rule, uint32_t *salt)
{
	struct json_object *json_salt;

	if (!json_object_object_get_ex(cur_rule, "salt", &json_salt)) {
		DOCA_LOG_DBG("Missing salt, default is 6");
		*salt = 6;
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(json_salt) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"salt\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	*salt = (uint32_t)json_object_get_int64(json_salt);
	return DOCA_SUCCESS;
}

/*
 * Parse json object for lifetime threshold
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @lifetime_threshold [out]: the parsed lifetime threshold
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_lifetime_threshold(struct json_object *cur_rule, uint32_t *lifetime_threshold)
{
	struct json_object *json_lifetime;

	if (!json_object_object_get_ex(cur_rule, "lifetime-threshold", &json_lifetime)) {
		DOCA_LOG_DBG("Missing lifetime-threshold, default is 0 (ignored)");
		*lifetime_threshold = 0;
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(json_lifetime) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"lifetime-threshold\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	*lifetime_threshold = (uint32_t)json_object_get_int64(json_lifetime);
	return DOCA_SUCCESS;
}

/*
 * Parse json object for esn
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @esn_en [out]: the parsed esn_en
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_esn_en(struct json_object *cur_rule, bool *esn_en)
{
	struct json_object *json_esn_en;

	if (!json_object_object_get_ex(cur_rule, "esn_en", &json_esn_en)) {
		DOCA_LOG_DBG("Missing esn_en, default is false");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(json_esn_en) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"esn_en\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	*esn_en = json_object_get_boolean(json_esn_en);
	return DOCA_SUCCESS;
}

/*
 * Parse protocol type from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @protocol [out]: the parsed protocol value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_protocol(struct json_object *cur_rule, enum doca_flow_l4_type_ext *protocol)
{
	doca_error_t result;
	struct json_object *json_protocol;
	const char *protocol_str;

	if (!json_object_object_get_ex(cur_rule, "protocol", &json_protocol)) {
		DOCA_LOG_ERR("Missing protocol type");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (json_object_get_type(json_protocol) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"protocol\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	protocol_str = json_object_get_string(json_protocol);
	result = parse_protocol_string(protocol_str, protocol);
	if (result != DOCA_SUCCESS)
		return result;
	return DOCA_SUCCESS;
}

/*
 * Parse l3 type from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @ip_version_string [in]: ip-version/encap-ip-version/inner-ip-version
 * @l3_type [out]: the parsed l3_type value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_l3_type(struct json_object *cur_rule,
				   char *ip_version_string,
				   enum doca_flow_l3_type *l3_type)
{
	struct json_object *json_ip;
	int ip;

	if (!json_object_object_get_ex(cur_rule, ip_version_string, &json_ip)) {
		DOCA_LOG_DBG("Missing ip-version, using default: IPv4");
		*l3_type = DOCA_FLOW_L3_TYPE_IP4;
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(json_ip) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"ip-version\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	ip = json_object_get_int(json_ip);
	if (ip == 4)
		*l3_type = DOCA_FLOW_L3_TYPE_IP4;
	else if (ip == 6)
		*l3_type = DOCA_FLOW_L3_TYPE_IP6;
	else {
		DOCA_LOG_ERR("Expecting 4 or 6 value for \"ip-version\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse IPv4 address from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @ip_type [in]: src-ip/dst-ip/encap-dst-ip
 * @ip [out]: the parsed dst_ip value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipv4(struct json_object *cur_rule, char *ip_type, doca_be32_t *ip)
{
	doca_error_t result;
	struct json_object *json_ip;

	if (!json_object_object_get_ex(cur_rule, ip_type, &json_ip)) {
		DOCA_LOG_ERR("Missing %s", ip_type);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (json_object_get_type(json_ip) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"%s\"", ip_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = parse_ipv4_str(json_object_get_string(json_ip), ip);
	if (result != DOCA_SUCCESS)
		return result;
	return DOCA_SUCCESS;
}

doca_error_t parse_ipv6_str(const char *str_ip, doca_be32_t ipv6_addr[])
{
	int i;
	struct in6_addr ip;

	if (inet_pton(AF_INET6, str_ip, &ip) != 1) {
		DOCA_LOG_ERR("Wrong format of ipv6 address");
		return DOCA_ERROR_INVALID_VALUE;
	}
	for (i = 0; i < 4; i++)
		ipv6_addr[i] = ip.__in6_u.__u6_addr32[i];
	return DOCA_SUCCESS;
}

/*
 * Parse IPv6 address from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @ip_type [in]: src-ip/dst-ip/encap-dst-ip
 * @ip [out]: the parsed ip value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ipv6(struct json_object *cur_rule, char *ip_type, doca_be32_t ip[])
{
	doca_error_t result;
	struct json_object *json_ip;

	if (!json_object_object_get_ex(cur_rule, ip_type, &json_ip)) {
		DOCA_LOG_ERR("Missing %s", ip_type);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (json_object_get_type(json_ip) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"%s\"", ip_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = parse_ipv6_str(json_object_get_string(json_ip), ip);
	if (result != DOCA_SUCCESS)
		return result;
	return DOCA_SUCCESS;
}

/*
 * Parse port from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @port_type [in]: src-port/dst-port
 * @port [out]: the parsed src_port value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_port(struct json_object *cur_rule, char *port_type, int *port)
{
	struct json_object *json_port;

	if (!json_object_object_get_ex(cur_rule, port_type, &json_port)) {
		DOCA_LOG_ERR("Missing %s", port_type);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (json_object_get_type(json_port) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"%s\"", port_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	*port = json_object_get_int(json_port);
	return DOCA_SUCCESS;
}

/*
 * Parse SPI from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @esp_spi [out]: the parsed esp_spi value
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_spi(struct json_object *cur_rule, doca_be32_t *esp_spi)
{
	struct json_object *json_spi;

	if (!json_object_object_get_ex(cur_rule, "spi", &json_spi)) {
		DOCA_LOG_ERR("Missing spi");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (json_object_get_type(json_spi) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"spi\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	*esp_spi = (uint32_t)json_object_get_int64(json_spi);
	return DOCA_SUCCESS;
}

/*
 * Parse IPv4 encrypt addresses from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @rule [out]: the current encrypt rule to fill
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_encrypt_ipv4(struct json_object *cur_rule, struct encrypt_rule *rule)
{
	doca_error_t result;

	result = create_ipv4(cur_rule, "src-ip", &rule->ip4.src_ip);
	if (result != DOCA_SUCCESS)
		return result;

	result = create_ipv4(cur_rule, "dst-ip", &rule->ip4.dst_ip);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Parse IPv6 encrypt addresses from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @rule [out]: the current encrypt rule to fill
 * @ip6_table [out]: created hash table with IP addresses
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_encrypt_ipv6(struct json_object *cur_rule,
				       struct encrypt_rule *rule,
				       struct rte_hash **ip6_table)
{
	int ret;
	doca_error_t result;

	result = create_ipv6(cur_rule, "src-ip", rule->ip6.src_ip);
	if (result != DOCA_SUCCESS)
		return result;

	result = create_ipv6(cur_rule, "dst-ip", rule->ip6.dst_ip);
	if (result != DOCA_SUCCESS)
		return result;

	ret = rte_hash_lookup(*ip6_table, (void *)rule->ip6.src_ip);
	if (ret < 0) {
		ret = rte_hash_add_key(*ip6_table, rule->ip6.src_ip);
		if (ret < 0) {
			DOCA_LOG_ERR("Failed to add address to hash table");
			return DOCA_ERROR_DRIVER;
		}
	}

	ret = rte_hash_lookup(*ip6_table, (void *)rule->ip6.dst_ip);
	if (ret < 0) {
		ret = rte_hash_add_key(*ip6_table, rule->ip6.dst_ip);
		if (ret < 0) {
			DOCA_LOG_ERR("Failed to add address to hash table");
			return DOCA_ERROR_DRIVER;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Parse encap IP addresses from json object rule
 *
 * @cur_rule [in]: json object of the current rule to parse
 * @rule [out]: the current encrypt rule to fill
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_encrypt_encap_ip(struct json_object *cur_rule, struct encrypt_rule *rule)
{
	doca_error_t result;

	if (rule->encap_l3_type == DOCA_FLOW_L3_TYPE_IP4) {
		result = create_ipv4(cur_rule, "encap-dst-ip", &rule->encap_dst_ip4);
		if (result != DOCA_SUCCESS)
			return result;
	} else {
		result = create_ipv6(cur_rule, "encap-dst-ip", rule->encap_dst_ip6);
		if (result != DOCA_SUCCESS)
			return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse json object of the decryption rules and set it in decrypt_rules array
 *
 * @json_rules [in]: json object of the rules to parse
 * @app_cfg [in/out]: internal array will hold the decryption rules
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_json_decrypt_rules(struct json_object *json_rules, struct ipsec_security_gw_config *app_cfg)
{
	int i;
	doca_error_t result;
	struct json_object *cur_rule;

	DOCA_LOG_DBG("Number of decrypt rules in input file: %d", app_cfg->app_rules.nb_decrypt_rules);

	for (i = 0; i < app_cfg->app_rules.nb_decrypt_rules; i++) {
		cur_rule = json_object_array_get_idx(json_rules, i);
		result = create_l3_type(cur_rule, "ip-version", &app_cfg->app_rules.decrypt_rules[i].l3_type);
		if (result != DOCA_SUCCESS)
			return result;

		if (app_cfg->app_rules.decrypt_rules[i].l3_type == DOCA_FLOW_L3_TYPE_IP4) {
			result = create_ipv4(cur_rule, "dst-ip", &app_cfg->app_rules.decrypt_rules[i].dst_ip4);
			if (result != DOCA_SUCCESS)
				return result;
		} else {
			result = create_ipv6(cur_rule, "dst-ip", app_cfg->app_rules.decrypt_rules[i].dst_ip6);
			if (result != DOCA_SUCCESS)
				return result;
		}
		result = create_spi(cur_rule, &app_cfg->app_rules.decrypt_rules[i].esp_spi);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_l3_type(cur_rule,
					"inner-ip-version",
					&app_cfg->app_rules.decrypt_rules[i].inner_l3_type);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_key_type(cur_rule, &app_cfg->app_rules.decrypt_rules[i].sa_attrs.key_type);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_key(cur_rule,
				    app_cfg->app_rules.decrypt_rules[i].sa_attrs.key_type,
				    app_cfg->app_rules.decrypt_rules[i].sa_attrs.enc_key_data);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_iv(cur_rule, &app_cfg->app_rules.decrypt_rules[i].sa_attrs.iv);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_salt(cur_rule, &app_cfg->app_rules.decrypt_rules[i].sa_attrs.salt);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_lifetime_threshold(cur_rule,
						   &app_cfg->app_rules.decrypt_rules[i].sa_attrs.lifetime_threshold);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_esn_en(cur_rule, &app_cfg->app_rules.decrypt_rules[i].sa_attrs.esn_en);
		if (result != DOCA_SUCCESS)
			return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse json object of the encryption rules and set it in encrypt_rules array
 *
 * @json_rules [in]: json object of the rules to parse
 * @app_cfg [in/out]: application configuration structure, will holds the encryption rules
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_json_encrypt_rules(struct json_object *json_rules, struct ipsec_security_gw_config *app_cfg)
{
	int i;
	doca_error_t result;
	struct json_object *cur_rule;

	DOCA_LOG_DBG("Number of encrypt rules in input file: %d", app_cfg->app_rules.nb_encrypt_rules);

	for (i = 0; i < app_cfg->app_rules.nb_encrypt_rules; i++) {
		cur_rule = json_object_array_get_idx(json_rules, i);
		result = create_l3_type(cur_rule, "ip-version", &app_cfg->app_rules.encrypt_rules[i].l3_type);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_protocol(cur_rule, &app_cfg->app_rules.encrypt_rules[i].protocol);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_l3_type(cur_rule,
					"encap-ip-version",
					&app_cfg->app_rules.encrypt_rules[i].encap_l3_type);
		if (result != DOCA_SUCCESS)
			return result;

		if (app_cfg->app_rules.encrypt_rules[i].l3_type == DOCA_FLOW_L3_TYPE_IP4) {
			result = parse_encrypt_ipv4(cur_rule, &app_cfg->app_rules.encrypt_rules[i]);
			if (result != DOCA_SUCCESS)
				return result;
		} else {
			result =
				parse_encrypt_ipv6(cur_rule, &app_cfg->app_rules.encrypt_rules[i], &app_cfg->ip6_table);
			if (result != DOCA_SUCCESS)
				return result;
		}
		if (app_cfg->mode == IPSEC_SECURITY_GW_TUNNEL) {
			result = parse_encrypt_encap_ip(cur_rule, &app_cfg->app_rules.encrypt_rules[i]);
			if (result != DOCA_SUCCESS)
				return result;
		}
		result = create_port(cur_rule, "src-port", &app_cfg->app_rules.encrypt_rules[i].src_port);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_port(cur_rule, "dst-port", &app_cfg->app_rules.encrypt_rules[i].dst_port);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_spi(cur_rule, &app_cfg->app_rules.encrypt_rules[i].esp_spi);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_key_type(cur_rule, &app_cfg->app_rules.encrypt_rules[i].sa_attrs.key_type);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_key(cur_rule,
				    app_cfg->app_rules.encrypt_rules[i].sa_attrs.key_type,
				    app_cfg->app_rules.encrypt_rules[i].sa_attrs.enc_key_data);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_iv(cur_rule, &app_cfg->app_rules.encrypt_rules[i].sa_attrs.iv);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_salt(cur_rule, &app_cfg->app_rules.encrypt_rules[i].sa_attrs.salt);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_lifetime_threshold(cur_rule,
						   &app_cfg->app_rules.encrypt_rules[i].sa_attrs.lifetime_threshold);
		if (result != DOCA_SUCCESS)
			return result;

		result = create_esn_en(cur_rule, &app_cfg->app_rules.encrypt_rules[i].sa_attrs.esn_en);
		if (result != DOCA_SUCCESS)
			return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse json object for ESP offload string
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_esp_offload(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	const char *offload_str;
	struct json_object *esp_header_offload;

	if (!json_object_object_get_ex(json_config, "esp-header-offload", &esp_header_offload)) {
		DOCA_LOG_DBG("Missing \"esp-header-offload\" parameter, default is offload both");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(esp_header_offload) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"esp-header-offload\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	offload_str = json_object_get_string(esp_header_offload);
	if (strcmp(offload_str, "both") == 0)
		app_cfg->offload = IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH;
	else if (strcmp(offload_str, "encap") == 0)
		app_cfg->offload = IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP;
	else if (strcmp(offload_str, "decap") == 0)
		app_cfg->offload = IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP;
	else if (strcmp(offload_str, "none") == 0)
		app_cfg->offload = IPSEC_SECURITY_GW_ESP_OFFLOAD_NONE;
	else {
		DOCA_LOG_ERR("ESP offload type %s is not supported", offload_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse json object for perf measurement string
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_perf_measurement(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	const char *perf_str;
	struct json_object *perf_measure;

	if (!json_object_object_get_ex(json_config, "perf-measurements", &perf_measure)) {
		DOCA_LOG_DBG("Missing \"perf-measurements\" parameter, default is none");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(perf_measure) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"perf-measurements\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	perf_str = json_object_get_string(perf_measure);
	if (strcmp(perf_str, "none") == 0)
		app_cfg->perf_measurement = IPSEC_SECURITY_GW_PERF_NONE;
	else if (strcmp(perf_str, "insertion-rate") == 0)
		app_cfg->perf_measurement = IPSEC_SECURITY_GW_PERF_INSERTION_RATE;
	else if (strcmp(perf_str, "bandwidth") == 0)
		app_cfg->perf_measurement = IPSEC_SECURITY_GW_PERF_BW;
	else if (strcmp(perf_str, "both") == 0)
		app_cfg->perf_measurement = IPSEC_SECURITY_GW_PERF_BOTH;
	else {
		DOCA_LOG_ERR("Perf type %s is not supported", perf_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse json object for SW anti-replay
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_antireplay(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *sw_antireplay_enable;

	if (!json_object_object_get_ex(json_config, "sw-antireplay-enable", &sw_antireplay_enable)) {
		DOCA_LOG_DBG("Missing \"sw-antireplay-enable\" parameter, using false as default");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(sw_antireplay_enable) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"sw-antireplay-enable\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	app_cfg->sw_antireplay = json_object_get_boolean(sw_antireplay_enable);
	return DOCA_SUCCESS;
}

/*
 * Parse json object for SW SN increment
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_sn_inc(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *sw_sn_inc_enable;

	if (!json_object_object_get_ex(json_config, "sw-sn-inc-enable", &sw_sn_inc_enable)) {
		DOCA_LOG_DBG("Missing \"sw-sn-inc-enable\" parameter, using false as default");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(sw_sn_inc_enable) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"sw-sn-inc-enable\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	app_cfg->sw_sn_inc_enable = json_object_get_boolean(sw_sn_inc_enable);
	return DOCA_SUCCESS;
}

/*
 * Parse json object for SN initial
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_sn_initial(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *sn_init_config;
	int64_t sn_init;

	if (!json_object_object_get_ex(json_config, "sn-initial", &sn_init_config)) {
		DOCA_LOG_DBG("Missing \"sn-initial\" parameter, using 1 as the default value");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(sn_init_config) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"sn-initial\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	sn_init = json_object_get_int64(sn_init_config); /* notice that in future use of 64 bits for SN (for e.g. ESN)
							    the maximum is 2^63 since we're reading int64 and not
							    uint64
							  */
	if (sn_init < 0 || sn_init > UINT32_MAX) {
		DOCA_LOG_ERR("\"sn-initial\" should get non-negative 32 bits value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->sn_initial = (uint64_t)sn_init;

	return DOCA_SUCCESS;
}

/*
 * Parse json object for switch configuration
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_switch_config(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *switch_config;
	bool is_switch;

	if (!json_object_object_get_ex(json_config, "switch", &switch_config)) {
		DOCA_LOG_DBG("Missing \"switch\" parameter, using vnf mode");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(switch_config) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"switch\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	is_switch = json_object_get_boolean(switch_config);
	if (is_switch)
		app_cfg->flow_mode = IPSEC_SECURITY_GW_SWITCH;

	return DOCA_SUCCESS;
}

/*
 * Parse json object for debug mode configuration
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_debug_config(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *debug_config;
	bool debug;

	if (!json_object_object_get_ex(json_config, "debug", &debug_config)) {
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(debug_config) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"debug\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	debug = json_object_get_boolean(debug_config);
	if (debug)
		app_cfg->debug_mode = true;

	return DOCA_SUCCESS;
}

/*
 * Parse json object for vxlan encap configuration
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_vxlan_encap_config(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *vxlan_encap_config;
	struct json_object *vni_config;
	bool vxlan_encap;

	if (!json_object_object_get_ex(json_config, "vxlan-encap", &vxlan_encap_config)) {
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(vxlan_encap_config) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"vxlan-encap\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	vxlan_encap = json_object_get_boolean(vxlan_encap_config);
	if (!vxlan_encap)
		return DOCA_SUCCESS;

	app_cfg->vxlan_encap = true;
	if (!json_object_object_get_ex(json_config, "vni", &vni_config)) {
		app_cfg->vni = 1;
		DOCA_LOG_DBG("Missing \"vni\" value, using 1 as default");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(vni_config) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"vni\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	app_cfg->vni = json_object_get_int(vni_config);

	return DOCA_SUCCESS;
}

/*
 * Parse json object for non-ESP marker encap configuration
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_marker_encap_config(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	struct json_object *marker_encap_config;
	bool marker_encap;

	if (!json_object_object_get_ex(json_config, "marker-encap", &marker_encap_config)) {
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(marker_encap_config) != json_type_boolean) {
		DOCA_LOG_ERR("Expecting a bool value for \"marker-encap\"");
		return DOCA_ERROR_INVALID_VALUE;
	}
	marker_encap = json_object_get_boolean(marker_encap_config);
	if (!marker_encap)
		return DOCA_SUCCESS;

	if (app_cfg->vxlan_encap) {
		DOCA_LOG_ERR("Non-ESP marker not supported for VXLAN encapsulation");
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	app_cfg->marker_encap = true;
	return DOCA_SUCCESS;
}

/*
 * Parse json object for bad syndrome fwd string
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_bad_syndrome_fwd(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	const char *fwd_str;
	struct json_object *fwd_obj;

	if (!json_object_object_get_ex(json_config, "fwd-bad-syndrome", &fwd_obj)) {
		DOCA_LOG_DBG("Missing \"fwd-bad-syndrome\" parameter, default is drop");
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(fwd_obj) != json_type_string) {
		DOCA_LOG_ERR("Expecting a string value for \"fwd-bad-syndrome\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	fwd_str = json_object_get_string(fwd_obj);
	if (strcmp(fwd_str, "drop") == 0)
		app_cfg->syndrome_fwd = IPSEC_SECURITY_GW_FWD_SYNDROME_DROP;
	else if (strcmp(fwd_str, "rss") == 0)
		app_cfg->syndrome_fwd = IPSEC_SECURITY_GW_FWD_SYNDROME_RSS;
	else {
		DOCA_LOG_ERR("Forward syndrome type %s is not supported", fwd_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Parse json object for ICV length
 *
 * @json_config [in]: json config object
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_icv_length(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	int icv_length_int;
	struct json_object *icv_length_config;

	if (!json_object_object_get_ex(json_config, "icv-length", &icv_length_config)) {
		DOCA_LOG_DBG("Missing \"icv-length\" parameter, default is 16");
		app_cfg->icv_length = DOCA_FLOW_CRYPTO_ICV_LENGTH_16;
		return DOCA_SUCCESS;
	}
	if (json_object_get_type(icv_length_config) != json_type_int) {
		DOCA_LOG_ERR("Expecting an int value for \"icv-length\"");
		return DOCA_ERROR_INVALID_VALUE;
	}

	icv_length_int = json_object_get_int(icv_length_config);
	switch (icv_length_int) {
	case 8:
		app_cfg->icv_length = DOCA_FLOW_CRYPTO_ICV_LENGTH_8;
		break;
	case 12:
		app_cfg->icv_length = DOCA_FLOW_CRYPTO_ICV_LENGTH_12;
		break;
	case 16:
		app_cfg->icv_length = DOCA_FLOW_CRYPTO_ICV_LENGTH_16;
		break;
	default:
		DOCA_LOG_ERR("ICV length can only be one of the following: 8, 12, 16");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Parse json object of the config
 *
 * @json_config [in]: json object of the config to parse
 * @app_cfg [out]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_json_config(struct json_object *json_config, struct ipsec_security_gw_config *app_cfg)
{
	doca_error_t result;

	result = parse_switch_config(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_antireplay(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_sn_initial(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_esp_offload(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_perf_measurement(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_sn_inc(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_debug_config(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	if (app_cfg->debug_mode) {
		result = parse_bad_syndrome_fwd(json_config, app_cfg);
		if (result != DOCA_SUCCESS)
			return result;
	}

	result = parse_vxlan_encap_config(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_marker_encap_config(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = parse_icv_length(json_config, app_cfg);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Check the input file size and allocate a buffer to read it
 *
 * @fp [in]: file pointer to the input rules file
 * @file_length [out]: total bytes in file
 * @json_data [out]: allocated buffer, with null termination
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t allocate_json_buffer_dynamic(FILE *fp, size_t *file_length, char **json_data)
{
	ssize_t buf_len = 0;

	/* use fseek to put file counter to the end, and calculate file length */
	if (fseek(fp, 0L, SEEK_END) == 0) {
		buf_len = ftell(fp);
		if (buf_len < 0) {
			DOCA_LOG_ERR("ftell() function failed");
			return DOCA_ERROR_IO_FAILED;
		}

		/* dynamic allocation */
		*json_data = (char *)calloc(1, buf_len + 1);
		if (*json_data == NULL) {
			DOCA_LOG_ERR("malloc() function failed");
			return DOCA_ERROR_NO_MEMORY;
		}

		/* return file counter to the beginning */
		if (fseek(fp, 0L, SEEK_SET) != 0) {
			free(*json_data);
			*json_data = NULL;
			DOCA_LOG_ERR("fseek() function failed");
			return DOCA_ERROR_IO_FAILED;
		}
	}
	*file_length = buf_len;
	return DOCA_SUCCESS;
}

/*
 * Run validation check to the parsed application parameters
 * - in VNF mode the application should get both secured and unsecured ports
 * - in switch mode the application should get only secured port. If the unsecured port is passed it will be ignored
 *
 * @app_cfg [in]: application configuration struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t validate_config(struct ipsec_security_gw_config *app_cfg)
{
	if (app_cfg->objects.secured_dev.doca_dev == NULL) {
		DOCA_LOG_ERR("Secure port is missing");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (app_cfg->objects.unsecured_dev.doca_dev == NULL) {
		DOCA_LOG_ERR("Unsecure port is missing");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF && app_cfg->objects.unsecured_dev.is_representor) {
		DOCA_LOG_ERR("Please specify only the device parameter for the unsecured device: -u");
		return DOCA_ERROR_INVALID_VALUE;
	} else if (app_cfg->flow_mode == IPSEC_SECURITY_GW_SWITCH && !app_cfg->objects.unsecured_dev.is_representor) {
		DOCA_LOG_ERR("Please specify the representor parameter for the unsecured device: -ur");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Verify that encap is enabled if SW increment is enabled */
	if (app_cfg->sw_sn_inc_enable && (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH ||
					  app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_ENCAP)) {
		DOCA_LOG_ERR("SW SN Increment cannot be enabled when offloading encap");
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* Verify that encap is enabled if SW anti-replay is enabled */
	if (app_cfg->sw_antireplay && (app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_BOTH ||
				       app_cfg->offload == IPSEC_SECURITY_GW_ESP_OFFLOAD_DECAP)) {
		DOCA_LOG_ERR("SW Anti-Replay cannot be enabled when offloading decap");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Create Hash table for IPv6 addresses
 *
 * @ip6_table [out]: the created hash table for ipv6 addresses
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_ip6_table(struct rte_hash **ip6_table)
{
	struct rte_hash_parameters table_params = {
		.name = "IPv6 table",
		.entries = MAX_NB_RULES * 2,
		.key_len = sizeof(uint32_t) * 4,
		.hash_func = rte_hash_crc,
		.hash_func_init_val = 0,
	};

	*ip6_table = rte_hash_create(&table_params);
	if (*ip6_table == NULL)
		return DOCA_ERROR_DRIVER;

	return DOCA_SUCCESS;
}

doca_error_t ipsec_security_gw_parse_config(struct ipsec_security_gw_config *app_cfg)
{
	FILE *json_fp;
	size_t file_length;
	char *json_data = NULL;
	struct json_object *parsed_json;
	struct json_object *json_encrypt_rules;
	struct json_object *json_decrypt_rules;
	struct json_object *json_config;
	doca_error_t result;
	int nb_encrypt_alloc, nb_decrypt_alloc;

	result = create_ip6_table(&app_cfg->ip6_table);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create table for ipv6 addresses");
		return result;
	}

	/* set default DOCA Flow mode to vnf */
	app_cfg->flow_mode = IPSEC_SECURITY_GW_VNF;

	json_fp = fopen(app_cfg->json_path, "r");
	if (json_fp == NULL) {
		DOCA_LOG_ERR("JSON file open failed");
		return DOCA_ERROR_IO_FAILED;
	}

	result = allocate_json_buffer_dynamic(json_fp, &file_length, &json_data);
	if (result != DOCA_SUCCESS) {
		fclose(json_fp);
		DOCA_LOG_ERR("Failed to allocate data buffer for the json file");
		return result;
	}

	if (fread(json_data, 1, file_length, json_fp) < file_length)
		DOCA_LOG_DBG("EOF reached");
	fclose(json_fp);

	parsed_json = json_tokener_parse(json_data);

	if (json_object_object_get_ex(parsed_json, "config", &json_config)) {
		result = parse_json_config(json_config, app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to parse config");
			goto json_release;
		}
	}

	result = validate_config(app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to validate config");
		goto json_release;
	}

	nb_encrypt_alloc = DYN_RESERVED_RULES;
	nb_decrypt_alloc = DYN_RESERVED_RULES;
	if (!app_cfg->socket_ctx.socket_conf) {
		if (!json_object_object_get_ex(parsed_json, "encrypt-rules", &json_encrypt_rules)) {
			DOCA_LOG_ERR("Missing \"encrypt-rules\" parameter");
			result = DOCA_ERROR_INVALID_VALUE;
			goto json_release;
		}

		if (!json_object_object_get_ex(parsed_json, "decrypt-rules", &json_decrypt_rules)) {
			DOCA_LOG_ERR("Missing \"decrypt-rules\" parameter");
			result = DOCA_ERROR_INVALID_VALUE;
			goto json_release;
		}

		app_cfg->app_rules.nb_encrypt_rules = json_object_array_length(json_encrypt_rules);
		app_cfg->app_rules.nb_decrypt_rules = json_object_array_length(json_decrypt_rules);
		if ((app_cfg->app_rules.nb_encrypt_rules > MAX_NB_RULES) ||
		    (app_cfg->app_rules.nb_decrypt_rules > MAX_NB_RULES)) {
			DOCA_LOG_ERR("Number of rules exceeds the maximum number of rules allowed");
			result = DOCA_ERROR_INVALID_VALUE;
			goto json_release;
		}

		if (app_cfg->app_rules.nb_encrypt_rules == 0 && app_cfg->app_rules.nb_decrypt_rules == 0) {
			DOCA_LOG_ERR("No encrypt and decrypt rules were found");
			result = DOCA_ERROR_INVALID_VALUE;
			goto json_release;
		}

		app_cfg->app_rules.nb_rules = app_cfg->app_rules.nb_encrypt_rules + app_cfg->app_rules.nb_decrypt_rules;

		nb_encrypt_alloc = app_cfg->app_rules.nb_encrypt_rules;
		nb_decrypt_alloc = app_cfg->app_rules.nb_decrypt_rules;
	} else {
		app_cfg->app_rules.nb_encrypt_rules = nb_encrypt_alloc;
		app_cfg->app_rules.nb_decrypt_rules = nb_decrypt_alloc;
	}

	app_cfg->app_rules.encrypt_rules = (struct encrypt_rule *)calloc(nb_encrypt_alloc, sizeof(struct encrypt_rule));
	if (app_cfg->app_rules.encrypt_rules == NULL) {
		DOCA_LOG_ERR("calloc() function failed to allocate encryption rules array");
		result = DOCA_ERROR_NO_MEMORY;
		goto json_release;
	}

	app_cfg->app_rules.decrypt_rules = (struct decrypt_rule *)calloc(nb_decrypt_alloc, sizeof(struct decrypt_rule));
	if (app_cfg->app_rules.decrypt_rules == NULL) {
		DOCA_LOG_ERR("calloc() function failed to allocate decryption rules array");
		result = DOCA_ERROR_NO_MEMORY;
		goto encrypt_release;
	}

	/* parse the rules and insert to the allocated arrays */
	if (!app_cfg->socket_ctx.socket_conf) {
		result = parse_json_encrypt_rules(json_encrypt_rules, app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to parse encrypt rules");
			goto dec_enc_release;
		}

		result = parse_json_decrypt_rules(json_decrypt_rules, app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to parse decrypt rules");
			goto dec_enc_release;
		}
	}
	json_object_put(parsed_json);
	free(json_data);
	return DOCA_SUCCESS;
dec_enc_release:
	free(app_cfg->app_rules.decrypt_rules);
encrypt_release:
	free(app_cfg->app_rules.encrypt_rules);
json_release:
	json_object_put(parsed_json);
	free(json_data);
	return result;
}

/*
 * Parse the input device and set the relevant fields in the struct
 *
 * @param [in]: Input parameter
 * @dev_info [out]: device info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_device_param(void *param, struct ipsec_security_gw_dev_info *dev_info)
{
	struct doca_argp_device_ctx *dev_ctx = (struct doca_argp_device_ctx *)param;

	if (dev_ctx->devargs != NULL) {
		DOCA_LOG_WARN("Passed device args are not needed, and will be overriden by the application");
		/* Fallthrough */
	}

	dev_info->doca_dev = dev_ctx->dev;

	return DOCA_SUCCESS;
}

/*
 * Parse the input representor device and set the relevant fields in the struct
 *
 * @param [in]: Input parameter
 * @dev_info [out]: device info struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_device_rep_param(void *param, struct ipsec_security_gw_dev_info *dev_info)
{
	struct doca_argp_device_rep_ctx *dev_rep_ctx = (struct doca_argp_device_rep_ctx *)param;

	if (dev_info->doca_dev_rep != NULL) {
		DOCA_LOG_ERR("More than a single representor was passed, yet only one representor is supported");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (dev_rep_ctx->dev_ctx.devargs != NULL) {
		DOCA_LOG_WARN(
			"Passed representor device args are not needed, and will be overriden by the application");
		/* Fallthrough */
	}

	dev_info->doca_dev = dev_rep_ctx->dev_ctx.dev;
	dev_info->doca_dev_rep = dev_rep_ctx->dev_rep;
	dev_info->is_representor = true;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DOCA device for secured port
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t secured_dev_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;

	return parse_device_param(param, &app_cfg->objects.secured_dev);
}

/*
 * ARGP Callback - Handle DOCA device for unsecured port
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t unsecured_dev_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;

	return parse_device_param(param, &app_cfg->objects.unsecured_dev);
}

/*
 * ARGP Callback - Handle DOCA device representor for unsecured port
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t unsecured_dev_rep_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;

	return parse_device_rep_param(param, &app_cfg->objects.unsecured_dev);
}

/*
 * ARGP Callback - Handle number of cores parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nb_cores_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;

	app_cfg->nb_cores = *(uint8_t *)param;
	if (app_cfg->nb_cores > MAX_CORES)
		DOCA_LOG_WARN(
			"The application's memory consumption depends on the amount of used cores, a value above %d might lead to unstable results",
			MAX_CORES);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle debug mode parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t debug_mode_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;

	app_cfg->debug_mode = *(bool *)param;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle rules file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t config_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;
	const char *json_path = (char *)param;

	if (strnlen(json_path, MAX_FILE_NAME) == MAX_FILE_NAME) {
		DOCA_LOG_ERR("JSON file name is too long - MAX=%d", MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (access(json_path, F_OK) == -1) {
		DOCA_LOG_ERR("JSON file was not found %s", json_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	strlcpy(app_cfg->json_path, json_path, MAX_FILE_NAME);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle application offload mode
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t offload_mode_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;
	const char *mode = (char *)param;

	if (strcmp(mode, "tunnel") == 0)
		app_cfg->mode = IPSEC_SECURITY_GW_TUNNEL;
	else if (strcmp(mode, "transport") == 0)
		app_cfg->mode = IPSEC_SECURITY_GW_TRANSPORT;
	else if (strcmp(mode, "udp_transport") == 0)
		app_cfg->mode = IPSEC_SECURITY_GW_UDP_TRANSPORT;
	else {
		DOCA_LOG_ERR("Illegal running mode = [%s]", mode);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle application socket mode
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t socket_callback(void *param, void *config)
{
	struct ipsec_security_gw_config *app_cfg = (struct ipsec_security_gw_config *)config;
	char *file_path = (char *)param;

	if (strnlen(file_path, MAX_SOCKET_PATH_NAME) == MAX_SOCKET_PATH_NAME) {
		DOCA_LOG_ERR("File name is too long - MAX=%d", MAX_SOCKET_PATH_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->socket_ctx.socket_path, file_path, MAX_SOCKET_PATH_NAME);
	app_cfg->socket_ctx.socket_conf = true;
	return DOCA_SUCCESS;
}

doca_error_t register_ipsec_security_gw_params(void)
{
	doca_error_t result;
	struct doca_argp_param *secured_param, *unsecured_param, *config_param, *ipsec_mode, *socket_path,
		*unsecured_rep_param, *nb_cores, *debug_mode;

	/* Create and register ingress device param */
	result = doca_argp_param_create(&secured_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(secured_param, "s");
	doca_argp_param_set_long_name(secured_param, "secured");
	doca_argp_param_set_description(secured_param, "secured port device identifier");
	doca_argp_param_set_callback(secured_param, secured_dev_callback);
	doca_argp_param_set_type(secured_param, DOCA_ARGP_TYPE_DEVICE);
	doca_argp_param_set_mandatory(secured_param);
	result = doca_argp_register_param(secured_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register egress device param */
	result = doca_argp_param_create(&unsecured_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(unsecured_param, "u");
	doca_argp_param_set_long_name(unsecured_param, "unsecured");
	doca_argp_param_set_description(unsecured_param, "unsecured port device identifier");
	doca_argp_param_set_callback(unsecured_param, unsecured_dev_callback);
	doca_argp_param_set_type(unsecured_param, DOCA_ARGP_TYPE_DEVICE);
	result = doca_argp_register_param(unsecured_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register json rules param */
	result = doca_argp_param_create(&config_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(config_param, "c");
	doca_argp_param_set_long_name(config_param, "config");
	doca_argp_param_set_description(config_param, "Path to the JSON file with application configuration");
	doca_argp_param_set_callback(config_param, config_callback);
	doca_argp_param_set_type(config_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(config_param);
	result = doca_argp_register_param(config_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register offload mode param */
	result = doca_argp_param_create(&ipsec_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(ipsec_mode, "m");
	doca_argp_param_set_long_name(ipsec_mode, "mode");
	doca_argp_param_set_description(ipsec_mode, "ipsec mode - {tunnel/transport/udp_transport}");
	doca_argp_param_set_callback(ipsec_mode, offload_mode_callback);
	doca_argp_param_set_type(ipsec_mode, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(ipsec_mode);
	result = doca_argp_register_param(ipsec_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&socket_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(socket_path, "i");
	doca_argp_param_set_long_name(socket_path, "ipc");
	doca_argp_param_set_description(socket_path, "IPC socket file path");
	doca_argp_param_set_callback(socket_path, socket_callback);
	doca_argp_param_set_type(socket_path, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(socket_path);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register unsecured device representor param */
	result = doca_argp_param_create(&unsecured_rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(unsecured_rep_param, "ur");
	doca_argp_param_set_long_name(unsecured_rep_param, "unsecured-rep");
	doca_argp_param_set_description(unsecured_rep_param, "unsecured port representor device identifier");
	doca_argp_param_set_callback(unsecured_rep_param, unsecured_dev_rep_callback);
	doca_argp_param_set_type(unsecured_rep_param, DOCA_ARGP_TYPE_DEVICE_REP);
	result = doca_argp_register_param(unsecured_rep_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register unsecured device name param */
	result = doca_argp_param_create(&nb_cores);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(nb_cores, "n");
	doca_argp_param_set_long_name(nb_cores, "nb-cores");
	doca_argp_param_set_description(nb_cores, "number of cores");
	doca_argp_param_set_callback(nb_cores, nb_cores_callback);
	doca_argp_param_set_type(nb_cores, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(nb_cores);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register debug mode param */
	result = doca_argp_param_create(&debug_mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(debug_mode, "debug");
	doca_argp_param_set_description(debug_mode, "Enable debug counters");
	doca_argp_param_set_callback(debug_mode, debug_mode_callback);
	doca_argp_param_set_type(debug_mode, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(debug_mode);
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

	return result;
}
