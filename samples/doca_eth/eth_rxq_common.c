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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <doca_bitfield.h>
#include <doca_log.h>
#include <doca_flow.h>

#include "eth_rxq_common.h"

DOCA_LOG_REGISTER(ETH::RXQ::COMMON);

#define COUNTERS_NUM (1 << 19)
#define DEFAULT_METADATA 1234

static bool is_flow_initialized = false;

/*
 * Initialize DOCA Flow with the flags: VNF/Hardware Steering/Isolated
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_doca_flow(void)
{
	doca_error_t result, tmp_result;
	struct doca_flow_cfg *rxq_flow_cfg;

	result = doca_flow_cfg_create(&rxq_flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg, err: %s", doca_error_get_name(result));
		return result;
	}
	result = doca_flow_cfg_set_pipe_queues(rxq_flow_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe_queues, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}
	result = doca_flow_cfg_set_mode_args(rxq_flow_cfg, "vnf,isolated");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mode_args, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}
	result = doca_flow_cfg_set_nr_counters(rxq_flow_cfg, COUNTERS_NUM);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr_counters, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}

	result = doca_flow_init(rxq_flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca flow, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}

destroy_cfg:
	tmp_result = doca_flow_cfg_destroy(rxq_flow_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow_cfg, err: %s", doca_error_get_name(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Start DOCA Flow with desired port ID
 *
 * @dev [in]: The doca device
 * @df_port [out]: DOCA Flow port to start
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t start_doca_flow_port(struct doca_dev *dev, uint16_t port_id, struct doca_flow_port **df_port)
{
	doca_error_t status, tmp_result;
	struct doca_flow_port_cfg *port_cfg;

	status = doca_flow_port_cfg_create(&port_cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port ID, err: %s", doca_error_get_name(status));
		goto destroy_port_cfg;
	}

	status = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg dev, err: %s", doca_error_get_name(status));
		goto destroy_port_cfg;
	}

	status = doca_flow_port_start(port_cfg, df_port);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca flow, err: %s", doca_error_get_name(status));
		goto destroy_port_cfg;
	}

destroy_port_cfg:
	tmp_result = doca_flow_port_cfg_destroy(port_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set start doca_flow port: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(status, tmp_result);
	}
	return status;
}

/*
 * Create root pipe and add an entry into desired RXQ queue
 *
 * @df_port [in]: DOCA Flow port to create root pipe in
 * @rxq_queue_ids [in]: Pointer to RXQ queue IDs array
 * @nb_queues [in]: Number of queues IDs in the array
 * @root_pipe [out]: DOCA Flow pipe to create
 * @root_entry [out]: DOCA Flow port entry to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_root_pipe(struct doca_flow_port *df_port,
				     uint16_t *rxq_queue_ids,
				     uint16_t nb_queues,
				     struct doca_flow_pipe **root_pipe,
				     struct doca_flow_pipe_entry **root_entry)
{
	doca_error_t status;
	struct doca_flow_actions actions, *actions_arr[1];
	struct doca_flow_match all_match;
	struct doca_flow_pipe_cfg *pipe_cfg;
	const char *pipe_name = "ROOT_PIPE";
	struct doca_flow_fwd all_fwd = {
		.type = DOCA_FLOW_FWD_RSS,
		.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
		.rss =
			{
				.queues_array = rxq_queue_ids,
				.nr_queues = nb_queues,
				.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP,
			},
	};
	struct doca_flow_fwd fwd_miss = {
		.type = DOCA_FLOW_FWD_DROP,
	};

	memset(&all_match, 0, sizeof(all_match));
	memset(&actions, 0, sizeof(actions));
	actions.meta.pkt_meta = DOCA_HTOBE32(DEFAULT_METADATA);
	actions_arr[0] = &actions;

	status = doca_flow_pipe_cfg_create(&pipe_cfg, df_port);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name, err: %s", doca_error_get_name(status));
		goto destroy_pipe_cfg;
	}
	status = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type, err: %s", doca_error_get_name(status));
		goto destroy_pipe_cfg;
	}
	status = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root, err: %s", doca_error_get_name(status));
		goto destroy_pipe_cfg;
	}
	status = doca_flow_pipe_cfg_set_match(pipe_cfg, &all_match, NULL);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match, err: %s", doca_error_get_name(status));
		goto destroy_pipe_cfg;
	}
	status = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(status));
		goto destroy_pipe_cfg;
	}

	status = doca_flow_pipe_create(pipe_cfg, &all_fwd, &fwd_miss, root_pipe);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca flow pipe, err: %s", doca_error_get_name(status));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	status = doca_flow_pipe_add_entry(0, *root_pipe, &all_match, &actions, NULL, NULL, 0, NULL, root_entry);
	if (status != DOCA_SUCCESS) {
		doca_flow_pipe_destroy(*root_pipe);
		DOCA_LOG_ERR("Failed to add doca flow entry, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_flow_entries_process(df_port, 0, 10000, 4);
	if (status != DOCA_SUCCESS) {
		doca_flow_pipe_destroy(*root_pipe);
		DOCA_LOG_ERR("Failed to process doca flow entry, err: %s", doca_error_get_name(status));
		return status;
	}

	DOCA_LOG_INFO("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return status;
}

doca_error_t rxq_common_init_doca_flow(struct doca_dev *dev, uint16_t port_id, struct eth_rxq_flow_resources *resources)
{
	doca_error_t status;

	if (dev == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	/* Check if init_doca_flow() was already called once since calling it more than once is illegal */
	if (is_flow_initialized == false) {
		status = init_doca_flow();
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate eth_rxq_flow_resources: failed to init DOCA flow, err: %s",
				     doca_error_get_name(status));
			return status;
		}

		is_flow_initialized = true;
	}

	status = start_doca_flow_port(dev, port_id, &(resources->df_port));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate eth_rxq_flow_resources: failed to init DOCA flow port, err: %s",
			     doca_error_get_name(status));
		goto destroy_doca_flow;
	}

	return DOCA_SUCCESS;

destroy_doca_flow:
	doca_flow_destroy();

	return status;
}

doca_error_t allocate_eth_rxq_flow_resources(struct eth_rxq_flow_config *cfg, struct eth_rxq_flow_resources *resources)
{
	doca_error_t status, clean_status;

	status = create_root_pipe(resources->df_port,
				  cfg->rxq_queue_ids,
				  cfg->nb_queues,
				  &(resources->root_pipe),
				  &(resources->root_entry));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate eth_rxq_flow_resources: failed to create root pipe, err: %s",
			     doca_error_get_name(status));
		goto destroy_doca_flow_port;
	}

	return DOCA_SUCCESS;

destroy_doca_flow_port:
	clean_status = doca_flow_port_stop(resources->df_port);
	if (clean_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(clean_status));
	doca_flow_destroy();

	return status;
}

doca_error_t destroy_eth_rxq_flow_resources(struct eth_rxq_flow_resources *resources)
{
	doca_error_t status;

	if (resources->root_pipe != NULL)
		doca_flow_pipe_destroy(resources->root_pipe);

	if (resources->df_port != NULL) {
		status = doca_flow_port_stop(resources->df_port);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(status));
			return status;
		}
	}

	if (is_flow_initialized == true) {
		doca_flow_destroy();
		is_flow_initialized = false;
	}

	return DOCA_SUCCESS;
}

doca_error_t get_pkt_headroom(struct doca_buf *pkt, uint16_t *headroom_size)
{
	void *pkt_head, *pkt_data;
	doca_error_t status;

	status = doca_buf_get_head(pkt, &pkt_head);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca_buf head, err: %s", doca_error_get_name(status));
		return status;
	}
	status = doca_buf_get_data(pkt, &pkt_data);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca_buf data, err: %s", doca_error_get_name(status));
		return status;
	}
	*headroom_size = (uint8_t *)(pkt_data) - (uint8_t *)(pkt_head);

	return DOCA_SUCCESS;
}

doca_error_t get_pkt_tailroom(struct doca_buf *pkt, uint16_t *tailroom_size)
{
	size_t pkt_len, pkt_data_len;
	doca_error_t status;
	uint16_t headroom_size;

	status = get_pkt_headroom(pkt, &headroom_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get packet headroom size, err: %s", doca_error_get_name(status));
		return status;
	}
	status = doca_buf_get_len(pkt, &pkt_len);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca_buf length, err: %s", doca_error_get_name(status));
		return status;
	}
	status = doca_buf_get_data_len(pkt, &pkt_data_len);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get doca_buf data length, err: %s", doca_error_get_name(status));
		return status;
	}
	*tailroom_size = pkt_len - pkt_data_len - headroom_size;

	return DOCA_SUCCESS;
}
