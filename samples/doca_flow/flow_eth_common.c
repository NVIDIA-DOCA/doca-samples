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

#include <unistd.h>

#include <doca_error.h>
#include <doca_eth_rxq.h>
#include <doca_ctx.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_log.h>

#include <common.h>
#include "flow_eth_common.h"

DOCA_LOG_REGISTER(FLOW_ETH_COMMON);

#define ETH_MAX_PKT_SIZE 1600
#define ETH_MAX_BURST_SIZE 128
#define ETH_PKT_RATE_MBPS 12500
#define ETH_MAX_PKT_PROC_TIME_US 1
#define ETH_LOG_MAX_LRO 1

struct flow_eth_common_rxq {
	struct doca_eth_rxq *rxq;
	struct doca_mmap *mmap;
	void *mmap_buf;
};

struct flow_eth_common_dev_context {
	struct flow_eth_common_rx_cfg cfg;
	struct flow_eth_common_rxq *rxqs;
	uint16_t nb_rxqs_created;
};

static doca_error_t create_eth_mmap_per_queue(struct doca_dev *dev,
					      uint32_t mmap_size,
					      struct doca_mmap **out_mmap,
					      void **out_buf)
{
	struct doca_mmap *mmap = NULL;
	void *buf = NULL;
	doca_error_t result;

	buf = malloc(mmap_size);
	if (buf == NULL)
		return DOCA_ERROR_NO_MEMORY;

	result = doca_mmap_create(&mmap);
	if (result != DOCA_SUCCESS)
		goto free_buf;
	result = doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	result = doca_mmap_set_memrange(mmap, buf, mmap_size);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	result = doca_mmap_set_max_num_devices(mmap, 1);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	result = doca_mmap_add_dev(mmap, dev);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	result = doca_mmap_start(mmap);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;

	/* success: set outputs and return */
	*out_mmap = mmap;
	*out_buf = buf;
	return DOCA_SUCCESS;

destroy_mmap:
	doca_mmap_destroy(mmap);
free_buf:
	free(buf);
	return result;
}

static void destroy_single_rxq(struct flow_eth_common_rxq *rxq_arr)
{
	struct doca_ctx *ctx = NULL;
	if (rxq_arr->rxq) {
		ctx = doca_eth_rxq_as_doca_ctx(rxq_arr->rxq);
		if (ctx)
			doca_ctx_stop(ctx);
		doca_eth_rxq_destroy(rxq_arr->rxq);
	}
	if (rxq_arr->mmap)
		doca_mmap_destroy(rxq_arr->mmap);
	if (rxq_arr->mmap_buf)
		free(rxq_arr->mmap_buf);
}

static void destroy_eth_rxqs(struct flow_eth_common_dev_context *dev_ctx)
{
	int i;

	if (dev_ctx->rxqs == NULL)
		return;

	for (i = 0; i < dev_ctx->nb_rxqs_created; i++)
		destroy_single_rxq(&dev_ctx->rxqs[i]);

	free(dev_ctx->rxqs);
	dev_ctx->rxqs = NULL;
	dev_ctx->nb_rxqs_created = 0;
}

static doca_error_t create_eth_rxqs(struct doca_dev *dev,
				    struct doca_pe *pe,
				    struct flow_eth_common_dev_context *dev_ctx)
{
	struct flow_eth_common_rx_cfg *cfg = &dev_ctx->cfg;
	uint32_t per_queue_buf_size = 0;
	doca_error_t result;

	result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
						       ETH_PKT_RATE_MBPS,
						       ETH_MAX_PKT_PROC_TIME_US,
						       ETH_MAX_PKT_SIZE,
						       ETH_MAX_BURST_SIZE,
						       ETH_LOG_MAX_LRO,
						       0 /* headroom */,
						       0 /* tailroom */,
						       &per_queue_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to estimate packet buffer size: %s", doca_error_get_descr(result));
		return result;
	}

	dev_ctx->rxqs = (struct flow_eth_common_rxq *)calloc(cfg->nb_rxqs, sizeof(*dev_ctx->rxqs));
	if (dev_ctx->rxqs == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for RXQs");
		return DOCA_ERROR_NO_MEMORY;
	}

	for (uint16_t i = 0; i < cfg->nb_rxqs; i++) {
		struct doca_eth_rxq *rxq = NULL;
		struct doca_ctx *ctx = NULL;
		struct doca_mmap *mmap = NULL;
		void *buf = NULL;
		union doca_data user_data;

		result = doca_eth_rxq_create(dev, ETH_MAX_BURST_SIZE, ETH_MAX_PKT_SIZE, &rxq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create RXQ %d: %s", i, doca_error_get_descr(result));
			goto error_cleanup;
		}

		result = doca_eth_rxq_set_type(rxq, DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set RXQ type: %s", doca_error_get_descr(result));
			goto error_cleanup_rxq;
		}

		/* Create a dedicated mmap per RXQ */
		result = create_eth_mmap_per_queue(dev, per_queue_buf_size, &mmap, &buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create mmap for RXQ %d: %s", i, doca_error_get_descr(result));
			goto error_cleanup_rxq;
		}

		result = doca_eth_rxq_set_pkt_buf(rxq, mmap, 0 /* offset */, per_queue_buf_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set packet buffer: %s", doca_error_get_descr(result));
			goto error_cleanup_mmap;
		}

		result = doca_eth_rxq_set_flow_tag(rxq, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set flow tag: %s", doca_error_get_descr(result));
			goto error_cleanup_mmap;
		}

		if (cfg->enable_metadata) {
			result = doca_eth_rxq_set_metadata_num(rxq, 1);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set metadata: %s", doca_error_get_descr(result));
				goto error_cleanup_mmap;
			}
		}

		/* Use provided user_data or default to queue index */
		if (cfg->rx_user_data.ptr == NULL)
			user_data.u64 = i;
		else
			user_data = cfg->rx_user_data;

		result = doca_eth_rxq_event_batch_managed_recv_register(rxq,
									DOCA_EVENT_BATCH_EVENTS_NUMBER_16,
									DOCA_EVENT_BATCH_EVENTS_NUMBER_1,
									user_data,
									cfg->rx_success_cb,
									cfg->rx_error_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to register event: %s", doca_error_get_descr(result));
			goto error_cleanup_mmap;
		}

		ctx = doca_eth_rxq_as_doca_ctx(rxq);
		if (ctx == NULL) {
			DOCA_LOG_ERR("Failed to get context from RXQ");
			result = DOCA_ERROR_BAD_STATE;
			goto error_cleanup_mmap;
		}

		result = doca_pe_connect_ctx(pe, ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to connect context to PE: %s", doca_error_get_descr(result));
			goto error_cleanup_mmap;
		}

		result = doca_ctx_start(ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
			goto error_cleanup_mmap;
		}

		/* Bind logical queue id */
		result = doca_eth_rxq_apply_queue_id(rxq, i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to apply queue ID: %s", doca_error_get_descr(result));
			/* Context is started, need to stop it */
			(void)doca_ctx_stop(ctx);
			goto error_cleanup_mmap;
		}

		/* Success for this queue - save resources */
		dev_ctx->rxqs[i].rxq = rxq;
		dev_ctx->rxqs[i].mmap = mmap;
		dev_ctx->rxqs[i].mmap_buf = buf;
		dev_ctx->nb_rxqs_created++;
		continue;

error_cleanup_mmap:
		if (mmap)
			doca_mmap_destroy(mmap);
		if (buf)
			free(buf);
error_cleanup_rxq:
		if (rxq)
			doca_eth_rxq_destroy(rxq);
error_cleanup:
		/* Clean up all previously created RXQs */
		destroy_eth_rxqs(dev_ctx);
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t flow_eth_common_create_dev_resources(struct doca_dev *dev,
						  struct doca_pe *pe,
						  struct flow_eth_common_rx_cfg *cfg,
						  struct flow_eth_common_dev_context **dev_ctx)
{
	struct flow_eth_common_dev_context *ctx = NULL;
	doca_error_t result;

	if (dev == NULL || pe == NULL || cfg == NULL || dev_ctx == NULL) {
		DOCA_LOG_ERR("Invalid parameters");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (cfg->nb_rxqs == 0) {
		DOCA_LOG_ERR("Number of RXQs must be > 0");
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx = (struct flow_eth_common_dev_context *)calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to allocate device context");
		return DOCA_ERROR_NO_MEMORY;
	}

	ctx->cfg = *cfg;
	result = create_eth_rxqs(dev, pe, ctx);
	if (result != DOCA_SUCCESS) {
		free(ctx);
		return result;
	}

	*dev_ctx = ctx;
	return DOCA_SUCCESS;
}

doca_error_t flow_eth_common_destroy_dev_resources(struct flow_eth_common_dev_context *dev_ctx)
{
	if (dev_ctx == NULL)
		return DOCA_SUCCESS;

	destroy_eth_rxqs(dev_ctx);
	free(dev_ctx);

	return DOCA_SUCCESS;
}

void flow_eth_common_set_dev_cfg(uint16_t nb_queues,
				 bool enable_metadata,
				 union doca_data rx_user_data,
				 doca_eth_rxq_event_batch_managed_recv_handler_cb_t rx_success_cb,
				 doca_eth_rxq_event_batch_managed_recv_handler_cb_t rx_error_cb,
				 struct flow_eth_common_rx_cfg *cfg)
{
	cfg->nb_rxqs = nb_queues;
	cfg->enable_metadata = enable_metadata;
	cfg->rx_success_cb = rx_success_cb;
	cfg->rx_error_cb = rx_error_cb;
	cfg->rx_user_data = rx_user_data;
}

void flow_eth_common_handle_pkts(struct doca_pe *pe, uint32_t wait_secs)
{
	uint32_t secs = wait_secs;

	while (secs--) {
		doca_pe_progress(pe);
		sleep(1);
	}
}
