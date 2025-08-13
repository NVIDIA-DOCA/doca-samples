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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>
#include <flow_switch_common.h>

DOCA_LOG_REGISTER(FLOW_ENTROPY);

/*
 * Entropy sample does not create flows.
 * Allocate minimal resources.
 */
#define DOCA_SAMPLE_ENTROPY_QUEUES_NUM 1
#define DOCA_SAMPLE_ENTROPY_ENTRIES_NUM 1

/*
 * Run flow_entropy sample
 *
 * @ctx [in]: flow switch context the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_entropy(struct flow_switch_ctx *ctx)
{
	const int nb_ports = 1;
	uint32_t actions_mem_size[nb_ports];
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[nb_ports];
	struct doca_flow_entropy_format header;
	uint16_t entropy;
	doca_error_t result;

	result = init_doca_flow(1, "switch,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(DOCA_SAMPLE_ENTROPY_ENTRIES_NUM));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	header.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	header.ip4.src_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	header.ip4.dst_ip = BE_IPV4_ADDR(7, 7, 7, 7);
	header.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	header.transport.src_port = DOCA_HTOBE16(1234);
	header.transport.dst_port = DOCA_HTOBE16(5678);

	/* The entropy result should be equal to 0xdb9e */
	result = doca_flow_port_calc_entropy(ports[0], &header, &entropy);
	if (result == DOCA_SUCCESS)
		DOCA_LOG_INFO("The entropy for the given packet header is:0x%x", DOCA_BETOH16(entropy));
	else
		DOCA_LOG_ERR("Failed to retrieve entropy: %s", doca_error_get_descr(result));

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
