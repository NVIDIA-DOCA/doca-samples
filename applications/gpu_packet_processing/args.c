/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stddef.h>
#include <doca_argp.h>
#include <utils.h>

#include "common.h"
#include "packets.h"

DOCA_LOG_REGISTER(GPU_ARGS);

/*
 * Get GPU PCIe address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t gpu_pci_address_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("PCI file name too long. Max %d", DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(app_cfg->gpu_pcie_addr, pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);

	return DOCA_SUCCESS;
}

/*
 * Enable GPU HTTP server mode.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t http_server_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	bool http_server = *((bool *)param);

	app_cfg->http_server = http_server;

	return DOCA_SUCCESS;
}

/*
 * Get NIC PCIe address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t nic_pci_address_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("PCI file name too long. Max %d", DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(app_cfg->nic_pcie_addr, pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);

	return DOCA_SUCCESS;
}

/*
 * Get GPU receive queue number.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t queue_num_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	int queue_num = *((int *)param);

	if (queue_num <= 0 || queue_num > MAX_QUEUES) {
		DOCA_LOG_ERR("GPU receive queue number is wrong 0 < %d < %d", queue_num, MAX_QUEUES);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->queue_num = queue_num;

	return DOCA_SUCCESS;
}

/*
 * Get UDP receive function execution scope.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t udp_rx_scope_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	char *scope = (char *)param;

	if (strcmp(scope, "thread") == 0)
		app_cfg->udp_rx.scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD;
	else if (strcmp(scope, "warp") == 0)
		app_cfg->udp_rx.scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP;
	else if (strcmp(scope, "block") == 0)
		app_cfg->udp_rx.scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK;
	else {
		DOCA_LOG_ERR("UDP receive scope %s is wrong, valid values are thread, warp or block", scope);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Get UDP receive max number of packets per receive call.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t udp_rx_max_pkts_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	int max_rx_pkts = *((int *)param);

	if (max_rx_pkts < 0 || max_rx_pkts > MAX_PKT_NUM) {
		DOCA_LOG_ERR("UDP receive max packets is wrong 0 <= %d <= %d", max_rx_pkts, MAX_PKT_NUM);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->udp_rx.max_rx_pkts = max_rx_pkts;

	return DOCA_SUCCESS;
}

/*
 * Get UDP receive timeout.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t udp_rx_timeout_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	int timeout_ns = *((int *)param);

	/* A zero timeout may never return from the receive function, preventing the kernel exit */
	if (timeout_ns <= 0) {
		DOCA_LOG_ERR("UDP receive timeout must be > 0 ns");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->udp_rx.timeout_ns = timeout_ns;

	return DOCA_SUCCESS;
}

/*
 * Enable UDP kernel stage latency measurements.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS
 */
static doca_error_t udp_stage_latency_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;

	app_cfg->udp_rx.stage_latency = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * Enable UDP kernel per-packet filtering.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS
 */
static doca_error_t udp_packet_filtering_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;

	app_cfg->udp_rx.process = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * Enable UDP CQE attribute check.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS
 */
static doca_error_t udp_cqe_attr_check_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;

	app_cfg->udp_rx.cqe_attr_check = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * Get UDP receive queue number of packet buffers.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t udp_rx_pkt_num_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	int pkt_num = *((int *)param);

	/* The GPU receive queue is a striding RQ with 512 strides (packets) per WQE and a power of 2 WQEs */
	if (pkt_num < UDP_RX_MIN_PKT_NUM || pkt_num > MAX_PKT_NUM || (pkt_num & (pkt_num - 1)) != 0) {
		DOCA_LOG_ERR("UDP receive packet buffers %d must be a power of 2 in [%d, %d]",
			     pkt_num,
			     UDP_RX_MIN_PKT_NUM,
			     MAX_PKT_NUM);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->udp_rx.pkt_num = pkt_num;

	return DOCA_SUCCESS;
}

/*
 * Get UDP receive queue packet buffer size.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t udp_rx_pkt_size_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;
	int pkt_size = *((int *)param);

	/* Packet buffer is the RQ stride: power of 2, up to 8KB to keep the striding RQ */
	if (pkt_size < UDP_RX_MIN_PKT_SIZE || pkt_size > MAX_PKT_SIZE || (pkt_size & (pkt_size - 1)) != 0) {
		DOCA_LOG_ERR("UDP receive packet buffer size %d must be a power of 2 in [%d, %d]",
			     pkt_size,
			     UDP_RX_MIN_PKT_SIZE,
			     MAX_PKT_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_cfg->udp_rx.pkt_size = pkt_size;

	return DOCA_SUCCESS;
}

/*
 * Allocate UDP receive packet buffers in host memory.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS
 */
static doca_error_t udp_pkt_host_mem_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;

	app_cfg->udp_rx.pkt_host_mem = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * Allocate UDP receive queue rings in host memory.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS
 */
static doca_error_t udp_rq_host_mem_callback(void *param, void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;

	app_cfg->udp_rx.rq_host_mem = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * Validate the combination of application parameters.
 *
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t app_params_validation_callback(void *config)
{
	struct app_gpu_cfg *app_cfg = (struct app_gpu_cfg *)config;

	/* A receive call can't return more packets than the receive queue holds */
	if (app_cfg->udp_rx.max_rx_pkts > app_cfg->udp_rx.pkt_num) {
		DOCA_LOG_ERR("UDP receive max packets %u can't be greater than UDP receive packet buffers %u",
			     app_cfg->udp_rx.max_rx_pkts,
			     app_cfg->udp_rx.pkt_num);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * Packet filtering wipes PKT_WIPE_BYTES from the UDP header: the wipe must stay within the packet buffer,
	 * otherwise it overwrites the beginning of the next packet.
	 */
	if (app_cfg->udp_rx.process &&
	    app_cfg->udp_rx.pkt_size < offsetof(struct eth_ip_udp_hdr, l4_hdr) + PKT_WIPE_BYTES) {
		DOCA_LOG_ERR("UDP receive packet buffer size %u too small for --udp-packet-filtering, must be >= %zu",
			     app_cfg->udp_rx.pkt_size,
			     offsetof(struct eth_ip_udp_hdr, l4_hdr) + PKT_WIPE_BYTES);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * Warp and block receive functions split max packets in an equal per-thread quota (at least 1) and return as
	 * soon as one thread reaches it: max packets must be a multiple of the threads polling the queue.
	 */
	if (app_cfg->udp_rx.max_rx_pkts > 0) {
		uint32_t nthreads = 1;

		if (app_cfg->udp_rx.scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP)
			nthreads = WARP_SIZE;
		else if (app_cfg->udp_rx.scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK)
			nthreads = CUDA_THREADS;

		if (app_cfg->udp_rx.max_rx_pkts % nthreads != 0) {
			DOCA_LOG_ERR("UDP receive max packets %u must be 0 or a multiple of %u with %s receive scope",
				     app_cfg->udp_rx.max_rx_pkts,
				     nthreads,
				     app_cfg->udp_rx.scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP ? "warp" : "block");
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Create and register an application command line parameter.
 *
 * @short_name [in]: Parameter short name, NULL if none
 * @long_name [in]: Parameter long name
 * @arguments [in]: Parameter arguments description, NULL for boolean flags without value
 * @description [in]: Parameter description
 * @callback [in]: Parameter callback
 * @type [in]: Parameter type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_param(const char *short_name,
				   const char *long_name,
				   const char *arguments,
				   const char *description,
				   doca_argp_param_cb_t callback,
				   enum doca_argp_type type)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	if (short_name != NULL)
		doca_argp_param_set_short_name(param, short_name);
	doca_argp_param_set_long_name(param, long_name);
	if (arguments != NULL)
		doca_argp_param_set_arguments(param, arguments);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, callback);
	doca_argp_param_set_type(param, type);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t register_application_params(void)
{
	doca_error_t result;
	struct doca_argp_param *gpu_param, *nic_param, *queue_param, *server_param;

	result = doca_argp_param_create(&gpu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(gpu_param, "g");
	doca_argp_param_set_long_name(gpu_param, "gpu");
	doca_argp_param_set_arguments(gpu_param, "<GPU PCIe address>");
	doca_argp_param_set_description(gpu_param, "GPU PCIe address to be used by the app");
	doca_argp_param_set_callback(gpu_param, gpu_pci_address_callback);
	doca_argp_param_set_type(gpu_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(gpu_param);
	result = doca_argp_register_param(gpu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&nic_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(nic_param, "n");
	doca_argp_param_set_long_name(nic_param, "nic");
	doca_argp_param_set_arguments(nic_param, "<NIC PCIe address>");
	doca_argp_param_set_description(nic_param, "DOCA device PCIe address used by the app");
	doca_argp_param_set_callback(nic_param, nic_pci_address_callback);
	doca_argp_param_set_type(nic_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(nic_param);
	result = doca_argp_register_param(nic_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&queue_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(queue_param, "q");
	doca_argp_param_set_long_name(queue_param, "queue");
	doca_argp_param_set_arguments(queue_param, "<num>");
	doca_argp_param_set_description(queue_param, "GPU receive queues per traffic type (UDP, TCP), 1-4");
	doca_argp_param_set_callback(queue_param, queue_num_callback);
	doca_argp_param_set_type(queue_param, DOCA_ARGP_TYPE_INT);
	doca_argp_param_set_mandatory(queue_param);
	result = doca_argp_register_param(queue_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&server_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(server_param, "s");
	doca_argp_param_set_long_name(server_param, "httpserver");
	doca_argp_param_set_description(server_param, "Enable GPU HTTP server mode");
	doca_argp_param_set_callback(server_param, http_server_callback);
	doca_argp_param_set_type(server_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(server_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = register_param(NULL,
				"udp-rx-scope",
				"<thread|warp|block>",
				"UDP receive function execution scope (default block)",
				udp_rx_scope_callback,
				DOCA_ARGP_TYPE_STRING);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(
		NULL,
		"udp-rx-max-pkts",
		"<num>",
		"UDP max packets per receive call, 0 = no limit, return on timeout (default 512, max --udp-rx-pkt-num). Exact with thread scope; with warp/block scope it is split in a per-thread quota (must be a multiple of 32/512) and the receive returns when any thread reaches it",
		udp_rx_max_pkts_callback,
		DOCA_ARGP_TYPE_INT);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(NULL,
				"udp-rx-timeout",
				"<ns>",
				"UDP receive call timeout in nanoseconds, > 0 (default 500000)",
				udp_rx_timeout_callback,
				DOCA_ARGP_TYPE_INT);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(NULL,
				"udp-stage-latency",
				NULL,
				"Report UDP kernel receive, processing and report stage latency every 5 s",
				udp_stage_latency_callback,
				DOCA_ARGP_TYPE_BOOLEAN);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(
		NULL,
		"udp-packet-filtering",
		NULL,
		"Process every UDP packet (DNS filter, DNS/other stats). Default: read only the packet size, count packets and bytes",
		udp_packet_filtering_callback,
		DOCA_ARGP_TYPE_BOOLEAN);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(
		NULL,
		"udp-cqe-attr-check",
		NULL,
		"Receive with CQE attributes and validate them for every UDP packet (bytes vs packet length, timestamp non-zero and non-decreasing, no holes)",
		udp_cqe_attr_check_callback,
		DOCA_ARGP_TYPE_BOOLEAN);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(NULL,
				"udp-rx-pkt-num",
				"<num>",
				"UDP packet buffers per receive queue, power of 2, 512-65536 (default 65536)",
				udp_rx_pkt_num_callback,
				DOCA_ARGP_TYPE_INT);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(
		NULL,
		"udp-rx-pkt-size",
		"<bytes>",
		"UDP packet buffer size, power of 2, 64-8192, >= largest received frame, >= 128 with --udp-packet-filtering (default 8192)",
		udp_rx_pkt_size_callback,
		DOCA_ARGP_TYPE_INT);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(NULL,
				"udp-pkt-host-mem",
				NULL,
				"Receive UDP packets in host memory instead of GPU memory",
				udp_pkt_host_mem_callback,
				DOCA_ARGP_TYPE_BOOLEAN);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_param(NULL,
				"udp-rq-host-mem",
				NULL,
				"Place UDP receive and completion queue rings in host memory instead of GPU memory",
				udp_rq_host_mem_callback,
				DOCA_ARGP_TYPE_BOOLEAN);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_register_validation_callback(app_params_validation_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP validation callback: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register version callback for DOCA SDK & RUNTIME */
	result = doca_argp_register_version_callback(sdk_version_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}
