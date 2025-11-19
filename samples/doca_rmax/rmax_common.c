/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rmax_common.h"

DOCA_LOG_REGISTER(rmax_common);

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct rmax_stream_config *cfg = (struct rmax_stream_config *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(cfg->pci_address, pci_address, len + 1);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle source IP address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t src_ip_callback(void *param, void *config)
{
	struct rmax_stream_config *cfg = (struct rmax_stream_config *)config;
	char *ip_addr_str = (char *)param;

	if (inet_pton(AF_INET, ip_addr_str, &cfg->src_ip.s_addr) != 1) {
		DOCA_LOG_ERR("Invalid source IP address: %s", ip_addr_str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle destination port parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dst_port_callback(void *param, void *config)
{
	struct rmax_stream_config *cfg = (struct rmax_stream_config *)config;
	int port = *(int *)param;

	if (port <= 0 || port > 65535) {
		DOCA_LOG_ERR("Invalid destination port: %d", port);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->dst_port = port;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle destination IP address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dst_ip_callback(void *param, void *config)
{
	struct rmax_stream_config *cfg = (struct rmax_stream_config *)config;
	char *ip_addr_str = (char *)param;

	if (inet_pton(AF_INET, ip_addr_str, &cfg->dst_ip.s_addr) != 1) {
		DOCA_LOG_ERR("Invalid destination IP address: %s", ip_addr_str);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

doca_error_t register_create_stream_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param;
	struct doca_argp_param *src_ip_param;
	struct doca_argp_param *dst_ip_param;
	struct doca_argp_param *dst_port_param;

	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI address");
	doca_argp_param_set_callback(pci_param, pci_address_callback);
	doca_argp_param_set_type(pci_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&src_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(src_ip_param, "s");
	doca_argp_param_set_long_name(src_ip_param, "src-ip");
	doca_argp_param_set_description(src_ip_param, "source IP address");
	doca_argp_param_set_callback(src_ip_param, src_ip_callback);
	doca_argp_param_set_type(src_ip_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(src_ip_param);
	result = doca_argp_register_param(src_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&dst_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dst_ip_param, "d");
	doca_argp_param_set_long_name(dst_ip_param, "dst-ip");
	doca_argp_param_set_description(dst_ip_param, "destination IP address");
	doca_argp_param_set_callback(dst_ip_param, dst_ip_callback);
	doca_argp_param_set_type(dst_ip_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(dst_ip_param);
	result = doca_argp_register_param(dst_ip_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&dst_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dst_port_param, "P");
	doca_argp_param_set_long_name(dst_port_param, "dst-port");
	doca_argp_param_set_description(dst_port_param, "destination port");
	doca_argp_param_set_callback(dst_port_param, dst_port_callback);
	doca_argp_param_set_type(dst_port_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(dst_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Free callback - free doca_buf allocated pointer
 *
 * @addr [in]: Memory range pointer
 * @len [in]: Memory range length
 * @opaque [in]: An opaque pointer passed to iterator
 */
static void free_callback(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;
	free(addr);
}

doca_error_t rmax_flow_set_attributes(struct rmax_stream_config *config, struct doca_rmax_flow *flow)
{
	doca_error_t result;

	result = doca_rmax_flow_set_src_ip(flow, &config->src_ip);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_rmax_flow_set_dst_ip(flow, &config->dst_ip);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_rmax_flow_set_dst_port(flow, config->dst_port);
	if (result != DOCA_SUCCESS)
		return result;

	return result;
}

doca_error_t rmax_stream_set_attributes(struct doca_rmax_in_stream *stream, struct rmax_stream_config *config)
{
	size_t num_buffers = (config->hdr_size > 0) ? 2 : 1;
	uint16_t pkt_size[MAX_BUFFERS];
	doca_error_t result;

	/* fill stream parameters */
	if (config->scatter_all)
		result = doca_rmax_in_stream_set_scatter_type_raw(stream);
	else
		result = doca_rmax_in_stream_set_scatter_type_ulp(stream);
	if (result != DOCA_SUCCESS)
		return result;

	if (config->order_by_rtp_seqn) {
		result = doca_rmax_in_stream_set_packet_placement_order_rtp_seqn(stream);
		if (result != DOCA_SUCCESS)
			return result;
	}

	result = doca_rmax_in_stream_set_elements_count(stream, config->num_elements);
	if (result != DOCA_SUCCESS)
		return result;

	if (num_buffers == 1)
		pkt_size[0] = config->data_size;
	else {
		/* Header-Data Split mode */
		pkt_size[0] = config->hdr_size;
		pkt_size[1] = config->data_size;
	}

	result = doca_rmax_in_stream_set_memblks_count(stream, num_buffers);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_rmax_in_stream_memblk_desc_set_min_size(stream, pkt_size);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_rmax_in_stream_memblk_desc_set_max_size(stream, pkt_size);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

doca_error_t rmax_stream_start(struct rmax_program_state *state)
{
	doca_error_t result;

	/* allow receiving rmax events using progress engine */
	result = doca_pe_connect_ctx(state->core_objects.pe, state->core_objects.ctx);
	if (result != DOCA_SUCCESS)
		return result;

	/* start the rmax context */
	result = doca_ctx_start(state->core_objects.ctx);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

doca_error_t rmax_stream_allocate_buf(struct rmax_program_state *state,
				      struct doca_rmax_in_stream *stream,
				      struct rmax_stream_config *config,
				      struct doca_buf **buffer,
				      uint16_t *stride_size)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	size_t num_buffers = (config->hdr_size > 0) ? 2 : 1;
	size_t size[MAX_BUFFERS] = {0, 0};
	char *ptr_memory = NULL;
	void *ptr[MAX_BUFFERS];
	doca_error_t result;

	/* query buffer size */
	result = doca_rmax_in_stream_get_memblk_size(stream, size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get memory block size: %s", doca_error_get_descr(result));
		return result;
	}

	/* query stride size */
	result = doca_rmax_in_stream_get_memblk_stride_size(stream, stride_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get memory block stride size: %s", doca_error_get_descr(result));
		return result;
	}

	/* allocate memory */
	ptr_memory = aligned_alloc(page_size, size[0] + size[1]);
	if (ptr_memory == NULL)
		return DOCA_ERROR_NO_MEMORY;

	result = doca_mmap_set_memrange(state->core_objects.src_mmap, ptr_memory, size[0] + size[1]);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range, ptr %p, size %zu: %s",
			     ptr_memory,
			     size[0] + size[1],
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_free_cb(state->core_objects.src_mmap, free_callback, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap free callback: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(state->core_objects.src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		return result;
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

		result = doca_buf_inventory_buf_get_by_addr(state->core_objects.buf_inv,
							    state->core_objects.src_mmap,
							    ptr[i],
							    size[i],
							    &buf);
		if (result != DOCA_SUCCESS)
			return result;
		if (i == 0)
			*buffer = buf;
		else {
			/* chain buffers */
			result = doca_buf_chain_list(*buffer, buf);
			if (result != DOCA_SUCCESS)
				return result;
		}
	}

	/* set memory buffer(s) */
	result = doca_rmax_in_stream_set_memblk(stream, *buffer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set stream memory block(s): %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

void rmax_create_stream_cleanup(struct rmax_program_state *state,
				struct doca_rmax_in_stream *stream,
				struct doca_rmax_flow *flow,
				struct doca_buf *buf)
{
	doca_error_t result;

	if (buf != NULL) {
		result = doca_buf_dec_refcount(buf, NULL);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to remove buffers: %s", doca_error_get_descr(result));
	}

	result = doca_rmax_flow_destroy(flow);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA Rmax flow: %s", doca_error_get_descr(result));

	if (state->core_objects.ctx != NULL) {
		result = doca_ctx_stop(state->core_objects.ctx);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Rmax in stream context: %s", doca_error_get_descr(result));
	}

	/* destroy stream */
	result = doca_rmax_in_stream_destroy(stream);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy the stream: %s", doca_error_get_descr(result));

	result = destroy_core_objects(&state->core_objects);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA core related objects: %s", doca_error_get_descr(result));

	result = doca_rmax_release();
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy the DOCA Rivermax: %s", doca_error_get_descr(result));
}
