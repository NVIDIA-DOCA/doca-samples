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

#include <arpa/inet.h>

#include <doca_argp.h>
#include <doca_log.h>

#include "gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SIMPLE_SEND::MAIN);

/* Expand a macro value into a string literal */
#define STRINGIFY_VALUE(x) #x
#define STRINGIFY(x) STRINGIFY_VALUE(x)

/*
 * ARGP Callback - Set shared QP execution mode (THREAD, WARP or BLOCK)
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t exec_scope_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	const int exec_scope = *(int *)param;

	if (exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD && exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP &&
	    exec_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK) {
		DOCA_LOG_ERR("Invalid exec scope %d, must be 0 (thread), 1 (warp) or 2 (block) (default %d)",
			     exec_scope,
			     DEFAULT_EXEC_SCOPE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->exec_scope = (uint32_t)exec_scope;

	return DOCA_SUCCESS;
}

/*
 * Get GPU PCIe address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t gpu_pci_address_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	char *pci_address = (char *)param;
	size_t len;

	len = strnlen(pci_address, MAX_PCI_ADDRESS_LEN);
	if (len >= MAX_PCI_ADDRESS_LEN) {
		DOCA_LOG_ERR("PCI address too long. Max %d", MAX_PCI_ADDRESS_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(sample_cfg->gpu_pcie_addr, pci_address, len + 1);

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
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	char *pci_address = (char *)param;
	size_t len;

	len = strnlen(pci_address, MAX_PCI_ADDRESS_LEN);
	if (len >= MAX_PCI_ADDRESS_LEN) {
		DOCA_LOG_ERR("PCI address too long. Max %d", MAX_PCI_ADDRESS_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(sample_cfg->nic_pcie_addr, pci_address, len + 1);

	return DOCA_SUCCESS;
}

/*
 * Get packet size input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t packet_size_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	const int pkt_size = *(int *)param;

	/* Every packet starts with an Ethernet, IPv4 and UDP header */
	if (pkt_size < MIN_PKT_SIZE || pkt_size > MAX_PKT_SIZE) {
		DOCA_LOG_ERR("Invalid packet size %d, must be between %d and %d bytes",
			     pkt_size,
			     MIN_PKT_SIZE,
			     MAX_PKT_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->pkt_size = (size_t)pkt_size;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Enable/Disable CPU proxy mode
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t proxy_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	const int cpu_proxy = *(int *)param;

	if (cpu_proxy != 0 && cpu_proxy != 1) {
		DOCA_LOG_ERR("Invalid CPU proxy mode %d, must be 0 or 1 (default %d)", cpu_proxy, DEFAULT_CPU_PROXY);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cpu_proxy == 0)
		sample_cfg->nic_handler = DOCA_GPUNETIO_ETH_NIC_HANDLER_GPU_SM_DB;
	else
		sample_cfg->nic_handler = DOCA_GPUNETIO_ETH_NIC_HANDLER_CPU_PROXY;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Enable shared QP execution
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t shared_qp_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	const int shared_qp = *(int *)param;

	if (shared_qp != 0 && shared_qp != 1) {
		DOCA_LOG_ERR("Invalid shared QP mode %d, must be 0 or 1 (default %d)", shared_qp, DEFAULT_SHARED_QP);
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->shared_qp = (shared_qp == 1);

	return DOCA_SUCCESS;
}

/*
 * Parse an IPv4 address in dotted decimal notation.
 *
 * @str [in]: IPv4 address string
 * @addr [out]: IPv4 address, network byte order
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t parse_ipv4_addr(const char *str, uint32_t *addr)
{
	struct in_addr in;

	if (inet_pton(AF_INET, str, &in) != 1) {
		DOCA_LOG_ERR("Invalid IPv4 address %s", str);
		return DOCA_ERROR_INVALID_VALUE;
	}

	*addr = in.s_addr;

	return DOCA_SUCCESS;
}

/*
 * Get IPv4 source address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t src_ip_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;

	return parse_ipv4_addr((char *)param, &sample_cfg->src_ip);
}

/*
 * Get IPv4 destination address input.
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t dst_ip_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;

	return parse_ipv4_addr((char *)param, &sample_cfg->dst_ip);
}

/*
 * Number of CUDA threads in the CUDA kernel send
 *
 * @param [in]: Command line parameter
 * @config [in]: Application configuration structure
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t cuda_threads_callback(void *param, void *config)
{
	struct sample_simple_send_cfg *sample_cfg = (struct sample_simple_send_cfg *)config;
	const int cuda_threads = *(int *)param;

	if (cuda_threads <= 0) {
		DOCA_LOG_ERR("Invalid number of CUDA threads %d, must be a positive value (default %d)",
			     cuda_threads,
			     DEFAULT_CUDA_THREADS);
		return DOCA_ERROR_INVALID_VALUE;
	}

	sample_cfg->cuda_threads = cuda_threads;

	return DOCA_SUCCESS;
}

/*
 * Register sample command line parameters.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_sample_params(void)
{
	doca_error_t result;
	struct doca_argp_param *exec_param, *gpu_param, *nic_param, *proxy_param, *shared_qp_param, *size_param,
		*thread_param, *src_ip_param, *dst_ip_param;

	result = doca_argp_param_create(&exec_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(exec_param, "e");
	doca_argp_param_set_long_name(exec_param, "exec-scope");
	doca_argp_param_set_description(
		exec_param,
		"Shared QP mode to test: per-thread (0) per-warp (1) per-block (2), default " STRINGIFY(
			DEFAULT_EXEC_SCOPE) ". Used only if shared qp is enabled.");
	doca_argp_param_set_callback(exec_param, exec_scope_callback);
	doca_argp_param_set_type(exec_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(exec_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&gpu_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(gpu_param, "g");
	doca_argp_param_set_long_name(gpu_param, "gpu");
	doca_argp_param_set_arguments(gpu_param, "<GPU PCIe address>");
	doca_argp_param_set_description(gpu_param, "GPU PCIe address to be used by the sample");
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
	doca_argp_param_set_description(nic_param, "DOCA device PCIe address used by the sample");
	doca_argp_param_set_callback(nic_param, nic_pci_address_callback);
	doca_argp_param_set_type(nic_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(nic_param);
	result = doca_argp_register_param(nic_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&proxy_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(proxy_param, "p");
	doca_argp_param_set_long_name(proxy_param, "proxy");
	doca_argp_param_set_arguments(proxy_param, "<Enable/disable CPU proxy mode>");
	doca_argp_param_set_description(proxy_param,
					"CPU proxy mode (0: Off 1: On), default " STRINGIFY(DEFAULT_CPU_PROXY));
	doca_argp_param_set_callback(proxy_param, proxy_callback);
	doca_argp_param_set_type(proxy_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(proxy_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&shared_qp_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(shared_qp_param, "q");
	doca_argp_param_set_long_name(shared_qp_param, "shared-qp");
	doca_argp_param_set_arguments(shared_qp_param, "<shared qp mode, 0: disabled, 1:enabled>");
	doca_argp_param_set_description(
		shared_qp_param,
		"Shared QP mode (0: disabled 1: enabled), default " STRINGIFY(DEFAULT_SHARED_QP));
	doca_argp_param_set_callback(shared_qp_param, shared_qp_callback);
	doca_argp_param_set_type(shared_qp_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(shared_qp_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(size_param, "s");
	doca_argp_param_set_long_name(size_param, "size");
	doca_argp_param_set_arguments(size_param, "<packet size>");
	doca_argp_param_set_description(size_param,
					"Packet size to send in bytes, Ethernet header included, without FCS");
	doca_argp_param_set_callback(size_param, packet_size_callback);
	doca_argp_param_set_type(size_param, DOCA_ARGP_TYPE_INT);
	doca_argp_param_set_mandatory(size_param);
	result = doca_argp_register_param(size_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&thread_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(thread_param, "t");
	doca_argp_param_set_long_name(thread_param, "threads");
	doca_argp_param_set_arguments(thread_param, "<CUDA threads in send kernel>");
	doca_argp_param_set_description(
		thread_param,
		"Number of CUDA threads in send kernel (default " STRINGIFY(DEFAULT_CUDA_THREADS) ")");
	doca_argp_param_set_callback(thread_param, cuda_threads_callback);
	doca_argp_param_set_type(thread_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(thread_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&src_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(src_ip_param, "src-ip");
	doca_argp_param_set_arguments(src_ip_param, "<IPv4 address>");
	doca_argp_param_set_description(src_ip_param,
					"IPv4 source address of the packets (default " DEFAULT_SRC_IP ")");
	doca_argp_param_set_callback(src_ip_param, src_ip_callback);
	doca_argp_param_set_type(src_ip_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(src_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&dst_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_long_name(dst_ip_param, "dst-ip");
	doca_argp_param_set_arguments(dst_ip_param, "<IPv4 address>");
	doca_argp_param_set_description(dst_ip_param,
					"IPv4 destination address of the packets (default " DEFAULT_DST_IP ")");
	doca_argp_param_set_callback(dst_ip_param, dst_ip_callback);
	doca_argp_param_set_type(dst_ip_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dst_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	struct sample_simple_send_cfg sample_cfg;
	int exit_status = EXIT_FAILURE;
	int cuda_id;
	char src_ip_str[INET_ADDRSTRLEN], dst_ip_str[INET_ADDRSTRLEN];
	cudaError_t cuda_ret;

	memset(&sample_cfg, 0, sizeof(sample_cfg));

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	/* Default values if not set from command line */
	sample_cfg.exec_scope = DEFAULT_EXEC_SCOPE;
	sample_cfg.shared_qp = (DEFAULT_SHARED_QP == 1);
	sample_cfg.nic_handler = (DEFAULT_CPU_PROXY == 1) ? DOCA_GPUNETIO_ETH_NIC_HANDLER_CPU_PROXY :
							    DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO;
	sample_cfg.cuda_threads = DEFAULT_CUDA_THREADS;
	if (parse_ipv4_addr(DEFAULT_SRC_IP, &sample_cfg.src_ip) != DOCA_SUCCESS ||
	    parse_ipv4_addr(DEFAULT_DST_IP, &sample_cfg.dst_ip) != DOCA_SUCCESS)
		goto sample_exit;

	result = doca_argp_init(NULL, &sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_sample_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/*
	 * Non-shared QP: one CUDA block with all the threads.
	 * Shared QP: SHARED_QP_BLOCKS CUDA blocks, each with cuda_threads / SHARED_QP_BLOCKS threads. The warp
	 * scope send requires full warps, so each block must have a multiple of DOCA_GPUNETIO_ETH_WARP_SIZE threads.
	 */
	if (sample_cfg.shared_qp == true) {
		const uint32_t align = DOCA_GPUNETIO_ETH_WARP_SIZE * SHARED_QP_BLOCKS;

		if (sample_cfg.cuda_threads % align != 0 ||
		    sample_cfg.cuda_threads > MAX_CUDA_BLOCK_THREADS * SHARED_QP_BLOCKS) {
			DOCA_LOG_ERR("Shared QP requires a number of CUDA threads multiple of %u, up to %u",
				     align,
				     MAX_CUDA_BLOCK_THREADS * SHARED_QP_BLOCKS);
			goto argp_cleanup;
		}
	} else if (sample_cfg.cuda_threads % DOCA_GPUNETIO_ETH_WARP_SIZE != 0 ||
		   sample_cfg.cuda_threads > MAX_CUDA_BLOCK_THREADS) {
		DOCA_LOG_ERR("Non-shared QP requires a number of CUDA threads multiple of %u, up to %u",
			     DOCA_GPUNETIO_ETH_WARP_SIZE,
			     MAX_CUDA_BLOCK_THREADS);
		goto argp_cleanup;
	}

	/*
	 * A CUDA context must be initialized before calling GPUNetIO functions.
	 * cudaFree(0) triggers the CUDA runtime initialization and report any errors.
	 */
	cuda_ret = cudaFree(0);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("CUDA initialization failed: %s", cudaGetErrorString(cuda_ret));
		goto argp_cleanup;
	}

	/* In a multi-GPU system, ensure CUDA refers to the right GPU device */
	cuda_ret = cudaDeviceGetByPCIBusId(&cuda_id, sample_cfg.gpu_pcie_addr);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Invalid GPU bus id provided %s", sample_cfg.gpu_pcie_addr);
		goto argp_cleanup;
	}

	cuda_ret = cudaSetDevice(cuda_id);
	if (cuda_ret != cudaSuccess) {
		DOCA_LOG_ERR("Failed to set CUDA device %d: %s", cuda_id, cudaGetErrorString(cuda_ret));
		goto argp_cleanup;
	}

	inet_ntop(AF_INET, &sample_cfg.src_ip, src_ip_str, sizeof(src_ip_str));
	inet_ntop(AF_INET, &sample_cfg.dst_ip, dst_ip_str, sizeof(dst_ip_str));

	if (sample_cfg.shared_qp == 0) {
		DOCA_LOG_INFO(
			"Sample configuration:\n\tGPU %s\n\tNIC %s\n\tPacket size %zu\n\tIPv4 %s -> %s, UDP port %u -> %u\n\tCUDA threads %u\n\tCPU Proxy %s\n\tShared QP No",
			sample_cfg.gpu_pcie_addr,
			sample_cfg.nic_pcie_addr,
			sample_cfg.pkt_size,
			src_ip_str,
			dst_ip_str,
			UDP_SRC_PORT,
			UDP_DST_PORT,
			sample_cfg.cuda_threads,
			((sample_cfg.nic_handler == DOCA_GPUNETIO_ETH_NIC_HANDLER_CPU_PROXY) ? "Yes" : "No"));
	} else {
		DOCA_LOG_INFO(
			"Sample configuration:\n\tGPU %s\n\tNIC %s\n\tPacket size %zu\n\tIPv4 %s -> %s, UDP port %u -> %u\n\tCUDA threads %u\n\tCPU Proxy %s\n\tShared QP Yes\n\tShared QP exec scope %s",
			sample_cfg.gpu_pcie_addr,
			sample_cfg.nic_pcie_addr,
			sample_cfg.pkt_size,
			src_ip_str,
			dst_ip_str,
			UDP_SRC_PORT,
			UDP_DST_PORT,
			sample_cfg.cuda_threads,
			((sample_cfg.nic_handler == DOCA_GPUNETIO_ETH_NIC_HANDLER_CPU_PROXY) ? "Yes" : "No"),
			((sample_cfg.exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD) ?
				 "Thread" :
				 (sample_cfg.exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP ? "Warp" : "Block")));
	}

	result = gpunetio_simple_send(&sample_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("gpunetio_simple_send() encountered an error: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
