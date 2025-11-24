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

#include "eth_rxq_common.h"

DOCA_LOG_REGISTER(ETH::RXQ::COMMON);

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
