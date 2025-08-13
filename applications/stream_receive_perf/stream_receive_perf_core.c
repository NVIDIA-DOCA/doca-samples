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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_ctx.h>
#include <doca_log.h>
#include <utils.h>
#include <samples/common.h>

DOCA_LOG_REGISTER(STREAM_RECEIVE_PERF_CORE);

#include "stream_receive_perf_core.h"

/*
 * Handles the completion event for the received stream data.
 * Invoked when the data is successfully received by the DOCA stream.
 * It updates statistics such as the number of received packets and bytes, optionally
 * dumps the received data in a readable format, and logs detailed information about
 * the completion event. The data is processed buffer-by-buffer and printed (if enabled)
 *
 * @event_rx_data [in]: Pointer to the received event data
 * @event_user_data [in]: User data associated with the event
 */
static void handle_completion(struct doca_rmax_in_stream_event_rx_data *event_rx_data, union doca_data event_user_data);

/*
 * Handles error events during stream data reception.
 * Captures and logs detailed information about any errors encountered
 * during stream data handling, including an error code and message. It ensures that
 * the application is aware of issues and halts the receive loop to prevent further
 * processing in erroneous scenarios
 *
 * @event_rx_data [in]: Pointer to the received event data
 * @event_user_data [in]: User data associated with the event
 */
static void handle_error(struct doca_rmax_in_stream_event_rx_data *event_rx_data, union doca_data event_user_data);

bool init_config(struct app_config *config)
{
	doca_error_t ret;

	config->list = false;
	config->dump = false;
	config->scatter_type = SCATTER_TYPE_RAW;
	config->tstamp_format = TIMESTAMP_FORMAT_RAW_COUNTER;
	config->src_ip.s_addr = 0;
	config->dst_ip.s_addr = 0;
	config->dev_ip.s_addr = 0;
	config->dst_port = 0;
	config->data_size = 1500;
	config->hdr_size = 0;
	config->num_elements = 262144;
	config->sleep_us = 0;
	config->min_packets = 0;
	config->max_packets = 0;
	config->affinity_mask_set = false;
	ret = doca_rmax_cpu_affinity_create(&config->affinity_mask);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create affinity mask: %s", doca_error_get_name(ret));
		return false;
	}
	return true;
}

void destroy_config(struct app_config *config)
{
	doca_error_t ret;

	ret = doca_rmax_cpu_affinity_destroy(config->affinity_mask);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy affinity mask: %s", doca_error_get_name(ret));
}

/*
 * Sets the list flag in the application configuration
 *
 * @param [in]: Unused parameter
 *
 * @opaque [in]: Pointer to the application configuration
 *
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t set_list_flag(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;

	(void)param;
	config->list = true;

	return DOCA_SUCCESS;
}

/*
 * Sets the scatter type parameter in the application configuration
 *
 * @param [in]: Pointer to the scatter type string
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the scatter type is invalid
 */
static doca_error_t set_scatter_type_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const char *str = (const char *)param;

	if (strcasecmp(str, "RAW") == 0)
		config->scatter_type = SCATTER_TYPE_RAW;
	else if (strcasecmp(str, "ULP") == 0)
		config->scatter_type = SCATTER_TYPE_ULP;
	else {
		DOCA_LOG_ERR("unknown scatter type '%s' was specified", str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the timestamp format parameter in the application configuration
 *
 * @param [in]: Pointer to the timestamp format string
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the timestamp format is invalid
 */
static doca_error_t set_tstamp_format_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const char *str = (const char *)param;

	if (strcasecmp(str, "raw") == 0)
		config->tstamp_format = TIMESTAMP_FORMAT_RAW_COUNTER;
	else if (strcasecmp(str, "free-running") == 0)
		config->tstamp_format = TIMESTAMP_FORMAT_FREE_RUNNING;
	else if (strcasecmp(str, "synced") == 0)
		config->tstamp_format = TIMESTAMP_FORMAT_PTP_SYNCED;
	else {
		DOCA_LOG_ERR("unknown timestamp format '%s' was specified", str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Parses and sets an IPv4 address parameter within the application configuration.
 * Validates the provided IP string format (e.g., `192.168.0.1`).
 *
 * @label [in]: Label describing the IP address type
 * @str [in]: Pointer to the IP address string
 * @out [out]: Pointer to the in_addr structure to store the parsed IP address
 * @return: DOCA_SUCCESS on success, or an error code if the IP address format is invalid
 */
static doca_error_t set_ip_param(const char *label, const char *str, struct in_addr *out)
{
	unsigned int ip[4];
	union {
		uint8_t octet[4];
		uint32_t addr;
	} addr;
	char dummy;

	if (sscanf(str, "%u.%u.%u.%u%c", &ip[0], &ip[1], &ip[2], &ip[3], &dummy) == 4 && ip[0] < 256 && ip[1] < 256 &&
	    ip[2] < 256 && ip[3] < 256) {
		addr.octet[0] = ip[0];
		addr.octet[1] = ip[1];
		addr.octet[2] = ip[2];
		addr.octet[3] = ip[3];
		out->s_addr = addr.addr;
	} else {
		DOCA_LOG_ERR("bad %s IP address format '%s'", label, str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the source IP address parameter in the application configuration
 *
 * @param [in]: Pointer to the IP address string
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the IP address format is invalid
 */
static doca_error_t set_src_ip_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const char *str = (const char *)param;

	return set_ip_param("source", str, &config->src_ip);
}

/*
 * Sets the destination IP address parameter in the application configuration
 *
 * @param [in]: Pointer to the IP address string
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the IP address format is invalid
 */
static doca_error_t set_dst_ip_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const char *str = (const char *)param;

	return set_ip_param("destination", str, &config->dst_ip);
}

/*
 * Sets the device IP address parameter in the application configuration
 *
 * @param [in]: Pointer to the IP address string
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the IP address format is invalid
 */
static doca_error_t set_dev_ip_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const char *str = (const char *)param;

	return set_ip_param("local interface", str, &config->dev_ip);
}

/*
 * Sets the destination port parameter in the application configuration
 *
 * @param [in]: Pointer to the port number
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the port number is invalid
 */
static doca_error_t set_dst_port_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value > 0 && value <= UINT16_MAX)
		config->dst_port = (uint16_t)value;
	else {
		DOCA_LOG_ERR("bad source port '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the data size parameter in the application configuration
 *
 * @param [in]: Pointer to the data size
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the data size is invalid
 */
static doca_error_t set_data_size_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value >= 0 && value <= UINT16_MAX)
		config->data_size = (uint16_t)value;
	else {
		DOCA_LOG_ERR("bad data size '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the header size parameter in the application configuration
 *
 * @param [in]: Pointer to the header size
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the header size is invalid
 */
static doca_error_t set_hdr_size_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value >= 0 && value <= UINT16_MAX)
		config->hdr_size = (uint16_t)value;
	else {
		DOCA_LOG_ERR("bad header size '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the number of elements parameter in the application configuration
 *
 * @param [in]: Pointer to the number of elements
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the number of elements is invalid
 */
static doca_error_t set_num_elements_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value > 0 && value <= UINT32_MAX)
		config->num_elements = (uint32_t)value;
	else {
		DOCA_LOG_ERR("bad number of elements '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the CPU affinity parameter in the application configuration.
 * Parses a string of CPU core indices (e.g., "0,1,2") to bind the application
 * to specific cores, optimizing resource usage and performance.
 *
 * @param [in]: Pointer to the CPU affinity string
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the CPU affinity is invalid
 */
static doca_error_t set_cpu_affinity_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const char *input = (const char *)param;
	char *str, *alloc;
	doca_error_t ret = DOCA_SUCCESS;

	alloc = str = strdup(input);
	if (str == NULL) {
		DOCA_LOG_ERR("unable to allocate memory: %s", strerror(errno));
		return DOCA_ERROR_NO_MEMORY;
	}

	while ((str = strtok(str, ",")) != NULL) {
		int idx;
		char dummy;

		if (sscanf(str, "%d%c", &idx, &dummy) != 1) {
			DOCA_LOG_ERR("bad CPU index '%s' was specified", str);
			ret = DOCA_ERROR_INVALID_VALUE;
			goto exit;
		}

		ret = doca_rmax_cpu_affinity_set(config->affinity_mask, idx);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("error setting CPU index '%d' in affinity mask", idx);
			goto exit;
		}

		str = NULL;
	}

	config->affinity_mask_set = true;
exit:
	free(alloc);

	return ret;
}

/*
 * Sets the sleep duration parameter in the application configuration
 *
 * @param [in]: Pointer to the sleep duration
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the sleep duration is invalid
 */
static doca_error_t set_sleep_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value > 0)
		config->sleep_us = value;
	else {
		DOCA_LOG_ERR("bad sleep duration '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the minimum packets number parameter in the application configuration
 *
 * @param [in]: Pointer to the minimum packets
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the minimum packets is invalid
 */
static doca_error_t set_min_packets_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value >= 0 && value <= UINT32_MAX)
		config->min_packets = (uint32_t)value;
	else {
		DOCA_LOG_ERR("bad minimum packets count '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the maximum packets number parameter in the application configuration
 *
 * @param [in]: Pointer to the maximum packets
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success, or an error code if the maximum packets is invalid
 */
static doca_error_t set_max_packets_param(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;
	const int value = *(const int *)param;

	if (value > 0 && value <= UINT32_MAX)
		config->max_packets = (uint32_t)value;
	else {
		DOCA_LOG_ERR("bad maximum packets count '%d' was specified", value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * Sets the dump flag in the application configuration.
 * Enables the option to dump packet content for debugging or analysis purposes.
 * If the flag is set, the application will provide a content dump of all received packets
 * during runtime.
 *
 * @param [in]: Unused parameter
 * @opaque [in]: Pointer to the application configuration
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t set_dump_flag(void *param, void *opaque)
{
	struct app_config *config = (struct app_config *)opaque;

	(void)param;
	config->dump = true;

	return DOCA_SUCCESS;
}

bool register_argp_params(void)
{
	doca_error_t ret;
	struct doca_argp_param *list_flag;
	struct doca_argp_param *scatter_type_param;
	struct doca_argp_param *tstamp_format_param;
	struct doca_argp_param *dev_ip_param;
	struct doca_argp_param *dst_ip_param;
	struct doca_argp_param *src_ip_param;
	struct doca_argp_param *dst_port_param;
	struct doca_argp_param *hdr_size_param;
	struct doca_argp_param *data_size_param;
	struct doca_argp_param *num_elements_param;
	struct doca_argp_param *cpu_affinity_param;
	struct doca_argp_param *min_packets_param;
	struct doca_argp_param *max_packets_param;
	struct doca_argp_param *sleep_param;
	struct doca_argp_param *dump_flag;

	/* --list flag */
	ret = doca_argp_param_create(&list_flag);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(list_flag, "list");
	doca_argp_param_set_description(list_flag, "List available devices");
	doca_argp_param_set_callback(list_flag, set_list_flag);
	doca_argp_param_set_type(list_flag, DOCA_ARGP_TYPE_BOOLEAN);
	ret = doca_argp_register_param(list_flag);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* --scatter-type parameter */
	ret = doca_argp_param_create(&scatter_type_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(scatter_type_param, "scatter-type");
	doca_argp_param_set_description(scatter_type_param, "Scattering type: RAW (default) or ULP");
	doca_argp_param_set_callback(scatter_type_param, set_scatter_type_param);
	doca_argp_param_set_type(scatter_type_param, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(scatter_type_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* --tstamp-format parameter */
	ret = doca_argp_param_create(&tstamp_format_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(tstamp_format_param, "tstamp-format");
	doca_argp_param_set_description(tstamp_format_param, "Timestamp format: raw (default), free-running or synced");
	doca_argp_param_set_callback(tstamp_format_param, set_tstamp_format_param);
	doca_argp_param_set_type(tstamp_format_param, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(tstamp_format_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -s,--src-ip parameter */
	ret = doca_argp_param_create(&src_ip_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(src_ip_param, "s");
	doca_argp_param_set_long_name(src_ip_param, "src-ip");
	doca_argp_param_set_description(src_ip_param, "Source address to read from");
	doca_argp_param_set_callback(src_ip_param, set_src_ip_param);
	doca_argp_param_set_type(src_ip_param, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(src_ip_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -d,--dst-ip parameter */
	ret = doca_argp_param_create(&dst_ip_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(dst_ip_param, "d");
	doca_argp_param_set_long_name(dst_ip_param, "dst-ip");
	doca_argp_param_set_description(dst_ip_param, "Destination address to bind to");
	doca_argp_param_set_callback(dst_ip_param, set_dst_ip_param);
	doca_argp_param_set_type(dst_ip_param, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(dst_ip_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -i,--local-ip parameter */
	ret = doca_argp_param_create(&dev_ip_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(dev_ip_param, "i");
	doca_argp_param_set_long_name(dev_ip_param, "local-ip");
	doca_argp_param_set_description(dev_ip_param, "IP of the local interface to receive data");
	doca_argp_param_set_callback(dev_ip_param, set_dev_ip_param);
	doca_argp_param_set_type(dev_ip_param, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(dev_ip_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -p,--dst-port parameter */
	ret = doca_argp_param_create(&dst_port_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(dst_port_param, "p");
	doca_argp_param_set_long_name(dst_port_param, "dst-port");
	doca_argp_param_set_description(dst_port_param, "Destination port to read from");
	doca_argp_param_set_callback(dst_port_param, set_dst_port_param);
	doca_argp_param_set_type(dst_port_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(dst_port_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -K,--packets parameter */
	ret = doca_argp_param_create(&num_elements_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(num_elements_param, "K");
	doca_argp_param_set_long_name(num_elements_param, "packets");
	doca_argp_param_set_description(num_elements_param,
					"Number of packets to allocate memory for (default 262144)");
	doca_argp_param_set_callback(num_elements_param, set_num_elements_param);
	doca_argp_param_set_type(num_elements_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(num_elements_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -y,--payload-size parameter */
	ret = doca_argp_param_create(&data_size_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(data_size_param, "y");
	doca_argp_param_set_long_name(data_size_param, "payload-size");
	doca_argp_param_set_description(data_size_param, "Packet's payload size (default 1500)");
	doca_argp_param_set_callback(data_size_param, set_data_size_param);
	doca_argp_param_set_type(data_size_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(data_size_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -e,--app-hdr-size parameter */
	ret = doca_argp_param_create(&hdr_size_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(hdr_size_param, "e");
	doca_argp_param_set_long_name(hdr_size_param, "app-hdr-size");
	doca_argp_param_set_description(hdr_size_param, "Packet's application header size (default 0)");
	doca_argp_param_set_callback(hdr_size_param, set_hdr_size_param);
	doca_argp_param_set_type(hdr_size_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(hdr_size_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* -a,--cpu-affinity parameter */
	ret = doca_argp_param_create(&cpu_affinity_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_short_name(cpu_affinity_param, "a");
	doca_argp_param_set_long_name(cpu_affinity_param, "cpu-affinity");
	doca_argp_param_set_description(cpu_affinity_param,
					"Comma separated list of CPU affinity cores for the application main thread");
	doca_argp_param_set_callback(cpu_affinity_param, set_cpu_affinity_param);
	doca_argp_param_set_type(cpu_affinity_param, DOCA_ARGP_TYPE_STRING);
	ret = doca_argp_register_param(cpu_affinity_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* --sleep parameter */
	ret = doca_argp_param_create(&sleep_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(sleep_param, "sleep");
	doca_argp_param_set_description(sleep_param, "Amount of microseconds to sleep between requests (default 0)");
	doca_argp_param_set_callback(sleep_param, set_sleep_param);
	doca_argp_param_set_type(sleep_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(sleep_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* --min parameter */
	ret = doca_argp_param_create(&min_packets_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(min_packets_param, "min");
	doca_argp_param_set_description(min_packets_param,
					"Block until at least this number of packets are received (default 0)");
	doca_argp_param_set_callback(min_packets_param, set_min_packets_param);
	doca_argp_param_set_type(min_packets_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(min_packets_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* --max parameter */
	ret = doca_argp_param_create(&max_packets_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(max_packets_param, "max");
	doca_argp_param_set_description(max_packets_param, "Maximum number of packets to return in one completion");
	doca_argp_param_set_callback(max_packets_param, set_max_packets_param);
	doca_argp_param_set_type(max_packets_param, DOCA_ARGP_TYPE_INT);
	ret = doca_argp_register_param(max_packets_param);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* --dump flag */
	ret = doca_argp_param_create(&dump_flag);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_name(ret));
		return false;
	}
	doca_argp_param_set_long_name(dump_flag, "dump");
	doca_argp_param_set_description(dump_flag, "Dump packet content");
	doca_argp_param_set_callback(dump_flag, set_dump_flag);
	doca_argp_param_set_type(dump_flag, DOCA_ARGP_TYPE_BOOLEAN);
	ret = doca_argp_register_param(dump_flag);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_name(ret));
		return false;
	}

	/* version callback */
	ret = doca_argp_register_version_callback(sdk_version_callback);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register version callback: %s", doca_error_get_name(ret));
		return false;
	}

	return true;
}

bool mandatory_args_set(struct app_config *config)
{
	bool status = true;

	if (config->dev_ip.s_addr == 0) {
		DOCA_LOG_ERR("Local interface IP is not set");
		status = false;
	}
	if (config->dst_ip.s_addr == 0) {
		DOCA_LOG_ERR("Destination multicast IP is not set");
		status = false;
	}
	if (config->src_ip.s_addr == 0) {
		DOCA_LOG_ERR("Source IP is not set");
		status = false;
	}
	if (config->dst_port == 0) {
		DOCA_LOG_ERR("Destination port is not set");
		status = false;
	}
	return status;
}

void list_devices(void)
{
	struct doca_devinfo **devinfo;
	uint32_t nb_devs;
	doca_error_t ret;

	ret = doca_devinfo_create_list(&devinfo, &nb_devs);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enumerate devices: %s", doca_error_get_name(ret));
		return;
	}
	DOCA_LOG_INFO("Iface\t\tIB dev\t\tBus ID\tIP addr\t\tPTP\n");
	for (uint32_t i = 0; i < nb_devs; ++i) {
		char dev_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];
		char netdev[DOCA_DEVINFO_IFACE_NAME_SIZE];
		char ibdev[DOCA_DEVINFO_IBDEV_NAME_SIZE];
		uint8_t addr[4];
		bool has_ptp = false;

		/* get network interface name */
		ret = doca_devinfo_get_iface_name(devinfo[i], netdev, sizeof(netdev));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to get interface name for device %d: %s", i, doca_error_get_name(ret));
			continue;
		}
		/* get Infiniband device name */
		ret = doca_devinfo_get_ibdev_name(devinfo[i], ibdev, sizeof(ibdev));
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to get Infiniband name for device %d: %s", i, doca_error_get_name(ret));
			continue;
		}
		/* get PCI address */
		ret = doca_devinfo_get_pci_addr_str(devinfo[i], dev_pci_addr);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to get PCI address for device %d: %s", i, doca_error_get_name(ret));
			continue;
		}
		/* get IP address */
		ret = doca_devinfo_get_ipv4_addr(devinfo[i], (uint8_t *)&addr, sizeof(addr));
		if (ret == DOCA_SUCCESS) {
			/* query PTP capability */
			ret = doca_rmax_get_ptp_clock_supported(devinfo[i]);
			switch (ret) {
			case DOCA_SUCCESS:
				has_ptp = true;
				break;
			case DOCA_ERROR_NOT_SUPPORTED:
				has_ptp = false;
				break;
			default: {
				DOCA_LOG_WARN("Failed to query PTP capability for device %d: %s",
					      i,
					      doca_error_get_name(ret));
				continue;
			}
			}
		} else {
			if (ret != DOCA_ERROR_NOT_FOUND)
				DOCA_LOG_WARN("Failed to query IP address for device %d: %s",
					      i,
					      doca_error_get_name(ret));
		}

		DOCA_LOG_INFO("%-8s\t%-8s\t%-8s\t%03d.%03d.%03d.%03d\t%c\n",
			      netdev,
			      ibdev,
			      dev_pci_addr,
			      addr[0],
			      addr[1],
			      addr[2],
			      addr[3],
			      (has_ptp) ? 'y' : 'n');
	}
	ret = doca_devinfo_destroy_list(devinfo);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to clean up devices list: %s", doca_error_get_name(ret));
}

struct doca_dev *open_device(struct in_addr *dev_ip)
{
	struct doca_devinfo **devinfo;
	struct doca_devinfo *found_devinfo = NULL;
	uint32_t nb_devs;
	doca_error_t ret;
	struct in_addr addr;
	struct doca_dev *dev = NULL;

	ret = doca_devinfo_create_list(&devinfo, &nb_devs);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enumerate devices: %s", doca_error_get_name(ret));
		return NULL;
	}
	for (uint32_t i = 0; i < nb_devs; ++i) {
		ret = doca_devinfo_get_ipv4_addr(devinfo[i], (uint8_t *)&addr, sizeof(addr));
		if (ret != DOCA_SUCCESS)
			continue;
		if (addr.s_addr != dev_ip->s_addr)
			continue;
		found_devinfo = devinfo[i];
		break;
	}
	if (found_devinfo) {
		ret = doca_dev_open(found_devinfo, &dev);
		if (ret != DOCA_SUCCESS)
			DOCA_LOG_WARN("Error opening network device: %s", doca_error_get_name(ret));
	} else
		DOCA_LOG_ERR("Device not found");

	ret = doca_devinfo_destroy_list(devinfo);
	if (ret != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to clean up devices list: %s", doca_error_get_name(ret));

	return dev;
}

/*
 * Frees allocated memory with the specified callback
 *
 * @addr [in]: Pointer to the allocated memory
 * @len [in]: Length of the allocated memory
 * @opaque [in]: Unused parameter
 */
static void free_callback(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;
	free(addr);
}

doca_error_t init_globals(struct app_config *config, struct doca_dev *dev, struct globals *globals)
{
	doca_error_t ret;
	size_t num_buffers = (config->hdr_size > 0) ? 2 : 1;

	/* create memory-related DOCA objects */
	ret = doca_mmap_create(&globals->mmap);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error creating mmap: %s", doca_error_get_name(ret));
		return ret;
	}
	ret = doca_mmap_add_dev(globals->mmap, dev);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error adding device to mmap: %s", doca_error_get_name(ret));
		return ret;
	}
	/* set mmap free callback */
	ret = doca_mmap_set_free_cb(globals->mmap, free_callback, NULL);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap free callback: %s", doca_error_get_name(ret));
		return ret;
	}
	ret = doca_buf_inventory_create(num_buffers, &globals->inventory);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error creating inventory: %s", doca_error_get_name(ret));
		return ret;
	}
	ret = doca_buf_inventory_start(globals->inventory);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error starting inventory: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_pe_create(&globals->pe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error creating progress engine: %s", doca_error_get_name(ret));
		return ret;
	}

	return DOCA_SUCCESS;
}

bool destroy_globals(struct globals *globals, struct doca_dev *dev)
{
	doca_error_t ret;
	bool is_ok = true;

	ret = doca_pe_destroy(globals->pe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error destroying progress engine: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	ret = doca_buf_inventory_stop(globals->inventory);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error stopping inventory: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	ret = doca_buf_inventory_destroy(globals->inventory);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error destroying inventory: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	/* will also free all allocated memory via callback */
	ret = doca_mmap_destroy(globals->mmap);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error destroying mmap: %s", doca_error_get_name(ret));
		is_ok = false;
	}

	return is_ok;
}

doca_error_t init_stream(struct app_config *config,
			 struct doca_dev *dev,
			 struct globals *globals,
			 struct stream_data *data)
{
	static const size_t page_size = 4096;
	doca_error_t ret;
	doca_error_t err;
	size_t num_buffers;
	size_t size[MAX_BUFFERS];
	void *ptr[MAX_BUFFERS];
	union doca_data event_user_data;
	char *ptr_memory = NULL;

	memset(&size, 0, sizeof(size));

	/* create stream object */
	ret = doca_rmax_in_stream_create(dev, &data->stream);
	if (ret != DOCA_SUCCESS)
		return ret;

	/* Register Rx data event handlers */
	event_user_data.ptr = (void *)data;
	ret = doca_rmax_in_stream_event_rx_data_register(data->stream, event_user_data, handle_completion, handle_error);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	/* fill stream parameters */
	switch (config->scatter_type) {
	case SCATTER_TYPE_RAW:
		ret = doca_rmax_in_stream_set_scatter_type_raw(data->stream);
		break;
	case SCATTER_TYPE_ULP:
		ret = doca_rmax_in_stream_set_scatter_type_ulp(data->stream);
		break;
	}
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	switch (config->tstamp_format) {
	case TIMESTAMP_FORMAT_RAW_COUNTER:
		ret = doca_rmax_in_stream_set_timestamp_format_raw_counter(data->stream);
		break;
	case TIMESTAMP_FORMAT_FREE_RUNNING:
		ret = doca_rmax_in_stream_set_timestamp_format_free_running(data->stream);
		break;
	case TIMESTAMP_FORMAT_PTP_SYNCED:
		ret = doca_rmax_in_stream_set_timestamp_format_ptp_synced(data->stream);
		break;
	}
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	ret = doca_rmax_in_stream_set_elements_count(data->stream, config->num_elements);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	ret = doca_rmax_in_stream_set_min_packets(data->stream, config->min_packets);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	if (config->max_packets > 0) {
		ret = doca_rmax_in_stream_set_max_packets(data->stream, config->max_packets);
		if (ret != DOCA_SUCCESS)
			goto destroy_stream;
	}

	if (config->hdr_size == 0) {
		num_buffers = 1;
		data->pkt_size[0] = config->data_size;
	} else {
		/* Header-Data Split mode */
		num_buffers = 2;
		data->pkt_size[0] = config->hdr_size;
		data->pkt_size[1] = config->data_size;
	}

	data->num_buffers = num_buffers;
	ret = doca_rmax_in_stream_set_memblks_count(data->stream, num_buffers);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	ret = doca_rmax_in_stream_memblk_desc_set_min_size(data->stream, data->pkt_size);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	ret = doca_rmax_in_stream_memblk_desc_set_max_size(data->stream, data->pkt_size);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;

	/* query buffer size */
	ret = doca_rmax_in_stream_get_memblk_size(data->stream, size);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;
	/* query stride size */
	ret = doca_rmax_in_stream_get_memblk_stride_size(data->stream, data->stride_size);
	if (ret != DOCA_SUCCESS)
		goto destroy_stream;

	/* allocate memory */
	ptr_memory = aligned_alloc(page_size, size[0] + size[1]);
	if (ptr_memory == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory size: %zu", size[0] + size[1]);
		ret = DOCA_ERROR_NO_MEMORY;
		goto destroy_stream;
	}

	ret = doca_mmap_set_memrange(globals->mmap, ptr_memory, size[0] + size[1]);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range, %p, size %zu: %s",
			     ptr_memory,
			     size[0] + size[1],
			     doca_error_get_name(ret));
		free(ptr_memory);
		goto destroy_stream;
	}

	/* start mmap */
	ret = doca_mmap_start(globals->mmap);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error starting mmap: %s", doca_error_get_name(ret));
		free(ptr_memory);
		goto destroy_stream;
	}

	if (num_buffers == 1) {
		ptr[0] = ptr_memory;
	} else {
		ptr[0] = ptr_memory;	       /* header */
		ptr[1] = ptr_memory + size[0]; /* data */
	}

	/* build memory buffer chain */
	for (size_t i = 0; i < num_buffers; ++i) {
		struct doca_buf *buf;

		if (ptr[i] == NULL) {
			ret = DOCA_ERROR_NO_MEMORY;
			if (i > 0)
				goto destroy_buffers;
			goto destroy_stream;
		}
		ret = doca_buf_inventory_buf_get_by_addr(globals->inventory, globals->mmap, ptr[i], size[i], &buf);
		if (ret != DOCA_SUCCESS) {
			if (i > 0)
				goto destroy_buffers;
			goto destroy_stream;
		}
		if (i == 0)
			data->buffer = buf;
		else {
			/* chain buffers */
			ret = doca_buf_chain_list(data->buffer, buf);
			if (ret != DOCA_SUCCESS)
				goto destroy_buffers;
		}
	}
	/* set memory buffer(s) */
	ret = doca_rmax_in_stream_set_memblk(data->stream, data->buffer);
	if (ret != DOCA_SUCCESS)
		goto destroy_buffers;

	/* connect to progress engine */
	ret = doca_pe_connect_ctx(globals->pe, doca_rmax_in_stream_as_ctx(data->stream));
	if (ret != DOCA_SUCCESS)
		goto destroy_buffers;

	/* start stream */
	ret = doca_ctx_start(doca_rmax_in_stream_as_ctx(data->stream));
	if (ret != DOCA_SUCCESS)
		goto destroy_buffers;

	/* attach a flow */
	ret = doca_rmax_flow_create(&data->flow);
	if (ret != DOCA_SUCCESS)
		goto stop_stream;
	ret = doca_rmax_flow_set_src_ip(data->flow, &config->src_ip);
	if (ret != DOCA_SUCCESS)
		goto destroy_flow;
	ret = doca_rmax_flow_set_dst_ip(data->flow, &config->dst_ip);
	if (ret != DOCA_SUCCESS)
		goto destroy_flow;
	ret = doca_rmax_flow_set_dst_port(data->flow, config->dst_port);
	if (ret != DOCA_SUCCESS)
		goto destroy_flow;
	ret = doca_rmax_flow_attach(data->flow, data->stream);
	if (ret != DOCA_SUCCESS)
		goto destroy_flow;

	data->recv_pkts = 0;
	data->recv_bytes = 0;
	data->dump = config->dump;

	return DOCA_SUCCESS;
destroy_flow:
	err = doca_rmax_flow_destroy(data->flow);
	if (err != DOCA_SUCCESS)
		DOCA_LOG_WARN("Error destroying flow: %s", doca_error_get_name(err));
stop_stream:
	err = doca_ctx_stop(doca_rmax_in_stream_as_ctx(data->stream));
	if (err != DOCA_SUCCESS)
		DOCA_LOG_WARN("Error stopping context: %s", doca_error_get_name(err));
destroy_buffers:
	err = doca_buf_dec_refcount(data->buffer, NULL);
	if (err != DOCA_SUCCESS)
		DOCA_LOG_WARN("Error removing buffers: %s", doca_error_get_name(err));
destroy_stream:
	err = doca_rmax_in_stream_destroy(data->stream);
	if (err != DOCA_SUCCESS)
		DOCA_LOG_WARN("Error destroying stream: %s", doca_error_get_name(err));
	return ret;
}

bool destroy_stream(struct doca_dev *dev, struct globals *globals, struct stream_data *data)
{
	doca_error_t ret;
	bool is_ok = true;

	/* detach flow */
	ret = doca_rmax_flow_detach(data->flow, data->stream);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error detaching flow: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	ret = doca_rmax_flow_destroy(data->flow);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error destroying flow: %s", doca_error_get_name(ret));
		is_ok = false;
	}

	/* stop stream */
	ret = doca_ctx_stop(doca_rmax_in_stream_as_ctx(data->stream));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error stopping context: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	/* will destroy all the buffers in the chain */
	ret = doca_buf_dec_refcount(data->buffer, NULL);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error removing buffers: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	/* destroy stream */
	ret = doca_rmax_in_stream_destroy(data->stream);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Error destroying stream: %s", doca_error_get_name(ret));
		is_ok = false;
	}
	return is_ok;
}

static void handle_completion(struct doca_rmax_in_stream_event_rx_data *event_rx_data, union doca_data event_user_data)
{
	struct stream_data *data = event_user_data.ptr;
	const struct doca_rmax_in_stream_result *comp = doca_rmax_in_stream_event_rx_data_get_result(event_rx_data);

	if (!comp)
		return;
	if (comp->elements_count <= 0)
		return;

	data->recv_pkts += comp->elements_count;
	for (size_t i = 0; i < data->num_buffers; ++i)
		data->recv_bytes += comp->elements_count * data->pkt_size[i];

	if (!data->dump)
		return;
	for (size_t i = 0; i < comp->elements_count; ++i)
		for (size_t chunk = 0; chunk < data->num_buffers; ++chunk) {
			const uint8_t *ptr = comp->memblk_ptr_arr[chunk] + data->stride_size[chunk] * i;
			char *dump_str = hex_dump(ptr, data->pkt_size[chunk]);

			DOCA_LOG_INFO("pkt %zu chunk %zu\n%s", i, chunk, dump_str);
			free(dump_str);
		}
}

/*
 * Prints the number of received packets and rx bitrate then reset the statistics.
 * Prints information with at least 1 second interval
 *
 * @data [in]: Pointer to the stream data
 * @return: true if statistics are printed successfully; false otherwise
 */
static bool print_statistics(struct stream_data *data)
{
	static const uint64_t us_in_s = 1000000L;
	struct timespec now;
	int ret;
	uint64_t dt;
	double mbits_received;

	ret = clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	if (ret != 0) {
		DOCA_LOG_ERR("error getting time: %s", strerror(errno));
		return false;
	}

	dt = (now.tv_sec - data->start.tv_sec) * us_in_s;
	dt += now.tv_nsec / 1000 - data->start.tv_nsec / 1000;
	/* ignore intervals shorter than 1 second */
	if (dt < us_in_s)
		return true;

	mbits_received = (double)(data->recv_bytes * 8) / dt;
	const char *unit = mbits_received > 1e3 ? "Gbps" : "Mbps";
	double rate = mbits_received > 1e3 ? mbits_received * 1e-3 : mbits_received;

	DOCA_LOG_INFO("Got %7zu packets | %7.2lf %s during %7.2lf sec\n", data->recv_pkts, rate, unit, dt * 1e-6);

	/* clear stats */
	data->start.tv_sec = now.tv_sec;
	data->start.tv_nsec = now.tv_nsec;
	data->recv_pkts = 0;
	data->recv_bytes = 0;

	return true;
}

static void handle_error(struct doca_rmax_in_stream_event_rx_data *event_rx_data, union doca_data event_user_data)
{
	struct stream_data *data = event_user_data.ptr;
	const struct doca_rmax_stream_error *err = doca_rmax_in_stream_event_rx_data_get_error(event_rx_data);

	if (err)
		DOCA_LOG_ERR("Error: code=%d message=%s", err->code, err->message);
	else
		DOCA_LOG_ERR("Unknown error");

	data->run_recv_loop = false;
}

bool run_recv_loop(const struct app_config *config, struct globals *globals, struct stream_data *data)
{
	int ret;

	ret = clock_gettime(CLOCK_MONOTONIC_RAW, &data->start);
	if (ret != 0) {
		DOCA_LOG_ERR("error getting time: %s", strerror(errno));
		return false;
	}

	data->run_recv_loop = true;

	while (data->run_recv_loop) {
		(void)doca_pe_progress(globals->pe);

		if (!print_statistics(data))
			return false;
		if (config->sleep_us > 0) {
			if (usleep(config->sleep_us) != 0) {
				if (errno != EINTR)
					DOCA_LOG_ERR("usleep error: %s", strerror(errno));
				return false;
			}
		}
	}

	return true;
}
