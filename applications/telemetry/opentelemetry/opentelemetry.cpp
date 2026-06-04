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

#include "opentelemetry_core.hpp"

#include <ctype.h>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(OPENTELEMETRY);

/* Supported Diag Data IDs */
#define DATA_ID_RX_BYTES 0x1020000100000000
#define DATA_ID_PRIOR_RX_BYTES 0x1020000200000000
#define DATA_ID_RX_PKTS 0x1020000300000000
#define DATA_ID_PRIOR_RX_PKTS 0x1020000400000000
#define DATA_ID_RX_DISCARD_BUF_PKTS 0x1020000500000000
#define DATA_ID_PRIOR_RX_PAUSE_PKTS 0x1020000600000000
#define DATA_ID_RX_TRANSPORT_ECN_PKTS 0x1080000400000000
#define DATA_ID_RX_TRANSPORT_CNP_PKTS 0x1080000500000000
#define DATA_ID_TX_TRANSPORT_CNP_PKTS 0x1100000100000000
#define DATA_ID_TX_TRANSPORT_DONE_DUE_TO_CC_DESCHED 0x1100000200000000
#define DATA_ID_TX_BYTES 0x1140000100000000
#define DATA_ID_PRIOR_TX_BYTES 0x1140000200000000
#define DATA_ID_TX_PKTS 0x1140000300000000
#define DATA_ID_PRIOR_TX_PKTS 0x1140000400000000
#define DATA_ID_PRIOR_TX_PAUSE_PKTS 0x1140000500000000

#define DATA_ID_PORT_MASK 0xFF
#define DATA_ID_PRIORITY_MASK 0xF00
#define DATA_ID_PRIORITY_SHIFT 8
#define DATA_ID_TYPE_MASK 0xFFFFFFFFFFFFF000

#define DATA_ID_HEX_LEN 18 /* Length of data id input - 0x followed by 16 character hex value */
#define DATA_ID_FILE_LINE_MAX 256
#define DEFAULT_EXPORT_IP "localhost"
#define DEFAULT_EXPORT_TIME_MSEC 2000

/*
 * ARGP Callback - Handle DOCA device PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dev_pci_addr_callback(void *param, void *config) noexcept
{
	struct opentelemetry_cfg *cfg = (struct opentelemetry_cfg *)config;
	const char *dev_pci_addr = (char *)param;
	size_t len;

	len = strnlen(dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	memcpy(cfg->dev_pci_addr, dev_pci_addr, len);
	cfg->dev_pci_addr[len] = '\0';

	return DOCA_SUCCESS;
}

/*
 * Helper to create and push a diag_data_id to the config vector
 *
 * @app_cfg [in]: App configuration file
 * @data_id [in]: Diag Data ID to use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static inline doca_error_t push_diag_data_entry(struct opentelemetry_cfg *app_cfg, uint64_t data_id) noexcept
{
	struct diag_data_id entry;
	uint8_t port_id = data_id & DATA_ID_PORT_MASK;
	uint8_t priority = (data_id & DATA_ID_PRIORITY_MASK) >> DATA_ID_PRIORITY_SHIFT;

	switch (data_id & DATA_ID_TYPE_MASK) {
	case DATA_ID_RX_BYTES:
		entry.name = "port_" + std::to_string(port_id) + "_rx_bytes";
		entry.desc = "The number of bytes received on the physical port";
		entry.dir = "receive";
		entry.unit = "By";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_PRIOR_RX_BYTES:
		entry.name = "port_" + std::to_string(port_id) + "_priority_rx_bytes";
		entry.desc = "The number of bytes received on the physical port and priority";
		entry.dir = "receive";
		entry.unit = "By";
		entry.port = port_id;
		entry.priority = priority;
		break;
	case DATA_ID_RX_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_rx_packets";
		entry.desc = "The number of received packets on the physical port";
		entry.dir = "receive";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_PRIOR_RX_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_priority_rx_packets";
		entry.desc = "The number of received packets on the physical port and priority";
		entry.dir = "receive";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = priority;
		break;
	case DATA_ID_RX_DISCARD_BUF_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_rx_discard_buf_packets";
		entry.desc = "The number of received packets dropped due to lack of buffers on physical port";
		entry.dir = "receive";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_PRIOR_RX_PAUSE_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_priority_rx_pauses_packets";
		entry.desc = "The number of link-layer pause packets received on a physical port and priority";
		entry.dir = "receive";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = priority;
		break;
	case DATA_ID_RX_TRANSPORT_ECN_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_rx_transport_ecn_packets";
		entry.desc =
			"The number of RoCEv2 packets received by the notification point which were marked for experiencing the congestion";
		entry.dir = "receive";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_RX_TRANSPORT_CNP_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_rx_transport_cnp_handled_packets";
		entry.desc = "The number of CNP received packets handled by the Reaction Point, per port";
		entry.dir = "receive";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_TX_TRANSPORT_CNP_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_tx_transport_cnp_sent_packets";
		entry.desc = "The number of CNP packets sent by the Notification Point, per port";
		entry.dir = "transmit";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_TX_TRANSPORT_DONE_DUE_TO_CC_DESCHED:
		entry.name = "tx_transport_done_due_to_cc_deschedule_events";
		entry.desc = "The number of QP descheduled due to congestion control rate limitation";
		entry.dir = "transmit";
		entry.unit = "1";
		entry.port = DATA_ID_NO_PORT;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_TX_BYTES:
		entry.name = "port_" + std::to_string(port_id) + "_tx_bytes";
		entry.desc = "The number of transmitted bytes on the physical port (excluding loopback traffic)";
		entry.dir = "transmit";
		entry.unit = "By";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_PRIOR_TX_BYTES:
		entry.name = "port_" + std::to_string(port_id) + "_priority_tx_bytes";
		entry.desc =
			"The number of transmitted bytes on the physical port and priority (excluding loopback traffic)";
		entry.dir = "transmit";
		entry.unit = "By";
		entry.port = port_id;
		entry.priority = priority;
		break;
	case DATA_ID_TX_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_tx_packets";
		entry.desc = "The number of transmitted packets on the physical port (excluding loopback traffic)";
		entry.dir = "transmit";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = DATA_ID_NO_PRIORITY;
		break;
	case DATA_ID_PRIOR_TX_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_priority_tx_packets";
		entry.desc =
			"The number of transmitted packets on the physical port and priority (excluding loopback traffic)";
		entry.dir = "transmit";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = priority;
		break;
	case DATA_ID_PRIOR_TX_PAUSE_PKTS:
		entry.name = "port_" + std::to_string(port_id) + "_priority_tx_pauses_packets";
		entry.desc = "The number of link-layer pause packets transmitted on a physical port and priority";
		entry.dir = "transmit";
		entry.unit = "{packet}";
		entry.port = port_id;
		entry.priority = priority;
		break;
	default:
		DOCA_LOG_ERR("Diag data ID %16lx is not supported by app", data_id);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	entry.data_id = data_id;
	try {
		app_cfg->diag_data.push_back(entry);
	} catch (const std::bad_alloc &) {
		DOCA_LOG_ERR("Failed to store diag entry: memory failure");
		return DOCA_ERROR_NO_MEMORY;
	}

	DOCA_LOG_INFO("Added Diag data_id: %16lx - %s", data_id, entry.name.c_str());

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DOCA diag data id parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t data_id_callback(void *param, void *config) noexcept
{
	struct opentelemetry_cfg *cfg = (struct opentelemetry_cfg *)config;
	const char *data_id_str = (const char *)param;
	uint64_t data_id;
	char *endptr;

	/* Enforce input string begins with a variation of '0x' */
	if (data_id_str == NULL || data_id_str[0] != '0' || (data_id_str[1] != 'x' && data_id_str[1] != 'X')) {
		DOCA_LOG_ERR("Data_id must start with 0x followed by 16 hex digits: failed on: %s", data_id_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Verify there are no trailing characters */
	if (strlen(data_id_str) > DATA_ID_HEX_LEN) {
		DOCA_LOG_ERR("Data_id must start with 0x followed by 16 hex digits: trailing characters detected: %s",
			     data_id_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Convert hex input to uint64_t and verify all 16 characters have been used */
	data_id = (uint64_t)strtoull(data_id_str, &endptr, 16);
	if (data_id == 0 || endptr != data_id_str + DATA_ID_HEX_LEN) {
		DOCA_LOG_ERR("Data_id must start with 0x followed by 16 hex digits: invalid value: %s", data_id_str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return push_diag_data_entry(cfg, data_id);
}

/*
 * ARGP Callback - Handle DOCA diag data id file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t data_id_file_callback(void *param, void *config) noexcept
{
	const char *file_name = (const char *)param;
	char line_buf[DATA_ID_FILE_LINE_MAX];
	FILE *fp;
	doca_error_t result;

	if (file_name == NULL || file_name[0] == '\0') {
		DOCA_LOG_ERR("Data ID file name is empty");
		return DOCA_ERROR_INVALID_VALUE;
	}

	errno = 0;
	fp = fopen(file_name, "r");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to open data ID file '%s': %s", file_name, strerror(errno));
		return DOCA_ERROR_IO_FAILED;
	}

	while (fgets(line_buf, (int)sizeof(line_buf), fp) != NULL) {
		char *line = line_buf;
		char *end;

		/* Trim leading whitespace */
		while (*line != '\0' && isspace((unsigned char)*line))
			line++;

		if (*line == '\0' || *line == '#')
			continue;

		/* Trim trailing whitespace and newline */
		end = line + strlen(line);
		while (end > line && (end[-1] == '\n' || isspace((unsigned char)end[-1])))
			end--;
		*end = '\0';
		if (end == line)
			continue;

		/* Add data id as if it was a command line input */
		result = data_id_callback(line, config);
		if (result != DOCA_SUCCESS) {
			fclose(fp);
			return result;
		}
	}

	if (ferror(fp)) {
		DOCA_LOG_ERR("Error reading data ID file '%s': %s", file_name, strerror(errno));
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	fclose(fp);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle export IP parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t export_ip_param_callback(void *param, void *config) noexcept
{
	struct opentelemetry_cfg *cfg = (struct opentelemetry_cfg *)config;
	const char *export_ip_str = (const char *)param;
	const char *p;
	char *endptr;
	int i;
	unsigned long octet;

	if (export_ip_str == NULL || export_ip_str[0] == '\0') {
		DOCA_LOG_ERR("Export IP parameter is empty");
		return DOCA_ERROR_INVALID_VALUE;
	}

	p = export_ip_str;
	for (i = 0; i < 4; i++) {
		if (*p == '\0' || *p == '.') {
			DOCA_LOG_ERR("Export IP is not a valid IPv4 address: expected 4 dot-separated numbers");
			return DOCA_ERROR_INVALID_VALUE;
		}

		errno = 0;
		octet = strtoul(p, &endptr, 10);
		if (endptr == p || *endptr != (i < 3 ? '.' : '\0')) {
			DOCA_LOG_ERR("Export IP is not a valid IPv4 address: '%s'", export_ip_str);
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (errno == ERANGE || octet > 255) {
			DOCA_LOG_ERR("Export IP octet out of range 0-255: '%s'", export_ip_str);
			return DOCA_ERROR_INVALID_VALUE;
		}

		p = endptr + (i < 3 ? 1 : 0);
	}

	cfg->export_ip = export_ip_str;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle export time parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t export_time_callback(void *param, void *config) noexcept
{
	struct opentelemetry_cfg *cfg = (struct opentelemetry_cfg *)config;
	int *time = (int *)param;

	if (*time <= 0) {
		DOCA_LOG_ERR("Time for OpenTelemetry exports must be a positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}

	cfg->export_time = *time;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle verbose parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t verbose_callback(void *param, void *config) noexcept
{
	struct opentelemetry_cfg *cfg = (struct opentelemetry_cfg *)config;

	cfg->verbose = *(bool *)param ? 1 : 0;

	return DOCA_SUCCESS;
}

/*
 * Register CLI parameters within the argp infrastructure
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_opentelemetry_params(void) noexcept
{
	doca_error_t result;
	struct doca_argp_param *dev_pci_addr_param, *data_id_param, *data_id_file_param, *export_ip_param,
		*export_time_param, *verbose_param;

	/* Create and register parameter to read a PCI address to be used in diag */
	result = doca_argp_param_create(&dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dev_pci_addr_param, "p");
	doca_argp_param_set_long_name(dev_pci_addr_param, "pci-addr");
	doca_argp_param_set_description(dev_pci_addr_param, "DOCA Diag device PCI address");
	doca_argp_param_set_callback(dev_pci_addr_param, dev_pci_addr_callback);
	doca_argp_param_set_type(dev_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(dev_pci_addr_param);
	result = doca_argp_register_param(dev_pci_addr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register parameter to a diag data_id and tag */
	result = doca_argp_param_create(&data_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(data_id_param, "d");
	doca_argp_param_set_long_name(data_id_param, "data-id");
	doca_argp_param_set_description(data_id_param,
					"Diag data_id to use (multiple accepted) e.g. 0x1020000100000000");
	doca_argp_param_set_callback(data_id_param, data_id_callback);
	doca_argp_param_set_type(data_id_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_multiplicity(data_id_param);
	result = doca_argp_register_param(data_id_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register parameter read a data_id file */
	result = doca_argp_param_create(&data_id_file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(data_id_file_param, "f");
	doca_argp_param_set_long_name(data_id_file_param, "data-id-file");
	doca_argp_param_set_description(data_id_file_param, "Name of file containing a list of data ids");
	doca_argp_param_set_callback(data_id_file_param, data_id_file_callback);
	doca_argp_param_set_type(data_id_file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(data_id_file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register parameter to accept the IP address to export telemetry to */
	result = doca_argp_param_create(&export_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(export_ip_param, "i");
	doca_argp_param_set_long_name(export_ip_param, "ip");
	doca_argp_param_set_description(export_ip_param,
					"IPv4 address to send OpenTelemetry payloads to (via HTTP POST)");
	doca_argp_param_set_callback(export_ip_param, export_ip_param_callback);
	doca_argp_param_set_type(export_ip_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(export_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register parameter to accept a time in milliseconds use to trigger exports */
	result = doca_argp_param_create(&export_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(export_time_param, "t");
	doca_argp_param_set_long_name(export_time_param, "time");
	doca_argp_param_set_description(export_time_param, "Time (in milliseconds) between OpenTelemetry exports");
	doca_argp_param_set_callback(export_time_param, export_time_callback);
	doca_argp_param_set_type(export_time_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(export_time_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register parameter to accept verbose mode */
	result = doca_argp_param_create(&verbose_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(verbose_param, "vb");
	doca_argp_param_set_long_name(verbose_param, "verbose");
	doca_argp_param_set_description(verbose_param,
					"Run in verbose mode (return counters as DOCA LOG as well as export)");
	doca_argp_param_set_callback(verbose_param, verbose_callback);
	doca_argp_param_set_type(verbose_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(verbose_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t configure_default_diag_data(struct opentelemetry_cfg *app_cfg)
{
	DOCA_LOG_INFO("Setting default Diag data_ids....");

	auto result = push_diag_data_entry(app_cfg, DATA_ID_RX_BYTES);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_PRIOR_RX_BYTES);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_RX_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_PRIOR_RX_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_RX_DISCARD_BUF_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_PRIOR_RX_PAUSE_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_RX_TRANSPORT_ECN_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_RX_TRANSPORT_CNP_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_TX_TRANSPORT_CNP_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_TX_TRANSPORT_DONE_DUE_TO_CC_DESCHED);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_TX_BYTES);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_PRIOR_TX_BYTES);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_TX_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_PRIOR_TX_PKTS);
	if (result != DOCA_SUCCESS)
		return result;
	result = push_diag_data_entry(app_cfg, DATA_ID_PRIOR_TX_PAUSE_PKTS);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

/*
 * Opentelemetry application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct opentelemetry_cfg app_cfg = {};
	struct opentelemetry_ctx opentel_ctx = {};
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Register a logger backend */
	auto result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_argp_init(NULL, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = register_opentelemetry_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register the program parameters: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	/* If the user has not added custom data_ids, use default values */
	if (app_cfg.diag_data.size() == 0) {
		result = configure_default_diag_data(&app_cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure default diag data ids: %s", doca_error_get_descr(result));
			goto destroy_argp;
		}
	}

	if (app_cfg.export_ip.empty())
		app_cfg.export_ip = DEFAULT_EXPORT_IP;

	if (app_cfg.export_time == 0)
		app_cfg.export_time = DEFAULT_EXPORT_TIME_MSEC;

	errno = 0;
	if (gethostname(app_cfg.host_name, sizeof(app_cfg.host_name)) != 0) {
		DOCA_LOG_ERR("Failed extract device host name: %s", strerror(errno));
		goto destroy_argp;
	}

	result = opentelemetry_init(&app_cfg, &opentel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize OpenTelemetry context: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	DOCA_LOG_INFO("Starting Diag data export....");
	result = opentelemetry_run(&app_cfg, &opentel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Main loop failed with error: %s", doca_error_get_descr(result));
		goto destroy_opentelemetry;
	}

	exit_status = EXIT_SUCCESS;

destroy_opentelemetry:
	result = opentelemetry_destroy(&opentel_ctx);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("Failed to destroy OpenTelemetry context: %s", doca_error_get_descr(result));

destroy_argp:
	/* ARGP destroy_resources */
	doca_argp_destroy();

	return exit_status;
}
