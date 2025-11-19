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
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <netinet/in.h>

#include "rmax_common.h"

DOCA_LOG_REGISTER(RMAX_CREATE_STREAM_HW_ORDER_HDS);

/*
 * Handle a successful Rx data event
 *
 * @event_rx_data [in]: the event that occurred. Holds result data.
 * @event_user_data [in]: user defined data that was previously provided on registration. Holds rmax_program_state.
 */
void rx_success_cb(struct doca_rmax_in_stream_event_rx_data *event_rx_data, union doca_data event_user_data)
{
	struct doca_rmax_in_stream_result *comp = doca_rmax_in_stream_event_rx_data_get_result(event_rx_data);

	if (comp->elements_count == 0)
		return;

	DOCA_LOG_INFO("Received %4d packet(s), first %lu last %lu", comp->elements_count, comp->ts_first, comp->ts_last);

	struct rmax_program_state *state = (struct rmax_program_state *)event_user_data.ptr;

	/* dump packets */
	uint8_t *header = comp->memblk_ptr_arr[0];
	uint8_t *payload = comp->memblk_ptr_arr[1];

	for (size_t i = 0; i < comp->elements_count;
	     ++i, header += state->stride_size[0], payload += state->stride_size[1]) {
		char *hdr_dump = hex_dump(header, state->config->hdr_size);
		char *pld_dump = hex_dump(payload, state->config->data_size);

		DOCA_LOG_DBG("Header:\n%s", hdr_dump);
		DOCA_LOG_DBG("Payload:\n%s", pld_dump);
		free(hdr_dump);
		free(pld_dump);
	}
}

/*
 * Handle a failed Rx data event
 *
 * @event_rx_data [in]: the event that occurred. Holds error data.
 * @event_user_data [in]: user defined data that was previously provided on registration. Holds rmax_program_state.
 */
void rx_error_cb(struct doca_rmax_in_stream_event_rx_data *event_rx_data, union doca_data event_user_data)
{
	struct doca_rmax_stream_error *err = doca_rmax_in_stream_event_rx_data_get_error(event_rx_data);
	struct rmax_program_state *state = (struct rmax_program_state *)event_user_data.ptr;

	DOCA_LOG_ERR("Error in Rx event: code=%d message=%s", err->code, err->message);

	state->run_pe_progress = false;
	state->exit_status = DOCA_ERROR_IO_FAILED;
}

/*
 * Run rmax_create_stream_hw_order_hds sample, which creates stream
 *
 * @state [in]: DOCA core related objects
 * @stream_config [in]: stream configurations
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t rmax_create_stream_hw_order_hds(struct rmax_program_state *state, struct rmax_stream_config *stream_config)
{
	struct doca_rmax_in_stream *stream = NULL;
	struct doca_buf *buf = NULL;
	struct doca_rmax_flow *flow = NULL;
	doca_error_t result;

	memset(state, 0, sizeof(*state));
	state->config = stream_config;

	/* open DOCA device with the given PCI address */
	result = open_doca_device_with_pci(stream_config->pci_address, NULL, &state->core_objects.dev);
	if (result != DOCA_SUCCESS)
		return result;

	/* DOCA RMAX library Initialization */
	result = doca_rmax_init();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize DOCA RMAX library: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* create core DOCA objects */
	result = create_core_objects(&state->core_objects, MAX_BUFFERS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA core related objects: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* create stream object */
	result = doca_rmax_in_stream_create(state->core_objects.dev, &stream);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA Rivermax stream: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* set stream attributes, based on received or default configurations */
	result = rmax_stream_set_attributes(stream, stream_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set stream attributes: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* Register Rx data event handlers */
	union doca_data event_user_data;

	event_user_data.ptr = (void *)state;
	result = doca_rmax_in_stream_event_rx_data_register(stream, event_user_data, rx_success_cb, rx_error_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register to Rx data event: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* convert DOCA Rmax stream to DOCA context */
	state->core_objects.ctx = doca_rmax_in_stream_as_ctx(stream);
	if (state->core_objects.ctx == NULL) {
		DOCA_LOG_ERR("Failed to convert stream to a context");
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* allocate buffers for received data */
	result = rmax_stream_allocate_buf(state, stream, stream_config, &buf, state->stride_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate RX buffers: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* connect the rivermax context to a progress engine and start it */
	result = rmax_stream_start(state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start rmax context: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* create a flow using DOCA Rmax API */
	result = doca_rmax_flow_create(&flow);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create a flow: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* create a DOCA Rmax flow and attach it to the given stream  */
	result = rmax_flow_set_attributes(stream_config, flow);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set flow attributes: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	/* Attach flow to a stream using DOCA Rmax API */
	result = doca_rmax_flow_attach(flow, stream);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach a flow: %s", doca_error_get_descr(result));
		rmax_create_stream_cleanup(state, stream, flow, buf);
		return result;
	}

	state->run_pe_progress = true;
	state->exit_status = DOCA_SUCCESS;

	while (state->run_pe_progress)
		(void)doca_pe_progress(state->core_objects.pe);

	if (state->exit_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Program exiting with failure to receive data. err=%s",
			     doca_error_get_name(state->exit_status));

	/* detach flow */
	result = doca_rmax_flow_detach(flow, stream);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to detach flow from stream: %s", doca_error_get_descr(result));

	/* Clean and destroy all relevant objects */
	rmax_create_stream_cleanup(state, stream, flow, buf);

	return DOCA_SUCCESS;
}
